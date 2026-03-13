#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path

from todo_lib import ensure_todo_template, load_runtime, now_iso, parse_tasks, short_session

SCRIPT_DIR = Path(__file__).resolve().parent

def resolve_repo_root() -> Path:
    env_root = os.environ.get("TODO_HUB_ROOT")
    if env_root:
        return Path(env_root).resolve()
    git_top = None
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
DEFAULT_TODO_PATH = REPO_ROOT / "docs/todo_list.md"
DEFAULT_RUNTIME_PATH = REPO_ROOT / ".codex/todo-hub/runtime.json"
DEFAULT_DASHBOARD_PATH = REPO_ROOT / "docs/todo_dashboard.md"


def _unit_state(unit_name: str | None) -> tuple[str | None, str | None]:
    if not unit_name:
        return None, None
    active = subprocess.run(
        ["systemctl", "--user", "show", unit_name, "--property", "ActiveState", "--value"],
        capture_output=True,
        text=True,
    )
    if active.returncode != 0:
        return None, None
    sub = subprocess.run(
        ["systemctl", "--user", "show", unit_name, "--property", "SubState", "--value"],
        capture_output=True,
        text=True,
    )
    return active.stdout.strip() or None, sub.stdout.strip() or None


def _runtime_state(runtime_entry: dict) -> str:
    status = runtime_entry.get("status") or "-"
    unit_name = runtime_entry.get("unit_name")
    active_state, sub_state = _unit_state(unit_name)
    if active_state:
        if active_state == "active":
            return "running"
        if active_state == "failed":
            return "failed"
        if active_state == "inactive":
            return "stopped"
        return f"{active_state}/{sub_state}" if sub_state else active_state

    pid = str(runtime_entry.get("pid") or "").strip()
    if pid:
        if Path(f"/proc/{pid}").exists():
            return "running"
        if status == "running":
            return "stopped"
    return status


def _recent_progress(runtime_entry: dict) -> list[str]:
    progress_file = runtime_entry.get("progress_file")
    if progress_file:
        progress_path = Path(progress_file)
        if progress_path.exists():
            content = progress_path.read_text(encoding="utf-8").strip().splitlines()
            if content:
                return content[:3]

    last_message_file = runtime_entry.get("last_message_file")
    if last_message_file:
        message_path = Path(last_message_file)
        if message_path.exists():
            content = message_path.read_text(encoding="utf-8").strip().splitlines()
            if content:
                return content[:3]

    log_file = runtime_entry.get("log_file")
    if not log_file:
        return []
    log_path = Path(log_file)
    if not log_path.exists():
        return []

    lines = log_path.read_text(encoding="utf-8", errors="ignore").splitlines()[-200:]
    for line in reversed(lines):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        item = payload.get("item") or {}
        if item.get("type") == "agent_message" and item.get("text"):
            return str(item["text"]).strip().splitlines()[:3]
    return []


def render_dashboard(todo_path: Path, runtime_path: Path, dashboard_path: Path, owner: str) -> str:
    ensure_todo_template(todo_path, owner)
    tasks = parse_tasks(todo_path)
    runtime = load_runtime(runtime_path)
    task_runtime = runtime.get("tasks", {})

    total = len(tasks)
    active = sum(1 for task in tasks if task["status"] != "done")
    focus_count = sum(1 for task in tasks if task["focus"])

    lines = [
        "# TODO Dashboard",
        "",
        f"- Updated at: `{now_iso()}`",
        f"- Owner: `{owner}`",
        f"- Total tasks: `{total}`",
        f"- Active tasks: `{active}`",
        f"- Focus tasks: `{focus_count}`",
        "",
        "## Focus",
        "",
    ]

    focus_tasks = [task for task in tasks if task["focus"]]
    if focus_tasks:
        for task in focus_tasks:
            runtime_entry = task_runtime.get(task["id"], {})
            lines.append(
                f"- `{task['id']}` | `{task['status']}` | runtime `{_runtime_state(runtime_entry)}` | "
                f"session `{short_session(runtime_entry.get('session_id'))}` | {task['title']}"
            )
    else:
        lines.append("- No focus tasks yet.")

    lines.extend(["", "## Tasks", ""])
    if not tasks:
        lines.append("- No active task blocks yet. Edit `docs/todo_list.md` and add a real task block.")
    else:
        for task in tasks:
            runtime_entry = task_runtime.get(task["id"], {})
            recent_progress = _recent_progress(runtime_entry)
            summary_parts = [
                f"status `{task['status']}`",
                f"runtime `{_runtime_state(runtime_entry)}`",
                f"enabled `{task['enabled']}`",
                f"focus `{task['focus']}`",
                f"branch `{runtime_entry.get('branch', '-')}`",
                f"session `{short_session(runtime_entry.get('session_id'))}`",
            ]
            lines.append(f"### {task['id']} · {task['title']}")
            lines.append("")
            lines.append(f"- {' | '.join(summary_parts)}")
            lines.append(f"- Worktree: `{runtime_entry.get('worktree', '-')}`")
            if runtime_entry.get("unit_name"):
                lines.append(f"- Unit: `{runtime_entry['unit_name']}`")
            if task["summary"]:
                lines.append(f"- Summary: {task['summary']}")
            if recent_progress:
                lines.append(f"- Last message: {' / '.join(recent_progress)}")
            if task["acceptance"]:
                lines.append(f"- Acceptance: {'；'.join(task['acceptance'])}")
            lines.append("")

    dashboard_path.parent.mkdir(parents=True, exist_ok=True)
    content = "\n".join(lines).rstrip() + "\n"
    dashboard_path.write_text(content, encoding="utf-8")
    return content


def main() -> int:
    parser = argparse.ArgumentParser(description="Refresh docs/todo_dashboard.md from todo/runtime state.")
    parser.add_argument("--todo-path", default=str(DEFAULT_TODO_PATH))
    parser.add_argument("--runtime-file", default=str(DEFAULT_RUNTIME_PATH))
    parser.add_argument("--dashboard-path", default=str(DEFAULT_DASHBOARD_PATH))
    parser.add_argument("--owner", default="zhouwu")
    args = parser.parse_args()

    render_dashboard(
        Path(args.todo_path),
        Path(args.runtime_file),
        Path(args.dashboard_path),
        args.owner,
    )
    print(args.dashboard_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
