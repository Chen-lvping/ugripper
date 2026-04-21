#!/usr/bin/env python3
import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

from work_duration_stats_v2 import detect_latest_usb_root


VIDEO_SUFFIXES = {".mkv", ".mp4", ".mov", ".avi"}
SCAN_SKIP_DIRS = {"logs", "runtime", "__pycache__"}
DEFAULT_WORKERS = max(1, min(6, os.cpu_count() or 1))
DEFAULT_FRAME_SMALL_DELTA_FACTOR = 0.35
DEFAULT_FRAME_LARGE_GAP_FACTOR = 1.8
DEFAULT_FRAME_START_GAP_FACTOR = 1.8
SCRIPT_DIR = Path(__file__).resolve().parent
TOOLS_ROOT = Path(os.environ.get("USB_TOOLS_DIR", str(SCRIPT_DIR.parent))).expanduser().resolve()
RUNTIME_ROOT = Path(os.environ.get("USB_RUNTIME_DIR", str(TOOLS_ROOT / "runtime"))).expanduser().resolve()
OUTPUT_ROOT_BASE = RUNTIME_ROOT / "fix_video_pts_v2"
KNOWN_FPS_HINTS = {
    "left_cam_main.mkv": 60.0,
    "right_cam_main.mkv": 60.0,
    "left_stereo.mkv": 60.0,
    "right_stereo.mkv": 60.0,
    "left_tcam_l.mkv": 120.0,
    "left_tcam_r.mkv": 120.0,
    "right_tcam_l.mkv": 120.0,
    "right_tcam_r.mkv": 120.0,
}
REPAIR_REASONS = {
    "negative_start_pts",
    "non_monotonic_frame_pts",
    "zero_delta_frame_pts",
    "tiny_delta_frame_pts",
    "large_gap_frame_pts",
    "start_offset_frame_pts",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "扫描 v2 数据集视频的 PTS / frame 时间轴异常，优先依据 frame 级时间戳判定，"
            "并在临时目录中生成修复结果，不修改原始数据。"
        )
    )
    parser.add_argument(
        "root_dir",
        nargs="?",
        help=(
            "待扫描目录；可传批次目录 / 设备目录 / episode 目录 / 单个视频。"
            "不传时自动选择最新 v2_usb_backups 输出目录。"
        ),
    )
    parser.add_argument(
        "--repair",
        action="store_true",
        help="对检测到的时间戳异常视频执行修复；默认只扫描并生成报告。",
    )
    parser.add_argument(
        "--in-place",
        action="store_true",
        help="直接覆盖输入路径中的视频；默认关闭，建议保持关闭以保护原始数据。",
    )
    parser.add_argument(
        "--keep-backup",
        action="store_true",
        help="配合 --in-place 使用；替换前把原文件保留为 *.ptsbak。",
    )
    parser.add_argument(
        "--output-root",
        help="修复输出目录；默认放到 runtime/fix_video_pts_v2 下的临时目录。",
    )
    parser.add_argument(
        "--runtime-root",
        help="扫描报告和默认修复输出的根目录；默认使用仓库 runtime/。",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="允许覆盖已存在的输出目录或输出文件。",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="仅处理前 N 个视频（按路径排序）；0 表示不限制。",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=DEFAULT_WORKERS,
        help=f"扫描并行度，默认 {DEFAULT_WORKERS}。",
    )
    parser.add_argument(
        "--frame-small-delta-factor",
        type=float,
        default=DEFAULT_FRAME_SMALL_DELTA_FACTOR,
        help="当相邻 frame 时间差 < 主导帧间隔 * 该系数时，判为异常小间隔。",
    )
    parser.add_argument(
        "--frame-large-gap-factor",
        type=float,
        default=DEFAULT_FRAME_LARGE_GAP_FACTOR,
        help="当相邻 frame 时间差 > 主导帧间隔 * 该系数时，判为大 gap / 疑似丢帧。",
    )
    parser.add_argument(
        "--frame-start-gap-factor",
        type=float,
        default=DEFAULT_FRAME_START_GAP_FACTOR,
        help="当首帧 frame 时间戳 > 主导帧间隔 * 该系数时，判为首帧偏移。",
    )
    parser.add_argument(
        "--report-json",
        help="扫描报告 JSON 输出路径；默认写到 runtime/fix_video_pts_v2/。",
    )
    parser.add_argument(
        "--report-csv",
        help="扫描报告 CSV 输出路径；默认写到 runtime/fix_video_pts_v2/。",
    )
    parser.add_argument(
        "--ffprobe-timeout",
        type=int,
        default=120,
        help="单个视频 ffprobe 超时秒数，默认 120。",
    )
    parser.add_argument(
        "--verify-repaired",
        action="store_true",
        help="修复后重新扫描输出文件，确认时间轴异常已消除。",
    )
    return parser.parse_args()


