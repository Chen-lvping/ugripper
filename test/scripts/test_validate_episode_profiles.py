#!/usr/bin/env python3
import importlib.util
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR_PATH = ROOT / ".codex/skills/validate-episode-data/scripts/validate_episode.py"
SPEC = importlib.util.spec_from_file_location("validate_episode", VALIDATOR_PATH)
VALIDATOR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = VALIDATOR
SPEC.loader.exec_module(VALIDATOR)


NOSTEREO_VIDEO_NAMES = [
    "cam_left.mkv",
    "tcam_left_l.mkv",
    "tcam_left_r.mkv",
    "cam_right.mkv",
    "tcam_right_l.mkv",
    "tcam_right_r.mkv",
]
STEREO_VIDEO_NAMES = [
    "cam_left.mkv",
    "tcam_left_l.mkv",
    "tcam_left_r.mkv",
    "stereo_left.mkv",
    "cam_right.mkv",
    "tcam_right_l.mkv",
    "tcam_right_r.mkv",
    "stereo_right.mkv",
]
UNIX_BASE_US = 1_785_000_000_000_000


def hardware_list(stereo_enabled):
    return {
        "gripper_right_sn": "DAG-R",
        "gripper_left_sn": "DAG-L",
        "cam_right_sn": "FE-R",
        "cam_left_sn": "FE-L",
        "cam_chest_sn": "",
        "tactile_right_l_sn": "TR-L",
        "tactile_right_r_sn": "TR-R",
        "tactile_left_l_sn": "TL-L",
        "tactile_left_r_sn": "TL-R",
        "stereo_right_sn": "ST-R" if stereo_enabled else "",
        "stereo_left_sn": "ST-L" if stereo_enabled else "",
    }


def video_details(names):
    return [
        {
            "name": name,
            "fps": 60.0,
            "duration_s": 10.0,
            "start_offset_us": UNIX_BASE_US + index * 1000,
        }
        for index, name in enumerate(names)
    ]


def metadata(stereo_enabled=False, data_version="3.1", marker_values=None):
    required_files = list(VALIDATOR.CORE_REQUIRED_FILES)
    names = NOSTEREO_VIDEO_NAMES
    if stereo_enabled:
        required_files.extend(VALIDATOR.STEREO_REQUIRED_FILES)
        names = STEREO_VIDEO_NAMES
    result = {
        "device_sn": "DAP-TEST",
        "device_type": "ugripper",
        "device_mode": "dual",
        "camera_codec": "h265",
        "hardware_version": "v2.5",
        "software_version": "v2.1.9" if stereo_enabled else "v2.1.9+nostereo",
        "das_usb_updater_version": "1.0.3",
        "data_version": data_version,
        "hardware_list": hardware_list(stereo_enabled),
        "episode_name": "episode_20260727_0001",
        "data_uuid": "test-uuid",
        "audio_uuid": "",
        "quality_check_status": "success",
        "quality_check_err_type": "",
        "collection_duration_s": 10.0,
    }
    if marker_values is not None:
        result.update(marker_values)
    result["require_files"] = required_files
    result["video_details"] = video_details(names)
    return result


