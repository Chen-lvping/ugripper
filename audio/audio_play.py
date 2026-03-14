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

from pulse_audio_utils import configure_pulse_audio_env, pulse_audio_env, get_forced_usb_audio_target

INPUT_EVENT_FORMAT = "llHHI"
INPUT_EVENT_SIZE = struct.calcsize(INPUT_EVENT_FORMAT)
EV_KEY = 0x01
KEY_MUTE = 113
KEY_VOLUMEDOWN = 114
KEY_VOLUMEUP = 115
DEFAULT_EVENT_SYMLINK = Path("/dev/input/ugripper_usb_audio_keys")
VOLUME_STEP = "5%"
RETRY_SEC = 1.0
PIPE_PATH = Path("/tmp/umi_audio_pipe")
READY_PATH = Path("/tmp/umi_audio_ready")
MIXER_FREQUENCY = 48000
MIXER_SIZE = -16
MIXER_CHANNELS = 2
MIXER_BUFFER = 512
WARMUP_MSEC = 120
LEADING_SILENCE_MSEC = 300


def setup_audio_device():
    target = configure_pulse_audio_env(require_source=False, disable_suspend_on_idle=True)
    if target.unloaded_modules:
        print(
            "[INFO] disabled PulseAudio suspend modules before playback init: "
            + ", ".join(target.unloaded_modules),
            flush=True,
        )
    print(
        f"Using PulseAudio USB headset sink: {target.sink.name} "
        f"({target.sink.description})",
        flush=True,
    )
    return target


def read_env_file_value(key: str, env_file: str = "/etc/environment"):
    try:
        with open(env_file, "r", encoding="utf-8") as file:
            for raw_line in file:
                line = raw_line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                current_key, value = line.split("=", 1)
                if current_key.strip() != key:
                    continue
                return value.strip().strip('"').strip("'")
    except Exception as exc:
        print(f"WARNING: failed to read {env_file}: {exc}", flush=True)
    return None


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

        if properties.get("ID_VENDOR_ID") != "0020" or properties.get("ID_MODEL_ID") != "0b21":
            continue
        if properties.get("ID_INPUT_KEY") != "1":
            continue
        return candidate

    raise FileNotFoundError("USB headset key input device not found")


def _run_pactl(*args: str) -> None:
    env = pulse_audio_env()
    subprocess.run(["pactl", *args], check=True, env=env)


def _handle_volume_key(code: int) -> None:
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
        self.pygame = None
        self.fx_channel = None
        self.bg_channel = None
        self.silence_sound = None

        self._remove_ready_marker()
        self._ensure_pipe()

        self.target = setup_audio_device()
        self.audio_env = os.environ.copy()
        boot_env = pulse_audio_env()
        if "XDG_RUNTIME_DIR" in boot_env:
            self.audio_env["XDG_RUNTIME_DIR"] = boot_env["XDG_RUNTIME_DIR"]
            os.environ["XDG_RUNTIME_DIR"] = boot_env["XDG_RUNTIME_DIR"]
        if "PULSE_SERVER" in boot_env:
            self.audio_env["PULSE_SERVER"] = boot_env["PULSE_SERVER"]
            os.environ["PULSE_SERVER"] = boot_env["PULSE_SERVER"]
        self.audio_env["PULSE_SINK"] = self.target.sink.name
        os.environ["PULSE_SINK"] = self.target.sink.name
        os.environ["SDL_AUDIODRIVER"] = "pulseaudio"

        self.init_mixer()

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
            "error": self.resolve_sound_path("error.wav"),
            "validation_failed": self.resolve_sound_path("validation_failed.wav"),
            "calib_start": self.resolve_sound_path("calib_start.wav"),
            "calibrating": self.resolve_sound_path("calibrating.wav"),
            "calib_done": self.resolve_sound_path("calib_done.wav"),
        }
        self.sounds = {
            sound_name: self.load_sound(sound_name, sound_path)
            for sound_name, sound_path in self.sound_paths.items()
        }

        self.key_thread.start()
        self.ready_path.write_text("ready\n", encoding="utf-8")
        print(f"Audio language: {self.lang}; search dirs: {self.audio_dirs}", flush=True)
        print("Audio daemon ready. Waiting for commands...", flush=True)

    def _ensure_pipe(self):
        try:
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

    def resolve_language(self, raw_lang: str) -> str:
        normalized = raw_lang.strip().strip('"').strip("'").lower()
        if normalized in ("en", "english", "en_us", "en_gb"):
            return "en"
        if normalized in ("zh", "cn", "zh_cn", "chinese", "zh_hans", "zh-hans", "中文"):
            return "zh"
        print(f"WARNING: unsupported UGRIPPER_LANG='{raw_lang}', fallback to zh", flush=True)
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
        print(f"Warning: Audio file not found for {filename}", flush=True)
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
            print(f"Warning: failed to load {sound_name} from {sound_path}: {exc}", flush=True)
            return None

    def play_sound(self, sound_name: str):
        sound = self.sounds.get(sound_name)
        if sound is None:
            print(f"Sound not available: {sound_name}", flush=True)
            return

        with self.playback_lock:
            if sound_name == "writing":
                if self.bg_channel is not None and self.bg_channel.get_busy():
                    return
                self.bg_channel.play(sound, loops=-1)
                print(f"Playing(loop): {sound_name}", flush=True)
                return

            if self.bg_channel is not None and self.bg_channel.get_busy():
                self.bg_channel.stop()
            if self.fx_channel is not None:
                self.fx_channel.stop()
                self.fx_channel.play(sound)
            print(f"Playing: {sound_name}", flush=True)

    def shutdown(self, signum=None, frame=None):
        print(f"Received signal {signum}, audio daemon exiting...", flush=True)
        self.stop_event.set()
        self._remove_ready_marker()
        with self.playback_lock:
            if self.bg_channel is not None:
                self.bg_channel.stop()
            if self.fx_channel is not None:
                self.fx_channel.stop()
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
        sys.exit(0)

    def volume_key_loop(self):
        fd = None

        while not self.stop_event.is_set():
            if fd is None:
                try:
                    device_path = _find_event_device()
                    fd = os.open(device_path, os.O_RDONLY | os.O_NONBLOCK)
                    print(f"[INFO] listening USB headset keys from {device_path}", flush=True)
                except Exception as exc:
                    print(f"[WARN] key device not ready: {exc}", flush=True)
                    self.stop_event.wait(RETRY_SEC)
                    continue

            try:
                readable, _, _ = select.select([fd], [], [], RETRY_SEC)
            except OSError as exc:
                if self.stop_event.is_set():
                    break
                print(f"[WARN] select failed, reopening input device: {exc}", flush=True)
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
                print(f"[WARN] read failed, reopening input device: {exc}", flush=True)
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
                print(f"[WARN] failed to handle volume key {code}: {exc}", flush=True)
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
                            print("Audio daemon exiting...", flush=True)
                            self.stop_event.set()
                            return
                        self.play_sound(line)
            except Exception as exc:
                print(f"Pipe error: {exc}, reopening in 1 second...", flush=True)
                self.stop_event.wait(1)


def main():
    player = AudioPlayer()
    player.run()


if __name__ == "__main__":
    main()
