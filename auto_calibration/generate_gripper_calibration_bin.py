#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import pathlib
import re
import sys


CALIBRATION_DATA_FORMAT_VERSION_V1 = 0x00010000
CALIBRATION_PAYLOAD_SIZE = 1024

VALID_RGB_CAMERA = 1 << 0
VALID_STEREO_CAM0 = 1 << 1
VALID_STEREO_CAM1 = 1 << 2
VALID_EXTRINSICS = 1 << 3
VALID_IMU0 = 1 << 4
VALID_RESIDUALS = 1 << 5

FLOAT_RE = r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?"


class GripperCalibrationHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_char * 4),
        ("dataFormatVersion", ctypes.c_uint32),
        ("payloadSize", ctypes.c_uint16),
        ("headerSize", ctypes.c_uint16),
        ("validFields", ctypes.c_uint32),
        ("reserved", ctypes.c_uint8 * 16),
    ]


class GripperRgbCameraBlock(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("cameraModelEnum", ctypes.c_uint32),
        ("distortionCoefficients", ctypes.c_float * 4),
        ("intrinsics", ctypes.c_float * 4),
        ("resolution", ctypes.c_float * 2),
        ("reserved", ctypes.c_uint8 * 4),
    ]


class GripperStereoCameraBlock(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("cameraModelEnum", ctypes.c_uint32),
        ("focalLength", ctypes.c_float * 2),
        ("principalPoint", ctypes.c_float * 2),
        ("distortionCoefficients", ctypes.c_float * 4),
        ("reserved", ctypes.c_float * 3),
    ]


class GripperExtrinsicsBlock(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("tIcCam0ToImu0", ctypes.c_float * 16),
        ("timeshiftCam0ToImu0", ctypes.c_float),
        ("reserved0", ctypes.c_float * 3),
        ("tIcCam1ToImu0", ctypes.c_float * 16),
        ("timeshiftCam1ToImu0", ctypes.c_float),
        ("baselineNorm", ctypes.c_float),
        ("reserved1", ctypes.c_float * 2),
    ]


class GripperImuBlock(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("updateRate", ctypes.c_float),
        ("accelerometerNoiseDensityDiscrete", ctypes.c_float),
        ("accelerometerRandomWalk", ctypes.c_float),
        ("gyroscopeNoiseDensityDiscrete", ctypes.c_float),
        ("gyroscopeRandomWalk", ctypes.c_float),
        ("reserved", ctypes.c_float * 3),
    ]


class GripperStatisticsBlock(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("mean", ctypes.c_float),
        ("median", ctypes.c_float),
        ("stddev", ctypes.c_float),
        ("reserved", ctypes.c_float),
    ]


class GripperResidualsBlock(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("reprojectionErrorCam0Px", GripperStatisticsBlock),
        ("reprojectionErrorCam1Px", GripperStatisticsBlock),
        ("gyroscopeErrorImu0RadS", GripperStatisticsBlock),
        ("accelerometerErrorImu0MS2", GripperStatisticsBlock),
    ]


class GripperCalibrationDataV1(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("header", GripperCalibrationHeader),
        ("rgbCamera", GripperRgbCameraBlock),
        ("stereoCam0", GripperStereoCameraBlock),
        ("stereoCam1", GripperStereoCameraBlock),
        ("extrinsics", GripperExtrinsicsBlock),
        ("imu0", GripperImuBlock),
        ("residuals", GripperResidualsBlock),
        ("reserved", ctypes.c_uint8 * 592),
    ]


assert ctypes.sizeof(GripperCalibrationDataV1) == CALIBRATION_PAYLOAD_SIZE


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def parse_inline_float_list(text: str, key: str, expected_count: int) -> list[float]:
    match = re.search(rf"^\s*{re.escape(key)}:\s*\[([^\]]+)\]", text, re.M)
    if not match:
        raise ValueError(f"missing list for {key}")
    values = [float(item.strip()) for item in match.group(1).split(",") if item.strip()]
    if len(values) != expected_count:
        raise ValueError(f"{key} expects {expected_count} values, got {len(values)}")
    return values


