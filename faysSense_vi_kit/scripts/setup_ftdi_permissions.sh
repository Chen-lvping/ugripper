#!/bin/bash

echo "正在设置FTDI设备权限..."

if [ "$EUID" -eq 0 ]; then
    echo "错误：请不要使用sudo运行此脚本"
    exit 1
fi

RULES_FILE="/etc/udev/rules.d/99-ftdi-video.rules"

# 直接检测设备ID
detected_devices=$(lsusb | grep "FTDI Superspeed Video Bridge" | grep -oE "[0-9a-fA-F]{4}:[0-9a-fA-F]{4}" | sort | uniq)

if [ -n "$detected_devices" ]; then
    echo "发现以下FTDI设备："
    echo "$detected_devices"
    
    # 生成规则
    {
        echo "# FTDI设备权限规则"
        echo "# 生成时间: $(date)"
        echo ""
        while IFS= read -r device; do
            vendor_id="${device%:*}"
            product_id="${device#*:}"
            echo "SUBSYSTEM==\"video\", ATTR{idVendor}==\"$vendor_id\", ATTR{idProduct}==\"$product_id\", MODE=\"0777\", GROUP=\"video\""
            echo "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"$vendor_id\", ATTR{idProduct}==\"$product_id\", MODE=\"0777\", GROUP=\"plugdev\""
            echo ""
        done <<< "$detected_devices"
    } | sudo tee "$RULES_FILE" > /dev/null
    
    echo "规则设置成功"
else
    echo "未检测到FTDI设备"
    exit 1
fi

sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -a -G video,plugdev $USER

echo "设置完成，请重新插拔设备"