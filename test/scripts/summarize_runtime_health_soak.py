#!/usr/bin/env python3

import argparse
import json
import re
from collections import Counter
from pathlib import Path


HEALTH_FAULT_RE = re.compile(r"hardware health fault \(([^)]+)\):")
HMI_DIAG_HEALTH_FAULT_RE = re.compile(r"\[HMI_DIAG\]\s+category=health_fault\s+key=([^\s]+)\s+led_state=([^\s]+)")
HMI_DIAG_HEALTH_RECOVERED_RE = re.compile(r"\[HMI_DIAG\]\s+category=health_recovered\s+recording=([01])")
VALIDATION_RE = re.compile(r"validation phase end: .* valid=(true|false)")
EPISODE_FAILED_RE = re.compile(r"episode validation failed:")
RECORDING_STARTED_RE = re.compile(r"recording started:")
SHORT_UP_RE = re.compile(r"\[HMI_DIAG\]\s+category=button_event\s+event=ShortUpPressed\b")
SHORT_DOWN_RE = re.compile(r"\[HMI_DIAG\]\s+category=button_event\s+event=ShortDownPressed\b")
SHUTDOWN_PROMPT_RE = re.compile(r"\[HMI_DIAG\]\s+category=button_event\s+event=ShutdownPromptRequested\b")


def load_lines(path: str):
    return Path(path).read_text(encoding="utf-8").splitlines()


def main():
    parser = argparse.ArgumentParser(
        description="Summarize runtime health fault / recovery / validation signals from a service log."
    )
    parser.add_argument("--input", required=True, help="Service log path")
    parser.add_argument("--json-out", help="Optional JSON output path")
    parser.add_argument("--max-health-fault", action="append", default=[], help="KEY=N threshold")
    parser.add_argument("--max-total-health-faults", type=int, default=None)
    parser.add_argument("--min-validations", type=int, default=None)
    parser.add_argument("--min-recording-starts", type=int, default=None)
    args = parser.parse_args()

    max_health_fault = {}
    failures = []
    for raw in args.max_health_fault:
        key, sep, count = raw.partition("=")
        if not sep:
            failures.append(f"invalid --max-health-fault value: {raw}")
            continue
        try:
            max_health_fault[key] = int(count)
        except ValueError:
            failures.append(f"invalid fault threshold integer: {raw}")

    lines = load_lines(args.input)
    health_faults = Counter()
    health_fault_led_pairs = Counter()
    validation_counts = Counter()
    recording_starts = 0
    recovered_count = 0
    recovered_while_recording_count = 0
    short_up_count = 0
    short_down_count = 0
    shutdown_prompt_count = 0

    for line in lines:
        match = HEALTH_FAULT_RE.search(line)
        if match:
            health_faults[match.group(1)] += 1

        match = HMI_DIAG_HEALTH_FAULT_RE.search(line)
        if match:
            health_fault_led_pairs[f"{match.group(1)}:{match.group(2)}"] += 1

        match = HMI_DIAG_HEALTH_RECOVERED_RE.search(line)
        if match:
            recovered_count += 1
            if match.group(1) == "1":
                recovered_while_recording_count += 1

        match = VALIDATION_RE.search(line)
        if match:
            validation_counts[match.group(1)] += 1

        if EPISODE_FAILED_RE.search(line):
            validation_counts["episode_validation_failed"] += 1

        if RECORDING_STARTED_RE.search(line):
            recording_starts += 1
        if SHORT_UP_RE.search(line):
            short_up_count += 1
        if SHORT_DOWN_RE.search(line):
            short_down_count += 1
        if SHUTDOWN_PROMPT_RE.search(line):
            shutdown_prompt_count += 1

    total_health_faults = sum(health_faults.values())

    if args.max_total_health_faults is not None and total_health_faults > args.max_total_health_faults:
        failures.append(
            f"total health faults {total_health_faults} exceed limit {args.max_total_health_faults}"
        )

    for key, limit in max_health_fault.items():
        count = health_faults.get(key, 0)
        if count > limit:
            failures.append(f"health fault {key} count {count} exceeds limit {limit}")

    if args.min_validations is not None and validation_counts.get("true", 0) < args.min_validations:
        failures.append(
            f"valid=true count {validation_counts.get('true', 0)} below minimum {args.min_validations}"
        )

    if args.min_recording_starts is not None and recording_starts < args.min_recording_starts:
        failures.append(
            f"recording started count {recording_starts} below minimum {args.min_recording_starts}"
        )

    summary = {
        "ok": not failures,
        "input": args.input,
        "health_faults": dict(health_faults),
        "health_fault_led_pairs": dict(health_fault_led_pairs),
        "total_health_faults": total_health_faults,
        "health_recovered_count": recovered_count,
        "health_recovered_while_recording_count": recovered_while_recording_count,
        "validation_counts": dict(validation_counts),
        "recording_started_count": recording_starts,
        "button_event_counts": {
            "ShortUpPressed": short_up_count,
            "ShortDownPressed": short_down_count,
            "ShutdownPromptRequested": shutdown_prompt_count,
        },
        "failures": failures,
    }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"[INFO] total_health_faults={total_health_faults}")
    for key, count in sorted(health_faults.items()):
        print(f"[INFO] health_fault {key} count={count}")
    print(f"[INFO] health_recovered count={recovered_count}")
    print(f"[INFO] health_recovered_while_recording count={recovered_while_recording_count}")
    print(f"[INFO] recording_started count={recording_starts}")
    for key, count in sorted(validation_counts.items()):
        print(f"[INFO] validation {key} count={count}")
    print(f"[INFO] button ShortUpPressed count={short_up_count}")
    print(f"[INFO] button ShortDownPressed count={short_down_count}")
    print(f"[INFO] button ShutdownPromptRequested count={shutdown_prompt_count}")

    if failures:
        for failure in failures:
            print(f"[FAIL] {failure}")
        return 1

    print("[PASS] runtime health soak summary within configured thresholds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
