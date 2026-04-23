#!/usr/bin/env python3

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path


PAIR_RE = re.compile(r"(\w+)=([^\s]+)")


def parse_diag_fields(line):
    marker = "[HMI_DIAG]"
    if marker not in line:
        return None
    payload = line.split(marker, 1)[1].strip()
    fields = {}
    for match in PAIR_RE.finditer(payload):
        fields[match.group(1)] = match.group(2)
    return fields


def load_lines(path):
    if path == "-":
        return sys.stdin.read().splitlines()
    return Path(path).read_text(encoding="utf-8").splitlines()


def main():
    parser = argparse.ArgumentParser(
        description="Parse [HMI_DIAG] lines and check button-event / LED-target consistency."
    )
    parser.add_argument("--input", required=True, help="Log file path, or - for stdin")
    parser.add_argument("--json-out", help="Optional JSON output path")
    parser.add_argument("--require-event", action="append", default=[])
    parser.add_argument("--require-led-state", action="append", default=[])
    parser.add_argument("--require-health-fault", action="append", default=[])
    parser.add_argument(
        "--require-health-fault-led",
        action="append",
        default=[],
        help="Require a health fault key to advertise the expected led state, format KEY:LED",
    )
    args = parser.parse_args()

    lines = load_lines(args.input)
    category_counts = Counter()
    event_counts = Counter()
    led_state_counts = Counter()
    health_fault_counts = Counter()
    health_fault_led_expectations = Counter()
    health_fault_led_pairs = Counter()
    button_raw_count = 0

    for line in lines:
        fields = parse_diag_fields(line)
        if not fields:
            continue
        category = fields.get("category", "unknown")
        category_counts[category] += 1
        if category == "button_raw":
            button_raw_count += 1
        elif category == "button_event":
            event_counts[fields.get("event", "unknown")] += 1
        elif category == "led_target":
            led_state_counts[fields.get("state", "unknown")] += 1
        elif category == "health_fault":
            key = fields.get("key", "unknown")
            health_fault_counts[key] += 1
            expected_led = fields.get("led_state")
            if expected_led:
                health_fault_led_expectations[expected_led] += 1
                health_fault_led_pairs[f"{key}:{expected_led}"] += 1

    failures = []
    for required in args.require_event:
        if event_counts[required] == 0:
            failures.append(f"missing required button event: {required}")
    for required in args.require_led_state:
        if led_state_counts[required] == 0:
            failures.append(f"missing required led state: {required}")
    for required in args.require_health_fault:
        if health_fault_counts[required] == 0:
            failures.append(f"missing required health fault: {required}")
    for required in args.require_health_fault_led:
        key, sep, led = required.partition(":")
        if not sep or not key or not led:
            failures.append(f"invalid --require-health-fault-led value: {required}")
            continue
        if health_fault_led_pairs[f"{key}:{led}"] == 0:
            failures.append(f"missing required health fault led mapping: {key}:{led}")

    if sum(event_counts.values()) > 0 and button_raw_count == 0:
        failures.append("button events exist but no button_raw snapshots were logged")

    if event_counts["ShutdownRequested"] > event_counts["ShutdownPromptRequested"]:
        failures.append("ShutdownRequested count exceeds ShutdownPromptRequested count")

    if sum(health_fault_counts.values()) > 0 and not health_fault_led_expectations:
        failures.append("health faults were logged but no led_state expectation was provided")

    for expected_led in sorted(health_fault_led_expectations):
        if led_state_counts[expected_led] == 0:
            failures.append(f"health fault expected led state not logged: {expected_led}")

    summary = {
        "ok": not failures,
        "input": args.input,
        "categories": dict(category_counts),
        "button_events": dict(event_counts),
        "led_targets": dict(led_state_counts),
        "health_faults": dict(health_fault_counts),
        "health_fault_led_expectations": dict(health_fault_led_expectations),
        "health_fault_led_pairs": dict(health_fault_led_pairs),
        "failures": failures,
    }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"[INFO] button_raw={button_raw_count}")
    for event_name, count in sorted(event_counts.items()):
        print(f"[INFO] event {event_name} count={count}")
    for led_state, count in sorted(led_state_counts.items()):
        print(f"[INFO] led_state {led_state} count={count}")
    for pair, count in sorted(health_fault_led_pairs.items()):
        print(f"[INFO] health_fault_led {pair} count={count}")

    if failures:
        for failure in failures:
            print(f"[FAIL] {failure}")
        return 1

    print("[PASS] no blocking HMI/button consistency failures detected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
