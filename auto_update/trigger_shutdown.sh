#!/bin/bash
set -e

REQUEST_FILE="/tmp/umi_system_action_request"
RESULT_FILE="/tmp/umi_system_action_result"
MOUNT_HELPER="/opt/ugripper/auto_update/mount_data_disk.sh"

write_result() {
    local result="${1:-error}"
    printf '%s\n' "$result" > "$RESULT_FILE"
    chmod 0666 "$RESULT_FILE" >/dev/null 2>&1 || true
}

if [ ! -f "$REQUEST_FILE" ]; then
    echo "No system action request file found, skip."
    exit 0
fi

ACTION="$(tr -d '[:space:]' < "$REQUEST_FILE" 2>/dev/null || true)"
ACTION="${ACTION,,}"
rm -f "$REQUEST_FILE"
rm -f "$RESULT_FILE"

case "$ACTION" in
    shutdown)
        echo "System action consumed: shutdown"
        write_result ok
        /usr/bin/systemctl poweroff
        ;;
    umount)
        echo "System action consumed: umount /mnt/data_disk"
        if "$MOUNT_HELPER" remove; then
            write_result ok
            exit 0
        fi
        echo "Failed to umount /mnt/data_disk" >&2
        write_result error
        exit 1
        ;;
    *)
        echo "Unsupported system action: ${ACTION:-<empty>}" >&2
        write_result error
        exit 1
        ;;
esac
