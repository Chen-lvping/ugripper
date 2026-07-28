#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path
from statistics import median
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
MCAP_FALLBACK_ROOTS = [
    PROJECT_ROOT / "build" / "_deps" / "mcap-src" / "python" / "mcap",
]

BASE_VIDEO_FILES = [
    ("left_cam_main", "cam_left.mkv", "left"),
    ("right_cam_main", "cam_right.mkv", "right"),
    ("left_stereo", "stereo_left.mkv", "left"),
    ("right_stereo", "stereo_right.mkv", "right"),
    ("left_tcam_l", "tcam_left_l.mkv", "left"),
    ("left_tcam_r", "tcam_left_r.mkv", "left"),
    ("right_tcam_l", "tcam_right_l.mkv", "right"),
    ("right_tcam_r", "tcam_right_r.mkv", "right"),
]
CHEST_VIDEO_FILE = ("chest_cam_main", "cam_chest.mkv", "chest")
MAIN_CAMERA_NAMES = {"cam_left_main", "cam_right_main", "cam_chest_main"}
STEREO_CAMERA_NAMES = {"stereo_left", "stereo_right"}
TACTILE_CAMERA_NAMES = {"tcam_left_l", "tcam_left_r", "tcam_right_l", "tcam_right_r"}
SENSOR_FILES = [
    ("left", "sensor_left.mcap", ["encoder_left"]),
    ("right", "sensor_right.mcap", ["encoder_right"]),
]
FAYS_MCAP_FILES = [
    ("left", "fays_data_left.mcap", "left_stereo"),
    ("right", "fays_data_right.mcap", "right_stereo"),
]
MCAP_MAGIC = b"\x89MCAP0\r\n"
FAYS_IMU_PAYLOAD_BYTES = 48
FAYS_CAMERA_PAYLOAD_BYTES = 4
EXPECTED_METADATA_KEY_ORDER = [
    "device_sn",
    "device_type",
    "device_mode",
    "camera_codec",
    "hardware_version",
    "software_version",
    "das_usb_updater_version",
    "data_version",
    "hardware_list",
    "episode_name",
    "data_uuid",
    "audio_uuid",
    "quality_check_status",
    "quality_check_err_type",
    "collection_duration_s",
    "task_start_s",
    "task_stop_s",
    "require_files",
    "video_details",
]
OPTIONAL_METADATA_KEYS = {"task_start_s", "task_stop_s"}
REQUIRED_METADATA_FIELDS = [
    "device_sn",
    "device_type",
    "device_mode",
    "camera_codec",
    "hardware_version",
    "software_version",
    "das_usb_updater_version",
    "data_version",
    "hardware_list",
    "episode_name",
    "data_uuid",
    "quality_check_status",
    "collection_duration_s",
    "require_files",
    "video_details",
]
EXPECTED_HARDWARE_LIST_KEY_ORDER = [
    "gripper_right_sn",
    "gripper_left_sn",
    "cam_right_sn",
    "cam_left_sn",
    "cam_chest_sn",
    "tactile_right_l_sn",
    "tactile_right_r_sn",
    "tactile_left_l_sn",
    "tactile_left_r_sn",
    "stereo_right_sn",
    "stereo_left_sn",
]
REQUIRED_HARDWARE_LIST_KEYS = set(EXPECTED_HARDWARE_LIST_KEY_ORDER)
EXPECTED_METADATA_KEYS = set(EXPECTED_METADATA_KEY_ORDER)
EXPECTED_VIDEO_DETAIL_KEY_ORDER = ["name", "fps", "duration_s", "start_offset_us"]
EXPECTED_METADATA_VIDEO_DETAIL_NAMES = [
    "cam_left.mkv",
    "tcam_left_l.mkv",
    "tcam_left_r.mkv",
    "stereo_left.mkv",
    "cam_right.mkv",
    "tcam_right_l.mkv",
    "tcam_right_r.mkv",
    "stereo_right.mkv",
    "cam_chest.mkv",
]
EXPECTED_CALIBRATION_KEY_ORDER = ["calibration_info", "observation"]
EXPECTED_CALIBRATION_INFO_KEY_ORDER = ["calibration_status", "format_version"]
EXPECTED_CALIBRATION_OBSERVATION_KEY_ORDER = ["images", "imu"]
EXPECTED_CALIBRATION_ROOT_KEYS = set(EXPECTED_CALIBRATION_KEY_ORDER)
EXPECTED_CALIBRATION_OBSERVATION_KEYS = {"images", "imu"}
REQUIRED_CALIBRATION_ROOT_KEYS = list(EXPECTED_CALIBRATION_KEY_ORDER)
EXPECTED_CALIBRATION_IMAGE_KEY_ORDER = [
    "cam_chest_main",
    "stereo_left",
    "tcam_left_l",
    "tcam_left_r",
    "stereo_right",
    "tcam_right_l",
    "tcam_right_r",
    "cam_left_main",
    "cam_right_main",
]
REQUIRED_CALIBRATION_IMU_KEYS = ["imu_left", "imu_right"]
MAIN_CAMERA_CALIBRATION_KEY_ORDER = [
    "camera_model",
    "distortion_coeffs",
    "distortion_model",
    "fps",
    "intrinsics",
    "names",
    "shape",
]
STEREO_CALIBRATION_KEY_ORDER = [
    "cam0",
    "cam1",
    "camera_model",
    "distortion_model",
    "extrinsics",
    "fps",
    "names",
    "shape",
]
STEREO_CAMERA_NODE_KEY_ORDER = ["distortion_coeffs", "intrinsics"]
TACTILE_CALIBRATION_KEY_ORDER = ["names", "serial", "shape"]
IMU_CALIBRATION_KEYS = {"update_rate_hz", "accelerometer", "gyroscope"}
CORE_REQUIRED_FILES = [
    "metadata.json",
    "calibration.json",
    *(
        file_name
        for camera_name, file_name, _ in BASE_VIDEO_FILES
        if camera_name not in {"left_stereo", "right_stereo"}
    ),
    *(file_name for _, file_name, _ in SENSOR_FILES),
]
STEREO_REQUIRED_FILES = [
    "stereo_left.mkv",
    "stereo_right.mkv",
    "fays_data_left.mcap",
    "fays_data_right.mcap",
]


def us_to_ms(value_us: float) -> float:
    return value_us / 1000.0


def ns_to_ms(value_ns: float) -> float:
    return value_ns / 1_000_000.0


def sec_to_us(value_sec: float) -> int:
    return int(round(value_sec * 1_000_000.0))


def safe_float(value: Any) -> float | None:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip()
    if not text or text == "N/A":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def safe_int(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            return None
        return int(round(value))
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text)
    except ValueError:
        try:
            return int(round(float(text)))
        except ValueError:
            return None


def parse_rate(rate_text: str | None) -> float | None:
    if rate_text is None:
        return None
    text = rate_text.strip()
    if not text or text == "0/0":
        return None
    if "/" in text:
        left, right = text.split("/", 1)
        try:
            num = float(left)
            den = float(right)
        except ValueError:
            return None
        if den == 0:
            return None
        return num / den
    return safe_float(text)


def safe_str(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, str):
        text = value.strip()
        return text or None
    return str(value)


def value_type_name(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "int"
    if isinstance(value, float):
        return "float"
    if isinstance(value, str):
        return "str"
    if isinstance(value, list):
        return "list"
    if isinstance(value, dict):
        return "dict"
    return type(value).__name__


def percentile(sorted_values: list[float], p: float) -> float:
    if not sorted_values:
        return 0.0
    index = int(round((len(sorted_values) - 1) * p))
    index = max(0, min(index, len(sorted_values) - 1))
    return sorted_values[index]


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        check=False,
    )


def ensure_ffprobe() -> None:
    if shutil.which("ffprobe") is None:
        raise SystemExit("missing required binary: ffprobe")


def import_mcap_reader():
    try:
        from mcap.stream_reader import StreamReader  # type: ignore

        return StreamReader
    except Exception:
        pass

    for root in MCAP_FALLBACK_ROOTS:
        root_text = str(root)
        if root.exists() and root_text not in sys.path:
            sys.path.insert(0, root_text)
        try:
            from mcap.stream_reader import StreamReader  # type: ignore

            return StreamReader
        except Exception:
            continue
    return None


StreamReader = import_mcap_reader()


@dataclass
class Finding:
    severity: str
    code: str
    summary: str
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class PacketGap:
    gap_ms: float
    prev_pts_sec: float
    curr_pts_sec: float
    flags: str


@dataclass
class VideoStats:
    name: str
    side: str
    path: str
    codec: str | None = None
    duration_sec: float | None = None
    avg_fps: float | None = None
    packet_count: int = 0
    first_pts_sec: float | None = None
    last_pts_sec: float | None = None
    first_system_time_us: int | None = None
    last_system_time_us: int | None = None
    duplicate_pts_count: int = 0
    backward_pts_count: int = 0
    max_gap_ms: float = 0.0
    median_delta_ms: float = 0.0
    p99_delta_ms: float = 0.0
    large_gap_count: int = 0
    largest_gaps: list[PacketGap] = field(default_factory=list)
    error: str | None = None


@dataclass
class SensorTopicStats:
    topic: str
    source_file: str
    count: int = 0
    first_log_time_ns: int | None = None
    last_log_time_ns: int | None = None
    median_gap_ms: float = 0.0
    p99_gap_ms: float = 0.0
    max_gap_ms: float = 0.0
    large_gap_count: int = 0
    gap_threshold_ms: float = 0.0


@dataclass
class FaysMcapStats:
    side: str
    source_file: str
    imu_count: int = 0
    camera_count: int = 0
    first_log_time_ns: int | None = None
    last_log_time_ns: int | None = None
    span_sec: float = 0.0
    first_camera_log_time_ns: int | None = None
    last_camera_log_time_ns: int | None = None
    first_camera_publish_time_ns: int | None = None
    last_camera_publish_time_ns: int | None = None
    camera_publish_span_sec: float = 0.0
    max_camera_gap_ms: float = 0.0
    camera_gap_count: int = 0
    camera_rollback_count: int = 0
    estimated_missing_frames: int = 0
    first_frame_index: int | None = None
    last_frame_index: int | None = None
    frame_index_error_count: int = 0


