#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PRIMARY_RESOURCE_ROOT="${PRIMARY_RESOURCE_ROOT_OVERRIDE:-/opt/ugripper}"
RESOURCE_ROOT="${RESOURCE_ROOT_OVERRIDE:-$PRIMARY_RESOURCE_ROOT}"
if [ ! -d "$RESOURCE_ROOT" ]; then
  RESOURCE_ROOT="$PROJECT_ROOT"
fi
UPDATER_RESOURCE_ROOT="${UPDATER_RESOURCE_ROOT_OVERRIDE:-/usr/local/lib/ugripper-usb-updater}"

resolve_existing_file() {
  local candidate=""

  for candidate in "$@"; do
    if [ -f "$candidate" ]; then
      printf '%s' "$candidate"
      return 0
    fi
  done

  return 1
}

SHELL_COMMON="$(resolve_existing_file \
  "${SHELL_COMMON_OVERRIDE:-}" \
  "$RESOURCE_ROOT/scripts/lib/ugripper_shell_common.sh" \
  "$PROJECT_ROOT/scripts/lib/ugripper_shell_common.sh" \
  "/usr/local/scripts/lib/ugripper_shell_common.sh" \
  || true)"

if [ -z "$SHELL_COMMON" ]; then
  echo "[usb-update][ERROR] missing shell helper: ${SHELL_COMMON_OVERRIDE:-$RESOURCE_ROOT/scripts/lib/ugripper_shell_common.sh}"
  exit 1
fi

# shellcheck source=/dev/null
. "$SHELL_COMMON"

# ================= 配置区域 =================
DATA_MOUNT_POINT="/mnt/data_disk"
MOUNT_POINT="$DATA_MOUNT_POINT"
LOG_FILE="/var/log/ugripper/usb_auto_update.log"
LOCK_FILE="/run/usb_auto_update.lock"
DATA_MOUNT_WAIT_RETRIES=30
DATA_MOUNT_WAIT_INTERVAL_SEC="0.2"
LOCK_BUSY_RERUN_EXIT_CODE=75

# 升级保护锁：安装期间用于抑制 network monitor 的重启/二次触发
UPGRADE_GUARD_FILE="/run/ugripper_installing_from_usb.lock"
NETWORK_MONITOR_SERVICE="ugripper-network-monitor.service"
POSTINST_SKIP_RERUN_MARKER="/run/ugripper_usb_update_skip_postinst_rerun"
SELF_UPDATE_PENDING_MARKER="/run/ugripper_usb_update_pending_completion"

AUTO_INSTALL_PACKAGES=(
  "ugripper-usb-updater"
  "ugripper"
  "bluetooth-gatt-server"
  "databot-device-joint"
  "device-ota-mender"
)

# ===========================================

DEFAULT_HMI_HELPER_BIN="$RESOURCE_ROOT/bin/GripperHmiTool/GripperHmiTool"
HMI_HELPER_BIN="${HMI_HELPER_BIN_OVERRIDE:-$DEFAULT_HMI_HELPER_BIN}"
HMI_PORT_ARGS=(--port /dev/right_gripper --port /dev/left_gripper)
CALIB_IMPORT_SCRIPT="${CALIB_IMPORT_SCRIPT_OVERRIDE:-$(resolve_existing_file \
  "$UPDATER_RESOURCE_ROOT/auto_calibration/import_camera_calibration.sh" \
  "$RESOURCE_ROOT/auto_calibration/import_camera_calibration.sh" \
  "$PROJECT_ROOT/auto_calibration/import_camera_calibration.sh" \
  || true)}"
FALLBACK_CAM_JSON="$RESOURCE_ROOT/bin/UgripperRuntime/config/fakeCamCalib.json"
HMI_LOCK_FILE="${HMI_LOCK_FILE_OVERRIDE:-/run/ugripper_hmi_operation.lock}"
HMI_LOCK_TIMEOUT_SEC="${HMI_LOCK_TIMEOUT_SEC_OVERRIDE:-30}"
LED_HELPER_DURATION_SEC="${LED_HELPER_DURATION_SEC_OVERRIDE:-1}"
LED_RUN_USER="ubuntu"
PID_LED_SHELL=""

