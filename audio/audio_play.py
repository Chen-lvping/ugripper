#!/usr/bin/env python3
import os
import re
import signal
import subprocess
import sys
import time

import pygame


def log_stderr(level: str, message: str):
    print(f"[AUDIO][{level}] {message}", file=sys.stderr, flush=True)


def find_alsa_card_by_name(target: str):
    """
    Parse `aplay -l` and return card number for the sound card whose name contains `target`.
    """
    try:
        out = subprocess.check_output(["aplay", "-l"], stderr=subprocess.STDOUT, text=True)
    except Exception as e:
        log_stderr("ERROR", f"failed to run aplay -l: {e}")
        return None

    for line in out.splitlines():
        m = re.search(r"^card\s+(\d+):\s*([^\[]+)\[", line.strip())
        if m:
            card_num = int(m.group(1))
            card_name = m.group(2).strip()
            if target.lower() in card_name.lower():
                return card_num

    m2 = re.search(rf"^card\s+(\d+):.*{re.escape(target)}", out, re.IGNORECASE | re.MULTILINE)
    if m2:
        return int(m2.group(1))

    return None


def setup_audio_device():
    os.environ["SDL_AUDIODRIVER"] = "alsa"

    target = "rockchipes8388"
    card = find_alsa_card_by_name(target)

    if card is None:
        log_stderr("WARN", f"ALSA card '{target}' not found, fallback to default")
        return

    dev = f"plughw:{card},0"
    os.environ["AUDIODEV"] = dev

    for cmd in (
        ["amixer", "-c", str(card), "set", "PCM", "85%", "unmute"],
        ["amixer", "-c", str(card), "sset", "Speaker", "on"],
        ["amixer", "-c", str(card), "sset", "Headphone", "on"],
    ):
        subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def read_env_file_value(key: str, env_file: str = "/etc/environment"):
    try:
        with open(env_file, "r", encoding="utf-8") as f:
            for raw_line in f:
                line = raw_line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k.strip() != key:
                    continue
                return v.strip().strip('"').strip("'")
    except Exception as e:
        log_stderr("WARN", f"failed to read {env_file}: {e}")
    return None