def parse_float_scalar(text: str, key: str) -> float:
    match = re.search(rf"^\s*{re.escape(key)}:\s*({FLOAT_RE})\s*$", text, re.M)
    if not match:
        raise ValueError(f"missing scalar for {key}")
    return float(match.group(1))


def parse_optional_uint_scalar(text: str, key: str) -> int | None:
    match = re.search(rf"^\s*{re.escape(key)}:\s*(0x[0-9A-Fa-f]+|\d+)\s*$", text, re.M)
    if not match:
        return None
    return int(match.group(1), 0)


def parse_named_block(text: str, block_name: str) -> str:
    match = re.search(
        rf"^\s*{re.escape(block_name)}:\s*$([\s\S]*?)(?=^\S|\Z)",
        text,
        re.M,
    )
    if not match:
        raise ValueError(f"missing block {block_name}")
    return match.group(1)


def parse_matrix_block(text: str, key: str) -> list[float]:
    match = re.search(
        rf"^\s*{re.escape(key)}:\s*$\n((?:\s*-\s*\[[^\]]+\]\s*\n?){{4}})",
        text,
        re.M,
    )
    if not match:
        raise ValueError(f"missing matrix block for {key}")
    rows = re.findall(r"\[([^\]]+)\]", match.group(1))
    if len(rows) != 4:
        raise ValueError(f"{key} expects 4 rows, got {len(rows)}")
    values: list[float] = []
    for row in rows:
        parts = [float(item.strip()) for item in row.split(",") if item.strip()]
        if len(parts) != 4:
            raise ValueError(f"{key} expects 4 columns, got {len(parts)}")
        values.extend(parts)
    return values


def fill_float_array(target, values: list[float]) -> None:
    if len(target) != len(values):
        raise ValueError(f"array size mismatch: {len(target)} != {len(values)}")
    for index, value in enumerate(values):
        target[index] = float(value)


def parse_statistics_block(text: str, block_name: str) -> tuple[float, float, float]:
    block = parse_named_block(text, block_name)
    mean = parse_float_scalar(block, "mean")
    median = parse_float_scalar(block, "median")
    std = parse_float_scalar(block, "std")
    return mean, median, std


def fill_statistics(target: GripperStatisticsBlock, values: tuple[float, float, float]) -> None:
    target.mean = values[0]
    target.median = values[1]
    target.stddev = values[2]
    target.reserved = 0.0


def trim_trailing_zero_length(payload: bytes) -> int:
    used = len(payload)
    while used > 0 and payload[used - 1] == 0:
        used -= 1
    return used


def finalize_payload(payload: GripperCalibrationDataV1) -> bytes:
    payload.header.payloadSize = trim_trailing_zero_length(bytes(payload))
    return bytes(payload)


def build_payload_template() -> GripperCalibrationDataV1:
    payload = GripperCalibrationDataV1()
    payload.header.magic = b"UCAL"
    payload.header.dataFormatVersion = CALIBRATION_DATA_FORMAT_VERSION_V1
    payload.header.payloadSize = ctypes.sizeof(GripperCalibrationHeader)
    payload.header.headerSize = ctypes.sizeof(GripperCalibrationHeader)
    payload.header.validFields = (
        VALID_RGB_CAMERA
        | VALID_STEREO_CAM0
        | VALID_STEREO_CAM1
        | VALID_EXTRINSICS
        | VALID_IMU0
        | VALID_RESIDUALS
    )
    return payload


def camera_model_enum_from_strings(camera_model: str | None, distortion_model: str | None) -> int:
    camera_model = (camera_model or "").strip().lower()
    distortion_model = (distortion_model or "").strip().lower()
    if camera_model == "pinhole" and distortion_model == "equidistant":
        return 1
    return 1


