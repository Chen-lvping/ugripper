#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(git rev-parse --show-toplevel)
TASK_ID=""
MESSAGE=""
OWNER="zhouwu"
TODO_PATH="$REPO_ROOT/docs/todo_list.md"
RUNTIME_FILE="$REPO_ROOT/.codex/todo-hub/runtime.json"
CODEX_BIN=$(command -v codex || true)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --task-id)
      TASK_ID="$2"
      shift 2
      ;;
    --message)
      MESSAGE="$2"
      shift 2
      ;;
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

if [[ -z "$CODEX_BIN" ]]; then
  echo "codex binary not found in PATH" >&2
  exit 1
fi

SESSION_ID=$(python3 "$SCRIPT_DIR/runtime_ctl.py" show --runtime-file "$RUNTIME_FILE" --task-id "$TASK_ID" | sed -n 's/.*"session_id": "\([^"]*\)".*/\1/p')
WORKTREE_DIR=$(python3 "$SCRIPT_DIR/runtime_ctl.py" show --runtime-file "$RUNTIME_FILE" --task-id "$TASK_ID" | sed -n 's/.*"worktree": "\([^"]*\)".*/\1/p')
LAST_MESSAGE_FILE="$REPO_ROOT/.codex/todo-hub/tasks/$TASK_ID/last_message.txt"
LOG_FILE="$REPO_ROOT/.codex/todo-hub/tasks/$TASK_ID/codex.jsonl"

if [[ -z "$SESSION_ID" ]]; then
  echo "task $TASK_ID has no session_id; launch it first" >&2
  exit 1
fi

cd "$WORKTREE_DIR"
"$CODEX_BIN" exec resume "$SESSION_ID" --dangerously-bypass-approvals-and-sandbox --json -o "$LAST_MESSAGE_FILE" "$MESSAGE" >>"$LOG_FILE" 2>&1

python3 "$SCRIPT_DIR/runtime_ctl.py" upsert \
  --runtime-file "$RUNTIME_FILE" \
  --task-id "$TASK_ID" \
  --owner "$OWNER" \
  --status "running" \
  --last_command "$MESSAGE" \
  --last_message_file "$LAST_MESSAGE_FILE" \
  --log_file "$LOG_FILE" >/dev/null

python3 "$SCRIPT_DIR/sync_dashboard.py" \
  --todo-path "$TODO_PATH" \
  --runtime-file "$RUNTIME_FILE" \
  --owner "$OWNER" >/dev/null

echo "$TASK_ID"
