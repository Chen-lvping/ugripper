#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
ROOT_NAME="$(basename "${ROOT_DIR}")"
BASE_TMP="${ROOT_DIR}/logs"
BASE_DISK="/mnt/data_disk/${ROOT_NAME}/logs"
CURRENT_RUN_FILE="${BASE_TMP}/.current_run_id"
SCRIPT_PATH="$0"
MAINTENANCE_HOLD_FILE="${ROOT_DIR}/maintenance_hold"

usage() {
  cat <<'EOF'
用法:
  bash board_repro_log.sh start [--auto-record] [options]
  bash board_repro_log.sh capture-now [RUN_ID]
  bash board_repro_log.sh stop [RUN_ID]

自动录制参数:
  --auto-record              启动前台软件触发连续录制。
  --no-live-logs             不启动后台 dmesg/journalctl 持续采集。
  --record-count N           录制轮数，默认 0 表示一直跑到手动停止。
  --record-sec N             每轮录制秒数，默认 20。
  --idle-sec N               每轮之间等待秒数，默认 10。
  --start-timeout-sec N      等待起录锁出现的秒数，默认 20。
  --stop-timeout-sec N       等待停录收尾完成的秒数，默认 180。
  --hws-interval-sec N       前台刷新 hws 的间隔秒数，默认 3。
  --control-command NAME     SHORT_UP 或 START_STOP，默认 SHORT_UP。
  --continue-on-error        报错抓日志后等待恢复并继续下一轮，不暂停人工确认。
  --error-cooldown-sec N     报错后继续前的基础等待秒数，默认 60。
  --no-disk-mirror           不把测试脚本日志镜像写入 /mnt/data_disk。
EOF
}

ensure_sudo() {
  if ! sudo -n true >/dev/null 2>&1; then
    if [ -n "${BOARD_SUDO_PASSWORD:-}" ]; then
      printf '%s\n' "${BOARD_SUDO_PASSWORD}" | sudo -S -p '' -v
    else
      sudo -v
    fi
  fi
}

latest_run_id() {
  if [ -n "${1:-}" ]; then
    printf '%s\n' "$1"
    return 0
  fi
  if [ -f "${CURRENT_RUN_FILE}" ]; then
    cat "${CURRENT_RUN_FILE}"
    return 0
  fi
  ls -1dt "${BASE_TMP}"/* 2>/dev/null | head -n1 | xargs -r basename
}

disk_out_for_run() {
  local run_id="$1"
  if findmnt -rn --mountpoint /mnt/data_disk >/dev/null 2>&1 && [ -w /mnt/data_disk ]; then
    if mkdir -p "${BASE_DISK}/${run_id}" 2>/dev/null; then
      printf '%s\n' "${BASE_DISK}/${run_id}"
    fi
  fi
}

start_pipeline() {
  local pidfile="$1"
  local command="$2"
  nohup bash -lc "${command}" >/dev/null 2>&1 &
  echo $! > "${pidfile}"
}

run_hws_once() {
  echo "===== hws $(date '+%F %T %Z') ====="
  if command -v /usr/local/bin/hws >/dev/null 2>&1; then
    /usr/local/bin/hws || true
  else
    echo "/usr/local/bin/hws not found"
  fi
}

colorize_hws() {
  awk '
    BEGIN {
      green = "\033[32m";
      red = "\033[31m";
      yellow = "\033[33m";
      gray = "\033[90m";
      reset = "\033[0m";
    }
    {
      gsub(/OK/, green "OK" reset);
      gsub(/FAIL/, red "FAIL" reset);
      gsub(/ERROR/, red "ERROR" reset);
      gsub(/missing/, red "missing" reset);
      gsub(/not-ready/, red "not-ready" reset);
      gsub(/offline/, red "offline" reset);
      gsub(/未启用/, gray "未启用" reset);
      print;
    }
  '
}

run_hws_once_color() {
  run_hws_once | colorize_hws
}

record_control_pipe="/dev/shm/ugripper/umi_record_control.pipe"
recording_lock="/tmp/umi_recording.lock"

wait_for_control_pipe() {
  local timeout_sec="$1"
  local deadline=$((SECONDS + timeout_sec))
  while [ "${SECONDS}" -lt "${deadline}" ]; do
    if [ -p "${record_control_pipe}" ]; then
      return 0
    fi
    sleep 1
  done
  return 1
}

send_record_command() {
  local command="$1"
  if [ ! -p "${record_control_pipe}" ]; then
    echo "软件录制控制 FIFO 未就绪: ${record_control_pipe}" >&2
    return 1
  fi
  printf '%s\n' "${command}" > "${record_control_pipe}"
}

wait_for_recording_lock_present() {
  local timeout_sec="$1"
  local deadline=$((SECONDS + timeout_sec))
  while [ "${SECONDS}" -lt "${deadline}" ]; do
    if [ -f "${recording_lock}" ]; then
      return 0
    fi
    sleep 1
  done
  return 1
}

wait_for_recording_lock_absent() {
  local timeout_sec="$1"
  local deadline=$((SECONDS + timeout_sec))
  while [ "${SECONDS}" -lt "${deadline}" ]; do
    if [ ! -f "${recording_lock}" ]; then
      return 0
    fi
    sleep 1
  done
  return 1
}

latest_episode_dir() {
  local data_root
  data_root="$(find /mnt/data_disk -mindepth 2 -maxdepth 2 -type d -path '*/data' 2>/dev/null | head -n1 || true)"
  if [ -z "${data_root}" ]; then
    return 0
  fi
  find "${data_root}" -maxdepth 1 -type d -name 'episode_*' 2>/dev/null | sort | tail -n1
}

