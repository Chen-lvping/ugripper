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

HMI_HELPER_BIN="/opt/ugripper/build/src/gripper_hmi/gripper_hmi_test"
HMI_PORT_ARGS=(--port /dev/right_gripper --port /dev/left_gripper)
CALIB_IMPORT_SCRIPT="/opt/ugripper/auto_calibration/import_camera_calibration.sh"
LED_RUN_USER="radxa"
PID_LED_SHELL=""

mkdir -p "$(dirname "$LOG_FILE")"

IN_UPGRADE_WINDOW=0
NEED_UGRIPPER_RESTART=0

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

trim_text() {
  local text="$1"
  text="${text#"${text%%[![:space:]]*}"}"
  text="${text%"${text##*[![:space:]]}"}"
  printf '%s' "$text"
}

normalize_language() {
  local raw="$1"
  local normalized

  normalized="$(trim_text "$raw")"
  normalized="${normalized#\"}"
  normalized="${normalized%\"}"
  normalized="${normalized#\'}"
  normalized="${normalized%\'}"
  normalized="$(trim_text "$normalized")"
  normalized="${normalized,,}"

  case "$normalized" in
    en|english|en_us|en_gb)
      printf 'en'
      return 0
      ;;
    zh|cn|zh_cn|chinese|zh_hans|zh-hans|中文)
      printf 'zh'
      return 0
      ;;
  esac

  return 1
}

normalize_camera_codec() {
  local raw="$1"
  local normalized

  normalized="$(trim_text "$raw")"
  normalized="${normalized#\"}"
  normalized="${normalized%\"}"
  normalized="${normalized#\'}"
  normalized="${normalized%\'}"
  normalized="$(trim_text "$normalized")"
  normalized="${normalized,,}"

  case "$normalized" in
    h264|h265)
      printf '%s' "$normalized"
      return 0
      ;;
  esac

  return 1
}

normalize_device_role() {
  local raw="$1"
  local normalized

  normalized="$(trim_text "$raw")"
  normalized="${normalized#\"}"
  normalized="${normalized%\"}"
  normalized="${normalized#\'}"
  normalized="${normalized%\'}"
  normalized="$(trim_text "$normalized")"
  normalized="${normalized,,}"

  case "$normalized" in
    master|m)
      printf 'master'
      return 0
      ;;
    slave|s)
      printf 'slave'
      return 0
      ;;
  esac

  return 1
}

parse_language_from_config() {
  local config_file="$1"
  local line key value lang

  [ -f "$config_file" ] || return 1

  while IFS= read -r line || [ -n "$line" ]; do
    line="$(trim_text "$line")"
    [ -z "$line" ] && continue

    case "$line" in
      \#*|\;*)
        continue
        ;;
    esac

    if [ "${line#*=}" = "$line" ]; then
      continue
    fi

    key="$(trim_text "${line%%=*}")"
    value="$(trim_text "${line#*=}")"
    key="${key,,}"

    case "$key" in
      language|voice_lang|voice_language|lang)
        if lang="$(normalize_language "$value")"; then
          printf '%s' "$lang"
          return 0
        fi
        ;;
    esac
  done < "$config_file"

  return 1
}

parse_camera_codec_from_config() {
  local config_file="$1"
  local line key value codec

  [ -f "$config_file" ] || return 1

  while IFS= read -r line || [ -n "$line" ]; do
    line="$(trim_text "$line")"
    [ -z "$line" ] && continue

    case "$line" in
      \#*|\;*)
        continue
        ;;
    esac

    if [ "${line#*=}" = "$line" ]; then
      continue
    fi

    key="$(trim_text "${line%%=*}")"
    value="$(trim_text "${line#*=}")"
    key="${key,,}"

    case "$key" in
      camera_codec|video_codec|triple_camera_codec|codec)
        if codec="$(normalize_camera_codec "$value")"; then
          printf '%s' "$codec"
          return 0
        fi
        ;;
    esac
  done < "$config_file"

  return 1
}

parse_device_role_from_config() {
  local config_file="$1"
  local line key value role

  [ -f "$config_file" ] || return 1

  while IFS= read -r line || [ -n "$line" ]; do
    line="$(trim_text "$line")"
    [ -z "$line" ] && continue

    case "$line" in
      \#*|\;*)
        continue
        ;;
    esac

    if [ "${line#*=}" = "$line" ]; then
      continue
    fi

    key="$(trim_text "${line%%=*}")"
    value="$(trim_text "${line#*=}")"
    key="${key,,}"

    case "$key" in
      device_role|role)
        if role="$(normalize_device_role "$value")"; then
          printf '%s' "$role"
          return 0
        fi
        ;;
    esac
  done < "$config_file"

  return 1
}