class ValidateEpisodeProfileTest(unittest.TestCase):
    def make_validator(self, episode_dir):
        return VALIDATOR.EpisodeValidator(Namespace(path=str(episode_dir), latest=False))

    def write_required_files(self, episode_dir, stereo_enabled):
        required_files = list(VALIDATOR.CORE_REQUIRED_FILES)
        if stereo_enabled:
            required_files.extend(VALIDATOR.STEREO_REQUIRED_FILES)
        for name in required_files:
            (episode_dir / name).write_bytes(b"x")

    def validate_metadata(self, payload):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            validator.metadata = payload
            validator.resolve_episode_profile()
            validator.resolve_active_video_files()
            validator.validate_metadata_fields()
            return validator

    def populate_global_stats(self, validator, stereo_enabled):
        validator.stereo_enabled = stereo_enabled
        validator.active_video_files = [
            entry
            for entry in VALIDATOR.BASE_VIDEO_FILES
            if stereo_enabled or entry[0] not in {"left_stereo", "right_stereo"}
        ]
        for index, (video_name, file_name, side) in enumerate(validator.active_video_files):
            validator.video_stats[video_name] = VALIDATOR.VideoStats(
                name=video_name,
                side=side,
                path=file_name,
                first_system_time_us=UNIX_BASE_US + index * 10_000,
            )
        validator.sensor_stats["encoder_left"] = VALIDATOR.SensorTopicStats(
            topic="encoder_left",
            source_file="sensor_left.mcap",
            first_publish_time_ns=(UNIX_BASE_US + 80_000) * 1000,
        )
        validator.sensor_stats["encoder_right"] = VALIDATOR.SensorTopicStats(
            topic="encoder_right",
            source_file="sensor_right.mcap",
            first_publish_time_ns=(UNIX_BASE_US + 90_000) * 1000,
        )
        if stereo_enabled:
            validator.fays_mcap_stats["fays_data_left.mcap"] = VALIDATOR.FaysMcapStats(
                side="left",
                source_file="fays_data_left.mcap",
                first_log_time_ns=123,
                first_imu_publish_time_ns=(UNIX_BASE_US + 100_000) * 1000,
                first_camera_publish_time_ns=(UNIX_BASE_US + 30_000) * 1000,
            )
            validator.fays_mcap_stats["fays_data_right.mcap"] = VALIDATOR.FaysMcapStats(
                side="right",
                source_file="fays_data_right.mcap",
                first_log_time_ns=456,
                first_imu_publish_time_ns=(UNIX_BASE_US + 110_000) * 1000,
                first_camera_publish_time_ns=(UNIX_BASE_US + 70_000) * 1000,
            )

    def test_nostereo_profile_does_not_require_stereo_or_fays(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            self.write_required_files(episode_dir, stereo_enabled=False)
            validator = self.make_validator(episode_dir)
            validator.metadata = metadata(stereo_enabled=False)
            validator.resolve_episode_profile()
            validator.resolve_active_video_files()
            validator.check_required_files()
            validator.validate_metadata_fields()

            self.assertFalse(validator.stereo_enabled)
            self.assertFalse(any(item.severity == "FAIL" for item in validator.findings))
            self.assertNotIn("left_stereo", {entry[0] for entry in validator.active_video_files})

    def test_stereo_profile_still_requires_stereo_and_fays(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            self.write_required_files(episode_dir, stereo_enabled=False)
            validator = self.make_validator(episode_dir)
            validator.metadata = metadata(stereo_enabled=True)
            validator.resolve_episode_profile()
            validator.resolve_active_video_files()
            validator.check_required_files()

            missing = {
                item.details.get("path", "").rsplit("/", 1)[-1]
                for item in validator.findings
                if item.code == "missing_file"
            }
            self.assertEqual(missing, set(VALIDATOR.STEREO_REQUIRED_FILES))

    def test_task_markers_are_optional_and_start_only_means_no_loop_closure(self):
        cases = [
            (None, None),
            ({"task_start_s": 0.0, "task_stop_s": 0.0}, None),
            ({"task_start_s": 1000.0}, "metadata_task_start_only"),
            ({"task_start_s": 1000.0, "task_stop_s": 0.0}, "metadata_task_start_only"),
            ({"task_start_s": 1000.0, "task_stop_s": 1010.0}, None),
        ]
        for marker_values, expected_info_code in cases:
            with self.subTest(marker_values=marker_values):
                validator = self.validate_metadata(metadata(marker_values=marker_values))
                self.assertFalse(any(item.severity == "FAIL" for item in validator.findings))
                info_codes = {item.code for item in validator.findings if item.severity == "INFO"}
                if expected_info_code is not None:
                    self.assertIn(expected_info_code, info_codes)

    def test_invalid_task_marker_order_fails(self):
        cases = [
            {"task_stop_s": 1010.0},
            {"task_start_s": -1.0, "task_stop_s": 0.0},
            {"task_start_s": 1010.0, "task_stop_s": 1000.0},
            {"task_start_s": 1000.0, "task_stop_s": 1000.0},
        ]
        for marker_values in cases:
            with self.subTest(marker_values=marker_values):
                validator = self.validate_metadata(metadata(marker_values=marker_values))
                self.assertTrue(
                    any(item.code == "metadata_task_markers" and item.severity == "FAIL" for item in validator.findings)
                )

    def test_data_version_3_0_is_accepted_as_legacy(self):
        validator = self.validate_metadata(metadata(data_version="3.0"))
        self.assertFalse(any(item.code == "metadata_data_version" for item in validator.findings))
        self.assertTrue(any(item.code == "metadata_data_version_legacy" for item in validator.findings))

    def test_relative_video_offsets_fail_unix_time_validation(self):
        payload = metadata()
        for index, detail in enumerate(payload["video_details"]):
            detail["start_offset_us"] = index
        validator = self.validate_metadata(payload)
        self.assertTrue(
            any(item.code == "metadata_video_time_domain" and item.severity == "FAIL" for item in validator.findings)
        )

    def test_global_first_frame_sync_lists_all_nostereo_streams(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            self.populate_global_stats(validator, stereo_enabled=False)
            validator.check_video_sensor_alignment()

            result = validator.global_first_frame_sync
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["fail_threshold_ms"], 1000.0)
            self.assertEqual(len(result["streams"]), len(NOSTEREO_VIDEO_NAMES) + 2)
            self.assertFalse(result["missing_streams"])

    def test_global_first_frame_sync_uses_fays_publish_time(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            self.populate_global_stats(validator, stereo_enabled=True)
            validator.check_video_sensor_alignment()

            result = validator.global_first_frame_sync
            self.assertEqual(result["status"], "PASS")
            names = {item["name"] for item in result["streams"]}
            self.assertIn("fays:left:imu", names)
            self.assertIn("fays:right:camera", names)
            self.assertNotIn("fays_data_left.mcap:logTime", names)

    def test_global_first_frame_sync_fails_at_one_second(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            self.populate_global_stats(validator, stereo_enabled=False)
            validator.sensor_stats["encoder_right"].first_publish_time_ns = (UNIX_BASE_US + 1_000_000) * 1000
            validator.check_video_sensor_alignment()

            self.assertEqual(validator.global_first_frame_sync["status"], "FAIL")
            self.assertEqual(validator.global_first_frame_sync["max_error_ms"], 1000.0)
            self.assertTrue(
                any(item.code == "global_first_frame_sync" and item.severity == "FAIL" for item in validator.findings)
            )

    def test_global_first_frame_sync_includes_optional_ego_topics(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            self.populate_global_stats(validator, stereo_enabled=False)
            validator.sensor_stats["ego:tracking"] = VALIDATOR.SensorTopicStats(
                topic="ego:tracking",
                source_file="ego/sensor.mcap",
                first_publish_time_ns=(UNIX_BASE_US + 120_000) * 1000,
            )
            validator.check_video_sensor_alignment()

            stream_names = {item["name"] for item in validator.global_first_frame_sync["streams"]}
            self.assertIn("sensor:ego:tracking", stream_names)

    def test_ego_monotonic_topics_use_head_pose_metadata_offset(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            raw_head_pose_us = 5_522_316_243
            head_pose_unix_us = UNIX_BASE_US + 120_000
            validator.ego_metadata = {
                "head_pose_details": [
                    {"name": "head_pose", "start_offset_us": head_pose_unix_us}
                ]
            }
            validator.sensor_stats["ego:head_pose"] = VALIDATOR.SensorTopicStats(
                topic="ego:head_pose",
                source_file="ego/sensor.mcap",
                first_publish_time_ns=raw_head_pose_us * 1000,
            )
            validator.sensor_stats["ego:imu/gyro"] = VALIDATOR.SensorTopicStats(
                topic="ego:imu/gyro",
                source_file="ego/sensor.mcap",
                first_publish_time_ns=(raw_head_pose_us + 13_888) * 1000,
            )
            validator.sensor_stats["ego:rgb_metainfo"] = VALIDATOR.SensorTopicStats(
                topic="ego:rgb_metainfo",
                source_file="ego/sensor.mcap",
                first_publish_time_ns=(UNIX_BASE_US + 60_000) * 1000,
            )

            validator.align_sensor_first_frames()

            self.assertEqual(
                validator.sensor_stats["ego:head_pose"].first_system_time_us,
                head_pose_unix_us,
            )
            self.assertEqual(
                validator.sensor_stats["ego:imu/gyro"].first_system_time_us,
                head_pose_unix_us + 13_888,
            )
            self.assertEqual(
                validator.sensor_stats["ego:rgb_metainfo"].first_system_time_us,
                UNIX_BASE_US + 60_000,
            )
            self.assertEqual(
                validator.sensor_stats["ego:head_pose"].time_alignment_method,
                "ego_head_pose_metadata_offset",
            )

    def test_global_first_frame_sync_includes_all_discovered_sensor_topics(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            self.populate_global_stats(validator, stereo_enabled=False)
            validator.sensor_stats["imu_left"] = VALIDATOR.SensorTopicStats(
                topic="imu_left",
                source_file="sensor_left.mcap",
                first_publish_time_ns=(UNIX_BASE_US + 100_000) * 1000,
            )

            validator.check_video_sensor_alignment()

            stream_names = {item["name"] for item in validator.global_first_frame_sync["streams"]}
            self.assertIn("sensor:imu_left", stream_names)

    def test_ego_monotonic_topics_fail_without_metadata_offset(self):
        with tempfile.TemporaryDirectory(prefix="ugripper-validator-") as tmp:
            episode_dir = Path(tmp) / "episode_20260727_0001"
            episode_dir.mkdir()
            validator = self.make_validator(episode_dir)
            validator.sensor_stats["ego:head_pose"] = VALIDATOR.SensorTopicStats(
                topic="ego:head_pose",
                source_file="ego/sensor.mcap",
                first_publish_time_ns=5_522_316_243_000,
            )

            validator.align_sensor_first_frames()

            self.assertIsNone(validator.sensor_stats["ego:head_pose"].first_system_time_us)
            self.assertTrue(
                any(item.code == "ego_time_alignment" and item.severity == "FAIL" for item in validator.findings)
            )


if __name__ == "__main__":
    unittest.main()
