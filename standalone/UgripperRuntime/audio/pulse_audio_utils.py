#!/usr/bin/env python3
import json
import os
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

SUPPORTED_USB_AUDIO_DEVICES = (
    {
        "vendor_id": "0020",
        "product_id": "0b21",
        "vendor_name": "liyuany",
        "product_name": "USB Audio",
        "sink_prefix": "alsa_output.usb-liyuany_USB_Audio",
        "source_prefix": "alsa_input.usb-liyuany_USB_Audio",
    },
    {
        "vendor_id": "0023",
        "product_id": "0b23",
        "vendor_name": "liyuany",
        "product_name": "USB PnP Sound Device",
        "sink_prefix": "alsa_output.usb-liyuany_USB_PnP_Sound_Device",
        "source_prefix": "alsa_input.usb-liyuany_USB_PnP_Sound_Device",
    },
)
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
    is_usb: bool = False
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


def _matches_supported_usb_audio_device(properties: dict, endpoint_name: str = "") -> bool:
    endpoint_name = endpoint_name or ""
    vendor_id = (
        properties.get("device.vendor.id")
        or properties.get("ID_VENDOR_ID")
        or ""
    ).lower()
    product_id = (
        properties.get("device.product.id")
        or properties.get("ID_MODEL_ID")
        or ""
    ).lower()
    vendor_name = (
        properties.get("device.vendor.name")
        or properties.get("device.vendor.name".upper())
        or properties.get("ID_VENDOR")
        or properties.get("ID_VENDOR_FROM_DATABASE")
        or ""
    ).lower()
    product_name = (
        properties.get("device.product.name")
        or properties.get("device.product.name".upper())
        or properties.get("ID_MODEL")
        or properties.get("ID_MODEL_FROM_DATABASE")
        or ""
    ).lower()

    for device in SUPPORTED_USB_AUDIO_DEVICES:
        if vendor_id == device["vendor_id"] and product_id == device["product_id"]:
            return True

        if device["vendor_name"].lower() in vendor_name and device["product_name"].lower() in product_name:
            return True

        if endpoint_name.startswith(device["sink_prefix"]) or endpoint_name.startswith(device["source_prefix"]):
            return True

    return False


def is_supported_usb_audio_input_device(properties: dict) -> bool:
    return _matches_supported_usb_audio_device(properties)


def _pick_supported_usb_endpoint(
    items: list[dict],
    *,
    allow_monitor: bool,
    kind: str,
    preferred_card: Optional[int] = None,
) -> Optional[PulseAudioEndpoint]:
    for device in SUPPORTED_USB_AUDIO_DEVICES:
        prefix = device[f"{kind}_prefix"]
        for item in items:
            properties = item.get("properties") or {}
            name = item.get("name") or ""
            if not allow_monitor and properties.get("device.class") == "monitor":
                continue
            if not allow_monitor and name.endswith(".monitor"):
                continue
            if preferred_card is not None and _card_from_properties(properties) != preferred_card:
                continue
            if not _matches_supported_usb_audio_device(properties, name):
                continue
            if not name.startswith(prefix) and not (
                (properties.get("device.vendor.id") or "").lower() == device["vendor_id"]
                and (properties.get("device.product.id") or "").lower() == device["product_id"]
            ):
                continue
            return PulseAudioEndpoint(
                name=name,
                description=item.get("description") or name,
                card=_card_from_properties(properties),
                properties=properties,
            )
    return None


def _pick_named_endpoint(
    items: list[dict],
    endpoint_name: Optional[str],
    *,
    allow_monitor: bool,
) -> Optional[PulseAudioEndpoint]:
    if not endpoint_name:
        return None
    for item in items:
        properties = item.get("properties") or {}
        name = item.get("name") or ""
        if not allow_monitor and properties.get("device.class") == "monitor":
            continue
        if not allow_monitor and name.endswith(".monitor"):
            continue
        if name != endpoint_name:
            continue
        return PulseAudioEndpoint(
            name=name,
            description=item.get("description") or name,
            card=_card_from_properties(properties),
            properties=properties,
        )
    return None


def _pick_default_endpoint(
    items: list[dict],
    *,
    allow_monitor: bool,
    info_key: str,
) -> Optional[PulseAudioEndpoint]:
    info = _read_pactl_info()
    named = _pick_named_endpoint(items, info.get(info_key), allow_monitor=allow_monitor)
    if named is not None:
        return named

    for item in items:
        properties = item.get("properties") or {}
        name = item.get("name") or ""
        if not allow_monitor and properties.get("device.class") == "monitor":
            continue
        if not allow_monitor and name.endswith(".monitor"):
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
    sink = _pick_supported_usb_endpoint(sinks, allow_monitor=False, kind="sink")
    is_usb = sink is not None
    if sink is None:
        sink = _pick_default_endpoint(sinks, allow_monitor=False, info_key="Default Sink")
    if sink is None:
        raise PulseAudioTargetNotFoundError(
            "no usable PulseAudio sink found (supported USB headset or default sink)"
        )

    sources = _run_pactl_json("list", "sources")
    source = None
    if is_usb:
        source = _pick_supported_usb_endpoint(
            sources,
            allow_monitor=False,
            kind="source",
            preferred_card=sink.card,
        )
        if source is None:
            source = _pick_supported_usb_endpoint(sources, allow_monitor=False, kind="source")
    if source is None:
        source = _pick_default_endpoint(sources, allow_monitor=False, info_key="Default Source")
    if require_source and source is None:
        raise PulseAudioTargetNotFoundError(
            "no usable PulseAudio source found (supported USB headset mic or default source)"
        )

    info = _read_pactl_info()
    server = info.get("Server String")
    return PulseAudioTarget(sink=sink, source=source, server=server, is_usb=is_usb)


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
