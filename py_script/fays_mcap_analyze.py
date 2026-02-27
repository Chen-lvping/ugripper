#!/usr/bin/env python3
"""
Analyze fays_data.mcap for data quality: IMU quality, video/IMU timestamp jumps.

Input:
  - fays_data.mcap: Binary MCAP with topic "c" (camera frame_index) and "i" (IMU)

Output:
  - Console report: timestamp jump counts, IMU statistics, quality metrics

Usage:
  python3 fays_mcap_analyze.py <mcap_file> [--video-thresh-ms 50] [--imu-thresh-ms 5]
"""
import argparse
import struct
import sys

from mcap.reader import make_reader
from mcap.stream_reader import StreamReader

# Default thresholds
DEFAULT_VIDEO_THRESH_MS = 50
DEFAULT_IMU_THRESH_MS = 5
MAX_JUMP_EXAMPLES = 10


def iter_mcap_messages(mcap_file):
    """
    Iterate over all messages in an MCAP file.
    Uses make_reader first; falls back to StreamReader for non-standard files.
    Yields (schema, channel, message) tuples.
    """
    with open(mcap_file, "rb") as f:
        reader = make_reader(f)
        yielded = 0
        for item in reader.iter_messages():
            yielded += 1
            yield item

        if yielded > 0:
            return

    with open(mcap_file, "rb") as f:
        schemas = {}
        channels = {}
        for record in StreamReader(f).records:
            name = type(record).__name__
            if name == "Schema":
                schemas[record.id] = record
            elif name == "Channel":
                channels[record.id] = record
            elif name == "Message":
                channel = channels.get(record.channel_id)
                if channel is None:
                    continue
                schema = schemas.get(channel.schema_id)
                yield schema, channel, record


def load_mcap_data(mcap_file):
    """
    Single-pass extraction of camera timestamps and raw IMU records.

    Returns:
        timestamps: dict mapping frame_index -> timestamp_ns (from topic "c")
        imu_records: list of (timestamp_ns, payload_bytes) (from topic "i")
    """
    timestamps = {}
    imu_records = []

    for _schema, channel, message in iter_mcap_messages(mcap_file):
        if channel.topic == "c":
            if len(message.data) >= 4:
                frame_index = struct.unpack("<I", message.data[:4])[0]
                timestamps[frame_index] = message.log_time
        elif channel.topic == "i":
            imu_records.append((message.log_time, bytes(message.data)))

    return timestamps, imu_records


