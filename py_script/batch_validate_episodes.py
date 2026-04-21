#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


SCAN_SKIP_DIRS = {"logs", "runtime", "__pycache__", ".Trash-1000", "lost+found", "System Volume Information"}
PROJECT_ROOT = Path(__file__).resolve().parents[1]
VALIDATE_SCRIPT = PROJECT_ROOT / ".codex" / "skills" / "validate-episode-data" / "scripts" / "validate_episode.py"
DEFAULT_OUTPUT_ROOT = PROJECT_ROOT / "runtime" / "batch_validate_episodes"
DEFAULT_ROOT = Path("/mnt/data_disk")


@dataclass
class EpisodeResult:
    episode_dir: str
    device_root: str
    device_name: str
    status: str
    returncode: int
    fail_count: int
    warn_count: int
    info_count: int
    top_issue_code: str
    top_issue_summary: str
    findings: list[dict[str, Any]]
    summary: dict[str, Any]
    stdout_path: str
    json_path: str
    error: str = ""


def safe_iter_dirs(path: Path) -> list[Path]:
    try:
        return sorted([item for item in path.iterdir() if item.is_dir()], key=lambda item: item.name)
    except Exception:
        return []


def candidate_key(path: Path) -> str:
    try:
        return str(path.resolve())
    except Exception:
        return str(path)


def list_episode_dirs(data_dir: Path) -> list[Path]:
    if not data_dir.is_dir():
        return []
    return [
        item
        for item in safe_iter_dirs(data_dir)
        if item.name.startswith("episode_")
    ]


def has_episode_dirs(device_root: Path) -> bool:
    return bool(list_episode_dirs(device_root / "data"))


def discover_device_roots(root_dir: Path) -> list[Path]:
    found: list[Path] = []
    seen: set[str] = set()

    def try_add(path: Path) -> None:
        if not has_episode_dirs(path):
            return
        key = candidate_key(path)
        if key in seen:
            return
        seen.add(key)
        found.append(path)

    try_add(root_dir)
    if found:
        return found

    for child in safe_iter_dirs(root_dir):
        if child.name in SCAN_SKIP_DIRS or child.name.endswith("_err"):
            continue
        try_add(child)
        for sub in safe_iter_dirs(child):
            if sub.name in SCAN_SKIP_DIRS or sub.name.endswith("_err"):
                continue
            try_add(sub)

    return sorted(found, key=lambda path: path.name)


def resolve_target(input_path: str) -> tuple[str, list[Path]]:
    root = Path(input_path).expanduser().resolve()
    if not root.exists():
        raise FileNotFoundError(f"输入路径不存在: {root}")

    if root.is_file():
        raise ValueError(f"输入路径必须是目录: {root}")

    if root.name.startswith("episode_"):
        parent = root.parent
        device_root = parent.parent if parent.name == "data" else parent
        return "episode", [device_root]

    if root.name == "data":
        device_root = root.parent
        if has_episode_dirs(device_root):
            return "data", [device_root]

    device_roots = discover_device_roots(root)
    if not device_roots:
        raise ValueError(f"在输入路径下未找到任何 data/episode_*: {root}")
    return "root", device_roots


def resolve_episode_list(input_path: str, latest_per_device: bool, limit: int) -> list[tuple[Path, Path]]:
    target_type, device_roots = resolve_target(input_path)
    episodes: list[tuple[Path, Path]] = []

    if target_type == "episode":
        episode_dir = Path(input_path).expanduser().resolve()
        device_root = device_roots[0]
        return [(device_root, episode_dir)]

    for device_root in device_roots:
        device_episodes = list_episode_dirs(device_root / "data")
        if latest_per_device and device_episodes:
            episodes.append((device_root, device_episodes[-1]))
        else:
            episodes.extend((device_root, episode_dir) for episode_dir in device_episodes)

    episodes.sort(key=lambda item: (item[0].name, item[1].name))
    if limit > 0:
        episodes = episodes[:limit]
    return episodes


