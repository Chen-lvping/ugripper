#!/usr/bin/env python3
import argparse
import csv
import json
import os
import statistics
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


SCAN_SKIP_DIRS = {"logs", "runtime", "__pycache__", ".Trash-1000", "lost+found"}
VIDEO_STREAM_FPS = {
    "left_cam_main.mkv": 60.0,
    "right_cam_main.mkv": 60.0,
    "left_stereo.mkv": 60.0,
    "right_stereo.mkv": 60.0,
    "left_tcam_l.mkv": 120.0,
    "left_tcam_r.mkv": 120.0,
    "right_tcam_l.mkv": 120.0,
    "right_tcam_r.mkv": 120.0,
}
DEFAULT_REPORT_ROOT = Path.home() / "v2_episode_video_drop_report"


def safe_iter_dirs(path):
    try:
        return sorted([item for item in path.iterdir() if item.is_dir()], key=lambda item: item.name)
    except Exception:
        return []


def list_episode_dirs(data_dir):
    if not data_dir.is_dir():
        return []
    try:
        return sorted(
            [item for item in data_dir.iterdir() if item.is_dir() and item.name.startswith("episode_")],
            key=lambda item: item.name,
        )
    except Exception:
        return []


def has_episode_dirs(device_root):
    return len(list_episode_dirs(device_root / "data")) > 0


def candidate_key(path):
    try:
        return str(path.resolve())
    except Exception:
        return str(path)


def discover_device_roots(root_dir):
    found = []
    seen = set()

    def try_add(path):
        if not has_episode_dirs(path):
            return
        key = candidate_key(path)
        if key in seen:
            return
        seen.add(key)
        found.append(path)

    try_add(root_dir)
    if found:
        return found

    for child in safe_iter_dirs(root_dir):
        if child.name in SCAN_SKIP_DIRS or child.name.endswith("_err"):
            continue
        try_add(child)
        for sub in safe_iter_dirs(child):
            if sub.name in SCAN_SKIP_DIRS or sub.name.endswith("_err"):
                continue
            try_add(sub)

    return found


def detect_latest_root():
    candidates = []
    for prefix in (
        Path("/media") / os.environ.get("USER", Path.home().name) / "data",
        Path.home(),
        Path.home() / "Desktop",
        Path.home() / "桌面",
    ):
        if not prefix.exists():
            continue
        try:
            for path in prefix.glob("v2_usb_backups_*"):
                if path.is_dir():
                    candidates.append(path)
        except Exception:
            continue
    if not candidates:
        return None
    candidates.sort(key=lambda item: item.stat().st_mtime, reverse=True)
    return candidates[0]


def resolve_root_dir(root_dir_arg):
    if root_dir_arg:
        return Path(root_dir_arg).expanduser().resolve()
    detected = detect_latest_root()
    return detected.resolve() if detected else None


def read_json(path):
    try:
        with path.open("r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def resolve_device_id(device_root):
    for ep_dir in list_episode_dirs(device_root / "data")[:3]:
        metadata = read_json(ep_dir / "metadata.json")
        if isinstance(metadata, dict):
            device_id = str(metadata.get("device_id") or "").strip()
            if device_id:
                return device_id.upper()
    return device_root.name.upper()


def safe_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def safe_pct_text(value):
    if value is None:
        return "-"
    return f"{value:.4f}%"


def run_ffprobe_json(cmd, timeout=30):
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=timeout,
            check=False,
        )
    except Exception:
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    try:
        return json.loads(proc.stdout)
    except Exception:
        return None


def probe_packets(video_path):
    return run_ffprobe_json(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "packet=pts_time,dts_time,duration_time",
            "-of",
            "json",
            str(video_path),
        ],
        timeout=90,
    )


def probe_format_meta(video_path):
    return run_ffprobe_json(
        [
            "ffprobe",
            "-v",
            "error",
            "-count_packets",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=start_time,avg_frame_rate,r_frame_rate,nb_read_packets,nb_frames:format=duration",
            "-of",
            "json",
            str(video_path),
        ],
        timeout=30,
    )


def analyze_packet_sequence(packets, expected_fps):
    frame_step = 1.0 / expected_fps
    timestamps = []
    packet_durations = []

    for packet in packets:
        ts = safe_float(packet.get("dts_time"))
        if ts is None:
            ts = safe_float(packet.get("pts_time"))
        if ts is None:
            continue
        duration = safe_float(packet.get("duration_time"))
        timestamps.append(ts)
        packet_durations.append(duration)

    if not timestamps:
        return None

    duplicate_timestamp_count = 0
    non_monotonic_count = 0
    positive_gaps = []
    large_gap_event_count = 0
    max_positive_gap_sec = 0.0

    for prev_ts, curr_ts in zip(timestamps, timestamps[1:]):
        gap_sec = curr_ts - prev_ts
        if gap_sec == 0:
            duplicate_timestamp_count += 1
            continue
        if gap_sec < 0:
            non_monotonic_count += 1
            continue
        positive_gaps.append(gap_sec)
        if gap_sec > frame_step * 1.5:
            large_gap_event_count += 1
        if gap_sec > max_positive_gap_sec:
            max_positive_gap_sec = gap_sec

    median_positive_gap_sec = statistics.median(positive_gaps) if positive_gaps else frame_step
    last_duration_sec = next(
        (item for item in reversed(packet_durations) if item is not None and item > 0),
        None,
    )
    if last_duration_sec is None:
        last_duration_sec = median_positive_gap_sec if median_positive_gap_sec > 0 else frame_step

    first_ts = timestamps[0]
    last_end_sec = timestamps[-1] + max(last_duration_sec, 0.0)
    packet_span_sec = max(last_end_sec - first_ts, max(last_duration_sec, frame_step))
    actual_frames = len(timestamps)
    packet_expected_frames = max(actual_frames, int(round(packet_span_sec * expected_fps)))
    packet_drop_frames = max(packet_expected_frames - actual_frames, 0)
    packet_drop_rate = packet_drop_frames / packet_expected_frames if packet_expected_frames > 0 else 0.0

    return {
        "actual_frames": actual_frames,
        "first_ts_sec": first_ts,
        "last_end_sec": last_end_sec,
        "packet_span_sec": packet_span_sec,
        "packet_expected_frames": packet_expected_frames,
        "packet_drop_frames": packet_drop_frames,
        "packet_drop_rate": packet_drop_rate,
        "median_positive_gap_sec": median_positive_gap_sec,
        "duplicate_timestamp_count": duplicate_timestamp_count,
        "non_monotonic_count": non_monotonic_count,
        "large_gap_event_count": large_gap_event_count,
        "max_positive_gap_sec": max_positive_gap_sec,
    }