@dataclass
class Report:
    episode_dir: str
    status: str
    findings: list[Finding]
    videos: dict[str, VideoStats]
    sensors: dict[str, SensorTopicStats]
    fays_mcaps: dict[str, FaysMcapStats]
    artifacts: dict[str, Any]
    summary: dict[str, Any]


def list_episode_dirs(root: Path) -> list[Path]:
    if not root.exists() or not root.is_dir():
        return []
    return sorted(
        [path for path in root.iterdir() if path.is_dir() and path.name.startswith("episode_")],
        key=lambda path: (path.name, path.stat().st_mtime_ns),
    )


def resolve_episode_dir(input_path: str, latest: bool) -> Path:
    path = Path(input_path).resolve()
    if not path.exists():
        raise SystemExit(f"path not found: {path}")

    if path.is_file():
        raise SystemExit(f"path is not a directory: {path}")

    if path.name.startswith("episode_"):
        return path

    candidates = list_episode_dirs(path)
    if not candidates:
        raise SystemExit(f"no episode_* directories found under: {path}")

    if latest:
        return candidates[-1]

    if len(candidates) == 1:
        return candidates[0]

    candidate_preview = "\n".join(f"- {candidate}" for candidate in candidates[-5:])
    raise SystemExit(
        "input is a parent directory with multiple episode_* children; "
        "pass --latest to auto-select the newest one, or pass the episode path directly.\n"
        f"recent candidates:\n{candidate_preview}"
    )


