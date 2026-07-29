#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
VALIDATOR="$PROJECT_ROOT/.codex/skills/validate-episode-data/scripts/validate_episode.py"

if command -v uv >/dev/null 2>&1; then
    exec uv run python "$VALIDATOR" "$@"
fi

if [ -x "$PROJECT_ROOT/.venv/bin/python3" ]; then
    exec "$PROJECT_ROOT/.venv/bin/python3" "$VALIDATOR" "$@"
fi

if [ -x /usr/bin/python3 ]; then
    exec /usr/bin/python3 "$VALIDATOR" "$@"
fi

exec python3 "$VALIDATOR" "$@"