def analyze_episode_stream(device_name, device_id, ep_dir, stream_name, expected_fps):
    video_path = ep_dir / stream_name
    row = {
        "device_name": device_name,
        "device_id": device_id,
        "episode": ep_dir.name,
        "stream_name": stream_name,
        "expected_fps": expected_fps,
        "video_path": str(video_path),
        "row_status": "ok",
        "issue": "",
        "actual_frames": None,
        "first_ts_sec": None,
        "last_end_sec": None,
        "packet_span_sec": None,
        "packet_expected_frames": None,
        "packet_drop_frames": None,
        "packet_drop_rate": None,
        "format_duration_sec": None,
        "start_time_sec": None,
        "format_effective_span_sec": None,
        "format_expected_frames": None,
        "format_drop_frames": None,
        "format_drop_rate": None,
        "actual_fps": None,
        "median_positive_gap_ms": None,
        "duplicate_timestamp_count": None,
        "non_monotonic_count": None,
        "large_gap_event_count": None,
        "max_positive_gap_ms": None,
        "meta_nb_read_packets": None,
        "meta_nb_frames": None,
        "meta_avg_frame_rate": None,
        "meta_r_frame_rate": None,
    }

    if not video_path.exists():
        row["row_status"] = "invalid"
        row["issue"] = "missing_file"
        return row

    try:
        if video_path.stat().st_size <= 0:
            row["row_status"] = "invalid"
            row["issue"] = "zero_size_file"
            return row
    except OSError:
        row["row_status"] = "invalid"
        row["issue"] = "stat_failed"
        return row

    packet_data = probe_packets(video_path)
    if not packet_data:
        row["row_status"] = "invalid"
        row["issue"] = "packet_probe_failed"
        return row

    packet_stats = analyze_packet_sequence(packet_data.get("packets", []), expected_fps)
    if not packet_stats:
        row["row_status"] = "invalid"
        row["issue"] = "no_valid_packets"
        return row

    meta_data = probe_format_meta(video_path) or {}
    meta_stream = (meta_data.get("streams") or [{}])[0]
    meta_format = meta_data.get("format") or {}

    row["actual_frames"] = packet_stats["actual_frames"]
    row["first_ts_sec"] = packet_stats["first_ts_sec"]
    row["last_end_sec"] = packet_stats["last_end_sec"]
    row["packet_span_sec"] = packet_stats["packet_span_sec"]
    row["packet_expected_frames"] = packet_stats["packet_expected_frames"]
    row["packet_drop_frames"] = packet_stats["packet_drop_frames"]
    row["packet_drop_rate"] = packet_stats["packet_drop_rate"]
    row["actual_fps"] = (
        packet_stats["actual_frames"] / packet_stats["packet_span_sec"]
        if packet_stats["packet_span_sec"] > 0
        else None
    )
    row["median_positive_gap_ms"] = packet_stats["median_positive_gap_sec"] * 1000.0
    row["duplicate_timestamp_count"] = packet_stats["duplicate_timestamp_count"]
    row["non_monotonic_count"] = packet_stats["non_monotonic_count"]
    row["large_gap_event_count"] = packet_stats["large_gap_event_count"]
    row["max_positive_gap_ms"] = packet_stats["max_positive_gap_sec"] * 1000.0

    row["format_duration_sec"] = safe_float(meta_format.get("duration"))
    row["start_time_sec"] = safe_float(meta_stream.get("start_time"))
    if (
        row["format_duration_sec"] is not None
        and row["start_time_sec"] is not None
        and row["format_duration_sec"] > row["start_time_sec"]
    ):
        row["format_effective_span_sec"] = row["format_duration_sec"] - row["start_time_sec"]
        row["format_expected_frames"] = max(
            row["actual_frames"],
            int(round(row["format_effective_span_sec"] * expected_fps)),
        )
        row["format_drop_frames"] = max(row["format_expected_frames"] - row["actual_frames"], 0)
        row["format_drop_rate"] = (
            row["format_drop_frames"] / row["format_expected_frames"]
            if row["format_expected_frames"] > 0
            else 0.0
        )

    row["meta_nb_read_packets"] = meta_stream.get("nb_read_packets") or ""
    row["meta_nb_frames"] = meta_stream.get("nb_frames") or ""
    row["meta_avg_frame_rate"] = meta_stream.get("avg_frame_rate") or ""
    row["meta_r_frame_rate"] = meta_stream.get("r_frame_rate") or ""
    return row


