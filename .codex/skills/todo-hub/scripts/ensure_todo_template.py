#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path

from todo_lib import ensure_todo_template


def main() -> int:
    parser = argparse.ArgumentParser(description="Ensure docs/todo_list.md exists.")
    parser.add_argument("todo_path", nargs="?", default="docs/todo_list.md")
    parser.add_argument("--owner", default="zhouwu")
    args = parser.parse_args()

    created = ensure_todo_template(Path(args.todo_path), args.owner)
    if created:
        print(f"created {args.todo_path}")
    else:
        print(f"kept {args.todo_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
