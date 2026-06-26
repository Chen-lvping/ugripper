#!/usr/bin/env bash
set -euo pipefail

TARGET="ubuntu@192.168.2.240"
DEST_ROOT="./tmp/ugripper_240_soak"
DURATION_HOURS=10
PULL_INTERVAL_SEC=3600
RECORD_SEC=60
IDLE_SEC=5
START_TIMEOUT_SEC=30
STOP_TIMEOUT_SEC=240
HWS_INTERVAL_SEC=5
PULL_DATA=0
CLEAN_BOARD_DATA=0

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
ROOT_NAME="$(basename "${ROOT_DIR}")"
BOARD_SCRIPT_LOCAL="${SCRIPT_DIR}/board_repro_log.sh"
REMOTE_ROOT="/tmp/${ROOT_NAME}"
BOARD_SCRIPT_REMOTE="${REMOTE_ROOT}/scripts/board_repro_log.sh"
MAINTENANCE_HOLD_REMOTE="${REMOTE_ROOT}/maintenance_hold"
LOCAL_STATE_DIR="${ROOT_DIR}/logs/.ssh"
KNOWN_HOSTS_FILE="${LOCAL_STATE_DIR}/known_hosts"
CONTROL_PATH="/tmp/${ROOT_NAME}_${USER:-user}_soak_%C.sock"
SSH_BASE_OPTS=()
RUN_ID=""

usage() {
  cat <<'EOF'
用法:
  ./scripts/run_240_soak_safe.sh [TARGET] [options]

默认目标: ubuntu@192.168.2.240

参数:
  --duration-hours N       长测小时数，默认 10。
  --pull-interval-sec N    主机侧拉取间隔，默认 3600。
  --record-sec N           每轮录制秒数，默认 60。
  --idle-sec N             每轮间隔秒数，默认 5。
  --dest DIR               本机拉取目录，默认 ./tmp/ugripper_240_soak。
  --pull-data              同步 episode 数据到本机。
  --clean-board-data       同步成功后清理板端 episode 数据。

默认只定期拉取 /tmp 测试日志，不碰 /mnt/data_disk episode 数据。
即使启用 --pull-data/--clean-board-data，也会先让板端自动测试在两轮之间 hold，
并确认没有录制、没有 restore USB、数据盘稳定后才执行。
EOF
}

