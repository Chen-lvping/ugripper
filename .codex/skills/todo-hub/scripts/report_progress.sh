#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT_DEFAULT=$(cd "$SCRIPT_DIR/../../../.." && pwd)
GIT_TOPLEVEL=$(git rev-parse --show-toplevel 2>/dev/null || true)
TODO_HUB_ROOT_FILE=""
if [[ -n "${TODO_HUB_ROOT:-}" ]]; then
  REPO_ROOT="$TODO_HUB_ROOT"
elif [[ -n "$GIT_TOPLEVEL" && -f "$GIT_TOPLEVEL/.codex/todo-hub/root_path.txt" ]]; then
  TODO_HUB_ROOT_FILE="$GIT_TOPLEVEL/.codex/todo-hub/root_path.txt"
  REPO_ROOT=$(<"$TODO_HUB_ROOT_FILE")
else
  REPO_ROOT="$REPO_ROOT_DEFAULT"
fi
TASK_ID=""
OWNER="zhouwu"
STATUS="running"
MESSAGE=""
TODO_PATH="$REPO_ROOT/docs/todo_list.md"
RUNTIME_FILE="$REPO_ROOT/.codex/todo-hub/runtime.json"
DASHBOARD_PATH="$REPO_ROOT/docs/todo_dashboard.md"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --task-id)
      TASK_ID="$2"
      shift 2
      ;;
    --owner)
      OWNER="$2"
      shift 2
      ;;
    --status)
      STATUS="$2"
      shift 2
      ;;
    --message)
      MESSAGE="$2"
      shift 2
      ;;
    --hub-root)
      REPO_ROOT="$2"
      TODO_PATH="$REPO_ROOT/docs/todo_list.md"
      RUNTIME_FILE="$REPO_ROOT/.codex/todo-hub/runtime.json"
      DASHBOARD_PATH="$REPO_ROOT/docs/todo_dashboard.md"
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
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$TASK_ID" || -z "$MESSAGE" ]]; then
  echo "--task-id and --message are required" >&2
  exit 1
fi

TASK_DIR="$REPO_ROOT/.codex/todo-hub/tasks/$TASK_ID"
mkdir -p "$TASK_DIR"
PROGRESS_FILE="$TASK_DIR/progress.txt"
printf '%s\n' "$MESSAGE" > "$PROGRESS_FILE"

python3 "$SCRIPT_DIR/runtime_ctl.py" upsert \
  --runtime-file "$RUNTIME_FILE" \
  --task-id "$TASK_ID" \
  --owner "$OWNER" \
  --status "$STATUS" \
  --progress_file "$PROGRESS_FILE" >/dev/null

python3 "$SCRIPT_DIR/sync_dashboard.py" \
  --todo-path "$TODO_PATH" \
  --runtime-file "$RUNTIME_FILE" \
  --dashboard-path "$DASHBOARD_PATH" \
  --owner "$OWNER" >/dev/null

echo "$PROGRESS_FILE"
