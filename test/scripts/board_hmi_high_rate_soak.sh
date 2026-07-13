#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_hmi_high_rate_soak.sh [options]

Options:
  --output-dir DIR         Result directory (default: /dev/shm/ugripper_hmi_high_rate_<time>)
  --stage-sec N            Duration of each traffic stage (default: 60)
  --saturation-repeat N    Number of saturation stages (default: 1)
  --hmi-inactive-ms N      Valid-frame age timeout threshold (default: 5500)
  --encoder-max-gap-ms N   Encoder maximum allowed gap (default: 20)
  --service-name NAME      Service stopped during direct access (default: ugripper.service)
  --ugripper-root DIR      Installed UGripper root (default: /opt/ugripper)
  --sensor-recorder-bin PATH
                            Override SensorRecorder binary
  --python-bin PATH        Python with mcap installed (default: UGripper venv)
  --help                    Show this help

Both HMI ports are opened exactly once. Traffic increases through these stages:
  baseline: state=1Hz
  low:      state=50Hz,  RGB=10Hz,  beep pulse=1Hz
  medium:   state=100Hz, RGB=20Hz,  beep pulse=2Hz
  high:     state=250Hz, RGB=50Hz,  beep pulse=5Hz
  extreme:  state=500Hz, RGB=100Hz, beep pulse=10Hz
  overload: state=800Hz, RGB=150Hz, beep pulse=20Hz
  saturation: state=1200Hz, RGB=250Hz, beep pulse=50Hz
  recovery: state=1Hz

The raw monitor distinguishes no bytes, invalid-XOR bytes, partial frames, and
valid replies. SensorRecorder runs concurrently for both encoders.
EOF
}

OUTPUT_DIR="/dev/shm/ugripper_hmi_high_rate_$(timestamp_slug)"
STAGE_SEC=60
SATURATION_REPEAT=1
HMI_INACTIVE_MS=5500
ENCODER_MAX_GAP_MS=20
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
SENSOR_RECORDER_BIN_OVERRIDE=""
PYTHON_BIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --stage-sec) STAGE_SEC="$2"; shift 2 ;;
        --saturation-repeat) SATURATION_REPEAT="$2"; shift 2 ;;
        --hmi-inactive-ms) HMI_INACTIVE_MS="$2"; shift 2 ;;
        --encoder-max-gap-ms) ENCODER_MAX_GAP_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ugripper-root) UGRIPPER_ROOT="$2"; shift 2 ;;
        --sensor-recorder-bin) SENSOR_RECORDER_BIN_OVERRIDE="$2"; shift 2 ;;
        --python-bin) PYTHON_BIN_OVERRIDE="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

for value_name in STAGE_SEC SATURATION_REPEAT HMI_INACTIVE_MS ENCODER_MAX_GAP_MS; do
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
    ls -l /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder > "${dir}/links.txt" 2>&1 || true
    lsusb -t > "${dir}/lsusb-tree.txt" 2>&1 || true
    journalctl -k --since "${START_TIME}" --no-pager -o short-precise -l > "${dir}/kernel.log" 2>&1 || true
    command -v hws >/dev/null 2>&1 && hws > "${dir}/hws.txt" 2>&1 || true
}

stop_sensor() {
    if [[ -n "${SENSOR_PID}" ]] && kill -0 "${SENSOR_PID}" 2>/dev/null; then
        kill -INT "${SENSOR_PID}" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "${SENSOR_PID}" 2>/dev/null || break
            sleep 0.1
        done
        kill -TERM "${SENSOR_PID}" 2>/dev/null || true
        wait "${SENSOR_PID}" 2>/dev/null || true
    fi
    SENSOR_PID=""
}

restore_service() {
    stop_sensor
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        for _ in $(seq 1 30); do
            systemctl is-active --quiet "${SERVICE_NAME}" && break
            sleep 1
        done
        SERVICE_RESTORED=1
    fi
}
trap restore_service EXIT INT TERM

print_section "HMI High-rate Soak Configuration"
echo "output_dir=${OUTPUT_DIR}"
echo "stage_sec=${STAGE_SEC}"
echo "saturation_repeat=${SATURATION_REPEAT}"
echo "hmi_inactive_ms=${HMI_INACTIVE_MS}"
echo "encoder_max_gap_ms=${ENCODER_MAX_GAP_MS}"

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
"${PYTHON_BIN}" - "${STAGE_SEC}" "${HMI_INACTIVE_MS}" "${OUTPUT_DIR}" "${SATURATION_REPEAT}" > "${OUTPUT_DIR}/hmi_stress.log" 2>&1 <<'PY'
import errno
import json
import os
import selectors
import sys
import termios
import time
from pathlib import Path

stage_seconds = int(sys.argv[1])
inactive_ms = int(sys.argv[2])
output_dir = Path(sys.argv[3])
saturation_repeat = int(sys.argv[4])
stages = [
    ("baseline", 1, 0, 0),
    ("low", 50, 10, 1),
    ("medium", 100, 20, 2),
    ("high", 250, 50, 5),
    ("extreme", 500, 100, 10),
    ("overload", 800, 150, 20),
]
stages.extend((f"saturation_{index + 1}", 1200, 250, 50) for index in range(saturation_repeat))
stages.append(("recovery", 1, 0, 0))

