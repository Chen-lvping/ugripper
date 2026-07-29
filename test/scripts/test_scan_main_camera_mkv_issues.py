#!/usr/bin/env python3
import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCANNER_PATH = ROOT / "test/scripts/scan_main_camera_mkv_issues.py"
SPEC = importlib.util.spec_from_file_location("scan_main_camera_mkv_issues", SCANNER_PATH)
SCANNER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = SCANNER
SPEC.loader.exec_module(SCANNER)


class ScanMainCameraMkvIssuesTest(unittest.TestCase):
    def test_side_from_name_uses_cam_file_names(self):
        self.assertEqual(SCANNER.side_from_name(Path("cam_left.mkv")), "left")
        self.assertEqual(SCANNER.side_from_name(Path("cam_right.mkv")), "right")
        with self.assertRaises(ValueError):
            SCANNER.side_from_name(Path("other.mkv"))

    def test_decode_scan_preserves_source_timestamps(self):
        completed = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        with mock.patch.object(SCANNER, "run_command", return_value=completed) as run_command:
            SCANNER.run_decode_scan(Path("cam_left.mkv"), event_limit=8)

        command = run_command.call_args.args[0]
        vsync_index = command.index("-vsync")
        time_base_index = command.index("-enc_time_base")
        self.assertEqual(command[vsync_index + 1], "0")
        self.assertEqual(command[time_base_index + 1], "1:1000000")

    def test_genuine_source_timestamp_anomaly_still_fails(self):
        summary = SCANNER.FileSummary(
            path="cam_left.mkv",
            side="left",
            episode_dir=".",
            status="pending",
            duplicate_dts_count=1,
        )
        self.assertEqual(SCANNER.classify_status(summary), "timestamp_anomaly")


if __name__ == "__main__":
    unittest.main()