def _safe_float(value):
    if value in (None, ""):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _parse_fraction(text):
    if not text:
        return None
    try:
        numerator, denominator = str(text).split("/", 1)
        numerator = float(numerator)
        denominator = float(denominator)
        if denominator == 0:
            return None
        return numerator / denominator
    except (TypeError, ValueError, ZeroDivisionError):
        return _safe_float(text)


def _format_float(value, digits=6):
    if value is None:
        return ""
    return f"{value:.{digits}f}"


def _format_number(value, digits=6):
    if value is None:
        return ""
    text = f"{value:.{digits}f}".rstrip("0").rstrip(".")
    return text or "0"


def _candidate_key(path):
    try:
        return str(path.resolve())
    except Exception:
        return str(path)


def _safe_source_tag(path):
    try:
        resolved = path.resolve()
    except Exception:
        resolved = path
    parts = [part for part in resolved.parts if part not in {os.sep, ""}]
    tail = "_".join(parts[-3:]) if len(parts) >= 3 else "_".join(parts)
    safe = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "_" for ch in tail)
    return safe.strip("_") or "input"


def resolve_root_dir(root_dir_arg):
    if root_dir_arg:
        return Path(root_dir_arg).expanduser().resolve()

    latest = detect_latest_usb_root()
    if latest is None:
        raise FileNotFoundError("未检测到可处理的 v2 数据目录，请手动传入 root_dir。")
    return latest.resolve()


def resolve_runtime_root(args):
    runtime_root = (
        Path(args.runtime_root).expanduser().resolve()
        if args.runtime_root
        else OUTPUT_ROOT_BASE
    )
    runtime_root.mkdir(parents=True, exist_ok=True)
    return runtime_root


def _iter_all_files(root_dir):
    if root_dir.is_file():
        yield root_dir
        return

    for current_root, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = sorted([name for name in dirnames if name not in SCAN_SKIP_DIRS])
        current_path = Path(current_root)
        for filename in sorted(filenames):
            yield current_path / filename


def list_video_files(root_dir):
    videos = []
    for path in _iter_all_files(root_dir):
        if path.suffix.lower() in VIDEO_SUFFIXES:
            videos.append(path)
    videos.sort(key=lambda path: str(path))
    return videos


def _run_ffprobe_json(cmd, timeout):
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )
    if not proc.stdout.strip():
        raise RuntimeError(proc.stderr.strip() or "ffprobe returned empty stdout")
    try:
        payload = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"ffprobe JSON parse failed: {exc}") from exc
    return payload, proc.stderr.strip(), proc.returncode


def probe_stream(video_path, timeout):
    payload, stderr_text, return_code = _run_ffprobe_json(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,avg_frame_rate,r_frame_rate,time_base,start_time,duration",
            "-of",
            "json",
            str(video_path),
        ],
        timeout=timeout,
    )
    stream = (payload.get("streams") or [{}])[0]
    return stream, stderr_text, return_code


