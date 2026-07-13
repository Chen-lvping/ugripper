#!/usr/bin/env python3

import csv
import math
import os
import re
import selectors
import signal
import subprocess
import sys
import termios
import time
from pathlib import Path


MODE = sys.argv[1]
OUTPUT_DIR = Path(sys.argv[2])
CYCLES = int(sys.argv[3])
PRE_SEC = int(sys.argv[4])
RECORD_SEC = int(sys.argv[5])
POST_SEC = int(sys.argv[6])
OFFSETS_MS = [int(value) for value in sys.argv[7].split(",") if value]
INACTIVE_MS = int(sys.argv[8])
SENSOR_RECORDER_BIN = sys.argv[9]

STATE_INTERVAL = 1.0
LED_RESEND_INTERVAL = 0.250
LOOP_INTERVAL = 0.001


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


STATE_REQUEST = standard(0x04, 0x02)


def render_pulse(now_ms, durations, color):
    cycle = sum(durations) + 300 * (len(durations) - 1) + 1200
    phase = now_ms % cycle
    for index, duration in enumerate(durations):
        if phase < duration:
            return color
        phase -= duration
        gap = 1200 if index + 1 == len(durations) else 300
        if phase < gap:
            return (0, 0, 0)
        phase -= gap
    return (0, 0, 0)


