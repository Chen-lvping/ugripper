#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


PACKAGE = "com.ssnwt.helloxr"
REMOTE_ROOT = "/sdcard/Android/data/com.ssnwt.helloxr/files/dataset"
FILES = (
    "rgb.mp4",
    "tracking.mp4",
    "ctrl.mp4",
    "audio.m4a",
    "sensor.mcap",
    "calibration.json",
    "metadata.json",
)
MP4_FILES = (
    "rgb.mp4",
    "tracking.mp4",
    "ctrl.mp4",
    "audio.m4a",
)
MP4_HEADER_SCAN_BYTES = int(os.environ.get("UGRIPPER_EGO_MP4_HEADER_SCAN_BYTES", str(1024 * 1024)))
MP4_HEADER_MERGE_GAP = int(os.environ.get("UGRIPPER_EGO_MP4_HEADER_MERGE_GAP", "4096"))
SXR_PROPS = {
    "ro.product.manufacturer": "SXR",
    "ro.product.model": "SXR_1",
    "ro.product.device": "SXR_1",
    "ro.product.name": "SXR_1",
    "ro.build.product": "SXR_1",
}


def now_ms() -> int:
    return int(time.time() * 1000)


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\\''") + "'"


class Adb:
    def __init__(self, serial: str = ""):
        self.serial = serial
        self.bin = resolve_adb_bin()

    def command(self, *args: str) -> list[str]:
        command = [self.bin]
        if self.serial:
            command.extend(["-s", self.serial])
        command.extend(args)
        return command

    def run(self, *args: str, timeout: int = 10, check: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run(
            self.command(*args),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=check,
        )

    def exec_out_bytes(self, remote_file: str, offset: int, count: int, timeout: int = 30) -> bytes:
        remote_q = shell_quote(remote_file)
        command = (
            f"dd if={remote_q} bs=4096 skip={offset} count={count} "
            "iflag=skip_bytes,count_bytes 2>/dev/null"
        )
        return subprocess.check_output(self.command("exec-out", "sh", "-c", command), timeout=timeout)


def resolve_adb_bin() -> str:
    configured = os.environ.get("UGRIPPER_EGO_ADB", "").strip()
    if configured:
        return configured

    bundled = Path(__file__).resolve().parent.parent / "adb" / "adb"
    if bundled.exists() and os.access(bundled, os.X_OK):
        try:
            subprocess.run(
                [str(bundled), "version"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=3,
                check=True,
            )
            return str(bundled)
        except Exception:
            pass

    return shutil.which("adb") or "adb"


def write_status(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(path)


def list_devices(adb: Adb) -> list[str]:
    result = adb.run("devices", "-l", timeout=5)
    if result.returncode != 0:
        raise RuntimeError((result.stderr or result.stdout).strip() or "adb devices failed")
    devices = []
    for line in result.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def get_props(adb: Adb) -> dict:
    props = {}
    for key in SXR_PROPS:
        result = adb.run("shell", "getprop", key, timeout=5)
        props[key] = result.stdout.strip().replace("\r", "") if result.returncode == 0 else ""
    return props


def is_ego_device(adb: Adb) -> tuple[bool, dict]:
    props = get_props(adb)
    props_match = all(props.get(key) == value for key, value in SXR_PROPS.items())
    package_result = adb.run("shell", "pm", "path", PACKAGE, timeout=8)
    dataset_result = adb.run("shell", "test", "-d", REMOTE_ROOT, timeout=8)
    detail = {
        "serial": adb.serial,
        "props": props,
        "props_match": props_match,
        "package_present": package_result.returncode == 0 and bool(package_result.stdout.strip()),
        "dataset_present": dataset_result.returncode == 0,
    }
    return props_match, detail


def detect_ego(preferred_serial: str = "") -> tuple[Adb | None, dict]:
    root_adb = Adb()
    candidates = [preferred_serial] if preferred_serial else list_devices(root_adb)
    details = []
    for serial in candidates:
        if not serial:
            continue
        adb = Adb(serial)
        ok, detail = is_ego_device(adb)
        details.append(detail)
        if ok:
            return adb, {"selected": detail, "candidates": details}
    return None, {"selected": None, "candidates": details}


def latest_temp_episode(adb: Adb) -> str:
    root_q = shell_quote(REMOTE_ROOT)
    command = f'for d in {root_q}/episode_*-temp; do [ -d "$d" ] && basename "$d"; done | sort | tail -n 1'
    result = adb.run("shell", command, timeout=8)
    if result.returncode != 0:
        return ""
    return result.stdout.strip().replace("\r", "")


def remote_path(episode_name: str, file_name: str = "") -> str:
    base = f"{REMOTE_ROOT.rstrip('/')}/{episode_name}"
    return f"{base}/{file_name}" if file_name else base


def remote_dir_exists(adb: Adb, episode_name: str) -> bool:
    result = adb.run("shell", "test", "-d", remote_path(episode_name), timeout=8)
    return result.returncode == 0


def remote_file_size(adb: Adb, remote_file: str) -> int | None:
    result = adb.run("shell", "stat", "-c", "%s", remote_file, timeout=8)
    if result.returncode != 0:
        return None
    text = result.stdout.strip().replace("\r", "")
    return int(text) if text.isdigit() else None


def sync_one_file(adb: Adb, remote_episode: str, local_episode_dir: Path, file_name: str) -> int:
    remote_file = remote_path(remote_episode, file_name)
    size = remote_file_size(adb, remote_file)
    if size is None:
        return 0
    local_episode_dir.mkdir(parents=True, exist_ok=True)
    local_file = local_episode_dir / file_name
    local_size = local_file.stat().st_size if local_file.exists() else 0
    if size <= local_size:
        return 0
    delta = size - local_size
    data = adb.exec_out_bytes(remote_file, local_size, delta)
    with local_file.open("ab") as output:
        output.write(data)
    return len(data)


def sync_episode_once(adb: Adb, remote_episode: str, local_episode_dir: Path) -> dict:
    copied = {}
    for file_name in FILES:
        try:
            delta = sync_one_file(adb, remote_episode, local_episode_dir, file_name)
        except Exception as exc:
            copied[file_name] = {"delta": 0, "error": str(exc)}
        else:
            copied[file_name] = {"delta": delta}
    return copied


def parse_mp4_boxes_from_prefix(data: bytes, total_size: int) -> list[dict]:
    boxes = []
    offset = 0
    available = len(data)
    while offset + 8 <= available and offset < total_size:
        header = data[offset : offset + 8]
        box_size, box_type = struct.unpack(">I4s", header)
        header_size = 8
        if box_size == 1:
            if offset + 16 > available:
                boxes.append({"offset": offset, "size": total_size - offset, "type": "unknown", "truncated": True})
                break
            box_size = struct.unpack(">Q", data[offset + 8 : offset + 16])[0]
            header_size = 16
        elif box_size == 0:
            box_size = total_size - offset

        box_name = box_type.decode("ascii", errors="replace")
        truncated = offset + box_size > available
        if box_size < header_size or offset + box_size > total_size:
            boxes.append({"offset": offset, "size": max(0, total_size - offset), "type": box_name, "truncated": True})
            break
        boxes.append(
            {
                "offset": offset,
                "size": box_size,
                "type": box_name,
                "header_size": header_size,
                "truncated": truncated,
            }
        )
        if truncated:
            break
        offset += box_size
    return boxes


def final_header_refresh_size(remote_prefix: bytes, remote_size: int) -> int:
    boxes = parse_mp4_boxes_from_prefix(remote_prefix, remote_size)
    for box in boxes:
        if box.get("type") == "mdat":
            return min(remote_size, int(box.get("offset") or 0) + max(16, int(box.get("header_size") or 8)))
    for box in boxes:
        if box.get("type") == "moov":
            return min(remote_size, int(box["offset"]) + int(box["size"]), len(remote_prefix))
    return min(remote_size, len(remote_prefix), 4096)


def diff_ranges_bytes(before: bytes, after: bytes) -> list[tuple[int, int]]:
    common_size = min(len(before), len(after))
    ranges = []
    current_start = None
    current_end = None
    offset = 0
    while offset < common_size:
        if before[offset] == after[offset]:
            offset += 1
            continue
        start = offset
        while offset < common_size and before[offset] != after[offset]:
            offset += 1
        end = offset
        if current_start is None:
            current_start = start
            current_end = end
        elif start <= current_end + 1:
            current_end = max(current_end, end)
        else:
            ranges.append((current_start, current_end))
            current_start = start
            current_end = end
    if current_start is not None:
        ranges.append((current_start, current_end))
    if len(after) > len(before):
        ranges.append((len(before), len(after)))
    return ranges


def merge_ranges(ranges: list[tuple[int, int]], gap: int) -> list[tuple[int, int]]:
    if not ranges:
        return []
    merged = [ranges[0]]
    for start, end in ranges[1:]:
        prev_start, prev_end = merged[-1]
        if start <= prev_end + gap:
            merged[-1] = (prev_start, max(prev_end, end))
        else:
            merged.append((start, end))
    return merged


def refresh_final_mp4_header(adb: Adb, remote_episode: str, local_episode_dir: Path, file_name: str) -> dict:
    detail = {"file": file_name, "status": "skipped", "reason": ""}
    remote_file = remote_path(remote_episode, file_name)
    remote_size = remote_file_size(adb, remote_file)
    if remote_size is None:
        detail["reason"] = "remote_missing"
        return detail
    local_file = local_episode_dir / file_name
    if not local_file.exists():
        detail["reason"] = "local_missing"
        return detail

    scan_size = min(remote_size, MP4_HEADER_SCAN_BYTES)
    remote_prefix = adb.exec_out_bytes(remote_file, 0, scan_size)
    if not remote_prefix:
        detail["reason"] = "remote_prefix_empty"
        return detail

    refresh_size = final_header_refresh_size(remote_prefix, remote_size)
    if refresh_size <= 0:
        detail["reason"] = "no_header_region"
        detail["remote_size"] = remote_size
        detail["boxes"] = parse_mp4_boxes_from_prefix(remote_prefix, remote_size)
        return detail
    if refresh_size > len(remote_prefix):
        remote_prefix = adb.exec_out_bytes(remote_file, 0, refresh_size)

    with local_file.open("rb") as input_file:
        local_prefix = input_file.read(refresh_size)
    ranges = merge_ranges(diff_ranges_bytes(local_prefix, remote_prefix[:refresh_size]), MP4_HEADER_MERGE_GAP)

    detail.update(
        {
            "status": "unchanged" if not ranges else "ok",
            "remote_size": remote_size,
            "local_size": local_file.stat().st_size,
            "scan_bytes": scan_size,
            "refresh_bytes": refresh_size,
            "ranges": [{"offset": start, "size": end - start, "end": end} for start, end in ranges],
            "boxes": parse_mp4_boxes_from_prefix(remote_prefix, remote_size),
        }
    )
    if not ranges:
        return detail

    with local_file.open("r+b") as output:
        for start, end in ranges:
            output.seek(start)
            output.write(remote_prefix[start:end])
        output.flush()
        os.fsync(output.fileno())
    return detail


def refresh_final_mp4_headers(adb: Adb, remote_episode: str, local_episode_dir: Path) -> dict:
    details = {}
    for file_name in MP4_FILES:
        try:
            details[file_name] = refresh_final_mp4_header(adb, remote_episode, local_episode_dir, file_name)
        except Exception as exc:
            details[file_name] = {"file": file_name, "status": "error", "error": str(exc)}
    return details


def find_valid_box_marker(data: bytes, box_name: bytes, start: int) -> int | None:
    marker = start
    while True:
        marker = data.find(box_name, marker)
        if marker < 4:
            return None
        box_start = marker - 4
        box_size = struct.unpack(">I", data[box_start:marker])[0]
        if box_size == 1 and box_start + 16 <= len(data):
            box_size = struct.unpack(">Q", data[box_start + 8 : box_start + 16])[0]
            header_size = 16
        else:
            header_size = 8
        if box_size >= header_size and box_start + box_size <= len(data):
            return box_start
        marker += 4


def repair_mp4_mdat_largesize(path: Path) -> dict:
    detail = {"file": path.name, "status": "skipped", "reason": ""}
    if not path.exists() or path.stat().st_size < 64:
        detail["reason"] = "missing_or_too_small"
        return detail

    data = path.read_bytes()
    mdat_type = data.find(b"mdat")
    if mdat_type < 4:
        detail["reason"] = "mdat_not_found"
        return detail
    mdat_start = mdat_type - 4
    header_size = struct.unpack(">I", data[mdat_start:mdat_type])[0]
    if header_size != 1:
        detail["reason"] = "mdat_not_extended_size"
        detail["mdat_header_size"] = header_size
        return detail

    moov_start = find_valid_box_marker(data, b"moov", mdat_type + 4)
    if moov_start is None or moov_start <= mdat_start + 16:
        detail["reason"] = "moov_not_found"
        return detail

    mdat_size = moov_start - mdat_start
    old_size = struct.unpack(">Q", data[mdat_start + 8 : mdat_start + 16])[0]
    detail.update(
        {
            "status": "ok",
            "mdat_offset": mdat_start,
            "moov_offset": moov_start,
            "old_mdat_largesize": old_size,
            "new_mdat_largesize": mdat_size,
        }
    )
    if old_size == mdat_size:
        detail["status"] = "unchanged"
        return detail

    with path.open("r+b") as output:
        output.seek(mdat_start + 8)
        output.write(struct.pack(">Q", mdat_size))
        output.flush()
        os.fsync(output.fileno())
    return detail


def repair_mp4_files(local_episode_dir: Path) -> dict:
    details = {}
    for file_name in MP4_FILES:
        path = local_episode_dir / file_name
        try:
            details[file_name] = repair_mp4_mdat_largesize(path)
        except Exception as exc:
            details[file_name] = {"file": file_name, "status": "error", "error": str(exc)}
    return details


def final_name(temp_episode: str) -> str:
    return temp_episode[:-5] if temp_episode.endswith("-temp") else temp_episode


def run_start(args: argparse.Namespace) -> int:
    status = {
        "status": "not_found",
        "phase": "start",
        "started_at_ms": now_ms(),
        "updated_at_ms": now_ms(),
        "error": "",
    }
    try:
        adb, detail = detect_ego(args.serial)
        status["detection"] = detail
        if adb is None:
            status["error"] = "no SXR/SXR_1 ego device found"
            write_status(args.status_file, status)
            return 0

        result = adb.run("shell", "am", "broadcast", "-a", f"{PACKAGE}.START_RECORDING", timeout=10)
        status["serial"] = adb.serial
        status["start_broadcast_returncode"] = result.returncode
        if result.returncode != 0:
            status["status"] = "start_failed"
            status["error"] = (result.stderr or result.stdout).strip()
            write_status(args.status_file, status)
            return 0

        remote_episode = ""
        deadline = time.monotonic() + args.detect_timeout
        while time.monotonic() < deadline:
            remote_episode = latest_temp_episode(adb)
            if remote_episode:
                break
            time.sleep(args.interval)

        if not remote_episode:
            status["status"] = "start_timeout"
            status["error"] = "ego app did not create episode_*-temp"
            write_status(args.status_file, status)
            return 0

        status.update(
            {
                "status": "recording",
                "phase": "sync",
                "remote_episode": remote_episode,
                "local_episode": str(args.episode_dir / "ego" / remote_episode),
            }
        )
        write_status(args.status_file, status)

        local_episode_dir = args.episode_dir / "ego" / remote_episode
        while True:
            sync_episode_once(adb, remote_episode, local_episode_dir)
            status["updated_at_ms"] = now_ms()
            write_status(args.status_file, status)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
    except Exception as exc:
        status["status"] = "error"
        status["error"] = str(exc)
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        return 0


def run_stop(args: argparse.Namespace) -> int:
    status = {}
    if args.status_file.exists():
        try:
            status = json.loads(args.status_file.read_text(encoding="utf-8"))
        except Exception:
            status = {}
    status.setdefault("started_at_ms", now_ms())
    status.update({"phase": "stop", "updated_at_ms": now_ms()})

    serial = str(status.get("serial") or args.serial or "")
    remote_episode = str(status.get("remote_episode") or "")
    try:
        if not serial:
            adb, detail = detect_ego(args.serial)
            status["detection"] = detail
            if adb is None:
                status["status"] = "not_found"
                status["error"] = "no SXR/SXR_1 ego device found during stop"
                write_status(args.status_file, status)
                return 0
        else:
            adb = Adb(serial)

        adb.run("shell", "am", "broadcast", "-a", f"{PACKAGE}.STOP_RECORDING", timeout=10)
    except Exception as exc:
        status["status"] = "error"
        status["error"] = str(exc)
        status["stop_broadcast_error"] = str(exc)
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        return 0

    if not remote_episode:
        remote_episode = latest_temp_episode(adb)
    if not remote_episode:
        status["status"] = "stop_no_remote_episode"
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        return 0

    local_episode_dir = args.episode_dir / "ego" / remote_episode
    final_episode = final_name(remote_episode)
    final_local_dir = args.episode_dir / "ego" / final_episode
    deadline = time.monotonic() + args.finalize_timeout
    while time.monotonic() < deadline:
        current_remote = remote_episode if remote_dir_exists(adb, remote_episode) else final_episode
        if remote_dir_exists(adb, current_remote):
            sync_episode_once(adb, current_remote, local_episode_dir)
        if current_remote == final_episode:
            if local_episode_dir != final_local_dir and local_episode_dir.exists() and not final_local_dir.exists():
                local_episode_dir.rename(final_local_dir)
            header_refresh_detail = refresh_final_mp4_headers(adb, final_episode, final_local_dir)
            repair_detail = repair_mp4_files(final_local_dir)
            status.update(
                {
                    "status": "finalized",
                    "phase": "done",
                    "remote_episode": final_episode,
                    "local_episode": str(final_local_dir),
                    "mp4_header_refresh": header_refresh_detail,
                    "mp4_repair": repair_detail,
                    "updated_at_ms": now_ms(),
                }
            )
            write_status(args.status_file, status)
            return 0
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        time.sleep(args.interval)

    status["status"] = "finalize_timeout"
    status["error"] = "ego episode did not finalize before timeout"
    status["updated_at_ms"] = now_ms()
    write_status(args.status_file, status)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("start", "stop"))
    parser.add_argument("--episode-dir", required=True, type=Path)
    parser.add_argument("--status-file", required=True, type=Path)
    parser.add_argument("--serial", default=os.environ.get("UGRIPPER_EGO_SERIAL", ""))
    parser.add_argument("--interval", type=float, default=float(os.environ.get("UGRIPPER_EGO_SYNC_INTERVAL_SEC", "1")))
    parser.add_argument("--detect-timeout", type=float, default=15.0)
    parser.add_argument("--finalize-timeout", type=float, default=60.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.episode_dir.mkdir(parents=True, exist_ok=True)
    if args.command == "start":
        return run_start(args)
    return run_stop(args)


if __name__ == "__main__":
    sys.exit(main())
