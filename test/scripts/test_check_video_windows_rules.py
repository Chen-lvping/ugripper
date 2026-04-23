#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


def load_module():
    script_path = Path(__file__).resolve().parent / "check_video_windows.py"
    spec = importlib.util.spec_from_file_location("check_video_windows_module", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


module = load_module()


class CheckVideoWindowsRulesTest(unittest.TestCase):
    def test_ok_summary_uses_ok_reason(self):
        summary = module.evaluate_windows(
            4.0,
            60.0,
            2.0,
            0.5,
            lambda _start_sec, _window_sec: (120, None),
        )

        self.assertTrue(summary["ok"])
        self.assertIsNone(summary["primary_failure"])
        self.assertEqual(summary["validation_reason"], "ok")

    def test_flags_low_frame_window(self):
        frame_counts = {
            0.0: (120, None),
            2.0: (40, None),
            4.0: (120, None),
        }

        summary = module.evaluate_windows(
            6.0,
            120.0,
            2.0,
            0.5,
            lambda start_sec, _window_sec: frame_counts[start_sec],
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["min_frames"], 120)
        self.assertEqual(summary["failure_types"]["low_frame_count"], 1)
        self.assertEqual(summary["primary_failure"], "low_frame_count")
        self.assertEqual(summary["validation_reason"], "video_low_frame_window")
        self.assertEqual(summary["windows"][1]["failures"], ["low_frame_count:40< 120"])

    def test_flags_decode_failure(self):
        summary = module.evaluate_windows(
            4.0,
            60.0,
            2.0,
            0.5,
            lambda _start_sec, _window_sec: (120, None),
            lambda start_sec, _window_sec: (start_sec == 0.0, "decoder exploded"),
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["failure_types"]["decode_error"], 1)
        self.assertEqual(summary["primary_failure"], "decode_error")
        self.assertEqual(summary["validation_reason"], "video_decode_error")
        self.assertEqual(summary["windows"][0]["failures"], [])
        self.assertEqual(summary["windows"][1]["failures"], ["decode_error:decoder exploded"])

    def test_handles_partial_tail_window(self):
        calls = []

        def counter(start_sec, window_sec):
            calls.append((start_sec, window_sec))
            return (30, None)

        summary = module.evaluate_windows(
            5.0,
            None,
            2.0,
            0.5,
            counter,
        )

        self.assertTrue(summary["ok"])
        self.assertEqual(
            calls,
            [
                (0.0, 2.0),
                (2.0, 2.0),
                (4.0, 1.0),
            ],
        )
        self.assertEqual(summary["windows"][2]["duration_sec"], 1.0)

    def test_scales_min_frames_for_partial_tail_window(self):
        frame_counts = {
            0.0: (60, None),
            2.0: (60, None),
            4.0: (30, None),
        }

        summary = module.evaluate_windows(
            5.0,
            60.0,
            2.0,
            0.5,
            lambda start_sec, _window_sec: frame_counts[start_sec],
        )

        self.assertTrue(summary["ok"])
        self.assertEqual(summary["windows"][0]["min_frames"], 60)
        self.assertEqual(summary["windows"][2]["min_frames"], 30)
        self.assertEqual(summary["windows"][2]["failures"], [])

    def test_flags_missing_frame_count_when_expected_fps_known(self):
        summary = module.evaluate_windows(
            4.0,
            60.0,
            2.0,
            0.5,
            lambda start_sec, _window_sec: (120, None) if start_sec == 0.0 else (None, None),
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["failure_types"]["frame_count_missing"], 1)
        self.assertEqual(summary["primary_failure"], "frame_count_missing")
        self.assertEqual(summary["validation_reason"], "video_frame_count_missing")
        self.assertEqual(summary["windows"][1]["failures"], ["frame_count_missing"])

    def test_decode_error_has_higher_priority_than_low_frame_count(self):
        summary = module.evaluate_windows(
            2.0,
            60.0,
            2.0,
            0.5,
            lambda _start_sec, _window_sec: (10, None),
            lambda _start_sec, _window_sec: (False, "decoder exploded"),
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["failure_types"]["low_frame_count"], 1)
        self.assertEqual(summary["failure_types"]["decode_error"], 1)
        self.assertEqual(summary["primary_failure"], "decode_error")
        self.assertEqual(summary["validation_reason"], "video_decode_error")


if __name__ == "__main__":
    unittest.main()
