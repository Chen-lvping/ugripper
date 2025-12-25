#!/usr/bin/env python3
import os
import time
import pygame


class AudioPlayer:
    def __init__(self):
        pygame.mixer.init()
        self.pipe_path = "/tmp/umi_audio_pipe"
        self.audio_dir = "./audio"

        # 预加载音频文件
        self.sounds = {
            "ready": self.load_sound("ready.wav"),
            "audio_recording_start": self.load_sound("audio_recording_start.wav"),
            "audio_recording_stop": self.load_sound("audio_recording_stop.wav"),
            "recording_start": self.load_sound("recording_start.wav"),
            "recording_stop": self.load_sound("recording_stop.wav"),
            "error": self.load_sound("error.wav"),
        }

        # 确保管道存在
        if not os.path.exists(self.pipe_path):
            os.mkfifo(self.pipe_path)

        print("Audio Player Ready. Waiting for commands...")

    def load_sound(self, filename):
        """加载音频文件"""
        filepath = os.path.join(self.audio_dir, filename)
        if os.path.exists(filepath):
            try:
                return pygame.mixer.Sound(filepath)
            except Exception as e:
                print(f"Warning: Could not load {filename}: {e}")
        else:
            print(f"Warning: Audio file not found: {filename}")
        return None

    def play_sound(self, sound_name):
        """播放指定声音"""
        if sound_name in self.sounds and self.sounds[sound_name]:
            try:
                self.sounds[sound_name].play()
                print(f"Playing: {sound_name}")
            except Exception as e:
                print(f"Error playing {sound_name}: {e}")
        else:
            print(f"Sound not available: {sound_name}")

    def run(self):
        """主循环，监听管道"""
        while True:
            try:
                with open(self.pipe_path, "r", buffering=1) as pipe:
                    while True:
                        line = pipe.readline().strip()
                        if line:
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
