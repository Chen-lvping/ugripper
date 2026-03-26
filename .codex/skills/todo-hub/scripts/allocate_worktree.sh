#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(git rev-parse --show-toplevel)
OWNER="zhouwu"
TASK_ID=""
TODO_PATH="$REPO_ROOT/docs/todo_list.md"
RUNTIME_FILE="$REPO_ROOT/.codex/todo-hub/runtime.json"
BASE_REF="$(git -C "$REPO_ROOT" branch --show-current)"

copy_untracked_snapshot() {
  local source_root="$1"
  local target_root="$2"
  local untracked_raw
  local untracked_filtered

  untracked_raw=$(mktemp)
  untracked_filtered=$(mktemp)
  git -C "$source_root" ls-files --others --exclude-standard -z >"$untracked_raw"
  python3 - "$untracked_raw" "$untracked_filtered" <<'PY'
from pathlib import Path
import sys

raw_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
raw = raw_path.read_bytes().split(b"\0")
excluded_prefixes = (
    b".worktrees/",
    b".codex/todo-hub/tasks/",
)
excluded_exact = {
    b".codex/todo-hub/runtime.json",
}
filtered: list[bytes] = []
for item in raw:
    if not item:
        continue
    if item in excluded_exact:
        continue
    if any(item.startswith(prefix) for prefix in excluded_prefixes):
        continue
    filtered.append(item)
out_path.write_bytes(b"\0".join(filtered) + (b"\0" if filtered else b""))
PY

  if [[ -s "$untracked_filtered" ]]; then
    tar -C "$source_root" --null -T "$untracked_filtered" -cf - | tar -C "$target_root" -xf -
  fi

  rm -f "$untracked_raw" "$untracked_filtered"
}

apply_dirty_snapshot() {
  local source_root="$1"
  local target_root="$2"
  local staged_patch
  local unstaged_patch

  staged_patch=$(mktemp)
  unstaged_patch=$(mktemp)
  local diff_args=(
    --
    .
    ':(exclude).worktrees/**'
    ':(exclude).codex/todo-hub/runtime.json'
    ':(exclude).codex/todo-hub/root_path.txt'
    ':(exclude).codex/todo-hub/tasks/**'
  )

  git -C "$source_root" diff --binary --cached HEAD "${diff_args[@]}" >"$staged_patch"
  git -C "$source_root" diff --binary "${diff_args[@]}" >"$unstaged_patch"

  if [[ -s "$staged_patch" ]]; then
    git -C "$target_root" apply --binary --index "$staged_patch"
  fi

  if [[ -s "$unstaged_patch" ]]; then
    git -C "$target_root" apply --binary "$unstaged_patch"
  fi

  copy_untracked_snapshot "$source_root" "$target_root"
  rm -f "$staged_patch" "$unstaged_patch"
}

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
    --base-ref)
      BASE_REF="$2"
      shift 2
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

if [[ -z "$BASE_REF" ]]; then
  BASE_REF=$(git -C "$REPO_ROOT" rev-parse --verify HEAD)
fi

python3 "$SCRIPT_DIR/ensure_todo_template.py" "$TODO_PATH" --owner "$OWNER" >/dev/null

BRANCH=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --owner "$OWNER" --field branch)
SLUG=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --field slug)
TITLE=$(python3 "$SCRIPT_DIR/parse_todo.py" "$TODO_PATH" --task-id "$TASK_ID" --field title)
WORKTREE_DIR="$REPO_ROOT/.worktrees/${TASK_ID,,}-$SLUG"
CREATED_NEW_BRANCH=0

if [[ ! -d "$REPO_ROOT/.worktrees" ]]; then
  mkdir -p "$REPO_ROOT/.worktrees"
fi

mkdir -p "$REPO_ROOT/.codex/todo-hub"
printf '%s\n' "$REPO_ROOT" > "$REPO_ROOT/.codex/todo-hub/root_path.txt"

if git -C "$REPO_ROOT" worktree list --porcelain | grep -Fq "worktree $WORKTREE_DIR"; then
  echo "reuse $WORKTREE_DIR" >&2
else
  if git -C "$REPO_ROOT" show-ref --verify --quiet "refs/heads/$BRANCH"; then
    git -C "$REPO_ROOT" worktree add "$WORKTREE_DIR" "$BRANCH" >/dev/null
  else
    git -C "$REPO_ROOT" worktree add -b "$BRANCH" "$WORKTREE_DIR" "$BASE_REF" >/dev/null
    CREATED_NEW_BRANCH=1
  fi
fi

mkdir -p "$WORKTREE_DIR/.codex/todo-hub"
printf '%s\n' "$REPO_ROOT" > "$WORKTREE_DIR/.codex/todo-hub/root_path.txt"

if [[ "$CREATED_NEW_BRANCH" == "1" ]]; then
  apply_dirty_snapshot "$REPO_ROOT" "$WORKTREE_DIR"
  mkdir -p "$WORKTREE_DIR/.codex/todo-hub"
  printf '%s\n' "$REPO_ROOT" > "$WORKTREE_DIR/.codex/todo-hub/root_path.txt"
fi

python3 "$SCRIPT_DIR/runtime_ctl.py" upsert \
  --runtime-file "$RUNTIME_FILE" \
  --task-id "$TASK_ID" \
  --owner "$OWNER" \
  --title "$TITLE" \
  --branch "$BRANCH" \
  --worktree "$WORKTREE_DIR" \
  --status "allocated" >/dev/null

python3 "$SCRIPT_DIR/sync_dashboard.py" \
  --todo-path "$TODO_PATH" \
  --runtime-file "$RUNTIME_FILE" \
  --owner "$OWNER" >/dev/null

echo "$WORKTREE_DIR"
