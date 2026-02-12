#!/bin/bash

DEVICE_ID=$(lsusb | grep "Future Technology Devices International, Ltd FTDI Superspeed Video Bridge" | awk '{print $6}')

# check if device ID is empty
if [ -z "$DEVICE_ID" ]; then
    echo "与相机未连接(Camera not connected)"
    exit 1
fi


BUS=$(lsusb | grep "$DEVICE_ID" | awk '{print $2}' | sed 's/^0*//')
DEVICE_NUM=$(lsusb | grep "$DEVICE_ID" | awk '{print $4}' | tr -d ':' | sed 's/^0*//')


USB_SPEED=$(lsusb -t | grep -B 1 -A 3 "Dev $DEVICE_NUM," | grep -oE "5000M|20000M|5G|10G" | head -n 1)

if [[ "$USB_SPEED" =~ (5000M|5G|20000M|10G) ]]; then
    echo "相机已连接在USB3.0接口(The camera is connected to the USB 3.0 port)"
else
    echo "相机连接错误, 请按照以下两点排查(Camera connection error, please check the following two points): "
    echo "   [1]: 检查相机是否插在USB3.0接口(Check if the camera is plugged into the USB 3.0 port)"
    echo "   [2]: 相机端typeC反过来插(Insert the camera type C in reverse)"
fi
