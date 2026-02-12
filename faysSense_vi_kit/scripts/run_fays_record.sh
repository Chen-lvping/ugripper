#!/bin/bash

# Get script directory and build directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")"

# 配置文件路径
CONFIG_FILE="$BUILD_DIR/config/fays_vikit.yaml"

# 设置 OpenCV 库路径 (OpenCV 已安装到 /usr/local)
# 注意：确保架构名称(aarch64)与你的实际目录一致
if [ -d "/usr/local/opencv-4.2.0-linux-aarch64/lib" ]; then
    export LD_LIBRARY_PATH="/usr/local/opencv-4.2.0-linux-aarch64/lib:$LD_LIBRARY_PATH"
else
    echo "Warning: OpenCV library directory not found at /usr/local/opencv-4.2.0-linux-aarch64/lib"
fi

# --- 设备自动发现与配置修改 (复用原逻辑) ---
devices=$(v4l2-ctl --list-devices)
# 筛选 FTDI 设备
FTDI_devices=$(echo "$devices" | awk '/FTDI Superspeed Video Bridge/{flag=1; next} /^[^[:space:]]/{flag=0} flag' | grep '/dev/video')
# 提取端口号数组
video_ports=($(echo "$FTDI_devices" | grep -oP '/dev/video\K[0-9]+'))
port_cnt=${#video_ports[@]}

if [ $port_cnt == 4 ]; then
    # 4端口模式 (通常无独立RGB或RGB未映射)
    stereo_port="/dev/video${video_ports[0]}"
    imu_port="/dev/video${video_ports[2]}"

    # 原脚本逻辑：计算一个未占用的 video 号 (虽然下面置为 NULL，但保留逻辑防止兼容性问题)
    all_videos=($(ls /dev/video* 2>/dev/null | grep -oP '/dev/video\K[0-9]+'))
    if [ ${#all_videos[@]} -gt 0 ]; then
        max_num=$(printf "%s\n" "${all_videos[@]}" | sort -n | tail -n 1)
    else
        max_num=-1
    fi
    rgb_num=$((max_num + 1))
    while [ -e "/dev/video${rgb_num}" ]; do
        rgb_num=$((rgb_num + 1))
    done
    rgb_port="/dev/video${rgb_num}"

    echo "Detected 4-port device."
    echo "Updating config file with:"
    echo "  stereo_dev_port: $stereo_port"
    echo "  imu_dev_port: $imu_port"
    echo "  rgb_dev_port: NULL"

    # 修改 yaml 配置
    sed -i -E "s|^rgb_dev_port:.*|rgb_dev_port: NULL|" "$CONFIG_FILE"
    sed -i -E "s|^stereo_dev_port:.*|stereo_dev_port: ${stereo_port}|" "$CONFIG_FILE"
    sed -i -E "s|^imu_dev_port:.*|imu_dev_port: ${imu_port}|" "$CONFIG_FILE"

elif [ $port_cnt == 6 ]; then
    # 6端口模式 (RGB + Stereo + IMU)
    rgb_port="/dev/video${video_ports[0]}"
    stereo_port="/dev/video${video_ports[2]}"
    imu_port="/dev/video${video_ports[4]}"
    
    echo "Detected 6-port device."
    echo "Updating config file with:"
    echo "  rgb_port: $rgb_port"
    echo "  stereo_dev_port: $stereo_port"
    echo "  imu_dev_port: $imu_port"

    # 修改 yaml 配置
    sed -i -E "s|^rgb_dev_port:.*|rgb_dev_port: ${rgb_port}|" "$CONFIG_FILE"
    sed -i -E "s|^stereo_dev_port:.*|stereo_dev_port: ${stereo_port}|" "$CONFIG_FILE"
    sed -i -E "s|^imu_dev_port:.*|imu_dev_port: ${imu_port}|" "$CONFIG_FILE"
else
    echo "Warning: Unexpected number of video ports found for FTDI device. Found $port_cnt ports."
fi

# --- 检查并安装 FTDI 驱动库 ---
# Libraries are now in PROJECT_ROOT/libs/ instead of SOURCE_DIR/thirdparty/
# if [ ! -f /usr/lib/libft602.so ] && [ -d "$PROJECT_ROOT/libs/ft602-linux-$(uname -m)" ]; then
#     echo "Installing libft602.so to /usr/lib/..."
#     cp "$PROJECT_ROOT/libs/ft602-linux-$(uname -m)/libft602.so" /usr/lib/
#     cp "$PROJECT_ROOT/libs/ft602-linux-$(uname -m)/libft602.so.1.0.17" /usr/lib/
# fi

OUTPUT_DIR="${1:-}"

# --- 启动录制程序 ---
EXECUTABLE="$BUILD_DIR/fays_record_example"
if [ -f "$EXECUTABLE" ]; then
    echo "Starting Fays Recording Example..."
    echo "  Config file: $CONFIG_FILE"
    echo "  Output directory: $OUTPUT_DIR"
    # Ensure OUTPUT_DIR is set and not empty
    if [ -z "$OUTPUT_DIR" ]; then
        echo "Error: OUTPUT_DIR is empty!"
        exit 1
    fi
    # Use exec to replace shell process with the binary, so signals are forwarded correctly
    exec "$EXECUTABLE" "$CONFIG_FILE" "$OUTPUT_DIR"
else
    echo "Error: Executable '$EXECUTABLE' not found. Did you run cmake & make?"
    exit 1
fi