def format_timestamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def make_run_dir(output_root: Path, root_hint: str) -> Path:
    safe_hint = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "_" for ch in root_hint).strip("_") or "scan"
    run_dir = output_root / f"{safe_hint}_{format_timestamp()}"
    run_dir.mkdir(parents=True, exist_ok=True)
    return run_dir


def build_validate_command(args: argparse.Namespace, episode_dir: Path) -> list[str]:
    command: list[str] = []
    if args.python_runner == "uv":
        command.extend(["uv", "run", "python"])
    else:
        command.append(sys.executable)
    command.extend([str(VALIDATE_SCRIPT), str(episode_dir), "--json", "--top", str(args.top)])
    if args.skip_video_packets:
        command.append("--skip-video-packets")
    if args.main_decode_mode:
        command.extend(["--main-decode-mode", args.main_decode_mode])
    return command


def find_first_issue(findings: list[dict[str, Any]]) -> tuple[str, str]:
    for severity in ("FAIL", "WARN", "INFO"):
        for item in findings:
            if item.get("severity") == severity:
                return str(item.get("code") or ""), str(item.get("summary") or "")
    return "", ""


def validate_one_episode(
    args: argparse.Namespace,
    device_root: Path,
    episode_dir: Path,
    run_dir: Path,
) -> EpisodeResult:
    episode_output_dir = run_dir / "episodes" / device_root.name
    episode_output_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = episode_output_dir / f"{episode_dir.name}.stdout.txt"
    json_path = episode_output_dir / f"{episode_dir.name}.json"

    command = build_validate_command(args, episode_dir)
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        check=False,
    )
    stdout_path.write_text(proc.stdout, encoding="utf-8")

    error_message = ""
    payload: dict[str, Any] = {}
    findings: list[dict[str, Any]] = []
    status = "FAIL"
    if proc.stdout.strip():
        try:
            payload = json.loads(proc.stdout)
            json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
            findings = payload.get("findings") or []
            status = str(payload.get("status") or "FAIL")
        except Exception as exc:
            error_message = f"结果 JSON 解析失败: {exc}"
    else:
        error_message = proc.stderr.strip() or "校验脚本未输出结果"

    if not payload:
        payload = {
            "episode_dir": str(episode_dir),
            "status": "FAIL",
            "summary": {},
            "findings": [],
            "error": error_message,
            "stderr": proc.stderr,
        }
        json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        status = "FAIL"

    fail_count = sum(1 for item in findings if item.get("severity") == "FAIL")
    warn_count = sum(1 for item in findings if item.get("severity") == "WARN")
    info_count = sum(1 for item in findings if item.get("severity") == "INFO")
    top_issue_code, top_issue_summary = find_first_issue(findings)
    if not error_message and proc.returncode not in (0, 1, 2):
        error_message = proc.stderr.strip() or f"校验命令退出码异常: {proc.returncode}"

    return EpisodeResult(
        episode_dir=str(episode_dir),
        device_root=str(device_root),
        device_name=device_root.name,
        status=status,
        returncode=proc.returncode,
        fail_count=fail_count,
        warn_count=warn_count,
        info_count=info_count,
        top_issue_code=top_issue_code,
        top_issue_summary=top_issue_summary,
        findings=findings,
        summary=payload.get("summary") or {},
        stdout_path=str(stdout_path),
        json_path=str(json_path),
        error=error_message,
    )


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def build_markdown_report(
    input_root: Path,
    run_dir: Path,
    results: list[EpisodeResult],
    args: argparse.Namespace,
) -> str:
    total = len(results)
    pass_count = sum(1 for item in results if item.status == "PASS")
    warn_count = sum(1 for item in results if item.status == "WARN")
    fail_count = sum(1 for item in results if item.status == "FAIL")
    lines = [
        "# Batch Episode Validation Report",
        "",
        f"- input_root: `{input_root}`",
        f"- generated_at: `{datetime.now().isoformat(timespec='seconds')}`",
        f"- python_runner: `{args.python_runner}`",
        f"- workers: `{args.workers}`",
        f"- total_episodes: `{total}`",
        f"- pass: `{pass_count}`",
        f"- warn: `{warn_count}`",
        f"- fail: `{fail_count}`",
        "",
        "## Worst Episodes",
        "",
        "| status | device | episode | fail | warn | top issue |",
        "| --- | --- | --- | ---: | ---: | --- |",
    ]
    ranked = sorted(
        results,
        key=lambda item: (
            {"FAIL": 0, "WARN": 1, "PASS": 2}.get(item.status, 3),
            -item.fail_count,
            -item.warn_count,
            item.device_name,
            item.episode_dir,
        ),
    )
    for item in ranked[:50]:
        lines.append(
            f"| {item.status} | {item.device_name} | {Path(item.episode_dir).name} | "
            f"{item.fail_count} | {item.warn_count} | {item.top_issue_summary or '-'} |"
        )

    flagged = [item for item in ranked if item.status != "PASS"]
    if flagged:
        lines.extend(["", "## Flagged Details", ""])
        for item in flagged[:100]:
            lines.append(f"### {item.status} {item.device_name}/{Path(item.episode_dir).name}")
            if item.error:
                lines.append(f"- runner_error: `{item.error}`")
            lines.append(f"- json: `{Path(item.json_path).relative_to(run_dir)}`")
            lines.append(f"- stdout: `{Path(item.stdout_path).relative_to(run_dir)}`")
            top_findings = item.findings[:5]
            if top_findings:
                for finding in top_findings:
                    lines.append(
                        f"- [{finding.get('severity', 'INFO')}] "
                        f"{finding.get('summary', '')}"
                    )
            else:
                lines.append("- no structured findings returned")
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def choose_python_runner(preferred: str) -> str:
    if preferred in {"uv", "python"}:
        return preferred
    return "uv" if shutil.which("uv") else "python"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "批量校验 ugripper episode 数据。默认复用 "
            ".codex/skills/validate-episode-data/scripts/validate_episode.py 的口径。"
        )
    )
    parser.add_argument(
        "root_dir",
        nargs="?",
        default=str(DEFAULT_ROOT),
        help="可传 /mnt/data_disk、设备目录、data 目录或单个 episode 目录。",
    )
    parser.add_argument(
        "--output-root",
        default=str(DEFAULT_OUTPUT_ROOT),
        help="报告输出根目录。",
    )
    parser.add_argument("--workers", type=int, default=max(1, min(6, os.cpu_count() or 1)))
    parser.add_argument("--limit", type=int, default=0, help="最多处理前 N 个 episode，0 表示不限制。")
    parser.add_argument(
        "--latest-per-device",
        action="store_true",
        help="每个设备只校验最新一条 episode。",
    )
    parser.add_argument("--skip-video-packets", action="store_true", help="跳过视频逐包扫描，加快速度。")
    parser.add_argument(
        "--main-decode-mode",
        choices=("auto", "full", "off"),
        default="auto",
        help="主摄专项解码扫描策略，透传给 validate_episode.py。",
    )
    parser.add_argument("--top", type=int, default=5, help="每条流保留的 gap 样本数量。")
    parser.add_argument(
        "--python-runner",
        choices=("auto", "uv", "python"),
        default="auto",
        help="选择调用 validate_episode.py 的运行器。",
    )
    return parser


