#!/usr/bin/env python3
"""Convert Kalibr text report to VINS-Fusion yaml files.

Input:
- Kalibr text report, e.g. fays_tmp_calib-results-imucam.txt

Output:
- StereoIMU-vinsfusion yaml
- cam0_equidistant yaml
- cam1_equidistant yaml
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Dict, List

FLOAT_RE = r"[+-]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?"
SCRIPT_DIR = Path(__file__).resolve().parent


def _require_match(pattern: str, text: str, what: str, flags: int = 0) -> re.Match[str]:
    match = re.search(pattern, text, flags)
    if not match:
        raise ValueError(f"Cannot find {what} in kalibr result file")
    return match


def _parse_float_list(raw: str, expected: int | None = None) -> List[float]:
    nums = [float(x) for x in re.findall(FLOAT_RE, raw)]
    if expected is not None and len(nums) != expected:
        raise ValueError(f"Expected {expected} numbers, got {len(nums)} from: {raw}")
    return nums


def parse_t_ic(text: str, cam_name: str) -> List[float]:
    pattern = (
        rf"Transformation \({re.escape(cam_name)}\):"
        rf".*?T_ic:\s*\({re.escape(cam_name)} to imu0\):\s*\n"
        rf"(\[\[.*?\]\])"
    )
    matrix_raw = _require_match(pattern, text, f"T_ic of {cam_name}", re.S).group(1)
    matrix_vals = _parse_float_list(matrix_raw, expected=16)
    return matrix_vals


def parse_timeshift(text: str, cam_name: str) -> float:
    pattern = rf"timeshift\s+{re.escape(cam_name)}\s+to\s+imu0:.*?\n\s*({FLOAT_RE})"
    return float(_require_match(pattern, text, f"timeshift of {cam_name}", re.S).group(1))


def parse_camera_block(text: str, cam_name: str) -> Dict[str, List[float]]:
    if cam_name == "cam0":
        block_pattern = rf"{re.escape(cam_name)}\n-----\n(.*?)(?=\ncam1\n-----)"
    elif cam_name == "cam1":
        block_pattern = rf"{re.escape(cam_name)}\n-----\n(.*?)(?=\nIMU configuration)"
    else:
        raise ValueError(f"Unsupported camera name: {cam_name}")

    block = _require_match(block_pattern, text, f"camera block {cam_name}", re.S).group(1)

    focal = _parse_float_list(
        _require_match(r"Focal length:\s*\[([^\]]+)\]", block, f"focal length of {cam_name}").group(1),
        expected=2,
    )
    principal = _parse_float_list(
        _require_match(r"Principal point:\s*\[([^\]]+)\]", block, f"principal point of {cam_name}").group(1),
        expected=2,
    )
    dist = _parse_float_list(
        _require_match(
            r"Distortion coefficients:\s*\[([^\]]+)\]",
            block,
            f"distortion coeffs of {cam_name}",
        ).group(1),
        expected=4,
    )

    return {
        "focal": focal,
        "principal": principal,
        "dist": dist,
    }


def parse_imu_params(text: str) -> Dict[str, float]:
    acc_block = _require_match(r"Accelerometer:\s*(.*?)\n\s*Gyroscope:", text, "accelerometer block", re.S).group(1)
    gyr_block = _require_match(r"Gyroscope:\s*(.*?)\n\s*T_ib", text, "gyroscope block", re.S).group(1)

    acc_n = float(_require_match(rf"Noise density \(discrete\):\s*({FLOAT_RE})", acc_block, "acc_n").group(1))
    acc_w = float(_require_match(rf"Random walk:\s*({FLOAT_RE})", acc_block, "acc_w").group(1))
    gyr_n = float(_require_match(rf"Noise density \(discrete\):\s*({FLOAT_RE})", gyr_block, "gyr_n").group(1))
    gyr_w = float(_require_match(rf"Random walk:\s*({FLOAT_RE})", gyr_block, "gyr_w").group(1))

    return {
        "acc_n": acc_n,
        "gyr_n": gyr_n,
        "acc_w": acc_w,
        "gyr_w": gyr_w,
    }


def e(v: float) -> str:
    return f"{v:.15e}"


def format_opencv_matrix_4x4(data: List[float]) -> str:
    if len(data) != 16:
        raise ValueError("4x4 matrix must have 16 numbers")
    return (
        f"   data: [{e(data[0])}, {e(data[1])}, {e(data[2])}, {e(data[3])},\n"
        f"          {e(data[4])}, {e(data[5])}, {e(data[6])}, {e(data[7])},\n"
        f"          {e(data[8])}, {e(data[9])}, {e(data[10])}, {e(data[11])},\n"
        f"          {e(data[12])}, {e(data[13])}, {e(data[14])}, {e(data[15])}]"
    )


def render_cam_yaml(cam: Dict[str, List[float]], width: int, height: int) -> str:
    fx, fy = cam["focal"]
    cx, cy = cam["principal"]
    k1, k2, k3, k4 = cam["dist"]
    return (
        "%YAML:1.0\n"
        "---\n"
        "model_type: KANNALA_BRANDT\n"
        "camera_name: camera\n"
        f"image_width: {width}\n"
        f"image_height: {height}\n"
        "projection_parameters:\n"
        f"   k2: {e(k1)}\n"
        f"   k3: {e(k2)}\n"
        f"   k4: {e(k3)}\n"
        f"   k5: {e(k4)}\n"
        f"   mu: {e(fx)}\n"
        f"   mv: {e(fy)}\n"
        f"   u0: {e(cx)}\n"
        f"   v0: {e(cy)}\n"
    )


def render_stereo_yaml(
    cam0_t_ic: List[float],
    cam1_t_ic: List[float],
    imu: Dict[str, float],
    td: float,
    width: int,
    height: int,
    cam0_yaml_name: str,
    cam1_yaml_name: str,
) -> str:
    return (
        "%YAML:1.0\n\n"
        "#common parameters\n"
        "#support: 1 imu 1 cam; 1 imu 2 cam: 2 cam;\n"
        "# Configuration generated from Kalibr calibration results\n"
        "imu: 1\n"
        "num_of_cam: 2\n\n"
        "imu_topic: \"/fays/atrak/imu\"\n"
        "image0_topic: \"/fays/atrak/cam0\"\n"
        "image1_topic: \"/fays/atrak/cam1\"\n"
        "output_path: \"~/output/\"\n\n"
        f"cam0_calib: \"{cam0_yaml_name}\"\n"
        f"cam1_calib: \"{cam1_yaml_name}\"\n"
        f"image_width: {width}\n"
        f"image_height: {height}\n\n"
        "\n"
        "# Extrinsic parameter between IMU and Camera.\n"
        "# From Kalibr: T_cam_imu (camera to IMU), converted to body_T_cam (IMU to camera)\n"
        "estimate_extrinsic: 0   # 0  Have an accurate extrinsic parameters. We will trust the following imu^R_cam, imu^T_cam, don't change it.\n"
        "                        # 1  Have an initial guess about extrinsic parameters. We will optimize around your initial guess.\n\n"
        "body_T_cam0: !!opencv-matrix\n"
        "   rows: 4\n"
        "   cols: 4\n"
        "   dt: d\n"
        f"{format_opencv_matrix_4x4(cam0_t_ic)}\n\n"
        "body_T_cam1: !!opencv-matrix\n"
        "   rows: 4\n"
        "   cols: 4\n"
        "   dt: d\n"
        f"{format_opencv_matrix_4x4(cam1_t_ic)}\n\n"
        "#Multiple thread support\n"
        "multiple_thread: 1\n\n"
        "#feature traker paprameters\n"
        "max_cnt: 120           # max feature number in feature tracking\n"
        "min_dist: 20            # min distance between two features\n"
        "freq: 10                # frequence (Hz) of publish tracking result. At least 10Hz for good estimation. If set 0, the frequence will be same as raw image\n"
        "F_threshold: 1.0        # ransac threshold (pixel)\n"
        "show_track: 1           # publish tracking image as topic\n"
        "flow_back: 1            # perform forward and backward optical flow to improve feature tracking accuracy\n"
        "equalize: 1 \n\n"
        "#optimization parameters\n"
        "max_solver_time: 0.04  # max solver itration time (ms), to guarantee real time\n"
        "max_num_iterations: 8   # max solver itrations, to guarantee real time\n"
        "keyframe_parallax: 10.0 # keyframe selection threshold (pixel)\n\n"
        "#imu parameters       The more accurate parameters you provide, the better performance\n"
        "# From Kalibr calibration results\n"
        f"acc_n: {imu['acc_n']}          # accelerometer measurement noise standard deviation (Kalibr discrete)\n"
        f"gyr_n: {imu['gyr_n']}          # gyroscope measurement noise standard deviation (Kalibr discrete)\n"
        f"acc_w: {imu['acc_w']}        # accelerometer bias random work noise standard deviation (Kalibr random walk)\n"
        f"gyr_w: {imu['gyr_w']}       # gyroscope bias random work noise standard deviation (Kalibr random walk)\n"
        "g_norm: 9.78     # gravity magnitude 9.81\n\n"
        "#unsynchronization parameters\n"
        "# VINS-Fusion: td = timeshift (t_cam + td = t_imu)\n"
        "estimate_td: 0                      # online estimate time offset between camera and imu\n"
        f"td: {td}          # initial value of time offset. unit: s. readed image clock + td = real image clock (IMU clock)\n\n"
        "#loop closure parameters\n"
        "load_previous_pose_graph: 0        # load and reuse previous pose graph; load from 'pose_graph_save_path'\n"
        "pose_graph_save_path: \"~/output/pose_graph/\" # save and load path\n"
        "save_image: 0                   # save image in pose graph for visualization prupose; you can close this function by setting 0\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert Kalibr txt result to VINS-Fusion yaml files")
    parser.add_argument(
        "--input",
        type=Path,
        default=SCRIPT_DIR / "fays_tmp_calib-results-imucam.txt",
        help="Path to Kalibr result txt file",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=SCRIPT_DIR,
        help="Output directory for generated yaml files",
    )
    parser.add_argument(
        "--stereo-out",
        default="StereoIMU-vinsfusion-converted.yaml",
        help="Output filename for stereo vins yaml",
    )
    parser.add_argument(
        "--cam0-out",
        default="cam0_equidistant-converted.yaml",
        help="Output filename for cam0 yaml",
    )
    parser.add_argument(
        "--cam1-out",
        default="cam1_equidistant-converted.yaml",
        help="Output filename for cam1 yaml",
    )
    parser.add_argument("--image-width", type=int, default=640, help="Image width used in output yaml")
    parser.add_argument("--image-height", type=int, default=400, help="Image height used in output yaml")
    parser.add_argument(
        "--td-source",
        choices=["cam0", "cam1"],
        default="cam0",
        help="Use timeshift of which camera for VINS td",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    text = args.input.read_text(encoding="utf-8")

    cam0_t_ic = parse_t_ic(text, "cam0")
    cam1_t_ic = parse_t_ic(text, "cam1")
    td = parse_timeshift(text, args.td_source)

    cam0 = parse_camera_block(text, "cam0")
    cam1 = parse_camera_block(text, "cam1")
    imu = parse_imu_params(text)

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    stereo_path = output_dir / args.stereo_out
    cam0_path = output_dir / args.cam0_out
    cam1_path = output_dir / args.cam1_out

    stereo_text = render_stereo_yaml(
        cam0_t_ic=cam0_t_ic,
        cam1_t_ic=cam1_t_ic,
        imu=imu,
        td=td,
        width=args.image_width,
        height=args.image_height,
        cam0_yaml_name=args.cam0_out,
        cam1_yaml_name=args.cam1_out,
    )

    cam0_text = render_cam_yaml(cam0, args.image_width, args.image_height)
    cam1_text = render_cam_yaml(cam1, args.image_width, args.image_height)

    stereo_path.write_text(stereo_text, encoding="utf-8")
    cam0_path.write_text(cam0_text, encoding="utf-8")
    cam1_path.write_text(cam1_text, encoding="utf-8")

    print(f"Generated: {stereo_path}")
    print(f"Generated: {cam0_path}")
    print(f"Generated: {cam1_path}")


if __name__ == "__main__":
    main()
    
# python3 fays_kalibr_to_vinsfusion.py --input fays_tmp_calib-results-imucam.txt
