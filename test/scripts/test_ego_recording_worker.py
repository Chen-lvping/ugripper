#!/usr/bin/env python3
import importlib.util
import argparse
import json
import subprocess
import tempfile
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

    def test_bind_persists_the_only_eligible_ego(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            status_file = root / "status.json"
            args = argparse.Namespace(
                serial="",
                binding_file=binding_file,
                binding_status_file=status_file,
            )
            selected = mock.Mock(serial="ego-001")
            details = [{"serial": "ego-001", "props_match": True}]
            with mock.patch.object(WORKER, "detect_egos", return_value=([selected], details)):
                self.assertEqual(WORKER.run_bind(args), 0)

            binding = json.loads(binding_file.read_text(encoding="utf-8"))
            self.assertEqual(binding["serial"], "ego-001")
            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "bound")

    def test_bind_rejects_multiple_eligible_egos(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            args = argparse.Namespace(
                serial="",
                binding_file=binding_file,
                binding_status_file=root / "status.json",
            )
            devices = [mock.Mock(serial="ego-001"), mock.Mock(serial="ego-002")]
            with mock.patch.object(WORKER, "detect_egos", return_value=(devices, [])):
                self.assertEqual(WORKER.run_bind(args), 1)
            self.assertFalse(binding_file.exists())

    def test_probe_requires_bound_serial_in_adb_devices(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            binding_file.write_text('{"serial":"ego-001"}\n', encoding="utf-8")
            args = argparse.Namespace(
                binding_file=binding_file,
                binding_status_file=root / "status.json",
            )
            with mock.patch.object(WORKER, "Adb", return_value=object()), mock.patch.object(
                WORKER, "list_devices", return_value=["ego-002"]
            ):
                self.assertEqual(WORKER.run_probe(args), 1)

            status = json.loads(args.binding_status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "disconnected")

    def test_unbind_disconnected_ego_without_accessing_adb(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            status_file = root / "status.json"
            binding_file.write_text('{"serial":"ego-001"}\n', encoding="utf-8")
            status_file.write_text(
                '{"serial":"ego-001","status":"disconnected"}\n', encoding="utf-8"
            )
            args = argparse.Namespace(
                binding_file=binding_file,
                binding_status_file=status_file,
            )

            with mock.patch.object(WORKER, "Adb", side_effect=AssertionError("ADB must not be used")):
                self.assertEqual(WORKER.run_unbind(args), 0)
            self.assertFalse(binding_file.exists())
            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "unbound")
            self.assertNotIn("serial", status)

            self.assertEqual(WORKER.run_unbind(args), 0)
            self.assertEqual(
                json.loads(status_file.read_text(encoding="utf-8"))["status"],
                "unbound",
            )

    def test_stale_probe_cannot_restore_connected_status_after_unbind(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            status_file = root / "status.json"
            binding_file.write_text('{"serial":"ego-001"}\n', encoding="utf-8")
            args = argparse.Namespace(
                binding_file=binding_file,
                binding_status_file=status_file,
            )

            def unbind_while_probe_is_running(_adb):
                self.assertEqual(WORKER.run_unbind(args), 0)
                return ["ego-001"]

            with mock.patch.object(WORKER, "Adb", return_value=object()), mock.patch.object(
                WORKER, "list_devices", side_effect=unbind_while_probe_is_running
            ):
                self.assertEqual(WORKER.run_probe(args), 1)

            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "unbound")
            self.assertNotIn("serial", status)

    def test_unbind_restores_binding_when_status_publish_fails(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            status_file = root / "status.json"
            binding_file.write_text('{"serial":"ego-001"}\n', encoding="utf-8")
            status_file.write_text(
                '{"serial":"ego-001","status":"connected"}\n', encoding="utf-8"
            )
            args = argparse.Namespace(
                binding_file=binding_file,
                binding_status_file=status_file,
            )

            with mock.patch.object(WORKER, "write_status", side_effect=OSError("write failed")):
                self.assertEqual(WORKER.run_unbind(args), 1)

            binding = json.loads(binding_file.read_text(encoding="utf-8"))
            self.assertEqual(binding["serial"], "ego-001")
            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "connected")

    def test_stale_probe_cannot_overwrite_a_new_binding(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            status_file = root / "status.json"
            binding_file.write_text('{"serial":"ego-001"}\n', encoding="utf-8")
            args = argparse.Namespace(
                binding_file=binding_file,
                binding_status_file=status_file,
            )

            def replace_binding_while_probe_is_running(_adb):
                with WORKER.binding_state_lock(binding_file):
                    WORKER.write_json_atomic(binding_file, {"serial": "ego-002"})
                    WORKER.write_status(
                        status_file,
                        {"status": "bound", "serial": "ego-002", "updated_at_ms": WORKER.now_ms()},
                    )
                return ["ego-001"]

            with mock.patch.object(WORKER, "Adb", return_value=object()), mock.patch.object(
                WORKER, "list_devices", side_effect=replace_binding_while_probe_is_running
            ):
                self.assertEqual(WORKER.run_probe(args), 1)

            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "bound")
            self.assertEqual(status["serial"], "ego-002")

    def test_failed_stale_probe_cannot_overwrite_unbound_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binding_file = root / "binding.json"
            status_file = root / "status.json"
            binding_file.write_text('{"serial":"ego-001"}\n', encoding="utf-8")
            args = argparse.Namespace(
                binding_file=binding_file,
                binding_status_file=status_file,
            )

            def unbind_then_fail(_adb):
                self.assertEqual(WORKER.run_unbind(args), 0)
                raise RuntimeError("adb devices failed")

            with mock.patch.object(WORKER, "Adb", return_value=object()), mock.patch.object(
                WORKER, "list_devices", side_effect=unbind_then_fail
            ):
                self.assertEqual(WORKER.run_probe(args), 1)

            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual(status["status"], "unbound")
            self.assertNotIn("serial", status)

    def test_start_uses_non_terminal_status_while_preparing_bound_ego(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            args = argparse.Namespace(
                serial="ego-001",
                episode_dir=root / "episode_0001-temp",
                status_file=root / "ego_sync.json",
                codec="h265",
                detect_timeout=1.0,
                interval=0.01,
            )
            adb = mock.Mock(serial="ego-001")
            adb.run.return_value = subprocess.CompletedProcess([], 0, "", "")
            snapshots = []

            with mock.patch.object(WORKER, "detect_ego", return_value=(adb, {})), mock.patch.object(
                WORKER, "sync_ego_time", return_value={"status": "ok"}
            ), mock.patch.object(WORKER, "snapshot_temp_episodes", return_value={}), mock.patch.object(
                WORKER, "set_ego_video_codec", return_value={"returncode": 0}
            ), mock.patch.object(
                WORKER,
                "find_new_temp_episodes",
                return_value=[("/remote/dataset", "episode_0001-temp")],
            ), mock.patch.object(
                WORKER, "sync_episode_once", side_effect=KeyboardInterrupt
            ), mock.patch.object(
                WORKER,
                "write_status",
                side_effect=lambda _path, payload: snapshots.append(dict(payload)),
            ):
                self.assertEqual(WORKER.run_start(args), 0)

            self.assertTrue(snapshots)
            self.assertEqual(snapshots[0]["status"], "starting")
            self.assertNotIn("not_found", [payload["status"] for payload in snapshots])


if __name__ == "__main__":
    unittest.main()
