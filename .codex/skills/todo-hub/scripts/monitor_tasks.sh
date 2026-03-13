#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OWNER="zhouwu"
TODO_PATH="docs/todo_list.md"
RUNTIME_FILE=".codex/todo-hub/runtime.json"
DASHBOARD_PATH="docs/todo_dashboard.md"
INTERVAL=10
PYTHON_BIN=$(command -v python3 || true)

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

if [[ -z "$PYTHON_BIN" ]]; then
  echo "python3 not found in PATH" >&2
  exit 1
fi

while true; do
  "$PYTHON_BIN" "$SCRIPT_DIR/sync_dashboard.py" \
    --todo-path "$TODO_PATH" \
    --runtime-file "$RUNTIME_FILE" \
    --dashboard-path "$DASHBOARD_PATH" \
    --owner "$OWNER" >/dev/null || true
  sleep "$INTERVAL"
done
