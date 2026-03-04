#!/usr/bin/env python3
"""
Validate whether fays_data.mcap still has IMU samples near the tail camera frames.
"""

import argparse
import sys
from collections import deque

from mcap.reader import NonSeekingReader


def fail(message: str) -> int:
    print(f"FAIL {message}")
    return 1


def validate_tail_imu(mcap_path: str, tail_cam_frames: int, max_lag_sec: float) -> int:
    max_lag_ns = int(max_lag_sec * 1_000_000_000)
    cam_tail = deque(maxlen=max(1, tail_cam_frames))
    cam_count = 0
    imu_count = 0
    last_imu_ts = None

    try:
        with open(mcap_path, "rb") as fh:
            reader = NonSeekingReader(fh)
            for _, channel, message in reader.iter_messages(log_time_order=False):
                if channel.topic == "c":
                    cam_count += 1
                    cam_tail.append(int(message.log_time))
                elif channel.topic == "i":
                    imu_count += 1
                    last_imu_ts = int(message.log_time)
    except Exception as exc:
        return fail(f"read error: {exc}")

    if cam_count <= 0:
        return fail("topic c has zero messages")
    if imu_count <= 0 or last_imu_ts is None:
        return fail("topic i has zero messages")
    if not cam_tail:
        return fail("failed to read tail cam frames")

    tail_cam_start_ts = cam_tail[0]
    tail_cam_end_ts = cam_tail[-1]

    if last_imu_ts < tail_cam_start_ts:
        return fail(
            "tail cam has no imu "
            f"(last_imu_ns={last_imu_ts}, tail_cam_start_ns={tail_cam_start_ts})"
        )

    lag_ns = tail_cam_end_ts - last_imu_ts
    if lag_ns < 0:
        lag_ns = 0

    if lag_ns > max_lag_ns:
        return fail(
            "imu tail lag too large "
            f"(lag_ns={lag_ns}, threshold_ns={max_lag_ns}, "
            f"tail_cam_end_ns={tail_cam_end_ts}, last_imu_ns={last_imu_ts})"
        )

    print(
        f"PASS tail_cam={len(cam_tail)} imu_count={imu_count} "
        f"cam_count={cam_count} lag_sec={lag_ns / 1_000_000_000:.3f}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check whether IMU data still exists at tail camera frames in fays_data.mcap."
    )
    parser.add_argument("--mcap", required=True, help="Path to fays_data.mcap")
    parser.add_argument(
        "--tail-cam-frames",
        type=int,
        default=5,
        help="Number of tail camera frames to check (default: 5)",
    )
    parser.add_argument(
        "--max-lag-sec",
        type=float,
        default=1.0,
        help="Maximum allowed lag from last camera frame to last IMU frame in seconds (default: 1.0)",
    )
    args = parser.parse_args()

    return validate_tail_imu(args.mcap, args.tail_cam_frames, args.max_lag_sec)


if __name__ == "__main__":
    sys.exit(main())