def probe_frames(video_path, timeout):
    payload, stderr_text, return_code = _run_ffprobe_json(
        [
            "ffprobe",
            "-v",
            "warning",
            "-select_streams",
            "v:0",
            "-show_entries",
            "frame=best_effort_timestamp_time,pkt_duration_time",
            "-of",
            "json",
            str(video_path),
        ],
        timeout=timeout,
    )
    frames = payload.get("frames") or []
    return frames, stderr_text, return_code


def infer_header_fps(video_path, stream):
    avg_fps = _parse_fraction(stream.get("avg_frame_rate"))
    r_fps = _parse_fraction(stream.get("r_frame_rate"))
    hint_fps = KNOWN_FPS_HINTS.get(video_path.name)
    for candidate in (avg_fps, r_fps, hint_fps):
        if candidate is not None and candidate > 0:
            return float(candidate)
    return None


def _median(values):
    if not values:
        return None
    values = sorted(values)
    middle = len(values) // 2
    if len(values) % 2 == 1:
        return values[middle]
    return (values[middle - 1] + values[middle]) / 2.0


def _estimate_effective_fps(dominant_delta):
    if dominant_delta is None or dominant_delta <= 0:
        return None
    fps = 1.0 / dominant_delta
    rounded = round(fps)
    if rounded > 0 and abs(fps - rounded) <= max(0.15, rounded * 0.01):
        return float(rounded)
    return fps


def _collect_frame_stats(frame_entries, time_base_sec):
    timestamps = []
    pkt_durations = []
    for frame in frame_entries:
        ts = _safe_float(frame.get("best_effort_timestamp_time"))
        if ts is not None:
            timestamps.append(ts)
        duration = _safe_float(frame.get("pkt_duration_time"))
        if duration is not None and duration > 0:
            pkt_durations.append(duration)

    frame_count = len(timestamps)
    first_ts = timestamps[0] if timestamps else None
    last_ts = timestamps[-1] if timestamps else None
    span_sec = None
    if first_ts is not None and last_ts is not None:
        span_sec = max(0.0, last_ts - first_ts)

    deltas = [right - left for left, right in zip(timestamps, timestamps[1:])]
    positive_deltas = [delta for delta in deltas if delta > 0]
    pkt_duration_median = _median(pkt_durations)
    dominant_delta = pkt_duration_median or _median(positive_deltas)
    epsilon_candidates = [1e-6]
    if time_base_sec:
        epsilon_candidates.append(time_base_sec / 2.0)
    if dominant_delta:
        epsilon_candidates.append(dominant_delta * 0.08)
    epsilon = max(epsilon_candidates)

    return {
        "frame_count": frame_count,
        "first_frame_ts": first_ts,
        "last_frame_ts": last_ts,
        "frame_span_sec": span_sec,
        "frame_deltas": deltas,
        "dominant_delta": dominant_delta,
        "epsilon": epsilon,
        "pkt_duration_median": pkt_duration_median,
        "frame_delta_min": min(deltas) if deltas else None,
        "frame_delta_median": _median(deltas) if deltas else None,
        "frame_delta_max": max(deltas) if deltas else None,
    }


def _build_issue_tags(metrics):
    issue_tags = []
    if metrics["stream_probe_error"]:
        issue_tags.append("stream_probe_failed")
    if metrics["frame_probe_error"]:
        issue_tags.append("frame_probe_failed")
        return issue_tags
    if metrics["frame_probe_warning"]:
        issue_tags.append("frame_probe_warning")
    if metrics["frame_count"] <= 1:
        issue_tags.append("insufficient_frames")
        return issue_tags
    if metrics["effective_fps"] is None:
        issue_tags.append("unknown_effective_fps")
    if metrics["header_fps"] is not None and metrics["effective_fps"] is not None:
        ratio = max(metrics["header_fps"], metrics["effective_fps"]) / max(
            min(metrics["header_fps"], metrics["effective_fps"]),
            1e-6,
        )
        if ratio >= 1.25:
            issue_tags.append("header_fps_mismatch")
    if metrics["first_frame_ts"] is not None and metrics["first_frame_ts"] < -metrics["epsilon"]:
        issue_tags.append("negative_start_pts")
    if metrics["reverse_delta_count"] > 0:
        issue_tags.append("non_monotonic_frame_pts")
    if metrics["zero_delta_count"] > 0:
        issue_tags.append("zero_delta_frame_pts")
    if metrics["small_delta_count"] > 0:
        issue_tags.append("tiny_delta_frame_pts")
    if metrics["large_gap_count"] > 0:
        issue_tags.append("large_gap_frame_pts")
    if metrics["start_gap_frames"] > 0:
        issue_tags.append("start_offset_frame_pts")
    return issue_tags