def main() -> int:
    if not VALIDATE_SCRIPT.exists():
        print(f"❌ 缺少校验脚本: {VALIDATE_SCRIPT}")
        return 2

    args = build_parser().parse_args()
    args.python_runner = choose_python_runner(args.python_runner)

    input_root = Path(args.root_dir).expanduser().resolve()
    output_root = Path(args.output_root).expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    try:
        episodes = resolve_episode_list(args.root_dir, args.latest_per_device, args.limit)
    except Exception as exc:
        print(f"❌ {exc}")
        return 2

    if not episodes:
        print(f"❌ 未找到任何 episode_*: {input_root}")
        return 2

    run_dir = make_run_dir(output_root, input_root.name)
    print(f"开始批量校验 {len(episodes)} 条 episode，workers={args.workers}")
    print(f"报告目录: {run_dir}")

    results: list[EpisodeResult] = []
    worker_count = max(1, int(args.workers or 1))
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        future_map = {
            executor.submit(validate_one_episode, args, device_root, episode_dir, run_dir): (device_root, episode_dir)
            for device_root, episode_dir in episodes
        }
        total = len(future_map)
        for index, future in enumerate(as_completed(future_map), 1):
            result = future.result()
            results.append(result)
            if index % 10 == 0 or index == total:
                print(f"进度: {index}/{total}")

    results.sort(key=lambda item: (item.device_name, Path(item.episode_dir).name))
    summary_payload = {
        "input_root": str(input_root),
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "python_runner": args.python_runner,
        "workers": worker_count,
        "total_episodes": len(results),
        "status_counts": {
            "PASS": sum(1 for item in results if item.status == "PASS"),
            "WARN": sum(1 for item in results if item.status == "WARN"),
            "FAIL": sum(1 for item in results if item.status == "FAIL"),
        },
        "episodes": [
            {
                "episode_dir": item.episode_dir,
                "device_root": item.device_root,
                "device_name": item.device_name,
                "status": item.status,
                "returncode": item.returncode,
                "fail_count": item.fail_count,
                "warn_count": item.warn_count,
                "info_count": item.info_count,
                "top_issue_code": item.top_issue_code,
                "top_issue_summary": item.top_issue_summary,
                "json_path": item.json_path,
                "stdout_path": item.stdout_path,
                "error": item.error,
                "summary": item.summary,
            }
            for item in results
        ],
    }
    (run_dir / "summary.json").write_text(
        json.dumps(summary_payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    summary_rows = [
        {
            "status": item.status,
            "device_name": item.device_name,
            "episode": Path(item.episode_dir).name,
            "episode_dir": item.episode_dir,
            "fail_count": item.fail_count,
            "warn_count": item.warn_count,
            "info_count": item.info_count,
            "top_issue_code": item.top_issue_code,
            "top_issue_summary": item.top_issue_summary,
            "runner_error": item.error,
            "json_path": item.json_path,
            "stdout_path": item.stdout_path,
        }
        for item in results
    ]
    write_csv(
        run_dir / "summary.csv",
        summary_rows,
        [
            "status",
            "device_name",
            "episode",
            "episode_dir",
            "fail_count",
            "warn_count",
            "info_count",
            "top_issue_code",
            "top_issue_summary",
            "runner_error",
            "json_path",
            "stdout_path",
        ],
    )

    finding_rows: list[dict[str, Any]] = []
    for item in results:
        for finding in item.findings:
            finding_rows.append(
                {
                    "status": item.status,
                    "device_name": item.device_name,
                    "episode": Path(item.episode_dir).name,
                    "severity": finding.get("severity", ""),
                    "code": finding.get("code", ""),
                    "summary": finding.get("summary", ""),
                    "details_json": json.dumps(finding.get("details") or {}, ensure_ascii=False, sort_keys=True),
                }
            )
    write_csv(
        run_dir / "findings.csv",
        finding_rows,
        ["status", "device_name", "episode", "severity", "code", "summary", "details_json"],
    )

    report_text = build_markdown_report(input_root, run_dir, results, args)
    (run_dir / "00_report.md").write_text(report_text, encoding="utf-8")

    pass_count = summary_payload["status_counts"]["PASS"]
    warn_count = summary_payload["status_counts"]["WARN"]
    fail_count = summary_payload["status_counts"]["FAIL"]
    print("批量校验完成")
    print(f"PASS={pass_count} WARN={warn_count} FAIL={fail_count}")
    print(f"Markdown: {run_dir / '00_report.md'}")
    print(f"JSON: {run_dir / 'summary.json'}")
    print(f"CSV: {run_dir / 'summary.csv'}")
    return 2 if fail_count > 0 else 1 if warn_count > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