mkdir -p "$(dirname "$LOG_FILE")"

IN_UPGRADE_WINDOW=0
NEED_UGRIPPER_RESTART=0
PACKAGE_INSTALL_WINDOW_READY=0
PACKAGES_CHANGED=0
RERUN_AFTER_SELF_UPDATE=0
UGRIPPER_PACKAGE_INSTALLED=0

log() {
  ugripper_log "usb-update" "INFO" "$*" | tee -a "$LOG_FILE"
}

realpath_or_empty() {
  local path="${1:-}"

  [ -n "$path" ] || return 0
  readlink -f "$path" 2>/dev/null || true
}

mount_source() {
  local mountpoint="$1"

  findmnt -rn -o SOURCE --mountpoint "$mountpoint" 2>/dev/null || true
}

mount_source_matches_device() {
  local mountpoint="$1"
  local devnode="$2"
  local mounted_source real_mounted_source real_devnode

  mounted_source="$(mount_source "$mountpoint")"
  [ -n "$mounted_source" ] || return 1

  real_mounted_source="$(realpath_or_empty "$mounted_source")"
  real_devnode="$(realpath_or_empty "$devnode")"

  [ -n "$real_mounted_source" ] || return 1
  [ -n "$real_devnode" ] || return 1
  [ "$real_mounted_source" = "$real_devnode" ]
}

prepare_scan_mountpoint() {
  local current_source retry_count

  retry_count=0
  while [ "$retry_count" -lt "$DATA_MOUNT_WAIT_RETRIES" ]; do
    if mount_source_matches_device "$DATA_MOUNT_POINT" "$DEV_NODE"; then
      MOUNT_POINT="$DATA_MOUNT_POINT"
      log "检测到 ${DATA_MOUNT_POINT} 已挂载当前设备，复用统一入口检查内容。"
      return 0
    fi

    current_source="$(mount_source "$DATA_MOUNT_POINT")"
    if [ -n "$current_source" ] && [ -n "$(realpath_or_empty "$current_source")" ]; then
      log "${DATA_MOUNT_POINT} 当前挂载的是其他设备（source=$current_source），跳过本次内容检查。"
      return 2
    fi

    retry_count=$((retry_count + 1))
    sleep "$DATA_MOUNT_WAIT_INTERVAL_SEC"
  done

  log "等待 ${DATA_MOUNT_POINT} 挂载当前设备超时，跳过本次内容检查。"
  return 1
}