def analyze_video(video_path, root_dir, args):
    relative_path = (
        str(video_path.relative_to(root_dir))
        if root_dir.is_dir()
        else video_path.name
    )
    metrics = {
        "src_path": str(video_path),
        "relative_path": relative_path,
        "codec_name": "",
        "header_fps": None,
        "effective_fps": None,
        "time_base_sec": None,
        "frame_count": 0,
        "first_frame_ts": None,
        "last_frame_ts": None,
        "frame_span_sec": None,
        "dominant_frame_delta": None,
        "pkt_duration_median": None,
        "frame_delta_min": None,
        "frame_delta_median": None,
        "frame_delta_max": None,
        "reverse_delta_count": 0,
        "zero_delta_count": 0,
        "small_delta_count": 0,
        "large_gap_count": 0,
        "estimated_missing_frames": 0,
        "start_gap_frames": 0,
        "issue_tags": [],
        "repair_needed": False,
        "repairable": False,
        "stream_probe_error": "",
        "frame_probe_error": "",
        "frame_probe_warning": "",
        "repair_status": "not_requested",
        "repair_output_path": "",
        "analysis_basis": "frame",
        "epsilon": None,
    }

    timeout = max(5, args.ffprobe_timeout)
    try:
        stream, stream_stderr, stream_rc = probe_stream(video_path, timeout=timeout)
        if stream_rc != 0 and not stream:
            raise RuntimeError(stream_stderr or "ffprobe stream probe failed")
    except Exception as exc:
        metrics["stream_probe_error"] = str(exc)
        metrics["issue_tags"] = _build_issue_tags(metrics)
        metrics["repair_needed"] = True
        return metrics

    metrics["codec_name"] = str(stream.get("codec_name") or "").strip()
    metrics["time_base_sec"] = _parse_fraction(stream.get("time_base"))
    metrics["header_fps"] = infer_header_fps(video_path, stream)

    try:
        frame_entries, frame_stderr, _frame_rc = probe_frames(video_path, timeout=timeout)
        metrics["frame_probe_warning"] = frame_stderr
    except Exception as exc:
        metrics["frame_probe_error"] = str(exc)
        metrics["issue_tags"] = _build_issue_tags(metrics)
        metrics["repair_needed"] = True
        return metrics

    frame_stats = _collect_frame_stats(frame_entries, metrics["time_base_sec"])
    metrics.update(
        {
            "frame_count": frame_stats["frame_count"],
            "first_frame_ts": frame_stats["first_frame_ts"],
            "last_frame_ts": frame_stats["last_frame_ts"],
            "frame_span_sec": frame_stats["frame_span_sec"],
            "dominant_frame_delta": frame_stats["dominant_delta"],
            "pkt_duration_median": frame_stats["pkt_duration_median"],
            "frame_delta_min": frame_stats["frame_delta_min"],
            "frame_delta_median": frame_stats["frame_delta_median"],
            "frame_delta_max": frame_stats["frame_delta_max"],
            "epsilon": frame_stats["epsilon"],
        }
    )
    metrics["effective_fps"] = _estimate_effective_fps(metrics["dominant_frame_delta"])

    dominant_delta = metrics["dominant_frame_delta"]
    epsilon = metrics["epsilon"] or 1e-6
    if dominant_delta and dominant_delta > 0:
        small_threshold = dominant_delta * args.frame_small_delta_factor
        large_threshold = dominant_delta * args.frame_large_gap_factor
        start_threshold = dominant_delta * args.frame_start_gap_factor
    else:
        small_threshold = None
        large_threshold = None
        start_threshold = None

    for delta in frame_stats["frame_deltas"]:
        if delta < -epsilon:
            metrics["reverse_delta_count"] += 1
        if abs(delta) <= epsilon:
            metrics["zero_delta_count"] += 1
            continue
        if small_threshold is not None and delta > 0 and delta < small_threshold:
            metrics["small_delta_count"] += 1
        if large_threshold is not None and delta > large_threshold:
            metrics["large_gap_count"] += 1
            metrics["estimated_missing_frames"] += max(
                int(round(delta / dominant_delta)) - 1,
                1,
            )

    if (
        start_threshold is not None
        and metrics["first_frame_ts"] is not None
        and metrics["first_frame_ts"] > start_threshold
    ):
        metrics["start_gap_frames"] = max(
            int(round(metrics["first_frame_ts"] / dominant_delta)),
            1,
        )

    metrics["issue_tags"] = _build_issue_tags(metrics)
    metrics["repair_needed"] = any(tag in REPAIR_REASONS for tag in metrics["issue_tags"])
    metrics["repairable"] = metrics["repair_needed"] and metrics["effective_fps"] is not None
    return metrics


