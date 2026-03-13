#!/usr/bin/env python3
import argparse
import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(PROJECT_ROOT / "audio"))

from pulse_audio_utils import configure_pulse_audio_env, pulse_audio_env


def generate_tone_wav(path: Path, seconds: float, frequency: float, volume: float) -> None:
    sample_rate = 44100
    amplitude = int(32767 * max(0.0, min(volume, 1.0)))
    total_samples = max(1, int(sample_rate * seconds))
    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(2)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        for index in range(total_samples):
            value = int(amplitude * math.sin(2.0 * math.pi * frequency * index / sample_rate))
            frame = struct.pack("<hh", value, value)
            wav_file.writeframesraw(frame)


def main() -> int:
    parser = argparse.ArgumentParser(description="Test the ugripper USB headset playback path via PulseAudio.")
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--frequency", type=float, default=1000.0)
    parser.add_argument("--volume", type=float, default=0.35)
    args = parser.parse_args()

    target = configure_pulse_audio_env(require_source=False, disable_suspend_on_idle=True)
    env = pulse_audio_env()

    if target.unloaded_modules:
        print(f"Disabled PulseAudio suspend modules: {', '.join(target.unloaded_modules)}")

    with tempfile.NamedTemporaryFile(prefix="ugripper-usb-audio-", suffix=".wav", delete=False) as tmp:
        wav_path = Path(tmp.name)
    generate_tone_wav(wav_path, seconds=args.seconds, frequency=args.frequency, volume=args.volume)

    print(f"PulseAudio sink selected: {target.sink.name} ({target.sink.description})")
    print(f"Playing {args.frequency:.1f}Hz tone for {args.seconds:.1f}s ...")
    try:
        subprocess.run(["paplay", f"--device={target.sink.name}", str(wav_path)], check=True, env=env)
    finally:
        wav_path.unlink(missing_ok=True)
    print("Playback test finished.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
