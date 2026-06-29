#!/usr/bin/env python3
import io
import os
import select
import signal
import struct
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path

from audio_common import audio_log, read_env_file_value
from pulse_audio_utils import (
    PulseAudioProbeError,
    PulseAudioTargetNotFoundError,
    configure_pulse_audio_env,
    get_forced_usb_audio_target,
    is_supported_usb_audio_input_device,
    probe_forced_usb_audio_target,
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
PIPE_PATH = Path("/dev/shm/ugripper/umi_audio_pipe")
READY_PATH = Path("/dev/shm/ugripper/umi_audio_ready")
MIXER_FREQUENCY = 48000
MIXER_SIZE = -16
MIXER_CHANNELS = 2
MIXER_BUFFER = 512
WARMUP_MSEC = 120
LEADING_SILENCE_MSEC = 300
LOOPING_SOUNDS = {"writing", "calibrating"}


def setup_audio_device():
    target = configure_pulse_audio_env(require_source=False, disable_suspend_on_idle=True)
    return target


def _find_event_device() -> Path:
    if DEFAULT_EVENT_SYMLINK.exists():
        return DEFAULT_EVENT_SYMLINK

    for candidate in sorted(Path("/dev/input").glob("event*")):
        try:
            output = subprocess.check_output(
                ["udevadm", "info", "--query=property", f"--name={candidate}"],
                stderr=subprocess.STDOUT,
                text=True,
            )
        except Exception:
            continue

        properties = {}
        for line in output.splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            properties[key.strip()] = value.strip()

        if not is_supported_usb_audio_input_device(properties):
            continue
        if properties.get("ID_INPUT_KEY") != "1":
            continue
        return candidate

    raise FileNotFoundError("supported USB headset key input device not found")


def _run_pactl(*args: str) -> None:
    env = pulse_audio_env()
    subprocess.run(["pactl", *args], check=True, env=env)


def _handle_volume_key(code: int) -> None:
    target = get_forced_usb_audio_target(require_source=False)
    sink = target.sink.name

    if code == KEY_VOLUMEUP:
        _run_pactl("set-sink-mute", sink, "0")
        _run_pactl("set-sink-volume", sink, f"+{VOLUME_STEP}")
        audio_log("INFO", f"preferred audio volume up -> {sink}")
    elif code == KEY_VOLUMEDOWN:
        _run_pactl("set-sink-mute", sink, "0")
        _run_pactl("set-sink-volume", sink, f"-{VOLUME_STEP}")
        audio_log("INFO", f"preferred audio volume down -> {sink}")
    elif code == KEY_MUTE:
        _run_pactl("set-sink-mute", sink, "toggle")
        audio_log("INFO", f"preferred audio mute toggle -> {sink}")


class AudioPlayer:
    def __init__(self):
        signal.signal(signal.SIGINT, self.shutdown)
        signal.signal(signal.SIGTERM, self.shutdown)

        self.pipe_path = PIPE_PATH
        self.ready_path = READY_PATH
        self.base_audio_dir = os.path.dirname(os.path.abspath(__file__))
        self.stop_event = threading.Event()
        self.playback_lock = threading.Lock()
        self.key_thread = threading.Thread(target=self.volume_key_loop, name="usb-headset-keys", daemon=True)
        self.backend_thread = threading.Thread(target=self.backend_loop, name="usb-headset-backend", daemon=True)
        self.pygame = None
        self.fx_channel = None
        self.bg_channel = None
        self.silence_sound = None
        self.target = None
        self.audio_env = os.environ.copy()
        self.backend_wait_logged = False
        self.key_device_wait_logged = False
        self.active_sound_name = None
        self.active_sound_looping = False

        self._remove_ready_marker()
        self._ensure_pipe()

        lang_from_file = read_env_file_value("UGRIPPER_LANG")
        self.lang = self.resolve_language(lang_from_file or "zh")
        self.audio_dirs = self.resolve_audio_dirs(self.lang)

        self.filename_aliases = {}
        self.sound_paths = {
            "ready": self.resolve_sound_path("ready.wav"),
            "audio_recording_stop": self.resolve_sound_path("audio_recording_stop.wav"),
            "pre_audio_recording": self.resolve_sound_path("pre_audio_recording.wav"),
            "post_audio_recording": self.resolve_sound_path("post_audio_recording.wav"),
            "no_reset_needed": self.resolve_sound_path("no_reset_needed.wav"),
            "writing": self.resolve_sound_path("writing.wav"),
            "shutdown": self.resolve_sound_path("shutdown.wav"),
            "recording_started": self.resolve_sound_path("recording_started.wav"),
            "reset_recording_start": self.resolve_sound_path("reset_recording_start.wav"),
            "recording_stop": self.resolve_sound_path("recording_stop.wav"),
            "umount": self.resolve_sound_path("umount.wav"),
            "error": self.resolve_sound_path("error.wav"),
            "validation_failed": self.resolve_sound_path("validation_failed.wav"),
            "calib_start": self.resolve_sound_path("calib_start.wav"),
            "calibrating": self.resolve_sound_path("calibrating.wav"),
            "calib_done": self.resolve_sound_path("calib_done.wav"),
            "left_tcam_l_damaged": self.resolve_sound_path("left_tcam_l_damaged.wav"),
            "left_tcam_r_damaged": self.resolve_sound_path("left_tcam_r_damaged.wav"),
            "right_tcam_l_damaged": self.resolve_sound_path("right_tcam_l_damaged.wav"),
            "right_tcam_r_damaged": self.resolve_sound_path("right_tcam_r_damaged.wav"),
        }
        self.sounds = {
            sound_name: None for sound_name in self.sound_paths
        }

        self.key_thread.start()
        self.backend_thread.start()
        audio_log("INFO", f"audio language: {self.lang}; search dirs: {self.audio_dirs}")
        audio_log("INFO", "audio daemon started. Waiting for preferred audio device and commands...")

    def _ensure_pipe(self):
        try:
            self.pipe_path.parent.mkdir(parents=True, exist_ok=True)
            if self.pipe_path.exists() and not self.pipe_path.is_fifo():
                self.pipe_path.unlink()
            if not self.pipe_path.exists():
                os.mkfifo(self.pipe_path)
        except Exception as exc:
            raise RuntimeError(f"failed to prepare audio pipe {self.pipe_path}: {exc}") from exc

    def _remove_ready_marker(self):
        try:
            if self.ready_path.exists():
                self.ready_path.unlink()
        except OSError:
            pass

    def _write_ready_marker(self):
        self.ready_path.parent.mkdir(parents=True, exist_ok=True)
        self.ready_path.write_text("ready\n", encoding="utf-8")

    def init_mixer(self):
        try:
            import pygame
        except Exception as exc:
            raise RuntimeError(f"failed to import pygame: {exc}") from exc

        self.pygame = pygame
        pygame.mixer.pre_init(
            frequency=MIXER_FREQUENCY,
            size=MIXER_SIZE,
            channels=MIXER_CHANNELS,
            buffer=MIXER_BUFFER,
        )
        pygame.init()
        if not pygame.mixer.get_init():
            pygame.mixer.init(
                frequency=MIXER_FREQUENCY,
                size=MIXER_SIZE,
                channels=MIXER_CHANNELS,
                buffer=MIXER_BUFFER,
            )

        pygame.mixer.set_num_channels(5)
        self.fx_channel = pygame.mixer.Channel(0)
        self.bg_channel = pygame.mixer.Channel(1)
        self.silence_sound = pygame.mixer.Sound(buffer=b"\x00" * int(MIXER_FREQUENCY * MIXER_CHANNELS * 2 * WARMUP_MSEC / 1000))

        self.fx_channel.play(self.silence_sound)
        while self.fx_channel.get_busy() and not self.stop_event.is_set():
            pygame.time.wait(10)
        pygame.time.wait(20)

    def _apply_audio_target(self, target):
        self.audio_env = os.environ.copy()
        boot_env = pulse_audio_env()
        if "XDG_RUNTIME_DIR" in boot_env:
            self.audio_env["XDG_RUNTIME_DIR"] = boot_env["XDG_RUNTIME_DIR"]
            os.environ["XDG_RUNTIME_DIR"] = boot_env["XDG_RUNTIME_DIR"]
        if "PULSE_SERVER" in boot_env:
            self.audio_env["PULSE_SERVER"] = boot_env["PULSE_SERVER"]
            os.environ["PULSE_SERVER"] = boot_env["PULSE_SERVER"]
        self.audio_env["PULSE_SINK"] = target.sink.name
        os.environ["PULSE_SINK"] = target.sink.name
        if target.source is not None:
            self.audio_env["PULSE_SOURCE"] = target.source.name
            os.environ["PULSE_SOURCE"] = target.source.name
        os.environ["SDL_AUDIODRIVER"] = "pulseaudio"

        self.init_mixer()
        self.sounds = {
            sound_name: self.load_sound(sound_name, sound_path)
            for sound_name, sound_path in self.sound_paths.items()
        }
        self.target = target
        self._write_ready_marker()

    def _stop_playback_locked(self):
        if self.bg_channel is not None:
            self.bg_channel.stop()
        if self.fx_channel is not None:
            self.fx_channel.stop()
        self.active_sound_name = None
        self.active_sound_looping = False

    def _teardown_audio_backend_locked(self):
        self._remove_ready_marker()
        self._stop_playback_locked()
        if self.pygame is not None:
            try:
                self.pygame.mixer.stop()
                self.pygame.mixer.quit()
            except Exception:
                pass
            try:
                self.pygame.quit()
            except Exception:
                pass

        self.pygame = None
        self.fx_channel = None
        self.bg_channel = None
        self.silence_sound = None
        self.target = None
        self.sounds = {
            sound_name: None for sound_name in self.sound_paths
        }

    def refresh_audio_backend(self, *, log_missing: bool) -> bool:
        current_target = self.target
        probed_target = probe_forced_usb_audio_target(require_source=False)
        current_sink = current_target.sink.name if current_target is not None else None

        if probed_target is None:
            with self.playback_lock:
                had_backend = self.target is not None or self.pygame is not None
                self._teardown_audio_backend_locked()
            if had_backend:
                audio_log("WARN", "no usable PulseAudio sink available, audio playback paused")
            elif log_missing and not self.backend_wait_logged:
                audio_log("WARN", "audio backend waiting for a usable PulseAudio sink")
            self.backend_wait_logged = True
            return False

        if current_sink == probed_target.sink.name and self.pygame is not None:
            self.backend_wait_logged = False
            return True

        try:
            target = setup_audio_device()
        except (PulseAudioTargetNotFoundError, PulseAudioProbeError) as exc:
            if log_missing and not self.backend_wait_logged:
                audio_log("WARN", f"audio backend waiting for a usable PulseAudio target: {exc}")
            self.backend_wait_logged = True
            return False

        with self.playback_lock:
            previous_sink = self.target.sink.name if self.target is not None else None
            self._teardown_audio_backend_locked()
            self._apply_audio_target(target)

        if target.unloaded_modules:
            audio_log(
                "INFO",
                "disabled PulseAudio suspend modules before playback init: "
                + ", ".join(target.unloaded_modules),
            )
        if previous_sink and previous_sink != target.sink.name:
            audio_log("INFO", f"preferred audio sink changed: {previous_sink} -> {target.sink.name}")
        selection_label = "USB headset" if target.is_usb else "system default"
        audio_log(
            "INFO",
            f"audio backend bound to {selection_label} sink: {target.sink.name} "
            f"({target.sink.description})",
        )
        self.backend_wait_logged = False
        return True

    def resolve_language(self, raw_lang: str) -> str:
        normalized = raw_lang.strip().strip('"').strip("'").lower()
        if normalized in ("en", "english", "en_us", "en_gb"):
            return "en"
        if normalized in ("zh", "cn", "zh_cn", "chinese", "zh_hans", "zh-hans", "中文"):
            return "zh"
        audio_log("WARN", f"unsupported UGRIPPER_LANG='{raw_lang}', fallback to zh")
        return "zh"

    def resolve_audio_dirs(self, lang: str):
        directories = []
        if lang == "en":
            directories.append(os.path.join(os.path.dirname(self.base_audio_dir), "audio_en"))
        directories.append(self.base_audio_dir)
        return directories

    def iter_candidate_paths(self, filename: str):
        names = [filename]
        names.extend(self.filename_aliases.get(filename, []))

        seen = set()
        for audio_dir in self.audio_dirs:
            for name in names:
                path = os.path.join(audio_dir, name)
                if path in seen:
                    continue
                seen.add(path)
                yield path

    def resolve_sound_path(self, filename: str):
        for filepath in self.iter_candidate_paths(filename):
            if os.path.exists(filepath):
                return filepath
        audio_log("WARN", f"audio file not found for {filename}")
        return None

    def add_leading_silence(self, sound_path: str, leading_msec: int) -> io.BytesIO:
        with wave.open(sound_path, "rb") as src:
            params = src.getparams()
            frames = src.readframes(src.getnframes())

        bytes_per_frame = params.sampwidth * params.nchannels
        silence_frames = max(1, int(params.framerate * leading_msec / 1000))
        silence = b"\x00" * (silence_frames * bytes_per_frame)

        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as dst:
            dst.setnchannels(params.nchannels)
            dst.setsampwidth(params.sampwidth)
            dst.setframerate(params.framerate)
            dst.writeframes(silence)
            dst.writeframes(frames)
        buffer.seek(0)
        return buffer

    def load_sound(self, sound_name: str, sound_path: str | None):
        if not sound_path:
            return None
        try:
            wav_buffer = self.add_leading_silence(sound_path, LEADING_SILENCE_MSEC)
            return self.pygame.mixer.Sound(file=wav_buffer)
        except Exception as exc:
            audio_log("WARN", f"failed to load {sound_name} from {sound_path}: {exc}")
            return None

    def play_sound(self, sound_name: str):
        if not self.refresh_audio_backend(log_missing=True):
            audio_log("WARN", f"skip sound because no audio backend is ready: {sound_name}")
            return

        looping = sound_name in LOOPING_SOUNDS
        sound = self.sounds.get(sound_name)

        with self.playback_lock:
            if (
                looping
                and self.active_sound_looping
                and self.active_sound_name == sound_name
                and self.bg_channel is not None
                and self.bg_channel.get_busy()
            ):
                audio_log("INFO", f"playing(loop): {sound_name} (unchanged)")
                return

            self._stop_playback_locked()

            if sound is None:
                audio_log("WARN", f"sound not available: {sound_name}")
                return

            if looping:
                if self.bg_channel is None:
                    audio_log("WARN", f"loop channel unavailable, skip sound: {sound_name}")
                    return
                self.bg_channel.play(sound, loops=-1)
                self.active_sound_name = sound_name
                self.active_sound_looping = True
                audio_log("INFO", f"playing(loop): {sound_name}")
                return

            if self.fx_channel is None:
                audio_log("WARN", f"fx channel unavailable, skip sound: {sound_name}")
                return
            self.fx_channel.play(sound)
            self.active_sound_name = sound_name
            self.active_sound_looping = False
            audio_log("INFO", f"playing: {sound_name}")

    def shutdown(self, signum=None, frame=None):
        audio_log("INFO", f"received signal {signum}, audio daemon exiting...")
        self.stop_event.set()
        self._remove_ready_marker()
        with self.playback_lock:
            self._teardown_audio_backend_locked()
        sys.exit(0)

    def backend_loop(self):
        while not self.stop_event.is_set():
            try:
                self.refresh_audio_backend(log_missing=False)
            except Exception as exc:
                audio_log("WARN", f"audio backend refresh failed: {exc}")
            self.stop_event.wait(RETRY_SEC)

    def volume_key_loop(self):
        fd = None

        while not self.stop_event.is_set():
            if fd is None:
                try:
                    device_path = _find_event_device()
                    fd = os.open(device_path, os.O_RDONLY | os.O_NONBLOCK)
                    self.key_device_wait_logged = False
                    audio_log("INFO", f"listening USB headset keys from {device_path}")
                except Exception as exc:
                    if not self.key_device_wait_logged:
                        audio_log("WARN", f"key device not ready: {exc}")
                        self.key_device_wait_logged = True
                    self.stop_event.wait(RETRY_SEC)
                    continue

            try:
                readable, _, _ = select.select([fd], [], [], RETRY_SEC)
            except OSError as exc:
                if self.stop_event.is_set():
                    break
                audio_log("WARN", f"select failed, reopening input device: {exc}")
                try:
                    os.close(fd)
                except OSError:
                    pass
                fd = None
                self.stop_event.wait(RETRY_SEC)
                continue

            if not readable:
                continue

            try:
                data = os.read(fd, INPUT_EVENT_SIZE)
            except OSError as exc:
                audio_log("WARN", f"read failed, reopening input device: {exc}")
                try:
                    os.close(fd)
                except OSError:
                    pass
                fd = None
                self.stop_event.wait(RETRY_SEC)
                continue

            if len(data) != INPUT_EVENT_SIZE:
                continue

            _, _, event_type, code, value = struct.unpack(INPUT_EVENT_FORMAT, data)
            if event_type != EV_KEY or value not in (1, 2):
                continue
            if code not in (KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE):
                continue

            try:
                _handle_volume_key(code)
            except Exception as exc:
                audio_log("WARN", f"failed to handle volume key {code}: {exc}")
                self.stop_event.wait(RETRY_SEC)

        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass

    def run(self):
        while not self.stop_event.is_set():
            try:
                with open(self.pipe_path, "r", buffering=1) as pipe:
                    while not self.stop_event.is_set():
                        raw_line = pipe.readline()
                        if raw_line == "":
                            break

                        line = raw_line.strip()
                        if not line:
                            continue
                        if line == "exit":
                            audio_log("INFO", "audio daemon exiting...")
                            self.stop_event.set()
                            return
                        self.play_sound(line)
            except Exception as exc:
                audio_log("WARN", f"pipe error: {exc}, reopening in 1 second...")
                self.stop_event.wait(1)


def main():
    player = AudioPlayer()
    player.run()


if __name__ == "__main__":
    main()
