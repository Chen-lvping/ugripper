#!/bin/bash

# Get script directory and build directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")"

CONFIG_FILE=""
EXECUTABLE="$BUILD_DIR/fays_record_example"
CMD_FIFO="/tmp/umi_fays_cmd"
ARCH="$(uname -m)"
LOCAL_FAYS_LIB_DIR="$BUILD_DIR/lib/fays_atrak/${ARCH}/Release"
LOCAL_FTDI_LIB_DIR="$BUILD_DIR/lib/ft602-linux-${ARCH}"
VIDEO_NAME="fays_stereo_output.mkv"
MCAP_NAME="fays_data.mcap"
CALIB_JSON_PATH=""
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
    local stereo_symlink="$1"
    local imu_symlink="$2"
    local missing=0

    if [ ! -e "$stereo_symlink" ]; then
        echo "Error: Missing Fays stereo symlink: $stereo_symlink"
        missing=1
    fi

    if [ ! -e "$imu_symlink" ]; then
        echo "Error: Missing Fays IMU symlink: $imu_symlink"
        missing=1
    fi

    if [ "$missing" -ne 0 ]; then
        echo "Hint: check udev rule camera_record/99-fixed-usb-map.rules and USB connection."
        return 1
    fi

    echo "Using fixed Fays symlink ports:"
    echo "  stereo_dev_port: $stereo_symlink -> $(readlink -f "$stereo_symlink")"
    echo "  imu_dev_port: $imu_symlink -> $(readlink -f "$imu_symlink")"

    return 0
}

verify_config_uses_fixed_symlinks() {
    local stereo_symlink="$1"
    local imu_symlink="$2"
    local stereo_cfg
    local imu_cfg

    if [ ! -f "$CONFIG_FILE" ]; then
        echo "Error: Config file not found: $CONFIG_FILE"
        return 1
    fi

    stereo_cfg=$(sed -n 's/^stereo_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)
    imu_cfg=$(sed -n 's/^imu_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)

    if [ "$stereo_cfg" != "$stereo_symlink" ]; then
        echo "Error: Config mismatch: stereo_dev_port=$stereo_cfg (expected $stereo_symlink)"
        return 1
    fi

    if [ "$imu_cfg" != "$imu_symlink" ]; then
        echo "Error: Config mismatch: imu_dev_port=$imu_cfg (expected $imu_symlink)"
        return 1
    fi

    return 0
}

require_config_file() {
    if [ -z "$CONFIG_FILE" ]; then
        echo "Error: --config FILE is required"
        return 1
    fi
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "Error: Config file not found: $CONFIG_FILE"
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
append_ld_library_path "$LOCAL_FTDI_LIB_DIR"
append_ld_library_path "/usr/local/lib"

if [ ! -f "$EXECUTABLE" ]; then
    echo "Error: Executable '$EXECUTABLE' not found. Did you run cmake & make?"
    exit 1
fi

POSITIONAL=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --control-fifo)
            CMD_FIFO="$2"
            shift 2
            ;;
        --video-name)
            VIDEO_NAME="$2"
            shift 2
            ;;
        --mcap-name)
            MCAP_NAME="$2"
            shift 2
            ;;
        --calib-json)
            CALIB_JSON_PATH="$2"
            shift 2
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done
set -- "${POSITIONAL[@]}"

MODE="${1:-}"

case "$MODE" in
    daemon)
        require_config_file || exit 1
        STEREO_SYMLINK=$(sed -n 's/^stereo_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)
        IMU_SYMLINK=$(sed -n 's/^imu_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)
        ensure_fixed_fays_symlinks "$STEREO_SYMLINK" "$IMU_SYMLINK" || exit 1
        verify_config_uses_fixed_symlinks "$STEREO_SYMLINK" "$IMU_SYMLINK" || exit 1
        echo "Starting Fays daemon mode..."
        echo "  Config file: $CONFIG_FILE"
        echo "  Control FIFO: $CMD_FIFO"
        args=("$CONFIG_FILE" --control-fifo "$CMD_FIFO" --video-name "$VIDEO_NAME" --mcap-name "$MCAP_NAME")
        if [ -n "$CALIB_JSON_PATH" ]; then
            args+=(--calib-json "$CALIB_JSON_PATH")
        fi
        exec "$EXECUTABLE" "${args[@]}"
        ;;
    dump-calib-json)
        require_config_file || exit 1
        if [ -z "$CALIB_JSON_PATH" ]; then
            echo "Error: dump-calib-json mode requires --calib-json PATH"
            exit 1
        fi
        STEREO_SYMLINK=$(sed -n 's/^stereo_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)
        IMU_SYMLINK=$(sed -n 's/^imu_dev_port:[[:space:]]*//p' "$CONFIG_FILE" | head -n1 | tr -d '"' | xargs)
        ensure_fixed_fays_symlinks "$STEREO_SYMLINK" "$IMU_SYMLINK" || exit 1
        verify_config_uses_fixed_symlinks "$STEREO_SYMLINK" "$IMU_SYMLINK" || exit 1
        exec "$EXECUTABLE" "$CONFIG_FILE" --dump-calib-json "$CALIB_JSON_PATH"
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
        echo "Usage:"
        echo "  $0 [--config FILE] [--control-fifo FIFO] [--video-name NAME] [--mcap-name NAME] [--calib-json PATH] daemon"
        echo "  $0 --config FILE --calib-json PATH dump-calib-json"
        echo "  $0 [--control-fifo FIFO] start <output_dir>"
        echo "  $0 [--control-fifo FIFO] stop"
        echo "  $0 [--control-fifo FIFO] exit"
        exit 1
        ;;
esac
