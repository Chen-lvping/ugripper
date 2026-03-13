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
        raise RuntimeError("sox not found; install it before generating a noise profile")
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
        raise RuntimeError(f"noise sample not written correctly: {capture_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Record ambient noise from the USB headset and generate a SoX noise profile.")
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--rate", type=int, default=16000)
    parser.add_argument("--profile", type=Path, default=PROJECT_ROOT / "audio" / "noise.prof")
    parser.add_argument("--keep-sample", action="store_true", help="Keep the processed mono noise sample next to the profile.")
    args = parser.parse_args()

    sox_bin = require_sox()
    target = configure_pulse_audio_env(require_source=True, disable_suspend_on_idle=True)
    env = pulse_audio_env()

    if target.unloaded_modules:
        print(f"Disabled PulseAudio suspend modules: {', '.join(target.unloaded_modules)}")

    profile_path = args.profile.resolve()
    profile_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(prefix="ugripper-noise-raw-", suffix=".wav", delete=False) as tmp:
        raw_sample_path = Path(tmp.name)
    processed_sample_path = raw_sample_path.with_name(raw_sample_path.stem + "_mono.wav")

    print(f"PulseAudio source selected: {target.source.name} ({target.source.description})")
    print(f"Recording ambient noise for {args.seconds:.1f}s ...")
    record_capture(raw_sample_path, target.source.name, env, args.seconds)

    try:
        subprocess.run([sox_bin, str(raw_sample_path), str(processed_sample_path), "remix", "1", "rate", str(args.rate)], check=True)
        subprocess.run([sox_bin, str(processed_sample_path), "-n", "noiseprof", str(profile_path)], check=True)
        print(f"Generated noise profile: {profile_path}")
        if args.keep_sample:
            kept_sample = profile_path.with_suffix(".sample.wav")
            processed_sample_path.replace(kept_sample)
            print(f"Kept processed noise sample: {kept_sample}")
    finally:
        raw_sample_path.unlink(missing_ok=True)
        if not args.keep_sample:
            processed_sample_path.unlink(missing_ok=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
