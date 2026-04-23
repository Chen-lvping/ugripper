#!/usr/bin/env python3

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path


def run_command(args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def parse_fraction(text):
    if not text or text == "0/0":
        return None
    if "/" in text:
        num, den = text.split("/", 1)
        try:
            num = float(num)
            den = float(den)
            if den == 0:
                return None
            return num / den
        except ValueError:
            return None
    try:
        return float(text)
    except ValueError:
        return None


def probe_stream_info(ffprobe_bin, input_path):
    code, out, err = run_command(
        [
            ffprobe_bin,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=avg_frame_rate,r_frame_rate:format=duration",
            "-of",
            "json",
            str(input_path),
        ]
    )
    if code != 0:
        raise RuntimeError(f"ffprobe failed: {err or out}")

    data = json.loads(out)
    duration = float(data["format"]["duration"])
    stream = data["streams"][0]
    fps = parse_fraction(stream.get("avg_frame_rate")) or parse_fraction(stream.get("r_frame_rate"))
    return duration, fps


def count_frames(ffprobe_bin, input_path, start_sec, window_sec):
    code, out, err = run_command(
        [
            ffprobe_bin,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-count_frames",
            "-show_entries",
            "stream=nb_read_frames",
            "-of",
            "default=nokey=1:noprint_wrappers=1",
            "-read_intervals",
            f"{start_sec}%+{window_sec}",
            str(input_path),
        ]
    )
    if code != 0:
        return None, err or out
    text = out.strip()
    if not text or text == "N/A":
        return None, None
    try:
        return int(text), None
    except ValueError:
        return None, f"unexpected frame count output: {text}"


def decode_window(ffmpeg_bin, input_path, start_sec, window_sec):
    code, out, err = run_command(
        [
            ffmpeg_bin,
            "-v",
            "error",
            "-xerror",
            "-i",
            str(input_path),
            "-ss",
            str(start_sec),
            "-t",
            str(window_sec),
            "-map",
            "0:v:0",
            "-f",
            "null",
            "-",
        ]
    )
    return code == 0, err or out


def summarize_failures(failure_types):
    if not failure_types:
        return {"primary_failure": None, "validation_reason": "ok"}

    priority = [
        "decode_error",
        "frame_count_error",
        "frame_count_missing",
        "low_frame_count",
    ]
    primary_failure = None
    for candidate in priority:
        if failure_types.get(candidate, 0) > 0:
            primary_failure = candidate
            break
    if primary_failure is None:
        primary_failure = sorted(failure_types.items(), key=lambda item: (-item[1], item[0]))[0][0]

    reason_map = {
        "decode_error": "video_decode_error",
        "frame_count_error": "video_frame_count_error",
        "frame_count_missing": "video_frame_count_missing",
        "low_frame_count": "video_low_frame_window",
    }
    return {
        "primary_failure": primary_failure,
        "validation_reason": reason_map.get(primary_failure, f"video_{primary_failure}"),
    }


def evaluate_windows(duration_sec, expected_fps, window_sec, min_frame_ratio, frame_counter, decode_checker=None):
    expected_frames_per_window = None
    min_frames = 1
    if expected_fps is not None:
        expected_frames_per_window = expected_fps * window_sec
        min_frames = max(1, math.floor(expected_frames_per_window * min_frame_ratio))

    windows = []
    failure_types = {}
    any_fail = False
    start_sec = 0.0
    while start_sec < duration_sec:
        this_window = min(window_sec, duration_sec - start_sec)
        window_min_frames = 1
        if expected_fps is not None:
            window_min_frames = max(1, math.floor(expected_fps * this_window * min_frame_ratio))
        frame_count, frame_error = frame_counter(start_sec, this_window)
        decode_ok = True
        decode_error = ""
        if decode_checker is not None:
            decode_ok, decode_error = decode_checker(start_sec, this_window)

        failures = []
        if frame_error:
            failures.append(f"frame_count_error:{frame_error}")
        elif expected_fps is not None and frame_count is None:
            failures.append("frame_count_missing")
        if frame_count is not None and frame_count < window_min_frames:
            failures.append(f"low_frame_count:{frame_count}< {window_min_frames}")
        if decode_checker is not None and not decode_ok:
            failures.append(f"decode_error:{decode_error}")

        windows.append(
            {
                "start_sec": round(start_sec, 3),
                "duration_sec": round(this_window, 3),
                "min_frames": window_min_frames,
                "frame_count": frame_count,
                "decode_ok": decode_ok,
                "failures": failures,
            }
        )
        if failures:
            any_fail = True
            for failure in failures:
                failure_type = failure.split(":", 1)[0]
                failure_types[failure_type] = failure_types.get(failure_type, 0) + 1

        start_sec += window_sec

    summary = summarize_failures(failure_types)
    return {
        "duration_sec": duration_sec,
        "expected_fps": expected_fps,
        "window_sec": window_sec,
        "min_frame_ratio": min_frame_ratio,
        "min_frames": min_frames,
        "failure_types": failure_types,
        "primary_failure": summary["primary_failure"],
        "validation_reason": summary["validation_reason"],
        "ok": not any_fail,
        "windows": windows,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Checks a video file window-by-window and reports decode failures or low-frame windows."
    )
    parser.add_argument("--input", required=True, help="Video file to inspect")
    parser.add_argument("--window-sec", type=float, default=2.0, help="Window size in seconds")
    parser.add_argument("--expected-fps", type=float, default=None, help="Override expected FPS")
    parser.add_argument(
        "--min-frame-ratio",
        type=float,
        default=0.5,
        help="Minimum fraction of expected frames required in a window",
    )
    parser.add_argument("--decode-check", action="store_true", help="Run ffmpeg decode check for each window")
    parser.add_argument("--json-out", help="Optional JSON output path")
    parser.add_argument("--ffprobe-bin", default="ffprobe")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg")
    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"[FAIL] {input_path}: file does not exist", file=sys.stderr)
        return 2

    duration_sec, probed_fps = probe_stream_info(args.ffprobe_bin, input_path)
    expected_fps = args.expected_fps if args.expected_fps is not None else probed_fps
    summary = evaluate_windows(
        duration_sec,
        expected_fps,
        args.window_sec,
        args.min_frame_ratio,
        lambda start_sec, this_window: count_frames(args.ffprobe_bin, input_path, start_sec, this_window),
        (lambda start_sec, this_window: decode_window(args.ffmpeg_bin, input_path, start_sec, this_window))
        if args.decode_check
        else None,
    )
    summary["input"] = str(input_path)

    for window in summary["windows"]:
        start_sec = window["start_sec"]
        end_sec = window["start_sec"] + window["duration_sec"]
        frame_count = window["frame_count"]
        failures = window["failures"]
        if failures:
            print(
                f"[FAIL] {start_sec:.3f}-{end_sec:.3f}s "
                f"frame_count={frame_count} failures={'; '.join(failures)}"
            )
        else:
            print(f"[PASS] {start_sec:.3f}-{end_sec:.3f}s frame_count={frame_count}")

    print(
        f"[SUMMARY] ok={summary['ok']} "
        f"validation_reason={summary['validation_reason']} "
        f"primary_failure={summary['primary_failure']}"
    )

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    return 1 if not summary["ok"] else 0


if __name__ == "__main__":
    sys.exit(main())
