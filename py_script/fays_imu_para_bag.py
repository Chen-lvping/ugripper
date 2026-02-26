#!/usr/bin/env python3
"""
Convert stereo MKV video and unified MCAP data to ROS1 bag file.

Input files (in the same directory):
  - fays_stereo_output.mkv: Top-bottom stereo video (upper=left cam, lower=right cam)
  - fays_data.mcap:         Binary-encoded MCAP containing:
                              topic "i" : IMU   (6xfloat64  gx,gy,gz,ax,ay,az)
                              topic "c" : Camera (uint32     frame_index)

Output:
  - output.bag: ROS1 bag with stereo images and IMU data

Usage:
  python3 fays_imu_para_bag.py <data_directory> [output_bag]

  data_directory: Directory containing the 2 input files
  output_bag:     Output bag file path (default: <data_directory>/output.bag)
"""
import rosbag
import rospy
import cv2
import numpy as np
import os
import sys
import struct

from sensor_msgs.msg import Image, Imu
from std_msgs.msg import Header
from cv_bridge import CvBridge
from mcap.reader import make_reader
from mcap.stream_reader import StreamReader

# ======================= Configuration =======================
# Topic names
LEFT_CAM_TOPIC = "/fays/atrak/cam0"
RIGHT_CAM_TOPIC = "/fays/atrak/cam1"
IMU_TOPIC = "/fays/atrak/imu"

# Frame IDs
LEFT_CAM_FRAME_ID = "cam0"
RIGHT_CAM_FRAME_ID = "cam1"
IMU_FRAME_ID = "imu_link"
# =============================================================

bridge = CvBridge()