def xor(data):
    value = 0
    for byte in data:
        value ^= byte
    return value

def standard(index, function, high=0, low=0):
    frame = bytearray((0x5A, 0x5A, index, function, high, low, 0))
    frame[-1] = xor(frame[:-1])
    return bytes(frame)

def rgb(red, green, blue):
    frame = bytearray((0x5A, 0x5A, 0x0F, red, green, blue, 0, 0))
    frame[6] = xor(frame[:6])
    return bytes(frame)

state_frame = standard(0x04, 0x02)
beep_on = standard(0x0E, 30, 0x03, 0xE8)
beep_off = standard(0x0E, 0, 0x03, 0xE8)

class Port:
    def __init__(self, side):
        self.side = side
        self.path = f"/dev/{side}_gripper"
        self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buffer = bytearray()
        self.opened_at = time.monotonic()
        self.last_raw_at = None
        self.last_valid_at = None
        self.timeout_active = False
        self.tx_state = 0
        self.tx_rgb = 0
        self.tx_beep = 0
        self.tx_errors = 0
        self.rx_bytes = 0
        self.valid_frames = 0
        self.beep_frames = 0
        self.key_frames = 0
        self.unknown_frames = 0
        self.invalid_xor = 0
        self.discarded_bytes = 0
        self.timeout_events = 0
        self.max_valid_age_ms = 0
        self.max_raw_age_ms = 0

    def send(self, frame, kind):
        try:
            written = os.write(self.fd, frame)
            if written != len(frame):
                self.tx_errors += 1
                return
            if kind == "state": self.tx_state += 1
            elif kind == "rgb": self.tx_rgb += 1
            else: self.tx_beep += 1
        except OSError as error:
            if error.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                self.tx_errors += 1

    def read(self, now):
        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            except OSError:
                self.tx_errors += 1
                break
            if not data:
                break
            self.rx_bytes += len(data)
            self.last_raw_at = now
            self.buffer.extend(data)
        self.parse(now)

    def parse(self, now):
        while len(self.buffer) >= 8:
            header = self.buffer.find(b"\xA5\xA5")
            if header < 0:
                self.discarded_bytes += len(self.buffer)
                self.buffer.clear()
                return
            if header:
                self.discarded_bytes += header
                del self.buffer[:header]
            if len(self.buffer) < 8:
                return
            frame = self.buffer[:8]
            if xor(frame[:7]) != frame[7]:
                self.invalid_xor += 1
                self.discarded_bytes += 1
                del self.buffer[0]
                continue
            del self.buffer[:8]
            self.valid_frames += 1
            self.last_valid_at = now
            if frame[2] == 0x04: self.beep_frames += 1
            elif frame[2] == 0x01: self.key_frames += 1
            else: self.unknown_frames += 1

    def ages(self, now):
        raw_base = self.last_raw_at if self.last_raw_at is not None else self.opened_at
        valid_base = self.last_valid_at if self.last_valid_at is not None else self.opened_at
        raw_age = int((now - raw_base) * 1000)
        valid_age = int((now - valid_base) * 1000)
        self.max_raw_age_ms = max(self.max_raw_age_ms, raw_age)
        self.max_valid_age_ms = max(self.max_valid_age_ms, valid_age)
        if valid_age >= inactive_ms and not self.timeout_active:
            self.timeout_active = True
            self.timeout_events += 1
            print(f"TIMEOUT side={self.side} valid_age_ms={valid_age} raw_age_ms={raw_age} rx_bytes={self.rx_bytes} valid_frames={self.valid_frames}", flush=True)
        elif valid_age < inactive_ms and self.timeout_active:
            self.timeout_active = False
            print(f"RECOVER side={self.side} valid_age_ms={valid_age}", flush=True)
        return raw_age, valid_age

ports = [Port("left"), Port("right")]
selector = selectors.DefaultSelector()
for port in ports:
    selector.register(port.fd, selectors.EVENT_READ, port)

