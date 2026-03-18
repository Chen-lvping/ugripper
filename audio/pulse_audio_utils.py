#!/usr/bin/env python3
import json
import os
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

USB_AUDIO_VENDOR_ID = "0020"
USB_AUDIO_PRODUCT_ID = "0b21"
USB_AUDIO_VENDOR_NAME = "liyuany"
USB_AUDIO_PRODUCT_NAME = "USB Audio"
USB_AUDIO_SINK_PREFIX = "alsa_output.usb-liyuany_USB_Audio"
USB_AUDIO_SOURCE_PREFIX = "alsa_input.usb-liyuany_USB_Audio"
DEFAULT_DISABLED_MODULES = ("module-suspend-on-idle",)
MODULE_APPLY_SETTLE_SEC = 0.2


@dataclass
class PulseAudioEndpoint:
    name: str
    description: str
    card: Optional[int]
    properties: dict


@dataclass
class PulseAudioTarget:
    sink: PulseAudioEndpoint
    source: Optional[PulseAudioEndpoint]
    server: Optional[str]
    unloaded_modules: tuple[str, ...] = ()


class PulseAudioTargetNotFoundError(RuntimeError):
    pass


class PulseAudioProbeError(RuntimeError):
    pass


def _bootstrap_pulse_env() -> dict[str, str]:
    env = os.environ.copy()
    runtime_dir = env.get("XDG_RUNTIME_DIR")
    uid_runtime_dir = Path(f"/run/user/{os.getuid()}")

    if not runtime_dir or not Path(runtime_dir).exists():
        if uid_runtime_dir.exists():
            runtime_dir = str(uid_runtime_dir)
            env["XDG_RUNTIME_DIR"] = runtime_dir

    if runtime_dir:
        pulse_native = Path(runtime_dir) / "pulse" / "native"
        if pulse_native.exists() and not env.get("PULSE_SERVER"):
            env["PULSE_SERVER"] = f"unix:{pulse_native}"

    return env


