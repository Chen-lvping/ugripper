#!/bin/bash

# Get script directory and build directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")"

CONFIG_FILE="$BUILD_DIR/config/fays_vikit.yaml"
EXECUTABLE="$BUILD_DIR/fays_record_example"
CMD_FIFO="/tmp/umi_fays_cmd"
ARCH="$(uname -m)"
LOCAL_FAYS_LIB_DIR="$BUILD_DIR/lib/fays_atrak/${ARCH}/Release"
FAYS_STEREO_SYMLINK="/dev/fays_stereo"
FAYS_IMU_SYMLINK="/dev/fays_imu"
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

ensure_fixed_fays_symlinks() {
    local missing=0

    if [ ! -e "$FAYS_STEREO_SYMLINK" ]; then
        echo "Error: Missing Fays stereo symlink: $FAYS_STEREO_SYMLINK"
        missing=1
    fi

    if [ ! -e "$FAYS_IMU_SYMLINK" ]; then
        echo "Error: Missing Fays IMU symlink: $FAYS_IMU_SYMLINK"
        missing=1
    fi

    if [ "$missing" -ne 0 ]; then
        echo "Hint: check udev rule camera_record/99-fixed-usb-map.rules and USB connection."
        return 1
    fi

    echo "Using fixed Fays symlink ports:"
    echo "  stereo_dev_port: $FAYS_STEREO_SYMLINK -> $(readlink -f "$FAYS_STEREO_SYMLINK")"
    echo "  imu_dev_port: $FAYS_IMU_SYMLINK -> $(readlink -f "$FAYS_IMU_SYMLINK")"

    return 0
}

verify_config_uses_fixed_symlinks() {
    local stereo_cfg
    local imu_cfg

    if [ ! -f "$CONFIG_FILE" ]; then
        echo "Error: Config file not found: $CONFIG_FILE"
        return 1
    fi

    stereo_cfg=$(sed -n 's/^stereo_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)
    imu_cfg=$(sed -n 's/^imu_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)

    if [ "$stereo_cfg" != "$FAYS_STEREO_SYMLINK" ]; then
        echo "Error: Config mismatch: stereo_dev_port=$stereo_cfg (expected $FAYS_STEREO_SYMLINK)"
        return 1
    fi

    if [ "$imu_cfg" != "$FAYS_IMU_SYMLINK" ]; then
        echo "Error: Config mismatch: imu_dev_port=$imu_cfg (expected $FAYS_IMU_SYMLINK)"
        return 1
    fi

    return 0
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
        ensure_fixed_fays_symlinks || exit 1
        verify_config_uses_fixed_symlinks || exit 1
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

        ensure_fixed_fays_symlinks || exit 1
        verify_config_uses_fixed_symlinks || exit 1
        echo "Starting Fays one-shot recording mode..."
        echo "  Config file: $CONFIG_FILE"
        echo "  Output directory: $OUTPUT_DIR"
        exec "$EXECUTABLE" "$CONFIG_FILE" "$OUTPUT_DIR"
        ;;
esac