class EpisodeValidator:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.episode_dir = resolve_episode_dir(args.path, args.latest)
        self.findings: list[Finding] = []
        self.metadata: dict[str, Any] = {}
        self.calibration: dict[str, Any] = {}
        self.video_start_offsets_us: dict[str, int] = {}
        self.video_stats: dict[str, VideoStats] = {}
        self.sensor_stats: dict[str, SensorTopicStats] = {}
        self.fays_mcap_stats: dict[str, FaysMcapStats] = {}
        self.artifacts: dict[str, Any] = {}
        self.stereo_enabled = True
        self.active_video_files: list[tuple[str, str, str]] = list(BASE_VIDEO_FILES)

    def add_finding(self, severity: str, code: str, summary: str, **details: Any) -> None:
        self.findings.append(Finding(severity=severity, code=code, summary=summary, details=details))

    def severity_rank(self, severity: str) -> int:
        return {"FAIL": 3, "WARN": 2, "INFO": 1}.get(severity, 0)

    def validate(self) -> Report:
        self.check_episode_dir()
        self.check_optional_artifacts()
        self.load_metadata()
        self.load_calibration()
        self.resolve_episode_profile()
        self.resolve_active_video_files()
        self.check_required_files()
        self.check_dynamic_required_files()
        self.validate_metadata_fields()
        self.validate_calibration_structure()
        self.scan_videos()
        self.scan_main_camera_pair()
        self.scan_sensors()
        self.scan_fays_mcaps()
        self.check_video_span_consistency()
        self.check_video_alignment()
        self.check_pair_alignment()
        self.check_first_frame_sync()
        self.check_stereo_session_consistency()
        self.check_sensor_alignment()
        self.check_sensor_coverage()
        self.check_video_sensor_alignment()
        self.check_video_sensor_overlap()
        self.classify_patterns()

        status = "PASS"
        if any(f.severity == "FAIL" for f in self.findings):
            status = "FAIL"
        elif any(f.severity == "WARN" for f in self.findings):
            status = "WARN"

        self.findings.sort(key=lambda item: (-self.severity_rank(item.severity), item.code, item.summary))
        summary = {
            "finding_counts": {
                "fail": sum(1 for item in self.findings if item.severity == "FAIL"),
                "warn": sum(1 for item in self.findings if item.severity == "WARN"),
                "info": sum(1 for item in self.findings if item.severity == "INFO"),
            },
            "video_count": len(self.video_stats),
            "sensor_topic_count": len(self.sensor_stats),
            "fays_mcap_count": len(self.fays_mcap_stats),
            "artifact_count": len(self.artifacts),
        }
        return Report(
            episode_dir=str(self.episode_dir),
            status=status,
            findings=self.findings,
            videos=self.video_stats,
            sensors=self.sensor_stats,
            fays_mcaps=self.fays_mcap_stats,
            artifacts=self.artifacts,
            summary=summary,
        )

    def check_episode_dir(self) -> None:
        if not self.episode_dir.exists():
            raise SystemExit(f"episode dir not found: {self.episode_dir}")
        if not self.episode_dir.is_dir():
            raise SystemExit(f"episode path is not a directory: {self.episode_dir}")

    def check_required_files(self) -> None:
        required_files = list(CORE_REQUIRED_FILES)
        if self.stereo_enabled:
            required_files.extend(STEREO_REQUIRED_FILES)
        for name in required_files:
            path = self.episode_dir / name
            if not path.exists():
                self.add_finding("FAIL", "missing_file", f"缺少关键文件: {name}", path=str(path))
                continue
            if path.stat().st_size <= 0:
                self.add_finding("FAIL", "empty_file", f"关键文件为空: {name}", path=str(path))

    def chest_camera_enabled(self) -> bool:
        chest_file = self.episode_dir / CHEST_VIDEO_FILE[1]
        images = ((self.calibration.get("observation") or {}).get("images") or {}) if isinstance(self.calibration, dict) else {}
        return (
            chest_file.exists()
            or CHEST_VIDEO_FILE[1] in self.video_start_offsets_us
            or CHEST_VIDEO_FILE[0] in images
        )

    def resolve_episode_profile(self) -> None:
        software_version = safe_str(self.metadata.get("software_version")) or ""
        self.stereo_enabled = "+nostereo" not in software_version.lower()

    def resolve_active_video_files(self) -> None:
        self.active_video_files = [
            entry
            for entry in BASE_VIDEO_FILES
            if self.stereo_enabled or entry[0] not in {"left_stereo", "right_stereo"}
        ]
        if self.chest_camera_enabled():
            self.active_video_files.insert(2, CHEST_VIDEO_FILE)

    def check_dynamic_required_files(self) -> None:
        if not self.chest_camera_enabled():
            return
        file_name = CHEST_VIDEO_FILE[1]
        path = self.episode_dir / file_name
        if not path.exists():
            self.add_finding("FAIL", "missing_file", f"缺少关键文件: {file_name}", path=str(path))
        elif path.stat().st_size <= 0:
            self.add_finding("FAIL", "empty_file", f"关键文件为空: {file_name}", path=str(path))

    def check_optional_artifacts(self) -> None:
        validation_error_path = self.episode_dir / "validation_error.log"
        if validation_error_path.exists():
            content = validation_error_path.read_text(encoding="utf-8", errors="replace").strip()
            self.artifacts["validation_error.log"] = {
                "path": str(validation_error_path),
                "size": validation_error_path.stat().st_size,
                "nonempty": bool(content),
            }
            severity = "FAIL" if content else "WARN"
            self.add_finding(
                severity,
                "validation_error_log",
                "存在 validation_error.log",
                path=str(validation_error_path),
                nonempty=bool(content),
                preview=content[:200] if content else "",
            )
        for name in ("audio_pre.wav", "audio_post.wav"):
            path = self.episode_dir / name
            if path.exists():
                self.artifacts[name] = {"path": str(path), "size": path.stat().st_size}

    def load_json_file(self, name: str) -> dict[str, Any]:
        path = self.episode_dir / name
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            self.add_finding("FAIL", "json_parse_error", f"{name} 解析失败", path=str(path), error=str(exc))
            return {}

    def load_metadata(self) -> None:
        self.metadata = self.load_json_file("metadata.json")

    def load_calibration(self) -> None:
        self.calibration = self.load_json_file("calibration.json")

    def validate_metadata_fields(self) -> None:
        metadata_key_order = list(self.metadata.keys())
        expected_metadata_key_order = [
            key
            for key in EXPECTED_METADATA_KEY_ORDER
            if key not in OPTIONAL_METADATA_KEYS or key in self.metadata
        ]
        if metadata_key_order != expected_metadata_key_order:
            self.add_finding(
                "FAIL",
                "metadata_schema_key_order",
                "metadata.json 顶层字段顺序不符合标准范本",
                actual_order=metadata_key_order,
                expected_order=expected_metadata_key_order,
            )
        metadata_keys = set(self.metadata.keys())
        missing_keys = sorted((EXPECTED_METADATA_KEYS - OPTIONAL_METADATA_KEYS) - metadata_keys)
        unexpected_keys = sorted(metadata_keys - EXPECTED_METADATA_KEYS)
        if missing_keys:
            self.add_finding(
                "FAIL",
                "metadata_schema_missing_keys",
                "metadata.json 缺少预期字段集合中的键",
                missing_keys=missing_keys,
            )
        if unexpected_keys:
            self.add_finding(
                "FAIL",
                "metadata_schema_unexpected_keys",
                "metadata.json 出现未登记字段，疑似 schema 被改动",
                unexpected_keys=unexpected_keys,
            )
        for field_name in REQUIRED_METADATA_FIELDS:
            value = self.metadata.get(field_name)
            if value in (None, "", []):
                self.add_finding("FAIL", "metadata_field", f"metadata.json 缺少字段: {field_name}")
        expected_types = {
            "camera_codec": str,
            "collection_duration_s": (int, float),
            "audio_uuid": str,
            "das_usb_updater_version": str,
            "data_uuid": str,
            "data_version": str,
            "device_mode": str,
            "device_sn": str,
            "device_type": str,
            "episode_name": str,
            "hardware_list": dict,
            "hardware_version": str,
            "quality_check_err_type": str,
            "quality_check_status": str,
            "require_files": list,
            "software_version": str,
            "task_start_s": (int, float),
            "task_stop_s": (int, float),
            "video_details": list,
        }
        for field_name, expected_type in expected_types.items():
            if field_name not in self.metadata:
                continue
            value = self.metadata[field_name]
            numeric_bool = (
                isinstance(value, bool)
                and isinstance(expected_type, tuple)
                and any(item in {int, float} for item in expected_type)
            )
            if numeric_bool or not isinstance(value, expected_type):
                if isinstance(expected_type, tuple):
                    expected_type_name = "|".join(t.__name__ for t in expected_type)
                else:
                    expected_type_name = expected_type.__name__
                self.add_finding(
                    "FAIL",
                    "metadata_schema_type",
                    f"metadata.json 字段类型异常: {field_name}",
                    expected_type=expected_type_name,
                    actual_type=value_type_name(value),
                )
        hardware_list = self.metadata.get("hardware_list")
        if isinstance(hardware_list, dict):
            hardware_key_order = list(hardware_list.keys())
            if hardware_key_order != EXPECTED_HARDWARE_LIST_KEY_ORDER:
                self.add_finding(
                    "FAIL",
                    "metadata_schema_key_order",
                    "metadata.json.hardware_list 字段顺序不符合标准范本",
                    actual_order=hardware_key_order,
                    expected_order=EXPECTED_HARDWARE_LIST_KEY_ORDER,
                )
            missing_keys = sorted(REQUIRED_HARDWARE_LIST_KEYS - set(hardware_list.keys()))
            unexpected_keys = sorted(set(hardware_list.keys()) - REQUIRED_HARDWARE_LIST_KEYS)
            if missing_keys:
                self.add_finding(
                    "FAIL",
                    "metadata_schema_missing_keys",
                    "metadata.json.hardware_list 缺少预期字段",
                    missing_keys=missing_keys,
                )
            if unexpected_keys:
                self.add_finding(
                    "WARN",
                    "metadata_schema_unexpected_keys",
                    "metadata.json.hardware_list 出现未登记字段",
                    unexpected_keys=unexpected_keys,
                )
            for key, nested_value in hardware_list.items():
                if nested_value is not None and not isinstance(nested_value, str):
                    self.add_finding(
                        "FAIL",
                        "metadata_schema_type",
                        f"metadata.json.hardware_list.{key} 字段类型异常",
                        expected_type="str",
                        actual_type=value_type_name(nested_value),
                    )
            required_non_empty = [
                "gripper_right_sn",
                "gripper_left_sn",
                "cam_right_sn",
                "cam_left_sn",
                "tactile_right_l_sn",
                "tactile_right_r_sn",
                "tactile_left_l_sn",
                "tactile_left_r_sn",
            ]
            if self.stereo_enabled:
                required_non_empty.extend(["stereo_right_sn", "stereo_left_sn"])
            if self.chest_camera_enabled():
                required_non_empty.append("cam_chest_sn")
            for key in required_non_empty:
                value = hardware_list.get(key)
                if not isinstance(value, str) or not value.strip():
                    self.add_finding(
                        "FAIL",
                        "metadata_hardware_sn",
                        f"metadata.json.hardware_list.{key} 为空",
                    )
        data_version = safe_str(self.metadata.get("data_version"))
        if data_version == "3.0":
            self.add_finding(
                "INFO",
                "metadata_data_version_legacy",
                "metadata.json 使用兼容的历史 data_version 3.0",
                data_version=data_version,
            )
        elif data_version is not None and data_version != "3.1":
            self.add_finding(
                "WARN",
                "metadata_data_version",
                "metadata.json 的 data_version 不是当前版本 3.1",
                data_version=data_version,
            )
        self.validate_task_markers()
        device_type = safe_str(self.metadata.get("device_type"))
        if device_type is not None and device_type not in {"ugripper", "ego", "glove"}:
            self.add_finding("WARN", "metadata_device_type", "metadata.json 的 device_type 不是常见值", device_type=device_type)
        device_mode = safe_str(self.metadata.get("device_mode"))
        if device_mode is not None and device_mode not in {"dual", "single_right", "single_left"}:
            self.add_finding("WARN", "metadata_device_mode", "metadata.json 的 device_mode 不是常见值", device_mode=device_mode)
        camera_codec = safe_str(self.metadata.get("camera_codec"))
        if camera_codec is not None and camera_codec not in {"h264", "h265"}:
            self.add_finding(
                "WARN",
                "metadata_camera_codec",
                "metadata.json 的 camera_codec 不是常见值",
                camera_codec=camera_codec,
            )
        quality_status = safe_str(self.metadata.get("quality_check_status"))
        if quality_status is not None and quality_status not in {"", "success", "fail"}:
            self.add_finding(
                "WARN",
                "metadata_quality_check_status",
                "metadata.json 的 quality_check_status 不是约定值",
                quality_check_status=quality_status,
            )
        quality_err_type = safe_str(self.metadata.get("quality_check_err_type"))
        if quality_err_type is not None and quality_err_type not in {
            "",
            "missing_file",
            "collection_duration_too_short",
            "frame_loss",
            "finalize_error",
            "calibration_error",
            "device_disconnected",
            "unknown",
        }:
            self.add_finding(
                "WARN",
                "metadata_quality_check_err_type",
                "metadata.json 的 quality_check_err_type 不是约定值",
                quality_check_err_type=quality_err_type,
            )
        require_files = self.metadata.get("require_files")
        if isinstance(require_files, list):
            expected_required_files = set(CORE_REQUIRED_FILES)
            if self.stereo_enabled:
                expected_required_files.update(STEREO_REQUIRED_FILES)
            actual_required_files = {str(name) for name in require_files}
            missing_required_names = sorted(expected_required_files - actual_required_files)
            if missing_required_names:
                self.add_finding(
                    "WARN",
                    "metadata_require_files",
                    "metadata.json.require_files 未覆盖校验脚本所需文件",
                    missing_names=missing_required_names,
                )
            if not self.stereo_enabled:
                unexpected_stereo_names = sorted(set(STEREO_REQUIRED_FILES) & actual_required_files)
                if unexpected_stereo_names:
                    self.add_finding(
                        "FAIL",
                        "metadata_nostereo_profile",
                        "+nostereo metadata.json.require_files 不应声明 stereo/Fays 产物",
                        unexpected_names=unexpected_stereo_names,
                    )
        video_details = self.metadata.get("video_details")
        if isinstance(video_details, list):
            detail_names = [detail.get("name") for detail in video_details if isinstance(detail, dict)]
            expected_detail_names = list(EXPECTED_METADATA_VIDEO_DETAIL_NAMES)
            if not self.stereo_enabled:
                expected_detail_names = [
                    name for name in expected_detail_names if name not in {"stereo_left.mkv", "stereo_right.mkv"}
                ]
            if not self.chest_camera_enabled():
                expected_detail_names = [name for name in expected_detail_names if name != "cam_chest.mkv"]
            if detail_names != expected_detail_names:
                self.add_finding(
                    "FAIL",
                    "metadata_video_details_order",
                    "metadata.json.video_details 顺序不符合标准范本",
                    actual_order=detail_names,
                    expected_order=expected_detail_names,
                )
            for index, detail in enumerate(video_details):
                if not isinstance(detail, dict):
                    self.add_finding("FAIL", "metadata_video_details", f"video_details[{index}] 不是 object")
                    continue
                detail_key_order = list(detail.keys())
                if detail_key_order != EXPECTED_VIDEO_DETAIL_KEY_ORDER:
                    self.add_finding(
                        "FAIL",
                        "metadata_video_details_key_order",
                        f"video_details[{index}] 字段顺序不符合标准范本",
                        actual_order=detail_key_order,
                        expected_order=EXPECTED_VIDEO_DETAIL_KEY_ORDER,
                    )
                for key in EXPECTED_VIDEO_DETAIL_KEY_ORDER:
                    if key not in detail:
                        self.add_finding("FAIL", "metadata_video_details", f"video_details[{index}] 缺少字段: {key}")
                if "duration" in detail:
                    self.add_finding("FAIL", "metadata_video_details", f"video_details[{index}] 应使用 duration_s，不应再写 duration")
                if "serial" in detail:
                    self.add_finding("FAIL", "metadata_video_details", f"video_details[{index}] 不应再写 serial 字段")
                for key in ("fps", "duration_s"):
                    if key in detail and not isinstance(detail.get(key), (int, float)):
                        self.add_finding("FAIL", "metadata_video_details", f"video_details[{index}].{key} 类型异常")
                if "start_offset_us" in detail and not isinstance(detail.get("start_offset_us"), int):
                    self.add_finding("FAIL", "metadata_video_details", f"video_details[{index}].start_offset_us 类型异常")
                if isinstance(detail.get("name"), str) and isinstance(detail.get("start_offset_us"), int):
                    self.video_start_offsets_us[detail["name"]] = detail["start_offset_us"]

    def validate_task_markers(self) -> None:
        start_present = "task_start_s" in self.metadata
        stop_present = "task_stop_s" in self.metadata
        if not start_present and not stop_present:
            return

        start_value = self.metadata.get("task_start_s", 0.0)
        stop_value = self.metadata.get("task_stop_s", 0.0)
        if (
            isinstance(start_value, bool)
            or isinstance(stop_value, bool)
            or not isinstance(start_value, (int, float))
            or not isinstance(stop_value, (int, float))
        ):
            return

        task_start_s = float(start_value)
        task_stop_s = float(stop_value)
        if not math.isfinite(task_start_s) or not math.isfinite(task_stop_s):
            self.add_finding(
                "FAIL",
                "metadata_task_markers",
                "task_start_s/task_stop_s 必须是有限数值",
                task_start_s=task_start_s,
                task_stop_s=task_stop_s,
            )
            return
        if task_start_s < 0.0 or task_stop_s < 0.0:
            self.add_finding(
                "FAIL",
                "metadata_task_markers",
                "task_start_s/task_stop_s 不得为负数",
                task_start_s=task_start_s,
                task_stop_s=task_stop_s,
            )
            return
        if task_stop_s > 0.0 and task_start_s <= 0.0:
            self.add_finding(
                "FAIL",
                "metadata_task_markers",
                "存在 task_stop_s 但缺少有效 task_start_s",
                task_start_s=task_start_s,
                task_stop_s=task_stop_s,
            )
            return
        if task_start_s > 0.0 and task_stop_s <= 0.0:
            self.add_finding(
                "INFO",
                "metadata_task_start_only",
                "仅存在任务开始标记，按无回环数据处理",
                task_start_s=task_start_s,
            )
            return
        if task_stop_s > 0.0 and task_stop_s <= task_start_s:
            self.add_finding(
                "FAIL",
                "metadata_task_markers",
                "task_stop_s 必须晚于 task_start_s",
                task_start_s=task_start_s,
                task_stop_s=task_stop_s,
            )

    def validate_calibration_structure(self) -> None:
        calibration_key_order = list(self.calibration.keys())
        if calibration_key_order != EXPECTED_CALIBRATION_KEY_ORDER:
            self.add_finding(
                "FAIL",
                "calibration_schema_key_order",
                "calibration.json 顶层字段顺序不符合 3.0 标准范本",
                actual_order=calibration_key_order,
                expected_order=EXPECTED_CALIBRATION_KEY_ORDER,
            )
        calibration_keys = set(self.calibration.keys())
        missing_root_keys = sorted(EXPECTED_CALIBRATION_ROOT_KEYS - calibration_keys)
        unexpected_root_keys = sorted(calibration_keys - EXPECTED_CALIBRATION_ROOT_KEYS)
        if missing_root_keys:
            self.add_finding(
                "FAIL",
                "calibration_schema_missing_keys",
                "calibration.json 缺少预期顶层字段",
                missing_keys=missing_root_keys,
            )
        if unexpected_root_keys:
            self.add_finding(
                "FAIL",
                "calibration_schema_unexpected_keys",
                "calibration.json 出现未登记顶层字段，疑似 schema 被改动",
                unexpected_keys=unexpected_root_keys,
            )
        for key in REQUIRED_CALIBRATION_ROOT_KEYS:
            if key not in self.calibration:
                self.add_finding("FAIL", "calibration_field", f"calibration.json 缺少字段: {key}")
        calibration_info = self.calibration.get("calibration_info")
        if not isinstance(calibration_info, dict):
            self.add_finding(
                "FAIL",
                "calibration_schema_type",
                "calibration.json.calibration_info 类型异常",
                expected_type="dict",
                actual_type=value_type_name(calibration_info),
            )
        else:
            if list(calibration_info.keys()) != EXPECTED_CALIBRATION_INFO_KEY_ORDER:
                self.add_finding(
                    "FAIL",
                    "calibration_schema_key_order",
                    "calibration.json.calibration_info 字段顺序不符合 3.0 标准范本",
                    actual_order=list(calibration_info.keys()),
                    expected_order=EXPECTED_CALIBRATION_INFO_KEY_ORDER,
                )
            if calibration_info.get("calibration_status") not in {"calibrated", "uncalibrated"}:
                self.add_finding(
                    "FAIL",
                    "calibration_field",
                    "calibration.json.calibration_info.calibration_status 非法",
                    value=calibration_info.get("calibration_status"),
                )
            if calibration_info.get("format_version") != "3.0":
                self.add_finding(
                    "FAIL",
                    "calibration_field",
                    "calibration.json.calibration_info.format_version 应为 3.0",
                    value=calibration_info.get("format_version"),
                )
        observation = self.calibration.get("observation")
        if not isinstance(observation, dict):
            self.add_finding(
                "FAIL",
                "calibration_schema_type",
                "calibration.json.observation 类型异常",
                expected_type="dict",
                actual_type=value_type_name(observation),
            )
            return
        if list(observation.keys()) != EXPECTED_CALIBRATION_OBSERVATION_KEY_ORDER:
            self.add_finding(
                "FAIL",
                "calibration_schema_key_order",
                "calibration.json.observation 字段顺序不符合 3.0 标准范本",
                actual_order=list(observation.keys()),
                expected_order=EXPECTED_CALIBRATION_OBSERVATION_KEY_ORDER,
            )
        observation_keys = set(observation.keys())
        missing_observation_keys = sorted(EXPECTED_CALIBRATION_OBSERVATION_KEYS - observation_keys)
        unexpected_observation_keys = sorted(observation_keys - EXPECTED_CALIBRATION_OBSERVATION_KEYS)
        if missing_observation_keys:
            self.add_finding(
                "FAIL",
                "calibration_schema_missing_keys",
                "calibration.json.observation 缺少预期字段",
                missing_keys=missing_observation_keys,
            )
        if unexpected_observation_keys:
            self.add_finding(
                "FAIL",
                "calibration_schema_unexpected_keys",
                "calibration.json.observation 出现未登记字段，疑似 schema 被改动",
                unexpected_keys=unexpected_observation_keys,
            )
        images = observation.get("images")
        if not isinstance(images, dict):
            self.add_finding("FAIL", "calibration_field", "calibration.json.observation.images 缺失或非法")
        else:
            expected_image_keys = list(EXPECTED_CALIBRATION_IMAGE_KEY_ORDER)
            if not self.chest_camera_enabled():
                expected_image_keys = [name for name in expected_image_keys if name != "cam_chest_main"]
            if list(images.keys()) != expected_image_keys:
                self.add_finding(
                    "FAIL",
                    "calibration_schema_key_order",
                    "calibration.json.observation.images 字段顺序不符合 3.0 标准范本",
                    actual_order=list(images.keys()),
                    expected_order=expected_image_keys,
                )
            unexpected_image_keys = sorted(set(images.keys()) - set(expected_image_keys))
            if unexpected_image_keys:
                self.add_finding(
                    "FAIL",
                    "calibration_schema_unexpected_keys",
                    "calibration.json.observation.images 出现未登记字段，疑似 schema 被改动",
                    unexpected_keys=unexpected_image_keys,
                )
            for key in expected_image_keys:
                if key not in images:
                    self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images 缺少 {key}")
            self.validate_calibration_image_entries(images, expected_image_keys)
        imu = observation.get("imu")
        if not isinstance(imu, dict):
            self.add_finding("FAIL", "calibration_field", "calibration.json.observation.imu 缺失或非法")
        else:
            if list(imu.keys()) != REQUIRED_CALIBRATION_IMU_KEYS:
                self.add_finding(
                    "FAIL",
                    "calibration_schema_key_order",
                    "calibration.json.observation.imu 字段顺序不符合 3.0 标准范本",
                    actual_order=list(imu.keys()),
                    expected_order=REQUIRED_CALIBRATION_IMU_KEYS,
                )
            unexpected_imu_keys = sorted(set(imu.keys()) - set(REQUIRED_CALIBRATION_IMU_KEYS))
            if unexpected_imu_keys:
                self.add_finding(
                    "FAIL",
                    "calibration_schema_unexpected_keys",
                    "calibration.json.observation.imu 出现未登记字段，疑似 schema 被改动",
                    unexpected_keys=unexpected_imu_keys,
                )
            for key in REQUIRED_CALIBRATION_IMU_KEYS:
                if key not in imu:
                    self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.imu 缺少 {key}")
            self.validate_calibration_imu_entries(imu)

    def validate_entry_allowed_keys(self, entry: Any, expected_keys: set[str] | list[str], path: str) -> None:
        if not isinstance(entry, dict):
            self.add_finding("FAIL", "calibration_field", f"{path} 缺失或非法")
            return
        expected_key_set = set(expected_keys)
        unexpected_keys = sorted(set(entry.keys()) - expected_key_set)
        if unexpected_keys:
            self.add_finding(
                "FAIL",
                "calibration_schema_unexpected_keys",
                f"{path} 出现未登记字段，疑似 schema 被改动",
                unexpected_keys=unexpected_keys,
            )
        if isinstance(expected_keys, list) and list(entry.keys()) != expected_keys:
            self.add_finding(
                "FAIL",
                "calibration_schema_key_order",
                f"{path} 字段顺序不符合 3.0 标准范本",
                actual_order=list(entry.keys()),
                expected_order=expected_keys,
            )

    def validate_calibration_image_entries(self, images: dict[str, Any], expected_image_keys: list[str]) -> None:
        for key in expected_image_keys:
            entry = images.get(key)
            if key in MAIN_CAMERA_NAMES:
                self.validate_entry_allowed_keys(entry, MAIN_CAMERA_CALIBRATION_KEY_ORDER, f"calibration.json.observation.images.{key}")
                if isinstance(entry, dict):
                    if entry.get("camera_model") != "pinhole":
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.camera_model 非法")
                    if entry.get("distortion_model") != "equidistant":
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.distortion_model 非法")
                    if entry.get("names") != ["height", "width", "channels"]:
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.names 非法")
                    if entry.get("shape") != [1080, 1920, 3]:
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.shape 非法")
            elif key in STEREO_CAMERA_NAMES:
                self.validate_entry_allowed_keys(entry, STEREO_CALIBRATION_KEY_ORDER, f"calibration.json.observation.images.{key}")
                if isinstance(entry, dict):
                    for camera_node in ("cam0", "cam1"):
                        self.validate_entry_allowed_keys(
                            entry.get(camera_node),
                            STEREO_CAMERA_NODE_KEY_ORDER,
                            f"calibration.json.observation.images.{key}.{camera_node}",
                        )
                    if entry.get("camera_model") != "pinhole":
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.camera_model 非法")
                    if entry.get("distortion_model") != "equidistant":
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.distortion_model 非法")
                    if entry.get("names") != ["height", "width", "channels"]:
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.names 非法")
                    if entry.get("shape") != [800, 640, 3]:
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.shape 非法")
            elif key in TACTILE_CAMERA_NAMES:
                self.validate_entry_allowed_keys(entry, TACTILE_CALIBRATION_KEY_ORDER, f"calibration.json.observation.images.{key}")
                if isinstance(entry, dict):
                    serial = entry.get("serial")
                    if not isinstance(serial, str) or not serial.strip():
                        self.add_finding(
                            "FAIL",
                            "calibration_field",
                            f"calibration.json.observation.images.{key}.serial 缺失或非法",
                        )
                    elif serial.strip().startswith("{{") and serial.strip().endswith("}}"):
                        self.add_finding(
                            "FAIL",
                            "calibration_field",
                            f"calibration.json.observation.images.{key}.serial 仍是占位符，未写入真实设备 serial",
                            serial=serial,
                        )
                    if entry.get("names") != ["height", "width", "channels"]:
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.names 非法")
                    if entry.get("shape") != [480, 640, 3]:
                        self.add_finding("FAIL", "calibration_field", f"calibration.json.observation.images.{key}.shape 非法")

    def validate_calibration_imu_entries(self, imu: dict[str, Any]) -> None:
        for key in REQUIRED_CALIBRATION_IMU_KEYS:
            self.validate_entry_allowed_keys(
                imu.get(key),
                IMU_CALIBRATION_KEYS,
                f"calibration.json.observation.imu.{key}",
            )

    def ffprobe_stream_info(self, path: Path) -> tuple[str | None, float | None, float | None]:
        result = run_command(
            [
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
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "ffprobe stream probe failed")
        payload = json.loads(result.stdout or "{}")
        streams = payload.get("streams") or []
        stream = streams[0] if streams else {}
        codec = stream.get("codec_name")
        avg_fps = parse_rate(stream.get("avg_frame_rate")) or parse_rate(stream.get("r_frame_rate"))
        duration_sec = safe_float((payload.get("format") or {}).get("duration"))
        return codec, duration_sec, avg_fps

    def ffprobe_packets(self, path: Path) -> list[dict[str, Any]]:
        result = run_command(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_packets",
                "-show_entries",
                "packet=pts_time,dts_time,flags",
                "-of",
                "json",
                str(path),
            ]
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "ffprobe packet probe failed")
        return (json.loads(result.stdout or "{}").get("packets") or [])

    def scan_videos(self) -> None:
        for video_name, file_name, side in self.active_video_files:
            path = self.episode_dir / file_name
            stats = VideoStats(name=video_name, side=side, path=str(path))
            self.video_stats[video_name] = stats
            if not path.exists() or path.stat().st_size <= 0:
                stats.error = "missing or empty file"
                continue

            try:
                stats.codec, stats.duration_sec, stats.avg_fps = self.ffprobe_stream_info(path)
            except Exception as exc:
                stats.error = str(exc)
                self.add_finding("FAIL", "video_unreadable", f"视频不可读: {file_name}", error=str(exc))
                continue

            record_offset_us = safe_int(self.video_start_offsets_us.get(file_name))
            if self.args.skip_video_packets:
                if stats.duration_sec is not None and record_offset_us is not None:
                    stats.first_pts_sec = 0.0
                    stats.last_pts_sec = max(0.0, stats.duration_sec)
                    stats.first_system_time_us = record_offset_us
                    stats.last_system_time_us = record_offset_us + sec_to_us(stats.duration_sec)
                continue

            try:
                packets = self.ffprobe_packets(path)
            except Exception as exc:
                stats.error = str(exc)
                self.add_finding("FAIL", "video_packet_probe_failed", f"视频逐包扫描失败: {file_name}", error=str(exc))
                continue

            pts_list: list[float] = []
            deltas_ms: list[float] = []
            largest_gaps: list[PacketGap] = []
            prev_pts: float | None = None
            for packet in packets:
                pts = safe_float(packet.get("pts_time"))
                if pts is None:
                    pts = safe_float(packet.get("dts_time"))
                if pts is None:
                    continue
                pts_list.append(pts)
                if prev_pts is not None:
                    delta_sec = pts - prev_pts
                    if abs(delta_sec) < 1e-9:
                        stats.duplicate_pts_count += 1
                    elif delta_sec < 0:
                        stats.backward_pts_count += 1
                    else:
                        delta_ms = delta_sec * 1000.0
                        deltas_ms.append(delta_ms)
                        if delta_ms > stats.max_gap_ms:
                            stats.max_gap_ms = delta_ms
                        largest_gaps.append(
                            PacketGap(
                                gap_ms=delta_ms,
                                prev_pts_sec=prev_pts,
                                curr_pts_sec=pts,
                                flags=str(packet.get("flags", "")),
                            )
                        )
                prev_pts = pts

            stats.packet_count = len(pts_list)
            if pts_list:
                stats.first_pts_sec = pts_list[0]
                stats.last_pts_sec = pts_list[-1]
                if record_offset_us is not None:
                    stats.first_system_time_us = record_offset_us + sec_to_us(stats.first_pts_sec)
                    stats.last_system_time_us = record_offset_us + sec_to_us(stats.last_pts_sec)
            if deltas_ms:
                sorted_deltas = sorted(deltas_ms)
                stats.median_delta_ms = median(sorted_deltas)
                stats.p99_delta_ms = percentile(sorted_deltas, 0.99)
                gap_floor_ms = self.args.video_gap_min_ms
                threshold_ms = max(gap_floor_ms, stats.median_delta_ms * self.args.video_gap_factor)
                stats.large_gap_count = sum(1 for value in deltas_ms if value > threshold_ms)
                stats.largest_gaps = sorted(largest_gaps, key=lambda item: item.gap_ms, reverse=True)[: self.args.top]

                if stats.max_gap_ms > threshold_ms:
                    severity = "FAIL" if stats.max_gap_ms >= self.args.video_gap_fail_ms else "WARN"
                    self.add_finding(
                        severity,
                        "video_gap",
                        f"{file_name} 存在异常视频 gap",
                        max_gap_ms=round(stats.max_gap_ms, 3),
                        threshold_ms=round(threshold_ms, 3),
                        median_delta_ms=round(stats.median_delta_ms, 3),
                        large_gap_count=stats.large_gap_count,
                    )
            if stats.packet_count <= 1:
                self.add_finding("FAIL", "video_too_few_packets", f"{file_name} 视频包数量过少", packet_count=stats.packet_count)
            if stats.duplicate_pts_count > 0:
                self.add_finding(
                    "WARN",
                    "video_duplicate_pts",
                    f"{file_name} 存在重复时间戳",
                    duplicate_pts_count=stats.duplicate_pts_count,
                )
            if stats.backward_pts_count > 0:
                self.add_finding(
                    "FAIL",
                    "video_backward_pts",
                    f"{file_name} 存在回退时间戳",
                    backward_pts_count=stats.backward_pts_count,
                )

    def scan_main_camera_pair(self) -> None:
        script_path = PROJECT_ROOT / "test" / "scripts" / "scan_main_camera_mkv_issues.py"
        if not script_path.exists():
            self.add_finding("WARN", "main_camera_scan_missing", "缺少主摄专项扫描脚本")
            return
        result = run_command(
            [
                sys.executable,
                str(script_path),
                "--decode-mode",
                self.args.main_decode_mode,
                "--json",
                str(self.episode_dir),
            ]
        )
        if result.returncode != 0 and not result.stdout.strip():
            self.add_finding(
                "WARN",
                "main_camera_scan_failed",
                "主摄专项扫描脚本执行失败",
                returncode=result.returncode,
                error=result.stderr.strip(),
            )
            return
        try:
            payload = json.loads(result.stdout or "{}")
        except Exception as exc:
            self.add_finding("WARN", "main_camera_scan_failed", "主摄专项扫描结果解析失败", error=str(exc))
            return

        self.artifacts["main_camera_scan"] = payload
        for file_summary in payload.get("files") or []:
            path = safe_str(file_summary.get("path")) or ""
            side = safe_str(file_summary.get("side")) or "unknown"
            decode = file_summary.get("decode") or {}
            non_monotonic_count = safe_int(decode.get("non_monotonic_dts_count")) or 0
            ref_missing_count = safe_int(decode.get("ref_missing_count")) or 0
            decode_error_count = safe_int(decode.get("decode_error_count")) or 0
            backward_dts_count = safe_int(file_summary.get("backward_dts_count")) or 0
            duplicate_dts_count = safe_int(file_summary.get("duplicate_dts_count")) or 0
            if ref_missing_count > 0 or decode_error_count > 0:
                self.add_finding(
                    "FAIL",
                    "main_camera_decode_error",
                    f"{side} 主摄存在解码异常",
                    path=path,
                    ref_missing_count=ref_missing_count,
                    decode_error_count=decode_error_count,
                )
            if non_monotonic_count > 0:
                severity = "WARN" if backward_dts_count == 0 else "FAIL"
                self.add_finding(
                    severity,
                    "main_camera_non_monotonic_dts",
                    f"{side} 主摄存在 non-monotonic dts 现象",
                    path=path,
                    non_monotonic_dts_count=non_monotonic_count,
                    backward_dts_count=backward_dts_count,
                    duplicate_dts_count=duplicate_dts_count,
                )

    def iter_mcap_messages(self, mcap_path: Path):
        if StreamReader is None:
            raise RuntimeError(
                "python mcap package is unavailable; prefer running this script via `uv run python ...`"
            )
        with mcap_path.open("rb") as handle:
            channels: dict[int, Any] = {}
            for record in StreamReader(handle).records:
                record_name = type(record).__name__
                if record_name == "Channel":
                    channels[record.id] = record
                    continue
                if record_name != "Message":
                    continue
                channel = channels.get(record.channel_id)
                if channel is None:
                    continue
                yield str(channel.topic), int(record.log_time)

    def iter_mcap_message_records(self, mcap_path: Path):
        if StreamReader is None:
            raise RuntimeError(
                "python mcap package is unavailable; prefer running this script via `uv run python ...`"
            )
        with mcap_path.open("rb") as handle:
            channels: dict[int, Any] = {}
            for record in StreamReader(handle).records:
                record_name = type(record).__name__
                if record_name == "Channel":
                    channels[record.id] = record
                    continue
                if record_name != "Message":
                    continue
                channel = channels.get(record.channel_id)
                if channel is None:
                    continue
                data = bytes(getattr(record, "data", b"") or b"")
                yield str(channel.topic), int(record.log_time), int(record.publish_time), data

    def scan_sensors(self) -> None:
        if StreamReader is None:
            self.add_finding(
                "WARN",
                "mcap_dependency_missing",
                "无法执行 sensor 深度分析，当前 Python 环境缺少可用的 mcap 读取器，建议改用 `uv run python`",
                fallback_roots=[str(path) for path in MCAP_FALLBACK_ROOTS],
            )
            return

        topic_times: dict[str, list[int]] = defaultdict(list)
        topic_sources: dict[str, str] = {}
        for _, file_name, expected_topics in SENSOR_FILES:
            path = self.episode_dir / file_name
            if not path.exists() or path.stat().st_size <= 0:
                continue
            try:
                for topic, log_time_ns in self.iter_mcap_messages(path):
                    topic_times[topic].append(log_time_ns)
                    topic_sources[topic] = str(path)
            except Exception as exc:
                self.add_finding("FAIL", "mcap_read_error", f"读取 {file_name} 失败", error=str(exc))
                continue

            for topic in expected_topics:
                if topic not in topic_times:
                    self.add_finding("FAIL", "missing_sensor_topic", f"{file_name} 缺少 topic: {topic}", path=str(path))

        for topic, timestamps in sorted(topic_times.items()):
            timestamps.sort()
            stats = SensorTopicStats(topic=topic, source_file=topic_sources.get(topic, ""))
            self.sensor_stats[topic] = stats
            stats.count = len(timestamps)
            stats.first_log_time_ns = timestamps[0]
            stats.last_log_time_ns = timestamps[-1]
            if len(timestamps) <= 1:
                self.add_finding("FAIL", "sensor_too_few_samples", f"{topic} 样本数过少", count=len(timestamps))
                continue

            gaps_ms = [
                (timestamps[index] - timestamps[index - 1]) / 1_000_000.0
                for index in range(1, len(timestamps))
                if timestamps[index] >= timestamps[index - 1]
            ]
            if not gaps_ms:
                self.add_finding("FAIL", "sensor_bad_timestamps", f"{topic} 时间戳异常，无法建立 gap 统计")
                continue

            sorted_gaps = sorted(gaps_ms)
            stats.median_gap_ms = median(sorted_gaps)
            stats.p99_gap_ms = percentile(sorted_gaps, 0.99)
            stats.max_gap_ms = max(sorted_gaps)
            min_gap_floor = self.args.imu_gap_min_ms if topic.startswith("imu_") else self.args.encoder_gap_min_ms
            stats.gap_threshold_ms = max(min_gap_floor, stats.median_gap_ms * self.args.sensor_gap_factor)
            stats.large_gap_count = sum(1 for value in gaps_ms if value > stats.gap_threshold_ms)

            if stats.max_gap_ms > stats.gap_threshold_ms:
                severity = "FAIL" if stats.max_gap_ms >= self.args.sensor_gap_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "sensor_gap",
                    f"{topic} 存在异常 gap",
                    max_gap_ms=round(stats.max_gap_ms, 3),
                    threshold_ms=round(stats.gap_threshold_ms, 3),
                    median_gap_ms=round(stats.median_gap_ms, 3),
                    large_gap_count=stats.large_gap_count,
                )

    def scan_fays_mcaps(self) -> None:
        if not self.stereo_enabled:
            return
        if StreamReader is None:
            self.add_finding(
                "WARN",
                "mcap_dependency_missing",
                "无法执行 Fays MCAP 深度分析，当前 Python 环境缺少可用的 mcap 读取器，建议改用 `uv run python`",
                fallback_roots=[str(path) for path in MCAP_FALLBACK_ROOTS],
            )
            return

        for side, file_name, stereo_camera_name in FAYS_MCAP_FILES:
            path = self.episode_dir / file_name
            if not path.exists() or path.stat().st_size <= 0:
                continue

            try:
                with path.open("rb") as handle:
                    head = handle.read(len(MCAP_MAGIC))
                    handle.seek(-len(MCAP_MAGIC), 2)
                    tail = handle.read(len(MCAP_MAGIC))
                if head != MCAP_MAGIC or tail != MCAP_MAGIC:
                    self.add_finding(
                        "FAIL",
                        "fays_mcap_magic",
                        f"{file_name} MCAP 头尾 magic 不完整",
                        path=str(path),
                        head_ok=head == MCAP_MAGIC,
                        tail_ok=tail == MCAP_MAGIC,
                        size=path.stat().st_size,
                    )
            except Exception as exc:
                self.add_finding("FAIL", "fays_mcap_read_error", f"读取 {file_name} 头尾失败", error=str(exc))
                continue

            stats = FaysMcapStats(side=side, source_file=str(path))
            previous_camera_log_ns: int | None = None
            previous_camera_publish_ns: int | None = None
            previous_frame_index: int | None = None
            nominal_camera_interval_ns = 40_000_000
            try:
                for topic, log_time_ns, publish_time_ns, data in self.iter_mcap_message_records(path):
                    data_size = len(data)
                    is_imu = topic == "i" or data_size == FAYS_IMU_PAYLOAD_BYTES
                    is_camera = topic == "c" or data_size == FAYS_CAMERA_PAYLOAD_BYTES
                    if not is_imu and not is_camera:
                        continue
                    if is_imu:
                        stats.imu_count += 1
                    if is_camera:
                        stats.camera_count += 1
                        frame_index = int.from_bytes(data[:4], byteorder="little", signed=False) if len(data) >= 4 else None
                        if stats.first_camera_log_time_ns is None:
                            stats.first_camera_log_time_ns = log_time_ns
                            stats.first_camera_publish_time_ns = publish_time_ns
                            stats.first_frame_index = frame_index
                        stats.last_camera_log_time_ns = log_time_ns
                        stats.last_camera_publish_time_ns = publish_time_ns
                        stats.last_frame_index = frame_index

                        if previous_camera_log_ns is not None and previous_camera_publish_ns is not None:
                            if log_time_ns < previous_camera_log_ns or publish_time_ns < previous_camera_publish_ns:
                                stats.camera_rollback_count += 1
                            else:
                                gap_ns = log_time_ns - previous_camera_log_ns
                                stats.max_camera_gap_ms = max(stats.max_camera_gap_ms, gap_ns / 1_000_000.0)
                                if gap_ns > nominal_camera_interval_ns + nominal_camera_interval_ns // 2:
                                    stats.camera_gap_count += 1
                                    intervals = (gap_ns + nominal_camera_interval_ns // 2) // nominal_camera_interval_ns
                                    stats.estimated_missing_frames += max(0, intervals - 1)
                        if (
                            frame_index is None
                            or (previous_frame_index is not None and frame_index != previous_frame_index + 1)
                        ):
                            stats.frame_index_error_count += 1
                        previous_camera_log_ns = log_time_ns
                        previous_camera_publish_ns = publish_time_ns
                        previous_frame_index = frame_index
                    if stats.first_log_time_ns is None or log_time_ns < stats.first_log_time_ns:
                        stats.first_log_time_ns = log_time_ns
                    if stats.last_log_time_ns is None or log_time_ns > stats.last_log_time_ns:
                        stats.last_log_time_ns = log_time_ns
            except Exception as exc:
                self.add_finding("FAIL", "fays_mcap_read_error", f"解析 {file_name} 失败", error=str(exc))
                continue

            if stats.first_log_time_ns is not None and stats.last_log_time_ns is not None:
                stats.span_sec = max(0.0, (stats.last_log_time_ns - stats.first_log_time_ns) / 1_000_000_000.0)
            if (
                stats.first_camera_publish_time_ns is not None
                and stats.last_camera_publish_time_ns is not None
            ):
                stats.camera_publish_span_sec = max(
                    0.0,
                    (stats.last_camera_publish_time_ns - stats.first_camera_publish_time_ns)
                    / 1_000_000_000.0,
                )
            self.fays_mcap_stats[file_name] = stats

            if stats.imu_count <= 0 or stats.camera_count <= 0:
                self.add_finding(
                    "FAIL",
                    "fays_mcap_topic",
                    f"{file_name} 缺少 Fays i/c topic 数据",
                    imu_count=stats.imu_count,
                    camera_count=stats.camera_count,
                )
                continue

            stereo_stats = self.video_stats.get(stereo_camera_name)
            if stereo_stats is not None:
                if stereo_stats.packet_count > 0 and stereo_stats.packet_count != stats.camera_count:
                    self.add_finding(
                        "FAIL",
                        "fays_video_frame_count",
                        f"{file_name} camera 消息数与对应 stereo MKV packet 数不一致",
                        camera_count=stats.camera_count,
                        video_packet_count=stereo_stats.packet_count,
                    )
                if stereo_stats.first_system_time_us is not None and stats.camera_publish_span_sec > 0:
                    stereo_stats.last_system_time_us = (
                        stereo_stats.first_system_time_us
                        + sec_to_us(stats.camera_publish_span_sec)
                    )

            metadata_detail = next(
                (
                    detail
                    for detail in self.metadata.get("video_details", [])
                    if isinstance(detail, dict) and detail.get("name") == f"stereo_{side}.mkv"
                ),
                None,
            )
            metadata_duration = safe_float(metadata_detail.get("duration_s")) if metadata_detail else None
            if metadata_duration is not None and stats.camera_publish_span_sec > 0:
                if abs(metadata_duration - round(stats.camera_publish_span_sec, 1)) > 0.051:
                    self.add_finding(
                        "FAIL",
                        "fays_metadata_duration",
                        f"{file_name} 的 publishTime 跨度与 metadata stereo duration_s 不一致",
                        metadata_duration_s=metadata_duration,
                        camera_publish_span_s=round(stats.camera_publish_span_sec, 6),
                    )

            if stats.camera_rollback_count > 0 or stats.frame_index_error_count > 0:
                self.add_finding(
                    "FAIL",
                    "fays_camera_timestamps",
                    f"{file_name} camera 时间戳或 frameIndex 不单调",
                    rollback_count=stats.camera_rollback_count,
                    frame_index_error_count=stats.frame_index_error_count,
                )
            if stats.camera_gap_count > 0:
                self.add_finding(
                    "WARN",
                    "fays_camera_gap",
                    f"{file_name} 存在真实设备时间 gap，metadata duration 已按 publishTime 保留该时间洞",
                    max_gap_ms=round(stats.max_camera_gap_ms, 3),
                    gap_count=stats.camera_gap_count,
                    estimated_missing_frames=stats.estimated_missing_frames,
                )

            reference_duration_sec = stereo_stats.duration_sec if stereo_stats else None
            if reference_duration_sec and reference_duration_sec > 0:
                coverage_ratio = stats.span_sec / reference_duration_sec
                if coverage_ratio < self.args.fays_mcap_coverage_warn_ratio:
                    severity = "FAIL" if coverage_ratio < self.args.fays_mcap_coverage_fail_ratio else "WARN"
                    self.add_finding(
                        severity,
                        "fays_mcap_coverage",
                        f"{file_name} 覆盖时长明显短于对应 stereo 视频",
                        span_sec=round(stats.span_sec, 3),
                        reference_video_duration_sec=round(reference_duration_sec, 3),
                        coverage_ratio=round(coverage_ratio, 3),
                    )

        left_fays = self.fays_mcap_stats.get("fays_data_left.mcap")
        right_fays = self.fays_mcap_stats.get("fays_data_right.mcap")
        left_metadata_offset = safe_int(self.video_start_offsets_us.get("stereo_left.mkv"))
        right_metadata_offset = safe_int(self.video_start_offsets_us.get("stereo_right.mkv"))
        if (
            left_fays is not None
            and right_fays is not None
            and left_fays.first_camera_publish_time_ns is not None
            and right_fays.first_camera_publish_time_ns is not None
            and left_metadata_offset is not None
            and right_metadata_offset is not None
        ):
            publish_delta_us = (
                right_fays.first_camera_publish_time_ns
                - left_fays.first_camera_publish_time_ns
            ) // 1000
            metadata_delta_us = right_metadata_offset - left_metadata_offset
            delta_error_us = abs(metadata_delta_us - publish_delta_us)
            if delta_error_us > 1000:
                self.add_finding(
                    "FAIL",
                    "fays_metadata_start_offset",
                    "左右 stereo metadata start_offset_us 未保持 MCAP publishTime 的真实起点差",
                    metadata_delta_us=metadata_delta_us,
                    publish_time_delta_us=publish_delta_us,
                    delta_error_us=delta_error_us,
                )

    def check_video_span_consistency(self) -> None:
        for stats in self.video_stats.values():
            if stats.duration_sec is None or stats.first_pts_sec is None or stats.last_pts_sec is None:
                continue
            packet_span_ms = (stats.last_pts_sec - stats.first_pts_sec) * 1000.0
            duration_span_ms = stats.duration_sec * 1000.0
            delta_ms = abs(duration_span_ms - packet_span_ms)
            if delta_ms > self.args.video_span_delta_warn_ms:
                severity = "FAIL" if delta_ms >= self.args.video_span_delta_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_span_consistency",
                    f"{stats.name} 容器 duration 与包级 span 偏差过大",
                    duration_sec=round(stats.duration_sec, 6),
                    packet_span_ms=round(packet_span_ms, 3),
                    delta_ms=round(delta_ms, 3),
                )

    def check_video_alignment(self) -> None:
        starts = {
            name: stats.first_system_time_us
            for name, stats in self.video_stats.items()
            if stats.first_system_time_us is not None
        }
        ends = {
            name: stats.last_system_time_us
            for name, stats in self.video_stats.items()
            if stats.last_system_time_us is not None
        }
        if len(starts) >= 2:
            earliest_start_us = min(starts.values())
            start_range_ms = us_to_ms(max(starts.values()) - min(starts.values()))
            self.add_finding(
                "INFO",
                "video_start_offsets",
                "所有 mkv 起始时间相对最早视频的偏移量",
                earliest_start_us=earliest_start_us,
                start_range_ms=round(start_range_ms, 3),
                per_camera_ms={name: round(us_to_ms(value - earliest_start_us), 3) for name, value in sorted(starts.items())},
            )
            if start_range_ms > self.args.video_start_warn_ms:
                severity = "FAIL" if start_range_ms >= self.args.video_start_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_start_alignment",
                    "多路视频起始时间对齐偏差过大",
                    start_range_ms=round(start_range_ms, 3),
                    per_camera_ms={name: round(us_to_ms(value - earliest_start_us), 3) for name, value in sorted(starts.items())},
                )
        if len(ends) >= 2:
            end_range_ms = us_to_ms(max(ends.values()) - min(ends.values()))
            if end_range_ms > self.args.video_end_warn_ms:
                severity = "FAIL" if end_range_ms >= self.args.video_end_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_end_alignment",
                    "多路视频结束时间对齐偏差过大",
                    end_range_ms=round(end_range_ms, 3),
                    earliest_end_us=min(ends.values()),
                    latest_end_us=max(ends.values()),
                )

    def check_pair_alignment(self) -> None:
        pair_defs = [
            ("main", "left_cam_main", "right_cam_main"),
            ("stereo", "left_stereo", "right_stereo"),
        ]
        for label, left_name, right_name in pair_defs:
            left = self.video_stats.get(left_name)
            right = self.video_stats.get(right_name)
            if left is None or right is None:
                continue
            if left.first_system_time_us is not None and right.first_system_time_us is not None:
                start_diff_ms = us_to_ms(abs(left.first_system_time_us - right.first_system_time_us))
                if start_diff_ms > self.args.video_pair_start_warn_ms:
                    severity = "FAIL" if start_diff_ms >= self.args.video_pair_start_fail_ms else "WARN"
                    self.add_finding(
                        severity,
                        "video_pair_start_alignment",
                        f"{label} 左右起始时间偏差过大",
                        start_diff_ms=round(start_diff_ms, 3),
                    )
            if left.last_system_time_us is not None and right.last_system_time_us is not None:
                end_diff_ms = us_to_ms(abs(left.last_system_time_us - right.last_system_time_us))
                if end_diff_ms > self.args.video_pair_end_warn_ms:
                    severity = "FAIL" if end_diff_ms >= self.args.video_pair_end_fail_ms else "WARN"
                    self.add_finding(
                        severity,
                        "video_pair_end_alignment",
                        f"{label} 左右结束时间偏差过大",
                        end_diff_ms=round(end_diff_ms, 3),
                    )

    def check_first_frame_sync(self) -> None:
        firsts = {
            name: stats.first_system_time_us
            for name, stats in self.video_stats.items()
            if stats.first_system_time_us is not None
        }
        if len(firsts) < 2:
            return
        non_main_firsts = {
            name: value for name, value in firsts.items() if name not in MAIN_CAMERA_NAMES
        }
        reference_us = min(non_main_firsts.values()) if non_main_firsts else min(firsts.values())
        per_mkv_offset_ms = {
            f"{name}.mkv": round(us_to_ms(first_system_time_us - reference_us), 3)
            for name, first_system_time_us in sorted(firsts.items())
        }
        self.add_finding(
            "INFO",
            "first_frame_offsets",
            "所有 mkv 首帧相对参考时刻的偏移量",
            reference_camera_group="non_main" if non_main_firsts else "all_videos",
            reference_system_time_us=reference_us,
            per_mkv_offset_ms=per_mkv_offset_ms,
        )

        for name, first_system_time_us in sorted(firsts.items()):
            error_ms = us_to_ms(abs(first_system_time_us - reference_us))
            warn_ms = self.args.main_camera_first_frame_warn_ms if name in MAIN_CAMERA_NAMES else self.args.first_frame_warn_ms
            fail_ms = self.args.main_camera_first_frame_fail_ms if name in MAIN_CAMERA_NAMES else self.args.first_frame_fail_ms
            if error_ms > warn_ms:
                severity = "FAIL" if error_ms >= fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "first_frame_sync",
                    f"{name} 首帧同步误差过大",
                    sync_error_ms=round(error_ms, 3),
                    reference_camera_group="non_main" if non_main_firsts else "all_videos",
                )

    def check_stereo_session_consistency(self) -> None:
        return

    def check_sensor_alignment(self) -> None:
        for topic_prefix in ("imu", "encoder"):
            left = self.sensor_stats.get(f"{topic_prefix}_left")
            right = self.sensor_stats.get(f"{topic_prefix}_right")
            if left is None or right is None:
                continue
            if left.first_log_time_ns is None or right.first_log_time_ns is None:
                continue
            start_diff_ms = ns_to_ms(abs(left.first_log_time_ns - right.first_log_time_ns))
            end_diff_ms = ns_to_ms(abs((left.last_log_time_ns or 0) - (right.last_log_time_ns or 0)))
            if start_diff_ms > self.args.sensor_lr_start_warn_ms:
                severity = "FAIL" if start_diff_ms >= self.args.sensor_lr_start_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "sensor_lr_start_alignment",
                    f"{topic_prefix} 左右起始同步偏差过大",
                    start_diff_ms=round(start_diff_ms, 3),
                )
            if end_diff_ms > self.args.sensor_lr_end_warn_ms:
                severity = "FAIL" if end_diff_ms >= self.args.sensor_lr_end_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "sensor_lr_end_alignment",
                    f"{topic_prefix} 左右结束同步偏差过大",
                    end_diff_ms=round(end_diff_ms, 3),
                )

    def check_sensor_coverage(self) -> None:
        reference_video_duration_sec = max(
            (stats.duration_sec or 0.0) for stats in self.video_stats.values()
        )
        if reference_video_duration_sec <= 0:
            return
        for topic_name, stats in self.sensor_stats.items():
            if stats.first_log_time_ns is None or stats.last_log_time_ns is None:
                continue
            duration_sec = (stats.last_log_time_ns - stats.first_log_time_ns) / 1_000_000_000.0
            coverage_ratio = duration_sec / reference_video_duration_sec if reference_video_duration_sec > 0 else 0.0
            if coverage_ratio < self.args.sensor_coverage_warn_ratio:
                severity = "FAIL" if coverage_ratio < self.args.sensor_coverage_fail_ratio else "WARN"
                self.add_finding(
                    severity,
                    "sensor_coverage",
                    f"{topic_name} 覆盖时长明显短于视频",
                    duration_sec=round(duration_sec, 3),
                    reference_video_duration_sec=round(reference_video_duration_sec, 3),
                    coverage_ratio=round(coverage_ratio, 3),
                )

    def side_video_window_us(self, side: str) -> tuple[int | None, int | None]:
        starts = [
            stats.first_system_time_us
            for stats in self.video_stats.values()
            if stats.side == side and stats.first_system_time_us is not None
        ]
        ends = [
            stats.last_system_time_us
            for stats in self.video_stats.values()
            if stats.side == side and stats.last_system_time_us is not None
        ]
        return (min(starts) if starts else None, max(ends) if ends else None)

    def side_sensor_window_ns(self, side: str) -> tuple[int | None, int | None]:
        topics = [self.sensor_stats.get(f"imu_{side}"), self.sensor_stats.get(f"encoder_{side}")]
        starts = [topic.first_log_time_ns for topic in topics if topic and topic.first_log_time_ns is not None]
        ends = [topic.last_log_time_ns for topic in topics if topic and topic.last_log_time_ns is not None]
        return (min(starts) if starts else None, max(ends) if ends else None)

    def check_video_sensor_alignment(self) -> None:
        return

    def check_video_sensor_overlap(self) -> None:
        return

    def classify_patterns(self) -> None:
        starts = {
            name: stats.first_system_time_us
            for name, stats in self.video_stats.items()
            if stats.first_system_time_us is not None
        }
        if len(starts) < 3:
            return

        stereo_starts = [value for name, value in starts.items() if name in STEREO_CAMERA_NAMES]
        tactile_starts = [value for name, value in starts.items() if name in TACTILE_CAMERA_NAMES]
        main_starts = [value for name, value in starts.items() if name in MAIN_CAMERA_NAMES]
        if stereo_starts and tactile_starts and main_starts:
            stereo_median = median(stereo_starts)
            tactile_median = median(tactile_starts)
            main_median = median(main_starts)
            stereo_to_tactile_ms = us_to_ms(tactile_median - stereo_median)
            tactile_to_main_ms = us_to_ms(main_median - tactile_median)
            if stereo_to_tactile_ms > self.args.pattern_group_stagger_warn_ms and tactile_to_main_ms > self.args.pattern_group_stagger_warn_ms:
                severity = (
                    "FAIL"
                    if stereo_to_tactile_ms >= self.args.pattern_group_stagger_fail_ms
                    or tactile_to_main_ms >= self.args.pattern_group_stagger_fail_ms
                    else "WARN"
                )
                self.add_finding(
                    severity,
                    "video_group_staggered_start",
                    "视频起录呈现 stereo -> tactile -> main 的分组错峰",
                    stereo_to_tactile_ms=round(stereo_to_tactile_ms, 3),
                    tactile_to_main_ms=round(tactile_to_main_ms, 3),
                )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate one ugripper episode directory in read-only mode.")
    parser.add_argument("path", help="Episode directory, or a parent directory that contains episode_* children.")
    parser.add_argument(
        "--latest",
        action="store_true",
        help="When PATH is a parent directory, auto-select the newest episode_* child.",
    )
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON.")
    parser.add_argument("--top", type=int, default=5, help="Max retained gap samples per stream.")
    parser.add_argument("--skip-video-packets", action="store_true", help="Skip ffprobe packet scan and only use container-level timing.")
    parser.add_argument("--video-gap-factor", type=float, default=3.0)
    parser.add_argument("--sensor-gap-factor", type=float, default=3.0)
    parser.add_argument("--video-gap-min-ms", type=float, default=80.0)
    parser.add_argument("--video-gap-fail-ms", type=float, default=150.0)
    parser.add_argument("--imu-gap-min-ms", type=float, default=20.0)
    parser.add_argument("--encoder-gap-min-ms", type=float, default=30.0)
    parser.add_argument("--sensor-gap-fail-ms", type=float, default=120.0)
    parser.add_argument("--video-start-warn-ms", type=float, default=500.0)
    parser.add_argument("--video-start-fail-ms", type=float, default=1000.0)
    parser.add_argument("--video-end-warn-ms", type=float, default=1000.0)
    parser.add_argument("--video-end-fail-ms", type=float, default=1500.0)
    parser.add_argument("--first-frame-warn-ms", type=float, default=33.0)
    parser.add_argument("--first-frame-fail-ms", type=float, default=80.0)
    parser.add_argument("--main-camera-first-frame-warn-ms", type=float, default=900.0)
    parser.add_argument("--main-camera-first-frame-fail-ms", type=float, default=1200.0)
    parser.add_argument("--stereo-consistency-warn-ms", type=float, default=10.0)
    parser.add_argument("--stereo-consistency-fail-ms", type=float, default=30.0)
    parser.add_argument("--sensor-lr-start-warn-ms", type=float, default=30.0)
    parser.add_argument("--sensor-lr-start-fail-ms", type=float, default=80.0)
    parser.add_argument("--sensor-lr-end-warn-ms", type=float, default=50.0)
    parser.add_argument("--sensor-lr-end-fail-ms", type=float, default=120.0)
    parser.add_argument("--video-sensor-start-warn-ms", type=float, default=120.0)
    parser.add_argument("--video-sensor-start-fail-ms", type=float, default=300.0)
    parser.add_argument("--video-sensor-end-warn-ms", type=float, default=1000.0)
    parser.add_argument("--video-sensor-end-fail-ms", type=float, default=1500.0)
    parser.add_argument("--video-sensor-overlap-warn-ratio", type=float, default=0.9)
    parser.add_argument("--video-sensor-overlap-fail-ratio", type=float, default=0.75)
    parser.add_argument("--video-span-delta-warn-ms", type=float, default=80.0)
    parser.add_argument("--video-span-delta-fail-ms", type=float, default=150.0)
    parser.add_argument("--video-pair-start-warn-ms", type=float, default=500.0)
    parser.add_argument("--video-pair-start-fail-ms", type=float, default=1000.0)
    parser.add_argument("--video-pair-end-warn-ms", type=float, default=80.0)
    parser.add_argument("--video-pair-end-fail-ms", type=float, default=180.0)
    parser.add_argument("--sensor-coverage-warn-ratio", type=float, default=0.9)
    parser.add_argument("--sensor-coverage-fail-ratio", type=float, default=0.75)
    parser.add_argument("--fays-mcap-coverage-warn-ratio", type=float, default=0.9)
    parser.add_argument("--fays-mcap-coverage-fail-ratio", type=float, default=0.75)
    parser.add_argument("--pattern-group-stagger-warn-ms", type=float, default=80.0)
    parser.add_argument("--pattern-group-stagger-fail-ms", type=float, default=150.0)
    parser.add_argument(
        "--main-decode-mode",
        choices=("auto", "full", "off"),
        default="auto",
        help="Decode scan policy for integrated main camera issue scan.",
    )
    return parser