episode_quality_status() {
  local episode_dir="$1"
  local metadata="${episode_dir}/metadata.json"
  if [ -z "${episode_dir}" ] || [ ! -f "${metadata}" ]; then
    return 0
  fi
  python3 - "${metadata}" <<'PY' 2>/dev/null || true
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
print(data.get("quality_check_status", ""))
print(data.get("quality_check_err_type", ""))
PY
}

episode_error_summary() {
  local episode_dir="$1"
  local err_type="${2:-}"
  local validation_file="${episode_dir}/validation_error.log"
  local detail=""
  if [ -n "${episode_dir}" ] && [ -f "${validation_file}" ]; then
    detail="$(sed -n '1,3p' "${validation_file}" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g')"
  fi

  local hardware="未明确"
  local cause="录制质量校验失败"
  local lower_detail
  lower_detail="$(printf '%s' "${detail}" | tr '[:upper:]' '[:lower:]')"

  case "${lower_detail}" in
    *"right fays"*|*"right stereo"*|*"stereo_right"*|*"right_fays_imu"*)
      hardware="右侧 Fays/stereo 链路（stereo_right / right_fays_imu / fays_data_right）"
      ;;
    *"left fays"*|*"left stereo"*|*"stereo_left"*|*"left_fays_imu"*)
      hardware="左侧 Fays/stereo 链路（stereo_left / left_fays_imu / fays_data_left）"
      ;;
    *"right_gripper"*|*"right gripper"*)
      hardware="右夹爪 HMI"
      ;;
    *"left_gripper"*|*"left gripper"*)
      hardware="左夹爪 HMI"
      ;;
    *"right_encoder"*|*"right encoder"*)
      hardware="右编码器"
      ;;
    *"left_encoder"*|*"left encoder"*)
      hardware="左编码器"
      ;;
    *"cam_right"*|*"right_cam"*)
      hardware="右主相机"
      ;;
    *"cam_left"*|*"left_cam"*)
      hardware="左主相机"
      ;;
    *"tcam_right"*)
      hardware="右触觉相机"
      ;;
    *"tcam_left"*)
      hardware="左触觉相机"
      ;;
    *"data_disk"*|*"disk_"*)
      hardware="数据盘"
      ;;
  esac

  case "${lower_detail}" in
    *"unhealthy during session"*)
      cause="录制过程中设备/recorder 不健康"
      ;;
    *"missing"*|*"not found"*)
      cause="关键文件或设备缺失"
      ;;
    *"finalize"*)
      cause="停录收尾/finalize 失败"
      ;;
    *"duration"*|*"too short"*)
      cause="录制时长异常"
      ;;
  esac

  if [ -z "${detail}" ]; then
    detail="metadata quality_check_err_type=${err_type:-unknown}"
  fi

  echo "错误摘要: ${cause}"
  echo "相关硬件: ${hardware}"
  echo "原始错误: ${detail}"
}

