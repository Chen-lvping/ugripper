#!/usr/bin/env python3

import importlib.util
import unittest


def load_module():
    from pathlib import Path

    script_path = Path(__file__).resolve().parent / "summarize_episode_reports.py"
    spec = importlib.util.spec_from_file_location("summarize_episode_reports_module", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


module = load_module()


def report(name, data):
    return {"name": name, "path": f"/tmp/{name}.json", "data": data}


class SummarizeEpisodeReportsTest(unittest.TestCase):
    def test_ok_summary_uses_ok_reason(self):
        summary = module.summarize_episode_reports(
            [
                report("sensor_left", {"ok": True, "reference_span_ns": 1_000}),
                report("sensor_right", {"ok": True, "reference_span_ns": 980}),
            ],
            [
                report("video_left", {"ok": True, "duration_sec": 12.0, "validation_reason": "ok"}),
                report("video_right", {"ok": True, "duration_sec": 11.5, "validation_reason": "ok"}),
            ],
            max_video_span_gap_sec=5.0,
        )

        self.assertTrue(summary["ok"])
        self.assertEqual(summary["validation_reason"], "ok")
        self.assertEqual(summary["sensor"]["cross_alignment"]["reference_span_ns"], 1_000)
        self.assertEqual(summary["video"]["cross_alignment"]["reference_span_sec"], 12.0)

    def test_sensor_span_gap_failure_has_primary_reason(self):
        summary = module.summarize_episode_reports(
            [
                report("sensor_left", {"ok": True, "reference_span_ns": 2_000}),
                report("sensor_right", {"ok": True, "reference_span_ns": 1_000}),
            ],
            [],
            max_sensor_span_gap_ns=500,
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["validation_reason"], "sensor_span_gap_too_large")
        self.assertFalse(summary["sensor"]["cross_alignment"]["ok"])

    def test_video_reason_bubbles_up(self):
        summary = module.summarize_episode_reports(
            [],
            [
                report(
                    "video_left",
                    {
                        "ok": False,
                        "duration_sec": 10.0,
                        "primary_failure": "decode_error",
                        "validation_reason": "video_decode_error",
                    },
                ),
                report("video_right", {"ok": True, "duration_sec": 10.0, "validation_reason": "ok"}),
            ],
            max_video_span_gap_sec=5.0,
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["validation_reason"], "video_decode_error")

    def test_video_span_gap_failure_is_detected(self):
        summary = module.summarize_episode_reports(
            [],
            [
                report("video_left", {"ok": True, "duration_sec": 12.0, "validation_reason": "ok"}),
                report("video_right", {"ok": True, "duration_sec": 3.0, "validation_reason": "ok"}),
            ],
            max_video_span_gap_sec=5.0,
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["validation_reason"], "video_span_gap_too_large")
        self.assertFalse(summary["video"]["cross_alignment"]["ok"])

    def test_gripper_and_hmi_failures_are_mapped(self):
        summary = module.summarize_episode_reports(
            [],
            [],
            gripper_report=report("gripper", {"ok": False, "failures": ["command timeouts detected: 1"]}),
            hmi_report=report("hmi", {"ok": False, "failures": ["missing required led state: Error2"]}),
        )

        self.assertFalse(summary["ok"])
        self.assertEqual(summary["validation_reason"], "gripper_log_invalid")


if __name__ == "__main__":
    unittest.main()