def _ns_to_ros_time(timestamp_ns):
    """Convert nanosecond timestamp to rospy.Time without float64 precision loss."""
    secs = int(timestamp_ns // 1_000_000_000)
    nsecs = int(timestamp_ns % 1_000_000_000)
    return rospy.Time(secs, nsecs)


def _is_gray_bgr(frame):
    """Check if a BGR frame is actually grayscale (B==G==R for all pixels)."""
    if len(frame.shape) != 3 or frame.shape[2] != 3:
        return False
    b, g, r = frame[:, :, 0], frame[:, :, 1], frame[:, :, 2]
    return bool(np.array_equal(b, g) and np.array_equal(g, r))


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

    # Fallback: StreamReader for files without a proper summary
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
    Single-pass extraction of both camera timestamps and raw IMU records.

    Returns:
        timestamps: dict mapping frame_index -> timestamp_ns  (from topic "c")
        imu_records: list of (timestamp_ns, payload_bytes)     (from topic "i")
    """
    timestamps = {}
    imu_records = []
    print(f"[MCAP] Reading {mcap_file} (single pass)...")

    for _schema, channel, message in iter_mcap_messages(mcap_file):
        if channel.topic == "c":
            if len(message.data) >= 4:
                frame_index = struct.unpack("<I", message.data[:4])[0]
                timestamps[frame_index] = message.log_time
        elif channel.topic == "i":
            imu_records.append((message.log_time, bytes(message.data)))

    print(f"[MCAP] Loaded {len(timestamps)} camera timestamps, {len(imu_records)} IMU samples")
    if timestamps:
        first_idx = min(timestamps.keys())
        last_idx = max(timestamps.keys())
        print(f"[MCAP] Cam frame range: {first_idx} to {last_idx}")
        print(
            f"[MCAP] Cam first: {timestamps[first_idx]} ns "
            f"({timestamps[first_idx] * 1e-9:.6f} sec)"
        )
        print(
            f"[MCAP] Cam last:  {timestamps[last_idx]} ns "
            f"({timestamps[last_idx] * 1e-9:.6f} sec)"
        )
    if imu_records:
        print(
            f"[MCAP] IMU first: {imu_records[0][0]} ns "
            f"({imu_records[0][0] * 1e-9:.6f} sec)"
        )
        print(
            f"[MCAP] IMU last:  {imu_records[-1][0]} ns "
            f"({imu_records[-1][0] * 1e-9:.6f} sec)"
        )

    return timestamps, imu_records


def iter_imu_messages(imu_records):
    """
    Generator that yields (timestamp_ns, topic, Imu msg) from pre-loaded IMU records.
    Each record payload: 6xfloat64 little-endian (gx, gy, gz, ax, ay, az) = 48 bytes.
    Falls back to 6xfloat32 (24 bytes) for compatibility.
    """
    print(f"[IMU] Building {len(imu_records)} IMU messages...")

    count = 0
    for timestamp_ns, payload in imu_records:
        stamp = _ns_to_ros_time(timestamp_ns)

        if len(payload) >= 48:
            gx, gy, gz, ax, ay, az = struct.unpack("<6d", payload[:48])
        elif len(payload) >= 24:
            gx, gy, gz, ax, ay, az = struct.unpack("<6f", payload[:24])
        else:
            print(f"[IMU] WARNING: Unexpected payload size {len(payload)} bytes, skipping")
            continue

        imu = Imu()
        imu.header.stamp = stamp
        imu.header.frame_id = IMU_FRAME_ID

        imu.angular_velocity.x = gx
        imu.angular_velocity.y = gy
        imu.angular_velocity.z = gz

        imu.linear_acceleration.x = ax
        imu.linear_acceleration.y = ay
        imu.linear_acceleration.z = az

        imu.orientation.w = 1.0
        imu.orientation.x = 0.0
        imu.orientation.y = 0.0
        imu.orientation.z = 0.0
        imu.orientation_covariance[0] = -1.0

        count += 1
        yield (timestamp_ns, IMU_TOPIC, imu)

    print(f"[IMU] Done: {count} IMU messages")


def iter_stereo_messages(mkv_file, timestamps_dict):
    """
    Generator that yields (timestamp_ns, topic, Image msg) for each stereo frame.
    Splits top-bottom stereo into left (top) and right (bottom) camera images.
    Timestamps come from the timestamps_dict (extracted from MCAP topic "c").
    """
    cap = cv2.VideoCapture(mkv_file)
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open {mkv_file}")

    fps = cap.get(cv2.CAP_PROP_FPS)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"[Stereo] Video: {width}x{height}, FPS={fps:.2f}, Total frames={total_frames}")

    half_height = height // 2
    frame_idx = 0
    left_count = 0
    right_count = 0
    is_mono = None

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        if frame_idx not in timestamps_dict:
            print(f"[Stereo] WARNING: No timestamp for frame {frame_idx}, skipping")
            frame_idx += 1
            continue

        timestamp_ns = timestamps_dict[frame_idx]
        stamp = _ns_to_ros_time(timestamp_ns)

        # Split into top (left camera) and bottom (right camera)
        left_frame = frame[0:half_height, :]
        right_frame = frame[half_height:, :]

        if is_mono is None:
            if len(frame.shape) == 2 or frame.shape[2] == 1:
                is_mono = True
            else:
                is_mono = _is_gray_bgr(frame)
            ch = frame.shape[2] if len(frame.shape) > 2 else 1
            print(f"[Stereo] First frame shape: {frame.shape}, channels={ch}")
            print(f"[Stereo] Left: {left_frame.shape}, Right: {right_frame.shape}")
            print(f"[Stereo] Detected encoding: {'mono8' if is_mono else 'bgr8'}")

        if is_mono:
            encoding = "mono8"
            if len(left_frame.shape) == 3:
                left_frame = cv2.cvtColor(left_frame, cv2.COLOR_BGR2GRAY)
                right_frame = cv2.cvtColor(right_frame, cv2.COLOR_BGR2GRAY)
        else:
            encoding = "bgr8"

        try:
            # Left camera image
            left_msg = bridge.cv2_to_imgmsg(left_frame, encoding=encoding)
            left_msg.header = Header()
            left_msg.header.stamp = stamp
            left_msg.header.frame_id = LEFT_CAM_FRAME_ID
            yield (timestamp_ns, LEFT_CAM_TOPIC, left_msg)
            left_count += 1

            # Right camera image
            right_msg = bridge.cv2_to_imgmsg(right_frame, encoding=encoding)
            right_msg.header = Header()
            right_msg.header.stamp = stamp
            right_msg.header.frame_id = RIGHT_CAM_FRAME_ID
            yield (timestamp_ns, RIGHT_CAM_TOPIC, right_msg)
            right_count += 1

            if left_count % 100 == 0:
                print(f"[Stereo] Processed {left_count} stereo pairs...")

        except Exception as e:
            print(f"[Stereo] Error converting frame {frame_idx}: {e}")
            frame_idx += 1
            continue

        frame_idx += 1

    cap.release()
    print(f"[Stereo] Done: {left_count} left + {right_count} right images")


def merge_and_write(bag, stereo_iter, imu_iter):
    """
    Merge stereo and IMU message streams by timestamp and write to bag
    in chronological order.
    """
    print("[Merge] Merging stereo and IMU streams by timestamp...")

    stereo_msg = next(stereo_iter, None)
    imu_msg = next(imu_iter, None)

    total_count = 0

    while stereo_msg is not None or imu_msg is not None:
        if stereo_msg is not None and imu_msg is not None:
            if stereo_msg[0] <= imu_msg[0]:
                ts_ns, topic, msg = stereo_msg
                bag.write(topic, msg, _ns_to_ros_time(ts_ns))
                stereo_msg = next(stereo_iter, None)
            else:
                ts_ns, topic, msg = imu_msg
                bag.write(topic, msg, _ns_to_ros_time(ts_ns))
                imu_msg = next(imu_iter, None)
        elif stereo_msg is not None:
            ts_ns, topic, msg = stereo_msg
            bag.write(topic, msg, _ns_to_ros_time(ts_ns))
            stereo_msg = next(stereo_iter, None)
        else:
            ts_ns, topic, msg = imu_msg
            bag.write(topic, msg, _ns_to_ros_time(ts_ns))
            imu_msg = next(imu_iter, None)

        total_count += 1
        if total_count % 5000 == 0:
            print(f"[Merge] Written {total_count} messages...")

    print(f"[Merge] Done: {total_count} total messages written")


def main():
    # Parse arguments
    if len(sys.argv) < 2:
        print("Usage: python3 fays_imu_para_bag.py <data_directory> [output_bag]")
        print()
        print("  data_directory: Directory containing fays_data.mcap and fays_stereo_output.mkv")
        print("  output_bag:     Output bag file path (default: <data_directory>/output.bag)")
        sys.exit(1)

    data_dir = sys.argv[1]
    if not data_dir.endswith("/"):
        data_dir += "/"

    mcap_file = data_dir + "fays_data.mcap"
    mkv_file = data_dir + "fays_stereo_output.mkv"

    if len(sys.argv) > 2:
        out_bag = sys.argv[2]
    else:
        out_bag = data_dir + "output.bag"

    # Verify input files exist
    for fpath in [mcap_file, mkv_file]:
        if not os.path.exists(fpath):
            print(f"[ERROR] File not found: {fpath}")
            sys.exit(1)

    print("=" * 55)
    print("  MCAP + MKV  -->  ROS1 Bag Converter")
    print("=" * 55)
    print(f"  MCAP: {mcap_file}")
    print(f"  MKV:  {mkv_file}")
    print(f"  OUT:  {out_bag}")
    print(f"  Topics: {LEFT_CAM_TOPIC}, {RIGHT_CAM_TOPIC}, {IMU_TOPIC}")
    print("=" * 55)
    print()

    rospy.init_node("fays_mcap_mkv_to_rosbag", anonymous=False)

    # Step 1: Single-pass MCAP extraction (camera timestamps + IMU records)
    timestamps_dict, imu_records = load_mcap_data(mcap_file)
    if len(timestamps_dict) == 0:
        print("[ERROR] No camera timestamps found in MCAP (topic 'c')!")
        sys.exit(1)

    print()

    # Step 2: Create iterators for stereo images and IMU data
    stereo_iter = iter_stereo_messages(mkv_file, timestamps_dict)
    imu_iter = iter_imu_messages(imu_records)

    # Step 3: Merge-sort by timestamp and write to bag
    with rosbag.Bag(out_bag, "w") as bag:
        merge_and_write(bag, stereo_iter, imu_iter)

    print()
    print(f"[Done] ROS bag saved to {out_bag}")

    # Print bag info
    bag = rosbag.Bag(out_bag)
    info = bag.get_type_and_topic_info()
    print()
    print("=== Bag Info ===")
    for topic_name, topic_info in info.topics.items():
        print(f"  {topic_name}: {topic_info.message_count} msgs, type={topic_info.msg_type}")
    bag.close()


if __name__ == "__main__":
    main()
