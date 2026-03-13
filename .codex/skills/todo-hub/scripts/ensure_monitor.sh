#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(git rev-parse --show-toplevel)
OWNER="zhouwu"
TODO_PATH="$REPO_ROOT/docs/todo_list.md"
RUNTIME_FILE="$REPO_ROOT/.codex/todo-hub/runtime.json"
DASHBOARD_PATH="$REPO_ROOT/docs/todo_dashboard.md"
INTERVAL=10
UNIT_NAME=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --owner)
      OWNER="$2"
      shift 2
      ;;
    --todo-path)
      TODO_PATH="$2"
      shift 2
      ;;
    --runtime-file)
      RUNTIME_FILE="$2"
      shift 2
      ;;
    --dashboard-path)
      DASHBOARD_PATH="$2"
      shift 2
      ;;
    --interval)
      INTERVAL="$2"
      shift 2
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

UNIT_NAME=$(printf 'todo-hub-monitor-%s' "$OWNER" | tr '[:upper:]' '[:lower:]')

if command -v systemctl >/dev/null 2>&1 && systemctl --user is-active --quiet "$UNIT_NAME"; then
  printf 'unit=%s\nstatus=running\n' "$UNIT_NAME"
  exit 0
fi

if command -v systemctl >/dev/null 2>&1; then
  systemctl --user stop "$UNIT_NAME" >/dev/null 2>&1 || true
  systemctl --user reset-failed "$UNIT_NAME" >/dev/null 2>&1 || true
fi

if command -v systemd-run >/dev/null 2>&1; then
  systemd-run --user --unit "$UNIT_NAME" --same-dir /usr/bin/bash \
    "$SCRIPT_DIR/monitor_tasks.sh" \
    --owner "$OWNER" \
    --todo-path "$TODO_PATH" \
    --runtime-file "$RUNTIME_FILE" \
    --dashboard-path "$DASHBOARD_PATH" \
    --interval "$INTERVAL" >/dev/null
  printf 'unit=%s\nstatus=started\n' "$UNIT_NAME"
else
  nohup /usr/bin/bash "$SCRIPT_DIR/monitor_tasks.sh" \
    --owner "$OWNER" \
    --todo-path "$TODO_PATH" \
    --runtime-file "$RUNTIME_FILE" \
    --dashboard-path "$DASHBOARD_PATH" \
    --interval "$INTERVAL" >/dev/null 2>&1 &
  printf 'pid=%s\nstatus=started\n' "$!"
fi