trim_text() {
  ugripper_trim_text "$1"
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

read_env_file_value() {
  local env_file="$1"
  local env_key="$2"

  ugripper_read_env_value "$env_file" "$env_key"
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

get_pkg_status() {
  local pkg_name="$1"

  dpkg-query -W -f='${Status}' "$pkg_name" 2>/dev/null || true
}

get_installed_version() {
  local pkg_name="$1"
  local status

  status="$(get_pkg_status "$pkg_name")"
  if [ "$status" = "install ok installed" ]; then
    dpkg-query -W -f='${Version}' "$pkg_name" 2>/dev/null || true
  fi
}

get_deb_field() {
  local deb_file="$1"
  local field_name="$2"

  dpkg-deb -f "$deb_file" "$field_name" 2>/dev/null || true
}

find_best_usb_deb_for_package() {
  local pkg_name="$1"
  local best_deb=""
  local best_ver=""
  local deb_file=""
  local deb_pkg=""
  local deb_ver=""

  while IFS= read -r -d '' deb_file; do
    deb_pkg="$(get_deb_field "$deb_file" Package)"
    [ "$deb_pkg" = "$pkg_name" ] || continue

    deb_ver="$(get_deb_field "$deb_file" Version)"
    [ -n "$deb_ver" ] || continue

    if [ -z "$best_ver" ] || dpkg --compare-versions "$deb_ver" gt "$best_ver"; then
      best_deb="$deb_file"
      best_ver="$deb_ver"
    fi
  done < <(find "$MOUNT_POINT" -maxdepth 1 -type f -name '*.deb' -print0 2>/dev/null)

  printf '%s' "$best_deb"
}

select_upgrade_completed_sound() {
  local env_lang="zh"
  local raw_lang=""
  local normalized_lang=""
  local primary_en_sound="$RESOURCE_ROOT/bin/UgripperRuntime/audio_en/upgrade_completed.wav"
  local primary_zh_sound="$RESOURCE_ROOT/bin/UgripperRuntime/audio/upgrade_completed.wav"

  raw_lang="$(read_env_file_value "/etc/environment" "UGRIPPER_LANG" || true)"
  if normalized_lang="$(normalize_language "$raw_lang")"; then
    env_lang="$normalized_lang"
  fi

  if [ "$env_lang" = "en" ]; then
    ugripper_resolve_existing_path \
      "$primary_en_sound" \
      "$primary_zh_sound"
    return 0
  fi

  ugripper_resolve_existing_path \
    "$primary_zh_sound" \
    "$primary_en_sound"
  return 0
}

resolve_audio_module_dir() {
  printf '%s' "$RESOURCE_ROOT/bin/UgripperRuntime/audio"
}

play_upgrade_completed_sound() {
  local sound_path=""
  local python_bin=""
  local audio_module_dir=""

  sound_path="$(select_upgrade_completed_sound || true)"
  if [ -z "$sound_path" ]; then
    log "未找到 upgrade_completed.wav，跳过升级完成提示音。"
    return 1
  fi

  if ! command -v paplay >/dev/null 2>&1; then
    log "paplay 不存在，跳过升级完成提示音。"
    return 1
  fi

  if ! command -v runuser >/dev/null 2>&1; then
    log "runuser 不存在，跳过升级完成提示音。"
    return 1
  fi

  if ! id "$LED_RUN_USER" >/dev/null 2>&1; then
    log "播放提示音所需用户不存在：$LED_RUN_USER，跳过升级完成提示音。"
    return 1
  fi

  python_bin="$RESOURCE_ROOT/.venv/bin/python3"
  if [ ! -x "$python_bin" ]; then
    python_bin="$(command -v python3 || true)"
  fi

  if [ -z "$python_bin" ]; then
    log "未找到可用的 python3，跳过升级完成提示音。"
    return 1
  fi

  audio_module_dir="$(resolve_audio_module_dir)"
  if [ ! -d "$audio_module_dir" ]; then
    log "未找到音频模块目录，跳过升级完成提示音。"
    return 1
  fi

  log "安装流程完成，开始通过 PulseAudio 播放升级完成提示音：$sound_path"
  if timeout 20s runuser -u "$LED_RUN_USER" -- env \
    UPGRADE_SOUND_PATH="$sound_path" \
    PYTHONPATH="$audio_module_dir" \
    "$python_bin" - <<'PY'
import os
import subprocess
from pathlib import Path

from pulse_audio_utils import configure_pulse_audio_env, pulse_audio_env

sound_path = Path(os.environ["UPGRADE_SOUND_PATH"])
if not sound_path.is_file():
    raise FileNotFoundError(f"sound file not found: {sound_path}")

target = configure_pulse_audio_env(require_source=False, disable_suspend_on_idle=True)
env = pulse_audio_env()
subprocess.run(["paplay", f"--device={target.sink.name}", str(sound_path)], check=True, env=env)
PY
  then
    log "升级完成提示音播放成功。"
    return 0
  fi

  log "升级完成提示音播放失败（已忽略，不影响安装结果）。"
  return 1
}

self_update_pending_completion() {
  [ -f "$SELF_UPDATE_PENDING_MARKER" ]
}

mark_self_update_pending_completion() {
  printf '%s\n' "$DEV_NODE" > "$SELF_UPDATE_PENDING_MARKER"
}

clear_self_update_pending_completion() {
  rm -f "$SELF_UPDATE_PENDING_MARKER"
}

should_play_upgrade_completed_sound() {
  if [ "$PACKAGES_CHANGED" -gt 0 ]; then
    return 0
  fi

  if [ "$RERUN_AFTER_SELF_UPDATE" -eq 1 ] && self_update_pending_completion; then
    return 0
  fi

  return 1
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

systemd_unit_exists() {
  local service_name="$1"

  [ -e "/etc/systemd/system/$service_name" ] && return 0
  [ -e "/lib/systemd/system/$service_name" ] && return 0
  [ -e "/usr/lib/systemd/system/$service_name" ] && return 0

  systemctl list-unit-files "$service_name" --no-legend 2>/dev/null | grep -q "^$service_name[[:space:]]"
}

stop_record_stack_fast() {
  local hmi_patterns=(
    'GripperHmiTool.*--state'
    'gripper_hmi_test.*--state'
  )
  local runtime_patterns=(
    '/opt/ugripper/run_record.sh'
    '/opt/ugripper/bin/UgripperRuntime/UgripperRuntime'
    'bin/UgripperRuntime/UgripperRuntime'
    'bin/CameraRecorder/CameraRecorder'
    'CameraRecorder.*--stereo-daemon'
    'audio/audio_play.py'
  )
  local pattern=""
  local port=""
  local waited=0

  stop_service_fast "ugripper.service" 6

  # 兜底：防止旧 run_record/runtime/camera/音频/灯光进程在安装窗口残留。
  if command -v pkill >/dev/null 2>&1; then
    for pattern in "${runtime_patterns[@]}"; do
      pkill -TERM -f "$pattern" >/dev/null 2>&1 || true
    done
    for pattern in "${hmi_patterns[@]}"; do
      pkill -TERM -f "$pattern" >/dev/null 2>&1 || true
    done
    sleep 0.5
    for pattern in "${runtime_patterns[@]}"; do
      pkill -KILL -f "$pattern" >/dev/null 2>&1 || true
    done
    for pattern in "${hmi_patterns[@]}"; do
      pkill -KILL -f "$pattern" >/dev/null 2>&1 || true
    done
  fi

  if command -v fuser >/dev/null 2>&1; then
    while [ "$waited" -lt 20 ]; do
      local busy=0
      for port in /dev/left_gripper /dev/right_gripper; do
        [ -e "$port" ] || continue
        if fuser "$port" >/dev/null 2>&1; then
          busy=1
        fi
      done
      [ "$busy" -eq 0 ] && break
      sleep 0.1
      waited=$((waited + 1))
    done

    for port in /dev/left_gripper /dev/right_gripper; do
      [ -e "$port" ] || continue
      if fuser "$port" >/dev/null 2>&1; then
        log "夹爪串口仍被占用，强制释放：$port"
        fuser -k "$port" >/dev/null 2>&1 || true
      fi
    done
  fi

  rm -f /dev/shm/ugripper/umi_audio_pipe \
    /dev/shm/ugripper/umi_audio_ready \
    /dev/shm/ugripper/umi_record_control.pipe \
    /dev/shm/ugripper/umi_stereo_camera_control.pipe \
    /dev/shm/ugripper/umi_left_fays_cmd \
    /dev/shm/ugripper/umi_right_fays_cmd \
    /tmp/umi_recording.lock || true
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

  (
    while :; do
      run_hmi_helper_queued --state "$state" --led-only --duration "$LED_HELPER_DURATION_SEC" >/dev/null 2>&1 || true
      sleep 0.05
    done
  ) &

  PID_LED_SHELL=$!
  sleep 0.2
}

run_hmi_helper_queued() {
  if [ ! -x "$HMI_HELPER_BIN" ]; then
    return 1
  fi

  if command -v flock >/dev/null 2>&1; then
    if id "$LED_RUN_USER" >/dev/null 2>&1; then
      flock -w "$HMI_LOCK_TIMEOUT_SEC" "$HMI_LOCK_FILE" \
        runuser -u "$LED_RUN_USER" -- "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" "$@"
    else
      flock -w "$HMI_LOCK_TIMEOUT_SEC" "$HMI_LOCK_FILE" \
        "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" "$@"
    fi
    return $?
  fi

  if id "$LED_RUN_USER" >/dev/null 2>&1; then
    runuser -u "$LED_RUN_USER" -- "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" "$@"
  else
    "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" "$@"
  fi
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

  if ! systemd_unit_exists "ugripper.service"; then
    log "ugripper.service 未安装，跳过重启。"
    NEED_UGRIPPER_RESTART=0
    return 0
  fi

  log "检测到配置已更新，重启 ugripper.service 以应用最新配置。"
  if systemctl restart ugripper.service >/dev/null 2>&1; then
    log "ugripper.service 重启成功。"
    NEED_UGRIPPER_RESTART=0
    return 0
  fi

  log "ugripper.service 重启失败。"
  return 1
}

ensure_package_install_window() {
  if [ "$PACKAGE_INSTALL_WINDOW_READY" -eq 1 ]; then
    return 0
  fi

  enter_upgrade_window
  stop_record_stack_fast
  PACKAGE_INSTALL_WINDOW_READY=1
}

install_usb_package_if_needed() {
  local pkg_name="$1"
  local deb_file=""
  local deb_version=""
  local installed_version=""
  local action_label=""

  deb_file="$(find_best_usb_deb_for_package "$pkg_name")"
  if [ -z "$deb_file" ]; then
    log "未在 U 盘发现 [$pkg_name] 安装包，跳过。"
    return 0
  fi

  deb_version="$(get_deb_field "$deb_file" Version)"
  if [ -z "$deb_version" ]; then
    log "无法读取 [$pkg_name] 包版本信息：$deb_file"
    return 1
  fi

  installed_version="$(get_installed_version "$pkg_name")"

  ensure_package_install_window

  if [ "$pkg_name" = "ugripper-usb-updater" ]; then
    printf '%s\n' "skip-postinst-rerun" > "$POSTINST_SKIP_RERUN_MARKER"
    mark_self_update_pending_completion
  fi

  if [ -n "$installed_version" ]; then
    if [ "$installed_version" = "$deb_version" ]; then
      action_label="重装"
    else
      action_label="升级"
    fi
  else
    action_label="安装"
  fi

  log "发现 [$pkg_name] 包：$deb_file（usb=$deb_version, installed=${installed_version:-not-installed}），开始${action_label}..."
  if dpkg -i --force-overwrite "$deb_file"; then
    log "[$pkg_name] ${action_label}成功。"
    PACKAGES_CHANGED=$((PACKAGES_CHANGED + 1))
    if [ "$pkg_name" = "ugripper" ]; then
      UGRIPPER_PACKAGE_INSTALLED=1
    fi
    return 0
  fi

  log "[$pkg_name] ${action_label}失败。"
  return 1
}

restore_ugripper_service() {
  restart_ugripper_if_needed || true

  if ! systemd_unit_exists "ugripper.service"; then
    log "ugripper.service 未安装，跳过业务服务恢复。"
    return 0
  fi

  if systemctl is-active --quiet ugripper.service; then
    return 0
  fi

  log "安装流程结束后 ugripper.service 未运行，尝试拉起。"
  if systemctl start ugripper.service >/dev/null 2>&1; then
    log "ugripper.service 启动成功。"
    return 0
  fi

  log "ugripper.service 启动失败。"
  return 1
}

apply_imports_with_led_feedback() {
  local cfg_lang="$1"
  local cfg_codec="$2"
  local do_calib_import="$3"
  local defer_restart_for_calibration="$4"
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

  if [ "$do_calib_import" = "1" ]; then
    log "检测到 ugripper_calib，开始导入标定数据。"
    if [ ! -x "$CALIB_IMPORT_SCRIPT" ]; then
      log "标定导入脚本不存在或不可执行：$CALIB_IMPORT_SCRIPT"
      has_failure=1
    elif HMI_LOCK_FILE_OVERRIDE="$HMI_LOCK_FILE" \
         HMI_LOCK_TIMEOUT_SEC_OVERRIDE="$HMI_LOCK_TIMEOUT_SEC" \
         HMI_HELPER_BIN_OVERRIDE="$HMI_HELPER_BIN" \
         FALLBACK_CAM_JSON_OVERRIDE="$FALLBACK_CAM_JSON" \
         "$CALIB_IMPORT_SCRIPT" "$MOUNT_POINT"; then
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
  if ! systemd_unit_exists "ugripper.service"; then
    log "ugripper.service 未安装，跳过导入后的服务重启。"
    NEED_UGRIPPER_RESTART=0
  elif systemctl restart ugripper.service >/dev/null 2>&1; then
    log "ugripper.service 重启成功。"
    NEED_UGRIPPER_RESTART=0
  else
    log "ugripper.service 重启失败。"
    has_failure=1
  fi

  [ "$has_failure" -eq 0 ]
}

cleanup() {
  stop_led_helper
  rm -f "$POSTINST_SKIP_RERUN_MARKER"
  leave_upgrade_window
}
trap cleanup EXIT

# udev 传进来的设备节点，如 /dev/sda1
DEV_NODE=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --rerun-after-self-update)
      RERUN_AFTER_SELF_UPDATE=1
      ;;
    --*)
      log "忽略未知参数：$1"
      ;;
    *)
      if [ -z "$DEV_NODE" ]; then
        DEV_NODE="$1"
      else
        log "忽略额外位置参数：$1"
      fi
      ;;
  esac
  shift
