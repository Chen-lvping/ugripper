#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

OUTPUT_DIR="/dev/shm/ugripper_encoder_cmd_ack_$(timestamp_slug)"
ITERATIONS=5000
TIMEOUT_MS=20
INTERVAL_MS=0
SERVICE_NAME="ugripper.service"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --iterations) ITERATIONS="$2"; shift 2 ;;
        --timeout-ms) TIMEOUT_MS="$2"; shift 2 ;;
        --interval-ms) INTERVAL_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--output-dir DIR] [--iterations N] [--timeout-ms N] [--interval-ms N]"
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "${OUTPUT_DIR}"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0

restore_service() {
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        if systemctl is-active --quiet "${SERVICE_NAME}"; then SERVICE_RESTORED=1; fi
    fi
}

cleanup() {
    local exit_code=$?
    jobs -pr | xargs -r kill >/dev/null 2>&1 || true
    restore_service
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

{
    date --iso-8601=ns
    systemctl status "${SERVICE_NAME}" --no-pager || true
    hws || true
    ls -l /dev/left_encoder /dev/right_encoder
} > "${OUTPUT_DIR}/before.txt" 2>&1

if systemctl is-active --quiet "${SERVICE_NAME}"; then
    SERVICE_WAS_ACTIVE=1
    systemctl stop "${SERVICE_NAME}"
    for _ in $(seq 1 30); do
        systemctl is-active --quiet "${SERVICE_NAME}" || break
        sleep 1
    done
    systemctl is-active --quiet "${SERVICE_NAME}" && { echo "failed to stop service" >&2; exit 1; }
fi

sleep 1
for device in /dev/left_encoder /dev/right_encoder; do
    [[ -e "${device}" ]] || { echo "missing ${device}" >&2; exit 1; }
    fuser "${device}" >/dev/null 2>&1 && { echo "busy ${device}" >&2; exit 1; }
done

python3 "${SCRIPT_DIR}/board_encoder_cmd_ack_latency.py" \
    --device /dev/left_encoder --iterations "${ITERATIONS}" \
    --timeout-ms "${TIMEOUT_MS}" --interval-ms "${INTERVAL_MS}" \
    --output "${OUTPUT_DIR}/left.json" > "${OUTPUT_DIR}/left.stdout" 2>&1 &
LEFT_PID=$!
python3 "${SCRIPT_DIR}/board_encoder_cmd_ack_latency.py" \
    --device /dev/right_encoder --iterations "${ITERATIONS}" \
    --timeout-ms "${TIMEOUT_MS}" --interval-ms "${INTERVAL_MS}" \
    --output "${OUTPUT_DIR}/right.json" > "${OUTPUT_DIR}/right.stdout" 2>&1 &
RIGHT_PID=$!

set +e
wait "${LEFT_PID}"; LEFT_RC=$?
wait "${RIGHT_PID}"; RIGHT_RC=$?
set -e

restore_service
sleep 5
{
    date --iso-8601=ns
    echo "service=$(systemctl is-active "${SERVICE_NAME}" || true)"
    hws || true
} > "${OUTPUT_DIR}/after.txt" 2>&1

python3 - "${OUTPUT_DIR}" "${LEFT_RC}" "${RIGHT_RC}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
results = {}
for side in ("left", "right"):
    path = root / f"{side}.json"
    results[side] = json.loads(path.read_text())["summary"] if path.exists() else {"missing": True}
results["exit_codes"] = {"left": int(sys.argv[2]), "right": int(sys.argv[3])}
(root / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
PY

echo "output_dir=${OUTPUT_DIR}"
[[ "${LEFT_RC}" -eq 0 && "${RIGHT_RC}" -eq 0 ]]
