#!/usr/bin/env python3
"""Batch-scan left/right main camera MKV files for timestamp and decode issues.

This script is intended for large-scale, read-only field diagnostics. It
recursively walks one or more roots, finds `left_cam_main.mkv` and
`right_cam_main.mkv`, performs a fast packet-level scan with ffprobe, and
optionally escalates suspicious files to a full decode scan with ffmpeg.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


TARGET_FILE_NAMES = {"left_cam_main.mkv", "right_cam_main.mkv"}
DEFAULT_ALIGN_WINDOW_SEC = 0.25
DEFAULT_LARGE_GAP_SEC = 0.05
DEFAULT_EVENT_LIMIT = 8


@dataclass
class GapEvent:
    prev_time_sec: float
    curr_time_sec: float
    delta_sec: float
    keyframe: bool


@dataclass
class DecodeSummary:
    scanned: bool = False
    non_monotonic_dts_count: int = 0
    ref_missing_count: int = 0
    decode_error_count: int = 0
    warning_samples: list[str] = field(default_factory=list)


@dataclass
class FileSummary:
    path: str
    side: str
    episode_dir: str
    status: str
    codec: str | None = None
    duration_sec: float | None = None
    avg_fps: float | None = None
    packet_count: int = 0
    keyframe_count: int = 0
    duplicate_dts_count: int = 0
    backward_dts_count: int = 0
    gap_count: int = 0
    max_gap_sec: float = 0.0
    suspicious: bool = False
    suspicious_reasons: list[str] = field(default_factory=list)
    largest_gaps: list[GapEvent] = field(default_factory=list)
    decode: DecodeSummary = field(default_factory=DecodeSummary)
    error: str | None = None


@dataclass
class PairSummary:
    episode_dir: str
    left_path: str | None = None
    right_path: str | None = None
    shared_gap_event_count: int = 0
    shared_gap_times_sec: list[float] = field(default_factory=list)
    left_only_large_gap_times_sec: list[float] = field(default_factory=list)
    right_only_large_gap_times_sec: list[float] = field(default_factory=list)
    pair_suspicious: bool = False


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Recursively scan left/right main camera MKV files for timestamp anomalies, "
            "packet gaps, and decode-side corruption."
        )
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="Root directories to scan. Defaults to the current directory.",
    )
    parser.add_argument(
        "--root",
        action="append",
        dest="roots",
        default=[],
        help="Additional root directory to scan. Can be provided multiple times.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Parallel worker count. Default: auto.",
    )
    parser.add_argument(
        "--decode-mode",
        choices=("auto", "full", "off"),
        default="auto",
        help=(
            "Decode scan policy. auto=only suspicious files, full=all files, "
            "off=packet scan only."
        ),
    )
    parser.add_argument(
        "--align-window-sec",
        type=float,
        default=DEFAULT_ALIGN_WINDOW_SEC,
        help="Max time difference when aligning left/right large-gap events.",
    )
    parser.add_argument(
        "--large-gap-sec",
        type=float,
        default=DEFAULT_LARGE_GAP_SEC,
        help="Absolute packet timestamp delta threshold considered a large gap.",
    )
    parser.add_argument(
        "--event-limit",
        type=int,
        default=DEFAULT_EVENT_LIMIT,
        help="Max retained gap/warning samples per file.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON instead of a text report.",
    )
    parser.add_argument(
        "--fail-on-issue",
        action="store_true",
        help="Exit with non-zero status when any suspicious file is found.",
    )
    return parser


def ensure_binary(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"required binary not found in PATH: {name}")


def parse_args() -> argparse.Namespace:
    parser = build_parser()
    args = parser.parse_args()
    roots = list(args.roots)
    roots.extend(args.paths)
    if not roots:
        roots = ["."]
    args.roots = [str(Path(root).resolve()) for root in roots]
    if args.jobs < 0:
        parser.error("--jobs must be >= 0")
    if args.align_window_sec < 0:
        parser.error("--align-window-sec must be >= 0")
    if args.large_gap_sec <= 0:
        parser.error("--large-gap-sec must be > 0")
    if args.event_limit <= 0:
        parser.error("--event-limit must be > 0")
    return args


def auto_jobs() -> int:
    cpu = os.cpu_count() or 4
    return max(1, min(16, cpu * 2))


def side_from_name(path: Path) -> str:
    return "left" if path.name.startswith("left_") else "right"


def discover_target_files(roots: Iterable[str]) -> list[Path]:
    discovered: list[Path] = []
    seen: set[str] = set()
    for root_text in roots:
        root = Path(root_text)
        if root.is_file():
            if root.name in TARGET_FILE_NAMES:
                resolved = str(root.resolve())
                if resolved not in seen:
                    seen.add(resolved)
                    discovered.append(root.resolve())
            continue
        if not root.exists():
            continue
        for dirpath, _, filenames in os.walk(root):
            for filename in filenames:
                if filename not in TARGET_FILE_NAMES:
                    continue
                full_path = str((Path(dirpath) / filename).resolve())
                if full_path in seen:
                    continue
                seen.add(full_path)
                discovered.append(Path(full_path))
    discovered.sort()
    return discovered


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        check=False,
    )


def safe_float(value: str | None) -> float | None:
    if value is None:
        return None
    text = value.strip()
    if not text or text == "N/A":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def parse_rate(rate_text: str | None) -> float | None:
    if not rate_text or rate_text == "0/0":
        return None
    if "/" in rate_text:
        num_text, den_text = rate_text.split("/", 1)
        try:
            num = float(num_text)
            den = float(den_text)
        except ValueError:
            return None
        if den == 0:
            return None
        return num / den
    return safe_float(rate_text)


def load_stream_info(path: Path) -> tuple[str | None, float | None, float | None]:
    command = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=codec_name,avg_frame_rate,r_frame_rate:format=duration",
        "-of",
        "json",
        str(path),
    ]
    result = run_command(command)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"ffprobe failed with code {result.returncode}")
    payload = json.loads(result.stdout or "{}")
    streams = payload.get("streams") or []
    if not streams:
        raise RuntimeError("ffprobe did not report a video stream")
    stream = streams[0]
    codec = stream.get("codec_name")
    avg_fps = parse_rate(stream.get("avg_frame_rate")) or parse_rate(stream.get("r_frame_rate"))
    duration = safe_float(((payload.get("format") or {}).get("duration")))
    return codec, duration, avg_fps


def maybe_append_limited(items: list, item, limit: int) -> None:
    if len(items) < limit:
        items.append(item)


def maybe_append_unique_limited(items: list[str], item: str, limit: int) -> None:
    if item in items:
        return
    if len(items) < limit:
        items.append(item)


def analyze_packets(
    path: Path,
    event_limit: int,
    large_gap_sec: float,
) -> tuple[int, int, int, int, int, float, list[GapEvent], list[float]]:
    command = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_packets",
        "-show_entries",
        "packet=pts_time,dts_time,flags",
        "-of",
        "csv=p=0",
        str(path),
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )

    packet_count = 0
    keyframe_count = 0
    duplicate_dts_count = 0
    backward_dts_count = 0
    gap_count = 0
    max_gap_sec = 0.0
    largest_gaps: list[GapEvent] = []
    large_gap_times: list[float] = []
    previous_t: float | None = None
    eps = 1e-9

    assert process.stdout is not None
    for raw_line in process.stdout:
        line = raw_line.strip()
        if not line:
            continue
        parts = [segment.strip() for segment in line.split(",")]
        while len(parts) < 3:
            parts.append("")
        pts = safe_float(parts[0])
        dts = safe_float(parts[1])
        flags = parts[2]
        timeline = dts if dts is not None else pts
        packet_count += 1
        if "K" in flags:
            keyframe_count += 1
        if timeline is None:
            continue
        if previous_t is not None:
            delta = timeline - previous_t
            if abs(delta) <= eps:
                duplicate_dts_count += 1
            elif delta < 0:
                backward_dts_count += 1
            if delta >= large_gap_sec:
                gap_count += 1
                max_gap_sec = max(max_gap_sec, delta)
                event = GapEvent(
                    prev_time_sec=previous_t,
                    curr_time_sec=timeline,
                    delta_sec=delta,
                    keyframe=("K" in flags),
                )
                maybe_append_limited(largest_gaps, event, event_limit)
                large_gap_times.append(previous_t)
        previous_t = timeline

    stderr_text = process.stderr.read() if process.stderr is not None else ""
    return_code = process.wait()
    if return_code != 0:
        detail = stderr_text.strip() or f"ffprobe packet scan failed with code {return_code}"
        raise RuntimeError(detail)

    largest_gaps.sort(key=lambda item: item.delta_sec, reverse=True)
    if len(largest_gaps) > event_limit:
        largest_gaps = largest_gaps[:event_limit]
    return (
        packet_count,
        keyframe_count,
        duplicate_dts_count,
        backward_dts_count,
        gap_count,
        max_gap_sec,
        largest_gaps,
        large_gap_times[:event_limit],
    )


def run_decode_scan(path: Path, event_limit: int) -> DecodeSummary:
    command = [
        "ffmpeg",
        "-v",
        "warning",
        "-i",
        str(path),
        "-map",
        "0:v:0",
        "-f",
        "null",
        "-",
    ]
    result = run_command(command)
    stderr_lines = [line.strip() for line in result.stderr.splitlines() if line.strip()]
    summary = DecodeSummary(scanned=True)
    for line in stderr_lines:
        lower = line.lower()
        captured = False
        if "non monotonically increasing dts" in lower:
            summary.non_monotonic_dts_count += 1
            captured = True
        if "could not find ref with poc" in lower:
            summary.ref_missing_count += 1
            captured = True
        if "error while decoding" in lower or "missing picture in access unit" in lower:
            summary.decode_error_count += 1
            captured = True
        if captured:
            maybe_append_unique_limited(summary.warning_samples, line, event_limit)
    if result.returncode != 0 and not summary.warning_samples:
        maybe_append_limited(
            summary.warning_samples,
            result.stderr.strip() or f"ffmpeg decode scan failed with code {result.returncode}",
            event_limit,
        )
        summary.decode_error_count += 1
    return summary


def classify_status(summary: FileSummary) -> str:
    if summary.error:
        return "error"
    severe_decode = summary.decode.ref_missing_count > 0 or summary.decode.decode_error_count > 0
    time_issue = (
        summary.backward_dts_count > 0
        or summary.duplicate_dts_count > 0
        or summary.max_gap_sec >= 0.1
        or summary.decode.non_monotonic_dts_count > 0
    )
    if severe_decode and time_issue:
        return "likely_corrupt"
    if severe_decode:
        return "decode_corrupt"
    if time_issue:
        return "timestamp_anomaly"
    return "ok"


def analyze_file(path: Path, args: argparse.Namespace) -> FileSummary:
    summary = FileSummary(
        path=str(path),
        side=side_from_name(path),
        episode_dir=str(path.parent),
        status="pending",
    )
    try:
        codec, duration_sec, avg_fps = load_stream_info(path)
        summary.codec = codec
        summary.duration_sec = duration_sec
        summary.avg_fps = avg_fps
        (
            summary.packet_count,
            summary.keyframe_count,
            summary.duplicate_dts_count,
            summary.backward_dts_count,
            summary.gap_count,
            summary.max_gap_sec,
            summary.largest_gaps,
            large_gap_times,
        ) = analyze_packets(path, args.event_limit, args.large_gap_sec)

        if summary.backward_dts_count > 0:
            summary.suspicious_reasons.append("backward_dts")
        if summary.duplicate_dts_count > 0:
            summary.suspicious_reasons.append("duplicate_dts")
        if summary.max_gap_sec >= args.large_gap_sec:
            summary.suspicious_reasons.append("large_packet_gap")

        should_decode = args.decode_mode == "full"
        if args.decode_mode == "auto" and summary.suspicious_reasons:
            should_decode = True
        if should_decode:
            summary.decode = run_decode_scan(path, args.event_limit)
            if summary.decode.non_monotonic_dts_count > 0:
                summary.suspicious_reasons.append("decode_non_monotonic_dts")
            if summary.decode.ref_missing_count > 0:
                summary.suspicious_reasons.append("missing_reference_frame")
            if summary.decode.decode_error_count > 0:
                summary.suspicious_reasons.append("decode_error")

        summary.suspicious = bool(summary.suspicious_reasons)
        summary.status = classify_status(summary)
    except Exception as exc:  # pragma: no cover - runtime diagnostics
        summary.error = str(exc)
        summary.suspicious = True
        summary.status = "error"
    return summary


def align_shared_events(left_times: list[float], right_times: list[float], window_sec: float) -> tuple[list[float], list[float], list[float]]:
    shared: list[float] = []
    left_only: list[float] = []
    right_only: list[float] = []
    right_used = [False] * len(right_times)

    for left_time in left_times:
        match_index = None
        best_delta = None
        for index, right_time in enumerate(right_times):
            delta = abs(left_time - right_time)
            if delta > window_sec:
                continue
            if match_index is None or delta < best_delta:
                match_index = index
                best_delta = delta
        if match_index is None:
            left_only.append(left_time)
            continue
        right_used[match_index] = True
        shared.append((left_time + right_times[match_index]) / 2.0)

    for index, right_time in enumerate(right_times):
        if not right_used[index]:
            right_only.append(right_time)
    return shared, left_only, right_only


def build_pair_summaries(file_summaries: list[FileSummary], align_window_sec: float) -> list[PairSummary]:
    by_dir: dict[str, dict[str, FileSummary]] = {}
    for summary in file_summaries:
        by_dir.setdefault(summary.episode_dir, {})[summary.side] = summary

    pairs: list[PairSummary] = []
    for episode_dir in sorted(by_dir):
        members = by_dir[episode_dir]
        left = members.get("left")
        right = members.get("right")
        pair = PairSummary(
            episode_dir=episode_dir,
            left_path=left.path if left else None,
            right_path=right.path if right else None,
        )
        if left and right:
            left_times = [event.prev_time_sec for event in left.largest_gaps]
            right_times = [event.prev_time_sec for event in right.largest_gaps]
            shared, left_only, right_only = align_shared_events(left_times, right_times, align_window_sec)
            pair.shared_gap_event_count = len(shared)
            pair.shared_gap_times_sec = shared
            pair.left_only_large_gap_times_sec = left_only
            pair.right_only_large_gap_times_sec = right_only
            pair.pair_suspicious = bool(shared) or left.suspicious or right.suspicious
        else:
            pair.pair_suspicious = bool(left and left.suspicious) or bool(right and right.suspicious)
        pairs.append(pair)
    return pairs


def format_float(value: float | None, digits: int = 3) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "-"
    return f"{value:.{digits}f}"


def print_text_report(
    file_summaries: list[FileSummary],
    pair_summaries: list[PairSummary],
    resolved_jobs: int,
) -> None:
    print(f"Resolved jobs: {resolved_jobs}")
    print(f"Scanned files: {len(file_summaries)}")
    suspicious_count = sum(1 for item in file_summaries if item.suspicious)
    print(f"Suspicious files: {suspicious_count}")
    print("")
    print(
        "STATUS            SIDE   DUR(s)   FPS     GAP_MAX  GAPS  DUP_DTS  BACK_DTS  REF_MISS  PATH"
    )
    for summary in file_summaries:
        print(
            f"{summary.status:<16}  "
            f"{summary.side:<5}  "
            f"{format_float(summary.duration_sec, 2):>7}  "
            f"{format_float(summary.avg_fps, 2):>6}  "
            f"{format_float(summary.max_gap_sec, 3):>7}  "
            f"{summary.gap_count:>4}  "
            f"{summary.duplicate_dts_count:>7}  "
            f"{summary.backward_dts_count:>8}  "
            f"{summary.decode.ref_missing_count:>8}  "
            f"{summary.path}"
        )
        if summary.suspicious_reasons:
            print(f"  reasons: {', '.join(summary.suspicious_reasons)}")
        if summary.largest_gaps:
            gap_items = ", ".join(
                f"{event.prev_time_sec:.3f}->{event.curr_time_sec:.3f} ({event.delta_sec:.3f}s)"
                for event in summary.largest_gaps
            )
            print(f"  large_gaps: {gap_items}")
        if summary.decode.warning_samples:
            print(f"  decode_samples: {' | '.join(summary.decode.warning_samples)}")
        if summary.error:
            print(f"  error: {summary.error}")

    if pair_summaries:
        print("")
        print("PAIR ALIGNMENT")
        for pair in pair_summaries:
            if not pair.left_path and not pair.right_path:
                continue
            status = "pair_suspicious" if pair.pair_suspicious else "pair_ok"
            print(f"{status}: {pair.episode_dir}")
            if pair.shared_gap_times_sec:
                aligned = ", ".join(f"{value:.3f}s" for value in pair.shared_gap_times_sec)
                print(f"  shared_large_gap_times: {aligned}")
            if pair.left_only_large_gap_times_sec:
                left_only = ", ".join(f"{value:.3f}s" for value in pair.left_only_large_gap_times_sec)
                print(f"  left_only_large_gaps: {left_only}")
            if pair.right_only_large_gap_times_sec:
                right_only = ", ".join(f"{value:.3f}s" for value in pair.right_only_large_gap_times_sec)
                print(f"  right_only_large_gaps: {right_only}")
            if not pair.left_path or not pair.right_path:
                print("  warning: only one side found in this directory")


def to_json_payload(file_summaries: list[FileSummary], pair_summaries: list[PairSummary], args: argparse.Namespace) -> dict:
    return {
        "roots": args.roots,
        "decode_mode": args.decode_mode,
        "jobs": args.jobs,
        "file_count": len(file_summaries),
        "suspicious_file_count": sum(1 for item in file_summaries if item.suspicious),
        "files": [asdict(summary) for summary in file_summaries],
        "pairs": [asdict(summary) for summary in pair_summaries],
    }


def main() -> int:
    args = parse_args()
    ensure_binary("ffprobe")
    if args.decode_mode != "off":
        ensure_binary("ffmpeg")

    paths = discover_target_files(args.roots)
    if not paths:
        if args.json:
            print(json.dumps(to_json_payload([], [], args), ensure_ascii=False, indent=2))
        else:
            print("No left/right main camera MKV files found.")
        return 0

    args.jobs = args.jobs or auto_jobs()
    file_summaries: list[FileSummary] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(analyze_file, path, args) for path in paths]
        for future in concurrent.futures.as_completed(futures):
            file_summaries.append(future.result())
    file_summaries.sort(key=lambda item: item.path)
    pair_summaries = build_pair_summaries(file_summaries, args.align_window_sec)

    if args.json:
        print(json.dumps(to_json_payload(file_summaries, pair_summaries, args), ensure_ascii=False, indent=2))
    else:
        print_text_report(file_summaries, pair_summaries, args.jobs)

    if args.fail_on_issue and any(item.suspicious for item in file_summaries):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