def parse_summary_to_payload(summary_path: pathlib.Path) -> bytes:
    text = read_text(summary_path)
    payload = build_payload_template()
    explicit_version = (
        parse_optional_uint_scalar(text, "calibration_data_format_version")
        or parse_optional_uint_scalar(text, "data_format_version")
    )
    if explicit_version is not None:
        payload.header.dataFormatVersion = explicit_version

    match = re.search(r"^\s*camera_model_enum:\s*(\d+)\s*$", text, re.M)
    if not match:
        raise ValueError("missing camera_model_enum")
    camera_model_enum = int(match.group(1))

    payload.rgbCamera.cameraModelEnum = camera_model_enum
    fill_float_array(payload.rgbCamera.distortionCoefficients, parse_inline_float_list(text, "distortion_coeffs", 4))
    fill_float_array(payload.rgbCamera.intrinsics, parse_inline_float_list(text, "intrinsics", 4))
    fill_float_array(payload.rgbCamera.resolution, parse_inline_float_list(text, "resolution", 2))

    cam0_block = parse_named_block(text, "cam0")
    payload.stereoCam0.cameraModelEnum = int(parse_float_scalar(cam0_block, "target_camera_model_enum"))
    fill_float_array(payload.stereoCam0.focalLength, parse_inline_float_list(cam0_block, "focal_length", 2))
    fill_float_array(payload.stereoCam0.principalPoint, parse_inline_float_list(cam0_block, "principal_point", 2))
    fill_float_array(payload.stereoCam0.distortionCoefficients, parse_inline_float_list(cam0_block, "distortion_coefficients", 4))

    cam1_block = parse_named_block(text, "cam1")
    payload.stereoCam1.cameraModelEnum = int(parse_float_scalar(cam1_block, "target_camera_model_enum"))
    fill_float_array(payload.stereoCam1.focalLength, parse_inline_float_list(cam1_block, "focal_length", 2))
    fill_float_array(payload.stereoCam1.principalPoint, parse_inline_float_list(cam1_block, "principal_point", 2))
    fill_float_array(payload.stereoCam1.distortionCoefficients, parse_inline_float_list(cam1_block, "distortion_coefficients", 4))

    fill_float_array(payload.extrinsics.tIcCam0ToImu0, parse_matrix_block(text, "T_ic_cam0_to_imu0"))
    fill_float_array(payload.extrinsics.tIcCam1ToImu0, parse_matrix_block(text, "T_ic_cam1_to_imu0"))
    payload.extrinsics.timeshiftCam0ToImu0 = parse_float_scalar(text, "timeshift_cam0_to_imu0")
    try:
        payload.extrinsics.timeshiftCam1ToImu0 = parse_float_scalar(text, "timeshift_cam1_to_imu0")
    except ValueError:
        payload.extrinsics.timeshiftCam1ToImu0 = 0.0
    payload.extrinsics.baselineNorm = parse_float_scalar(text, "baseline_norm")

    imu0_block = parse_named_block(text, "imu0")
    payload.imu0.updateRate = parse_float_scalar(imu0_block, "update_rate")
    accel_block = parse_named_block(imu0_block, "accelerometer")
    gyro_block = parse_named_block(imu0_block, "gyroscope")
    payload.imu0.accelerometerNoiseDensityDiscrete = parse_float_scalar(accel_block, "noise_density_discrete")
    payload.imu0.accelerometerRandomWalk = parse_float_scalar(accel_block, "random_walk")
    payload.imu0.gyroscopeNoiseDensityDiscrete = parse_float_scalar(gyro_block, "noise_density_discrete")
    payload.imu0.gyroscopeRandomWalk = parse_float_scalar(gyro_block, "random_walk")

    residuals_block = parse_named_block(text, "residuals")
    fill_statistics(payload.residuals.reprojectionErrorCam0Px, parse_statistics_block(residuals_block, "reprojection_error_cam0_px"))
    fill_statistics(payload.residuals.reprojectionErrorCam1Px, parse_statistics_block(residuals_block, "reprojection_error_cam1_px"))
    fill_statistics(payload.residuals.gyroscopeErrorImu0RadS, parse_statistics_block(residuals_block, "gyroscope_error_imu0_rad_s"))
    fill_statistics(payload.residuals.accelerometerErrorImu0MS2, parse_statistics_block(residuals_block, "accelerometer_error_imu0_m_s2"))

    return finalize_payload(payload)


