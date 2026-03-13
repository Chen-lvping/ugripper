#!/usr/bin/env python3

from __future__ import annotations

import json
import re
import textwrap
import ast
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

TASK_MARKER = "<!-- TODO_HUB_TASK -->"
TASK_PATTERN = re.compile(
    r"<!--\s*TODO_HUB_TASK\s*-->\s*(?:###.*?\n)?```toml\n(.*?)\n```",
    re.DOTALL,
)
VALID_TYPES = {"feature", "test", "fix"}
VALID_STATUSES = {"todo", "in_progress", "blocked", "done"}


def now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def branch_name(task: dict[str, Any], owner: str) -> str:
    return f"{task['type']}/{owner}/{task['slug']}"


def default_todo_template(owner: str) -> str:
    return textwrap.dedent(
        f"""\
        # TODO List

        这个文件由你和 `todo-hub` 主控 agent 共同维护。你可以直接手改；skill 会按约定解析真实任务。

        ## 规则

        - 默认不会启动全部任务；只会启动你明确点名、`focus = true` 或筛选命中的任务。
        - 只有带 `<!-- TODO_HUB_TASK -->` 标记的 TOML 代码块才会被解析成真实任务。
        - `slug` 使用英文短横线，用于分支名与 worktree 目录。
        - 分支格式固定为：
          - `feature/{owner}/<slug>`
          - `test/{owner}/<slug>`
          - `fix/{owner}/<slug>`

        ## 新任务模板（复制后记得补 `<!-- TODO_HUB_TASK -->` 标记）

        ```toml
        id = "TASK-001"
        title = "示例：补充 USB 音频回归测试"
        type = "test"
        slug = "usb-audio-regression"
        status = "todo"
        enabled = false
        focus = false
        depends_on = []
        summary = ""
        acceptance = [
          "写第一条验收条件",
          "写第二条验收条件",
        ]
        notes = \"\"\"
        这里写上下文、限制、关注文件、排除范围等。
        \"\"\"
        ```

        ## Active Tasks

        在下面追加真实任务块。
        """
    )


def ensure_todo_template(todo_path: Path, owner: str) -> bool:
    todo_path.parent.mkdir(parents=True, exist_ok=True)
    if todo_path.exists():
        return False
    todo_path.write_text(default_todo_template(owner), encoding="utf-8")
    return True


def _normalize_task(task: dict[str, Any]) -> dict[str, Any]:
    normalized = {
        "id": str(task.get("id", "")).strip(),
        "title": str(task.get("title", "")).strip(),
        "type": str(task.get("type", "")).strip(),
        "slug": str(task.get("slug", "")).strip(),
        "status": str(task.get("status", "")).strip(),
        "enabled": bool(task.get("enabled", False)),
        "focus": bool(task.get("focus", False)),
        "depends_on": list(task.get("depends_on", [])),
        "summary": str(task.get("summary", "")).strip(),
        "acceptance": list(task.get("acceptance", [])),
        "notes": str(task.get("notes", "")).rstrip(),
    }

    missing = [
        field
        for field in ("id", "title", "type", "slug", "status")
        if not normalized[field]
    ]
    if missing:
        raise ValueError(f"missing required fields: {', '.join(missing)}")
    if normalized["type"] not in VALID_TYPES:
        raise ValueError(f"invalid type: {normalized['type']}")
    if normalized["status"] not in VALID_STATUSES:
        raise ValueError(f"invalid status: {normalized['status']}")
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", normalized["slug"]):
        raise ValueError(f"invalid slug: {normalized['slug']}")
    return normalized


def _pythonize_literal(raw: str) -> str:
    raw = re.sub(r"\btrue\b", "True", raw)
    raw = re.sub(r"\bfalse\b", "False", raw)
    return raw


def parse_task_block(block: str) -> dict[str, Any]:
    lines = block.splitlines()
    index = 0
    parsed: dict[str, Any] = {}

    while index < len(lines):
        line = lines[index].strip()
        index += 1
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"invalid line: {line}")

        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()

        if value == '"""':
            chunks: list[str] = []
            while index < len(lines):
                current = lines[index]
                index += 1
                if current.strip() == '"""':
                    break
                chunks.append(current)
            parsed[key] = "\n".join(chunks).rstrip()
            continue

        if value.startswith("[") and not value.endswith("]"):
            chunks = [value]
            balance = value.count("[") - value.count("]")
            while index < len(lines) and balance > 0:
                current = lines[index].strip()
                index += 1
                if not current or current.startswith("#"):
                    continue
                chunks.append(current)
                balance += current.count("[") - current.count("]")
            value = " ".join(chunks)

        parsed[key] = ast.literal_eval(_pythonize_literal(value))

    return parsed


def parse_tasks(todo_path: Path) -> list[dict[str, Any]]:
    if not todo_path.exists():
        return []
    content = todo_path.read_text(encoding="utf-8")
    tasks: list[dict[str, Any]] = []
    for index, block in enumerate(TASK_PATTERN.findall(content), start=1):
        try:
            raw = parse_task_block(block)
            tasks.append(_normalize_task(raw))
        except Exception as exc:
            raise ValueError(f"failed to parse task block #{index}: {exc}") from exc
    return tasks


def filter_tasks(
    tasks: list[dict[str, Any]],
    *,
    task_id: str | None = None,
    enabled_only: bool = False,
    focus_only: bool = False,
    task_type: str | None = None,
    status: str | None = None,
) -> list[dict[str, Any]]:
    filtered = tasks
    if task_id:
        filtered = [task for task in filtered if task["id"] == task_id]
    if enabled_only:
        filtered = [task for task in filtered if task["enabled"]]
    if focus_only:
        filtered = [task for task in filtered if task["focus"]]
    if task_type:
        filtered = [task for task in filtered if task["type"] == task_type]
    if status:
        filtered = [task for task in filtered if task["status"] == status]
    return filtered


def load_runtime(runtime_path: Path) -> dict[str, Any]:
    if not runtime_path.exists():
        return {"updated_at": None, "owner": None, "tasks": {}}
    return json.loads(runtime_path.read_text(encoding="utf-8"))


def save_runtime(runtime_path: Path, payload: dict[str, Any]) -> None:
    runtime_path.parent.mkdir(parents=True, exist_ok=True)
    payload["updated_at"] = now_iso()
    runtime_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def short_session(session_id: str | None) -> str:
    if not session_id:
        return "-"
    return session_id[:12]