set_ugripper_language() {
  local lang="$1"
  local env_file="/etc/environment"
  local env_key="UGRIPPER_LANG"
  local env_line="${env_key}=${lang}"

  if [ ! -e "$env_file" ]; then
    if ! printf '%s\n' "$env_line" > "$env_file"; then
      log "写入 $env_file 失败，语言设置未生效。"
      return 1
    fi
    export UGRIPPER_LANG="$lang"
    log "已创建 $env_file 并写入 $env_line"
    return 0
  fi

  if grep -qE "^${env_key}=" "$env_file"; then
    if ! sed -i -E "s|^${env_key}=.*$|${env_line}|" "$env_file"; then
      log "更新 $env_file 中 ${env_key} 失败。"
      return 1
    fi
    log "已更新 $env_file 中 ${env_key}=${lang}"
  else
    if ! printf '\n%s\n' "$env_line" >> "$env_file"; then
      log "追加 ${env_key} 到 $env_file 失败。"
      return 1
    fi
    log "已追加 $env_line 到 $env_file"
  fi

  export UGRIPPER_LANG="$lang"
  return 0
}

set_camera_codec() {
  local codec="$1"
  local env_file="/etc/environment"
  local env_key="CAMERA_CODEC"
  local env_line="${env_key}=${codec}"

  if [ ! -e "$env_file" ]; then
    if ! printf '%s\n' "$env_line" > "$env_file"; then
      log "写入 $env_file 失败，编码器设置未生效。"
      return 1
    fi
    export CAMERA_CODEC="$codec"
    log "已创建 $env_file 并写入 $env_line"
    return 0
  fi

  if grep -qE "^${env_key}=" "$env_file"; then
    if ! sed -i -E "s|^${env_key}=.*$|${env_line}|" "$env_file"; then
      log "更新 $env_file 中 ${env_key} 失败。"
      return 1
    fi
    log "已更新 $env_file 中 ${env_key}=${codec}"
  else
    if ! printf '\n%s\n' "$env_line" >> "$env_file"; then
      log "追加 ${env_key} 到 $env_file 失败。"
      return 1
    fi
    log "已追加 $env_line 到 $env_file"
  fi

  export CAMERA_CODEC="$codec"
  return 0
}

set_device_role() {
  local role="$1"
  local env_file="/etc/environment"
  local env_key="DEVICE_ROLE"
  local env_line="${env_key}=${role}"

  if [ ! -e "$env_file" ]; then
    if ! printf '%s\n' "$env_line" > "$env_file"; then
      log "写入 $env_file 失败，设备角色设置未生效。"
      return 1
    fi
    export DEVICE_ROLE="$role"
    log "已创建 $env_file 并写入 $env_line"
    return 0
  fi

  if grep -qE "^${env_key}=" "$env_file"; then
    if ! sed -i -E "s|^${env_key}=.*$|${env_line}|" "$env_file"; then
      log "更新 $env_file 中 ${env_key} 失败。"
      return 1
    fi
    log "已更新 $env_file 中 ${env_key}=${role}"
  else
    if ! printf '\n%s\n' "$env_line" >> "$env_file"; then
      log "追加 ${env_key} 到 $env_file 失败。"
      return 1
    fi
    log "已追加 $env_line 到 $env_file"
  fi

  export DEVICE_ROLE="$role"
  return 0
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
    pkill -TERM -f 'gripper_hmi_test.*--state' >/dev/null 2>&1 || true
    pkill -TERM -f 'audio/audio_play.py' >/dev/null 2>&1 || true
    sleep 0.3
    pkill -KILL -f '/opt/ugripper/run_record.sh' >/dev/null 2>&1 || true
    pkill -KILL -f 'gripper_hmi_test.*--state' >/dev/null 2>&1 || true
    pkill -KILL -f 'audio/audio_play.py' >/dev/null 2>&1 || true
  fi

  rm -f /tmp/umi_audio_pipe /tmp/umi_recording.lock || true
}

kill_tree() {
  local pid="$1"
  local child=""

  [ -n "$pid" ] || return 0

  for child in $(pgrep -P "$pid" 2>/dev/null || true); do
    kill_tree "$child"
  done

  if kill -0 "$pid" >/dev/null 2>&1; then
    kill -TERM "$pid" >/dev/null 2>&1 || true
    sleep 0.05
    kill -KILL "$pid" >/dev/null 2>&1 || true
  fi
}

