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
TIME_SYNC_SAMPLES = int(os.environ.get("UGRIPPER_EGO_TIME_SYNC_SAMPLES", "3"))
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
    tmp.write_text(json.dumps(compact_status(payload), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(path)


def compact_status(payload: dict) -> dict:
    if not isinstance(payload, dict):
        return {}
    compact = dict(payload)
    for key in (
        "detection",
        "pre_start_temp_episodes",
        "mp4_header_refresh",
        "mp4_repair",
        "stop_broadcast_error",
    ):
        compact.pop(key, None)
    if isinstance(compact.get("time_sync"), dict):
        compact["time_sync"] = compact_time_sync(compact["time_sync"])
    if isinstance(compact.get("remote_cleanup"), dict):
        cleanup = compact["remote_cleanup"]
        compact["remote_cleanup"] = {
            key: cleanup[key]
            for key in ("status", "error")
            if key in cleanup and cleanup[key] not in ("", None)
        }
    if isinstance(compact.get("finalize"), dict):
        finalize = compact["finalize"]
        compact["finalize"] = {
            key: finalize[key]
            for key in ("status", "error")
            if key in finalize and finalize[key] not in ("", None)
        }
    return compact


def compact_time_sync(detail: dict) -> dict:
    keys = (
        "status",
        "elapsed_ms",
        "pre_best_offset_ms",
        "mid_best_offset_ms",
        "post_best_offset_ms",
        "set_results",
        "error",
    )
    compact = {
        key: detail[key]
        for key in keys
        if key in detail and detail[key] not in ("", None)
    }
    if isinstance(compact.get("set_results"), list):
        compact["set_results"] = [
            summarize_set_time_result(result)
            for result in compact["set_results"]
            if isinstance(result, dict)
        ]
    return compact


def read_status(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


def ego_codec_for_camera_codec(camera_codec: str) -> str:
    codec = (camera_codec or "").strip().lower()
    if codec in ("h265", "hevc"):
        return "hevc"
    return "avc"


def set_ego_video_codec(adb: Adb, camera_codec: str) -> dict:
    ego_codec = ego_codec_for_camera_codec(camera_codec)
    detail = {
        "camera_codec": camera_codec,
        "ego_codec": ego_codec,
    }
    try:
        result = adb.run(
            "shell",
            "am",
            "broadcast",
            "-a",
            f"{PACKAGE}.SET_VIDEO_CODEC",
            "--es",
            "codec",
            ego_codec,
            timeout=10,
        )
    except Exception as exc:
        detail.update({"returncode": -1, "error": str(exc)})
        return detail
    detail.update(
        {
            "returncode": result.returncode,
            "stdout": result.stdout.strip().replace("\r", ""),
            "stderr": result.stderr.strip().replace("\r", ""),
        }
    )
    return detail


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


def list_temp_episodes(adb: Adb) -> list[str]:
    root_q = shell_quote(REMOTE_ROOT)
    command = f'for d in {root_q}/episode_*-temp; do [ -d "$d" ] && basename "$d"; done | sort'
    result = adb.run("shell", command, timeout=8)
    if result.returncode != 0:
        return []
    return [line.strip().replace("\r", "") for line in result.stdout.splitlines() if line.strip()]


def parse_epoch_ms(text: str) -> tuple[int | None, str]:
    value = text.strip().replace("\r", "")
    if not value.isdigit():
        return None, "invalid"
    if len(value) >= 18:
        return int(value[:19]) // 1000000, "ns"
    if len(value) >= 13:
        return int(value[:13]), "ms"
    if len(value) >= 10:
        return int(value[:10]) * 1000, "s"
    return None, "invalid"


def read_remote_epoch_ms(adb: Adb) -> tuple[int | None, dict]:
    commands = (
        ("date", "+%s%3N"),
        ("date", "+%s%N"),
        ("toybox", "date", "+%s%3N"),
        ("toybox", "date", "+%s%N"),
    )
    attempts = []
    for command in commands:
        started = now_ms()
        result = adb.run("shell", *command, timeout=3)
        finished = now_ms()
        remote_ms, precision = parse_epoch_ms(result.stdout)
        attempts.append(
            {
                "command": " ".join(command),
                "returncode": result.returncode,
                "elapsed_ms": finished - started,
                "precision": precision,
                "stdout": result.stdout.strip().replace("\r", ""),
                "stderr": result.stderr.strip().replace("\r", ""),
            }
        )
        if result.returncode == 0 and remote_ms is not None:
            return remote_ms, attempts[-1]
    return None, {"attempts": attempts}


def measure_time_offset_sample(adb: Adb) -> dict:
    host_before_ms = now_ms()
    remote_ms, read_detail = read_remote_epoch_ms(adb)
    host_after_ms = now_ms()
    sample = {
        "host_before_ms": host_before_ms,
        "host_after_ms": host_after_ms,
        "rtt_ms": host_after_ms - host_before_ms,
        "remote_ms": remote_ms,
        "read": read_detail,
    }
    if remote_ms is not None:
        host_midpoint_ms = (host_before_ms + host_after_ms) / 2.0
        sample["offset_ms"] = round(remote_ms - host_midpoint_ms, 3)
    return sample


def measure_time_offsets(adb: Adb, samples: int) -> list[dict]:
    return [measure_time_offset_sample(adb) for _ in range(max(1, samples))]


def best_offset_ms(samples: list[dict]) -> float | None:
    valid = [sample for sample in samples if isinstance(sample.get("offset_ms"), (int, float))]
    if not valid:
        return None
    best = min(valid, key=lambda item: int(item.get("rtt_ms") or 999999))
    return float(best["offset_ms"])


def set_remote_time_ms(adb: Adb, target_ms: int) -> dict:
    started = now_ms()
    result = adb.run("shell", "cmd", "alarm", "set-time", str(int(target_ms)), timeout=5)
    finished = now_ms()
    return {
        "target_ms": int(target_ms),
        "returncode": result.returncode,
        "elapsed_ms": finished - started,
        "stdout": result.stdout.strip().replace("\r", ""),
        "stderr": result.stderr.strip().replace("\r", ""),
    }


def summarize_set_time_result(result: dict) -> dict:
    detail = {
        "returncode": result.get("returncode"),
        "elapsed_ms": result.get("elapsed_ms"),
    }
    error = result.get("stderr") or result.get("stdout")
    if result.get("returncode") != 0 and error:
        detail["error"] = str(error)
    return detail


def sync_ego_time(adb: Adb) -> dict:
    started = now_ms()
    detail: dict = {
        "status": "error",
        "started_at_ms": started,
        "sample_count": max(1, TIME_SYNC_SAMPLES),
    }
    try:
        pre_samples = measure_time_offsets(adb, TIME_SYNC_SAMPLES)
        first_set = set_remote_time_ms(adb, now_ms())
        mid_samples = measure_time_offsets(adb, TIME_SYNC_SAMPLES)
        correction_offset = best_offset_ms(mid_samples)
        set_results = [first_set]
        if first_set.get("returncode") == 0 and correction_offset is not None:
            corrected_target_ms = now_ms() - int(round(correction_offset))
            set_results.append(set_remote_time_ms(adb, corrected_target_ms))
        post_samples = measure_time_offsets(adb, TIME_SYNC_SAMPLES)

        final_offset = best_offset_ms(post_samples)
        errors = [
            (result.get("stderr") or result.get("stdout") or f"set-time rc={result.get('returncode')}")
            for result in set_results
            if result.get("returncode") != 0
        ]
        finished = now_ms()
        detail.update(
            {
                "status": "ok" if not errors and final_offset is not None else "error",
                "finished_at_ms": finished,
                "elapsed_ms": finished - started,
                "pre_best_offset_ms": best_offset_ms(pre_samples),
                "mid_best_offset_ms": correction_offset,
                "post_best_offset_ms": final_offset,
                "set_results": [summarize_set_time_result(result) for result in set_results],
            }
        )
        if errors:
            detail["error"] = "; ".join(str(error) for error in errors if error)
        elif final_offset is None:
            detail["error"] = "unable to read ego time after sync"
        return detail
    except Exception as exc:
        finished = now_ms()
        detail.update(
            {
                "status": "error",
                "finished_at_ms": finished,
                "elapsed_ms": finished - started,
                "error": str(exc),
            }
        )
        return detail


def latest_temp_episode(adb: Adb, exclude: set[str] | None = None) -> str:
    excluded = exclude or set()
    candidates = [episode for episode in list_temp_episodes(adb) if episode not in excluded]
    return candidates[-1] if candidates else ""


def remote_path(episode_name: str, file_name: str = "") -> str:
    base = f"{REMOTE_ROOT.rstrip('/')}/{episode_name}"
    return f"{base}/{file_name}" if file_name else base


def remote_dir_exists(adb: Adb, episode_name: str) -> bool:
    result = adb.run("shell", "test", "-d", remote_path(episode_name), timeout=8)
    return result.returncode == 0


def remote_dir_missing(adb: Adb, episode_name: str) -> bool:
    result = adb.run("shell", "test", "!", "-e", remote_path(episode_name), timeout=8)
    return result.returncode == 0


def remote_file_size(adb: Adb, remote_file: str) -> int | None:
    result = adb.run("shell", "stat", "-c", "%s", remote_file, timeout=8)
    if result.returncode != 0:
        return None
    text = result.stdout.strip().replace("\r", "")
    return int(text) if text.isdigit() else None


def sync_one_file(
    adb: Adb,
    remote_episode: str,
    local_episode_dir: Path,
    file_name: str,
    replace_shrunk: bool = False,
) -> int:
    remote_file = remote_path(remote_episode, file_name)
    size = remote_file_size(adb, remote_file)
    if size is None:
        return 0
    local_episode_dir.mkdir(parents=True, exist_ok=True)
    local_file = local_episode_dir / file_name
    local_size = local_file.stat().st_size if local_file.exists() else 0
    if replace_shrunk and size < local_size:
        local_file.unlink()
        local_size = 0
    if size <= local_size:
        return 0
    delta = size - local_size
    data = adb.exec_out_bytes(remote_file, local_size, delta)
    with local_file.open("ab") as output:
        output.write(data)
    return len(data)


def sync_episode_once(
    adb: Adb,
    remote_episode: str,
    local_episode_dir: Path,
    replace_shrunk: bool = False,
) -> dict:
    copied = {}
    for file_name in FILES:
        try:
            delta = sync_one_file(adb, remote_episode, local_episode_dir, file_name, replace_shrunk=replace_shrunk)
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


def summarize_finalize_repair(header_refresh_detail: dict, repair_detail: dict) -> dict:
    errors = []
    for group_name, details in (
        ("mp4_header_refresh", header_refresh_detail),
        ("mp4_repair", repair_detail),
    ):
        if not isinstance(details, dict):
            continue
        for file_name, detail in details.items():
            if not isinstance(detail, dict):
                continue
            if detail.get("status") == "error":
                errors.append(f"{group_name}.{file_name}: {detail.get('error') or detail.get('reason') or 'error'}")
    if errors:
        return {"status": "error", "error": "; ".join(errors)}
    return {"status": "finalized"}


def cleanup_status(status: str, remote_episode: str = "", error: str = "", **extra: object) -> dict:
    detail = {"status": status}
    if error:
        detail["error"] = error
    return detail


def safe_remote_episode_name(name: str) -> bool:
    return bool(name) and name.startswith("episode_") and "/" not in name and ".." not in name and not name.endswith("-temp")


def verify_remote_and_local_file_sizes(adb: Adb, remote_episode: str, local_episode_dir: Path) -> tuple[bool, dict]:
    details = {}
    ok = True
    for file_name in FILES:
        local_file = local_episode_dir / file_name
        local_size = local_file.stat().st_size if local_file.exists() else None
        remote_size = remote_file_size(adb, remote_path(remote_episode, file_name))
        file_ok = local_size is not None and remote_size is not None and int(local_size) == int(remote_size)
        if not file_ok:
            ok = False
        details[file_name] = {
            "ok": file_ok,
            "local_size": local_size,
            "remote_size": remote_size,
        }
    return ok, details


def remove_remote_episode(adb: Adb, remote_episode: str) -> tuple[bool, str]:
    if not safe_remote_episode_name(remote_episode):
        return False, f"unsafe remote episode name: {remote_episode}"
    remote_q = shell_quote(remote_path(remote_episode))
    command = f"rm -rf {remote_q}"
    result = adb.run("shell", command, timeout=30)
    if result.returncode != 0:
        return False, (result.stderr or result.stdout).strip() or "remote rm failed"
    if not remote_dir_missing(adb, remote_episode):
        return False, "remote episode still exists after rm"
    return True, ""


def run_cleanup(args: argparse.Namespace) -> int:
    status = read_status(args.status_file)
    status.setdefault("started_at_ms", now_ms())
    status.update({"phase": "cleanup", "updated_at_ms": now_ms()})

    remote_episode = str(status.get("remote_episode") or "")
    local_episode = str(status.get("local_episode") or "")
    serial = str(status.get("serial") or args.serial or "")

    def finish(detail: dict, ok: bool = True) -> int:
        status["remote_cleanup"] = detail
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        return 0 if ok else 1

    if status.get("status") != "finalized":
        return finish(cleanup_status("skipped", remote_episode, "ego sync status is not finalized"))
    if not safe_remote_episode_name(remote_episode):
        return finish(cleanup_status("skipped", remote_episode, "remote episode is missing, unsafe, or still temporary"), False)
    local_episode_dir = Path(local_episode) if local_episode else args.episode_dir / "ego"
    if not local_episode_dir.exists():
        return finish(cleanup_status("skipped", remote_episode, f"local ego episode missing: {local_episode_dir}"), False)

    try:
        adb = Adb(serial) if serial else detect_ego(args.serial)[0]
        if adb is None:
            return finish(cleanup_status("error", remote_episode, "no SXR/SXR_1 ego device found during cleanup"), False)
        if not remote_dir_exists(adb, remote_episode):
            return finish(cleanup_status("already_missing", remote_episode, deleted_at_ms=now_ms()))
        sizes_ok, size_details = verify_remote_and_local_file_sizes(adb, remote_episode, local_episode_dir)
        if not sizes_ok:
            return finish(cleanup_status("skipped", remote_episode, "remote/local file sizes do not match", files=size_details), False)
        deleted, error = remove_remote_episode(adb, remote_episode)
        if not deleted:
            return finish(cleanup_status("error", remote_episode, error, files=size_details), False)
        return finish(cleanup_status("deleted", remote_episode, deleted_at_ms=now_ms(), files=size_details))
    except Exception as exc:
        return finish(cleanup_status("error", remote_episode, str(exc)), False)


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
        adb, _ = detect_ego(args.serial)
        if adb is None:
            status["error"] = "no SXR/SXR_1 ego device found"
            write_status(args.status_file, status)
            return 0

        status["serial"] = adb.serial
        status["time_sync"] = {
            "status": "running",
            "started_at_ms": now_ms(),
        }
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        status["time_sync"] = sync_ego_time(adb)
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)

        existing_temp_episodes = list_temp_episodes(adb)
        status["video_codec"] = set_ego_video_codec(adb, args.codec)
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)

        result = adb.run("shell", "am", "broadcast", "-a", f"{PACKAGE}.START_RECORDING", timeout=10)
        status["start_broadcast_returncode"] = result.returncode
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        if result.returncode != 0:
            status["status"] = "start_failed"
            status["error"] = (result.stderr or result.stdout).strip()
            write_status(args.status_file, status)
            return 0

        remote_episode = ""
        deadline = time.monotonic() + args.detect_timeout
        excluded_temp_episodes = set(existing_temp_episodes)
        while time.monotonic() < deadline:
            remote_episode = latest_temp_episode(adb, excluded_temp_episodes)
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
                "local_episode": str(args.episode_dir / "ego"),
            }
        )
        write_status(args.status_file, status)

        local_episode_dir = args.episode_dir / "ego"
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
    status = read_status(args.status_file)
    status.setdefault("started_at_ms", now_ms())
    status.update({"phase": "stop", "updated_at_ms": now_ms()})

    serial = str(status.get("serial") or args.serial or "")
    remote_episode = str(status.get("remote_episode") or "")
    try:
        if not serial:
            adb, _ = detect_ego(args.serial)
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
        status["status"] = "stop_no_remote_episode"
        status["error"] = "remote_episode missing from ego_sync.json"
        status["updated_at_ms"] = now_ms()
        write_status(args.status_file, status)
        return 0

    local_episode_dir = args.episode_dir / "ego"
    final_episode = final_name(remote_episode)
    final_local_dir = args.episode_dir / "ego"
    deadline = time.monotonic() + args.finalize_timeout
    while time.monotonic() < deadline:
        current_remote = remote_episode if remote_dir_exists(adb, remote_episode) else final_episode
        if remote_dir_exists(adb, current_remote):
            sync_episode_once(
                adb,
                current_remote,
                local_episode_dir,
                replace_shrunk=(current_remote == final_episode),
            )
        if current_remote == final_episode:
            header_refresh_detail = refresh_final_mp4_headers(adb, final_episode, final_local_dir)
            repair_detail = repair_mp4_files(final_local_dir)
            status.update(
                {
                    "status": "finalized",
                    "phase": "done",
                    "remote_episode": final_episode,
                    "local_episode": str(final_local_dir),
                    "finalize": summarize_finalize_repair(header_refresh_detail, repair_detail),
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
    parser.add_argument("command", choices=("start", "stop", "cleanup"))
    parser.add_argument("--episode-dir", required=True, type=Path)
    parser.add_argument("--status-file", required=True, type=Path)
    parser.add_argument("--serial", default=os.environ.get("UGRIPPER_EGO_SERIAL", ""))
    parser.add_argument("--interval", type=float, default=float(os.environ.get("UGRIPPER_EGO_SYNC_INTERVAL_SEC", "1")))
    parser.add_argument("--detect-timeout", type=float, default=15.0)
    parser.add_argument("--finalize-timeout", type=float, default=60.0)
    parser.add_argument("--codec", choices=("h264", "h265"), default="h264")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.episode_dir.mkdir(parents=True, exist_ok=True)
    if args.command == "start":
        return run_start(args)
    if args.command == "stop":
        return run_stop(args)
    return run_cleanup(args)


if __name__ == "__main__":
    sys.exit(main())
