#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Python版本的串口编码器管理器
自动检测并连接所有 /dev/ttyCH9344USB* 设备
"""

import serial
import threading
import time
import struct
import glob
from dataclasses import dataclass
from typing import Callable, Optional, List
import queue


@dataclass
class EncoderData:
    """编码器数据结构"""

    position: int = 0
    current_position_rad: float = 0.0
    timestamp: float = 0.0
    valid: bool = False


class SerialEncoder:
    """串口编码器类"""

    def __init__(
        self, encoder_id: int, port: str, baudrate: int = 115200, name: str = ""
    ):
        self.encoder_id = encoder_id
        self.port = port
        self.baudrate = baudrate
        self.name = name or f"SerialEncoder{encoder_id}"

        self.serial_port: Optional[serial.Serial] = None
        self.lock = threading.Lock()
        self.running = False

        # 编码器状态
        self.encoder_data = EncoderData()

        # 线程
        self.read_thread: Optional[threading.Thread] = None
        self.request_thread: Optional[threading.Thread] = None

        # 配置
        self.request_period_ms = 5  # 请求周期（毫秒）
        self.frame_min_len = 7

    def connect(self) -> bool:
        """连接串口"""
        try:
            self.serial_port = serial.Serial(
                port=self.port, baudrate=self.baudrate, timeout=0.1, write_timeout=0.1
            )
            print(f"✅ [{self.name}] 连接成功: {self.port}")
            return True
        except Exception as e:
            print(f"❌ [{self.name}] 连接失败: {self.port} - {e}")
            return False

    def disconnect(self):
        """断开连接"""
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()

    def calculate_crc16(self, data: bytes) -> int:
        """计算Modbus CRC16校验"""
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

    def send_data_request(self):
        """发送数据请求"""
        if not self.serial_port or not self.serial_port.is_open:
            return

        try:
            # 构建Modbus读取命令
            command = bytearray(
                [
                    0x01,  # 设备地址
                    0x03,  # 功能码（读保持寄存器）
                    0x00,  # 起始地址高位
                    0x41,  # 起始地址低位
                    0x00,  # 寄存器数量高位
                    0x01,  # 寄存器数量低位
                ]
            )

            # 计算并添加CRC
            crc = self.calculate_crc16(command)
            command.append(crc & 0xFF)
            command.append((crc >> 8) & 0xFF)
            # 打印command 16进制
            # print("Command:", " ".join(f"{b:02X}" for b in command))

            # 发送
            self.serial_port.write(command)
            self.serial_port.flush()

        except Exception as e:
            print(f"❌ [{self.name}] 发送错误: {e}")

    def recv_parse(self, data: bytes):
        """解析接收到的数据"""
        # print(" ".join(f"{b:02X}" for b in data))
        try:
            # 解析编码器位置（字节3和4）
            position = (data[3] << 8) + data[4]

            # 转换为弧度
            position_deg = float(position) / 65536.0 * 360.0
            position_rad = position_deg * 0.0174532925  # DEGTORAD

            # 更新状态
            with self.lock:
                self.encoder_data.position = position
                self.encoder_data.current_position_rad = position_rad
                self.encoder_data.timestamp = time.time()
                self.encoder_data.valid = True

        except Exception as e:
            print(f"❌ [{self.name}] 解析错误: {e}")

    def read_loop(self):
        """读取线程循环"""
        buffer = bytearray()

        while self.running:
            try:
                if self.serial_port and self.serial_port.is_open:
                    # 读取可用数据
                    if self.serial_port.in_waiting > 0:
                        data = self.serial_port.read(self.serial_port.in_waiting)
                        buffer.extend(data)

                        # 解析完整帧（至少8字节）
                        while len(buffer) >= 8:
                            # 简单的帧解析：查找有效数据
                            # 查找帧头（0x01 0x03）
                            if buffer[0] != 0x01 or buffer[1] != 0x03:
                                buffer.pop(0)
                                continue
                            # 解析数据
                            data_len = buffer[2]
                            if len(buffer) < 5 + data_len:
                                break  # 等待更多数据
                            self.recv_parse(bytes(buffer[: 5 + data_len]))
                            buffer = buffer[5 + data_len :]

                time.sleep(0.001)  # 1ms

            except Exception as e:
                print(f"❌ [{self.name}] 读取错误: {e}")
                time.sleep(0.1)

    def request_loop(self):
        """请求线程循环"""
        while self.running:
            try:
                self.send_data_request()
                time.sleep(self.request_period_ms / 1000.0)
            except Exception as e:
                print(f"❌ [{self.name}] 请求错误: {e}")
                time.sleep(0.1)

    def start(self):
        """启动读取和请求线程"""
        if not self.serial_port or not self.serial_port.is_open:
            print(f"❌ [{self.name}] 串口未连接")
            return False

        self.running = True

        # 启动读取线程
        self.read_thread = threading.Thread(target=self.read_loop, daemon=True)
        self.read_thread.start()

        # 启动请求线程
        self.request_thread = threading.Thread(target=self.request_loop, daemon=True)
        self.request_thread.start()

        return True

    def stop(self):
        """停止线程"""
        self.running = False

        if self.read_thread:
            self.read_thread.join(timeout=1.0)
        if self.request_thread:
            self.request_thread.join(timeout=1.0)

    def get_state(self) -> EncoderData:
        """获取编码器状态"""
        with self.lock:
            return EncoderData(
                position=self.encoder_data.position,
                current_position_rad=self.encoder_data.current_position_rad,
                timestamp=self.encoder_data.timestamp,
                valid=self.encoder_data.valid,
            )


class SerialManager:
    """串口管理器"""

    def __init__(self):
        self.encoders: List[SerialEncoder] = []

    def add_encoder(self, encoder: SerialEncoder):
        """添加编码器"""
        self.encoders.append(encoder)

    def start_all(self):
        """启动所有编码器"""
        for encoder in self.encoders:
            encoder.start()

    def stop_all(self):
        """停止所有编码器"""
        for encoder in self.encoders:
            encoder.stop()
            encoder.disconnect()


def find_ttych9344_devices():
    import os

    devices = []

    for i in range(31):  # 0到30
        device_path = f"/dev/ttyCH9344USB{i}"
        if os.path.exists(device_path):
            devices.append(device_path)

    return devices


def main():
    print("=" * 60)
    print("串口编码器管理器 - Python版本")
    print("=" * 60)

    # 查找所有设备
    # devices = find_ttych9344_devices()
    devices = ["/dev/ttyS7"]

    if not devices:
        print("❌ 未找到任何 /dev/ttyCH9344USB* 设备")
        print("   请检查设备连接和驱动")
        return

    print(f"✅ 找到 {len(devices)} 个设备:")
    for device in devices:
        print(f"   - {device}")
    print()

    # 创建串口管理器
    serial_manager = SerialManager()

    # 创建编码器实例
    encoders = []
    for i, device in enumerate(devices):
        encoder = SerialEncoder(
            encoder_id=i + 1, port=device, baudrate=115200, name=f"Encoder{i+1}"
        )

        # 连接
        if encoder.connect():
            encoders.append(encoder)
            serial_manager.add_encoder(encoder)
        else:
            print(f"⚠️  跳过设备: {device}")

    if not encoders:
        print("❌ 没有成功连接的编码器")
        return

    print(f"\n✅ 成功连接 {len(encoders)} 个编码器")
    print("=" * 60)

    # 启动所有编码器
    serial_manager.start_all()

    print("\n开始读取编码器数据...")
    print("按 Ctrl+C 停止\n")

    # 主循环
    main_count = 0
    main_times = 5000  # 运行5000次循环

    try:
        while main_times > 0:
            main_count += 1
            # main_times -= 1

            # 每100次循环打印一次
            if main_count % 1000 == 1:
                print(f"[{main_count:5d}] 编码器: ", end="")

                for encoder in encoders:
                    state = encoder.get_state()
                    if state.valid:
                        print(f"{state.current_position_rad:7.4f} ", end="")
                    else:
                        print("  N/A   ", end="")

                print()  # 换行

            time.sleep(0.0001)  # 100微秒

    except KeyboardInterrupt:
        print("\n\n⚠️  收到中断信号，正在停止...")

    # 清理
    print("\n正在关闭所有编码器...")
    serial_manager.stop_all()

    print("✅ 程序结束")
    print("=" * 60)


if __name__ == "__main__":
    main()