stage_rows = []
try:
    for stage_index, (name, state_hz, rgb_hz, beep_hz) in enumerate(stages):
        start = time.monotonic()
        end = start + stage_seconds
        next_state = {port.side: start for port in ports}
        next_rgb = {port.side: start for port in ports}
        next_beep = {port.side: start for port in ports}
        beep_is_on = {port.side: False for port in ports}
        before = {port.side: dict(port.__dict__) for port in ports}
        last_report = start
        print(f"STAGE_START name={name} state_hz={state_hz} rgb_hz={rgb_hz} beep_hz={beep_hz}", flush=True)
        while True:
            now = time.monotonic()
            if now >= end:
                break
            for port in ports:
                if now >= next_state[port.side]:
                    port.send(state_frame, "state")
                    next_state[port.side] += 1.0 / state_hz
                    if next_state[port.side] < now - 0.1:
                        next_state[port.side] = now
                if rgb_hz and now >= next_rgb[port.side]:
                    tick = port.tx_rgb + stage_index * 37
                    port.send(rgb((tick * 17) % 200 + 20, (tick * 29) % 200 + 20, (tick * 43) % 200 + 20), "rgb")
                    next_rgb[port.side] += 1.0 / rgb_hz
                if beep_hz and now >= next_beep[port.side]:
                    beep_is_on[port.side] = not beep_is_on[port.side]
                    port.send(beep_on if beep_is_on[port.side] else beep_off, "beep")
                    next_beep[port.side] += 0.5 / beep_hz
            for key, _ in selector.select(0.0005):
                key.data.read(time.monotonic())
            now = time.monotonic()
            for port in ports:
                port.read(now)
                port.ages(now)
            if now - last_report >= 1.0:
                last_report = now
                for port in ports:
                    raw_age, valid_age = port.ages(now)
                    print(f"STATUS stage={name} side={port.side} raw_age_ms={raw_age} valid_age_ms={valid_age} tx_state={port.tx_state} tx_rgb={port.tx_rgb} tx_beep={port.tx_beep} rx_bytes={port.rx_bytes} valid_frames={port.valid_frames} invalid_xor={port.invalid_xor} partial_bytes={len(port.buffer)}", flush=True)
        for port in ports:
            port.send(beep_off, "beep")
        settle_end = time.monotonic() + 0.5
        while time.monotonic() < settle_end:
            for key, _ in selector.select(0.01):
                key.data.read(time.monotonic())
        for port in ports:
            prior = before[port.side]
            row = {
                "stage": name,
                "side": port.side,
                "state_hz": state_hz,
                "rgb_hz": rgb_hz,
                "beep_hz": beep_hz,
                "tx_state": port.tx_state - prior["tx_state"],
                "tx_rgb": port.tx_rgb - prior["tx_rgb"],
                "tx_beep": port.tx_beep - prior["tx_beep"],
                "tx_errors": port.tx_errors - prior["tx_errors"],
                "rx_bytes": port.rx_bytes - prior["rx_bytes"],
                "valid_frames": port.valid_frames - prior["valid_frames"],
                "invalid_xor": port.invalid_xor - prior["invalid_xor"],
                "discarded_bytes": port.discarded_bytes - prior["discarded_bytes"],
                "timeout_events": port.timeout_events - prior["timeout_events"],
                "max_valid_age_ms_total": port.max_valid_age_ms,
                "partial_bytes_end": len(port.buffer),
            }
            stage_rows.append(row)
            print("STAGE_RESULT " + json.dumps(row, sort_keys=True), flush=True)
finally:
    for port in ports:
        try: port.send(beep_off, "beep")
        except Exception: pass
        try: os.close(port.fd)
        except Exception: pass

with (output_dir / "hmi_stage_report.tsv").open("w") as output:
    columns = list(stage_rows[0])
    output.write("\t".join(columns) + "\n")
    for row in stage_rows:
        output.write("\t".join(str(row[column]) for column in columns) + "\n")

failed = any(row["timeout_events"] or row["tx_errors"] for row in stage_rows)
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
                yielded = True
                yield channel, message
        except Exception:
            if yielded: raise
        if yielded: return
        stream.seek(0)
        channels = {}
        for record in StreamReader(stream).records:
            name = type(record).__name__
            if name == "Channel": channels[record.id] = record
            elif name == "Message" and record.channel_id in channels: yield channels[record.channel_id], record

root = Path(sys.argv[1]); allowed = int(sys.argv[2]); thresholds = (2, 5, 10, 20, 100, 1000)
print("side\tcount\tspan_sec\trate_hz\tmax_gap_ms\tgt2ms\tgt5ms\tgt10ms\tgt20ms\tgt100ms\tgt1000ms\tresult")
failed = False
for side in ("left", "right"):
    timestamps = [int(getattr(message, "log_time", getattr(message, "logTime", 0))) for _, message in messages(root / f"sensor_{side}.mcap")]
    gaps = [current - previous for previous, current in zip(timestamps, timestamps[1:])]
    span = timestamps[-1] - timestamps[0] if len(timestamps) > 1 else 0
    rate = (len(timestamps) - 1) / (span / 1e9) if span else 0.0
    maximum = max(gaps, default=0) / 1e6
    counts = [sum(gap > threshold * 1_000_000 for gap in gaps) for threshold in thresholds]
    result = "pass" if timestamps and maximum <= allowed else "fail"
    failed = failed or result == "fail"
    print(f"{side}\t{len(timestamps)}\t{span/1e9:.3f}\t{rate:.3f}\t{maximum:.3f}\t" + "\t".join(map(str, counts)) + f"\t{result}")
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
cat "${OUTPUT_DIR}/hmi_stage_report.tsv"
cat "${OUTPUT_DIR}/encoder_gap_report.tsv"
echo "result_dir=${OUTPUT_DIR}"

[[ "${HMI_EXIT}" == "0" && "${ENCODER_EXIT}" == "0" ]]