print_runtime_ready_diagnostics() {
  echo "===== 运行时诊断 $(date '+%F %T %Z') ====="
  echo "ugripper.service=$(systemctl is-active ugripper.service 2>/dev/null || true)"
  if [ -p "${record_control_pipe}" ]; then
    ls -l "${record_control_pipe}"
  else
    echo "record_control_pipe_missing=${record_control_pipe}"
  fi
  ps -ef | grep -E 'UgripperRuntime|record_runtime' | grep -v grep || true
  echo "----- 最近 ugripper.service 日志 -----"
  journalctl -u ugripper.service -n 30 --no-pager -l 2>/dev/null || true
}

stop_pidfile() {
  local pidfile="$1"
  if [ ! -f "${pidfile}" ]; then
    return 0
  fi
  local pid
  pid="$(cat "${pidfile}")"
  if [ -n "${pid}" ] && kill -0 "${pid}" >/dev/null 2>&1; then
    pkill -TERM -P "${pid}" >/dev/null 2>&1 || true
    kill "${pid}" >/dev/null 2>&1 || true
    sleep 1
    pkill -KILL -P "${pid}" >/dev/null 2>&1 || true
    kill -9 "${pid}" >/dev/null 2>&1 || true
  fi
  rm -f "${pidfile}"
}

copy_if_needed() {
  local src="$1"
  local dst="$2"
  if [ -n "${dst}" ]; then
    mkdir -p "$(dirname "${dst}")" 2>/dev/null || true
    if ! cp "${src}" "${dst}" 2>/dev/null; then
      echo "WARN: failed to mirror ${src} to ${dst}" >&2
    fi
  fi
}

sudo_prefix() {
  if [ -n "${BOARD_SUDO_PASSWORD:-}" ]; then
    printf 'printf "%%s\\n" "$BOARD_SUDO_PASSWORD" | sudo -S -p ""'
  else
    printf 'sudo'
  fi
}

capture_command() {
  local command="$1"
  local tmp_file="$2"
  local disk_file="${3:-}"
  bash -lc "${command}" > "${tmp_file}"
  copy_if_needed "${tmp_file}" "${disk_file}"
}

require_option_value() {
  local option="$1"
  local value="${2:-}"
  if [ -z "${value}" ]; then
    echo "缺少参数值: ${option}" >&2
    exit 1
  fi
}

start_capture() {
  local live_logs="${1:-1}"
  local disk_mirror="${2:-1}"
  ensure_sudo

  local run_id tmp_out disk_out sudo_cmd
  run_id="$(date '+%Y%m%d_%H%M%S')"
  tmp_out="${BASE_TMP}/${run_id}"
  mkdir -p "${tmp_out}"
  if [ "${disk_mirror}" -eq 1 ]; then
    disk_out="$(disk_out_for_run "${run_id}" || true)"
  else
    disk_out=""
  fi
  sudo_cmd="$(sudo_prefix)"

  printf '%s\n' "${run_id}" > "${CURRENT_RUN_FILE}"
  date '+board_time=%F %T %Z' > "${tmp_out}/start_board_time.log"
  copy_if_needed "${tmp_out}/start_board_time.log" "${disk_out:+${disk_out}/start_board_time.log}"

  if [ "${live_logs}" -eq 1 ]; then
    start_pipeline "${tmp_out}/dmesg_live.pid" \
      "${sudo_cmd} dmesg -wT | tee '${tmp_out}/dmesg_live.log' ${disk_out:+" '${disk_out}/dmesg_live.log'"} >/dev/null"
    start_pipeline "${tmp_out}/ugripper_live.pid" \
      "journalctl -fu ugripper.service -o short-iso --no-pager | tee '${tmp_out}/ugripper_live.log' ${disk_out:+" '${disk_out}/ugripper_live.log'"} >/dev/null"
  fi
  start_pipeline "${tmp_out}/hws_5s.pid" \
    "while true; do echo \"===== \$(date '+%F %T %Z') =====\"; /usr/local/bin/hws; sleep 5; done | tee '${tmp_out}/hws_5s.log' ${disk_out:+" '${disk_out}/hws_5s.log'"} >/dev/null"

  echo "RUN_ID=${run_id}"
  echo "TMP_OUT=${tmp_out}"
  if [ -n "${disk_out}" ]; then
    echo "DISK_OUT=${disk_out}"
  else
    echo "DISK_OUT=<disabled-or-unavailable>"
  fi
}