start_led_helper() {
  if [ ! -x "$HMI_HELPER_BIN" ]; then
    log "HMI helper 不存在：$HMI_HELPER_BIN，跳过配置灯效。"
    return 1
  fi

  return 0
}

set_led_state() {
  local state="$1"

  if [ ! -x "$HMI_HELPER_BIN" ]; then
    return 1
  fi

  stop_led_helper

  if id "$LED_RUN_USER" >/dev/null 2>&1; then
    runuser -u "$LED_RUN_USER" -- "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" --state "$state" --led-only --duration 0 >/dev/null 2>&1 &
  else
    "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" --state "$state" --led-only --duration 0 >/dev/null 2>&1 &
  fi

  PID_LED_SHELL=$!
  sleep 0.2
}

stop_led_helper() {
  if [ -n "$PID_LED_SHELL" ]; then
    kill_tree "$PID_LED_SHELL"
    PID_LED_SHELL=""
  fi
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

restart_ugripper_if_needed() {
  if [ "$NEED_UGRIPPER_RESTART" -ne 1 ]; then
    return 0
  fi

  log "检测到配置已更新，重启 ugripper.service 以应用最新配置。"
  if systemctl restart ugripper.service >/dev/null 2>&1; then
    log "ugripper.service 重启成功。"
    return 0
  fi

  log "ugripper.service 重启失败。"
  return 1
}

apply_imports_with_led_feedback() {
  local cfg_lang="$1"
  local cfg_codec="$2"
  local cfg_role="$3"
  local do_calib_import="$4"
  local defer_restart_for_calibration="$5"
  local yellow_start_ts=0
  local yellow_elapsed=0
  local has_failure=0
  local any_imported=0
  local calib_rc=0

  enter_upgrade_window
  stop_record_stack_fast

  if ! start_led_helper; then
    log "LED helper 启动失败，将继续执行导入流程。"
  fi

  yellow_start_ts=$(date +%s)
  # 复用校准灯效：CALIB_RUN 为黄灯快闪
  set_led_state "CALIB_RUN"

  if [ -n "$cfg_lang" ]; then
    if set_ugripper_language "$cfg_lang"; then
      log "已应用语言配置：UGRIPPER_LANG=$cfg_lang"
      any_imported=1
    else
      log "语言配置写入失败。"
      has_failure=1
    fi
  fi

  if [ -n "$cfg_codec" ]; then
    if set_camera_codec "$cfg_codec"; then
      log "已应用编码器配置：CAMERA_CODEC=$cfg_codec"
      any_imported=1
    else
      log "编码器配置写入失败。"
      has_failure=1
    fi
  fi

  if [ -n "$cfg_role" ]; then
    if set_device_role "$cfg_role"; then
      log "已应用设备角色配置：DEVICE_ROLE=$cfg_role"
      any_imported=1
    else
      log "设备角色配置写入失败。"
      has_failure=1
    fi
  fi

  if [ "$do_calib_import" = "1" ]; then
    log "检测到 ugripper_calib，开始导入 calibration.json。"
    if [ ! -x "$CALIB_IMPORT_SCRIPT" ]; then
      log "标定导入脚本不存在或不可执行：$CALIB_IMPORT_SCRIPT"
      has_failure=1
    elif "$CALIB_IMPORT_SCRIPT" "$MOUNT_POINT"; then
      log "标定导入成功。"
      any_imported=1
    else
      calib_rc=$?
      log "标定导入失败（退出码=$calib_rc）。"
      has_failure=1
    fi
  fi

  yellow_elapsed=$(( $(date +%s) - yellow_start_ts ))
  if [ "$yellow_elapsed" -lt 2 ]; then
    sleep $((2 - yellow_elapsed))
  fi

  if [ "$has_failure" -eq 1 ]; then
    # 导入失败时进入红灯告警
    set_led_state "ERROR_1"
    sleep 2
  else
    # 复用校准灯效：CALIB_DONE 为绿灯完成态
    set_led_state "CALIB_DONE"
    sleep 1
  fi
  stop_led_helper

  if [ "$defer_restart_for_calibration" = "1" ]; then
    if [ "$has_failure" -eq 1 ]; then
      log "导入流程存在失败项，但检测到 calibration.txt；继续后续 encoder 校准，并由校准流程统一恢复 ugripper.service。"
    else
      log "导入流程完成且检测到 calibration.txt，跳过中间重启，交由后续校准流程统一恢复 ugripper.service。"
    fi
    [ "$has_failure" -eq 0 ]
    return
  fi

  if [ "$any_imported" -ne 1 ]; then
    log "未成功导入任何内容，仍尝试重启服务恢复运行。"
  fi

  log "导入流程完成，重启 ugripper.service（仅一次）。"
  if systemctl restart ugripper.service >/dev/null 2>&1; then
    log "ugripper.service 重启成功。"
    NEED_UGRIPPER_RESTART=0
  else
    log "ugripper.service 重启失败。"
    has_failure=1
  fi

  [ "$has_failure" -eq 0 ]
}

cleanup() {
  if mountpoint -q "$MOUNT_POINT"; then
    umount "$MOUNT_POINT" || true
  fi
  stop_led_helper
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

CONFIG_FILE="$MOUNT_POINT/config.txt"
HAS_CALIB_IMPORT=0
if [ -f "$CONFIG_FILE" ]; then
  log "检测到 config.txt，检查语言/编码器/设备角色配置。"
  CONFIG_LANG="$(parse_language_from_config "$CONFIG_FILE" || true)"
  if [ -n "$CONFIG_LANG" ]; then
    log "解析到语言配置：UGRIPPER_LANG=$CONFIG_LANG"
  else
    log "config.txt 未找到有效语言设置（支持 LANGUAGE/VOICE_LANG，值为 zh/en），跳过。"
  fi

  CONFIG_CAMERA_CODEC="$(parse_camera_codec_from_config "$CONFIG_FILE" || true)"
  if [ -n "$CONFIG_CAMERA_CODEC" ]; then
    log "解析到编码器配置：CAMERA_CODEC=$CONFIG_CAMERA_CODEC"
  else
    log "config.txt 未找到有效编码器设置（支持 CAMERA_CODEC/VIDEO_CODEC/TRIPLE_CAMERA_CODEC/CODEC，值为 h264/h265），跳过。"
  fi

  CONFIG_DEVICE_ROLE="$(parse_device_role_from_config "$CONFIG_FILE" || true)"
  if [ -n "$CONFIG_DEVICE_ROLE" ]; then
    log "解析到设备角色配置：DEVICE_ROLE=$CONFIG_DEVICE_ROLE"
  else
    log "config.txt 未找到有效设备角色设置（支持 DEVICE_ROLE/ROLE，值为 master/slave），跳过。"
  fi
else
  log "未检测到 config.txt，跳过语言/编码器/设备角色配置。"
fi

if [ -d "$MOUNT_POINT/ugripper_calib" ]; then
  HAS_CALIB_IMPORT=1
fi

HAS_CALIBRATION_TRIGGER=0
if [ -f "$MOUNT_POINT/calibration.txt" ]; then
  HAS_CALIBRATION_TRIGGER=1
  log "检测到 calibration.txt：本次 U 盘流程会在导入阶段后追加 encoder 零位校准。"
fi

if [ -n "${CONFIG_LANG:-}" ] || [ -n "${CONFIG_CAMERA_CODEC:-}" ] || [ -n "${CONFIG_DEVICE_ROLE:-}" ] || [ "$HAS_CALIB_IMPORT" -eq 1 ]; then
  if ! apply_imports_with_led_feedback "${CONFIG_LANG:-}" "${CONFIG_CAMERA_CODEC:-}" "${CONFIG_DEVICE_ROLE:-}" "$HAS_CALIB_IMPORT" "$HAS_CALIBRATION_TRIGGER"; then
    log "导入流程存在失败项（已执行失败红灯），继续后续流程。"
  else
    log "导入流程成功完成。"
  fi
else
  log "未检测到可导入配置或标定目录，跳过统一导入流程。"
fi

# ========================================================
# 0. 校准触发（导入后处理 calibration.txt）
# ========================================================
if [ "$HAS_CALIBRATION_TRIGGER" -eq 1 ]; then
  log "准备触发 ugripper-calibration.service：仅根目录 calibration.txt 会触发 encoder 零位校准，ugripper_calib 不会误触发。"
  if systemctl start ugripper-calibration.service --no-block; then
    log "已触发 ugripper-calibration.service，跳过本次 deb 升级流程。"
  else
    log "触发 ugripper-calibration.service 失败，尝试恢复 ugripper.service。"
    systemctl start ugripper.service >/dev/null 2>&1 || true
    exit 1
  fi
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
  restart_ugripper_if_needed || true
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

restart_ugripper_if_needed || true

log "usb_auto_update done."
exit 0
