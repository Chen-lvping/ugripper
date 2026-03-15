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
OWNER="zhouwu"
TASK_ID=""
TODO_PATH="$REPO_ROOT/docs/todo_list.md"
RUNTIME_FILE="$REPO_ROOT/.codex/todo-hub/runtime.json"
DASHBOARD_PATH="$REPO_ROOT/docs/todo_dashboard.md"
DRY_RUN=0
NEW_AGENT=0
CODEX_BIN=$(command -v codex || true)
PYTHON_BIN=$(command -v python3 || true)

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
    --new-agent)
      NEW_AGENT=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$TASK_ID" ]]; then
  echo "--task-id is required" >&2
  exit 1
fi

if [[ -z "$CODEX_BIN" ]]; then
  echo "codex binary not found in PATH" >&2
  exit 1
fi

if [[ -z "$PYTHON_BIN" ]]; then
  echo "python3 not found in PATH" >&2
  exit 1
fi

WORKTREE_DIR=$(bash "$SCRIPT_DIR/allocate_worktree.sh" --task-id "$TASK_ID" --owner "$OWNER" --todo-path "$TODO_PATH" --runtime-file "$RUNTIME_FILE" | tail -n 1)
BRANCH=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --owner "$OWNER" --field branch)
TITLE=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --field title)
ACCEPTANCE=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --field acceptance)
NOTES=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --field notes)
EXISTING_SESSION_ID=$(python3 "$SCRIPT_DIR/runtime_ctl.py" show --runtime-file "$RUNTIME_FILE" --task-id "$TASK_ID" | sed -n 's/.*"session_id": "\([^"]*\)".*/\1/p')

TASK_DIR="$REPO_ROOT/.codex/todo-hub/tasks/$TASK_ID"
mkdir -p "$TASK_DIR"
INITIAL_PROMPT_FILE="$TASK_DIR/prompt.txt"
RESUME_PROMPT_FILE="$TASK_DIR/resume_prompt.txt"
LAST_MESSAGE_FILE="$TASK_DIR/last_message.txt"
LOG_FILE="$TASK_DIR/codex.jsonl"
RUNNER_FILE="$TASK_DIR/run_session.sh"
PROGRESS_FILE="$TASK_DIR/progress.txt"
REPORT_PROGRESS_SCRIPT="$REPO_ROOT/.codex/skills/todo-hub/scripts/report_progress.sh"
UNIT_NAME=$(printf 'todo-hub-%s-%s' "$OWNER" "$TASK_ID" | tr '[:upper:]' '[:lower:]')

cat >"$INITIAL_PROMPT_FILE" <<EOF2
你是主控 agent 拉起的子 agent，会在当前 git worktree 中只处理一个任务。

任务信息：
- task_id: $TASK_ID
- title: $TITLE
- branch: $BRANCH
- worktree: $WORKTREE_DIR

执行要求：
1. 在当前 worktree 内工作，不要切到其他目录或其他分支。
2. 真实代码修改前，优先阅读 docs/agent/overview.md，并在其中按需查找系统链路、部署和排障章节。
3. 当前轮次默认处于“规划阶段”，你只需要输出：
   - 实现计划
   - 影响文件列表
   - 文档同步计划
   - 关键风险/待确认点
4. 在用户通过主控 agent 明确确认前，不得修改任何代码、脚本、配置或文档，也不要执行构建、安装、升级、重启、录制等有副作用动作。
5. 完成规划后，主动执行一次进度上报脚本刷新 dashboard，然后结束本轮会话等待主控 agent 续接。
6. 若遇阻塞，在最终回复里明确写出 blocker 和下一步建议。
7. 示例：bash "$REPORT_PROGRESS_SCRIPT" --hub-root "$REPO_ROOT" --task-id "$TASK_ID" --owner "$OWNER" --status running --message "已完成任务规划，等待主控汇总和用户确认。"

验收条件：
$ACCEPTANCE

补充说明：
$NOTES
EOF2

cat >"$RESUME_PROMPT_FILE" <<EOF2
继续这个 TASK 的既有会话，不要重置上下文，不要把之前的分析、计划和已完成工作当作丢失。

任务信息（若与旧上下文有差异，以这里为准）：
- task_id: $TASK_ID
- title: $TITLE
- branch: $BRANCH
- worktree: $WORKTREE_DIR

最新验收条件：
$ACCEPTANCE

最新补充说明：
$NOTES