data_disk_ready() {
  findmnt -rn --mountpoint /mnt/data_disk >/dev/null 2>&1 || return 1
  findmnt -rn --mountpoint /mnt/data_disk -o OPTIONS 2>/dev/null | grep -qw rw || return 1
  [ -w /mnt/data_disk ] || return 1
}

restore_or_system_action_active() {
  pgrep -f 'ugripper_restore_usb|auto_restore_usb' >/dev/null 2>&1 && return 0
  [ -e /run/ugripper/system_action_request ] && return 0
  [ -e /tmp/umi_system_action_request ] && return 0
  return 1
}

hws_all_ready() {
  local out
  out="$(/usr/local/bin/hws 2>&1 || true)"
  printf '%s\n' "${out}" | grep -Eiq 'FAIL|ERROR|missing|not-ready|offline' && return 1
  return 0
}

runtime_ready_for_next_cycle() {
  [ ! -f "${recording_lock}" ] || return 1
  [ -p "${record_control_pipe}" ] || return 1
  [ "$(systemctl is-active ugripper.service 2>/dev/null || true)" = "active" ] || return 1
  data_disk_ready || return 1
  restore_or_system_action_active && return 1
  hws_all_ready || return 1
  return 0
}

wait_for_recovery_ready() {
  local timeout_sec="$1"
  local stable_sec="$2"
  local loop_log="$3"
  local deadline stable_start now
  deadline=$((SECONDS + timeout_sec))
  stable_start=0

  echo "等待运行时/数据盘/硬件从错误或自动复位后恢复，timeout=${timeout_sec}s stable=${stable_sec}s" | tee -a "${loop_log}"
  while [ "${SECONDS}" -lt "${deadline}" ]; do
    now="${SECONDS}"
    if runtime_ready_for_next_cycle; then
      if [ "${stable_start}" -eq 0 ]; then
        stable_start="${now}"
        echo "恢复检查首次通过: $(date '+%F %T %Z')" | tee -a "${loop_log}"
      fi
      if [ $((now - stable_start)) -ge "${stable_sec}" ]; then
        echo "恢复检查稳定通过: $(date '+%F %T %Z')" | tee -a "${loop_log}"
        return 0
      fi
    else
      stable_start=0
    fi
    sleep 5
  done

  echo "WARN: 恢复等待超时，下一轮仍会尝试继续: $(date '+%F %T %Z')" | tee -a "${loop_log}"
  run_hws_once | tee -a "${loop_log}" | colorize_hws
  return 1
}

wait_if_maintenance_requested() {
  local loop_log="$1"
  local max_hold_sec="${2:-1800}"
  local hold_start now

  while [ -f "${MAINTENANCE_HOLD_FILE}" ]; do
    now="$(date +%s)"
    hold_start="$(sed -n '1p' "${MAINTENANCE_HOLD_FILE}" 2>/dev/null || true)"
    case "${hold_start}" in
      ''|*[!0-9]*)
        hold_start="${now}"
        ;;
    esac
    if [ $((now - hold_start)) -gt "${max_hold_sec}" ]; then
      echo "WARN: maintenance hold stale, removing ${MAINTENANCE_HOLD_FILE}" | tee -a "${loop_log}"
      rm -f "${MAINTENANCE_HOLD_FILE}"
      break
    fi
    echo "maintenance hold active, wait before next cycle: $(date '+%F %T %Z')" | tee -a "${loop_log}"
    sleep 5
  done
}