def analyze_video_timestamps(timestamps_dict, thresh_ns):
    """
    Analyze video (topic "c") timestamp jumps.
    Sorts by frame_index to get consecutive frame order.
    """
    cam_timestamps = [(idx, ts) for idx, ts in timestamps_dict.items()]
    cam_timestamps.sort(key=lambda x: x[0])

    if len(cam_timestamps) < 2:
        return {"count": 0, "intervals": [], "jumps": [], "stats": None}

    intervals = []
    jumps = []

    for i in range(1, len(cam_timestamps)):
        dt_ns = cam_timestamps[i][1] - cam_timestamps[i - 1][1]
        dt_ms = dt_ns / 1e6
        intervals.append(dt_ns)

        if dt_ns > thresh_ns:
            jumps.append(
                {
                    "idx": cam_timestamps[i][0],
                    "prev_idx": cam_timestamps[i - 1][0],
                    "dt_ms": dt_ms,
                    "dt_ns": dt_ns,
                }
            )

    intervals_arr = intervals
    mean_ns = sum(intervals_arr) / len(intervals_arr)
    variance = sum((x - mean_ns) ** 2 for x in intervals_arr) / len(intervals_arr)
    std_ns = variance ** 0.5
    sorted_intervals = sorted(intervals_arr)

    stats = {
        "count": len(cam_timestamps),
        "mean_ms": mean_ns / 1e6,
        "std_ms": std_ns / 1e6,
        "min_ms": min(intervals_arr) / 1e6,
        "max_ms": max(intervals_arr) / 1e6,
        "median_ms": sorted_intervals[len(sorted_intervals) // 2] / 1e6,
    }

    return {"count": len(jumps), "intervals": intervals, "jumps": jumps, "stats": stats}


def analyze_imu_timestamps(imu_records, thresh_ns):
    """
    Analyze IMU (topic "i") timestamp jumps.
    Records are already in chronological order.
    """
    if len(imu_records) < 2:
        return {"count": 0, "intervals": [], "jumps": [], "stats": None}

    intervals = []
    jumps = []

    for i in range(1, len(imu_records)):
        dt_ns = imu_records[i][0] - imu_records[i - 1][0]
        dt_ms = dt_ns / 1e6
        intervals.append(dt_ns)

        if dt_ns > thresh_ns:
            jumps.append(
                {
                    "i": i,
                    "prev_i": i - 1,
                    "dt_ms": dt_ms,
                    "dt_ns": dt_ns,
                    "ts_ns": imu_records[i][0],
                }
            )

    intervals_arr = intervals
    mean_ns = sum(intervals_arr) / len(intervals_arr)
    variance = sum((x - mean_ns) ** 2 for x in intervals_arr) / len(intervals_arr)
    std_ns = variance ** 0.5
    sorted_intervals = sorted(intervals_arr)

    stats = {
        "count": len(imu_records),
        "mean_ms": mean_ns / 1e6,
        "std_ms": std_ns / 1e6,
        "min_ms": min(intervals_arr) / 1e6,
        "max_ms": max(intervals_arr) / 1e6,
        "median_ms": sorted_intervals[len(sorted_intervals) // 2] / 1e6,
        "rate_hz": 1e9 / mean_ns if mean_ns > 0 else 0,
    }

    return {"count": len(jumps), "intervals": intervals, "jumps": jumps, "stats": stats}


def parse_imu_payload(payload):
    """Parse IMU payload: 6xfloat64 or 6xfloat32. Returns (gx, gy, gz, ax, ay, az)."""
    if len(payload) >= 48:
        return struct.unpack("<6d", payload[:48])
    if len(payload) >= 24:
        return struct.unpack("<6f", payload[:24])
    return None


def analyze_imu_quality(imu_records):
    """
    Analyze IMU data quality: gyro/accel statistics, outliers, constant/zero detection.
    """
    gyro_x, gyro_y, gyro_z = [], [], []
    acc_x, acc_y, acc_z = [], [], []
    parse_errors = 0

    for _ts, payload in imu_records:
        vals = parse_imu_payload(payload)
        if vals is None:
            parse_errors += 1
            continue
        gx, gy, gz, ax, ay, az = vals
        gyro_x.append(gx)
        gyro_y.append(gy)
        gyro_z.append(gz)
        acc_x.append(ax)
        acc_y.append(ay)
        acc_z.append(az)

    def stats(arr, name):
        if not arr:
            return None
        n = len(arr)
        mean = sum(arr) / n
        var = sum((x - mean) ** 2 for x in arr) / n
        std = var ** 0.5
        return {
            "name": name,
            "mean": mean,
            "std": std,
            "min": min(arr),
            "max": max(arr),
            "range": max(arr) - min(arr),
        }

    gyro_stats = [
        stats(gyro_x, "gx"),
        stats(gyro_y, "gy"),
        stats(gyro_z, "gz"),
    ]
    acc_stats = [
        stats(acc_x, "ax"),
        stats(acc_y, "ay"),
        stats(acc_z, "az"),
    ]

    # Check for constant/zero values
    def is_constant(arr, tol=1e-9):
        if len(arr) < 2:
            return True
        first = arr[0]
        return all(abs(x - first) < tol for x in arr)

    def is_all_zero(arr, tol=1e-9):
        return all(abs(x) < tol for x in arr)

    anomalies = []
    for arr, name in [
        (gyro_x, "gx"),
        (gyro_y, "gy"),
        (gyro_z, "gz"),
        (acc_x, "ax"),
        (acc_y, "ay"),
        (acc_z, "az"),
    ]:
        if is_all_zero(arr):
            anomalies.append(f"{name}: all zeros")
        elif is_constant(arr):
            anomalies.append(f"{name}: constant ({arr[0]:.6f})")

    return {
        "gyro": gyro_stats,
        "acc": acc_stats,
        "parse_errors": parse_errors,
        "valid_count": len(gyro_x),
        "anomalies": anomalies,
    }


def print_report(
    mcap_file,
    timestamps_dict,
    imu_records,
    video_result,
    imu_result,
    imu_quality,
    video_thresh_ms,
    imu_thresh_ms,
):
    """Print structured analysis report to console."""
    print("=" * 60)
    print("  MCAP Data Quality Analysis Report")
    print("=" * 60)
    print(f"  File: {mcap_file}")
    print(f"  Video jump threshold: {video_thresh_ms} ms")
    print(f"  IMU jump threshold:   {imu_thresh_ms} ms")
    print("=" * 60)
    print()

    # File overview
    print("--- File Overview ---")
    print(f"  Camera (topic 'c'): {len(timestamps_dict)} messages")
    print(f"  IMU (topic 'i'):    {len(imu_records)} messages")
    if timestamps_dict:
        first_idx = min(timestamps_dict.keys())
        last_idx = max(timestamps_dict.keys())
        print(f"  Camera frame range: {first_idx} to {last_idx}")
    if imu_records:
        t0 = imu_records[0][0] * 1e-9
        t1 = imu_records[-1][0] * 1e-9
        print(f"  IMU time span: {t0:.6f} s to {t1:.6f} s ({t1 - t0:.3f} s)")
    print()

    # Video timestamp analysis
    print("--- Video Timestamp Analysis ---")
    if video_result["stats"] is None:
        print("  (insufficient data)")
    else:
        s = video_result["stats"]
        print(f"  Frame count: {s['count']}")
        print(f"  Interval: mean={s['mean_ms']:.3f} ms, std={s['std_ms']:.3f} ms")
        print(f"  Interval: min={s['min_ms']:.3f} ms, max={s['max_ms']:.3f} ms")
        print(f"  Interval median: {s['median_ms']:.3f} ms")
        print(f"  Jumps (dt > {video_thresh_ms} ms): {video_result['count']}")
        if video_result["jumps"]:
            examples = video_result["jumps"][:MAX_JUMP_EXAMPLES]
            for j in examples:
                print(f"    frame {j['prev_idx']} -> {j['idx']}: dt={j['dt_ms']:.2f} ms")
            if len(video_result["jumps"]) > MAX_JUMP_EXAMPLES:
                print(f"    ... and {len(video_result['jumps']) - MAX_JUMP_EXAMPLES} more")
    print()

    # IMU timestamp analysis
    print("--- IMU Timestamp Analysis ---")
    if imu_result["stats"] is None:
        print("  (insufficient data)")
    else:
        s = imu_result["stats"]
        print(f"  Sample count: {s['count']}")
        print(f"  Interval: mean={s['mean_ms']:.3f} ms, std={s['std_ms']:.3f} ms")
        print(f"  Interval: min={s['min_ms']:.3f} ms, max={s['max_ms']:.3f} ms")
        print(f"  Interval median: {s['median_ms']:.3f} ms")
        print(f"  Estimated rate: {s['rate_hz']:.1f} Hz")
        print(f"  Jumps (dt > {imu_thresh_ms} ms): {imu_result['count']}")
        if imu_result["jumps"]:
            examples = imu_result["jumps"][:MAX_JUMP_EXAMPLES]
            for j in examples:
                ts_sec = j["ts_ns"] * 1e-9
                print(f"    sample {j['prev_i']} -> {j['i']}: dt={j['dt_ms']:.2f} ms at t={ts_sec:.3f} s")
            if len(imu_result["jumps"]) > MAX_JUMP_EXAMPLES:
                print(f"    ... and {len(imu_result['jumps']) - MAX_JUMP_EXAMPLES} more")
    print()

    # IMU data quality
    print("--- IMU Data Quality ---")
    if imu_quality["parse_errors"] > 0:
        print(f"  Parse errors: {imu_quality['parse_errors']}")
    print(f"  Valid samples: {imu_quality['valid_count']}")
    if imu_quality["anomalies"]:
        print("  Anomalies:")
        for a in imu_quality["anomalies"]:
            print(f"    - {a}")
    print("  Gyroscope (rad/s):")
    for g in imu_quality["gyro"]:
        if g:
            print(f"    {g['name']}: mean={g['mean']:.6f}, std={g['std']:.6f}, range=[{g['min']:.6f}, {g['max']:.6f}]")
    print("  Accelerometer (m/s^2):")
    for a in imu_quality["acc"]:
        if a:
            print(f"    {a['name']}: mean={a['mean']:.6f}, std={a['std']:.6f}, range=[{a['min']:.6f}, {a['max']:.6f}]")
    print()
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(
        description="Analyze fays_data.mcap for IMU quality and timestamp jumps"
    )
    parser.add_argument(
        "mcap_file",
        help="Path to MCAP file (e.g. fays_data.mcap)",
    )
    parser.add_argument(
        "--video-thresh-ms",
        type=float,
        default=DEFAULT_VIDEO_THRESH_MS,
        help=f"Video timestamp jump threshold in ms (default: {DEFAULT_VIDEO_THRESH_MS})",
    )
    parser.add_argument(
        "--imu-thresh-ms",
        type=float,
        default=DEFAULT_IMU_THRESH_MS,
        help=f"IMU timestamp jump threshold in ms (default: {DEFAULT_IMU_THRESH_MS})",
    )
    args = parser.parse_args()

    mcap_file = args.mcap_file
    video_thresh_ns = args.video_thresh_ms * 1e6
    imu_thresh_ns = args.imu_thresh_ms * 1e6

    try:
        timestamps_dict, imu_records = load_mcap_data(mcap_file)
    except FileNotFoundError:
        print(f"[ERROR] File not found: {mcap_file}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"[ERROR] Failed to read MCAP: {e}", file=sys.stderr)
        sys.exit(1)

    video_result = analyze_video_timestamps(timestamps_dict, video_thresh_ns)
    imu_result = analyze_imu_timestamps(imu_records, imu_thresh_ns)
    imu_quality = analyze_imu_quality(imu_records)

    print_report(
        mcap_file,
        timestamps_dict,
        imu_records,
        video_result,
        imu_result,
        imu_quality,
        args.video_thresh_ms,
        args.imu_thresh_ms,
    )


if __name__ == "__main__":
    main()
