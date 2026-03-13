#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path

from todo_lib import branch_name, filter_tasks, parse_tasks


def main() -> int:
    parser = argparse.ArgumentParser(description="Parse docs/todo_list.md task blocks.")
    parser.add_argument("todo_path", nargs="?", default="docs/todo_list.md")
    parser.add_argument("--task-id")
    parser.add_argument("--enabled-only", action="store_true")
    parser.add_argument("--focus", action="store_true")
    parser.add_argument("--type", dest="task_type")
    parser.add_argument("--status")
    parser.add_argument("--owner", default="zhouwu")
    parser.add_argument("--field")
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args()

    tasks = filter_tasks(
        parse_tasks(Path(args.todo_path)),
        task_id=args.task_id,
        enabled_only=args.enabled_only,
        focus_only=args.focus,
        task_type=args.task_type,
        status=args.status,
    )
    for task in tasks:
        task["branch"] = branch_name(task, args.owner)

    if args.field:
        if not args.task_id:
            raise SystemExit("--field requires --task-id")
        if len(tasks) != 1:
            raise SystemExit(f"task not found: {args.task_id}")
        value = tasks[0].get(args.field)
        if value is None:
            raise SystemExit(f"field not found: {args.field}")
        if isinstance(value, (list, dict)):
            print(json.dumps(value, ensure_ascii=False))
        else:
            print(value)
        return 0

    payload = tasks[0] if args.task_id and len(tasks) == 1 else tasks
    print(json.dumps(payload, ensure_ascii=False, indent=2 if args.pretty else None))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
