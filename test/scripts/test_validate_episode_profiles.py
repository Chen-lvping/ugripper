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
        {"name": name, "fps": 60.0, "duration_s": 10.0, "start_offset_us": index}
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


if __name__ == "__main__":
    unittest.main()
