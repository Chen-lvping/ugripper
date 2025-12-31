#!/bin/bash
set -euo pipefail

# ================= 配置区域 =================
# 如果你想只允许固定 USB 口触发（强校验），填你的 by-path（推荐）
USB_DEV_PATH="/dev/disk/by-path/platform-xhci-hcd.1.auto-usb-0:1.3:1.0-scsi-0:0:0:0-part1"

DEB_PREFIX="ugripper_*_arm64"
MOUNT_POINT="/mnt/usb_updater_tmp"

LOG_FILE="/var/log/ugripper/usb_auto_update.log"
LOCK_FILE="/run/usb_auto_update.lock"

# 是否强制要求 “触发设备 == 固定口 by-path”
ENFORCE_BY_PATH="0"   # 1=强制校验；0=不校验（任何U盘都可触发）
# ===========================================

mkdir -p "$(dirname "$LOG_FILE")"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

cleanup() {
  if mountpoint -q "$MOUNT_POINT"; then
    umount "$MOUNT_POINT" || true
  fi
}
trap cleanup EXIT

# 防并发：udev 可能短时间触发多次
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
  log "已有更新实例在运行，退出。"
  exit 0
fi

# udev 传进来的设备节点，如 /dev/sda1
DEV_NODE="${1:-}"

if [ -z "$DEV_NODE" ]; then
  log "未传入设备节点参数，退出。"
  exit 0
fi

if [ ! -b "$DEV_NODE" ]; then
  log "设备节点不是块设备：$DEV_NODE，退出。"
  exit 0
fi

log "Triggered by device: $DEV_NODE"

# （可选）强校验：只允许固定口 by-path
if [ "$ENFORCE_BY_PATH" = "1" ]; then
  if [ ! -e "$USB_DEV_PATH" ]; then
    log "固定口 by-path 不存在：$USB_DEV_PATH（可能还没生成），退出。"
    exit 0
  fi

  REAL_BY_PATH="$(readlink -f "$USB_DEV_PATH" || true)"
  REAL_DEV_NODE="$(readlink -f "$DEV_NODE" || true)"

  if [ -z "$REAL_BY_PATH" ] || [ -z "$REAL_DEV_NODE" ]; then
    log "无法解析设备真实路径，退出。"
    exit 0
  fi

  if [ "$REAL_BY_PATH" != "$REAL_DEV_NODE" ]; then
    log "触发设备不匹配固定口：by-path=$REAL_BY_PATH, trigger=$REAL_DEV_NODE，忽略。"
    exit 0
  fi
fi

mkdir -p "$MOUNT_POINT"

# 挂载（建议 ro，避免写U盘）
if mount "$DEV_NODE" "$MOUNT_POINT" -o ro; then
  log "Mounted $DEV_NODE to $MOUNT_POINT (ro)"
else
  log "挂载失败：$DEV_NODE"
  exit 1
fi

# 查找 deb
DEB_FILE="$(find "$MOUNT_POINT" -maxdepth 1 -type f -name "${DEB_PREFIX}*.deb" | head -n 1 || true)"
if [ -z "${DEB_FILE:-}" ]; then
  log "未发现更新包（匹配 ${DEB_PREFIX}*.deb），跳过。"
  exit 0
fi

log "发现更新包：$DEB_FILE，开始安装..."
if dpkg -i --force-overwrite "$DEB_FILE"; then
  log "安装/更新成功。"
else
  log "dpkg 安装失败。"
  exit 1
fi

# 更新后尝试重启 ugripper.service 应用新版本
if systemctl list-unit-files | awk '{print $1}' | grep -qx "ugripper.service"; then
  log "尝试重启 ugripper.service..."
  systemctl try-restart ugripper.service || log "警告：ugripper.service 重启失败，请检查。"
else
  log "未发现 ugripper.service，跳过重启。"
fi

log "usb_auto_update done."
exit 0
