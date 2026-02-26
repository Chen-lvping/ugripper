#!/bin/bash

# Get script directory and build directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")"

CONFIG_FILE="$BUILD_DIR/config/fays_vikit.yaml"
EXECUTABLE="$BUILD_DIR/fays_record_example"
CMD_FIFO="/tmp/umi_fays_cmd"
ARCH="$(uname -m)"
LOCAL_FAYS_LIB_DIR="$BUILD_DIR/lib/fays_atrak/${ARCH}/Release"
CMD_SEND_TIMEOUT_SEC="${FAYS_CMD_TIMEOUT_SEC:-0.35}"

append_ld_library_path() {
    local path_to_add="$1"
    if [ -d "$path_to_add" ]; then
        case ":$LD_LIBRARY_PATH:" in
            *":$path_to_add:"*) ;;
            *) export LD_LIBRARY_PATH="$path_to_add${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
        esac
    else
        echo "Warning: Library directory not found: $path_to_add"
    fi
}

configure_fays_ports() {
    local devices
    local FTDI_devices
    local video_ports
    local port_cnt

    devices=$(v4l2-ctl --list-devices)
    FTDI_devices=$(echo "$devices" | awk '/FTDI Superspeed Video Bridge/{flag=1; next} /^[^[:space:]]/{flag=0} flag' | grep '/dev/video')
    video_ports=($(echo "$FTDI_devices" | grep -oP '/dev/video\K[0-9]+'))
    port_cnt=${#video_ports[@]}

    if [ "$port_cnt" = 4 ]; then
        local stereo_port imu_port all_videos max_num rgb_num
        stereo_port="/dev/video${video_ports[0]}"
        imu_port="/dev/video${video_ports[2]}"

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

        echo "Detected 4-port device."
        echo "Updating config file with:"
        echo "  stereo_dev_port: $stereo_port"
        echo "  imu_dev_port: $imu_port"
        echo "  rgb_dev_port: NULL"

        sed -i -E "s|^rgb_dev_port:.*|rgb_dev_port: NULL|" "$CONFIG_FILE"
        sed -i -E "s|^stereo_dev_port:.*|stereo_dev_port: ${stereo_port}|" "$CONFIG_FILE"
        sed -i -E "s|^imu_dev_port:.*|imu_dev_port: ${imu_port}|" "$CONFIG_FILE"
    elif [ "$port_cnt" = 6 ]; then
        local rgb_port stereo_port imu_port
        rgb_port="/dev/video${video_ports[0]}"
        stereo_port="/dev/video${video_ports[2]}"
        imu_port="/dev/video${video_ports[4]}"

        echo "Detected 6-port device."
        echo "Updating config file with:"
        echo "  rgb_port: $rgb_port"
        echo "  stereo_dev_port: $stereo_port"
        echo "  imu_dev_port: $imu_port"

        sed -i -E "s|^rgb_dev_port:.*|rgb_dev_port: ${rgb_port}|" "$CONFIG_FILE"
        sed -i -E "s|^stereo_dev_port:.*|stereo_dev_port: ${stereo_port}|" "$CONFIG_FILE"
        sed -i -E "s|^imu_dev_port:.*|imu_dev_port: ${imu_port}|" "$CONFIG_FILE"
    else
        echo "Warning: Unexpected number of video ports found for FTDI device. Found $port_cnt ports."
    fi
}

ensure_fifo() {
    if [ -e "$CMD_FIFO" ] && [ ! -p "$CMD_FIFO" ]; then
        echo "Error: $CMD_FIFO exists but is not a FIFO"
        return 1
    fi
    if [ ! -p "$CMD_FIFO" ]; then
        mkfifo "$CMD_FIFO"
    fi
    return 0
}

send_control_cmd() {
    local cmd="$1"

    if [ ! -p "$CMD_FIFO" ]; then
        echo "Error: Control FIFO not found: $CMD_FIFO"
        return 1
    fi

    if ! timeout "$CMD_SEND_TIMEOUT_SEC" bash -c 'printf "%s\n" "$1" > "$2"' _ "$cmd" "$CMD_FIFO" 2>/dev/null; then
        echo "Error: Failed to send command '$cmd' via $CMD_FIFO"
        return 1
    fi

    echo "[Control] Sent: $cmd"
    return 0
}

append_ld_library_path "$LOCAL_FAYS_LIB_DIR"
append_ld_library_path "/usr/local/lib"
append_ld_library_path "/usr/local/opencv-4.2.0-linux-aarch64/lib"

if [ ! -f "$EXECUTABLE" ]; then
    echo "Error: Executable '$EXECUTABLE' not found. Did you run cmake & make?"
    exit 1
fi

MODE="${1:-}"

case "$MODE" in
    daemon)
        configure_fays_ports
        ensure_fifo || exit 1
        echo "Starting Fays daemon mode..."
        echo "  Config file: $CONFIG_FILE"
        echo "  Control FIFO: $CMD_FIFO"
        exec "$EXECUTABLE" "$CONFIG_FILE" --control-fifo "$CMD_FIFO"
        ;;
    start)
        OUTPUT_DIR="${2:-}"
        if [ -z "$OUTPUT_DIR" ]; then
            echo "Error: start mode requires output directory"
            exit 1
        fi
        send_control_cmd "START|$OUTPUT_DIR"
        ;;
    stop)
        send_control_cmd "STOP"
        ;;
    exit)
        send_control_cmd "EXIT"
        ;;
    *)
        # Backward-compatible one-shot mode: run_fays_record.sh <output_dir>
        OUTPUT_DIR="${1:-}"
        if [ -z "$OUTPUT_DIR" ]; then
            echo "Usage:"
            echo "  $0 daemon"
            echo "  $0 start <output_dir>"
            echo "  $0 stop"
            echo "  $0 exit"
            echo "  $0 <output_dir>    # one-shot backward-compatible mode"
            exit 1
        fi

        configure_fays_ports
        echo "Starting Fays one-shot recording mode..."
        echo "  Config file: $CONFIG_FILE"
        echo "  Output directory: $OUTPUT_DIR"
        exec "$EXECUTABLE" "$CONFIG_FILE" "$OUTPUT_DIR"
        ;;
esac