def summarize_reports(reports):
    summary = {
        "video_count": len(reports),
        "repair_needed_count": sum(1 for item in reports if item["repair_needed"]),
        "repairable_count": sum(1 for item in reports if item["repairable"]),
        "frame_probe_failed_count": sum(1 for item in reports if item["frame_probe_error"]),
        "frame_probe_warning_count": sum(1 for item in reports if item["frame_probe_warning"]),
        "by_name": {},
    }
    grouped = {}
    for item in reports:
        bucket = grouped.setdefault(
            Path(item["relative_path"]).name,
            {
                "total": 0,
                "repair_needed": 0,
                "repairable": 0,
                "frame_probe_warning": 0,
                "large_gap_count": 0,
                "estimated_missing_frames": 0,
                "zero_delta_count": 0,
                "small_delta_count": 0,
                "header_fps_mismatch": 0,
            },
        )
        bucket["total"] += 1
        bucket["repair_needed"] += int(item["repair_needed"])
        bucket["repairable"] += int(item["repairable"])
        bucket["frame_probe_warning"] += int(bool(item["frame_probe_warning"]))
        bucket["large_gap_count"] += item["large_gap_count"]
        bucket["estimated_missing_frames"] += item["estimated_missing_frames"]
        bucket["zero_delta_count"] += item["zero_delta_count"]
        bucket["small_delta_count"] += item["small_delta_count"]
        bucket["header_fps_mismatch"] += int("header_fps_mismatch" in item["issue_tags"])
    summary["by_name"] = dict(sorted(grouped.items()))
    return summary


def print_summary(summary):
    print("=" * 72)
    print("PTS / Frame 时间轴扫描汇总")
    print("=" * 72)
    print(f"总视频数: {summary['video_count']}")
    print(f"需要修复: {summary['repair_needed_count']}")
    print(f"可直接修复: {summary['repairable_count']}")
    print(f"frame probe 失败: {summary['frame_probe_failed_count']}")
    print(f"frame probe 警告: {summary['frame_probe_warning_count']}")
    print("-" * 72)
    for name, item in summary["by_name"].items():
        print(
            f"{name}: {item['repair_needed']}/{item['total']} 可疑 | "
            f"大gap={item['large_gap_count']} | "
            f"疑似丢帧={item['estimated_missing_frames']} | "
            f"零间隔={item['zero_delta_count']} | "
            f"异常小间隔={item['small_delta_count']} | "
            f"header_fps不匹配={item['header_fps_mismatch']} | "
            f"frame警告={item['frame_probe_warning']}"
        )


