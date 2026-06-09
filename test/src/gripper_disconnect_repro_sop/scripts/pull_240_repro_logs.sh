#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-ubuntu@192.168.2.240}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
ROOT_NAME="$(basename "${ROOT_DIR}")"
DEST_ROOT="${2:-${ROOT_DIR}/logs}"

ssh_cmd() {
  if [ -n "${BOARD_SSH_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    sshpass -p "${BOARD_SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$@"
  else
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$@"
  fi
}

scp_cmd() {
  if [ -n "${BOARD_SSH_PASSWORD:-}" ] && command -v sshpass >/dev/null 2>&1; then
    sshpass -p "${BOARD_SSH_PASSWORD}" scp -r -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$@"
  else
    scp -r -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$@"
  fi
}

RUN_ID="$(ssh_cmd "${TARGET}" "ls -1dt /tmp/${ROOT_NAME}/logs/* 2>/dev/null | head -n1 | xargs -r basename")"
if [ -z "${RUN_ID}" ]; then
  echo "No board log directory found on ${TARGET}" >&2
  exit 1
fi

mkdir -p "${DEST_ROOT}"
scp_cmd "${TARGET}:/tmp/${ROOT_NAME}/logs/${RUN_ID}" "${DEST_ROOT}/"

echo "Pulled logs to ${DEST_ROOT}/${RUN_ID}"