ssh_cmd() {
  if [ -n "${BOARD_SSH_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    sshpass -p "${BOARD_SSH_PASSWORD}" ssh "${SSH_BASE_OPTS[@]}" "$@"
  else
    ssh "${SSH_BASE_OPTS[@]}" "$@"
  fi
}

scp_cmd() {
  if [ -n "${BOARD_SSH_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    sshpass -p "${BOARD_SSH_PASSWORD}" scp -r "${SSH_BASE_OPTS[@]}" "$@"
  else
    scp -r "${SSH_BASE_OPTS[@]}" "$@"
  fi
}

rsync_cmd() {
  if [ -n "${BOARD_SSH_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    SSHPASS="${BOARD_SSH_PASSWORD}" rsync -e "sshpass -e ssh $(printf '%q ' "${SSH_BASE_OPTS[@]}")" "$@"
  else
    rsync -e "ssh $(printf '%q ' "${SSH_BASE_OPTS[@]}")" "$@"
  fi
}

shell_quote() {
  local value="$1"
  printf "'%s'" "$(printf '%s' "${value}" | sed "s/'/'\\\\''/g")"
}

require_option_value() {
  local option="$1"
  local value="${2:-}"
  if [ -z "${value}" ]; then
    echo "缺少参数值: ${option}" >&2
    exit 1
  fi
}

start_ssh_master() {
  mkdir -p "${LOCAL_STATE_DIR}"
  chmod 700 "${LOCAL_STATE_DIR}" 2>/dev/null || true
  SSH_BASE_OPTS=(
    -o StrictHostKeyChecking=accept-new
    -o UserKnownHostsFile="${KNOWN_HOSTS_FILE}"
    -o ControlMaster=auto
    -o ControlPersist=10m
    -o ControlPath="${CONTROL_PATH}"
  )
  ssh_cmd -MNf "${TARGET}" || true
}

stop_ssh_master() {
  if [ "${#SSH_BASE_OPTS[@]}" -gt 0 ]; then
    ssh "${SSH_BASE_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
  fi
}

sync_board_time() {
  local host_epoch
  host_epoch="$(date +%s)"
  printf '%s\n' "${BOARD_SUDO_PASSWORD}" | ssh_cmd "${TARGET}" \
    "sudo -S -p '' sh -c 'date -s @${host_epoch} >/dev/null; hwclock -w >/dev/null 2>&1 || true; date \"+%F %T %Z\"'"
}

deploy_board_script() {
  ssh_cmd "${TARGET}" "mkdir -p '${REMOTE_ROOT}/scripts' '${REMOTE_ROOT}/logs'"
  scp_cmd "${BOARD_SCRIPT_LOCAL}" "${TARGET}:${BOARD_SCRIPT_REMOTE}"
  ssh_cmd "${TARGET}" "chmod +x '${BOARD_SCRIPT_REMOTE}'"
}

calc_record_count() {
  local total_sec per_cycle
  total_sec=$((DURATION_HOURS * 3600))
  per_cycle=$((RECORD_SEC + IDLE_SEC))
  echo $(((total_sec + per_cycle - 1) / per_cycle))
}

start_remote_test() {
  local record_count remote_command
  record_count="$(calc_record_count)"
  remote_command="BOARD_SUDO_PASSWORD=$(shell_quote "${BOARD_SUDO_PASSWORD}") nohup bash $(shell_quote "${BOARD_SCRIPT_REMOTE}") start --auto-record --no-live-logs --no-disk-mirror --continue-on-error --record-count ${record_count} --record-sec ${RECORD_SEC} --idle-sec ${IDLE_SEC} --start-timeout-sec ${START_TIMEOUT_SEC} --stop-timeout-sec ${STOP_TIMEOUT_SEC} --hws-interval-sec ${HWS_INTERVAL_SEC} > '${REMOTE_ROOT}/soak_runner.out' 2>&1 &"
  ssh_cmd "${TARGET}" "${remote_command}"
  sleep 2
  RUN_ID="$(ssh_cmd "${TARGET}" "cat '${REMOTE_ROOT}/logs/.current_run_id' 2>/dev/null || true")"
  if [ -z "${RUN_ID}" ]; then
    echo "未能获取 RUN_ID。" >&2
    ssh_cmd "${TARGET}" "sed -n '1,120p' '${REMOTE_ROOT}/soak_runner.out' 2>/dev/null || true" >&2
    exit 1
  fi
  echo "RUN_ID=${RUN_ID}"
  echo "record_count=${record_count}"
}

remote_test_alive() {
  ssh_cmd "${TARGET}" "pgrep -f 'board_repro_log.sh start --auto-record' >/dev/null 2>&1"
}

remote_test_complete() {
  ssh_cmd "${TARGET}" "[ ! -f '${REMOTE_ROOT}/logs/.current_run_id' ] || ! pgrep -f 'board_repro_log.sh start --auto-record' >/dev/null 2>&1"
}

set_maintenance_hold() {
  ssh_cmd "${TARGET}" "date +%s > '${MAINTENANCE_HOLD_REMOTE}'"
}

clear_maintenance_hold() {
  ssh_cmd "${TARGET}" "rm -f '${MAINTENANCE_HOLD_REMOTE}'" || true
}

stop_remote_capture() {
  ssh_cmd "${TARGET}" "BOARD_SUDO_PASSWORD=$(shell_quote "${BOARD_SUDO_PASSWORD}") bash $(shell_quote "${BOARD_SCRIPT_REMOTE}") stop ${RUN_ID}" || true
}

wait_remote_safe_for_data_ops() {
  local timeout_sec="${1:-900}"
  local deadline
  deadline=$((SECONDS + timeout_sec))
  while [ "${SECONDS}" -lt "${deadline}" ]; do
    if ssh_cmd "${TARGET}" '
      [ ! -f /tmp/umi_recording.lock ] &&
      [ -p /tmp/umi_record_control.pipe ] &&
      [ "$(systemctl is-active ugripper.service 2>/dev/null || true)" = active ] &&
      findmnt -rn --mountpoint /mnt/data_disk >/dev/null 2>&1 &&
      findmnt -rn --mountpoint /mnt/data_disk -o OPTIONS 2>/dev/null | grep -qw rw &&
      [ -w /mnt/data_disk ] &&
      ! pgrep -f "ugripper_restore_usb|auto_restore_usb" >/dev/null 2>&1 &&
      [ ! -e /run/ugripper/system_action_request ] &&
      [ ! -e /tmp/umi_system_action_request ]
    ' >/dev/null 2>&1; then
      sleep 10
      if ssh_cmd "${TARGET}" '[ ! -f /tmp/umi_recording.lock ] && findmnt -rn --mountpoint /mnt/data_disk >/dev/null 2>&1 && ! pgrep -f "ugripper_restore_usb|auto_restore_usb" >/dev/null 2>&1' >/dev/null 2>&1; then
        return 0
      fi
    fi
    sleep 5
  done
  return 1
}

pull_logs() {
  local dest="${DEST_ROOT}/${RUN_ID}"
  mkdir -p "${dest}"
  scp_cmd "${TARGET}:${REMOTE_ROOT}/logs/${RUN_ID}" "${DEST_ROOT}/" || true
  ssh_cmd "${TARGET}" "cp '${REMOTE_ROOT}/soak_runner.out' '${REMOTE_ROOT}/logs/${RUN_ID}/soak_runner.out' 2>/dev/null || true" >/dev/null 2>&1 || true
  echo "logs_pulled=${dest}"
}

episode_data_root() {
  ssh_cmd "${TARGET}" "find /mnt/data_disk -mindepth 2 -maxdepth 2 -type d -path '*/data' 2>/dev/null | head -n1"
}

pull_and_optionally_clean_data() {
  local data_root dest
  data_root="$(episode_data_root || true)"
  if [ -z "${data_root}" ]; then
    echo "WARN: episode data root not found; skip data pull" >&2
    return 0
  fi
  dest="${DEST_ROOT}/${RUN_ID}/data"
  mkdir -p "${dest}"
  rsync_cmd -a --partial --info=stats1,progress2 "${TARGET}:${data_root}/" "${dest}/"
  if [ "${CLEAN_BOARD_DATA}" -eq 1 ]; then
    ssh_cmd "${TARGET}" "find '${data_root}' -mindepth 1 -maxdepth 1 -type d -name 'episode_*' ! -name '*-temp' -exec rm -rf {} +"
  fi
}

maintenance_pull_cycle() {
  echo "===== maintenance pull $(date '+%F %T %Z') ====="
  set_maintenance_hold
  if wait_remote_safe_for_data_ops 900; then
    pull_logs
    if [ "${PULL_DATA}" -eq 1 ]; then
      pull_and_optionally_clean_data
    fi
  else
    echo "WARN: remote did not become safe for data ops; skip this pull window" >&2
    pull_logs
  fi
  clear_maintenance_hold
}

cleanup() {
  clear_maintenance_hold
  stop_ssh_master
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --duration-hours)
      require_option_value "$1" "${2:-}"
      DURATION_HOURS="$2"
      shift 2
      ;;
    --pull-interval-sec)
      require_option_value "$1" "${2:-}"
      PULL_INTERVAL_SEC="$2"
      shift 2
      ;;
    --record-sec)
      require_option_value "$1" "${2:-}"
      RECORD_SEC="$2"
      shift 2
      ;;
    --idle-sec)
      require_option_value "$1" "${2:-}"
      IDLE_SEC="$2"
      shift 2
      ;;
    --dest)
      require_option_value "$1" "${2:-}"
      DEST_ROOT="$2"
      shift 2
      ;;
    --pull-data)
      PULL_DATA=1
      shift
      ;;
    --clean-board-data)
      CLEAN_BOARD_DATA=1
      PULL_DATA=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "未知参数: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      TARGET="$1"
      shift
      ;;
  esac
done

BOARD_SSH_PASSWORD="${BOARD_SSH_PASSWORD:-ubuntu}"
BOARD_SUDO_PASSWORD="${BOARD_SUDO_PASSWORD:-ubuntu}"

start_ssh_master
trap cleanup EXIT INT TERM

echo "sync_board_time=$(sync_board_time)"
deploy_board_script
mkdir -p "${DEST_ROOT}"
start_remote_test

next_pull=$((SECONDS + PULL_INTERVAL_SEC))
while true; do
  if remote_test_complete; then
    echo "remote_test_complete=$(date '+%F %T %Z')"
    maintenance_pull_cycle
    stop_remote_capture
    pull_logs
    break
  fi
  if [ "${SECONDS}" -ge "${next_pull}" ]; then
    maintenance_pull_cycle
    next_pull=$((SECONDS + PULL_INTERVAL_SEC))
  fi
  if ! remote_test_alive; then
    echo "WARN: remote auto-record process not found; pulling logs and exiting monitor" >&2
    pull_logs
    exit 1
  fi
  sleep 30
done

echo "soak_done=${DEST_ROOT}/${RUN_ID}"