def _run_pactl_json(*args: str):
    env = _bootstrap_pulse_env()
    try:
        out = subprocess.check_output(
            ["pactl", "-f", "json", *args],
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
    except FileNotFoundError as exc:
        raise PulseAudioProbeError("pactl not found; pulseaudio-utils is required") from exc
    except subprocess.CalledProcessError as exc:
        raise PulseAudioProbeError(f"pactl {' '.join(args)} failed: {exc.output.strip()}") from exc

    try:
        return json.loads(out)
    except json.JSONDecodeError as exc:
        raise PulseAudioProbeError(f"failed to parse pactl json output for {' '.join(args)}") from exc


def _run_pactl_text(*args: str) -> str:
    env = _bootstrap_pulse_env()
    try:
        return subprocess.check_output(
            ["pactl", *args],
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
    except FileNotFoundError as exc:
        raise PulseAudioProbeError("pactl not found; pulseaudio-utils is required") from exc
    except subprocess.CalledProcessError as exc:
        raise PulseAudioProbeError(f"pactl {' '.join(args)} failed: {exc.output.strip()}") from exc


def _read_pactl_info() -> dict[str, str]:
    try:
        out = _run_pactl_text("info")
    except PulseAudioProbeError:
        return {}

    info: dict[str, str] = {}
    for line in out.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        info[key.strip()] = value.strip()
    return info


def _card_from_properties(properties: dict) -> Optional[int]:
    raw = properties.get("alsa.card")
    if raw is None:
        return None
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


def _matches_target(properties: dict, endpoint_name: str, prefix: str) -> bool:
    if properties.get("device.bus") == "usb" and \
       properties.get("device.vendor.id") == USB_AUDIO_VENDOR_ID and \
       properties.get("device.product.id") == USB_AUDIO_PRODUCT_ID:
        return True

    if properties.get("device.vendor.name") == USB_AUDIO_VENDOR_NAME and \
       properties.get("device.product.name") == USB_AUDIO_PRODUCT_NAME:
        return True

    return endpoint_name.startswith(prefix)


def _pick_endpoint(items: list[dict], prefix: str, *, allow_monitor: bool) -> Optional[PulseAudioEndpoint]:
    for item in items:
        properties = item.get("properties") or {}
        name = item.get("name") or ""
        if not allow_monitor and properties.get("device.class") == "monitor":
            continue
        if not allow_monitor and name.endswith(".monitor"):
            continue
        if not _matches_target(properties, name, prefix):
            continue
        return PulseAudioEndpoint(
            name=name,
            description=item.get("description") or name,
            card=_card_from_properties(properties),
            properties=properties,
        )
    return None


def _list_loaded_modules() -> set[str]:
    loaded_modules: set[str] = set()
    output = _run_pactl_text("list", "short", "modules")
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        parts = raw_line.split("\t")
        if len(parts) >= 2:
            loaded_modules.add(parts[1].strip())
            continue

        columns = line.split(None, 2)
        if len(columns) >= 2:
            loaded_modules.add(columns[1].strip())
    return loaded_modules


def disable_pulse_idle_suspend() -> tuple[str, ...]:
    try:
        loaded_modules = _list_loaded_modules()
    except PulseAudioProbeError:
        return ()

    unloaded_modules: list[str] = []
    for module_name in DEFAULT_DISABLED_MODULES:
        if module_name not in loaded_modules:
            continue
        try:
            _run_pactl_text("unload-module", module_name)
        except PulseAudioProbeError:
            continue
        unloaded_modules.append(module_name)

    if unloaded_modules:
        time.sleep(MODULE_APPLY_SETTLE_SEC)
    return tuple(unloaded_modules)


def get_forced_usb_audio_target(require_source: bool = False) -> PulseAudioTarget:
    sinks = _run_pactl_json("list", "sinks")
    sink = _pick_endpoint(sinks, USB_AUDIO_SINK_PREFIX, allow_monitor=False)
    if sink is None:
        raise PulseAudioTargetNotFoundError(
            f"target USB headset not found in PulseAudio sinks (need {USB_AUDIO_VENDOR_ID}:{USB_AUDIO_PRODUCT_ID})"
        )

    sources = _run_pactl_json("list", "sources")
    source = _pick_endpoint(sources, USB_AUDIO_SOURCE_PREFIX, allow_monitor=False)
    if require_source and source is None:
        raise PulseAudioTargetNotFoundError(
            f"target USB headset microphone not found in PulseAudio sources (need {USB_AUDIO_VENDOR_ID}:{USB_AUDIO_PRODUCT_ID})"
        )

    info = _read_pactl_info()
    server = info.get("Server String")
    return PulseAudioTarget(sink=sink, source=source, server=server)


def probe_forced_usb_audio_target(require_source: bool = False) -> Optional[PulseAudioTarget]:
    try:
        return get_forced_usb_audio_target(require_source=require_source)
    except (PulseAudioTargetNotFoundError, PulseAudioProbeError):
        return None


def wait_for_forced_usb_audio_target(
    timeout_sec: float,
    *,
    poll_interval_sec: float = 0.5,
    require_source: bool = False,
) -> PulseAudioTarget:
    deadline = time.monotonic() + max(0.0, timeout_sec)
    last_error: RuntimeError | None = None

    while True:
        try:
            return get_forced_usb_audio_target(require_source=require_source)
        except (PulseAudioTargetNotFoundError, PulseAudioProbeError) as exc:
            last_error = exc
            if time.monotonic() >= deadline:
                raise exc
            time.sleep(max(0.05, poll_interval_sec))

    if last_error is not None:
        raise last_error


def configure_pulse_audio_env(require_source: bool = False, disable_suspend_on_idle: bool = True) -> PulseAudioTarget:
    unloaded_modules = ()
    if disable_suspend_on_idle:
        unloaded_modules = disable_pulse_idle_suspend()

    target = get_forced_usb_audio_target(require_source=require_source)
    target.unloaded_modules = unloaded_modules

    env = _bootstrap_pulse_env()
    if env.get("XDG_RUNTIME_DIR"):
        os.environ["XDG_RUNTIME_DIR"] = env["XDG_RUNTIME_DIR"]
    if env.get("PULSE_SERVER"):
        os.environ["PULSE_SERVER"] = env["PULSE_SERVER"]
    os.environ["SDL_AUDIODRIVER"] = "pulse"
    os.environ["PULSE_SINK"] = target.sink.name
    if target.source is not None:
        os.environ["PULSE_SOURCE"] = target.source.name
    return target


def pulse_audio_env() -> dict[str, str]:
    return _bootstrap_pulse_env()
