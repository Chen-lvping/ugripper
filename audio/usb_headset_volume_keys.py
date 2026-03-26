#!/usr/bin/env python3
import os
import select
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

from pulse_audio_utils import (
    get_forced_usb_audio_target,
    is_supported_usb_audio_input_device,
    pulse_audio_env,
)

INPUT_EVENT_FORMAT = "llHHI"
INPUT_EVENT_SIZE = struct.calcsize(INPUT_EVENT_FORMAT)
EV_KEY = 0x01
KEY_MUTE = 113
KEY_VOLUMEDOWN = 114
KEY_VOLUMEUP = 115
DEFAULT_EVENT_SYMLINK = Path("/dev/input/ugripper_usb_audio_keys")
VOLUME_STEP = "5%"
RETRY_SEC = 1.0


def _find_event_device() -> Path:
    if DEFAULT_EVENT_SYMLINK.exists():
        return DEFAULT_EVENT_SYMLINK

    for candidate in sorted(Path("/dev/input").glob("event*")):
        try:
            out = subprocess.check_output(
                ["udevadm", "info", "--query=property", f"--name={candidate}"],
                stderr=subprocess.STDOUT,
                text=True,
            )
        except Exception:
            continue

        props = {}
        for line in out.splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            props[key.strip()] = value.strip()

        if not is_supported_usb_audio_input_device(props):
            continue
        if props.get("ID_INPUT_KEY") != "1":
            continue
        return candidate

    raise FileNotFoundError("supported USB headset key input device not found")


def _run_pactl(*args: str) -> None:
    env = pulse_audio_env()
    subprocess.run(["pactl", *args], check=True, env=env)


def _handle_key(code: int) -> None:
    target = get_forced_usb_audio_target(require_source=False)
    sink = target.sink.name

    if code == KEY_VOLUMEUP:
        _run_pactl("set-sink-mute", sink, "0")
        _run_pactl("set-sink-volume", sink, f"+{VOLUME_STEP}")
        print(f"[INFO] USB headset volume up -> {sink}", flush=True)
    elif code == KEY_VOLUMEDOWN:
        _run_pactl("set-sink-mute", sink, "0")
        _run_pactl("set-sink-volume", sink, f"-{VOLUME_STEP}")
        print(f"[INFO] USB headset volume down -> {sink}", flush=True)
    elif code == KEY_MUTE:
        _run_pactl("set-sink-mute", sink, "toggle")
        print(f"[INFO] USB headset mute toggle -> {sink}", flush=True)


class VolumeKeyListener:
    def __init__(self):
        self.running = True
        self.fd = None
        self.device_path: Path | None = None
        self.device_wait_logged = False
        signal.signal(signal.SIGINT, self._shutdown)
        signal.signal(signal.SIGTERM, self._shutdown)

    def _shutdown(self, signum, frame):
        print(f"[INFO] volume key listener exiting on signal {signum}", flush=True)
        self.running = False
        self._close_fd()

    def _close_fd(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def _open_device(self):
        self.device_path = _find_event_device()
        self.fd = os.open(self.device_path, os.O_RDONLY | os.O_NONBLOCK)
        self.device_wait_logged = False
        print(f"[INFO] listening USB headset keys from {self.device_path}", flush=True)

    def run(self):
        while self.running:
            if self.fd is None:
                try:
                    self._open_device()
                except Exception as exc:
                    if not self.device_wait_logged:
                        print(f"[WARN] key device not ready: {exc}", flush=True)
                        self.device_wait_logged = True
                    time.sleep(RETRY_SEC)
                    continue

            try:
                readable, _, _ = select.select([self.fd], [], [], RETRY_SEC)
            except OSError as exc:
                if not self.running:
                    break
                print(f"[WARN] select failed, reopening input device: {exc}", flush=True)
                self._close_fd()
                time.sleep(RETRY_SEC)
                continue

            if not readable:
                continue

            try:
                data = os.read(self.fd, INPUT_EVENT_SIZE)
            except OSError as exc:
                print(f"[WARN] read failed, reopening input device: {exc}", flush=True)
                self._close_fd()
                time.sleep(RETRY_SEC)
                continue

            if len(data) != INPUT_EVENT_SIZE:
                continue

            _, _, event_type, code, value = struct.unpack(INPUT_EVENT_FORMAT, data)
            if event_type != EV_KEY or value not in (1, 2):
                continue
            if code not in (KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE):
                continue

            try:
                _handle_key(code)
            except Exception as exc:
                print(f"[WARN] failed to handle volume key {code}: {exc}", flush=True)
                time.sleep(RETRY_SEC)


def main() -> int:
    listener = VolumeKeyListener()
    listener.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
