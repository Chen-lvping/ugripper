#!/bin/bash
set -euo pipefail

camera_name="${1:-}"

if [ "$camera_name" != "right_cam_main" ]; then
    echo "[main_camera_packet] usage: $0 right_cam_main" >&2
    exit 2
fi

device_path="/dev/$camera_name"
state_dir="/run/ugripper/main_camera_packet_size"
mkdir -p "$state_dir"

lock_file="$state_dir/${camera_name}.lock"
stamp_file="$state_dir/${camera_name}.stamp"

exec 9>"$lock_file"
if ! flock -n 9; then
    exit 0
fi

wait_for_device() {
    for _ in $(seq 1 80); do
        if [ -e "$device_path" ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

current_stamp() {
    local devnum=""
    local busnum=""
    if [ -e /sys/bus/usb/devices/1-1.4.4/devnum ]; then
        devnum="$(cat /sys/bus/usb/devices/1-1.4.4/devnum 2>/dev/null || true)"
        busnum="$(cat /sys/bus/usb/devices/1-1.4.4/busnum 2>/dev/null || true)"
    elif [ -e /sys/bus/usb/devices/5-1.4.4/devnum ]; then
        devnum="$(cat /sys/bus/usb/devices/5-1.4.4/devnum 2>/dev/null || true)"
        busnum="$(cat /sys/bus/usb/devices/5-1.4.4/busnum 2>/dev/null || true)"
    fi
    printf '%s:%s\n' "$busnum" "$devnum"
}

if ! wait_for_device; then
    echo "[main_camera_packet] camera node did not appear: $device_path" >&2
    exit 1
fi

initial_stamp="$(current_stamp)"
if [ -f "$stamp_file" ] && [ "$(cat "$stamp_file" 2>/dev/null || true)" = "$initial_stamp" ]; then
    exit 0
fi

/opt/ugripper/bin/CameraRecorder/main_camera_xu_tool \
    --device "$device_path" \
    --set-bandwidth low

if ! wait_for_device; then
    echo "[main_camera_packet] camera node disappeared after low-bandwidth set: $device_path" >&2
    exit 1
fi

final_stamp="$(current_stamp)"
printf '%s\n' "$final_stamp" > "$stamp_file"
echo "[main_camera_packet] low_bandwidth_set: camera=$camera_name"
exit 0
