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
    # 设置 PCM 音量为 85%
    try:
        subprocess.run(
            ["amixer", "-c", str(card), "set", "PCM", "85%", "unmute"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        print(f"ALSA PCM volume set to 85% on card {card}")
    except subprocess.CalledProcessError:
        print("WARNING: Failed to set PCM volume (control may not exist)")


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
        self.audio_dir = os.path.dirname(os.path.abspath(__file__))
        self.volume = 1.0

        self.sounds = {
            "ready": self.load_sound("ready.wav"),
            "audio_recording_start": self.load_sound("audio_recording_start.wav"),
            "audio_recording_stop": self.load_sound("audio_recording_stop.wav"),
            "pre_audio_recording": self.load_sound("pre_audio_recording.wav"),
            "post_audio_recording": self.load_sound("post_audio_recording.wav"),
            "writing": self.load_sound("writing.wav"),
            "recording_start": self.load_sound("recording_start.wav"),
            "recording_stop": self.load_sound("recording_stop.wav"),
            "error": self.load_sound("error.wav"),
            "validation_failed": self.load_sound("validation_failed.wav"),
            "calib_start": self.load_sound("calib_start.wav"),  # "准备进入校准"
            "calibrating": self.load_sound("calibrating.wav"),  # "正在校准中"
            "calib_done": self.load_sound("calib_done.wav"),    # "校准完成"
        }

        if not os.path.exists(self.pipe_path):
            os.mkfifo(self.pipe_path)

        print("Audio Player Ready. Waiting for commands...")

    def shutdown(self, signum, frame):
        """处理退出信号，避免子进程卡死"""
        print(f"Received signal {signum}, audio player exiting...")
        sys.exit(0)

    def load_sound(self, filename):
        filepath = os.path.join(self.audio_dir, filename)
        if os.path.exists(filepath):
            try:
                sound = pygame.mixer.Sound(filepath)
                sound.set_volume(self.volume)
                return sound
            except Exception as e:
                print(f"Warning: Could not load {filename}: {e}")
        else:
            print(f"Warning: Audio file not found: {filepath}")
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
