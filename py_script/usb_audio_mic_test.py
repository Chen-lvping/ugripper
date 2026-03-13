#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(PROJECT_ROOT / "audio"))

from pulse_audio_utils import configure_pulse_audio_env, pulse_audio_env


def require_sox() -> str:
    sox_bin = shutil.which("sox")
    if sox_bin is None:
        raise RuntimeError("sox not found; install it before using this test script")
    return sox_bin


def record_capture(capture_path: Path, source_name: str, env: dict[str, str], seconds: float) -> None:
    timeout_bin = shutil.which("timeout")
    if timeout_bin is None:
        raise RuntimeError("timeout command not found")

    cmd = [
        timeout_bin,
        "-s",
        "INT",
        f"{max(0.2, seconds):.3f}s",
        "parecord",
        f"--device={source_name}",
        "--file-format=wav",
        "--fix-rate",
        "--fix-channels",
        "--format=s16le",
        str(capture_path),
    ]
    result = subprocess.run(cmd, env=env, check=False)
    if result.returncode not in (0, 124, 130):
        raise subprocess.CalledProcessError(result.returncode, cmd)
    if not capture_path.is_file() or capture_path.stat().st_size <= 44:
        raise RuntimeError(f"capture file not written correctly: {capture_path}")


def render_output(capture_path: Path, output_path: Path, rate: int, channels: int, denoise: bool, noise_profile: Path, amount: float) -> None:
    sox_bin = require_sox()
    prepared_path = capture_path.with_name(capture_path.stem + "_prepared.wav")
    denoised_path = capture_path.with_name(capture_path.stem + "_denoised.wav")

    try:
        if channels == 1:
            subprocess.run([sox_bin, str(capture_path), str(prepared_path), "remix", "1", "rate", str(rate)], check=True)
        else:
            subprocess.run([sox_bin, str(capture_path), str(prepared_path), "channels", str(channels), "rate", str(rate)], check=True)

        if denoise:
            if channels != 1:
                raise RuntimeError("denoise currently supports mono output only; use --channels 1")
            if not noise_profile.is_file():
                raise RuntimeError(
                    f"noise profile not found: {noise_profile}. Run py_script/usb_audio_noise_profile.py first."
                )
            subprocess.run(
                [sox_bin, str(prepared_path), str(denoised_path), "noisered", str(noise_profile), f"{amount:.2f}"],
                check=True,
            )
            subprocess.run([sox_bin, str(denoised_path), str(output_path), "norm"], check=True)
            print(f"Applied SoX denoise with profile: {noise_profile}")
        else:
            prepared_path.replace(output_path)
            print("Denoise disabled; saved post-processed raw capture")
    finally:
        prepared_path.unlink(missing_ok=True)
        denoised_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Record and verify the ugripper USB headset microphone via PulseAudio.")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--rate", type=int, default=16000)
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--playback", action="store_true", help="Play the recorded file back after capture.")
    parser.add_argument("--output", type=Path, default=None, help="Optional wav output path.")
    parser.add_argument("--noise-profile", type=Path, default=PROJECT_ROOT / "audio" / "noise.prof")
    parser.add_argument("--denoise-amount", type=float, default=0.15)
    parser.add_argument("--denoise", action="store_true", help="Apply SoX noisered using an explicitly prepared noise profile.")
    args = parser.parse_args()

    target = configure_pulse_audio_env(require_source=True, disable_suspend_on_idle=True)
    env = pulse_audio_env()

    if target.unloaded_modules:
        print(f"Disabled PulseAudio suspend modules: {', '.join(target.unloaded_modules)}")

    if args.output is None:
        tmp = tempfile.NamedTemporaryFile(prefix="ugripper-usb-mic-", suffix=".wav", delete=False)
        output_path = Path(tmp.name)
        tmp.close()
    else:
        output_path = args.output.resolve()
        output_path.parent.mkdir(parents=True, exist_ok=True)

    capture_path = output_path.with_name(output_path.stem + "_capture.wav")
    print(f"PulseAudio source selected: {target.source.name} ({target.source.description})")
    print("Recording microphone sample ...")
    record_capture(capture_path, target.source.name, env, args.seconds)
    render_output(capture_path, output_path, args.rate, args.channels, args.denoise, args.noise_profile.resolve(), args.denoise_amount)
    capture_path.unlink(missing_ok=True)

    print(f"Saved recording to {output_path}")

    if args.playback:
        print("Playing recorded sample back ...")
        subprocess.run(["paplay", f"--device={target.sink.name}", str(output_path)], check=True, env=env)

    print("Microphone test finished.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
