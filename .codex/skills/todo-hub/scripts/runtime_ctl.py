#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path

from todo_lib import load_runtime, save_runtime

SCRIPT_DIR = Path(__file__).resolve().parent

def resolve_repo_root() -> Path:
    env_root = os.environ.get("TODO_HUB_ROOT")
    if env_root:
        return Path(env_root).resolve()
    result = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if result.returncode == 0:
        git_top = Path(result.stdout.strip())
        marker = git_top / ".codex/todo-hub/root_path.txt"
        if marker.exists():
            content = marker.read_text(encoding="utf-8").strip()
            if content:
                return Path(content).resolve()
    return SCRIPT_DIR.parent.parent.parent.parent.resolve()

REPO_ROOT = resolve_repo_root()
DEFAULT_RUNTIME_PATH = REPO_ROOT / ".codex/todo-hub/runtime.json"


FIELDS = (
    "branch",
    "worktree",
    "session_id",
    "pid",
    "unit_name",
    "status",
    "last_event_at",
    "last_message_file",
    "log_file",
    "last_command",
    "title",
    "progress_file",
)


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--runtime-file",
        default=str(DEFAULT_RUNTIME_PATH),
        help="Path to runtime json file.",
    )


def cmd_upsert(args: argparse.Namespace) -> int:
    runtime_path = Path(args.runtime_file)
    payload = load_runtime(runtime_path)
    payload["owner"] = args.owner or payload.get("owner")
    task_entry = payload.setdefault("tasks", {}).setdefault(args.task_id, {})

    for key in FIELDS:
        value = getattr(args, key)
        if value is not None:
            task_entry[key] = value

    save_runtime(runtime_path, payload)
    print(json.dumps(task_entry, ensure_ascii=False, indent=2))
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    payload = load_runtime(Path(args.runtime_file))
    if args.task_id:
        print(json.dumps(payload.get("tasks", {}).get(args.task_id, {}), ensure_ascii=False, indent=2))
    else:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Manage todo-hub runtime state.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    upsert = subparsers.add_parser("upsert")
    add_common_args(upsert)
    upsert.add_argument("--task-id", required=True)
    upsert.add_argument("--owner")
    for field in FIELDS:
        upsert.add_argument(f"--{field}")
    upsert.set_defaults(func=cmd_upsert)

    show = subparsers.add_parser("show")
    add_common_args(show)
    show.add_argument("--task-id")
    show.set_defaults(func=cmd_show)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