def summarize_rows(rows):
    valid_rows = [row for row in rows if row["row_status"] == "ok"]
    invalid_rows = [row for row in rows if row["row_status"] != "ok"]

    total_actual_frames = sum(row["actual_frames"] or 0 for row in valid_rows)
    total_packet_drop_frames = sum(row["packet_drop_frames"] or 0 for row in valid_rows)
    total_packet_expected_frames = sum(row["packet_expected_frames"] or 0 for row in valid_rows)
    total_format_drop_frames = sum(row["format_drop_frames"] or 0 for row in valid_rows)
    total_format_expected_frames = sum(row["format_expected_frames"] or 0 for row in valid_rows)

    return {
        "valid_row_count": len(valid_rows),
        "invalid_row_count": len(invalid_rows),
        "total_actual_frames": total_actual_frames,
        "total_packet_drop_frames": total_packet_drop_frames,
        "total_packet_expected_frames": total_packet_expected_frames,
        "total_packet_drop_rate": (
            total_packet_drop_frames / total_packet_expected_frames
            if total_packet_expected_frames > 0
            else None
        ),
        "total_format_drop_frames": total_format_drop_frames,
        "total_format_expected_frames": total_format_expected_frames,
        "total_format_drop_rate": (
            total_format_drop_frames / total_format_expected_frames
            if total_format_expected_frames > 0
            else None
        ),
    }


def summarize_by_key(rows, key):
    grouped = {}
    for row in rows:
        grouped.setdefault(row[key], []).append(row)

    summary_rows = []
    for group_key in sorted(grouped):
        items = grouped[group_key]
        summary = summarize_rows(items)
        valid_items = [item for item in items if item["row_status"] == "ok" and item["packet_drop_rate"] is not None]
        worst_item = max(valid_items, key=lambda item: (item["packet_drop_rate"], item["packet_drop_frames"] or 0)) if valid_items else None
        summary_rows.append(
            {
                key: group_key,
                "row_count": len(items),
                "valid_row_count": summary["valid_row_count"],
                "invalid_row_count": summary["invalid_row_count"],
                "total_actual_frames": summary["total_actual_frames"],
                "total_packet_drop_frames": summary["total_packet_drop_frames"],
                "total_packet_expected_frames": summary["total_packet_expected_frames"],
                "total_packet_drop_rate": summary["total_packet_drop_rate"],
                "total_format_drop_frames": summary["total_format_drop_frames"],
                "total_format_expected_frames": summary["total_format_expected_frames"],
                "total_format_drop_rate": summary["total_format_drop_rate"],
                "worst_episode": worst_item["episode"] if worst_item else "",
                "worst_stream_name": worst_item["stream_name"] if worst_item else "",
                "worst_packet_drop_rate": worst_item["packet_drop_rate"] if worst_item else None,
                "worst_packet_drop_frames": worst_item["packet_drop_frames"] if worst_item else None,
            }
        )
    return summary_rows


def analyze_device_root(device_root, jobs):
    device_name = device_root.name
    device_id = resolve_device_id(device_root)
    episode_dirs = list_episode_dirs(device_root / "data")
    rows = []

    task_count = max(1, len(episode_dirs) * len(VIDEO_STREAM_FPS))
    max_workers = max(1, min(jobs, task_count))
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_map = {}
        for ep_dir in episode_dirs:
            for stream_name, expected_fps in VIDEO_STREAM_FPS.items():
                future = executor.submit(
                    analyze_episode_stream,
                    device_name,
                    device_id,
                    ep_dir,
                    stream_name,
                    expected_fps,
                )
                future_map[future] = (ep_dir.name, stream_name)

        for future in as_completed(future_map):
            rows.append(future.result())

    rows.sort(key=lambda row: (row["episode"], row["stream_name"]))
    return {
        "device_name": device_name,
        "device_id": device_id,
        "device_root": str(device_root),
        "episode_count": len(episode_dirs),
        "rows": rows,
        "stream_summary": summarize_by_key(rows, "stream_name"),
        "episode_summary": summarize_by_key(rows, "episode"),
        "summary": summarize_rows(rows),
    }


def evaluate_device_result(result, max_device_drop_rate_pct, max_stream_drop_rate_pct):
    summary = result["summary"]
    fail_reasons = []
    total_packet_drop_rate_pct = (
        (summary["total_packet_drop_rate"] or 0.0) * 100.0
        if summary["total_packet_drop_rate"] is not None
        else None
    )

    if summary["invalid_row_count"] > 0:
        fail_reasons.append(f"存在 {summary['invalid_row_count']} 个无效视频流")

    if (
        max_device_drop_rate_pct >= 0
        and total_packet_drop_rate_pct is not None
        and total_packet_drop_rate_pct > max_device_drop_rate_pct
    ):
        fail_reasons.append(
            f"设备总丢帧率 {safe_pct_text(total_packet_drop_rate_pct)} 超过阈值 "
            f"{safe_pct_text(max_device_drop_rate_pct)}"
        )

    if max_stream_drop_rate_pct >= 0:
        for row in result["stream_summary"]:
            stream_rate_pct = (
                (row["total_packet_drop_rate"] or 0.0) * 100.0
                if row["total_packet_drop_rate"] is not None
                else None
            )
            if stream_rate_pct is not None and stream_rate_pct > max_stream_drop_rate_pct:
                fail_reasons.append(
                    f"{row['stream_name']} 丢帧率 {safe_pct_text(stream_rate_pct)} 超过阈值 "
                    f"{safe_pct_text(max_stream_drop_rate_pct)}"
                )

    return {
        "status": "PASS" if not fail_reasons else "FAIL",
        "fail_reasons": fail_reasons,
    }


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def float_or_default(value, default=0.0):
    parsed = safe_float(value)
    return parsed if parsed is not None else default