续接要求：
1. 先吸收最新任务说明，再基于既有上下文继续推进。
2. 若任务说明有新增或纠正，明确指出你已更新判断，而不是从零重做。
3. 优先延续已有结论、未完成项和验证计划。
4. 若最新消息没有明确的执行授权语义（如“开始执行”“方案已确认，开始实现”），则继续停留在规划阶段，只更新计划，不做代码修改。
5. 只有在最新消息明确授权执行时，才进入实现阶段。
6. 最终回复仍需聚焦这个任务，不要扩散到其他任务。
EOF2

RESUME_MODE=0
PROMPT_FILE="$INITIAL_PROMPT_FILE"
SESSION_ID_FOR_RUN=""
if [[ "$NEW_AGENT" != "1" && -n "$EXISTING_SESSION_ID" ]]; then
  RESUME_MODE=1
  PROMPT_FILE="$RESUME_PROMPT_FILE"
  SESSION_ID_FOR_RUN="$EXISTING_SESSION_ID"
fi

cat >"$RUNNER_FILE" <<EOF2
#!/usr/bin/env bash
set -euo pipefail
TASK_EXIT=0
SESSION_ID_VALUE="$SESSION_ID_FOR_RUN"
RESUME_MODE_VALUE="$RESUME_MODE"
update_runtime_and_dashboard() {
  local session_id="\$SESSION_ID_VALUE"
  local runtime_status="stopped"
  if [[ -z "\$session_id" && -f "$LOG_FILE" ]]; then
    session_id=\$(sed -n 's/.*"thread_id":"\([^"]*\)".*/\1/p' "$LOG_FILE" | tail -n 1)
  fi
  if [[ "\$TASK_EXIT" -ne 0 ]]; then
    runtime_status="failed"
  fi
  "$PYTHON_BIN" "$SCRIPT_DIR/runtime_ctl.py" upsert \
    --runtime-file "$RUNTIME_FILE" \
    --task-id "$TASK_ID" \
    --owner "$OWNER" \
    --title "$TITLE" \
    --branch "$BRANCH" \
    --worktree "$WORKTREE_DIR" \
    --unit_name "$UNIT_NAME" \
    --session_id "\$session_id" \
    --pid "" \
    --status "\$runtime_status" \
    --last_message_file "$LAST_MESSAGE_FILE" \
    --log_file "$LOG_FILE" \
    --progress_file "$PROGRESS_FILE" >/dev/null || true
  "$PYTHON_BIN" "$SCRIPT_DIR/sync_dashboard.py" \
    --todo-path "$TODO_PATH" \
    --runtime-file "$RUNTIME_FILE" \
    --dashboard-path "$DASHBOARD_PATH" \
    --owner "$OWNER" >/dev/null || true
}
trap update_runtime_and_dashboard EXIT
cd "$WORKTREE_DIR"
set +e
if [[ "\$RESUME_MODE_VALUE" == "1" ]]; then
  "$CODEX_BIN" exec resume --dangerously-bypass-approvals-and-sandbox --json -o "$LAST_MESSAGE_FILE" "\$SESSION_ID_VALUE" - <"$PROMPT_FILE" >>"$LOG_FILE" 2>&1
else
  "$CODEX_BIN" exec --dangerously-bypass-approvals-and-sandbox --json -o "$LAST_MESSAGE_FILE" - <"$PROMPT_FILE" >>"$LOG_FILE" 2>&1
fi
TASK_EXIT=\$?
set -e
exit "\$TASK_EXIT"
EOF2
chmod +x "$RUNNER_FILE"

if [[ "$DRY_RUN" == "1" ]]; then
  python3 "$SCRIPT_DIR/runtime_ctl.py" upsert \
    --runtime-file "$RUNTIME_FILE" \
    --task-id "$TASK_ID" \
    --owner "$OWNER" \
    --title "$TITLE" \
    --branch "$BRANCH" \
    --worktree "$WORKTREE_DIR" \
    --unit_name "$UNIT_NAME" \
    --session_id "$SESSION_ID_FOR_RUN" \
    --status "dry_run" \
    --last_message_file "$LAST_MESSAGE_FILE" \
    --log_file "$LOG_FILE" \
    --progress_file "$PROGRESS_FILE" >/dev/null
  python3 "$SCRIPT_DIR/sync_dashboard.py" \
    --todo-path "$TODO_PATH" \
    --runtime-file "$RUNTIME_FILE" \
    --dashboard-path "$DASHBOARD_PATH" \
    --owner "$OWNER" >/dev/null
  echo "$PROMPT_FILE"
  exit 0
fi

