#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


PRIORITY_REASONS = [
    "missing_required_suite",
    "suite_failed",
    "mixed_package_versions",
    "ok",
]


def choose_reason(reasons):
    if not reasons:
        return "ok"
    for candidate in PRIORITY_REASONS:
        if candidate in reasons:
            return candidate
    return sorted(reasons)[0]


def parse_suite_arg(raw):
    if "=" not in raw:
        raise argparse.ArgumentTypeError(f"invalid SUITE=PATH: {raw}")
    suite, path = raw.split("=", 1)
    suite = suite.strip()
    path = path.strip()
    if not suite or not path:
        raise argparse.ArgumentTypeError(f"invalid SUITE=PATH: {raw}")
    return suite, path


def parse_version_arg(raw):
    if "=" not in raw:
        raise argparse.ArgumentTypeError(f"invalid SUITE=VERSION: {raw}")
    suite, version = raw.split("=", 1)
    suite = suite.strip()
    version = version.strip()
    if not suite or not version:
        raise argparse.ArgumentTypeError(f"invalid SUITE=VERSION: {raw}")
    return suite, version


def load_json(path):
    report_path = Path(path)
    data = json.loads(report_path.read_text(encoding="utf-8"))
    return report_path, data


def suite_validation_reason(data):
    if "validation_reason" in data:
        return data.get("validation_reason") or "unknown"
    return "ok" if data.get("ok") else "unknown"


def build_suite_summary(suite_name, path, version):
    report_path, data = load_json(path)
    return {
        "suite": suite_name,
        "path": str(report_path),
        "ok": bool(data.get("ok", False)),
        "validation_reason": suite_validation_reason(data),
        "package_version": version,
    }


def summarize_rollout(suites, required_suites):
    reasons = []
    suite_names = {item["suite"] for item in suites}
    missing_suites = [name for name in required_suites if name not in suite_names]
    if missing_suites:
        reasons.append("missing_required_suite")

    all_ok = True
    versions = []
    for suite in suites:
        if not suite["ok"]:
            all_ok = False
            reasons.append("suite_failed")
        if suite["package_version"]:
            versions.append(suite["package_version"])

    unique_versions = sorted(set(versions))
    uniform_package_version = len(unique_versions) <= 1 and len(versions) == len(suites)
    if not uniform_package_version:
        reasons.append("mixed_package_versions")

    release_gate_ready = all_ok and not missing_suites and uniform_package_version
    return {
        "ok": all_ok,
        "release_gate_ready": release_gate_ready,
        "validation_reason": choose_reason(reasons),
        "required_suites": required_suites,
        "missing_suites": missing_suites,
        "uniform_package_version": uniform_package_version,
        "package_versions": unique_versions,
        "suites": suites,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Summarize board rollout suite outputs into a single release-gate style report."
    )
    parser.add_argument(
        "--suite-report",
        action="append",
        default=[],
        type=parse_suite_arg,
        help="Suite summary in SUITE=PATH form",
    )
    parser.add_argument(
        "--suite-version",
        action="append",
        default=[],
        type=parse_version_arg,
        help="Package version for a suite in SUITE=VERSION form",
    )
    parser.add_argument(
        "--require-suite",
        action="append",
        default=[],
        help="Required suite name; may be repeated",
    )
    parser.add_argument("--json-out", help="Optional JSON output path")
    args = parser.parse_args()

    versions = {suite: version for suite, version in args.suite_version}
    suites = [
        build_suite_summary(suite_name, path, versions.get(suite_name))
        for suite_name, path in args.suite_report
    ]
    summary = summarize_rollout(suites, args.require_suite)

    print(
        f"[SUMMARY] ok={summary['ok']} "
        f"release_gate_ready={summary['release_gate_ready']} "
        f"validation_reason={summary['validation_reason']}"
    )

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
