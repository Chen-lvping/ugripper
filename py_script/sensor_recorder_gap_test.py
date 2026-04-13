#!/usr/bin/env python3
from __future__ import annotations

import argparse
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from statistics import median
from typing import Iterable


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SENSOR_RECORDER_BIN = PROJECT_ROOT / "build" / "src" / "sensor_recorder" / "sensor_recorder"
DEFAULT_OUTPUT_ROOT = Path("/tmp")
EXPECTED_MCAP_FILES = ("sensor_data_left.mcap", "sensor_data_right.mcap")


def import_mcap_stream_reader():
    try:
        from mcap.stream_reader import StreamReader as reader

        return reader
    except ImportError:
        pass

    local_lib_root = Path.home() / ".local" / "lib"
    current_version = f"python{sys.version_info.major}.{sys.version_info.minor}"
    candidates = sorted(local_lib_root.glob("python*/site-packages"), reverse=True)
    for candidate in candidates:
        if candidate.parent.name == current_version:
            continue
        candidate_str = str(candidate)
        if candidate_str in sys.path:
            continue
        sys.path.insert(0, candidate_str)
        try:
            from mcap.stream_reader import StreamReader as reader

            return reader
        except ImportError:
            try:
                sys.path.remove(candidate_str)
            except ValueError:
                pass

    return None


StreamReader = import_mcap_stream_reader()


class SensorGapTestError(RuntimeError):
    pass


@dataclass(frozen=True)
class GapEvent:
    topic: str
    gap_ms: float
    log_time_ns: int
    source_file: Path


@dataclass(frozen=True)
class TopicStats:
    topic: str
    count: int
    median_ms: float
    p99_ms: float
    max_ms: float
    over_warn: int
    over_fail: int
    max_event: GapEvent | None


def ns_to_text(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1e9).strftime("%Y-%m-%d %H:%M:%S.%f")


def percentile(sorted_values: list[float], p: float) -> float:
    if not sorted_values:
        return 0.0
    index = int(round((len(sorted_values) - 1) * p))
    index = max(0, min(index, len(sorted_values) - 1))
    return sorted_values[index]


def ensure_mcap_dependency() -> None:
    if StreamReader is None:
        raise SensorGapTestError(
            "缺少 python mcap 依赖，请执行 `uv run ...` 或 `python3 -m pip install --user mcap`。"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="单独跑 sensor_recorder 或分析现有 MCAP，并输出各 topic 的 gap 统计。"
    )
    parser.add_argument(
        "--mode",
        choices=("record", "analyze"),
        default="record",
        help="record: 拉起 sensor_recorder 录制后分析；analyze: 只分析现有目录。",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=25.0,
        help="record 模式录制秒数，默认 25 秒。",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="输出目录。record 模式不传则自动创建 /tmp/sensor_gap_test_<timestamp>。",
    )
    parser.add_argument(
        "--sensor-recorder-bin",
        type=Path,
        default=DEFAULT_SENSOR_RECORDER_BIN,
        help=f"sensor_recorder 二进制路径，默认 {DEFAULT_SENSOR_RECORDER_BIN}",
    )
    parser.add_argument(
        "--warn-gap-ms",
        type=float,
        default=10.0,
        help="统计 warning 阈值，默认 10ms。",
    )
    parser.add_argument(
        "--fail-gap-ms",
        type=float,
        default=15.0,
        help="统计 fail 阈值，默认 15ms。",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=5,
        help="每个 topic 输出最大的前 N 个 gap，默认 5。",
    )
    return parser.parse_args()


def create_output_dir(requested: Path | None) -> Path:
    if requested is not None:
        requested.mkdir(parents=True, exist_ok=True)
        return requested
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = DEFAULT_OUTPUT_ROOT / f"sensor_gap_test_{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=False)
    return output_dir


def assert_output_has_mcaps(output_dir: Path) -> None:
    missing = []
    for name in EXPECTED_MCAP_FILES:
        path = output_dir / name
        if not path.exists() or path.stat().st_size <= 0:
            missing.append(str(path))
    if missing:
        raise SensorGapTestError("缺少 MCAP 输出文件：\n- " + "\n- ".join(missing))