if command -v systemctl >/dev/null 2>&1 && systemctl --user is-active --quiet "$UNIT_NAME"; then
  PID=$(systemctl --user show "$UNIT_NAME" --property MainPID --value 2>/dev/null || true)
  SESSION_ID="${SESSION_ID_FOR_RUN:-$(sed -n 's/.*"thread_id":"\([^"]*\)".*/\1/p' "$LOG_FILE" | tail -n 1)}"
  python3 "$SCRIPT_DIR/runtime_ctl.py" upsert \
    --runtime-file "$RUNTIME_FILE" \
    --task-id "$TASK_ID" \
    --owner "$OWNER" \
    --title "$TITLE" \
    --branch "$BRANCH" \
    --worktree "$WORKTREE_DIR" \
    --unit_name "$UNIT_NAME" \
    --session_id "$SESSION_ID" \
    --pid "$PID" \
    --status "running" \
    --last_message_file "$LAST_MESSAGE_FILE" \
    --log_file "$LOG_FILE" \
    --progress_file "$PROGRESS_FILE" >/dev/null
  python3 "$SCRIPT_DIR/sync_dashboard.py" \
    --todo-path "$TODO_PATH" \
    --runtime-file "$RUNTIME_FILE" \
    --dashboard-path "$DASHBOARD_PATH" \
    --owner "$OWNER" >/dev/null
  printf 'task=%s\nsession=%s\npid=%s\nunit=%s\nworktree=%s\n' "$TASK_ID" "$SESSION_ID" "$PID" "$UNIT_NAME" "$WORKTREE_DIR"
  exit 0
fi

if command -v systemctl >/dev/null 2>&1; then
  systemctl --user stop "$UNIT_NAME" >/dev/null 2>&1 || true
  systemctl --user reset-failed "$UNIT_NAME" >/dev/null 2>&1 || true
fi

if [[ "$RESUME_MODE" != "1" ]]; then
  : >"$LOG_FILE"
  rm -f "$LAST_MESSAGE_FILE"
else
  touch "$LOG_FILE"
fi

if command -v systemd-run >/dev/null 2>&1; then
  systemd-run --user --unit "$UNIT_NAME" --collect --same-dir /usr/bin/bash "$RUNNER_FILE" >/dev/null
  PID=$(systemctl --user show "$UNIT_NAME" --property MainPID --value 2>/dev/null || true)
else
  nohup /usr/bin/bash "$RUNNER_FILE" >/dev/null 2>&1 &
  PID=$!
fi

SESSION_ID="$SESSION_ID_FOR_RUN"
RUNTIME_STATUS="running"
for _ in $(seq 1 40); do
  sleep 1
  if [[ -z "$SESSION_ID" && -f "$LOG_FILE" ]]; then
    SESSION_ID=$(sed -n 's/.*"thread_id":"\([^"]*\)".*/\1/p' "$LOG_FILE" | tail -n 1)
    if [[ -n "$SESSION_ID" ]]; then
      break
    fi
  fi
  if command -v systemctl >/dev/null 2>&1; then
    ACTIVE_STATE=$(systemctl --user show "$UNIT_NAME" --property ActiveState --value 2>/dev/null || true)
    if [[ "$ACTIVE_STATE" == "failed" ]]; then
      RUNTIME_STATUS="failed"
      break
    fi
    if [[ "$ACTIVE_STATE" == "inactive" ]]; then
      RUNTIME_STATUS="stopped"
      break
    fi
  elif [[ -n "$PID" && ! -d "/proc/$PID" ]]; then
    RUNTIME_STATUS="stopped"
    break
  fi
  if [[ -n "$SESSION_ID" ]]; then
    break
  fi
done

python3 "$SCRIPT_DIR/runtime_ctl.py" upsert \
  --runtime-file "$RUNTIME_FILE" \
  --task-id "$TASK_ID" \
  --owner "$OWNER" \
  --title "$TITLE" \
  --branch "$BRANCH" \
  --worktree "$WORKTREE_DIR" \
  --unit_name "$UNIT_NAME" \
  --session_id "$SESSION_ID" \
  --pid "$PID" \
  --status "$RUNTIME_STATUS" \
  --last_message_file "$LAST_MESSAGE_FILE" \
  --log_file "$LOG_FILE" \
  --progress_file "$PROGRESS_FILE" >/dev/null

python3 "$SCRIPT_DIR/sync_dashboard.py" \
  --todo-path "$TODO_PATH" \
  --runtime-file "$RUNTIME_FILE" \
  --dashboard-path "$DASHBOARD_PATH" \
  --owner "$OWNER" >/dev/null

printf 'task=%s\nsession=%s\npid=%s\nunit=%s\nworktree=%s\n' "$TASK_ID" "$SESSION_ID" "$PID" "$UNIT_NAME" "$WORKTREE_DIR"
