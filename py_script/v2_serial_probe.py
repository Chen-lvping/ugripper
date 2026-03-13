#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2 串口探测脚本。

用途：
1. 枚举 v2 上可能承载 IM648 / encoder 的 USB 串口。
2. 用现有协议分别探测 IM648 和 encoder。
3. 可选将 encoder 从 115200 配置为 1Mbps。

说明：
- 默认只做低副作用探测，不修改设备配置。
- `--set-encoder-1m` 会向识别出的 encoder 发送“切到 1Mbps + 重启”命令。
- 按现场反馈，encoder 改波特率后需要断电重上电才稳定生效；脚本默认只下发配置，不继续强制切主流程。
"""

from __future__ import annotations

import argparse
import binascii
import glob
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple

try:
    import serial
    from serial import SerialException
except ImportError:  # pragma: no cover
    serial = None

    class SerialException(Exception):
        pass


TTY_GLOBS: Sequence[str] = (
    "/dev/ttyUSB*",
    "/dev/ttyACM*",
    "/dev/ttyCH9344USB*",
)

KNOWN_USB_SERIAL_IDS = {
    "1a86:55d9": "QinHeng USB2.0 To Multi Serial Ports",
    "1a86:7523": "QinHeng CH340 Serial",
    "1a86:5523": "QinHeng CH341 Serial",
}

IM648_CMD_BEGIN = 0x49
IM648_CMD_END = 0x4D
IM648_WAKE_PREFIX = bytes([0x00] * 47 + [0xFF, 0x00, 0xFF])

ENCODER_DEFAULT_ADDR = 0x01
ENCODER_FUNC_RD = 0x03
ENCODER_FUNC_WR = 0x06
ENCODER_REGISTER_ANGLE_LOW = 0x41
ENCODER_REGISTER_SET_BAUDRATE = 0x00
ENCODER_REGISTER_RESTART = 0x54
ENCODER_BAUD_1M_TYPE = 0x07
ENCODER_PRECISION = 65536
ENCODER_PRECISION_HALF = 32768


@dataclass
class UsbNode:
    sysfs_name: str
    vidpid: str
    manufacturer: str = ""
    product: str = ""
    driver: str = ""

    def label(self) -> str:
        parts = [self.sysfs_name, self.vidpid]
        if self.product:
            parts.append(self.product)
        elif self.manufacturer:
            parts.append(self.manufacturer)
        return " ".join(parts)


@dataclass
class PortInfo:
    path: str
    tty_name: str
    usb_chain: List[UsbNode] = field(default_factory=list)
    by_path: str = ""
    driver: str = ""

    @property
    def usb_leaf(self) -> Optional[UsbNode]:
        return self.usb_chain[-1] if self.usb_chain else None

    @property
    def usb_path(self) -> str:
        leaf = self.usb_leaf
        return leaf.sysfs_name if leaf else ""

    @property
    def vidpid(self) -> str:
        leaf = self.usb_leaf
        return leaf.vidpid if leaf else ""

    @property
    def product(self) -> str:
        leaf = self.usb_leaf
        return leaf.product if leaf else ""

    @property
    def chain_text(self) -> str:
        if not self.usb_chain:
            return "onboard/non-usb"
        return " -> ".join(node.label() for node in self.usb_chain)


@dataclass
class ProbeResult:
    kind: str
    success: bool
    baudrate: int
    summary: str
    details: str = ""
    raw_hex: str = ""


@dataclass
class UsbBridgeInfo:
    usb_path: str
    vidpid: str
    manufacturer: str = ""
    product: str = ""
    driver: str = ""
    tty_names: List[str] = field(default_factory=list)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def find_dev_serial_by_path(dev_path: str) -> str:
    by_path_dir = Path("/dev/serial/by-path")
    if not by_path_dir.exists():
        return ""

    target = os.path.realpath(dev_path)
    for entry in sorted(by_path_dir.iterdir()):
        try:
            if os.path.realpath(entry) == target:
                return str(entry)
        except OSError:
            continue
    return ""


def get_usb_chain_from_tty(tty_name: str) -> List[UsbNode]:
    sysfs_tty = Path("/sys/class/tty") / tty_name / "device"
    if not sysfs_tty.exists():
        return []

    chain: List[UsbNode] = []
    seen = set()
    current = sysfs_tty.resolve()

    while current != current.parent:
        id_vendor = current / "idVendor"
        id_product = current / "idProduct"
        if id_vendor.exists() and id_product.exists() and current not in seen:
            seen.add(current)
            driver = ""
            try:
                driver = (current / "driver").resolve().name
            except OSError:
                driver = ""

            chain.append(
                UsbNode(
                    sysfs_name=current.name,
                    vidpid=f"{read_text(id_vendor)}:{read_text(id_product)}",
                    manufacturer=read_text(current / "manufacturer"),
                    product=read_text(current / "product"),
                    driver=driver,
                )
            )
        current = current.parent

    chain.reverse()
    return chain


def build_port_info(dev_path: str) -> PortInfo:
    tty_name = os.path.basename(dev_path)
    chain = get_usb_chain_from_tty(tty_name)
    driver = ""
    try:
        driver = (Path("/sys/class/tty") / tty_name / "device" / "driver").resolve().name
    except OSError:
        driver = ""

    return PortInfo(
        path=dev_path,
        tty_name=tty_name,
        usb_chain=chain,
        by_path=find_dev_serial_by_path(dev_path),
        driver=driver,
    )


def iter_tty_candidates(include_ttys: bool) -> List[PortInfo]:
    patterns = list(TTY_GLOBS)
    if include_ttys:
        patterns.append("/dev/ttyS*")

    found = sorted({path for pattern in patterns for path in glob.glob(pattern)})
    return [build_port_info(path) for path in found]


def find_usb_bridges() -> List[UsbBridgeInfo]:
    bridges: List[UsbBridgeInfo] = []
    for sysfs_path in sorted(Path("/sys/bus/usb/devices").iterdir()):
        if not (sysfs_path / "idVendor").exists() or not (sysfs_path / "idProduct").exists():
            continue

        vidpid = f"{read_text(sysfs_path / 'idVendor')}:{read_text(sysfs_path / 'idProduct')}"
        manufacturer = read_text(sysfs_path / "manufacturer")
        product = read_text(sysfs_path / "product")
        product_text = f"{manufacturer} {product} {KNOWN_USB_SERIAL_IDS.get(vidpid, '')}".lower()

        if vidpid not in KNOWN_USB_SERIAL_IDS and "serial" not in product_text and "qinheng" not in product_text:
            continue

        driver = ""
        try:
            driver = (sysfs_path / "driver").resolve().name
        except OSError:
            driver = ""

        tty_names: List[str] = []
        for tty in Path("/sys/class/tty").iterdir():
            chain = get_usb_chain_from_tty(tty.name)
            leaf = chain[-1].sysfs_name if chain else ""
            if leaf == sysfs_path.name:
                tty_names.append(tty.name)

        bridges.append(
            UsbBridgeInfo(
                usb_path=sysfs_path.name,
                vidpid=vidpid,
                manufacturer=manufacturer,
                product=product or KNOWN_USB_SERIAL_IDS.get(vidpid, ""),
                driver=driver,
                tty_names=sorted(tty_names),
            )
        )

    return bridges


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    return crc


def encode_encoder_write(register: int, value: int) -> bytes:
    body = bytes(
        [
            ENCODER_DEFAULT_ADDR,
            ENCODER_FUNC_WR,
            0x00,
            register,
            0x00,
            value,
        ]
    )
    crc = crc16_modbus(body)
    return body + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def encode_encoder_read(register: int, count: int) -> bytes:
    body = bytes(
        [
            ENCODER_DEFAULT_ADDR,
            ENCODER_FUNC_RD,
            0x00,
            register,
            0x00,
            count,
        ]
    )
    crc = crc16_modbus(body)
    return body + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def parse_encoder_frames(data: bytes) -> List[bytes]:
    frames: List[bytes] = []
    index = 0
    while index <= len(data) - 7:
        if data[index] != ENCODER_DEFAULT_ADDR:
            index += 1
            continue

        func = data[index + 1]
        if func == ENCODER_FUNC_RD:
            payload_len = data[index + 2]
            if payload_len not in (0x02, 0x04):
                index += 1
                continue
            frame_len = payload_len + 5
        elif func == ENCODER_FUNC_WR:
            frame_len = 8
        else:
            index += 1
            continue

        if index + frame_len > len(data):
            break

        frame = data[index : index + frame_len]
        crc = crc16_modbus(frame[:-2])
        if frame[-2] == (crc & 0xFF) and frame[-1] == ((crc >> 8) & 0xFF):
            frames.append(frame)
            index += frame_len
        else:
            index += 1
    return frames


def decode_encoder_position(frame: bytes) -> Tuple[int, float]:
    raw_pos = (frame[3] << 8) | frame[4]
    if raw_pos > ENCODER_PRECISION_HALF:
        raw_pos -= ENCODER_PRECISION
    rad = raw_pos * (2.0 * 3.141592653589793 / ENCODER_PRECISION)
    return raw_pos, rad


def build_im648_command(payload: bytes, target_addr: int = 0xFF) -> bytes:
    body = bytes([target_addr, len(payload)]) + payload
    checksum = sum(body) & 0xFF
    return IM648_WAKE_PREFIX + bytes([IM648_CMD_BEGIN]) + body + bytes([checksum, IM648_CMD_END])


def parse_im648_packets(data: bytes) -> List[bytes]:
    packets: List[bytes] = []
    index = 0
    while index < len(data):
        if data[index] != IM648_CMD_BEGIN:
            index += 1
            continue

        if index + 5 > len(data):
            break

        address = data[index + 1]
        payload_len = data[index + 2]
        if address == 0xFF or payload_len <= 0 or payload_len > 73:
            index += 1
            continue

        frame_len = payload_len + 5
        if index + frame_len > len(data):
            break

        frame = data[index : index + frame_len]
        checksum = sum(frame[1:-2]) & 0xFF
        if checksum == frame[-2] and frame[-1] == IM648_CMD_END:
            packets.append(frame)
            index += frame_len
        else:
            index += 1
    return packets


def open_serial(port: str, baudrate: int, timeout: float = 0.05) -> serial.Serial:
    if serial is None:
        raise SerialException("pyserial 未安装，无法执行串口协议探测")

    kwargs = dict(port=port, baudrate=baudrate, timeout=timeout, write_timeout=timeout)
    try:
        return serial.Serial(exclusive=True, **kwargs)
    except TypeError:
        return serial.Serial(**kwargs)


def read_until_deadline(ser: serial.Serial, duration_sec: float) -> bytes:
    deadline = time.monotonic() + duration_sec
    chunks = bytearray()
    while time.monotonic() < deadline:
        waiting = max(ser.in_waiting, 1)
        data = ser.read(waiting)
        if data:
            chunks.extend(data)
        else:
            time.sleep(0.01)
    return bytes(chunks)


def probe_encoder(port: str, baudrate: int, timeout_sec: float) -> ProbeResult:
    command = encode_encoder_read(ENCODER_REGISTER_ANGLE_LOW, 1)
    try:
        with open_serial(port, baudrate, timeout=0.05) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            ser.write(command)
            ser.flush()
            raw = read_until_deadline(ser, timeout_sec)
    except (SerialException, OSError) as exc:
        return ProbeResult("encoder", False, baudrate, f"打开失败: {exc}")

    frames = parse_encoder_frames(raw)
    if not frames:
        return ProbeResult("encoder", False, baudrate, "无有效 Modbus 响应", raw_hex=binascii.hexlify(raw).decode())

    for frame in frames:
        if frame[1] == ENCODER_FUNC_RD and frame[2] == 0x02:
            position_raw, position_rad = decode_encoder_position(frame)
            return ProbeResult(
                "encoder",
                True,
                baudrate,
                f"识别为 encoder，raw={position_raw}, rad={position_rad:.6f}",
                details=f"frame={frame.hex(' ')}",
                raw_hex=binascii.hexlify(raw).decode(),
            )

    return ProbeResult(
        "encoder",
        True,
        baudrate,
        f"收到 encoder 配置/其他响应，共 {len(frames)} 帧",
        details=f"first_frame={frames[0].hex(' ')}",
        raw_hex=binascii.hexlify(raw).decode(),
    )


def probe_im648(port: str, baudrate: int, timeout_sec: float) -> ProbeResult:
    commands = [
        ("passive", b""),
        ("cmd_10", build_im648_command(bytes([0x10]))),
        ("cmd_03+cmd_10", build_im648_command(bytes([0x03])) + build_im648_command(bytes([0x10]))),
    ]

    try:
        with open_serial(port, baudrate, timeout=0.05) as ser:
            for action, payload in commands:
                if payload:
                    ser.reset_input_buffer()
                    ser.reset_output_buffer()
                    ser.write(payload)
                    ser.flush()
                    time.sleep(0.05)

                raw = read_until_deadline(ser, timeout_sec)
                packets = parse_im648_packets(raw)
                if packets:
                    frame = packets[0]
                    device_addr = frame[1]
                    payload_len = frame[2]
                    cmd = frame[3] if payload_len > 0 else None
                    summary = (
                        f"识别为 IM648，addr={device_addr}, payload_len={payload_len}, "
                        f"cmd=0x{cmd:02X}, mode={action}"
                    )
                    return ProbeResult(
                        "imu",
                        True,
                        baudrate,
                        summary,
                        details=f"packet={frame.hex(' ')}",
                        raw_hex=binascii.hexlify(raw).decode(),
                    )
    except (SerialException, OSError) as exc:
        return ProbeResult("imu", False, baudrate, f"打开失败: {exc}")

    return ProbeResult("imu", False, baudrate, "无有效 IM648 协议响应")


def set_encoder_to_1m(port: str, current_baudrate: int, timeout_sec: float) -> Tuple[bool, List[str]]:
    logs: List[str] = []
    baud_cmd = encode_encoder_write(ENCODER_REGISTER_SET_BAUDRATE, ENCODER_BAUD_1M_TYPE)
    restart_cmd = encode_encoder_write(ENCODER_REGISTER_RESTART, 0x01)

    try:
        with open_serial(port, current_baudrate, timeout=0.05) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()

            ser.write(baud_cmd)
            ser.flush()
            time.sleep(0.05)
            baud_resp = read_until_deadline(ser, timeout_sec)
            baud_frames = parse_encoder_frames(baud_resp)
            if not baud_frames:
                logs.append("波特率写入命令未收到有效响应")
                return False, logs
            logs.append(f"波特率命令响应: {baud_frames[0].hex(' ')}")

            ser.reset_input_buffer()
            ser.write(restart_cmd)
            ser.flush()
            time.sleep(0.05)
            restart_resp = read_until_deadline(ser, timeout_sec)
            restart_frames = parse_encoder_frames(restart_resp)
            if restart_frames:
                logs.append(f"重启命令响应: {restart_frames[0].hex(' ')}")
            else:
                logs.append("重启命令发送完成，未等待到有效响应（设备可能已立即重启）")
    except (SerialException, OSError) as exc:
        logs.append(f"串口操作失败: {exc}")
        return False, logs

    logs.append("已下发 encoder 切换到 1Mbps 的配置。请断电重上电后，再按 1Mbps 重新验证。")
    return True, logs


def print_usb_bridges(bridges: Sequence[UsbBridgeInfo]) -> None:
    if not bridges:
        print("\n[USB 串口桥设备] 未发现已知 USB 串口桥。")
        return

    print("\n[USB 串口桥设备]")
    for bridge in bridges:
        product = bridge.product or KNOWN_USB_SERIAL_IDS.get(bridge.vidpid, "")
        tty_text = ", ".join(bridge.tty_names) if bridge.tty_names else "<无 tty 节点>"
        print(
            f"- usb_path={bridge.usb_path} vidpid={bridge.vidpid} product={product or '<unknown>'} "
            f"driver={bridge.driver or '<none>'} tty={tty_text}"
        )


def print_port_header(port: PortInfo) -> None:
    leaf = port.usb_leaf
    product = leaf.product if leaf and leaf.product else ""
    print(f"\n=== {port.path} ===")
    print(f"usb_path : {port.usb_path or '<non-usb>'}")
    print(f"vid:pid  : {port.vidpid or '<unknown>'}")
    print(f"product  : {product or '<unknown>'}")
    print(f"driver   : {port.driver or '<unknown>'}")
    print(f"by-path  : {port.by_path or '<none>'}")
    print(f"topology : {port.chain_text}")


def choose_encoder_port(
    explicit_port: str,
    port_results: Sequence[Tuple[PortInfo, ProbeResult, ProbeResult]],
) -> Optional[Tuple[PortInfo, ProbeResult]]:
    if explicit_port:
        for port, encoder_result, _ in port_results:
            if port.path == explicit_port:
                return port, encoder_result
        return None

    detected = [(port, encoder_result) for port, encoder_result, _ in port_results if encoder_result.success]
    if len(detected) == 1:
        return detected[0]
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="探测 v2 上的 IM648 / encoder 串口")
    parser.add_argument("--encoder-baud", type=int, default=115200, help="encoder 探测波特率，默认 115200")
    parser.add_argument("--imu-baud", type=int, default=115200, help="IM648 探测波特率，默认 115200")
    parser.add_argument("--timeout", type=float, default=0.25, help="每次协议探测等待时间（秒），默认 0.25")
    parser.add_argument("--include-ttyS", action="store_true", help="额外探测 /dev/ttyS*（默认不扫板载串口）")
    parser.add_argument(
        "--set-encoder-1m",
        action="store_true",
        help="对识别出的 encoder 下发 1Mbps 配置 + 重启命令（需要断电重上电后生效）",
    )
    parser.add_argument(
        "--encoder-port",
        default="",
        help="配合 --set-encoder-1m 使用，显式指定要配置的 encoder 串口；未指定时仅在自动识别出唯一 encoder 时执行",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    print("=" * 72)
    print("v2 串口探测脚本")
    print("=" * 72)
    print(f"encoder 探测波特率: {args.encoder_baud}")
    print(f"IM648   探测波特率: {args.imu_baud}")
    print(f"单次等待时间      : {args.timeout:.2f}s")
    print(f"扫描 ttyS         : {'yes' if args.include_ttyS else 'no'}")
    print(f"执行 1Mbps 配置   : {'yes' if args.set_encoder_1m else 'no'}")

    bridges = find_usb_bridges()
    print_usb_bridges(bridges)

    if serial is None:
        print("\n[依赖检查]")
        print("- 未安装 pyserial；当前仅输出 USB/tty 拓扑，未执行 IM648 / encoder 协议探测。")
        print("- 安装命令: python3 -m pip install pyserial")

    ports = iter_tty_candidates(args.include_ttyS)
    if not ports:
        print("\n[串口探测] 未发现可用 tty 候选。")
        if any(bridge.vidpid == "1a86:55d9" and not bridge.tty_names for bridge in bridges):
            print("提示: 已看到 `1a86:55d9` USB 串口桥，但没有对应 tty 节点，优先检查内核驱动/枚举。")
        return 1

    port_results: List[Tuple[PortInfo, ProbeResult, ProbeResult]] = []
    for port in ports:
        print_port_header(port)
        if serial is None:
            encoder_result = ProbeResult("encoder", False, args.encoder_baud, "缺少 pyserial，未执行探测")
            imu_result = ProbeResult("imu", False, args.imu_baud, "缺少 pyserial，未执行探测")
        else:
            encoder_result = probe_encoder(port.path, args.encoder_baud, args.timeout)
            imu_result = probe_im648(port.path, args.imu_baud, args.timeout)
        port_results.append((port, encoder_result, imu_result))

        print(f"encoder : {'PASS' if encoder_result.success else 'MISS'} @ {encoder_result.baudrate} | {encoder_result.summary}")
        if encoder_result.details:
            print(f"           {encoder_result.details}")
        print(f"imu     : {'PASS' if imu_result.success else 'MISS'} @ {imu_result.baudrate} | {imu_result.summary}")
        if imu_result.details:
            print(f"           {imu_result.details}")

    detected_encoders = [port.path for port, encoder_result, _ in port_results if encoder_result.success]
    detected_imus = [port.path for port, _, imu_result in port_results if imu_result.success]

    print("\n[汇总]")
    print(f"- encoder 识别结果: {', '.join(detected_encoders) if detected_encoders else '<none>'}")
    print(f"- IM648   识别结果: {', '.join(detected_imus) if detected_imus else '<none>'}")

    if args.set_encoder_1m:
        selected = choose_encoder_port(args.encoder_port, port_results)
        if selected is None:
            print("\n[1Mbps 配置]")
            if args.encoder_port:
                print(f"- 未找到指定端口 {args.encoder_port}，或该端口未识别为 encoder。")
            else:
                print("- 未执行：需要自动识别出唯一 encoder，或通过 --encoder-port 显式指定。")
            return 2

        port, encoder_result = selected
        print("\n[1Mbps 配置]")
        print(f"- 目标端口: {port.path}")
        print(f"- 当前探测: {encoder_result.summary}")
        success, logs = set_encoder_to_1m(port.path, args.encoder_baud, args.timeout)
        for line in logs:
            print(f"- {line}")
        return 0 if success else 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
