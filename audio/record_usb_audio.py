#!/usr/bin/env python3
import argparse
import signal
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from pulse_audio_utils import configure_pulse_audio_env, pulse_audio_env

parecord_process: subprocess.Popen | None = None
forwarded_signal = signal.SIGINT


def forward_signal(signum: int, _frame) -> None:
    global forwarded_signal
    forwarded_signal = signum
    if parecord_process is not None and parecord_process.poll() is None:
        parecord_process.send_signal(signum)


def main() -> int:
    global parecord_process

    parser = argparse.ArgumentParser(description="Record audio from the preferred PulseAudio source.")
    parser.add_argument("--output", required=True, type=Path, help="Target wav path")
    parser.add_argument("--seconds", type=float, default=0.0, help="Optional max duration; <=0 means wait for SIGINT/SIGTERM")
    args = parser.parse_args()

    target = configure_pulse_audio_env(require_source=True, disable_suspend_on_idle=True)
    if target.unloaded_modules:
        print(
            "[INFO] disabled PulseAudio suspend modules before capture init: "
            + ", ".join(target.unloaded_modules),
            flush=True,
        )
    env = pulse_audio_env()
    if "XDG_RUNTIME_DIR" in env:
        env["XDG_RUNTIME_DIR"] = env["XDG_RUNTIME_DIR"]
    if "PULSE_SERVER" in env:
        env["PULSE_SERVER"] = env["PULSE_SERVER"]
    env["SDL_AUDIODRIVER"] = "pulse"
    env["PULSE_SOURCE"] = target.source.name

    output_path = args.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        "parecord",
        f"--device={target.source.name}",
        "--file-format=wav",
        "--fix-rate",
        "--fix-channels",
        "--format=s16le",
        str(output_path),
    ]

    signal.signal(signal.SIGINT, forward_signal)
    signal.signal(signal.SIGTERM, forward_signal)

    parecord_process = subprocess.Popen(cmd, env=env)

    try:
        if args.seconds > 0:
            try:
                parecord_process.wait(timeout=max(0.2, args.seconds))
            except subprocess.TimeoutExpired:
                parecord_process.send_signal(signal.SIGINT)
                parecord_process.wait()
        else:
            parecord_process.wait()
    finally:
        if parecord_process.poll() is None:
            parecord_process.kill()
            parecord_process.wait()

    if parecord_process.returncode not in (0, -signal.SIGINT, -signal.SIGTERM, 130, 143):
        return parecord_process.returncode

    if not output_path.is_file() or output_path.stat().st_size <= 44:
        print(f"capture file not written correctly: {output_path}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
