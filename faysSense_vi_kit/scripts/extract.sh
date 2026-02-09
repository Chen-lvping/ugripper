#!/bin/bash
devices=$(v4l2-ctl --list-devices)
FTDI_devices=$(echo "$devices" | awk '/FTDI Superspeed Video Bridge/{flag=1; next} /^[^[:space:]]/{flag=0} flag' | grep '/dev/video')
video_ports=($(echo "$FTDI_devices" | grep -oP '/dev/video\K[0-9]+'))
port_cnt=${#video_ports[@]}

if [ $port_cnt == 4 ]; then
    echo "stereo_dev_port: /dev/video${video_ports[0]}  imu_dev_port: /dev/video${video_ports[2]}"
elif [ $port_cnt == 6 ]; then
    echo "rgb_dev_port: /dev/video${video_ports[0]}  stereo_dev_port: /dev/video${video_ports[2]}  imu_dev_port: /dev/video${video_ports[4]}"
else
    echo "Error: Not enough video ports found for FTDI device."
fi
