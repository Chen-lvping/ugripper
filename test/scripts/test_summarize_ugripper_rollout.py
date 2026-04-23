#!/usr/bin/env python3

import importlib.util
import unittest


def load_module():
    from pathlib import Path

    script_path = Path(__file__).resolve().parent / "summarize_ugripper_rollout.py"
    spec = importlib.util.spec_from_file_location("summarize_ugripper_rollout_module", script_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


module = load_module()


def suite(name, ok=True, validation_reason="ok", package_version=None):
    return {
        "suite": name,
        "path": f"/tmp/{name}.json",
        "ok": ok,
        "validation_reason": validation_reason,
        "package_version": package_version,
    }


class SummarizeUgripperRolloutTest(unittest.TestCase):
    def test_release_gate_ready_when_all_required_suites_pass_same_version(self):
        summary = module.summarize_rollout(
            [
                suite("camera", package_version="1.2.8+merge13"),
                suite("sensor", package_version="1.2.8+merge13"),
                suite("service", package_version="1.2.8+merge13"),
                suite("gripper_hmi", package_version="1.2.8+merge13"),
            ],
            ["camera", "sensor", "service", "gripper_hmi"],
        )

        self.assertTrue(summary["ok"])
        self.assertTrue(summary["release_gate_ready"])
        self.assertEqual(summary["validation_reason"], "ok")

    def test_mixed_package_versions_block_release_gate(self):
        summary = module.summarize_rollout(
            [
                suite("camera", package_version="1.2.8+merge8"),
                suite("sensor", package_version="1.2.8+merge8"),
                suite("service", package_version="1.2.8+merge8"),
                suite("gripper_hmi", package_version="1.2.8+merge13"),
            ],
            ["camera", "sensor", "service", "gripper_hmi"],
        )

        self.assertTrue(summary["ok"])
        self.assertFalse(summary["release_gate_ready"])
        self.assertEqual(summary["validation_reason"], "mixed_package_versions")

    def test_missing_required_suite_is_reported(self):
        summary = module.summarize_rollout(
            [
                suite("camera", package_version="1.2.8+merge13"),
                suite("sensor", package_version="1.2.8+merge13"),
            ],
            ["camera", "sensor", "service"],
        )

        self.assertTrue(summary["ok"])
        self.assertFalse(summary["release_gate_ready"])
        self.assertEqual(summary["validation_reason"], "missing_required_suite")
        self.assertEqual(summary["missing_suites"], ["service"])

    def test_failed_suite_bubbles_up(self):
        summary = module.summarize_rollout(
            [
                suite("camera", ok=False, validation_reason="video_decode_error", package_version="1.2.8+merge13"),
                suite("sensor", package_version="1.2.8+merge13"),
            ],
            ["camera", "sensor"],
        )

        self.assertFalse(summary["ok"])
        self.assertFalse(summary["release_gate_ready"])
        self.assertEqual(summary["validation_reason"], "suite_failed")


if __name__ == "__main__":
    unittest.main()