def build_device_summary_rows(results, evaluations):
    rows = []
    for result in results:
        evaluation = evaluations[result["device_id"]]
        summary = result["summary"]
        valid_episodes = [row for row in result["episode_summary"] if row["total_packet_drop_rate"] is not None]
        worst_episode = max(valid_episodes, key=lambda row: row["total_packet_drop_rate"]) if valid_episodes else None
        valid_streams = [row for row in result["stream_summary"] if row["total_packet_drop_rate"] is not None]
        worst_stream = max(valid_streams, key=lambda row: row["total_packet_drop_rate"]) if valid_streams else None

        rows.append(
            {
                "device_name": result["device_name"],
                "device_id": result["device_id"],
                "device_root": result["device_root"],
                "episode_count": result["episode_count"],
                "status": evaluation["status"],
                "fail_reasons": " | ".join(evaluation["fail_reasons"]),
                "valid_row_count": summary["valid_row_count"],
                "invalid_row_count": summary["invalid_row_count"],
                "total_actual_frames": summary["total_actual_frames"],
                "total_packet_drop_frames": summary["total_packet_drop_frames"],
                "total_packet_expected_frames": summary["total_packet_expected_frames"],
                "device_drop_rate_pct": round((summary["total_packet_drop_rate"] or 0.0) * 100.0, 6)
                if summary["total_packet_drop_rate"] is not None
                else "",
                "total_format_drop_frames": summary["total_format_drop_frames"],
                "total_format_expected_frames": summary["total_format_expected_frames"],
                "format_drop_rate_pct": round((summary["total_format_drop_rate"] or 0.0) * 100.0, 6)
                if summary["total_format_drop_rate"] is not None
                else "",
                "worst_episode": worst_episode["episode"] if worst_episode else "",
                "worst_episode_drop_rate_pct": round((worst_episode["total_packet_drop_rate"] or 0.0) * 100.0, 6)
                if worst_episode and worst_episode["total_packet_drop_rate"] is not None
                else "",
                "worst_stream_name": worst_stream["stream_name"] if worst_stream else "",
                "worst_stream_drop_rate_pct": round((worst_stream["total_packet_drop_rate"] or 0.0) * 100.0, 6)
                if worst_stream and worst_stream["total_packet_drop_rate"] is not None
                else "",
            }
        )
    return rows


def build_stream_summary_rows(results, evaluations):
    rows = []
    for result in results:
        evaluation = evaluations[result["device_id"]]
        for row in result["stream_summary"]:
            rows.append(
                {
                    "device_name": result["device_name"],
                    "device_id": result["device_id"],
                    "status": evaluation["status"],
                    "stream_name": row["stream_name"],
                    "row_count": row["row_count"],
                    "valid_row_count": row["valid_row_count"],
                    "invalid_row_count": row["invalid_row_count"],
                    "total_actual_frames": row["total_actual_frames"],
                    "total_packet_drop_frames": row["total_packet_drop_frames"],
                    "total_packet_expected_frames": row["total_packet_expected_frames"],
                    "drop_rate_pct": round((row["total_packet_drop_rate"] or 0.0) * 100.0, 6)
                    if row["total_packet_drop_rate"] is not None
                    else "",
                    "total_format_drop_frames": row["total_format_drop_frames"],
                    "total_format_expected_frames": row["total_format_expected_frames"],
                    "format_drop_rate_pct": round((row["total_format_drop_rate"] or 0.0) * 100.0, 6)
                    if row["total_format_drop_rate"] is not None
                    else "",
                    "worst_episode": row["worst_episode"],
                    "worst_packet_drop_rate_pct": round((row["worst_packet_drop_rate"] or 0.0) * 100.0, 6)
                    if row["worst_packet_drop_rate"] is not None
                    else "",
                }
            )
    return rows


def build_episode_summary_rows(results, evaluations):
    rows = []
    for result in results:
        evaluation = evaluations[result["device_id"]]
        for row in result["episode_summary"]:
            rows.append(
                {
                    "device_name": result["device_name"],
                    "device_id": result["device_id"],
                    "status": evaluation["status"],
                    "episode": row["episode"],
                    "row_count": row["row_count"],
                    "valid_row_count": row["valid_row_count"],
                    "invalid_row_count": row["invalid_row_count"],
                    "total_actual_frames": row["total_actual_frames"],
                    "total_packet_drop_frames": row["total_packet_drop_frames"],
                    "total_packet_expected_frames": row["total_packet_expected_frames"],
                    "drop_rate_pct": round((row["total_packet_drop_rate"] or 0.0) * 100.0, 6)
                    if row["total_packet_drop_rate"] is not None
                    else "",
                    "total_format_drop_frames": row["total_format_drop_frames"],
                    "total_format_expected_frames": row["total_format_expected_frames"],
                    "format_drop_rate_pct": round((row["total_format_drop_rate"] or 0.0) * 100.0, 6)
                    if row["total_format_drop_rate"] is not None
                    else "",
                    "worst_stream_name": row["worst_stream_name"],
                    "worst_packet_drop_rate_pct": round((row["worst_packet_drop_rate"] or 0.0) * 100.0, 6)
                    if row["worst_packet_drop_rate"] is not None
                    else "",
                }
            )
    return rows


