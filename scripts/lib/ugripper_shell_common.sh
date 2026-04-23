#!/bin/bash

ugripper_trim_text() {
    local text="${1:-}"
    text="${text#"${text%%[![:space:]]*}"}"
    text="${text%"${text##*[![:space:]]}"}"
    printf '%s' "$text"
}

ugripper_log() {
    local prefix="${1:-ugripper}"
    local level="${2:-INFO}"
    shift 2 || true
    printf '[%s][%s][%s] %s\n' "$prefix" "$level" "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

ugripper_read_env_value() {
    local env_file="$1"
    local key="$2"
    local line=""
    local current_key=""
    local value=""

    [ -f "$env_file" ] || return 1

    while IFS= read -r line || [ -n "$line" ]; do
        line="$(ugripper_trim_text "$line")"
        [ -n "$line" ] || continue

        case "$line" in
            \#*)
                continue
                ;;
        esac

        if [ "${line#*=}" = "$line" ]; then
            continue
        fi

        current_key="$(ugripper_trim_text "${line%%=*}")"
        value="$(ugripper_trim_text "${line#*=}")"
        if [ "$current_key" != "$key" ]; then
            continue
        fi

        value="${value#\"}"
        value="${value%\"}"
        value="${value#\'}"
        value="${value%\'}"
        printf '%s' "$(ugripper_trim_text "$value")"
        return 0
    done < "$env_file"

    return 0
}

ugripper_resolve_python() {
    local project_root="${1:-}"

    if [ -n "$project_root" ] && [ -x "$project_root/.venv/bin/python3" ]; then
        printf '%s' "$project_root/.venv/bin/python3"
        return 0
    fi

    if command -v uv >/dev/null 2>&1; then
        printf '%s' "uv run python3"
        return 0
    fi

    printf '%s' "python3"
}

ugripper_resolve_first_existing() {
    local candidate=""

    for candidate in "$@"; do
        [ -n "$candidate" ] || continue

        if [ -e "$candidate" ] || [ -L "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done

    printf '%s' "${1:-}"
}

ugripper_resolve_existing_path() {
    ugripper_resolve_first_existing "$@"
}
