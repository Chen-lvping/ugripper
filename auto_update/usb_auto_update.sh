#!/bin/bash
set -euo pipefail

# ================= 配置区域 =================
# 如果你想只允许固定 USB 口触发（强校验），填你的 by-path（推荐）
USB_DEV_PATH="/dev/disk/by-path/platform-xhci-hcd.1.auto-usb-0:1.3:1.0-scsi-0:0:0:0-part1"

# 业务主程序包前缀
DEB_PREFIX="ugripper_*_arm64"
# Updater 自身更新包前缀
UPDATER_PREFIX="ugripper-usb-updater*"

MOUNT_POINT="/mnt/usb_updater_tmp"
LOG_FILE="/var/log/ugripper/usb_auto_update.log"
LOCK_FILE="/run/usb_auto_update.lock"

# 升级保护锁：安装期间用于抑制 network monitor 的重启/二次触发
UPGRADE_GUARD_FILE="/run/ugripper_installing_from_usb.lock"
NETWORK_MONITOR_SERVICE="ugripper-network-monitor.service"

# 是否强制要求 “触发设备 == 固定口 by-path”
ENFORCE_BY_PATH="0"   # 1=强制校验；0=不校验
# ===========================================

mkdir -p "$(dirname "$LOG_FILE")"

IN_UPGRADE_WINDOW=0

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

stop_service_fast() {
  local service_name="$1"
  local timeout_sec="${2:-6}"

  timeout "${timeout_sec}s" systemctl stop "$service_name" >/dev/null 2>&1 || true

  if systemctl is-active --quiet "$service_name"; then
    log "服务 $service_name 停止超时，执行 kill。"
    systemctl kill "$service_name" >/dev/null 2>&1 || true
  fi
}

stop_record_stack_fast() {
  stop_service_fast "ugripper.service" 6

  # 兜底：防止旧 run_record/音频/灯光进程在安装窗口残留
  if command -v pkill >/dev/null 2>&1; then
    pkill -TERM -f '/opt/ugripper/run_record.sh' >/dev/null 2>&1 || true
    pkill -TERM -f 'led_manager.py' >/dev/null 2>&1 || true
    pkill -TERM -f 'audio/audio_play.py' >/dev/null 2>&1 || true
    sleep 0.3
    pkill -KILL -f '/opt/ugripper/run_record.sh' >/dev/null 2>&1 || true
    pkill -KILL -f 'led_manager.py' >/dev/null 2>&1 || true
    pkill -KILL -f 'audio/audio_play.py' >/dev/null 2>&1 || true
  fi

  rm -f /tmp/umi_audio_pipe /tmp/umi_led_pipe /tmp/umi_recording.lock || true
}

enter_upgrade_window() {
  if [ "$IN_UPGRADE_WINDOW" -eq 1 ]; then
    return
  fi

  log "进入升级保护窗口：创建 guard 并暂停 network monitor。"
  touch "$UPGRADE_GUARD_FILE"
  stop_service_fast "$NETWORK_MONITOR_SERVICE" 4
  IN_UPGRADE_WINDOW=1
}

leave_upgrade_window() {
  if [ "$IN_UPGRADE_WINDOW" -eq 0 ]; then
    return
  fi

  rm -f "$UPGRADE_GUARD_FILE"
  systemctl start "$NETWORK_MONITOR_SERVICE" >/dev/null 2>&1 || true
  IN_UPGRADE_WINDOW=0
  log "退出升级保护窗口。"
}

cleanup() {
  if mountpoint -q "$MOUNT_POINT"; then
    umount "$MOUNT_POINT" || true
  fi
  leave_upgrade_window
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

# ========================================================
# 0. 校准触发（检测 calibration.txt）
# ========================================================
if [ -f "$MOUNT_POINT/calibration.txt" ]; then
  log "检测到 calibration.txt，触发 ugripper-calibration.service，跳过本次升级流程。"
  systemctl start ugripper-calibration.service --no-block || {
    log "触发 ugripper-calibration.service 失败。"
    exit 1
  }
  exit 0
fi

# ========================================================
# 1. 优先检查并更新自身 (Self-Update)
# ========================================================
UPDATER_DEB="$(find "$MOUNT_POINT" -maxdepth 1 -type f -name "${UPDATER_PREFIX}.deb" | head -n 1 || true)"

if [ -n "${UPDATER_DEB:-}" ]; then
  log "发现 Updater 自身更新包：$UPDATER_DEB，开始自我更新..."
  enter_upgrade_window
  stop_record_stack_fast

  # 注意：在 Linux 中，Bash 脚本运行时文件被删除或替换（dpkg 会做原子替换），
  # 当前运行的进程仍持有旧文件的 inode 句柄，因此会继续执行旧脚本剩下的逻辑直到结束。
  # 这是安全的，新逻辑将在下一次触发时生效。
  if dpkg -i --force-overwrite "$UPDATER_DEB"; then
    log "Updater 自我更新成功。"
  else
    log "Updater 自我更新失败 (dpkg error)，将尝试继续后续业务更新。"
  fi
else
  log "未发现自身更新包 ($UPDATER_PREFIX.deb)，跳过自我更新。"
fi

# ========================================================
# 2. 检查并更新业务主程序 (ugripper)
# ========================================================
DEB_FILE="$(find "$MOUNT_POINT" -maxdepth 1 -type f -name "${DEB_PREFIX}*.deb" | head -n 1 || true)"
if [ -z "${DEB_FILE:-}" ]; then
  if [ "$IN_UPGRADE_WINDOW" -eq 1 ]; then
    # 仅升级 updater 时，恢复主服务
    systemctl start ugripper.service >/dev/null 2>&1 || true
  fi
  log "未发现更新包（匹配 ${DEB_PREFIX}*.deb），跳过。"
  exit 0
fi

log "发现更新包：$DEB_FILE，开始安装..."
enter_upgrade_window
stop_record_stack_fast

# 安装更新包
if dpkg -i --force-overwrite "$DEB_FILE"; then
  log "安装/更新成功。"
else
  log "dpkg 安装失败。"
  exit 1
fi

log "usb_auto_update done."
exit 0
