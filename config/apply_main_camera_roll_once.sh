#!/bin/bash
set -euo pipefail

camera_name="${1:-}"
usb_kernel="${2:-}"

if [ -z "$camera_name" ] || [ -z "$usb_kernel" ]; then
    echo "[main_camera_roll] usage: $0 <camera_name> <usb_kernel>" >&2
    exit 2
fi

case "$camera_name" in
    left_cam_main|right_cam_main)
        ;;
    *)
        echo "[main_camera_roll] unsupported camera: $camera_name" >&2
        exit 2
        ;;
esac

sysfs_dir="/sys/bus/usb/devices/$usb_kernel"
if [ ! -d "$sysfs_dir" ]; then
    echo "[main_camera_roll] usb device path missing: $sysfs_dir" >&2
    exit 1
fi

devnum="$(cat "$sysfs_dir/devnum" 2>/dev/null || true)"
if [ -z "$devnum" ]; then
    echo "[main_camera_roll] failed to read devnum from: $sysfs_dir" >&2
    exit 1
fi

state_dir="/run/ugripper/uvc_roll"
mkdir -p "$state_dir"
stamp_file="$state_dir/${camera_name}_${usb_kernel}.devnum"
lock_file="$state_dir/${camera_name}_${usb_kernel}.lock"

exec 9>"$lock_file"
if ! flock -n 9; then
    exit 0
fi

if [ -f "$stamp_file" ] && [ "$(cat "$stamp_file" 2>/dev/null || true)" = "$devnum" ]; then
    exit 0
fi

device_path="/dev/$camera_name"
for _ in $(seq 1 50); do
    if [ -e "$device_path" ]; then
        break
    fi
    sleep 0.2
done

if [ ! -e "$device_path" ]; then
    echo "[main_camera_roll] camera node did not appear: $device_path" >&2
    exit 1
fi

if /opt/ugripper/bin/CameraRecorder/CameraRecorder \
    --apply-uvc-roll-only \
    --config-yaml /opt/ugripper/bin/CameraRecorder/config/camera_recorder.yaml \
    --only "$camera_name"; then
    printf '%s\n' "$devnum" > "$stamp_file"
    echo "[main_camera_roll] roll processed: camera=$camera_name usb=$usb_kernel devnum=$devnum"
    exit 0
fi

echo "[main_camera_roll] roll apply failed: camera=$camera_name usb=$usb_kernel devnum=$devnum" >&2
exit 1
