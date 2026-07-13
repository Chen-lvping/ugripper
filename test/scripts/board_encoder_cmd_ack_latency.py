#!/usr/bin/env python3
import argparse
import json
import os
import select
import statistics
import struct
import termios
import time
from pathlib import Path


REQUEST = bytes.fromhex("01 03 00 41 00 01 D4 1E")
EXPECTED_RESPONSE_SIZE = 7


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def configure_serial(fd: int, baudrate: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    attrs[4] = termios.B1000000 if baudrate == 1_000_000 else termios.B115200
    attrs[5] = attrs[4]
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * fraction + 0.5)))
    return ordered[index]


def run_side(device: str, iterations: int, timeout_ms: float, interval_ms: float, output: Path) -> dict:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure_serial(fd, 1_000_000)
    rows = []
    latencies = []
    timeouts = 0
    crc_errors = 0
    invalid_frames = 0
    rx_buffer = bytearray()

    try:
        for sequence in range(1, iterations + 1):
            termios.tcflush(fd, termios.TCIFLUSH)
            rx_buffer.clear()
            start_ns = time.monotonic_ns()
            written = os.write(fd, REQUEST)
            deadline_ns = start_ns + int(timeout_ms * 1_000_000)
            frame = b""
            status = "timeout"

            while time.monotonic_ns() < deadline_ns:
                remaining = max(0.0, (deadline_ns - time.monotonic_ns()) / 1_000_000_000)
                readable, _, _ = select.select([fd], [], [], remaining)
                if not readable:
                    break
                chunk = os.read(fd, 256)
                if not chunk:
                    continue
                rx_buffer.extend(chunk)
                while len(rx_buffer) >= 3:
                    if rx_buffer[0] != 0x01 or rx_buffer[1] != 0x03 or rx_buffer[2] != 0x02:
                        del rx_buffer[0]
                        invalid_frames += 1
                        continue
                    if len(rx_buffer) < EXPECTED_RESPONSE_SIZE:
                        break
                    candidate = bytes(rx_buffer[:EXPECTED_RESPONSE_SIZE])
                    del rx_buffer[:EXPECTED_RESPONSE_SIZE]
                    expected_crc = crc16_modbus(candidate[:-2])
                    received_crc = candidate[-2] | (candidate[-1] << 8)
                    if expected_crc != received_crc:
                        crc_errors += 1
                        status = "crc_error"
                        continue
                    frame = candidate
                    status = "ok"
                    break
                if status == "ok":
                    break

            end_ns = time.monotonic_ns()
            latency_us = (end_ns - start_ns) / 1000.0
            if status == "ok":
                latencies.append(latency_us)
            else:
                timeouts += 1
            rows.append(
                {
                    "sequence": sequence,
                    "status": status,
                    "latency_us": round(latency_us, 3),
                    "written": written,
                    "response_hex": frame.hex(" "),
                }
            )
            if interval_ms > 0:
                time.sleep(interval_ms / 1000.0)
    finally:
        os.close(fd)

    summary = {
        "device": device,
        "iterations": iterations,
        "successes": len(latencies),
        "timeouts": timeouts,
        "crc_errors": crc_errors,
        "discarded_prefix_bytes": invalid_frames,
        "timeout_ms": timeout_ms,
        "interval_ms": interval_ms,
        "request_hex": REQUEST.hex(" "),
        "latency_us": {
            "min": round(min(latencies), 3) if latencies else 0.0,
            "mean": round(statistics.fmean(latencies), 3) if latencies else 0.0,
            "p50": round(percentile(latencies, 0.50), 3),
            "p95": round(percentile(latencies, 0.95), 3),
            "p99": round(percentile(latencies, 0.99), 3),
            "max": round(max(latencies), 3) if latencies else 0.0,
        },
    }
    output.write_text(json.dumps({"summary": summary, "samples": rows}, indent=2) + "\n")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Measure encoder Modbus command-to-ACK latency")
    parser.add_argument("--device", required=True)
    parser.add_argument("--iterations", type=int, default=5000)
    parser.add_argument("--timeout-ms", type=float, default=20.0)
    parser.add_argument("--interval-ms", type=float, default=0.0)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.iterations <= 0 or args.timeout_ms <= 0 or args.interval_ms < 0:
        parser.error("iterations and timeout must be positive; interval must be non-negative")
    summary = run_side(
        args.device,
        args.iterations,
        args.timeout_ms,
        args.interval_ms,
        Path(args.output),
    )
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["timeouts"] == 0 and summary["crc_errors"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