done

# 防并发：udev 可能短时间触发多次
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
  if [ "$RERUN_AFTER_SELF_UPDATE" -eq 1 ]; then
    log "自更新后的重入流程等待锁仍被占用，稍后由 postinst 再试。"
    exit "$LOCK_BUSY_RERUN_EXIT_CODE"
  fi
  log "已有更新实例在运行，退出。"
  exit 0
fi

if [ -z "$DEV_NODE" ]; then
  log "未传入设备节点参数，退出。"
  exit 0
fi

if [ ! -b "$DEV_NODE" ]; then
  log "设备节点不是块设备：$DEV_NODE，退出。"
  exit 0
fi

log "Triggered by device: $DEV_NODE"
if [ "$RERUN_AFTER_SELF_UPDATE" -eq 1 ]; then
  log "当前为 usb-updater 自更新后的重入安装流程。"
fi

if prepare_scan_mountpoint; then
  :
else
  rc=$?
  if [ "$rc" -eq 2 ]; then
    exit 0
  fi
  exit 1
fi

CONFIG_FILE="$MOUNT_POINT/config.txt"
HAS_CALIB_IMPORT=0
if [ -f "$CONFIG_FILE" ]; then
  log "检测到 config.txt，检查语言与编码器配置。"
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
else
  log "未检测到 config.txt，跳过语言与编码器配置。"