def parse_optional_yaml_block(text: str, block_name: str) -> str | None:
    match = re.search(
        rf"^\s*{re.escape(block_name)}:\s*$([\s\S]*?)(?=^\S|\Z)",
        text,
        re.M,
    )
    if not match:
        return None
    return match.group(1)


def parse_camchain_text(camchain_text: str) -> dict[str, list[float] | str | int]:
    block = parse_optional_yaml_block(camchain_text, "cam0") or camchain_text
    camera_model = re.search(r"^\s*camera_model:\s*(.+?)\s*$", block, re.M)
    distortion_model = re.search(r"^\s*distortion_model:\s*(.+?)\s*$", block, re.M)
    intrinsics = parse_inline_float_list(block, "intrinsics", 4)
    distortion_coeffs = parse_inline_float_list(block, "distortion_coeffs", 4)
    resolution = parse_inline_float_list(block, "resolution", 2)
    rostopic_match = re.search(r"^\s*rostopic:\s*(.+?)\s*$", block, re.M)

    return {
        "camera_model": camera_model.group(1).strip() if camera_model else "pinhole",
        "distortion_model": distortion_model.group(1).strip() if distortion_model else "equidistant",
        "intrinsics": intrinsics,
        "distortion_coeffs": distortion_coeffs,
        "resolution": resolution,
        "rostopic": rostopic_match.group(1).strip() if rostopic_match else "",
    }


def parse_section_between(text: str, start_pattern: str, end_pattern: str | None) -> str:
    pattern = start_pattern
    if end_pattern is None:
        pattern += r"([\s\S]*?)\Z"
    else:
        pattern += rf"([\s\S]*?){end_pattern}"
    match = re.search(pattern, text, re.M)
    if not match:
        raise ValueError(f"missing section for pattern: {start_pattern}")
    return match.group(1)


def parse_matrix_from_section(section: str, label: str) -> list[float]:
    match = re.search(rf"{re.escape(label)}\s*\n(\[\[.*?\]\])", section, re.S)
    if not match:
        raise ValueError(f"missing matrix for {label}")
    values = [float(item) for item in re.findall(FLOAT_RE, match.group(1))]
    if len(values) != 16:
        raise ValueError(f"{label} expects 16 values, got {len(values)}")
    return values


def parse_stats_line(text: str, label: str) -> tuple[float, float, float]:
    match = re.search(
        rf"{re.escape(label)}\s*mean\s+({FLOAT_RE}),\s*median\s+({FLOAT_RE}),\s*std:\s*({FLOAT_RE})",
        text,
    )
    if not match:
        raise ValueError(f"missing stats line for {label}")
    return float(match.group(1)), float(match.group(2)), float(match.group(3))


def parse_float_after_label(text: str, label: str) -> float:
    match = re.search(rf"{re.escape(label)}\s*({FLOAT_RE})", text)
    if not match:
        raise ValueError(f"missing value for {label}")
    return float(match.group(1))


