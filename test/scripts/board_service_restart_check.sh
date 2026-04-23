#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_service_restart_check.sh [options]

Options:
  --output-dir DIR            Output directory for collected status and logs
  --service-name NAME         systemd service name (default: ugripper.service)
  --stereo-status PATH        Stereo status file path (default: /tmp/umi_stereo_camera_status.json)
  --timeout-sec N             Wait timeout in seconds (default: 45)
  --journal-since TEXT        journalctl --since value (default: 5 min ago)
  --skip-restart              Do not restart service, only check current state
  --skip-systemctl            Skip systemctl status/is-active checks
  --allow-health-faults       Do not fail when health fault lines are found in recent log
  --keep-output               Keep output directory if present
  --help                      Show this help
EOF
}

OUTPUT_DIR=""
SERVICE_NAME="ugripper.service"
STEREO_STATUS_FILE="/tmp/umi_stereo_camera_status.json"
TIMEOUT_SEC=45
JOURNAL_SINCE="5 min ago"
SKIP_RESTART=0
SKIP_SYSTEMCTL=0
ALLOW_HEALTH_FAULTS=0
KEEP_OUTPUT=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --service-name)
            SERVICE_NAME="$2"
            shift 2
            ;;
        --stereo-status)
            STEREO_STATUS_FILE="$2"
            shift 2
            ;;
        --timeout-sec)
            TIMEOUT_SEC="$2"
            shift 2
            ;;
        --journal-since)
            JOURNAL_SINCE="$2"
            shift 2
            ;;
        --skip-restart)
            SKIP_RESTART=1
            shift
            ;;
        --skip-systemctl)
            SKIP_SYSTEMCTL=1
            shift
            ;;
        --allow-health-faults)
            ALLOW_HEALTH_FAULTS=1
            shift
            ;;
        --keep-output)
            KEEP_OUTPUT=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="/tmp/pp_board_service_restart_$(timestamp_slug)"
fi

if [[ "${KEEP_OUTPUT}" != "1" ]]; then
    rm -rf "${OUTPUT_DIR}"
fi
mkdir -p "${OUTPUT_DIR}"

print_section "Service Restart Check"
echo "output_dir=${OUTPUT_DIR}"
echo "service_name=${SERVICE_NAME}"
echo "stereo_status=${STEREO_STATUS_FILE}"
echo "timeout_sec=${TIMEOUT_SEC}"

if [[ "${SKIP_RESTART}" != "1" ]]; then
    print_section "Restart Service"
    systemctl restart "${SERVICE_NAME}"
fi

print_section "Wait For Active"
if ! timeout "${TIMEOUT_SEC}s" bash -lc "until systemctl is-active --quiet '${SERVICE_NAME}'; do sleep 1; done"; then
    echo "service did not become active within timeout" >&2
    exit 1
fi

print_section "Wait For Stereo Status"
if ! wait_for_path "${STEREO_STATUS_FILE}" "${TIMEOUT_SEC}" 1; then
    echo "stereo status file did not appear: ${STEREO_STATUS_FILE}" >&2
    exit 1
fi

cp "${STEREO_STATUS_FILE}" "${OUTPUT_DIR}/stereo_status.json"

python3 - "$STEREO_STATUS_FILE" <<'PY'
import json
import sys
path = sys.argv[1]
with open(path, "r", encoding="utf-8") as fh:
    data = json.load(fh)
if not data.get("ready", False):
    raise SystemExit("stereo status ready=false")
if data.get("service_state") != "ready":
    raise SystemExit(f"stereo service_state not ready: {data.get('service_state')}")
PY

print_section "Collect Logs"
journalctl -u "${SERVICE_NAME}" --since "${JOURNAL_SINCE}" --no-pager -o short-precise -l > "${OUTPUT_DIR}/service.log"

if [[ "${SKIP_SYSTEMCTL}" != "1" ]]; then
    systemctl status "${SERVICE_NAME}" --no-pager -l > "${OUTPUT_DIR}/systemctl_status.txt"
    systemctl is-active "${SERVICE_NAME}" > "${OUTPUT_DIR}/systemctl_is_active.txt"
fi

if [[ "${ALLOW_HEALTH_FAULTS}" != "1" ]]; then
    if grep -qi 'hardware health fault' "${OUTPUT_DIR}/service.log"; then
        echo "hardware health fault detected in recent service log" >&2
        exit 1
    fi
fi

if ! grep -q 'audio player recovered and is ready' "${OUTPUT_DIR}/service.log"; then
    echo "audio ready recovery log not found in recent service log" >&2
    exit 1
fi

print_section "Done"
echo "service_log=${OUTPUT_DIR}/service.log"
echo "stereo_status_copy=${OUTPUT_DIR}/stereo_status.json"
if [[ "${SKIP_SYSTEMCTL}" != "1" ]]; then
    echo "systemctl_status=${OUTPUT_DIR}/systemctl_status.txt"
fi
