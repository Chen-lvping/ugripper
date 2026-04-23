#!/usr/bin/env python3

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


PAIR_RE = re.compile(r"(\w+)=([^\s]+)")


def parse_diag_fields(line):
    marker = "[GRIPPER_DIAG]"
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


def parse_count_requirement(raw_value, option_name):
    if ":" not in raw_value:
        raise argparse.ArgumentTypeError(
            f"{option_name} requires COMMAND:COUNT or FIELD:COUNT format, got: {raw_value}"
        )
    key, count_text = raw_value.split(":", 1)
    key = key.strip()
    if not key:
        raise argparse.ArgumentTypeError(f"{option_name} requires a non-empty key")
    try:
        count = int(count_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"{option_name} count must be an integer, got: {count_text}"
        ) from exc
    return key, count


def main():
    parser = argparse.ArgumentParser(
        description="Parse [GRIPPER_DIAG] lines and summarize command retries/timeouts/failures."
    )
    parser.add_argument("--input", required=True, help="Log file path, or - for stdin")
    parser.add_argument("--json-out", help="Optional JSON output path")
    parser.add_argument("--allow-timeouts", action="store_true")
    parser.add_argument("--allow-failures", action="store_true")
    parser.add_argument("--require-command", action="append", default=[])
    parser.add_argument(
        "--require-command-retries-at-least",
        action="append",
        default=[],
        metavar="COMMAND:COUNT",
        help="Require aggregated retries for a command to be at least COUNT",
    )
    parser.add_argument(
        "--require-command-recoveries-at-least",
        action="append",
        default=[],
        metavar="COMMAND:COUNT",
        help="Require aggregated recoveries for a command to be at least COUNT",
    )
    parser.add_argument(
        "--require-io-total-at-least",
        action="append",
        default=[],
        metavar="FIELD:COUNT",
        help="Require io_summary total FIELD to be at least COUNT",
    )
    args = parser.parse_args()

    lines = load_lines(args.input)
    command_rows = []
    io_rows = []
    aggregated = defaultdict(
        lambda: {
            "invocations": 0,
            "sends": 0,
            "acks": 0,
            "timeouts": 0,
            "retries": 0,
            "recoveries": 0,
            "successes": 0,
            "failures": 0,
            "ports": set(),
        }
    )
    io_totals = {
        "tx_led": 0,
        "tx_beep": 0,
        "tx_state_req": 0,
        "rx_frames": 0,
        "rx_key_reports": 0,
        "rx_beep_states": 0,
        "io_failures": 0,
        "connect_success": 0,
        "reconnect_success": 0,
        "exclusive_commands": 0,
        "exclusive_acks": 0,
        "exclusive_timeouts": 0,
        "exclusive_retries": 0,
        "exclusive_failures": 0,
        "exclusive_recoveries": 0,
    }

    for line in lines:
        fields = parse_diag_fields(line)
        if not fields:
            continue
        category = fields.get("category", "")
        if category == "command_summary":
            command_rows.append(fields)
            command = fields.get("command", "unknown")
            entry = aggregated[command]
            entry["invocations"] += 1
            entry["sends"] += int(fields.get("sends", "0"))
            entry["acks"] += int(fields.get("acks", "0"))
            entry["timeouts"] += int(fields.get("timeouts", "0"))
            entry["retries"] += int(fields.get("retries", "0"))
            entry["recoveries"] += int(fields.get("recoveries", "0"))
            if fields.get("status") == "success":
                entry["successes"] += 1
            else:
                entry["failures"] += 1
            if "port" in fields:
                entry["ports"].add(fields["port"])
        elif category == "io_summary":
            io_rows.append(fields)
            io_totals["tx_led"] += int(fields.get("tx_led", "0"))
            io_totals["tx_beep"] += int(fields.get("tx_beep", "0"))
            io_totals["tx_state_req"] += int(fields.get("tx_state_req", "0"))
            io_totals["rx_frames"] += int(fields.get("rx_frames", "0"))
            io_totals["rx_key_reports"] += int(fields.get("rx_key_reports", "0"))
            io_totals["rx_beep_states"] += int(fields.get("rx_beep_states", "0"))
            io_totals["io_failures"] += int(fields.get("io_failures", "0"))
            io_totals["connect_success"] += int(fields.get("connect_success", "0"))
            io_totals["reconnect_success"] += int(fields.get("reconnect_success", "0"))
            io_totals["exclusive_commands"] += int(fields.get("exclusive_commands", "0"))
            io_totals["exclusive_acks"] += int(fields.get("exclusive_acks", "0"))
            io_totals["exclusive_timeouts"] += int(fields.get("exclusive_timeouts", "0"))
            io_totals["exclusive_retries"] += int(fields.get("exclusive_retries", "0"))
            io_totals["exclusive_failures"] += int(fields.get("exclusive_failures", "0"))
            io_totals["exclusive_recoveries"] += int(fields.get("exclusive_recoveries", "0"))

    failures = []
    for required in args.require_command:
        if aggregated[required]["invocations"] == 0:
            failures.append(f"missing required command summary: {required}")

    for raw_requirement in args.require_command_retries_at_least:
        command, minimum = parse_count_requirement(raw_requirement, "--require-command-retries-at-least")
        actual = aggregated[command]["retries"]
        if actual < minimum:
            failures.append(
                f"command retries below threshold: {command} actual={actual} expected>={minimum}"
            )

    for raw_requirement in args.require_command_recoveries_at_least:
        command, minimum = parse_count_requirement(raw_requirement, "--require-command-recoveries-at-least")
        actual = aggregated[command]["recoveries"]
        if actual < minimum:
            failures.append(
                f"command recoveries below threshold: {command} actual={actual} expected>={minimum}"
            )

    for raw_requirement in args.require_io_total_at_least:
        field, minimum = parse_count_requirement(raw_requirement, "--require-io-total-at-least")
        if field not in io_totals:
            failures.append(f"unknown io total field: {field}")
            continue
        actual = io_totals[field]
        if actual < minimum:
            failures.append(
                f"io total below threshold: {field} actual={actual} expected>={minimum}"
            )

    total_timeouts = sum(item["timeouts"] for item in aggregated.values())
    total_failures = sum(item["failures"] for item in aggregated.values())
    if total_timeouts > 0 and not args.allow_timeouts:
        failures.append(f"command timeouts detected: {total_timeouts}")
    if total_failures > 0 and not args.allow_failures:
        failures.append(f"command failures detected: {total_failures}")
    if io_totals["exclusive_timeouts"] > 0 and not args.allow_timeouts:
        failures.append(f"exclusive command timeouts detected: {io_totals['exclusive_timeouts']}")
    if io_totals["exclusive_failures"] > 0 and not args.allow_failures:
        failures.append(f"exclusive command failures detected: {io_totals['exclusive_failures']}")
    if io_totals["io_failures"] > 0 and not args.allow_failures:
        failures.append(f"I/O failures detected: {io_totals['io_failures']}")

    command_summary = {}
    for command, item in aggregated.items():
        command_summary[command] = {
            "invocations": item["invocations"],
            "sends": item["sends"],
            "acks": item["acks"],
            "timeouts": item["timeouts"],
            "retries": item["retries"],
            "recoveries": item["recoveries"],
            "successes": item["successes"],
            "failures": item["failures"],
            "ports": sorted(item["ports"]),
        }

    summary = {
        "ok": not failures,
        "input": args.input,
        "command_summary_count": len(command_rows),
        "io_summary_count": len(io_rows),
        "commands": command_summary,
        "io_totals": io_totals,
        "failures": failures,
    }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    for command, item in sorted(command_summary.items()):
        print(
            f"[INFO] {command} invocations={item['invocations']} sends={item['sends']} "
            f"acks={item['acks']} timeouts={item['timeouts']} retries={item['retries']} "
            f"recoveries={item['recoveries']} failures={item['failures']}"
        )

    if failures:
        for failure in failures:
            print(f"[FAIL] {failure}")
        return 1

    print("[PASS] no blocking gripper command failures detected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