auto_record_loop() {
  local run_id="$1"
  local record_count="$2"
  local record_sec="$3"
  local idle_sec="$4"
  local start_timeout_sec="$5"
  local stop_timeout_sec="$6"
  local hws_interval_sec="$7"
  local control_command="$8"
  local continue_on_error="${9:-0}"
  local error_cooldown_sec="${10:-60}"

  local tmp_out disk_out loop_log disk_loop_log cycle start_command stop_command
  tmp_out="${BASE_TMP}/${run_id}"
  disk_out="$(disk_out_for_run "${run_id}" || true)"
  loop_log="${tmp_out}/auto_record_loop.log"
  disk_loop_log="${disk_out:+${disk_out}/auto_record_loop.log}"

  case "${control_command}" in
    SHORT_UP|short_up)
      start_command="SHORT_UP"
      stop_command="SHORT_UP"
      ;;
    START_STOP|start_stop|START|start)
      start_command="START"
      stop_command="STOP"
      ;;
    *)
      echo "未知控制命令模式: ${control_command}" >&2
      exit 1
      ;;
  esac

  echo "AUTO_RECORD_RUN_ID=${run_id}" | tee -a "${loop_log}"
  copy_if_needed "${loop_log}" "${disk_loop_log}"

  if ! wait_for_control_pipe "${start_timeout_sec}"; then
    echo "软件录制控制 FIFO 在 ${start_timeout_sec}s 内未出现，ugripper 运行时未就绪。" | tee -a "${loop_log}"
    print_runtime_ready_diagnostics | tee -a "${loop_log}"
    capture_now "${run_id}" || true
    copy_if_needed "${loop_log}" "${disk_loop_log}"
    exit 1
  fi

  cycle=1
  while [ "${record_count}" -eq 0 ] || [ "${cycle}" -le "${record_count}" ]; do
    local before_episode after_episode status err_type elapsed error_reason
    wait_if_maintenance_requested "${loop_log}"
    before_episode="$(latest_episode_dir || true)"
    error_reason=""

    {
      echo
      echo "===== cycle ${cycle} start $(date '+%F %T %Z') ====="
      echo "command=${start_command}"
    } | tee -a "${loop_log}"

    run_hws_once | tee -a "${loop_log}" | colorize_hws
    if ! send_record_command "${start_command}"; then
      error_reason="failed_to_send_start"
    elif ! wait_for_recording_lock_present "${start_timeout_sec}"; then
      error_reason="recording_lock_not_created"
    fi

    elapsed=0
    while [ -z "${error_reason}" ] && [ "${elapsed}" -lt "${record_sec}" ]; do
      sleep "${hws_interval_sec}"
      elapsed=$((elapsed + hws_interval_sec))
      run_hws_once | tee -a "${loop_log}" | colorize_hws
      if [ ! -f "${recording_lock}" ]; then
        error_reason="recording_lock_disappeared"
      fi
    done

    if [ -z "${error_reason}" ]; then
      echo "command=${stop_command}" | tee -a "${loop_log}"
      if ! send_record_command "${stop_command}"; then
        error_reason="failed_to_send_stop"
      elif ! wait_for_recording_lock_absent "${stop_timeout_sec}"; then
        error_reason="recording_lock_not_removed"
      fi
    fi

    after_episode="$(latest_episode_dir || true)"
    {
      echo "before_episode=${before_episode:-<none>}"
      echo "after_episode=${after_episode:-<none>}"
    } | tee -a "${loop_log}"

    if [ -z "${error_reason}" ] && [ -n "${after_episode}" ] && [ "${after_episode}" != "${before_episode}" ]; then
      mapfile -t quality < <(episode_quality_status "${after_episode}")
      status="${quality[0]:-}"
      err_type="${quality[1]:-}"
      echo "quality_check_status=${status:-<missing>}" | tee -a "${loop_log}"
      echo "quality_check_err_type=${err_type:-<empty>}" | tee -a "${loop_log}"
      if [ "${status}" = "fail" ]; then
        error_reason="metadata_quality_fail_${err_type:-unknown}"
        episode_error_summary "${after_episode}" "${err_type}" | tee -a "${loop_log}"
      fi
    fi

    copy_if_needed "${loop_log}" "${disk_loop_log}"

    if [ -n "${error_reason}" ]; then
      echo "ERROR_REASON=${error_reason}" | tee -a "${loop_log}"
      capture_now "${run_id}" || true
      copy_if_needed "${loop_log}" "${disk_loop_log}"
      if [ "${continue_on_error}" -eq 1 ]; then
        echo "continue_on_error=1; cooldown ${error_cooldown_sec}s 后等待恢复并继续。" | tee -a "${loop_log}"
        sleep "${error_cooldown_sec}"
        wait_for_recovery_ready 420 20 "${loop_log}" || true
      else
        echo
        echo "自动测试已因错误暂停。"
        echo "请先记录现场现象，确认或恢复硬件连接后，按 Enter 继续。"
        if [ -t 0 ]; then
          read -r _
        else
          echo "未检测到交互输入，等待 60s 后继续。"
          sleep 60
        fi
      fi
      echo "用户确认继续: $(date '+%F %T %Z')" | tee -a "${loop_log}"
      run_hws_once | tee -a "${loop_log}" | colorize_hws
      copy_if_needed "${loop_log}" "${disk_loop_log}"
    fi

    if [ "${idle_sec}" -gt 0 ]; then
      sleep "${idle_sec}"
    fi
    cycle=$((cycle + 1))
  done
}

