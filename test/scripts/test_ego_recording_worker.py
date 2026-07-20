#!/usr/bin/env python3
import importlib.util
import subprocess
import unittest
from pathlib import Path
from unittest import mock


WORKER_PATH = (
    Path(__file__).resolve().parents[2]
    / "standalone"
    / "UgripperRuntime"
    / "ego"
    / "ego_recording_worker.py"
)
SPEC = importlib.util.spec_from_file_location("ego_recording_worker", WORKER_PATH)
WORKER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(WORKER)


class FakeAdb:
    def __init__(self, roots):
        self.roots = roots

    def run(self, *args, **kwargs):
        if args[:2] == ("shell", "test"):
            return subprocess.CompletedProcess(args, 0 if args[-1] in self.roots else 1, "", "")
        if len(args) == 2 and args[0] == "shell" and str(args[1]).startswith("find "):
            return subprocess.CompletedProcess(args, 0, "\n".join(self.roots) + "\n", "")
        raise AssertionError(f"unexpected adb command: {args}")


class EgoRecordingWorkerTest(unittest.TestCase):
    def test_discovers_serial_scoped_dataset_root(self):
        root = "/sdcard/Android/data/com.ssnwt.helloxr/files/1150063703226061200012/data/dataset"
        with mock.patch.dict(WORKER.os.environ, {}, clear=True):
            self.assertEqual(WORKER.list_dataset_roots(FakeAdb([root])), [root])

    def test_explicit_root_override_wins(self):
        override = "/custom/ego/dataset"
        with mock.patch.dict(WORKER.os.environ, {WORKER.REMOTE_ROOT_OVERRIDE_ENV: override}, clear=True):
            self.assertEqual(WORKER.list_dataset_roots(FakeAdb([])), [override])

    def test_finds_only_new_temp_episode_and_keeps_its_root(self):
        legacy_root = "/sdcard/Android/data/com.ssnwt.helloxr/files/dataset"
        serial_root = "/sdcard/Android/data/com.ssnwt.helloxr/files/1150063703226061200012/data/dataset"
        with mock.patch.object(WORKER, "list_dataset_roots", return_value=[legacy_root, serial_root]), mock.patch.object(
            WORKER,
            "list_temp_episodes",
            side_effect=lambda _adb, root: {
                legacy_root: ["episode_20260720_0001-temp"],
                serial_root: ["episode_20260720_0018-temp"],
            }[root],
        ):
            candidates = WORKER.find_new_temp_episodes(
                object(), {legacy_root: {"episode_20260720_0001-temp"}, serial_root: set()}
            )
        self.assertEqual(candidates, [(serial_root, "episode_20260720_0018-temp")])

    def test_remote_path_is_scoped_to_discovered_root(self):
        root = "/sdcard/Android/data/com.ssnwt.helloxr/files/serial/data/dataset"
        self.assertEqual(
            WORKER.remote_path(root, "episode_20260720_0018-temp", "sensor.mcap"),
            root + "/episode_20260720_0018-temp/sensor.mcap",
        )


if __name__ == "__main__":
    unittest.main()