class AudioPlayer:
    def __init__(self):
        signal.signal(signal.SIGINT, self.shutdown)
        signal.signal(signal.SIGTERM, self.shutdown)

        self.pipe_path = "/tmp/umi_audio_pipe"
        self.base_audio_dir = os.path.dirname(os.path.abspath(__file__))
        self.last_log_ts = {}
        lang_from_file = read_env_file_value("UGRIPPER_LANG")
        self.lang = self.resolve_language(lang_from_file or "zh")
        self.audio_dirs = self.resolve_audio_dirs(self.lang)
        self.volume = 1.0
        self.bg_channel = None
        self.fx_channel = None

        self.filename_aliases = {
            "recording_start.wav": ["recording_started.wav"],
        }
        self.sound_files = {
            "ready": "ready.wav",
            "audio_recording_start": "audio_recording_start.wav",
            "audio_recording_stop": "audio_recording_stop.wav",
            "pre_audio_recording": "pre_audio_recording.wav",
            "post_audio_recording": "post_audio_recording.wav",
            "no_reset_needed": "no_reset_needed.wav",
            "writing": "writing.wav",
            "shutdown": "shutdown.wav",
            "recording_start": "recording_start.wav",
            "reset_recording_start": "reset_recording_start.wav",
            "recording_stop": "recording_stop.wav",
            "error": "error.wav",
            "validation_failed": "validation_failed.wav",
            "calib_start": "calib_start.wav",
            "calibrating": "calibrating.wav",
            "calib_done": "calib_done.wav",
        }
        self.sounds = {}

        if not os.path.exists(self.pipe_path):
            os.mkfifo(self.pipe_path)

        self.initialize_audio_engine(initial=True)

        print(f"Audio language: {self.lang}; search dirs: {self.audio_dirs}")
        print("Audio Player Ready. Waiting for commands...")

    def log_throttled(self, level: str, key: str, message: str, interval_sec: float = 15.0):
        now = time.monotonic()
        last_ts = self.last_log_ts.get(key, 0.0)
        if now - last_ts < interval_sec:
            return
        self.last_log_ts[key] = now
        log_stderr(level, message)

    def resolve_language(self, raw_lang: str) -> str:
        normalized = raw_lang.strip().strip('"').strip("'").lower()
        if normalized in ("en", "english", "en_us", "en_gb"):
            return "en"
        if normalized in ("zh", "cn", "zh_cn", "chinese", "zh_hans", "zh-hans", "中文"):
            return "zh"
        self.log_throttled("WARN", "lang_fallback", f"unsupported UGRIPPER_LANG='{raw_lang}', fallback to zh", 60.0)
        return "zh"

    def resolve_audio_dirs(self, lang: str):
        dirs = []
        if lang == "en":
            en_dir = os.path.join(os.path.dirname(self.base_audio_dir), "audio_en")
            dirs.append(en_dir)
        dirs.append(self.base_audio_dir)
        return dirs

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

    def shutdown(self, signum, frame):
        print(f"Received signal {signum}, audio player exiting...")
        sys.exit(0)

    def load_sound(self, filename):
        for filepath in self.iter_candidate_paths(filename):
            if not os.path.exists(filepath):
                continue
            try:
                sound = pygame.mixer.Sound(filepath)
                sound.set_volume(self.volume)
                return sound
            except Exception as e:
                self.log_throttled(
                    "ERROR",
                    f"load:{filepath}:{type(e).__name__}",
                    f"failed to load sound '{filepath}': {e}",
                    30.0,
                )
        return None

    def reload_sounds(self):
        self.sounds = {
            sound_name: self.load_sound(filename)
            for sound_name, filename in self.sound_files.items()
        }

    def initialize_audio_engine(self, initial: bool = False):
        try:
            setup_audio_device()
            if pygame.mixer.get_init():
                pygame.mixer.quit()
            pygame.mixer.init(frequency=44100, size=-16, channels=2, buffer=512)
            self.bg_channel = pygame.mixer.Channel(0)
            self.fx_channel = pygame.mixer.Channel(1)
            self.reload_sounds()
            if not initial:
                self.log_throttled("WARN", "mixer_recovered", "audio mixer recovered", 5.0)
            return True
        except Exception as e:
            self.bg_channel = None
            self.fx_channel = None
            self.sounds = {}
            self.log_throttled(
                "ERROR",
                f"mixer_init:{type(e).__name__}",
                f"failed to initialize pygame mixer: {e}",
                10.0,
            )
            return False

    def recover_audio_engine(self, reason: str):
        self.log_throttled("WARN", f"recover:{reason}", f"attempting audio mixer recovery ({reason})", 10.0)
        return self.initialize_audio_engine(initial=False)

    def play_with_channels(self, sound_name: str):
        sound = self.sounds.get(sound_name)
        if sound is None:
            self.log_throttled(
                "WARN",
                f"sound_unavailable:{sound_name}",
                f"sound '{sound_name}' is unavailable",
                30.0,
            )
            return False

        if sound_name == "writing":
            self.bg_channel.play(sound, loops=-1)
        else:
            if self.bg_channel and self.bg_channel.get_busy():
                self.bg_channel.stop()
            self.fx_channel.play(sound)
        return True

    def play_sound(self, sound_name):
        if not pygame.mixer.get_init() or self.bg_channel is None or self.fx_channel is None:
            if not self.recover_audio_engine("mixer_uninitialized"):
                return

        try:
            if self.play_with_channels(sound_name):
                print(f"Playing: {sound_name}")
                return
        except Exception as e:
            self.log_throttled(
                "ERROR",
                f"play:{sound_name}:{type(e).__name__}",
                f"failed to play '{sound_name}': {e}",
                15.0,
            )

        if not self.recover_audio_engine(f"play_failed:{sound_name}"):
            return

        try:
            self.play_with_channels(sound_name)
        except Exception as e:
            self.log_throttled(
                "ERROR",
                f"play_retry:{sound_name}:{type(e).__name__}",
                f"failed to replay '{sound_name}' after recovery: {e}",
                20.0,
            )

    def run(self):
        while True:
            try:
                with open(self.pipe_path, "r", buffering=1) as pipe:
                    while True:
                        raw_line = pipe.readline()
                        if raw_line == "":
                            break

                        line = raw_line.strip()
                        if line == "exit":
                            print("Audio Player exiting...")
                            return
                        self.play_sound(line)
            except Exception as e:
                self.log_throttled(
                    "ERROR",
                    f"pipe:{type(e).__name__}",
                    f"pipe error, reopening in 1 second: {e}",
                    15.0,
                )
                time.sleep(1)


def main():
    player = AudioPlayer()
    player.run()


if __name__ == "__main__":
    main()