def build_episode_stream_rows(results, evaluations):
    rows = []
    for result in results:
        evaluation = evaluations[result["device_id"]]
        for row in result["rows"]:
            rows.append(
                {
                    "device_name": row["device_name"],
                    "device_id": row["device_id"],
                    "status": evaluation["status"],
                    "episode": row["episode"],
                    "stream_name": row["stream_name"],
                    "expected_fps": row["expected_fps"],
                    "actual_frames": row["actual_frames"] if row["actual_frames"] is not None else "",
                    "first_ts_sec": round(row["first_ts_sec"], 6) if row["first_ts_sec"] is not None else "",
                    "last_end_sec": round(row["last_end_sec"], 6) if row["last_end_sec"] is not None else "",
                    "packet_span_sec": round(row["packet_span_sec"], 6) if row["packet_span_sec"] is not None else "",
                    "packet_expected_frames": row["packet_expected_frames"] if row["packet_expected_frames"] is not None else "",
                    "packet_drop_frames": row["packet_drop_frames"] if row["packet_drop_frames"] is not None else "",
                    "packet_drop_rate_pct": round((row["packet_drop_rate"] or 0.0) * 100.0, 6)
                    if row["packet_drop_rate"] is not None
                    else "",
                    "format_duration_sec": round(row["format_duration_sec"], 6)
                    if row["format_duration_sec"] is not None
                    else "",
                    "start_time_sec": round(row["start_time_sec"], 6) if row["start_time_sec"] is not None else "",
                    "format_effective_span_sec": round(row["format_effective_span_sec"], 6)
                    if row["format_effective_span_sec"] is not None
                    else "",
                    "format_expected_frames": row["format_expected_frames"] if row["format_expected_frames"] is not None else "",
                    "format_drop_frames": row["format_drop_frames"] if row["format_drop_frames"] is not None else "",
                    "format_drop_rate_pct": round((row["format_drop_rate"] or 0.0) * 100.0, 6)
                    if row["format_drop_rate"] is not None
                    else "",
                    "actual_fps": round(row["actual_fps"], 6) if row["actual_fps"] is not None else "",
                    "median_positive_gap_ms": round(row["median_positive_gap_ms"], 6)
                    if row["median_positive_gap_ms"] is not None
                    else "",
                    "duplicate_timestamp_count": row["duplicate_timestamp_count"] if row["duplicate_timestamp_count"] is not None else "",
                    "non_monotonic_count": row["non_monotonic_count"] if row["non_monotonic_count"] is not None else "",
                    "large_gap_event_count": row["large_gap_event_count"] if row["large_gap_event_count"] is not None else "",
                    "max_positive_gap_ms": round(row["max_positive_gap_ms"], 6)
                    if row["max_positive_gap_ms"] is not None
                    else "",
                    "meta_nb_read_packets": row["meta_nb_read_packets"],
                    "meta_nb_frames": row["meta_nb_frames"],
                    "meta_avg_frame_rate": row["meta_avg_frame_rate"],
                    "meta_r_frame_rate": row["meta_r_frame_rate"],
                    "row_status": row["row_status"],
                    "issue": row["issue"],
                    "video_path": row["video_path"],
                }
            )
    return rows


def rank_episode_stream_rows(rows):
    return sorted(
        rows,
        key=lambda row: (
            1 if row["row_status"] != "ok" else 0,
            float_or_default(row["packet_drop_rate_pct"]),
            float_or_default(row["format_drop_rate_pct"]),
            float_or_default(row["packet_drop_frames"]),
        ),
        reverse=True,
    )


def rank_episode_summary_rows(rows):
    return sorted(
        rows,
        key=lambda row: (
            float_or_default(row["drop_rate_pct"]),
            float_or_default(row["format_drop_rate_pct"]),
            float_or_default(row["invalid_row_count"]),
        ),
        reverse=True,
    )


def build_alert_episode_stream_rows(rows, stream_drop_threshold_pct):
    alerts = []
    for row in rows:
        if row["row_status"] != "ok":
            alerts.append(row)
            continue
        if stream_drop_threshold_pct >= 0 and float_or_default(row["packet_drop_rate_pct"], -1.0) > stream_drop_threshold_pct:
            alerts.append(row)
    return alerts


def build_alert_episode_rows(rows, stream_drop_threshold_pct):
    alerts = []
    for row in rows:
        if float_or_default(row["invalid_row_count"]) > 0:
            alerts.append(row)
            continue
        if stream_drop_threshold_pct >= 0 and float_or_default(row["worst_packet_drop_rate_pct"], -1.0) > stream_drop_threshold_pct:
            alerts.append(row)
    return alerts


def markdown_table(headers, rows):
    if not rows:
        return ["(none)"]
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join(["---"] * len(headers)) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(str(item) for item in row) + " |")
    return lines


def pct_cell(value):
    if value is None or value == "":
        return ""
    return f"{value}%"