def _build_default_run_dir(root_dir, runtime_root):
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = runtime_root / f"{_safe_source_tag(root_dir)}_{timestamp}"
    run_dir.mkdir(parents=True, exist_ok=True)
    return run_dir


def build_report_paths(root_dir, runtime_root, args):
    if args.report_json:
        report_json = Path(args.report_json).expanduser().resolve()
        report_json.parent.mkdir(parents=True, exist_ok=True)
    else:
        report_json = _build_default_run_dir(root_dir, runtime_root) / "pts_scan_report.json"

    if args.report_csv:
        report_csv = Path(args.report_csv).expanduser().resolve()
        report_csv.parent.mkdir(parents=True, exist_ok=True)
    else:
        base_dir = report_json.parent
        report_csv = base_dir / "pts_scan_report.csv"

    return report_json, report_csv


def write_reports(report_json_path, report_csv_path, root_dir, args, reports, summary):
    payload = {
        "root_dir": str(root_dir),
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "repair_requested": bool(args.repair),
        "summary": summary,
        "videos": reports,
    }
    with report_json_path.open("w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    fieldnames = [
        "relative_path",
        "codec_name",
        "analysis_basis",
        "header_fps",
        "effective_fps",
        "time_base_sec",
        "frame_count",
        "first_frame_ts",
        "last_frame_ts",
        "frame_span_sec",
        "dominant_frame_delta",
        "pkt_duration_median",
        "frame_delta_min",
        "frame_delta_median",
        "frame_delta_max",
        "reverse_delta_count",
        "zero_delta_count",
        "small_delta_count",
        "large_gap_count",
        "estimated_missing_frames",
        "start_gap_frames",
        "repair_needed",
        "repairable",
        "issue_tags",
        "repair_status",
        "repair_output_path",
        "stream_probe_error",
        "frame_probe_error",
        "frame_probe_warning",
    ]
    with report_csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for item in reports:
            writer.writerow(
                {
                    "relative_path": item["relative_path"],
                    "codec_name": item["codec_name"],
                    "analysis_basis": item["analysis_basis"],
                    "header_fps": _format_float(item["header_fps"], digits=3),
                    "effective_fps": _format_float(item["effective_fps"], digits=3),
                    "time_base_sec": _format_float(item["time_base_sec"], digits=9),
                    "frame_count": item["frame_count"],
                    "first_frame_ts": _format_float(item["first_frame_ts"]),
                    "last_frame_ts": _format_float(item["last_frame_ts"]),
                    "frame_span_sec": _format_float(item["frame_span_sec"]),
                    "dominant_frame_delta": _format_float(item["dominant_frame_delta"]),
                    "pkt_duration_median": _format_float(item["pkt_duration_median"]),
                    "frame_delta_min": _format_float(item["frame_delta_min"]),
                    "frame_delta_median": _format_float(item["frame_delta_median"]),
                    "frame_delta_max": _format_float(item["frame_delta_max"]),
                    "reverse_delta_count": item["reverse_delta_count"],
                    "zero_delta_count": item["zero_delta_count"],
                    "small_delta_count": item["small_delta_count"],
                    "large_gap_count": item["large_gap_count"],
                    "estimated_missing_frames": item["estimated_missing_frames"],
                    "start_gap_frames": item["start_gap_frames"],
                    "repair_needed": int(item["repair_needed"]),
                    "repairable": int(item["repairable"]),
                    "issue_tags": ";".join(item["issue_tags"]),
                    "repair_status": item["repair_status"],
                    "repair_output_path": item["repair_output_path"],
                    "stream_probe_error": item["stream_probe_error"],
                    "frame_probe_error": item["frame_probe_error"],
                    "frame_probe_warning": item["frame_probe_warning"],
                }
            )


def _is_relative_to(path, parent):
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except Exception:
        return False


