#!/usr/bin/env python3
import os
import re
import subprocess
import time
import pygame
import signal
import sys


def find_alsa_card_by_name(target: str):
    """
    Parse `aplay -l` and return card number for the sound card whose name contains `target`.
    """
    try:
        out = subprocess.check_output(["aplay", "-l"], stderr=subprocess.STDOUT, text=True)
    except Exception as e:
        print(f"ERROR: failed to run aplay -l: {e}")
        return None

    # match "card <n>: <name> ["
    for line in out.splitlines():
        m = re.search(r"^card\s+(\d+):\s*([^\[]+)\[", line.strip())
        if m:
            card_num = int(m.group(1))
            card_name = m.group(2).strip()
            if target.lower() in card_name.lower():
                return card_num

    # fallback: also try matching whole output
    m2 = re.search(rf"^card\s+(\d+):.*{re.escape(target)}", out, re.IGNORECASE | re.MULTILINE)
    if m2:
        return int(m2.group(1))

    return None


def setup_audio_device():
    # 强制 SDL 走 ALSA（避免默认走 pulse/pipewire 导致 Host is down）
    os.environ["SDL_AUDIODRIVER"] = "alsa"

    # 按声卡名找 card 编号
    target = "rockchipes8388"
    card = find_alsa_card_by_name(target)

    if card is None:
        # 找不到就退回 default
        print(f"WARNING: ALSA card '{target}' not found, fallback to default")
        return

    # 选设备 0
    dev = f"plughw:{card},0"
    os.environ["AUDIODEV"] = dev
    print(f"Using ALSA device: {dev} (matched card name: {target})")
    # 最小兜底：升级后可能出现播放开关被关，启动时恢复一次
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
                return v.strip().strip("\"").strip("'")
    except Exception as e:
        print(f"WARNING: failed to read {env_file}: {e}")
    return None


class AudioPlayer:
    def __init__(self):
        # 注册信号处理，捕获 SIGINT 和 SIGTERM 以便正常退出
        signal.signal(signal.SIGINT, self.shutdown)
        signal.signal(signal.SIGTERM, self.shutdown)

        setup_audio_device()

        pygame.mixer.init(frequency=44100, size=-16, channels=2, buffer=512)
        # Reserve a background channel for long/looping prompts (e.g. "writing")
        self.bg_channel = pygame.mixer.Channel(0)
        self.fx_channel = pygame.mixer.Channel(1)

        self.pipe_path = "/tmp/umi_audio_pipe"
        self.base_audio_dir = os.path.dirname(os.path.abspath(__file__))
        lang_from_file = read_env_file_value("UGRIPPER_LANG")
        self.lang = self.resolve_language(lang_from_file or "zh")
        self.audio_dirs = self.resolve_audio_dirs(self.lang)
        self.volume = 1.0

        # 兼容现有英文包中命名差异
        self.filename_aliases = {
            "recording_start.wav": ["recording_started.wav"],
        }

        self.sounds = {
            "ready": self.load_sound("ready.wav"),
            "audio_recording_start": self.load_sound("audio_recording_start.wav"),
            "audio_recording_stop": self.load_sound("audio_recording_stop.wav"),
            "pre_audio_recording": self.load_sound("pre_audio_recording.wav"),
            "post_audio_recording": self.load_sound("post_audio_recording.wav"),
            "no_reset_needed": self.load_sound("no_reset_needed.wav"),
            "writing": self.load_sound("writing.wav"),
            "shutdown": self.load_sound("shutdown.wav"),
            "recording_start": self.load_sound("recording_start.wav"),
            "reset_recording_start": self.load_sound("reset_recording_start.wav"),
            "recording_stop": self.load_sound("recording_stop.wav"),
            "error": self.load_sound("error.wav"),
            "validation_failed": self.load_sound("validation_failed.wav"),
            "calib_start": self.load_sound("calib_start.wav"),  # "准备进入校准"
            "calibrating": self.load_sound("calibrating.wav"),  # "正在校准中"
            "calib_done": self.load_sound("calib_done.wav"),    # "校准完成"
        }

        if not os.path.exists(self.pipe_path):
            os.mkfifo(self.pipe_path)

        print(f"Audio language: {self.lang}; search dirs: {self.audio_dirs}")
        print("Audio Player Ready. Waiting for commands...")

    def resolve_language(self, raw_lang: str) -> str:
        normalized = raw_lang.strip().strip("\"").strip("'").lower()
        if normalized in ("en", "english", "en_us", "en_gb"):
            return "en"
        if normalized in ("zh", "cn", "zh_cn", "chinese", "zh_hans", "zh-hans", "中文"):
            return "zh"
        print(f"WARNING: unsupported UGRIPPER_LANG='{raw_lang}', fallback to zh")
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
        """处理退出信号，避免子进程卡死"""
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
                print(f"Warning: Could not load {filepath}: {e}")

        print(f"Warning: Audio file not found for {filename}")
        return None

    def play_sound(self, sound_name):
        if sound_name in self.sounds and self.sounds[sound_name]:
            try:
                if sound_name == "writing":
                    # Loop "writing" until a later cue (e.g. "ready") interrupts it.
                    self.bg_channel.play(self.sounds[sound_name], loops=-1)
                else:
                    # Any non-writing cue interrupts "writing".
                    if self.bg_channel.get_busy():
                        self.bg_channel.stop()
                    self.fx_channel.play(self.sounds[sound_name])
                print(f"Playing: {sound_name}")
            except Exception as e:
                print(f"Error playing {sound_name}: {e}")
        else:
            print(f"Sound not available: {sound_name}")

    def run(self):
        while True:
            try:
                with open(self.pipe_path, "r", buffering=1) as pipe:
                    while True:
                        raw_line = pipe.readline()
                        if raw_line == "":
                            break  # EOF reached, reopen pipe

                        line = raw_line.strip()
                        if line == "exit":
                            print("Audio Player exiting...")
                            return
                        self.play_sound(line)
            except Exception as e:
                print(f"Pipe error: {e}, reopening in 1 second...")
                time.sleep(1)


def main():
    player = AudioPlayer()
    player.run()


if __name__ == "__main__":
    main()
