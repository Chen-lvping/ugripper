#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_hmi_strict_reply_soak.sh [options]

Options:
  --output-dir DIR          Result directory (default: /dev/shm/ugripper_hmi_strict_reply_<time>)
  --baseline-sec N          Serialized 1Hz baseline duration (default: 60)
  --stress-sec N            Serialized pressure duration (default: 300)
  --reply-timeout-ms N      Per-request reply timeout (default: 250)
  --stress-interval-ms N    Delay after a matched reply (default: 20)
  --quarantine-ms N         Quiet drain after timeout (default: 200)
  --encoder-max-gap-ms N    Encoder maximum allowed gap (default: 20)
  --service-name NAME       Service stopped during direct access (default: ugripper.service)
  --ugripper-root DIR       Installed UGripper root (default: /opt/ugripper)
  --sensor-recorder-bin PATH
                             Override SensorRecorder binary
  --python-bin PATH         Python with mcap installed (default: UGripper venv)
  --help                     Show this help

Each HMI port has at most one state request in flight. The next request is sent
only after a valid beep-state frame arrives or the current request times out.
After a timeout, the port enters a quiet drain window so a late reply cannot be
mistaken for the next request. No RGB or beep-control commands are sent.
EOF
}

OUTPUT_DIR="/dev/shm/ugripper_hmi_strict_reply_$(timestamp_slug)"
BASELINE_SEC=60
STRESS_SEC=300
REPLY_TIMEOUT_MS=250
STRESS_INTERVAL_MS=20
QUARANTINE_MS=200
ENCODER_MAX_GAP_MS=20
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
SENSOR_RECORDER_BIN_OVERRIDE=""
PYTHON_BIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --baseline-sec) BASELINE_SEC="$2"; shift 2 ;;
        --stress-sec) STRESS_SEC="$2"; shift 2 ;;
        --reply-timeout-ms) REPLY_TIMEOUT_MS="$2"; shift 2 ;;
        --stress-interval-ms) STRESS_INTERVAL_MS="$2"; shift 2 ;;
        --quarantine-ms) QUARANTINE_MS="$2"; shift 2 ;;
        --encoder-max-gap-ms) ENCODER_MAX_GAP_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ugripper-root) UGRIPPER_ROOT="$2"; shift 2 ;;
        --sensor-recorder-bin) SENSOR_RECORDER_BIN_OVERRIDE="$2"; shift 2 ;;
        --python-bin) PYTHON_BIN_OVERRIDE="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

for value_name in BASELINE_SEC STRESS_SEC REPLY_TIMEOUT_MS STRESS_INTERVAL_MS QUARANTINE_MS ENCODER_MAX_GAP_MS; do
    value="${!value_name}"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${value_name} must be a positive integer" >&2; exit 2; }
done

