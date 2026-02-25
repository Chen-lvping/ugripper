#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import rospy
import rosbag
import cv2
import numpy as np
import yaml
from sensor_msgs.msg import Image
from cv_bridge import CvBridge

def load_camera_params(yaml_file, cam_name="cam0"):
    """
    从 Kalibr camchain.yaml 文件中读取内参和畸变参数
    """
    with open(yaml_file, 'r') as f:
        cam_data = yaml.safe_load(f)

    cam = cam_data[cam_name]
    fx, fy, cx, cy = cam["intrinsics"]
    D = np.array(cam["distortion_coeffs"], dtype=np.float64)
    K = np.array([[fx, 0, cx],
                  [0, fy, cy],
                  [0, 0, 1]], dtype=np.float64)
    resolution = cam["resolution"]
    return K, D, resolution

def main():
    rospy.init_node("bag_undistort_and_publish_yaml")

    bag_path = rospy.get_param("~bag", "/home/junquan/dm_test/calib/calib_data/911262q0001f5/rgb_video_ros.bag")
    input_topic = rospy.get_param("~input_topic", "/camera/image_raw")
    output_topic = rospy.get_param("~output_topic", "/camera/image_undistorted")
    yaml_file = rospy.get_param("~cam_yaml", "/home/junquan/dm_test/calib/calib_data/911262q0001f5/rgb_video_ros-camchain.yaml")
    cam_name = rospy.get_param("~cam_name", "cam0")

    if bag_path == "":
        rospy.logerr("No bag file specified!")
        return

    # 读取 YAML 内参
    K, D, resolution = load_camera_params(yaml_file, cam_name)
    rospy.loginfo("Loaded camera intrinsics from YAML: fx=%.3f fy=%.3f cx=%.3f cy=%.3f", K[0,0], K[1,1], K[0,2], K[1,2])
    rospy.loginfo("Distortion coefficients: %s", D)

    pub = rospy.Publisher(output_topic, Image, queue_size=10)
    bridge = CvBridge()

    map1, map2 = None, None

    rospy.loginfo("Opening rosbag: %s", bag_path)
    bag = rosbag.Bag(bag_path, "r")

    for topic, msg, t in bag.read_messages(topics=[input_topic]):
        if rospy.is_shutdown():
            break

        # ROS Image -> OpenCV
        try:
            cv_img = bridge.imgmsg_to_cv2(msg, desired_encoding="bgr8")
        except Exception as e:
            rospy.logerr("cv_bridge error: %s", e)
            continue

        h, w = cv_img.shape[:2]

        # 第一次初始化映射表
        if map1 is None:
            rospy.loginfo("Initializing fisheye undistortion maps...")
            map1, map2 = cv2.fisheye.initUndistortRectifyMap(
                K, D, np.eye(3), K, (w, h), cv2.CV_16SC2
            )

        # 去畸变
        undistorted = cv2.remap(
            cv_img,
            map1,
            map2,
            interpolation=cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT
        )

        # OpenCV -> ROS Image
        out_msg = bridge.cv2_to_imgmsg(undistorted, encoding="bgr8")
        out_msg.header = msg.header
        pub.publish(out_msg)

        # 原图和去畸变图拼接并缩小一半显示
        comparison = np.hstack((cv_img, undistorted))
        comparison = cv2.resize(comparison, (0,0), fx=0.5, fy=0.5)
        cv2.imshow("Original | Undistorted (Equidistant)", comparison)
        if cv2.waitKey(1) == 27:  # ESC 退出
            break

        rospy.sleep(0.001)

    bag.close()
    cv2.destroyAllWindows()
    rospy.loginfo("Finished processing rosbag")


if __name__ == "__main__":
    main()