capture_now() {
  ensure_sudo

  local run_id tmp_out disk_out t_error since until err_id tmp_err disk_err sudo_cmd
  run_id="$(latest_run_id "${1:-}")"
  if [ -z "${run_id}" ]; then
    echo "未找到 RUN_ID，请先启动采集。" >&2
    exit 1
  fi

  tmp_out="${BASE_TMP}/${run_id}"
  disk_out="$(disk_out_for_run "${run_id}" || true)"
  sudo_cmd="$(sudo_prefix)"
  t_error="$(date '+%F %T')"
  since="$(date -d "${t_error} 2 minutes ago" '+%F %T')"
  until="$(date -d "${t_error} 2 minutes" '+%F %T')"
  err_id="error_$(date -d "${t_error}" '+%Y%m%d_%H%M%S')"
  tmp_err="${tmp_out}/${err_id}"
  mkdir -p "${tmp_err}"
  if [ -n "${disk_out}" ]; then
    disk_err="${disk_out}/${err_id}"
    mkdir -p "${disk_err}"
  else
    disk_err=""
  fi

  printf 'T_ERROR=%s\nSINCE=%s\nUNTIL=%s\n' "${t_error}" "${since}" "${until}" > "${tmp_err}/capture_meta.log"
  copy_if_needed "${tmp_err}/capture_meta.log" "${disk_err:+${disk_err}/capture_meta.log}"

  capture_command "${sudo_cmd} journalctl -k --since '${since}' --until '${until}' --no-pager" \
    "${tmp_err}/kernel_pm2min.log" "${disk_err:+${disk_err}/kernel_pm2min.log}"
  capture_command "journalctl -u ugripper.service --since '${since}' --until '${until}' --no-pager -l" \
    "${tmp_err}/ugripper_service_pm2min.log" "${disk_err:+${disk_err}/ugripper_service_pm2min.log}"
  capture_command "/usr/local/bin/hws" \
    "${tmp_err}/hws_at_error.log" "${disk_err:+${disk_err}/hws_at_error.log}"
  capture_command "{ lsusb -t; lsusb; }" \
    "${tmp_err}/lsusb_at_error.log" "${disk_err:+${disk_err}/lsusb_at_error.log}"
  capture_command "${sudo_cmd} dmesg -T" \
    "${tmp_err}/dmesg_full_at_error.log" "${disk_err:+${disk_err}/dmesg_full_at_error.log}"
  capture_command "date '+board_time=%F %T %Z'" \
    "${tmp_err}/board_time_at_capture.log" "${disk_err:+${disk_err}/board_time_at_capture.log}"

  echo "已抓取错误日志:"
  echo "  ${tmp_err}"
  if [ -n "${disk_err}" ]; then
    echo "  ${disk_err}"
  fi
}