def write_markdown_report(
    path,
    root_dir,
    report_dir,
    args,
    device_summary_rows,
    stream_summary_rows,
    ranked_episode_rows,
    ranked_episode_stream_rows,
    alert_episode_rows,
    alert_episode_stream_rows,
):
    lines = [
        "# V2 Episode Video Drop Report",
        "",
        "## Quick View",
        f"- root_dir: `{root_dir}`",
        f"- report_dir: `{report_dir}`",
        f"- max_device_drop_rate_pct: `{args.max_device_drop_rate}`",
        f"- max_stream_drop_rate_pct: `{args.max_stream_drop_rate}`",
        f"- device_count: `{len(device_summary_rows)}`",
        f"- alert_episode_count: `{len(alert_episode_rows)}`",
        f"- alert_episode_stream_count: `{len(alert_episode_stream_rows)}`",
        "",
        "## Open These First",
        "- `device_summary.csv`: one row per device",
        "- `stream_summary.csv`: one row per stream",
        "- `alert_episode_streams.csv`: only invalid or over-threshold episode-stream rows",
        "- `ranked_episode_streams.csv`: worst episode-stream rows sorted from high to low",
        "- `ranked_episodes.csv`: worst episodes sorted from high to low",
        "",
        "## Device Summary",
    ]

    device_table_rows = []
    for row in device_summary_rows:
        device_table_rows.append(
            [
                row["device_id"],
                row["status"],
                row["episode_count"],
                pct_cell(row["device_drop_rate_pct"]),
                pct_cell(row["format_drop_rate_pct"]),
                row["worst_episode"],
                row["worst_stream_name"],
            ]
        )
    lines.extend(
        markdown_table(
            [
                "device_id",
                "status",
                "episodes",
                "drop_rate_pct",
                "format_drop_rate_pct",
                "worst_episode",
                "worst_stream",
            ],
            device_table_rows,
        )
    )
    lines.append("")
    lines.append("## Stream Summary")

    stream_table_rows = []
    for row in stream_summary_rows:
        stream_table_rows.append(
            [
                row["device_id"],
                row["stream_name"],
                pct_cell(row["drop_rate_pct"]),
                pct_cell(row["format_drop_rate_pct"]),
                row["worst_episode"],
                pct_cell(row["worst_packet_drop_rate_pct"]),
            ]
        )
    lines.extend(
        markdown_table(
            [
                "device_id",
                "stream_name",
                "drop_rate_pct",
                "format_drop_rate_pct",
                "worst_episode",
                "worst_episode_rate_pct",
            ],
            stream_table_rows,
        )
    )
    lines.append("")
    lines.append("## Top 20 Worst Episode-Stream Rows")
    top_stream_rows = []
    for index, row in enumerate(ranked_episode_stream_rows[:20], 1):
        top_stream_rows.append(
            [
                index,
                row["device_id"],
                row["episode"],
                row["stream_name"],
                row["row_status"],
                pct_cell(row["packet_drop_rate_pct"]),
                row["packet_drop_frames"],
                pct_cell(row["format_drop_rate_pct"]),
            ]
        )
    lines.extend(
        markdown_table(
            [
                "rank",
                "device_id",
                "episode",
                "stream_name",
                "status",
                "drop_rate_pct",
                "drop_frames",
                "format_drop_rate_pct",
            ],
            top_stream_rows,
        )
    )
    lines.append("")
    lines.append("## Top 20 Worst Episodes")
    top_episode_rows = []
    for index, row in enumerate(ranked_episode_rows[:20], 1):
        top_episode_rows.append(
            [
                index,
                row["device_id"],
                row["episode"],
                pct_cell(row["drop_rate_pct"]),
                pct_cell(row["format_drop_rate_pct"]),
                row["worst_stream_name"],
                row["invalid_row_count"],
            ]
        )
    lines.extend(
        markdown_table(
            [
                "rank",
                "device_id",
                "episode",
                "drop_rate_pct",
                "format_drop_rate_pct",
                "worst_stream",
                "invalid_rows",
            ],
            top_episode_rows,
        )
    )
    lines.append("")
    lines.append("## Files")
    lines.append("- `device_summary.csv`")
    lines.append("- `stream_summary.csv`")
    lines.append("- `episode_summary.csv`")
    lines.append("- `episode_stream_stats.csv`")
    lines.append("- `ranked_episodes.csv`")
    lines.append("- `ranked_episode_streams.csv`")
    lines.append("- `alert_episodes.csv`")
    lines.append("- `alert_episode_streams.csv`")
    lines.append("- `summary.json`")
    lines.append("- `report.txt`")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_text_report(path, results, evaluations, args, root_dir):
    with path.open("w", encoding="utf-8") as f:
        f.write("V2 Episode Video Drop Report\n")
        f.write(f"root_dir: {root_dir}\n")
        f.write(f"max_device_drop_rate_pct: {args.max_device_drop_rate}\n")
        f.write(f"max_stream_drop_rate_pct: {args.max_stream_drop_rate}\n")
        f.write("open_first: 00_report.md, device_summary.csv, alert_episode_streams.csv\n")
        f.write("primary_method: packet-span expected frames = round(packet_span_sec * expected_fps)\n")
        f.write("reference_method: format-span expected frames = round((format_duration - start_time) * expected_fps)\n")
        f.write("scope: each episode under all 8 video streams\n")
        f.write("diagnostics: timestamp duplicate/non-monotonic/large-gap counters are for debugging only\n\n")

        for result in results:
            evaluation = evaluations[result["device_id"]]
            summary = result["summary"]
            f.write(
                f"[{evaluation['status']}] device_id={result['device_id']} "
                f"episodes={result['episode_count']} "
                f"device_drop_rate={safe_pct_text((summary['total_packet_drop_rate'] or 0.0) * 100.0 if summary['total_packet_drop_rate'] is not None else None)} "
                f"format_drop_rate={safe_pct_text((summary['total_format_drop_rate'] or 0.0) * 100.0 if summary['total_format_drop_rate'] is not None else None)} "
                f"invalid_rows={summary['invalid_row_count']}\n"
            )
            for stream_row in result["stream_summary"]:
                f.write(
                    f"  - {stream_row['stream_name']}: "
                    f"drop_rate={safe_pct_text((stream_row['total_packet_drop_rate'] or 0.0) * 100.0 if stream_row['total_packet_drop_rate'] is not None else None)} "
                    f"packet_drop_frames={stream_row['total_packet_drop_frames']} "
                    f"format_drop_rate={safe_pct_text((stream_row['total_format_drop_rate'] or 0.0) * 100.0 if stream_row['total_format_drop_rate'] is not None else None)} "
                    f"invalid={stream_row['invalid_row_count']}\n"
                )
            for reason in evaluation["fail_reasons"]:
                f.write(f"    * {reason}\n")
            f.write("\n")


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "只读统计 v2 数据每个 episode 下 8 路视频的丢帧率。"
            "脚本不会修改原始数据，只会在独立报告目录输出 CSV/JSON/TXT。"
        )
    )
    parser.add_argument(
        "root_dir",
        nargs="?",
        help="v2 批次目录或单设备目录；不传时自动找最新的 v2_usb_backups_*。",
    )
    parser.add_argument(
        "--device",
        action="append",
        default=[],
        help="只处理指定设备目录名或 device_id，可重复传入。",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=max(1, min(os.cpu_count() or 4, 8)),
        help="并发任务数，默认最多 8。",
    )
    parser.add_argument(
        "--report-dir",
        default=str(DEFAULT_REPORT_ROOT),
        help=f"报告输出根目录，默认 {DEFAULT_REPORT_ROOT}",
    )
    parser.add_argument(
        "--max-device-drop-rate",
        type=float,
        default=1.0,
        help="设备总丢帧率阈值，单位 %%；默认 1.0，设为负数则关闭。",
    )
    parser.add_argument(
        "--max-stream-drop-rate",
        type=float,
        default=2.0,
        help="单视频流聚合丢帧率阈值，单位 %%；默认 2.0，设为负数则关闭。",
    )
    return parser.parse_args()