def print_text_report(report: Report) -> None:
    print(f"Episode: {report.episode_dir}")
    print(f"Status:  {report.status}")
    print()

    if report.findings:
        print("Findings:")
        for item in report.findings:
            print(f"- [{item.severity}] {item.summary}")
            if item.details:
                details_text = ", ".join(f"{key}={value}" for key, value in item.details.items())
                print(f"  {details_text}")
    else:
        print("Findings:")
        print("- [INFO] 未发现异常")
    print()

    if report.videos:
        print("Videos:")
        for name in sorted(report.videos):
            item = report.videos[name]
            print(
                f"- {name}: codec={item.codec} duration_sec={item.duration_sec} "
                f"packet_count={item.packet_count} max_gap_ms={round(item.max_gap_ms, 3)} "
                f"dup={item.duplicate_pts_count} back={item.backward_pts_count}"
            )
    if report.sensors:
        print()
        print("Sensors:")
        for name in sorted(report.sensors):
            item = report.sensors[name]
            print(
                f"- {name}: count={item.count} median_gap_ms={round(item.median_gap_ms, 3)} "
                f"max_gap_ms={round(item.max_gap_ms, 3)} large_gap_count={item.large_gap_count}"
            )
    if report.fays_mcaps:
        print()
        print("Fays MCAPs:")
        for name in sorted(report.fays_mcaps):
            item = report.fays_mcaps[name]
            print(
                f"- {name}: imu_count={item.imu_count} camera_count={item.camera_count} "
                f"span_sec={round(item.span_sec, 3)} "
                f"camera_publish_span_sec={round(item.camera_publish_span_sec, 3)} "
                f"max_camera_gap_ms={round(item.max_camera_gap_ms, 3)} "
                f"estimated_missing_frames={item.estimated_missing_frames}"
            )
    if report.artifacts:
        print()
        print("Artifacts:")
        for name in sorted(report.artifacts):
            print(f"- {name}: present")


def report_to_json(report: Report) -> str:
    return json.dumps(
        {
            "episode_dir": report.episode_dir,
            "status": report.status,
            "summary": report.summary,
            "findings": [asdict(item) for item in report.findings],
            "videos": {key: asdict(value) for key, value in report.videos.items()},
            "sensors": {key: asdict(value) for key, value in report.sensors.items()},
            "fays_mcaps": {key: asdict(value) for key, value in report.fays_mcaps.items()},
            "artifacts": report.artifacts,
        },
        ensure_ascii=False,
        indent=2,
    )


def main() -> int:
    ensure_ffprobe()
    args = build_parser().parse_args()
    validator = EpisodeValidator(args)
    report = validator.validate()
    if args.json:
        print(report_to_json(report))
    else:
        print_text_report(report)
    return 2 if report.status == "FAIL" else 1 if report.status == "WARN" else 0


if __name__ == "__main__":
    raise SystemExit(main())
