#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import os
import struct
import sys
import time
from pathlib import Path


def resolve_repo_root() -> Path:
    candidates = []
    env_root = os.environ.get("UGRIPPER_ROOT")
    if env_root:
        candidates.append(Path(env_root))
    candidates.extend(
        [
            Path.cwd(),
            Path("/opt/ugripper"),
            Path(__file__).resolve().parent.parent,
        ]
    )

    for candidate in candidates:
        toolkit_path = candidate / "scripts/lib/gripper_hmi_toolkit.py"
        if toolkit_path.is_file():
            return candidate

    raise RuntimeError("failed to resolve ugripper root; set UGRIPPER_ROOT or run from /opt/ugripper")


def load_toolkit(repo_root: Path):
    toolkit_path = repo_root / "scripts/lib/gripper_hmi_toolkit.py"
    spec = importlib.util.spec_from_file_location("gripper_hmi_toolkit", toolkit_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load toolkit: {toolkit_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def read_calibration_raw(client, tk) -> bytes:
    def collect_frames(timeout_s: float):
        raw = bytearray(tk.CALIBRATION_PAYLOAD_SIZE)
        received = [False] * tk.CALIBRATION_CHUNK_COUNT
        response_token = 0
        status_code = 0
        deadline = time.monotonic() + timeout_s
        buffer = bytearray()
        while time.monotonic() < deadline:
            chunk = client.read_available(max(0.01, deadline - time.monotonic()))
            if chunk:
                buffer.extend(chunk)
            while len(buffer) >= 2:
                pos = buffer.find(tk.RAW_DATA_FRAME_HEAD)
                if pos < 0:
                    if len(buffer) > 1:
                        del buffer[:-1]
                    break
                if pos > 0:
                    del buffer[:pos]
                if len(buffer) >= 20 and tk.validate_calibration_read_frame(bytes(buffer[:20])):
                    frame = bytes(buffer[:20])
                    token = frame[2]
                    if token < tk.CALIBRATION_CHUNK_COUNT:
                        raw[token * tk.CALIBRATION_CHUNK_SIZE:(token + 1) * tk.CALIBRATION_CHUNK_SIZE] = frame[3:19]
                        received[token] = True
                    del buffer[:20]
                    continue
                if len(buffer) >= tk.STATUS_FRAME_LENGTH:
                    frame = bytes(buffer[:tk.STATUS_FRAME_LENGTH])
                    if tk.is_exclusive_status_frame(frame):
                        response_token = frame[2]
                        status_code = frame[3]
                        del buffer[:tk.STATUS_FRAME_LENGTH]
                        continue
                    if tk.calculate_xor(frame[:-1]) == frame[-1]:
                        del buffer[:tk.STATUS_FRAME_LENGTH]
                        continue
                if len(buffer) < 20:
                    break
                del buffer[0]
        return raw, received, response_token, status_code

    def request_range_read():
        client.write_bytes(tk.build_read_calibration_chunk_command(0x3F), "read_calib_range")
        return collect_frames(tk.CALIBRATION_RANGE_READ_TIMEOUT_S)

    def request_single_chunk_read(packet_index: int):
        client.write_bytes(
            tk.build_read_calibration_chunk_command(packet_index),
            f"read_calib_chunk{packet_index}",
        )
        return client.read_data_or_status(
            packet_index,
            tk.CALIBRATION_CHUNK_SIZE,
            tk.EXCLUSIVE_COMMAND_TIMEOUT_S,
        )

    client.flush_input()
    range_read_collected = False
    raw = bytearray(tk.CALIBRATION_PAYLOAD_SIZE)
    received = [False] * tk.CALIBRATION_CHUNK_COUNT
    response_token = 0
    status_code = 0

    for attempt in range(tk.CALIBRATION_READ_RETRY_LIMIT):
        raw, received, response_token, status_code = request_range_read()
        if all(received):
            range_read_collected = True
            break
        if (
            not any(received)
            and attempt + 1 < tk.CALIBRATION_READ_RETRY_LIMIT
            and client.abort_calibration_write_state(
                "range read timeout recovery"
                if status_code == 0
                else f"range read recovery after {tk.describe_status_frame(response_token, status_code)}"
            )
        ):
            continue
        if (
            tk.is_calibration_abort_recovery_status(status_code)
            and attempt + 1 < tk.CALIBRATION_READ_RETRY_LIMIT
            and client.abort_calibration_write_state(
                f"range read recovery after {tk.describe_status_frame(response_token, status_code)}"
            )
        ):
            continue
        if not tk.is_retryable_calibration_status(status_code):
            break
        client.flush_input()
        time.sleep(tk.CALIBRATION_CHUNK_SEND_INTERVAL_S)

    need_sequential_fallback = (
        not range_read_collected and not all(received) and tk.is_retryable_calibration_status(status_code)
    )
    if need_sequential_fallback:
        client.flush_input()
        raw = bytearray(tk.CALIBRATION_PAYLOAD_SIZE)
        received = [False] * tk.CALIBRATION_CHUNK_COUNT
        response_token = 0
        status_code = 0
        for packet_index in range(tk.CALIBRATION_CHUNK_COUNT):
            chunk_received = False
            result_kind = "timeout"
            result_payload: bytes | tuple[int, int] = b""
            for attempt in range(tk.CALIBRATION_CHUNK_RETRY_LIMIT):
                result_kind, result_payload = request_single_chunk_read(packet_index)
                if result_kind == "data":
                    payload = result_payload  # type: ignore[assignment]
                    if len(payload) == tk.CALIBRATION_CHUNK_SIZE:
                        raw[
                            packet_index * tk.CALIBRATION_CHUNK_SIZE:(packet_index + 1) * tk.CALIBRATION_CHUNK_SIZE
                        ] = payload
                        received[packet_index] = True
                        chunk_received = True
                        break
                if result_kind == "status":
                    token, status_code = result_payload  # type: ignore[misc]
                    response_token = token
                    if (
                        tk.is_calibration_abort_recovery_status(status_code)
                        and attempt + 1 < tk.CALIBRATION_CHUNK_RETRY_LIMIT
                        and client.abort_calibration_write_state(
                            f"chunk read recovery packet={packet_index} after {tk.describe_status_frame(token, status_code)}"
                        )
                    ):
                        continue
                    if tk.is_retryable_calibration_status(status_code):
                        time.sleep(tk.CALIBRATION_CHUNK_SEND_INTERVAL_S)
                        continue
                break
            if not chunk_received:
                if result_kind == "status":
                    client.last_error = (
                        f"calibration read failed at chunk {packet_index}: "
                        f"{tk.describe_status_frame(response_token, status_code)}"
                    )
                else:
                    client.last_error = f"timed out waiting for calibration chunk {packet_index}"
                raise RuntimeError(client.last_error)

    for packet_index, ok in enumerate(received):
        if ok:
            continue
        if status_code != 0:
            client.last_error = (
                f"calibration read failed at chunk {packet_index}: "
                f"{tk.describe_status_frame(response_token, status_code)}"
            )
        else:
            client.last_error = f"timed out waiting for calibration chunk {packet_index}"
        raise RuntimeError(client.last_error)

    payload = bytes(raw)
    magic, version, payload_size, header_size, valid_fields = struct.unpack("<4sIHHI", payload[:16])
    client.log(
        f"raw_calibration_header magic={magic.decode(errors='ignore')} version=0x{version:08X} "
        f"payload_size={payload_size} header_size={header_size} valid_fields=0x{valid_fields:08X}"
    )
    client.last_error = ""
    return payload


def format_hex_lines(data: bytes, width: int = 16, limit: int = 128) -> str:
    lines = []
    for offset in range(0, min(len(data), limit), width):
        chunk = data[offset:offset + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
        lines.append(f"{offset:04x}: {hex_part:<{width * 3 - 1}}  {ascii_part}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Dump raw gripper calibration payload without UCAL validation.")
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/left_gripper")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--dump-bin", help="Write raw 1024-byte payload to file")
    parser.add_argument("--head-bytes", type=int, default=128, help="How many bytes to hex-print")
    parser.add_argument("--log-file", help="Optional protocol log file")
    args = parser.parse_args()

    repo_root = resolve_repo_root()
    tk = load_toolkit(repo_root)

    log_cb = None
    if args.log_file:
        log_path = Path(args.log_file)

        def _log(message: str) -> None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            with log_path.open("a", encoding="utf-8") as fh:
                fh.write(message + "\n")

        log_cb = _log

    client = tk.GripperHmiClient(tk.ClientOptions(port=args.port, baudrate=args.baud, log_callback=log_cb))
    try:
        with client:
            payload = read_calibration_raw(client, tk)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    if args.dump_bin:
        dump_path = Path(args.dump_bin)
        dump_path.parent.mkdir(parents=True, exist_ok=True)
        dump_path.write_bytes(payload)
        print(f"dump_bin={dump_path}")

    magic = payload[:4]
    version, payload_size, header_size, valid_fields = struct.unpack("<IHHI", payload[4:16])
    print(f"port={args.port}")
    print(f"magic_ascii={magic.decode('ascii', errors='replace')}")
    print(f"magic_hex={magic.hex()}")
    print(f"data_format_version=0x{version:08x}")
    print(f"payload_size={payload_size}")
    print(f"header_size={header_size}")
    print(f"valid_fields=0x{valid_fields:08x}")
    print(f"first_{min(args.head_bytes, len(payload))}_bytes:")
    print(format_hex_lines(payload, limit=args.head_bytes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