def resolve_output_root(root_dir, runtime_root, args):
    if not args.repair or args.in_place:
        return None

    if args.output_root:
        output_root = Path(args.output_root).expanduser().resolve()
    else:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_root = runtime_root / f"{_safe_source_tag(root_dir)}_pts_fixed_{timestamp}"

    if output_root.exists() and any(output_root.iterdir()) and not args.overwrite:
        raise FileExistsError(
            f"输出目录已存在且非空：{output_root}；如需覆盖请追加 --overwrite。"
        )
    if root_dir.is_dir() and _is_relative_to(output_root, root_dir):
        raise ValueError("输出目录不能位于输入目录内部。")

    output_root.mkdir(parents=True, exist_ok=True)
    return output_root


def _remove_path_if_exists(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def _copy_file(src_path, dst_path, overwrite=False):
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    if dst_path.exists():
        if not overwrite:
            return "skipped_existing"
        _remove_path_if_exists(dst_path)
    shutil.copy2(src_path, dst_path)
    return "copied"


def _run_ffmpeg_repair(src_path, dst_path, effective_fps):
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    expr = f"N/({_format_number(effective_fps)}*TB)"
    proc = subprocess.run(
        [
            "ffmpeg",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src_path),
            "-map",
            "0",
            "-c",
            "copy",
            "-bsf:v",
            f"setts=pts={expr}:dts={expr}",
            str(dst_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "ffmpeg repair failed")
    try:
        shutil.copystat(src_path, dst_path, follow_symlinks=True)
    except OSError:
        pass


def repair_video_to_output(src_path, dst_path, effective_fps, overwrite=False):
    if dst_path.exists():
        if not overwrite:
            return "skipped_existing"
        _remove_path_if_exists(dst_path)
    tmp_path = dst_path.with_name(f"{dst_path.stem}.ptsfix.tmp{dst_path.suffix}")
    if tmp_path.exists():
        _remove_path_if_exists(tmp_path)
    try:
        _run_ffmpeg_repair(src_path, tmp_path, effective_fps)
        tmp_path.replace(dst_path)
    finally:
        if tmp_path.exists():
            _remove_path_if_exists(tmp_path)
    return "repaired"


def repair_video_in_place(src_path, effective_fps, keep_backup=False):
    tmp_path = src_path.with_name(f"{src_path.stem}.ptsfix.tmp{src_path.suffix}")
    backup_path = src_path.with_name(f"{src_path.name}.ptsbak")
    if tmp_path.exists():
        _remove_path_if_exists(tmp_path)
    try:
        _run_ffmpeg_repair(src_path, tmp_path, effective_fps)
        if keep_backup:
            if backup_path.exists():
                raise FileExistsError(f"备份文件已存在：{backup_path}")
            src_path.replace(backup_path)
            tmp_path.replace(src_path)
        else:
            tmp_path.replace(src_path)
    finally:
        if tmp_path.exists():
            _remove_path_if_exists(tmp_path)
    return str(src_path)


def scan_videos(video_paths, root_dir, args):
    if not video_paths:
        return []

    reports = []
    worker_count = max(1, int(args.workers or 1))
    total = len(video_paths)
    print(f"开始扫描 {total} 个视频，workers={worker_count}")
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        future_map = {
            executor.submit(analyze_video, video_path, root_dir, args): video_path
            for video_path in video_paths
        }
        for index, future in enumerate(as_completed(future_map), 1):
            reports.append(future.result())
            if index % 20 == 0 or index == total:
                print(f"扫描进度: {index}/{total}")

    reports.sort(key=lambda item: item["relative_path"])
    return reports


def perform_repairs(root_dir, output_root, reports, args):
    if not args.repair:
        return

    report_map = {item["src_path"]: item for item in reports}
    if args.in_place:
        targets = [item for item in reports if item["repairable"]]
        print(f"开始原地修复 {len(targets)} 个视频")
        for index, item in enumerate(targets, 1):
            src_path = Path(item["src_path"])
            try:
                repaired_path = repair_video_in_place(
                    src_path,
                    item["effective_fps"],
                    keep_backup=args.keep_backup,
                )
                item["repair_status"] = "repaired"
                item["repair_output_path"] = repaired_path
                if args.verify_repaired:
                    verified = analyze_video(src_path, root_dir, args)
                    if verified["repair_needed"]:
                        raise RuntimeError(
                            f"修复后仍存在时间轴异常: {','.join(verified['issue_tags']) or 'unknown'}"
                        )
            except Exception as exc:
                item["repair_status"] = f"repair_failed: {exc}"
            if index % 10 == 0 or index == len(targets):
                print(f"修复进度: {index}/{len(targets)}")
        return

    print(f"开始输出修复目录：{output_root}")
    all_files = list(_iter_all_files(root_dir))
    total = len(all_files)
    processed = 0
    for src_path in all_files:
        relative_path = src_path.name if root_dir.is_file() else src_path.relative_to(root_dir)
        dst_path = output_root / relative_path
        item = report_map.get(str(src_path))
        if item is None:
            status = _copy_file(src_path, dst_path, overwrite=args.overwrite)
        elif item["repairable"]:
            try:
                status = repair_video_to_output(
                    src_path,
                    dst_path,
                    item["effective_fps"],
                    overwrite=args.overwrite,
                )
                item["repair_output_path"] = str(dst_path)
                if args.verify_repaired:
                    verified = analyze_video(dst_path, output_root, args)
                    if verified["repair_needed"]:
                        raise RuntimeError(
                            f"修复后仍存在时间轴异常: {','.join(verified['issue_tags']) or 'unknown'}"
                        )
            except Exception as exc:
                item["repair_status"] = f"repair_failed: {exc}"
                status = _copy_file(src_path, dst_path, overwrite=args.overwrite)
        else:
            status = _copy_file(src_path, dst_path, overwrite=args.overwrite)

        if item is not None and item["repair_status"] == "not_requested":
            if item["repairable"]:
                item["repair_status"] = status
            elif item["repair_needed"]:
                item["repair_status"] = "detected_but_unrepairable"
            else:
                item["repair_status"] = status

        processed += 1
        if processed % 50 == 0 or processed == total:
            print(f"输出进度: {processed}/{total}")


def main():
    args = parse_args()
    try:
        root_dir = resolve_root_dir(args.root_dir)
    except Exception as exc:
        print(f"❌ {exc}")
        return 2

    if not root_dir.exists():
        print(f"❌ 输入路径不存在：{root_dir}")
        return 2

    if args.in_place and not args.repair:
        print("❌ --in-place 只能和 --repair 一起使用。")
        return 2
    if args.in_place:
        print("⚠️ 当前使用 --in-place，会直接修改输入路径中的文件。")

    runtime_root = resolve_runtime_root(args)
    try:
        output_root = resolve_output_root(root_dir, runtime_root, args)
        report_json_path, report_csv_path = build_report_paths(root_dir, runtime_root, args)
    except Exception as exc:
        print(f"❌ {exc}")
        return 2

    video_paths = list_video_files(root_dir)
    if args.limit and args.limit > 0:
        video_paths = video_paths[: args.limit]
    if not video_paths:
        print(f"❌ 未找到可处理视频：{root_dir}")
        return 2

    reports = scan_videos(video_paths, root_dir, args)
    summary = summarize_reports(reports)
    print_summary(summary)

    try:
        perform_repairs(root_dir, output_root, reports, args)
    except KeyboardInterrupt:
        print("\n⚠️ 已中断修复。")
        return 130

    summary = summarize_reports(reports)
    write_reports(report_json_path, report_csv_path, root_dir, args, reports, summary)

    print("-" * 72)
    if args.repair:
        repaired_count = sum(1 for item in reports if item["repair_status"] == "repaired")
        print(f"修复完成，成功修复 {repaired_count} 个视频。")
        if output_root is not None:
            print(f"输出目录: {output_root}")
    else:
        print("扫描完成，未执行修复。")
    print(f"JSON 报告: {report_json_path}")
    print(f"CSV 报告: {report_csv_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