def filter_device_roots(device_roots, device_filters):
    if not device_filters:
        return device_roots
    normalized = {str(item).strip().upper() for item in device_filters if str(item).strip()}
    filtered = []
    for device_root in device_roots:
        device_id = resolve_device_id(device_root).upper()
        device_name = device_root.name.upper()
        if device_id in normalized or device_name in normalized:
            filtered.append(device_root)
    return filtered


def main():
    args = parse_args()
    root_dir = resolve_root_dir(args.root_dir)
    if root_dir is None or not root_dir.exists():
        print("未找到可用的 v2 数据目录。")
        raise SystemExit(1)

    device_roots = filter_device_roots(discover_device_roots(root_dir), args.device)
    if not device_roots:
        print(f"未找到设备目录（需满足 data/episode_*）: {root_dir}")
        raise SystemExit(1)

    report_root = Path(args.report_dir).expanduser().resolve()
    report_dir = report_root / f"v2_episode_video_drop_report_{root_dir.name}"
    report_dir.mkdir(parents=True, exist_ok=True)

    print(f"统计目录: {root_dir}")
    print(f"设备数: {len(device_roots)}")
    print(f"报告目录: {report_dir}")

    results = []
    for index, device_root in enumerate(device_roots, 1):
        print(f"[{index}/{len(device_roots)}] 开始统计: {device_root}")
        results.append(analyze_device_root(device_root, jobs=args.jobs))

    evaluations = {
        result["device_id"]: evaluate_device_result(
            result,
            max_device_drop_rate_pct=args.max_device_drop_rate,
            max_stream_drop_rate_pct=args.max_stream_drop_rate,
        )
        for result in results
    }

    device_summary_rows = build_device_summary_rows(results, evaluations)
    stream_summary_rows = build_stream_summary_rows(results, evaluations)
    episode_summary_rows = build_episode_summary_rows(results, evaluations)
    episode_stream_rows = build_episode_stream_rows(results, evaluations)
    ranked_episode_rows = rank_episode_summary_rows(episode_summary_rows)
    ranked_episode_stream_rows = rank_episode_stream_rows(episode_stream_rows)
    alert_episode_rows = build_alert_episode_rows(
        ranked_episode_rows,
        args.max_stream_drop_rate,
    )
    alert_episode_stream_rows = build_alert_episode_stream_rows(
        ranked_episode_stream_rows,
        args.max_stream_drop_rate,
    )

    write_csv(
        report_dir / "device_summary.csv",
        [
            "device_name",
            "device_id",
            "device_root",
            "episode_count",
            "status",
            "fail_reasons",
            "valid_row_count",
            "invalid_row_count",
            "total_actual_frames",
            "total_packet_drop_frames",
            "total_packet_expected_frames",
            "device_drop_rate_pct",
            "total_format_drop_frames",
            "total_format_expected_frames",
            "format_drop_rate_pct",
            "worst_episode",
            "worst_episode_drop_rate_pct",
            "worst_stream_name",
            "worst_stream_drop_rate_pct",
        ],
        device_summary_rows,
    )
    write_csv(
        report_dir / "stream_summary.csv",
        [
            "device_name",
            "device_id",
            "status",
            "stream_name",
            "row_count",
            "valid_row_count",
            "invalid_row_count",
            "total_actual_frames",
            "total_packet_drop_frames",
            "total_packet_expected_frames",
            "drop_rate_pct",
            "total_format_drop_frames",
            "total_format_expected_frames",
            "format_drop_rate_pct",
            "worst_episode",
            "worst_packet_drop_rate_pct",
        ],
        stream_summary_rows,
    )
    write_csv(
        report_dir / "episode_summary.csv",
        [
            "device_name",
            "device_id",
            "status",
            "episode",
            "row_count",
            "valid_row_count",
            "invalid_row_count",
            "total_actual_frames",
            "total_packet_drop_frames",
            "total_packet_expected_frames",
            "drop_rate_pct",
            "total_format_drop_frames",
            "total_format_expected_frames",
            "format_drop_rate_pct",
            "worst_stream_name",
            "worst_packet_drop_rate_pct",
        ],
        episode_summary_rows,
    )
    write_csv(
        report_dir / "episode_stream_stats.csv",
        [
            "device_name",
            "device_id",
            "status",
            "episode",
            "stream_name",
            "expected_fps",
            "actual_frames",
            "first_ts_sec",
            "last_end_sec",
            "packet_span_sec",
            "packet_expected_frames",
            "packet_drop_frames",
            "packet_drop_rate_pct",
            "format_duration_sec",
            "start_time_sec",
            "format_effective_span_sec",
            "format_expected_frames",
            "format_drop_frames",
            "format_drop_rate_pct",
            "actual_fps",
            "median_positive_gap_ms",
            "duplicate_timestamp_count",
            "non_monotonic_count",
            "large_gap_event_count",
            "max_positive_gap_ms",
            "meta_nb_read_packets",
            "meta_nb_frames",
            "meta_avg_frame_rate",
            "meta_r_frame_rate",
            "row_status",
            "issue",
            "video_path",
        ],
        episode_stream_rows,
    )
    write_csv(
        report_dir / "ranked_episodes.csv",
        [
            "device_name",
            "device_id",
            "status",
            "episode",
            "row_count",
            "valid_row_count",
            "invalid_row_count",
            "total_actual_frames",
            "total_packet_drop_frames",
            "total_packet_expected_frames",
            "drop_rate_pct",
            "total_format_drop_frames",
            "total_format_expected_frames",
            "format_drop_rate_pct",
            "worst_stream_name",
            "worst_packet_drop_rate_pct",
        ],
        ranked_episode_rows,
    )
    write_csv(
        report_dir / "ranked_episode_streams.csv",
        [
            "device_name",
            "device_id",
            "status",
            "episode",
            "stream_name",
            "expected_fps",
            "actual_frames",
            "first_ts_sec",
            "last_end_sec",
            "packet_span_sec",
            "packet_expected_frames",
            "packet_drop_frames",
            "packet_drop_rate_pct",
            "format_duration_sec",
            "start_time_sec",
            "format_effective_span_sec",
            "format_expected_frames",
            "format_drop_frames",
            "format_drop_rate_pct",
            "actual_fps",
            "median_positive_gap_ms",
            "duplicate_timestamp_count",
            "non_monotonic_count",
            "large_gap_event_count",
            "max_positive_gap_ms",
            "meta_nb_read_packets",
            "meta_nb_frames",
            "meta_avg_frame_rate",
            "meta_r_frame_rate",
            "row_status",
            "issue",
            "video_path",
        ],
        ranked_episode_stream_rows,
    )
    write_csv(
        report_dir / "alert_episodes.csv",
        [
            "device_name",
            "device_id",
            "status",
            "episode",
            "row_count",
            "valid_row_count",
            "invalid_row_count",
            "total_actual_frames",
            "total_packet_drop_frames",
            "total_packet_expected_frames",
            "drop_rate_pct",
            "total_format_drop_frames",
            "total_format_expected_frames",
            "format_drop_rate_pct",
            "worst_stream_name",
            "worst_packet_drop_rate_pct",
        ],
        alert_episode_rows,
    )
    write_csv(
        report_dir / "alert_episode_streams.csv",
        [
            "device_name",
            "device_id",
            "status",
            "episode",
            "stream_name",
            "expected_fps",
            "actual_frames",
            "first_ts_sec",
            "last_end_sec",
            "packet_span_sec",
            "packet_expected_frames",
            "packet_drop_frames",
            "packet_drop_rate_pct",
            "format_duration_sec",
            "start_time_sec",
            "format_effective_span_sec",
            "format_expected_frames",
            "format_drop_frames",
            "format_drop_rate_pct",
            "actual_fps",
            "median_positive_gap_ms",
            "duplicate_timestamp_count",
            "non_monotonic_count",
            "large_gap_event_count",
            "max_positive_gap_ms",
            "meta_nb_read_packets",
            "meta_nb_frames",
            "meta_avg_frame_rate",
            "meta_r_frame_rate",
            "row_status",
            "issue",
            "video_path",
        ],
        alert_episode_stream_rows,
    )

    summary_payload = {
        "root_dir": str(root_dir),
        "report_dir": str(report_dir),
        "max_device_drop_rate_pct": args.max_device_drop_rate,
        "max_stream_drop_rate_pct": args.max_stream_drop_rate,
        "device_count": len(device_summary_rows),
        "alert_episode_count": len(alert_episode_rows),
        "alert_episode_stream_count": len(alert_episode_stream_rows),
        "results": device_summary_rows,
    }
    (report_dir / "summary.json").write_text(
        json.dumps(summary_payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    write_markdown_report(
        report_dir / "00_report.md",
        root_dir,
        report_dir,
        args,
        device_summary_rows,
        stream_summary_rows,
        ranked_episode_rows,
        ranked_episode_stream_rows,
        alert_episode_rows,
        alert_episode_stream_rows,
    )
    write_text_report(report_dir / "report.txt", results, evaluations, args, root_dir)

    pass_count = sum(1 for item in device_summary_rows if item["status"] == "PASS")
    print(f"汇总: PASS {pass_count} / {len(device_summary_rows)}")
    print(f"报告已输出到: {report_dir}")
    print(f"先看: {report_dir / '00_report.md'}")
    raise SystemExit(0 if pass_count == len(device_summary_rows) else 2)


if __name__ == "__main__":
    main()