def record_sensor_data(args: argparse.Namespace, output_dir: Path) -> Path:
    sensor_bin = args.sensor_recorder_bin.resolve()
    if not sensor_bin.exists():
        raise SensorGapTestError(f"找不到 sensor_recorder 二进制：{sensor_bin}")

    log_path = output_dir / "sensor_recorder_gap_test.log"
    print(f"[record] output_dir={output_dir}")
    print(f"[record] sensor_recorder={sensor_bin}")
    print(f"[record] duration={args.duration:.1f}s")

    with log_path.open("w", encoding="utf-8") as log_file:
        process = subprocess.Popen(
            [str(sensor_bin), str(output_dir)],
            cwd=PROJECT_ROOT,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            time.sleep(max(0.0, args.duration))
            process.send_signal(signal.SIGINT)
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        except KeyboardInterrupt:
            process.send_signal(signal.SIGINT)
            process.wait(timeout=10)
            raise

    print(f"[record] exit_code={process.returncode}")
    print(f"[record] log={log_path}")
    if process.returncode not in (0, -signal.SIGINT, 130):
        raise SensorGapTestError(f"sensor_recorder 退出异常，exit_code={process.returncode}")

    assert_output_has_mcaps(output_dir)
    return log_path


def iter_mcap_messages(mcap_path: Path):
    ensure_mcap_dependency()
    with mcap_path.open("rb") as stream:
        channels = {}
        for record in StreamReader(stream).records:
            record_name = type(record).__name__
            if record_name == "Channel":
                channels[record.id] = record
                continue
            if record_name != "Message":
                continue
            channel = channels.get(record.channel_id)
            if channel is None:
                continue
            yield channel.topic, record.log_time


def collect_gap_events(output_dir: Path) -> dict[str, list[GapEvent]]:
    per_topic: dict[str, list[GapEvent]] = {}
    for mcap_name in EXPECTED_MCAP_FILES:
        mcap_path = output_dir / mcap_name
        previous_log_time: dict[str, int] = {}
        for topic, log_time_ns in iter_mcap_messages(mcap_path):
            prev = previous_log_time.get(topic)
            if prev is not None:
                per_topic.setdefault(topic, []).append(
                    GapEvent(
                        topic=topic,
                        gap_ms=(log_time_ns - prev) / 1e6,
                        log_time_ns=log_time_ns,
                        source_file=mcap_path,
                    )
                )
            previous_log_time[topic] = log_time_ns
    if not per_topic:
        raise SensorGapTestError(f"未在 {output_dir} 里读到任何 sensor topic。")
    return per_topic


def build_topic_stats(
    per_topic_events: dict[str, list[GapEvent]],
    warn_gap_ms: float,
    fail_gap_ms: float,
) -> list[TopicStats]:
    stats: list[TopicStats] = []
    for topic in sorted(per_topic_events):
        events = per_topic_events[topic]
        gap_values = sorted(event.gap_ms for event in events)
        max_event = max(events, key=lambda event: event.gap_ms, default=None)
        stats.append(
            TopicStats(
                topic=topic,
                count=len(events),
                median_ms=median(gap_values),
                p99_ms=percentile(gap_values, 0.99),
                max_ms=max(gap_values),
                over_warn=sum(value >= warn_gap_ms for value in gap_values),
                over_fail=sum(value >= fail_gap_ms for value in gap_values),
                max_event=max_event,
            )
        )
    return stats


def print_stats(
    output_dir: Path,
    stats: Iterable[TopicStats],
    per_topic_events: dict[str, list[GapEvent]],
    top_n: int,
    warn_gap_ms: float,
    fail_gap_ms: float,
) -> None:
    print()
    print(f"[summary] output_dir={output_dir}")
    print(f"[summary] warn_gap_ms={warn_gap_ms:.3f}, fail_gap_ms={fail_gap_ms:.3f}")
    for topic_stats in stats:
        print(
            f"{topic_stats.topic:<14} "
            f"count={topic_stats.count:<6d} "
            f"median={topic_stats.median_ms:>7.3f}ms "
            f"p99={topic_stats.p99_ms:>7.3f}ms "
            f"max={topic_stats.max_ms:>7.3f}ms "
            f">=warn={topic_stats.over_warn:<4d} "
            f">=fail={topic_stats.over_fail:<4d}"
        )

        top_events = sorted(
            per_topic_events[topic_stats.topic],
            key=lambda event: event.gap_ms,
            reverse=True,
        )[: max(0, top_n)]
        for event in top_events:
            print(
                f"  top gap {event.gap_ms:>7.3f}ms at {ns_to_text(event.log_time_ns)} "
                f"file={event.source_file.name}"
            )


def main() -> int:
    args = parse_args()

    if args.mode == "analyze":
        if args.output_dir is None:
            raise SensorGapTestError("--mode analyze 需要传 --output-dir")
        output_dir = args.output_dir.resolve()
    else:
        output_dir = create_output_dir(args.output_dir)
        record_sensor_data(args, output_dir)

    assert_output_has_mcaps(output_dir)
    per_topic_events = collect_gap_events(output_dir)
    stats = build_topic_stats(per_topic_events, args.warn_gap_ms, args.fail_gap_ms)
    print_stats(output_dir, stats, per_topic_events, args.top, args.warn_gap_ms, args.fail_gap_ms)

    failed_topics = [topic_stats for topic_stats in stats if topic_stats.over_fail > 0]
    if failed_topics:
        print()
        print("[result] FAIL: 检测到超过 fail 阈值的 gap。")
        return 2

    print()
    print("[result] PASS: 未检测到超过 fail 阈值的 gap。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SensorGapTestError as exc:
        print(f"[error] {exc}", file=sys.stderr)
        raise SystemExit(1)
