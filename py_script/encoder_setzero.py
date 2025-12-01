#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口编码器归零程序
对所有 /dev/ttyCH9344USB* 设备进行归零操作
"""

import serial
import time
import os
from typing import List

# 编码器寄存器定义
ENCODER_DEFAULT_ADDR = 0x01
ENCODER_FUNC_WR = 0x06  # 写单个寄存器
ENCODER_REGISTER_ZERO_SET = 0x52

def calculate_crc16(data: bytes) -> tuple:
    """
    计算Modbus CRC16校验
    返回: (crc_low, crc_high)
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    
    crc_low = crc & 0xFF
    crc_high = (crc >> 8) & 0xFF
    return (crc_low, crc_high)


def send_zero_command(ser: serial.Serial, device_name: str) -> bool:
    """
    向编码器发送归零命令
    
    Args:
        ser: 串口对象
        device_name: 设备名称（用于日志）
    
    Returns:
        bool: 成功返回True，失败返回False
    """
    try:
        # 构建归零命令（不含CRC）
        send_buf = bytearray([
            ENCODER_DEFAULT_ADDR,      # 0x01 - 设备地址
            ENCODER_FUNC_WR,           # 0x06 - 功能码（写单个寄存器）
            0x00,                      # 寄存器地址高位
            ENCODER_REGISTER_ZERO_SET, # 0x52 - 寄存器地址低位（归零寄存器）
            0x00,                      # 数据高位
            0x01                       # 数据低位（1表示设置当前位置为零点）
        ])
        
        # 计算CRC
        crc_low, crc_high = calculate_crc16(send_buf)
        send_buf.append(crc_low)
        send_buf.append(crc_high)
        
        # 发送前等待5ms
        time.sleep(0.005)
        
        # 发送命令
        bytes_written = ser.write(send_buf)
        ser.flush()
        
        if bytes_written != len(send_buf):
            print(f"  ⚠️  [{device_name}] 写入字节数不匹配: {bytes_written}/{len(send_buf)}")
            return False
        
        # 发送后等待5ms
        time.sleep(0.005)
        
        # 读取响应（可选）
        if ser.in_waiting > 0:
            response = ser.read(ser.in_waiting)
            print(f"  ✅ [{device_name}] 归零成功，响应: {response.hex()}")
        else:
            print(f"  ✅ [{device_name}] 归零命令已发送")
        
        return True
        
    except Exception as e:
        print(f"  ❌ [{device_name}] 归零失败: {e}")
        return False


def find_devices() -> List[str]:
    """查找所有ttyCH9344USB设备（0-30）"""
    devices = []
    for i in range(31):
        device_path = f'/dev/ttyCH9344USB{i}'
        if os.path.exists(device_path):
            devices.append(device_path)
    return devices


def zero_all_encoders(baudrate: int = 115200):
    """
    对所有编码器进行归零操作
    
    Args:
        baudrate: 波特率，默认115200
    """
    print("=" * 60)
    print("串口编码器归零程序")
    print("=" * 60)
    
    # 查找设备
    devices = find_devices()
    
    if not devices:
        print("❌ 未找到任何设备")
        print("   请检查设备连接")
        return
    
    print(f"\n找到 {len(devices)} 个设备:")
    for device in devices:
        print(f"  - {device}")
    
    print("\n" + "=" * 60)
    print("开始归零操作...")
    print("=" * 60 + "\n")
    
    success_count = 0
    fail_count = 0
    
    # 对每个设备进行归零
    for i, device_path in enumerate(devices):
        device_name = f"USB{device_path.split('USB')[-1]}"
        print(f"[{i+1}/{len(devices)}] 正在归零 {device_path}...")
        
        try:
            # 打开串口
            ser = serial.Serial(
                port=device_path,
                baudrate=baudrate,
                timeout=1.0,
                write_timeout=1.0
            )
            
            # 发送归零命令
            if send_zero_command(ser, device_name):
                success_count += 1
            else:
                fail_count += 1
            
            # 关闭串口
            ser.close()
            
            # 设备间延迟
            time.sleep(0.01)
            
        except Exception as e:
            print(f"  ❌ [{device_name}] 无法打开串口: {e}")
            fail_count += 1
    
    # 统计结果
    print("\n" + "=" * 60)
    print("归零操作完成")
    print("=" * 60)
    print(f"  总设备数: {len(devices)}")
    print(f"  ✅ 成功: {success_count}")
    print(f"  ❌ 失败: {fail_count}")
    print("=" * 60)


def zero_single_encoder(device_path: str, baudrate: int = 115200) -> bool:
    """
    对单个编码器进行归零
    
    Args:
        device_path: 设备路径，如 '/dev/ttyCH9344USB0'
        baudrate: 波特率
    
    Returns:
        bool: 成功返回True
    """
    print(f"正在归零 {device_path}...")
    
    try:
        ser = serial.Serial(
            port=device_path,
            baudrate=baudrate,
            timeout=1.0,
            write_timeout=1.0
        )
        
        device_name = f"USB{device_path.split('USB')[-1]}"
        result = send_zero_command(ser, device_name)
        
        ser.close()
        return result
        
    except Exception as e:
        print(f"  ❌ 归零失败: {e}")
        return False


def main():
    import sys
    
    # 检查命令行参数
    if len(sys.argv) > 1:
        if sys.argv[1] == '--help' or sys.argv[1] == '-h':
            print("用法:")
            print("  python3 zero_encoders.py              # 归零所有设备")
            print("  python3 zero_encoders.py <device>     # 归零单个设备")
            print("\n示例:")
            print("  python3 zero_encoders.py")
            print("  python3 zero_encoders.py /dev/ttyCH9344USB0")
            return
        else:
            # 归零单个设备
            device_path = sys.argv[1]
            if not device_path.startswith('/dev/'):
                device_path = f'/dev/ttyCH9344USB{device_path}'
            
            zero_single_encoder(device_path)
    else:
        # 归零所有设备
        zero_all_encoders()


if __name__ == '__main__':
    main()