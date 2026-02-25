#!/usr/bin/env python3
"""
Alternative version using ffmpeg-python for accurate PTS extraction
Requires: pip install ffmpeg-python
"""
import argparse
import rosbag
import rospy
import cv2
import json
import subprocess
import numpy as np

from sensor_msgs.msg import Image
from std_msgs.msg import Header
from cv_bridge import CvBridge

# ======================= 配置区 =======================
IMAGE_TOPIC = "/camera/image_raw"

CAM_FRAME_ID = "camera"
# =====================================================

bridge = CvBridge()


def load_boot_time(info_json):
    """
    Use boot_time_offset from info.json (seconds)
    """
    with open(info_json, "r") as f:
        info = json.load(f)

    return float(info["boot_time_offset"])   # seconds


def extract_pts_from_mkv(mkv_file):
    """
    Extract PTS timestamps from MKV file using ffprobe
    Returns: list of (frame_index, pts_seconds) tuples
    """
    print("[FFmpeg] Extracting PTS from MKV file...")
    
    # Use ffprobe to extract frame PTS
    cmd = [
        'ffprobe',
        '-v', 'error',
        '-select_streams', 'v:0',
        '-show_entries', 'frame=pkt_pts_time',
        '-of', 'csv=p=0',
        mkv_file
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        pts_list = []
        for idx, line in enumerate(result.stdout.strip().split('\n')):
            if line:
                try:
                    pts_sec = float(line)
                    pts_list.append((idx, pts_sec))
                except ValueError:
                    continue
        
        print(f"[FFmpeg] Extracted {len(pts_list)} PTS timestamps")
        if len(pts_list) > 0:
            print(f"[FFmpeg] First PTS: {pts_list[0][1]:.6f}s, Last PTS: {pts_list[-1][1]:.6f}s")
        
        return pts_list
    except subprocess.CalledProcessError as e:
        print(f"[FFmpeg] Error: {e}")
        print(f"[FFmpeg] stderr: {e.stderr}")
        return None
    except FileNotFoundError:
        print("[FFmpeg] ERROR: ffprobe not found. Please install ffmpeg.")
        return None


def write_camera_with_pts(bag, boot_time_sec, pts_list, cam_mkv):
    """
    Write camera frames using accurate PTS from ffprobe
    """
    cap = cv2.VideoCapture(cam_mkv)
    if not cap.isOpened():
        raise RuntimeError("Cannot open cam.mkv")

    fps = cap.get(cv2.CAP_PROP_FPS)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"[Camera] Video properties: {width}x{height}, FPS={fps:.2f}, Total frames={total_frames}")

    if pts_list is None or len(pts_list) == 0:
        print("[Camera] WARNING: No PTS list, falling back to frame-based timing")
        pts_list = [(i, i/fps) for i in range(total_frames)]

    count = 0
    pts_idx = 0
    
    while True:
        ret, frame = cap.read()
        if not ret:
            break

        # Use PTS from ffprobe if available
        if pts_idx < len(pts_list):
            _, pts_sec = pts_list[pts_idx]
        else:
            # Fallback
            pts_sec = pts_idx / fps if fps > 0 else pts_idx * 0.033

        stamp = rospy.Time.from_sec(boot_time_sec + pts_sec)

        new_width = int(frame.shape[1] / 1)
        new_height = int(frame.shape[0] / 1)
        frame_resized = cv2.resize(frame, (new_width, new_height))

        # Debug first frame
        if count == 0:
            print(f"[Camera] First frame: original shape={frame.shape}, resized shape={frame_resized.shape}, "
                  f"dtype={frame_resized.dtype}, pts_sec={pts_sec:.6f}, stamp={stamp.to_sec():.6f}")

        # Validate frame
        if frame_resized is None or frame_resized.size == 0:
            print(f"[Camera] Warning: Empty frame at index {pts_idx}")
            pts_idx += 1
            continue

        try:
            img_msg = bridge.cv2_to_imgmsg(frame_resized, encoding="bgr8")
            img_msg.header = Header()
            img_msg.header.stamp = stamp
            img_msg.header.frame_id = CAM_FRAME_ID
            
            if count % 2 == 0:
                bag.write(IMAGE_TOPIC, img_msg, stamp)
            #bag.write(IMAGE_TOPIC, img_msg, stamp)
            count += 1
            
            if count % 100 == 0:
                print(f"[Camera] Processed {count}/{total_frames} frames")
        except Exception as e:
            print(f"[Camera] Error converting frame {pts_idx}: {e}")
            import traceback
            traceback.print_exc()
            pts_idx += 1
            continue

        pts_idx += 1

    cap.release()
    print(f"[Camera] {count} frames written successfully")


def main():
    parser = argparse.ArgumentParser(description="Convert MKV video to ROS bag with PTS timestamps")
    parser.add_argument("-i", "--cam-mkv", required=True, help="Input MKV video path")
    parser.add_argument("-j", "--info-json", required=True, help="Path to info.json")
    parser.add_argument("-o", "--out-bag", required=True, help="Output rosbag path")
    args = parser.parse_args()

    rospy.init_node("mkv_to_rosbag_ffmpeg", anonymous=False)

    boot_time_sec = load_boot_time(args.info_json)
    print(f"[Info] boot_time_offset = {boot_time_sec:.6f} sec")

    # Extract PTS using ffprobe
    pts_list = extract_pts_from_mkv(args.cam_mkv)

    with rosbag.Bag(args.out_bag, "w") as bag:
        write_camera_with_pts(bag, boot_time_sec, pts_list, args.cam_mkv)

    print(f"[Done] Rosbag saved to {args.out_bag}")


if __name__ == "__main__":
    main()

#python3 rgb_mkv_to_rosbag_ffmpeg.py --cam-mkv /home/junquan/dm_test/calib/calib_data/911262q0001f5/cam.mkv --info-json /home/junquan/dm_test/calib/calib_data/911262q0001f5/info.json --out-bag /home/junquan/dm_test/calib/calib_data/911262q0001f5/rgb_video_ros.bag