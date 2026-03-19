#!/usr/bin/env python3
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

SUPPORTED_USB_AUDIO_DEVICES = (
    {
        "vendor_id": "0020",
        "product_id": "0b21",
        "vendor_name": "liyuany",
        "product_name": "USB Audio",
    },
    {
        "vendor_id": "0023",
        "product_id": "0b23",
        "vendor_name": "liyuany",
        "product_name": "USB PnP Sound Device",
    },
)
USB_AUDIO_CONTROL_SYMLINK = Path("/dev/snd/ugripper_usb_audio_control")
USB_AUDIO_PLAYBACK_SYMLINK = Path("/dev/snd/ugripper_usb_audio_playback")
USB_AUDIO_CAPTURE_SYMLINK = Path("/dev/snd/ugripper_usb_audio_capture")


def _normalize_usb_name(value: str) -> str:
    return value.lower().replace(" ", "_").replace("-", "_")


@dataclass
class UsbAudioDevice:
    card: int
    device: int
    playback_node: str
    capture_node: Optional[str]

    @property
    def alsa_hw(self) -> str:
        return f"hw:{self.card},{self.device}"

    @property
    def alsa_plughw(self) -> str:
        return f"plughw:{self.card},{self.device}"


class UsbAudioNotFoundError(RuntimeError):
    pass


class UsbAudioProbeError(RuntimeError):
    pass


def _parse_card_device_from_symlink(path: Path, expected_suffix: str) -> tuple[int, int, str]:
    if not path.exists():
        raise UsbAudioNotFoundError(f"required ALSA symlink missing: {path}")

    resolved = os.path.realpath(path)
    base = os.path.basename(resolved)
    match = re.fullmatch(rf"pcmC(\d+)D(\d+){expected_suffix}", base)
    if not match:
        raise UsbAudioProbeError(
            f"unexpected ALSA node for {path}: {resolved} (expected pcmC*D*{expected_suffix})"
        )
    return int(match.group(1)), int(match.group(2)), resolved


def _read_capture_node(card: int, device: int) -> Optional[str]:
    if USB_AUDIO_CAPTURE_SYMLINK.exists():
        resolved = os.path.realpath(USB_AUDIO_CAPTURE_SYMLINK)
        base = os.path.basename(resolved)
        if re.fullmatch(rf"pcmC{card}D{device}c", base):
            return resolved
    candidate = f"/dev/snd/pcmC{card}D{device}c"
    if os.path.exists(candidate):
        return candidate
    return None


def get_usb_audio_device() -> UsbAudioDevice:
    card, device, playback_node = _parse_card_device_from_symlink(USB_AUDIO_PLAYBACK_SYMLINK, "p")
    capture_node = _read_capture_node(card, device)
    return UsbAudioDevice(card=card, device=device, playback_node=playback_node, capture_node=capture_node)


def require_usb_audio_device() -> UsbAudioDevice:
    device = get_usb_audio_device()
    usb_info = describe_usb_audio_device()
    if not usb_info:
        raise UsbAudioProbeError("USB audio ALSA symlink exists but udev USB metadata is unavailable")

    vendor_id = (usb_info.get("ID_VENDOR_ID") or "").lower()
    model_id = (usb_info.get("ID_MODEL_ID") or "").lower()
    vendor_name = (usb_info.get("ID_VENDOR") or usb_info.get("ID_VENDOR_FROM_DATABASE") or "").lower()
    model_name = _normalize_usb_name(usb_info.get("ID_MODEL") or usb_info.get("ID_MODEL_FROM_DATABASE") or "")

    for supported in SUPPORTED_USB_AUDIO_DEVICES:
        if vendor_id == supported["vendor_id"] and model_id == supported["product_id"]:
            return device
        if (
            supported["vendor_name"].lower() in vendor_name
            and _normalize_usb_name(supported["product_name"]) in model_name
        ):
            return device

    supported_list = ", ".join(
        f'{item["vendor_id"]}:{item["product_id"]}' for item in SUPPORTED_USB_AUDIO_DEVICES
    )
    raise UsbAudioProbeError(
        f"USB audio metadata mismatch: expected one of {supported_list}, got {vendor_id}:{model_id}"
    )

    return device


def describe_usb_audio_device() -> dict[str, str]:
    if not USB_AUDIO_CONTROL_SYMLINK.exists():
        return {}

    try:
        out = subprocess.check_output(
            ["udevadm", "info", "--query=property", f"--name={USB_AUDIO_CONTROL_SYMLINK}"],
            stderr=subprocess.STDOUT,
            text=True,
        )
    except Exception:
        return {}

    props: dict[str, str] = {}
    for line in out.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        props[key.strip()] = value.strip()
    return props


def ensure_playback_mixer(card: int) -> None:
    for cmd in (
        ["amixer", "-c", str(card), "set", "PCM", "85%", "unmute"],
        ["amixer", "-c", str(card), "sset", "Speaker", "on"],
        ["amixer", "-c", str(card), "sset", "Headphone", "on"],
        ["amixer", "-c", str(card), "sset", "Mic", "cap"],
        ["amixer", "-c", str(card), "sset", "Capture", "80%", "cap"],
    ):
        subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