SENSOR_RECORDER_BIN="$(resolve_executable "${SENSOR_RECORDER_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/SensorRecorder/SensorRecorder")"
PYTHON_BIN="$(resolve_executable "${PYTHON_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/.venv/bin/python3" python3)"

mkdir -p "${OUTPUT_DIR}/sensor_probe" "${OUTPUT_DIR}/snapshots"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0
SENSOR_PID=""

collect_snapshot() {
    local name="$1"
    local dir="${OUTPUT_DIR}/snapshots/${name}"
    mkdir -p "${dir}"
    date -Ins > "${dir}/time.txt"
    systemctl status "${SERVICE_NAME}" --no-pager -l > "${dir}/service.txt" 2>&1 || true
    ps -eo pid,stat,comm,args > "${dir}/processes.txt" 2>&1 || true
    ls -l /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder > "${dir}/links.txt" 2>&1 || true
    lsusb -t > "${dir}/lsusb-tree.txt" 2>&1 || true
    journalctl -k --since "${START_TIME}" --no-pager -o short-precise -l > "${dir}/kernel.log" 2>&1 || true
    command -v hws >/dev/null 2>&1 && hws > "${dir}/hws.txt" 2>&1 || true
}

stop_sensor() {
    if [[ -n "${SENSOR_PID}" ]] && kill -0 "${SENSOR_PID}" 2>/dev/null; then
        kill -INT "${SENSOR_PID}" 2>/dev/null || true
        for _ in $(seq 1 100); do kill -0 "${SENSOR_PID}" 2>/dev/null || break; sleep 0.1; done
        kill -TERM "${SENSOR_PID}" 2>/dev/null || true
        wait "${SENSOR_PID}" 2>/dev/null || true
    fi
    SENSOR_PID=""
}

restore_service() {
    stop_sensor
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" && break; sleep 1; done
        SERVICE_RESTORED=1
    fi
}
trap restore_service EXIT INT TERM

print_section "HMI Strict Reply Soak Configuration"
echo "output_dir=${OUTPUT_DIR}"
echo "baseline_sec=${BASELINE_SEC}"
echo "stress_sec=${STRESS_SEC}"
echo "reply_timeout_ms=${REPLY_TIMEOUT_MS}"
echo "stress_interval_ms=${STRESS_INTERVAL_MS}"
echo "quarantine_ms=${QUARANTINE_MS}"

collect_snapshot before
systemctl is-active --quiet "${SERVICE_NAME}" && SERVICE_WAS_ACTIVE=1
systemctl stop "${SERVICE_NAME}"
for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" || break; sleep 1; done
systemctl is-active --quiet "${SERVICE_NAME}" && { echo "service is still active" >&2; exit 1; }

for path in /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder; do
    [[ -e "${path}" ]] || { echo "missing device: ${path}" >&2; exit 1; }
done

"${SENSOR_RECORDER_BIN}" "${OUTPUT_DIR}/sensor_probe" > "${OUTPUT_DIR}/sensor_recorder.log" 2>&1 &
SENSOR_PID=$!
sleep 3
kill -0 "${SENSOR_PID}" 2>/dev/null || { cat "${OUTPUT_DIR}/sensor_recorder.log" >&2; exit 1; }

set +e
"${PYTHON_BIN}" - "${OUTPUT_DIR}" "${BASELINE_SEC}" "${STRESS_SEC}" "${REPLY_TIMEOUT_MS}" \
    "${STRESS_INTERVAL_MS}" "${QUARANTINE_MS}" > "${OUTPUT_DIR}/strict_reply.log" 2>&1 <<'PY'
import csv
import errno
import os
import selectors
import statistics
import sys
import termios
import time
from pathlib import Path

output_dir = Path(sys.argv[1])
baseline_sec = int(sys.argv[2])
stress_sec = int(sys.argv[3])
reply_timeout = int(sys.argv[4]) / 1000.0
stress_interval = int(sys.argv[5]) / 1000.0
quarantine = int(sys.argv[6]) / 1000.0

def xor(data):
    value = 0
    for byte in data: value ^= byte
    return value

request = bytearray((0x5A, 0x5A, 0x04, 0x02, 0x00, 0x00, 0x00))
request[-1] = xor(request[:-1])
request = bytes(request)

class Port:
    def __init__(self, side):
        self.side = side
        self.path = f"/dev/{side}_gripper"
        self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buffer = bytearray()
        self.pending_at = None
        self.pending_seq = 0
        self.next_send = time.monotonic()
        self.quarantine_until = 0.0
        self.tx = 0
        self.tx_errors = 0
        self.matched = 0
        self.timeouts = 0
        self.late_frames = 0
        self.rx_bytes = 0
        self.valid_frames = 0
        self.invalid_xor = 0
        self.discarded = 0
        self.latencies = []
        self.events = []

    def send(self, now):
        try:
            if os.write(self.fd, request) != len(request):
                self.tx_errors += 1
                return
        except OSError as error:
            if error.errno not in (errno.EAGAIN, errno.EWOULDBLOCK): self.tx_errors += 1
            return
        self.tx += 1
        self.pending_seq = self.tx
        self.pending_at = now

    def read(self, now, phase, interval):
        while True:
            try: data = os.read(self.fd, 4096)
            except BlockingIOError: break
            except OSError:
                self.tx_errors += 1
                break
            if not data: break
            self.rx_bytes += len(data)
            self.buffer.extend(data)
        while len(self.buffer) >= 8:
            header = self.buffer.find(b"\xA5\xA5")
            if header < 0:
                self.discarded += len(self.buffer)
                self.buffer.clear()
                break
            if header:
                self.discarded += header
                del self.buffer[:header]
            if len(self.buffer) < 8: break
            frame = self.buffer[:8]
            if xor(frame[:7]) != frame[7]:
                self.invalid_xor += 1
                self.discarded += 1
                del self.buffer[0]
                continue
            del self.buffer[:8]
            self.valid_frames += 1
            if frame[2] != 0x04:
                continue
            if self.pending_at is None:
                self.late_frames += 1
                self.events.append((phase, self.side, self.pending_seq, "late", "", now))
                continue
            latency_ms = (now - self.pending_at) * 1000.0
            self.matched += 1
            self.latencies.append(latency_ms)
            self.events.append((phase, self.side, self.pending_seq, "reply", f"{latency_ms:.3f}", now))
            self.pending_at = None
            self.next_send = now + interval

    def check_timeout(self, now, phase):
        if self.pending_at is None or now - self.pending_at < reply_timeout: return
        age_ms = (now - self.pending_at) * 1000.0
        self.timeouts += 1
        self.events.append((phase, self.side, self.pending_seq, "timeout", f"{age_ms:.3f}", now))
        print(f"TIMEOUT phase={phase} side={self.side} seq={self.pending_seq} age_ms={age_ms:.3f} rx_bytes={self.rx_bytes} valid_frames={self.valid_frames}", flush=True)
        self.pending_at = None
        self.quarantine_until = now + quarantine
        self.next_send = self.quarantine_until

ports = [Port("left"), Port("right")]
selector = selectors.DefaultSelector()
for port in ports: selector.register(port.fd, selectors.EVENT_READ, port)

phases = (("baseline", baseline_sec, 1.0), ("stress", stress_sec, stress_interval))
phase_rows = []
try:
    for phase, duration, interval in phases:
        start = time.monotonic(); end = start + duration
        before = {port.side: (port.tx, port.matched, port.timeouts, port.late_frames, port.tx_errors, len(port.latencies)) for port in ports}
        print(f"PHASE_START phase={phase} duration={duration} interval_ms={interval*1000:.3f}", flush=True)
        while time.monotonic() < end:
            now = time.monotonic()
            for port in ports:
                if port.pending_at is None and now >= port.next_send and now >= port.quarantine_until:
                    port.send(now)
            for key, _ in selector.select(0.001): key.data.read(time.monotonic(), phase, interval)
            now = time.monotonic()
            for port in ports:
                port.read(now, phase, interval)
                port.check_timeout(now, phase)
        for port in ports:
            deadline = time.monotonic() + reply_timeout
            while port.pending_at is not None and time.monotonic() < deadline:
                for key, _ in selector.select(0.001): key.data.read(time.monotonic(), phase, interval)
                port.read(time.monotonic(), phase, interval)
                port.check_timeout(time.monotonic(), phase)
            old = before[port.side]
            latencies = port.latencies[old[5]:]
            phase_rows.append({
                "phase": phase, "side": port.side,
                "tx": port.tx-old[0], "matched": port.matched-old[1],
                "timeouts": port.timeouts-old[2], "late_frames": port.late_frames-old[3],
                "tx_errors": port.tx_errors-old[4],
                "min_ms": min(latencies, default=0.0),
                "p50_ms": statistics.median(latencies) if latencies else 0.0,
                "p99_ms": sorted(latencies)[min(len(latencies)-1, int(len(latencies)*0.99))] if latencies else 0.0,
                "max_ms": max(latencies, default=0.0),
            })
finally:
    for port in ports: os.close(port.fd)

with (output_dir / "strict_reply_report.tsv").open("w", newline="") as output:
    writer = csv.DictWriter(output, fieldnames=phase_rows[0].keys(), delimiter="\t")
    writer.writeheader(); writer.writerows(phase_rows)
with (output_dir / "strict_reply_events.tsv").open("w", newline="") as output:
    writer = csv.writer(output, delimiter="\t")
    writer.writerow(("phase", "side", "sequence", "event", "latency_or_age_ms", "steady_time"))
    for port in ports: writer.writerows(port.events)
with (output_dir / "strict_reply_parser.tsv").open("w", newline="") as output:
    writer = csv.writer(output, delimiter="\t")
    writer.writerow(("side", "rx_bytes", "valid_frames", "invalid_xor", "discarded_bytes", "partial_bytes"))
    for port in ports: writer.writerow((port.side, port.rx_bytes, port.valid_frames, port.invalid_xor, port.discarded, len(port.buffer)))

failed = any(row["timeouts"] or row["late_frames"] or row["tx_errors"] for row in phase_rows)
raise SystemExit(1 if failed else 0)
PY
HMI_EXIT=$?
set -e

stop_sensor

set +e
"${PYTHON_BIN}" - "${OUTPUT_DIR}/sensor_probe" "${ENCODER_MAX_GAP_MS}" > "${OUTPUT_DIR}/encoder_gap_report.tsv" <<'PY'
import sys
from pathlib import Path
from mcap.reader import make_reader
from mcap.stream_reader import StreamReader

def messages(path):
    with path.open("rb") as stream:
        yielded = False
        try:
            for _schema, channel, message in make_reader(stream).iter_messages():
                yielded = True; yield channel, message
        except Exception:
            if yielded: raise
        if yielded: return
        stream.seek(0); channels = {}
        for record in StreamReader(stream).records:
            name = type(record).__name__
            if name == "Channel": channels[record.id] = record
            elif name == "Message" and record.channel_id in channels: yield channels[record.channel_id], record

root=Path(sys.argv[1]); allowed=int(sys.argv[2]); thresholds=(2,5,10,20,100,1000)
print("side\tcount\tspan_sec\trate_hz\tmax_gap_ms\tgt2ms\tgt5ms\tgt10ms\tgt20ms\tgt100ms\tgt1000ms\tresult")
failed=False
for side in ("left","right"):
    ts=[int(getattr(message,"log_time",getattr(message,"logTime",0))) for _,message in messages(root/f"sensor_{side}.mcap")]
    gaps=[b-a for a,b in zip(ts,ts[1:])]; span=ts[-1]-ts[0] if len(ts)>1 else 0
    rate=(len(ts)-1)/(span/1e9) if span else 0.0; maximum=max(gaps,default=0)/1e6
    counts=[sum(gap>threshold*1_000_000 for gap in gaps) for threshold in thresholds]
    result="pass" if ts and maximum<=allowed else "fail"; failed |= result=="fail"
    print(f"{side}\t{len(ts)}\t{span/1e9:.3f}\t{rate:.3f}\t{maximum:.3f}\t"+"\t".join(map(str,counts))+f"\t{result}")
raise SystemExit(1 if failed else 0)
PY
ENCODER_EXIT=$?
set -e

collect_snapshot after_direct_test
restore_service
collect_snapshot after_service_restore
journalctl -u "${SERVICE_NAME}" --since "${START_TIME}" --no-pager -o short-precise -l > "${OUTPUT_DIR}/service.log" 2>&1 || true

cat > "${OUTPUT_DIR}/summary.txt" <<EOF
start_time=${START_TIME}
end_time=$(date '+%Y-%m-%d %H:%M:%S')
hmi_exit=${HMI_EXIT}
encoder_exit=${ENCODER_EXIT}
service_was_active=${SERVICE_WAS_ACTIVE}
service_restored=${SERVICE_RESTORED}
EOF

print_section "Summary"
cat "${OUTPUT_DIR}/summary.txt"
cat "${OUTPUT_DIR}/strict_reply_report.tsv"
cat "${OUTPUT_DIR}/strict_reply_parser.tsv"
cat "${OUTPUT_DIR}/encoder_gap_report.tsv"
echo "result_dir=${OUTPUT_DIR}"

[[ "${HMI_EXIT}" == "0" && "${ENCODER_EXIT}" == "0" ]]
