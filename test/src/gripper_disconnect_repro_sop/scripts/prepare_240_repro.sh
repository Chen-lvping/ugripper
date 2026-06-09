#!/usr/bin/env bash
set -euo pipefail

TARGET="ubuntu@192.168.2.240"
AUTO_RECORD=1
REMOTE_AUTO_ARGS=()
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
ROOT_NAME="$(basename "${ROOT_DIR}")"
BOARD_SCRIPT_LOCAL="${SCRIPT_DIR}/board_repro_log.sh"
REMOTE_ROOT="/tmp/${ROOT_NAME}"
BOARD_SCRIPT_REMOTE="${REMOTE_ROOT}/scripts/board_repro_log.sh"
LOCAL_STATE_DIR="${ROOT_DIR}/logs/.ssh"
KNOWN_HOSTS_FILE="${LOCAL_STATE_DIR}/known_hosts"
CONTROL_PATH="/tmp/${ROOT_NAME}_${USER:-user}_%C.sock"
SSH_BASE_OPTS=()

usage() {
  cat <<'EOF'
用法:
  ./scripts/prepare_240_repro.sh [TARGET] [auto options]
  ./scripts/prepare_240_repro.sh --prepare-only [TARGET]

默认板端: ubuntu@192.168.2.240
默认动作: 自动连续录制测试

自动测试参数:
  --record-count N           录制轮数，默认 0 表示一直跑。
  --record-sec N             每轮录制秒数。
  --idle-sec N               每轮间隔秒数。
  --start-timeout-sec N      等待起录超时秒数。
  --stop-timeout-sec N       等待停录收尾超时秒数。
  --hws-interval-sec N       hws 刷新间隔秒数。
  --control-command SHORT_UP|START_STOP
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
    sshpass -p "${BOARD_SSH_PASSWORD}" scp "${SSH_BASE_OPTS[@]}" "$@"
  else
    scp "${SSH_BASE_OPTS[@]}" "$@"
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
  if ! ssh_cmd -MNf "${TARGET}"; then
    echo "警告: SSH 复用连接启动失败，将继续使用普通 SSH 连接。" >&2
  fi
}

stop_ssh_master() {
  if [ "${#SSH_BASE_OPTS[@]}" -gt 0 ]; then
    ssh "${SSH_BASE_OPTS[@]}" -O exit "${TARGET}" >/dev/null 2>&1 || true
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

while [ "$#" -gt 0 ]; do
  case "$1" in
    --auto-record)
      AUTO_RECORD=1
      shift
      ;;
    --prepare-only)
      AUTO_RECORD=0
      shift
      ;;
    --record-count|--record-sec|--idle-sec|--start-timeout-sec|--stop-timeout-sec|--hws-interval-sec|--control-command)
      require_option_value "$1" "${2:-}"
      REMOTE_AUTO_ARGS+=("$1" "$2")
      shift 2
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

if [ ! -f "${BOARD_SCRIPT_LOCAL}" ]; then
  echo "找不到板端脚本: ${BOARD_SCRIPT_LOCAL}" >&2
  exit 1
fi

BOARD_SSH_PASSWORD="${BOARD_SSH_PASSWORD:-ubuntu}"
BOARD_SUDO_PASSWORD="${BOARD_SUDO_PASSWORD:-ubuntu}"

start_ssh_master
trap stop_ssh_master EXIT

HOST_EPOCH="$(date +%s)"
HOST_TIME_DISPLAY="$(date '+%F %T %Z')"
BOARD_TIME_BEFORE="$(ssh_cmd "${TARGET}" "date '+%F %T %Z'")"

printf '%s\n' "${BOARD_SUDO_PASSWORD}" | ssh_cmd "${TARGET}" \
  "sudo -S -p '' sh -c 'date -s @${HOST_EPOCH} >/dev/null; hwclock -w >/dev/null 2>&1 || true; date \"+%F %T %Z\"'"

BOARD_TIME_AFTER="$(ssh_cmd "${TARGET}" "date '+%F %T %Z'")"

ssh_cmd "${TARGET}" "mkdir -p '${REMOTE_ROOT}/scripts' '${REMOTE_ROOT}/logs'"
scp_cmd "${BOARD_SCRIPT_LOCAL}" "${TARGET}:${BOARD_SCRIPT_REMOTE}"
ssh_cmd "${TARGET}" "chmod +x '${BOARD_SCRIPT_REMOTE}'"

echo "主机时间      : ${HOST_TIME_DISPLAY}"
echo "板端同步前    : ${BOARD_TIME_BEFORE}"
echo "板端同步后    : ${BOARD_TIME_AFTER}"
echo "板端脚本      : ${BOARD_SCRIPT_REMOTE}"
echo
if [ "${AUTO_RECORD}" -eq 1 ]; then
  remote_command="BOARD_SUDO_PASSWORD=$(shell_quote "${BOARD_SUDO_PASSWORD}") bash $(shell_quote "${BOARD_SCRIPT_REMOTE}") start --auto-record --no-live-logs"
  for arg in "${REMOTE_AUTO_ARGS[@]}"; do
    remote_command+=" $(shell_quote "${arg}")"
  done
  stop_command="BOARD_SUDO_PASSWORD=$(shell_quote "${BOARD_SUDO_PASSWORD}") bash $(shell_quote "${BOARD_SCRIPT_REMOTE}") stop"
  cleanup_auto_record() {
    echo
    echo "正在停止板端采集..."
    ssh_cmd "${TARGET}" "${stop_command}" || true
  }
  trap cleanup_auto_record INT TERM

  echo "开始自动测试: ${TARGET}"
  echo "按 Ctrl-C 可停止前台测试，脚本会自动尝试停止板端采集。"
  echo
  set +e
  ssh_cmd -tt "${TARGET}" "${remote_command}"
  remote_status=$?
  set -e

  trap - INT TERM
  cleanup_auto_record
  exit "${remote_status}"
else
  echo "脚本已下发。手动模式下一步:"
  echo "  ssh ${TARGET}"
  echo "  bash ${BOARD_SCRIPT_REMOTE} start"
  echo "  bash ${BOARD_SCRIPT_REMOTE} capture-now"
  echo "  bash ${BOARD_SCRIPT_REMOTE} stop"
  echo
  echo "自动测试命令:"
  echo "  ./scripts/prepare_240_repro.sh"
fi

echo
echo "注意:"
echo "  板端重启后 /tmp 下的临时脚本和采集状态会丢失。"
echo "  下一轮测试前重新执行 ./scripts/prepare_240_repro.sh 即可。"
