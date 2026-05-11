#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
TARGET_HOST="${TARGET_HOST:-192.168.1.110}"
TARGET_USER="${TARGET_USER:-ubuntu}"
TARGET_PASSWORD="${TARGET_PASSWORD:-}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/ugripper-deb-install}"
KNOWN_HOSTS_PATH="${KNOWN_HOSTS_PATH:-/tmp/codex_known_hosts_3588ether}"
DEB_PATH=""

usage() {
    cat >&2 <<EOF
Usage: ${SCRIPT_NAME} --deb <path> [--host <host>] [--user <user>] [--password <password>] [--remote-dir <dir>]

Defaults:
  --host       ${TARGET_HOST}
  --user       ${TARGET_USER}
  --remote-dir ${REMOTE_DIR}

Environment overrides:
  TARGET_HOST TARGET_USER TARGET_PASSWORD REMOTE_DIR KNOWN_HOSTS_PATH
EOF
}

require_tool() {
    local tool_name="$1"
    if ! command -v "$tool_name" >/dev/null 2>&1; then
        echo "Required command not found: $tool_name" >&2
        exit 1
    fi
}

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --deb)
            [[ $# -ge 2 ]] || { echo "--deb requires a value" >&2; usage; exit 2; }
            DEB_PATH="$2"
            shift 2
            ;;
        --host)
            [[ $# -ge 2 ]] || { echo "--host requires a value" >&2; usage; exit 2; }
            TARGET_HOST="$2"
            shift 2
            ;;
        --user)
            [[ $# -ge 2 ]] || { echo "--user requires a value" >&2; usage; exit 2; }
            TARGET_USER="$2"
            shift 2
            ;;
        --password)
            [[ $# -ge 2 ]] || { echo "--password requires a value" >&2; usage; exit 2; }
            TARGET_PASSWORD="$2"
            shift 2
            ;;
        --remote-dir)
            [[ $# -ge 2 ]] || { echo "--remote-dir requires a value" >&2; usage; exit 2; }
            REMOTE_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            if [[ -z "$DEB_PATH" && "$1" != -* ]]; then
                DEB_PATH="$1"
                shift
            else
                echo "Unknown argument: $1" >&2
                usage
                exit 2
            fi
            ;;
    esac
done

if [[ -z "$DEB_PATH" ]]; then
    echo "Missing --deb <path>" >&2
    usage
    exit 2
fi
if [[ ! -f "$DEB_PATH" ]]; then
    echo "Deb file not found: $DEB_PATH" >&2
    exit 1
fi

require_tool ssh
require_tool scp
if [[ -n "$TARGET_PASSWORD" ]]; then
    require_tool sshpass
fi

DEB_PATH="$(realpath "$DEB_PATH")"
DEB_NAME="$(basename "$DEB_PATH")"
REMOTE_DEB="${REMOTE_DIR}/${DEB_NAME}"
TARGET="${TARGET_USER}@${TARGET_HOST}"

SSH_OPTS=(
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile="${KNOWN_HOSTS_PATH}"
)

ssh_cmd() {
    if [[ -n "$TARGET_PASSWORD" ]]; then
        SSHPASS="$TARGET_PASSWORD" sshpass -e ssh "${SSH_OPTS[@]}" "$TARGET" "$@"
    else
        ssh "${SSH_OPTS[@]}" "$TARGET" "$@"
    fi
}

scp_cmd() {
    if [[ -n "$TARGET_PASSWORD" ]]; then
        SSHPASS="$TARGET_PASSWORD" sshpass -e scp "${SSH_OPTS[@]}" "$DEB_PATH" "$TARGET:$REMOTE_DEB"
    else
        scp "${SSH_OPTS[@]}" "$DEB_PATH" "$TARGET:$REMOTE_DEB"
    fi
}

password_literal="$(shell_quote "$TARGET_PASSWORD")"
remote_deb_literal="$(shell_quote "$REMOTE_DEB")"
remote_dir_literal="$(shell_quote "$REMOTE_DIR")"

echo "==> Target: $TARGET"
echo "==> Deb:    $DEB_PATH"
echo "==> Remote: $REMOTE_DEB"

ssh_cmd "mkdir -p ${remote_dir_literal}"
scp_cmd

if [[ -n "$TARGET_PASSWORD" ]]; then
    read -r -d '' REMOTE_SCRIPT <<EOF || true
set -euo pipefail
sudo_run() {
    printf '%s\n' ${password_literal} | sudo -S -p '' "\$@"
}
sudo_run dpkg -i ${remote_deb_literal}
dpkg-query -W -f='package=\${Package}\nversion=\${Version}\narchitecture=\${Architecture}\n' ugripper
systemctl --no-pager --full status ugripper.service | sed -n '1,25p' || true
EOF
else
    read -r -d '' REMOTE_SCRIPT <<EOF || true
set -euo pipefail
sudo_run() {
    sudo "\$@"
}
sudo_run dpkg -i ${remote_deb_literal}
dpkg-query -W -f='package=\${Package}\nversion=\${Version}\narchitecture=\${Architecture}\n' ugripper
systemctl --no-pager --full status ugripper.service | sed -n '1,25p' || true
EOF
fi

ssh_cmd "bash -s" <<< "$REMOTE_SCRIPT"
echo "==> Install completed."