fi

if [ -d "$MOUNT_POINT/ugripper_calib" ]; then
  HAS_CALIB_IMPORT=1
fi

HAS_CALIBRATION_TRIGGER=0
if [ -f "$MOUNT_POINT/calibration.txt" ]; then
  HAS_CALIBRATION_TRIGGER=1
  log "检测到 calibration.txt：本次 U 盘流程会在导入阶段后追加 encoder 零位校准。"
fi

if [ -n "${CONFIG_LANG:-}" ] || [ -n "${CONFIG_CAMERA_CODEC:-}" ] || [ "$HAS_CALIB_IMPORT" -eq 1 ]; then
  if ! apply_imports_with_led_feedback "${CONFIG_LANG:-}" "${CONFIG_CAMERA_CODEC:-}" "$HAS_CALIB_IMPORT" "$HAS_CALIBRATION_TRIGGER"; then
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
    if systemd_unit_exists "ugripper.service"; then
      systemctl start ugripper.service >/dev/null 2>&1 || true
    else
      log "ugripper.service 未安装，跳过校准失败后的服务恢复。"
    fi
    exit 1
  fi
  exit 0
fi

for pkg_name in "${AUTO_INSTALL_PACKAGES[@]}"; do
  if [ "$pkg_name" = "ugripper-usb-updater" ] && [ "$RERUN_AFTER_SELF_UPDATE" -eq 1 ]; then
    log "当前为自更新后的重入流程，跳过重复安装 [ugripper-usb-updater]。"
    continue
  fi

  install_usb_package_if_needed "$pkg_name"
done

if [ "$PACKAGES_CHANGED" -eq 0 ]; then
  log "未检测到需要安装或升级的目标软件包，跳过 deb 安装。"
fi

if should_play_upgrade_completed_sound; then
  play_upgrade_completed_sound || true
  clear_self_update_pending_completion
fi

restore_ugripper_service || true

log "usb_auto_update done."
exit 0