def render_color(side, state, now):
    now_ms = int(now * 1000)
    if state == "recording":
        return (0, 255, 0) if now_ms % 1000 < 500 else (0, 0, 0)
    if side == "right":
        return render_pulse(now_ms, (700, 220, 220), (255, 110, 0))
    phase = (now_ms % 4500) / 4500.0
    pulse = 0.5 + 0.5 * math.cos(2.0 * math.pi * phase)
    level = round(120.0 * pulse * pulse * pulse)
    return (0, level, (12 * level) // 150)


class HmiPort:
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
        now = time.monotonic()
        self.opened_at = now
        self.last_valid_at = None
        self.last_raw_at = None
        self.next_state_at = now
        self.last_led_at = 0.0
        self.last_color = None
        self.buffer = bytearray()
        self.tx_state = 0
        self.tx_led = 0
        self.tx_errors = 0
        self.rx_bytes = 0
        self.valid_frames = 0
        self.beep_frames = 0
        self.key_frames = 0
        self.invalid_xor = 0
        self.discarded_bytes = 0
        self.timeout_events = 0
        self.timeout_active = False
        self.max_age_ms = 0

    def counters(self):
        return {
            "tx_state": self.tx_state,
            "tx_led": self.tx_led,
            "tx_errors": self.tx_errors,
            "rx_bytes": self.rx_bytes,
            "valid_frames": self.valid_frames,
            "invalid_xor": self.invalid_xor,
            "discarded_bytes": self.discarded_bytes,
            "timeout_events": self.timeout_events,
        }

    def write(self, frame, kind):
        try:
            written = os.write(self.fd, frame)
        except OSError:
            self.tx_errors += 1
            return
        if written != len(frame):
            self.tx_errors += 1
            return
        if kind == "state":
            self.tx_state += 1
        else:
            self.tx_led += 1

    def service_tx(self, now, state):
        if now >= self.next_state_at:
            self.write(STATE_REQUEST, "state")
            self.next_state_at += STATE_INTERVAL
            if self.next_state_at < now:
                self.next_state_at = now + STATE_INTERVAL
        color = render_color(self.side, state, now)
        if color != self.last_color or now - self.last_led_at >= LED_RESEND_INTERVAL:
            self.write(rgb(*color), "led")
            self.last_color = color
            self.last_led_at = now

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
            self.last_raw_at = now
            self.rx_bytes += len(data)
            self.buffer.extend(data)
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
            if frame[2] == 0x04:
                self.beep_frames += 1
            elif frame[2] == 0x01:
                self.key_frames += 1

    def observe_age(self, now, label):
        base = self.last_valid_at if self.last_valid_at is not None else self.opened_at
        age_ms = int((now - base) * 1000)
        self.max_age_ms = max(self.max_age_ms, age_ms)
        if age_ms >= INACTIVE_MS and not self.timeout_active:
            self.timeout_active = True
            self.timeout_events += 1
            print(
                f"TIMEOUT label={label} side={self.side} age_ms={age_ms} "
                f"tx_state={self.tx_state} tx_led={self.tx_led} rx_bytes={self.rx_bytes} "
                f"valid_frames={self.valid_frames} invalid_xor={self.invalid_xor}",
                flush=True,
            )
        elif age_ms < INACTIVE_MS and self.timeout_active:
            self.timeout_active = False
            print(f"RECOVER label={label} side={self.side} age_ms={age_ms}", flush=True)
        return age_ms

    def close(self):
        os.close(self.fd)


class SensorSession:
    def __init__(self, cycle_dir):
        self.cycle_dir = cycle_dir
        self.log_path = cycle_dir / "sensor_recorder.log"
        self.log_stream = None
        self.process = None
        self.started_at = None
        self.exit_code = None

    def start(self):
        self.cycle_dir.mkdir(parents=True, exist_ok=True)
        self.log_stream = self.log_path.open("wb")
        self.started_at = time.monotonic()
        self.process = subprocess.Popen(
            [SENSOR_RECORDER_BIN, str(self.cycle_dir)],
            stdout=self.log_stream,
            stderr=subprocess.STDOUT,
        )

    def stop(self):
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        self.exit_code = self.process.returncode
        self.log_stream.close()

    def stats(self):
        text = self.log_path.read_text(errors="replace") if self.log_path.exists() else ""
        values = {}
        for side in ("left", "right"):
            match = re.search(
                rf"\[SensorStats-encoder_{side}\].*received_samples=(\d+), emitted_messages=(\d+)", text
            )
            values[f"sensor_{side}_received"] = int(match.group(1)) if match else 0
            values[f"sensor_{side}_emitted"] = int(match.group(2)) if match else 0
        return values


ports = [HmiPort("left"), HmiPort("right")]
selector = selectors.DefaultSelector()
for port in ports:
    selector.register(port.fd, selectors.EVENT_READ, port)


def service_until(deadline, state, label, pending_sensor=None, sensor_at=None):
    sensor = pending_sensor
    while time.monotonic() < deadline:
        now = time.monotonic()
        if sensor is not None and sensor.process is None and sensor_at is not None and now >= sensor_at:
            sensor.start()
            print(f"SENSOR_START label={label} steady={sensor.started_at:.6f}", flush=True)
        for port in ports:
            port.service_tx(now, state)
        for key, _ in selector.select(LOOP_INTERVAL):
            key.data.read(time.monotonic())
        now = time.monotonic()
        for port in ports:
            port.read(now)
            port.observe_age(now, label)
    return sensor


offsets = [0] if MODE in ("combo", "led-only") else OFFSETS_MS
rows = []
persistent_sensor = None
failed = False

try:
    service_until(time.monotonic() + 2.0, "pre", "warmup")
    if MODE == "led-only":
        persistent_sensor = SensorSession(OUTPUT_DIR / "sensor_cycles" / "persistent")
        persistent_sensor.start()
        service_until(time.monotonic() + 2.0, "pre", "persistent_encoder_warmup")

    for offset_ms in offsets:
        for cycle in range(1, CYCLES + 1):
            label = f"offset_{offset_ms}_cycle_{cycle}"
            cycle_dir = OUTPUT_DIR / "sensor_cycles" / f"offset_{offset_ms}" / f"cycle_{cycle:04d}"
            sensor = persistent_sensor if MODE == "led-only" else SensorSession(cycle_dir)
            before = {port.side: port.counters() for port in ports}
            cycle_max_age = {port.side: 0 for port in ports}

            transition_at = time.monotonic() + PRE_SEC
            sensor_at = transition_at + offset_ms / 1000.0
            if MODE == "led-only":
                sensor_at = None
            elif sensor_at < time.monotonic():
                sensor_at = time.monotonic()

            service_until(transition_at, "pre", label, sensor, sensor_at)
            actual_transition = time.monotonic()
            print(
                f"RECORDING_TRANSITION label={label} steady={actual_transition:.6f} "
                f"sensor_target={sensor_at if sensor_at is not None else 0:.6f}",
                flush=True,
            )
            service_until(actual_transition + RECORD_SEC, "recording", label, sensor, sensor_at)

            if MODE != "led-only":
                sensor.stop()
            service_until(time.monotonic() + POST_SEC, "pre", label)

            row = {
                "mode": MODE,
                "offset_ms": offset_ms,
                "cycle": cycle,
                "transition_steady": f"{actual_transition:.6f}",
                "sensor_start_delta_ms": "",
                "sensor_exit": "" if MODE == "led-only" else sensor.exit_code,
            }
            if MODE != "led-only" and sensor.started_at is not None:
                row["sensor_start_delta_ms"] = f"{(sensor.started_at - actual_transition) * 1000.0:.3f}"
            for port in ports:
                current = port.counters()
                prior = before[port.side]
                side = port.side
                for key in current:
                    row[f"{side}_{key}"] = current[key] - prior[key]
                base = port.last_valid_at if port.last_valid_at is not None else port.opened_at
                row[f"{side}_end_age_ms"] = int((time.monotonic() - base) * 1000)
                row[f"{side}_max_age_ms_total"] = port.max_age_ms
            if MODE != "led-only":
                row.update(sensor.stats())
            else:
                row.update({
                    "sensor_left_received": 0,
                    "sensor_left_emitted": 0,
                    "sensor_right_received": 0,
                    "sensor_right_emitted": 0,
                })
            rows.append(row)
            cycle_failed = any(row[f"{side}_timeout_events"] or row[f"{side}_tx_errors"] for side in ("left", "right"))
            if MODE != "led-only":
                cycle_failed = cycle_failed or sensor.exit_code != 0
                cycle_failed = cycle_failed or row["sensor_left_received"] == 0 or row["sensor_right_received"] == 0
            failed = failed or cycle_failed
            print(
                f"CYCLE_RESULT label={label} result={'fail' if cycle_failed else 'pass'} "
                f"left_timeout={row['left_timeout_events']} right_timeout={row['right_timeout_events']} "
                f"left_rx={row['left_valid_frames']} right_rx={row['right_valid_frames']}",
                flush=True,
            )
finally:
    if persistent_sensor is not None:
        persistent_sensor.stop()
    for port in ports:
        port.close()

if persistent_sensor is not None:
    persistent_stats = persistent_sensor.stats()
    for row in rows:
        row.update(persistent_stats)

with (OUTPUT_DIR / "cycle_report.tsv").open("w", newline="") as output:
    writer = csv.DictWriter(output, fieldnames=rows[0].keys(), delimiter="\t")
    writer.writeheader()
    writer.writerows(rows)

with (OUTPUT_DIR / "hmi_report.tsv").open("w", newline="") as output:
    writer = csv.writer(output, delimiter="\t")
    writer.writerow(("side", "tx_state", "tx_led", "tx_errors", "rx_bytes", "valid_frames", "invalid_xor", "discarded_bytes", "timeout_events", "max_age_ms", "partial_bytes"))
    for port in ports:
        writer.writerow((port.side, port.tx_state, port.tx_led, port.tx_errors, port.rx_bytes, port.valid_frames, port.invalid_xor, port.discarded_bytes, port.timeout_events, port.max_age_ms, len(port.buffer)))

raise SystemExit(1 if failed else 0)