def parse_raw_pair_to_payload(camchain_path: pathlib.Path, imucam_path: pathlib.Path) -> bytes:
    camchain_text = read_text(camchain_path)
    imucam_text = read_text(imucam_path)
    payload = build_payload_template()

    rgb = parse_camchain_text(camchain_text)
    rgb_enum = camera_model_enum_from_strings(
        str(rgb["camera_model"]),
        str(rgb["distortion_model"]),
    )
    payload.rgbCamera.cameraModelEnum = rgb_enum
    fill_float_array(payload.rgbCamera.distortionCoefficients, list(rgb["distortion_coeffs"]))  # type: ignore[arg-type]
    fill_float_array(payload.rgbCamera.intrinsics, list(rgb["intrinsics"]))  # type: ignore[arg-type]
    fill_float_array(payload.rgbCamera.resolution, list(rgb["resolution"]))  # type: ignore[arg-type]

    residuals_section = parse_section_between(imucam_text, r"Residuals\n-+\n", r"\n\nTransformation \(cam0\):")
    fill_statistics(
        payload.residuals.reprojectionErrorCam0Px,
        parse_stats_line(residuals_section, "Reprojection error (cam0) [px]:"),
    )
    fill_statistics(
        payload.residuals.reprojectionErrorCam1Px,
        parse_stats_line(residuals_section, "Reprojection error (cam1) [px]:"),
    )
    fill_statistics(
        payload.residuals.gyroscopeErrorImu0RadS,
        parse_stats_line(residuals_section, "Gyroscope error (imu0) [rad/s]:"),
    )
    fill_statistics(
        payload.residuals.accelerometerErrorImu0MS2,
        parse_stats_line(residuals_section, "Accelerometer error (imu0) [m/s^2]:"),
    )

    transform_cam0 = parse_section_between(imucam_text, r"Transformation \(cam0\):\n-+\n", r"\n\nTransformation \(cam1\):")
    transform_cam1 = parse_section_between(imucam_text, r"Transformation \(cam1\):\n-+\n", r"\n\nBaselines:")
    fill_float_array(payload.extrinsics.tIcCam0ToImu0, parse_matrix_from_section(transform_cam0, "T_ic:  (cam0 to imu0):"))
    fill_float_array(payload.extrinsics.tIcCam1ToImu0, parse_matrix_from_section(transform_cam1, "T_ic:  (cam1 to imu0):"))
    payload.extrinsics.timeshiftCam0ToImu0 = parse_float_after_label(transform_cam0, "timeshift cam0 to imu0: [s] (t_imu = t_cam + shift)")
    payload.extrinsics.timeshiftCam1ToImu0 = parse_float_after_label(transform_cam1, "timeshift cam1 to imu0: [s] (t_imu = t_cam + shift)")

    baselines_section = parse_section_between(imucam_text, r"Baselines:\n-+\n", r"\n\nGravity vector in target coords:")
    payload.extrinsics.baselineNorm = parse_float_after_label(baselines_section, "baseline norm:")

    cam0_section = parse_section_between(imucam_text, r"\ncam0\n-----\n", r"\n\ncam1\n-----\n")
    cam1_section = parse_section_between(imucam_text, r"\ncam1\n-----\n", r"\n\n\nIMU configuration")
    payload.stereoCam0.cameraModelEnum = camera_model_enum_from_strings(
        re.search(r"Camera model:\s*(.+?)\s*$", cam0_section, re.M).group(1),  # type: ignore[union-attr]
        re.search(r"Distortion model:\s*(.+?)\s*$", cam0_section, re.M).group(1),  # type: ignore[union-attr]
    )
    payload.stereoCam1.cameraModelEnum = camera_model_enum_from_strings(
        re.search(r"Camera model:\s*(.+?)\s*$", cam1_section, re.M).group(1),  # type: ignore[union-attr]
        re.search(r"Distortion model:\s*(.+?)\s*$", cam1_section, re.M).group(1),  # type: ignore[union-attr]
    )
    fill_float_array(payload.stereoCam0.focalLength, [float(v) for v in re.findall(FLOAT_RE, re.search(r"Focal length:\s*\[([^\]]+)\]", cam0_section).group(1))])  # type: ignore[union-attr]
    fill_float_array(payload.stereoCam0.principalPoint, [float(v) for v in re.findall(FLOAT_RE, re.search(r"Principal point:\s*\[([^\]]+)\]", cam0_section).group(1))])  # type: ignore[union-attr]
    fill_float_array(payload.stereoCam0.distortionCoefficients, [float(v) for v in re.findall(FLOAT_RE, re.search(r"Distortion coefficients:\s*\[([^\]]+)\]", cam0_section).group(1))])  # type: ignore[union-attr]
    fill_float_array(payload.stereoCam1.focalLength, [float(v) for v in re.findall(FLOAT_RE, re.search(r"Focal length:\s*\[([^\]]+)\]", cam1_section).group(1))])  # type: ignore[union-attr]
    fill_float_array(payload.stereoCam1.principalPoint, [float(v) for v in re.findall(FLOAT_RE, re.search(r"Principal point:\s*\[([^\]]+)\]", cam1_section).group(1))])  # type: ignore[union-attr]
    fill_float_array(payload.stereoCam1.distortionCoefficients, [float(v) for v in re.findall(FLOAT_RE, re.search(r"Distortion coefficients:\s*\[([^\]]+)\]", cam1_section).group(1))])  # type: ignore[union-attr]

    imu_section = parse_section_between(imucam_text, r"IMU0:\n\s*-+\n", None)
    payload.imu0.updateRate = parse_float_after_label(imu_section, "Update rate:")
    payload.imu0.accelerometerNoiseDensityDiscrete = parse_float_after_label(imu_section, "Noise density (discrete):")
    accel_random_walk_match = re.search(
        rf"Accelerometer:\s*\n(?:.*\n)*?\s+Random walk:\s*({FLOAT_RE})",
        imu_section,
        re.M,
    )
    gyro_discrete_match = re.search(
        rf"Gyroscope:\s*\n(?:.*\n)*?\s+Noise density \(discrete\):\s*({FLOAT_RE})",
        imu_section,
        re.M,
    )
    gyro_random_walk_match = re.search(
        rf"Gyroscope:\s*\n(?:.*\n)*?\s+Random walk:\s*({FLOAT_RE})",
        imu_section,
        re.M,
    )
    if not accel_random_walk_match or not gyro_discrete_match or not gyro_random_walk_match:
        raise ValueError("missing IMU random walk / gyroscope discrete noise values")
    payload.imu0.accelerometerRandomWalk = float(accel_random_walk_match.group(1))
    payload.imu0.gyroscopeNoiseDensityDiscrete = float(gyro_discrete_match.group(1))
    payload.imu0.gyroscopeRandomWalk = float(gyro_random_walk_match.group(1))

    return finalize_payload(payload)


