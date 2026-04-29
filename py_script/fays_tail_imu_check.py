#!/usr/bin/env python3
"""
Validate whether fays_data.mcap still has IMU samples near the tail camera frames.
"""

import argparse
import sys
from io import BytesIO

from mcap.data_stream import ReadDataStream
from mcap.opcode import Opcode
from mcap.reader import make_reader
from mcap.records import Channel, Chunk, Message
from mcap.stream_reader import breakup_chunk


def fail(message: str) -> int:
    print(f"FAIL {message}")
    return 1


def load_chunk_records(file_handle, chunk_offset: int):
    file_handle.seek(chunk_offset)
    opcode_raw = file_handle.read(1)
    if not opcode_raw:
        return []

    opcode = opcode_raw[0]
    length_raw = file_handle.read(8)
    if len(length_raw) != 8:
        return []

    payload_len = int.from_bytes(length_raw, "little")
    payload = file_handle.read(payload_len)
    if len(payload) != payload_len:
        return []

    if opcode != Opcode.CHUNK:
        return []

    chunk = Chunk.read(ReadDataStream(BytesIO(payload)))
    return breakup_chunk(chunk)


def classify_topic(channel_id: int, payload_size: int, channel_topics: dict) -> str:
    topic = channel_topics.get(channel_id)
    if topic in {"c", "i"}:
        return topic

    # Fays current writer layout:
    # - topic c (camera frame index): 4 bytes
    # - topic i (imu sample): 48 bytes
    if payload_size == 4:
        return "c"
    if payload_size == 48:
        return "i"

    return ""


def validate_tail_imu(mcap_path: str, tail_cam_frames: int, max_lag_sec: float) -> int:
    max_lag_ns = int(max_lag_sec * 1_000_000_000)
    tail_cam_frames = max(1, tail_cam_frames)

    try:
        with open(mcap_path, "rb") as fh:
            summary = make_reader(fh).get_summary()
    except Exception as exc:
        return fail(f"failed to read mcap summary: {exc}")

    if summary is None or summary.statistics is None:
        return fail("missing summary statistics")

    chunk_indexes = list(summary.chunk_indexes or [])
    if not chunk_indexes:
        return fail("missing chunk indexes")

    channel_topics = {
        int(channel_id): channel.topic for channel_id, channel in (summary.channels or {}).items()
    }

    cam_tail_desc = []
    last_imu_ts = None
    last_cam_ts = None
    cam_channel_ids = set()
    imu_channel_ids = set()

    try:
        with open(mcap_path, "rb") as fh:
            for chunk_index in reversed(chunk_indexes):
                records = load_chunk_records(fh, int(chunk_index.chunk_start_offset))
                if not records:
                    continue

                for record in records:
                    if isinstance(record, Channel):
                        channel_topics[int(record.id)] = record.topic

                for record in reversed(records):
                    if not isinstance(record, Message):
                        continue

                    topic = classify_topic(
                        channel_id=int(record.channel_id),
                        payload_size=len(record.data),
                        channel_topics=channel_topics,
                    )
                    if topic == "c":
                        cam_channel_ids.add(int(record.channel_id))
                        if last_cam_ts is None:
                            last_cam_ts = int(record.log_time)
                        if len(cam_tail_desc) < tail_cam_frames:
                            cam_tail_desc.append(int(record.log_time))
                    elif topic == "i":
                        imu_channel_ids.add(int(record.channel_id))
                        if last_imu_ts is None:
                            last_imu_ts = int(record.log_time)

                    if last_imu_ts is not None and len(cam_tail_desc) >= tail_cam_frames:
                        break

                if last_imu_ts is not None and len(cam_tail_desc) >= tail_cam_frames:
                    break
    except Exception as exc:
        return fail(f"read error: {exc}")

    if not cam_tail_desc:
        return fail("topic c has zero messages")
    if last_imu_ts is None:
        return fail("topic i has zero messages")
    if last_cam_ts is None:
        return fail("failed to read tail cam frames")

    statistics = summary.statistics
    channel_message_counts = statistics.channel_message_counts or {}
    cam_count = sum(int(channel_message_counts.get(cid, 0)) for cid in cam_channel_ids)
    imu_count = sum(int(channel_message_counts.get(cid, 0)) for cid in imu_channel_ids)

    if cam_count <= 0:
        cam_count = len(cam_tail_desc)
    if imu_count <= 0:
        imu_count = 1

    tail_cam = sorted(cam_tail_desc)
    tail_cam_start_ts = tail_cam[0]
    tail_cam_end_ts = tail_cam[-1]

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
        f"PASS tail_cam={len(tail_cam)} imu_count={imu_count} "
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
