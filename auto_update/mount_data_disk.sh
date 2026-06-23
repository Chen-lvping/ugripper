#!/bin/bash
set -euo pipefail

ACTION="${1:-add}"
DEVNODE="${2:-}"
FS_TYPE="${3:-}"
MOUNT_POINT="/mnt/data_disk"
LOCK_FILE="/run/ugripper_data_disk_mount.lock"
TARGET_USER="ubuntu"
TARGET_UID="$(id -u "$TARGET_USER" 2>/dev/null || echo 1000)"
TARGET_GID="$(id -g "$TARGET_USER" 2>/dev/null || echo 1000)"
MOUNT_OPTS="uid=${TARGET_UID},gid=${TARGET_GID},noatime"
UNMOUNT_TIMEOUT_SEC=5

current_source() {
    findmnt -rn -o SOURCE --mountpoint "$MOUNT_POINT" 2>/dev/null || true
}

mountpoints_for_source() {
    local source="${1:-}"
    [ -n "$source" ] || return 0
    findmnt -rn -S "$source" -o TARGET 2>/dev/null || true
}

mount_active() {
    findmnt -rn --mountpoint "$MOUNT_POINT" >/dev/null 2>&1
}

source_exists() {
    local source="${1:-}"
    [ -n "$source" ] && [ -e "$source" ]
}

run_unmount_with_timeout() {
    timeout -k 1s "${UNMOUNT_TIMEOUT_SEC}s" "$@" >/dev/null 2>&1 || true
}

run_mount_command() {
    # Do not leak the lock fd into long-lived mount helpers.
    "$@" 9>&-
}

ensure_mountpoint_dir() {
    mkdir -p "$MOUNT_POINT"
    chown root:root "$MOUNT_POINT" >/dev/null 2>&1 || true
    chmod 0555 "$MOUNT_POINT" >/dev/null 2>&1 || true
}

cleanup_mountpoint() {
    local source="${1:-}"
    if [ -n "$source" ]; then
        while IFS= read -r target; do
            [ -n "$target" ] || continue
            [ "$target" != "$MOUNT_POINT" ] || continue
            run_unmount_with_timeout /usr/bin/systemd-umount "$target"
            if findmnt -rn --mountpoint "$target" >/dev/null 2>&1; then
                run_unmount_with_timeout umount "$target"
            fi
            if findmnt -rn --mountpoint "$target" >/dev/null 2>&1; then
                run_unmount_with_timeout umount -l "$target"
            fi
        done < <(mountpoints_for_source "$source")
    fi

    if mount_active; then
        run_unmount_with_timeout /usr/bin/systemd-umount "$MOUNT_POINT"
    fi
    if mount_active; then
        run_unmount_with_timeout umount "$MOUNT_POINT"
    fi
    if mount_active; then
        run_unmount_with_timeout umount -l "$MOUNT_POINT"
    fi
    if mount_active && command -v fusermount >/dev/null 2>&1; then
        run_unmount_with_timeout fusermount -u -z "$MOUNT_POINT"
    fi
    if mount_active && command -v fusermount3 >/dev/null 2>&1; then
        run_unmount_with_timeout fusermount3 -u -z "$MOUNT_POINT"
    fi

    if mount_active; then
        echo "mount_data_disk.sh: failed to clean mountpoint $MOUNT_POINT" >&2
        return 1
    fi

    ensure_mountpoint_dir
    return 0
}

exec 9>"$LOCK_FILE"
flock -x 9
ensure_mountpoint_dir

case "$ACTION" in
    remove)
        CURRENT_SOURCE="$(current_source)"
        if [ -z "$CURRENT_SOURCE" ]; then
            ensure_mountpoint_dir
            exit 0
        fi
        if [ -n "$DEVNODE" ] && [ "$CURRENT_SOURCE" != "$DEVNODE" ] && source_exists "$CURRENT_SOURCE"; then
            ensure_mountpoint_dir
            exit 0
        fi
        cleanup_mountpoint "$CURRENT_SOURCE"
        exit 0
        ;;
    add)
        ;;
    *)
        echo "mount_data_disk.sh: unsupported action: $ACTION" >&2
        exit 1
        ;;
esac

if [ -z "$DEVNODE" ]; then
    echo "mount_data_disk.sh: missing device node" >&2
    exit 1
fi

CURRENT_SOURCE="$(current_source)"
if [ "$CURRENT_SOURCE" = "$DEVNODE" ] && source_exists "$CURRENT_SOURCE"; then
    exit 0
fi

if [ -n "$CURRENT_SOURCE" ]; then
    cleanup_mountpoint "$CURRENT_SOURCE" || exit 1
fi

ensure_mountpoint_dir

if [ "$FS_TYPE" = "exfat" ]; then
    if run_mount_command mount -t exfat -o "$MOUNT_OPTS" "$DEVNODE" "$MOUNT_POINT"; then
        exit 0
    fi
else
    if run_mount_command /usr/bin/systemd-mount --collect -o "$MOUNT_OPTS" "$DEVNODE" "$MOUNT_POINT"; then
        exit 0
    fi
fi

cleanup_mountpoint || true
exit 1