def detect_raw_pair_from_dir(input_dir: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    camchains = sorted(
        path for path in input_dir.iterdir()
        if path.is_file() and path.suffix.lower() in {".yaml", ".yml"} and "camchain" in path.name.lower()
    )
    imucams = sorted(
        path for path in input_dir.iterdir()
        if path.is_file() and path.name.lower() == "output-results-imucam.txt"
    )
    if len(camchains) != 1 or len(imucams) != 1:
        raise ValueError(f"expected exactly one camchain and one output-results-imucam.txt in {input_dir}")
    return camchains[0], imucams[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate 1024-byte gripper calibration payload.")
    parser.add_argument("--input", help="Summary markdown, raw calibration directory, or preselected source path")
    parser.add_argument("--camchain", help="Raw rgb_video_ros-camchain.yaml path")
    parser.add_argument("--imucam", help="Raw output-results-imucam.txt path")
    parser.add_argument("--output-bin", required=True, help="Output 1024-byte binary payload path")
    args = parser.parse_args()

    output_path = pathlib.Path(args.output_bin)

    payload: bytes
    if args.camchain or args.imucam:
        if not args.camchain or not args.imucam:
            raise ValueError("--camchain and --imucam must be provided together")
        payload = parse_raw_pair_to_payload(pathlib.Path(args.camchain), pathlib.Path(args.imucam))
    elif args.input:
        input_path = pathlib.Path(args.input)
        if input_path.is_dir():
            camchain_path, imucam_path = detect_raw_pair_from_dir(input_path)
            payload = parse_raw_pair_to_payload(camchain_path, imucam_path)
        elif input_path.suffix.lower() == ".md":
            payload = parse_summary_to_payload(input_path)
        else:
            raise ValueError(f"unsupported input source: {input_path}")
    else:
        raise ValueError("one of --input or (--camchain and --imucam) is required")

    if len(payload) != CALIBRATION_PAYLOAD_SIZE:
        raise ValueError(f"payload size mismatch: {len(payload)} != {CALIBRATION_PAYLOAD_SIZE}")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(payload)
    print(output_path)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover
        print(f"[generate-gripper-calib] {exc}", file=sys.stderr)
        raise