stop_capture() {
  local run_id tmp_out
  run_id="$(latest_run_id "${1:-}")"
  if [ -z "${run_id}" ]; then
    echo "未找到 RUN_ID。" >&2
    exit 1
  fi
  tmp_out="${BASE_TMP}/${run_id}"

  stop_pidfile "${tmp_out}/dmesg_live.pid"
  stop_pidfile "${tmp_out}/ugripper_live.pid"
  stop_pidfile "${tmp_out}/hws_5s.pid"

  date '+board_time=%F %T %Z' > "${tmp_out}/stop_board_time.log"
  local disk_out
  disk_out="$(disk_out_for_run "${run_id}" || true)"
  copy_if_needed "${tmp_out}/stop_board_time.log" "${disk_out:+${disk_out}/stop_board_time.log}"

  echo "Stopped RUN_ID=${run_id}"
  echo "TMP_OUT=${tmp_out}"
  if [ -n "${disk_out}" ]; then
    echo "DISK_OUT=${disk_out}"
  fi

  rm -f "${CURRENT_RUN_FILE}"
  if [ "${SCRIPT_PATH}" = "${ROOT_DIR}/scripts/board_repro_log.sh" ]; then
    rm -f "${SCRIPT_PATH}"
    echo "已删除板端临时脚本: ${SCRIPT_PATH}"
  fi
}

main() {
  mkdir -p "${BASE_TMP}"

  case "${1:-}" in
    start)
      shift
      local auto_record=0 live_logs=1 disk_mirror=1 record_count=0 record_sec=20 idle_sec=10 start_timeout_sec=20 stop_timeout_sec=180 hws_interval_sec=3 control_command=SHORT_UP continue_on_error=0 error_cooldown_sec=60
      while [ "$#" -gt 0 ]; do
        case "$1" in
          --auto-record)
            auto_record=1
            shift
            ;;
          --no-live-logs)
            live_logs=0
            shift
            ;;
          --no-disk-mirror)
            disk_mirror=0
            shift
            ;;
          --record-count)
            require_option_value "$1" "${2:-}"
            record_count="$2"
            shift 2
            ;;
          --record-sec)
            require_option_value "$1" "${2:-}"
            record_sec="$2"
            shift 2
            ;;
          --idle-sec)
            require_option_value "$1" "${2:-}"
            idle_sec="$2"
            shift 2
            ;;
          --start-timeout-sec)
            require_option_value "$1" "${2:-}"
            start_timeout_sec="$2"
            shift 2
            ;;
          --stop-timeout-sec)
            require_option_value "$1" "${2:-}"
            stop_timeout_sec="$2"
            shift 2
            ;;
          --hws-interval-sec)
            require_option_value "$1" "${2:-}"
            hws_interval_sec="$2"
            shift 2
            ;;
          --control-command)
            require_option_value "$1" "${2:-}"
            control_command="$2"
            shift 2
            ;;
          --continue-on-error)
            continue_on_error=1
            shift
            ;;
          --error-cooldown-sec)
            require_option_value "$1" "${2:-}"
            error_cooldown_sec="$2"
            shift 2
            ;;
          *)
            echo "未知 start 参数: $1" >&2
            usage >&2
            exit 1
            ;;
        esac
      done
      start_capture "${live_logs}" "${disk_mirror}"
      if [ "${auto_record}" -eq 1 ]; then
        auto_record_loop "$(latest_run_id)" "${record_count}" "${record_sec}" "${idle_sec}" \
          "${start_timeout_sec}" "${stop_timeout_sec}" "${hws_interval_sec}" "${control_command}" \
          "${continue_on_error}" "${error_cooldown_sec}"
      fi
      ;;
    capture-now)
      capture_now "${2:-}"
      ;;
    stop)
      stop_capture "${2:-}"
      ;;
    -h|--help|"")
      usage
      ;;
    *)
      echo "未知命令: ${1}" >&2
      usage >&2
      exit 1
      ;;
  esac
}

main "$@"
