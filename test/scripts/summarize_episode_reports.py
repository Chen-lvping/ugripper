#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


PRIORITY_ORDER = [
    "sensor_span_gap_too_large",
    "video_span_gap_too_large",
    "video_decode_error",
    "video_frame_count_error",
    "video_frame_count_missing",
    "video_low_frame_window",
    "sensor_mcap_invalid",
    "gripper_log_invalid",
    "hmi_log_invalid",
]


def choose_primary_reason(reasons):
    if not reasons:
        return "ok"
    for candidate in PRIORITY_ORDER:
        if candidate in reasons:
            return candidate
    return sorted(reasons)[0]


def load_report(path):
    report_path = Path(path)
    return {
        "name": report_path.stem,
        "path": str(report_path),
        "data": json.loads(report_path.read_text(encoding="utf-8")),
    }


def summarize_sensor_reports(reports, max_span_gap_ns=None):
    summary = {
        "ok": True,
        "reports": [],
        "cross_alignment": None,
        "validation_reasons": [],
    }
    if not reports:
        return summary

    reference_span_ns = 0
    for report in reports:
        data = report["data"]
        report_summary = {
            "name": report["name"],
            "path": report["path"],
            "ok": bool(data.get("ok", False)),
            "reference_span_ns": int(data.get("reference_span_ns", 0) or 0),
            "failures": list(data.get("failures", [])),
        }
        summary["reports"].append(report_summary)
        reference_span_ns = max(reference_span_ns, report_summary["reference_span_ns"])
        if not report_summary["ok"]:
            summary["ok"] = False
            summary["validation_reasons"].append("sensor_mcap_invalid")

    if len(summary["reports"]) >= 2:
        aligned_reports = []
        alignment_ok = True
        for report in summary["reports"]:
            gap_ns = max(0, reference_span_ns - report["reference_span_ns"])
            aligned_reports.append(
                {
                    "name": report["name"],
                    "path": report["path"],
                    "reference_span_ns": report["reference_span_ns"],
                    "gap_ns": gap_ns,
                }
            )
            if max_span_gap_ns is not None and gap_ns > max_span_gap_ns:
                alignment_ok = False

        summary["cross_alignment"] = {
            "ok": alignment_ok,
            "reference_span_ns": reference_span_ns,
            "max_span_gap_ns": max_span_gap_ns,
            "reports": aligned_reports,
        }
        if not alignment_ok:
            summary["ok"] = False
            summary["validation_reasons"].append("sensor_span_gap_too_large")

    return summary


def summarize_video_reports(reports, max_span_gap_sec=None):
    summary = {
        "ok": True,
        "reports": [],
        "cross_alignment": None,
        "validation_reasons": [],
    }
    if not reports:
        return summary

    reference_span_sec = 0.0
    for report in reports:
        data = report["data"]
        report_summary = {
            "name": report["name"],
            "path": report["path"],
            "ok": bool(data.get("ok", False)),
            "duration_sec": float(data.get("duration_sec", 0.0) or 0.0),
            "primary_failure": data.get("primary_failure"),
            "validation_reason": data.get("validation_reason", "ok"),
            "failure_types": dict(data.get("failure_types", {})),
        }
        summary["reports"].append(report_summary)
        reference_span_sec = max(reference_span_sec, report_summary["duration_sec"])
        if not report_summary["ok"]:
            summary["ok"] = False
            summary["validation_reasons"].append(report_summary["validation_reason"])

    if len(summary["reports"]) >= 2:
        aligned_reports = []
        alignment_ok = True
        for report in summary["reports"]:
            gap_sec = max(0.0, reference_span_sec - report["duration_sec"])
            aligned_reports.append(
                {
                    "name": report["name"],
                    "path": report["path"],
                    "duration_sec": report["duration_sec"],
                    "gap_sec": round(gap_sec, 3),
                }
            )
            if max_span_gap_sec is not None and gap_sec > max_span_gap_sec:
                alignment_ok = False

        summary["cross_alignment"] = {
            "ok": alignment_ok,
            "reference_span_sec": round(reference_span_sec, 3),
            "max_span_gap_sec": max_span_gap_sec,
            "reports": aligned_reports,
        }
        if not alignment_ok:
            summary["ok"] = False
            summary["validation_reasons"].append("video_span_gap_too_large")

    return summary


def summarize_simple_report(report, failure_reason):
    if report is None:
        return None
    data = report["data"]
    return {
        "name": report["name"],
        "path": report["path"],
        "ok": bool(data.get("ok", False)),
        "failures": list(data.get("failures", [])),
        "validation_reasons": [] if data.get("ok", False) else [failure_reason],
    }


def summarize_episode_reports(
    sensor_reports,
    video_reports,
    gripper_report=None,
    hmi_report=None,
    max_sensor_span_gap_ns=None,
    max_video_span_gap_sec=None,
):
    sensor_summary = summarize_sensor_reports(sensor_reports, max_sensor_span_gap_ns)
    video_summary = summarize_video_reports(video_reports, max_video_span_gap_sec)
    gripper_summary = summarize_simple_report(gripper_report, "gripper_log_invalid")
    hmi_summary = summarize_simple_report(hmi_report, "hmi_log_invalid")

    reasons = []
    reasons.extend(sensor_summary["validation_reasons"])
    reasons.extend(video_summary["validation_reasons"])
    if gripper_summary is not None:
        reasons.extend(gripper_summary["validation_reasons"])
    if hmi_summary is not None:
        reasons.extend(hmi_summary["validation_reasons"])

    overall_ok = sensor_summary["ok"] and video_summary["ok"]
    if gripper_summary is not None:
        overall_ok = overall_ok and gripper_summary["ok"]
    if hmi_summary is not None:
        overall_ok = overall_ok and hmi_summary["ok"]

    return {
        "ok": overall_ok,
        "validation_reason": choose_primary_reason(reasons),
        "sensor": sensor_summary,
        "video": video_summary,
        "gripper": gripper_summary,
        "hmi": hmi_summary,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Summarizes sensor/video/gripper/HMI analyzer JSON outputs into one episode summary."
    )
    parser.add_argument("--sensor-report", action="append", default=[], help="Sensor report JSON path")
    parser.add_argument("--video-report", action="append", default=[], help="Video report JSON path")
    parser.add_argument("--gripper-report", help="Gripper report JSON path")
    parser.add_argument("--hmi-report", help="HMI report JSON path")
    parser.add_argument("--max-sensor-span-gap-ns", type=int, default=None)
    parser.add_argument("--max-video-span-gap-sec", type=float, default=5.0)
    parser.add_argument("--json-out", help="Optional JSON output path")
    args = parser.parse_args()

    summary = summarize_episode_reports(
        [load_report(path) for path in args.sensor_report],
        [load_report(path) for path in args.video_report],
        load_report(args.gripper_report) if args.gripper_report else None,
        load_report(args.hmi_report) if args.hmi_report else None,
        max_sensor_span_gap_ns=args.max_sensor_span_gap_ns,
        max_video_span_gap_sec=args.max_video_span_gap_sec,
    )

    print(
        f"[SUMMARY] ok={summary['ok']} "
        f"validation_reason={summary['validation_reason']}"
    )

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
