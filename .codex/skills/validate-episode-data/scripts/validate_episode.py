#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path
from statistics import median
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[4]
MCAP_FALLBACK_ROOTS = [
    PROJECT_ROOT / "build" / "_deps" / "mcap-src" / "python" / "mcap",
]

VIDEO_FILES = [
    ("left_cam_main", "left_cam_main.mkv", "left"),
    ("right_cam_main", "right_cam_main.mkv", "right"),
    ("left_stereo", "left_stereo.mkv", "left"),
    ("right_stereo", "right_stereo.mkv", "right"),
    ("left_tcam_l", "left_tcam_l.mkv", "left"),
    ("left_tcam_r", "left_tcam_r.mkv", "left"),
    ("right_tcam_l", "right_tcam_l.mkv", "right"),
    ("right_tcam_r", "right_tcam_r.mkv", "right"),
]
SENSOR_FILES = [
    ("left", "sensor_data_left.mcap", ["imu_left", "encoder_left"]),
    ("right", "sensor_data_right.mcap", ["imu_right", "encoder_right"]),
]
REQUIRED_FILES = [
    "metadata.json",
    "calibration.json",
    "info.json",
    *(file_name for _, file_name, _ in VIDEO_FILES),
    *(file_name for _, file_name, _ in SENSOR_FILES),
]


def us_to_ms(value_us: float) -> float:
    return value_us / 1000.0


def ns_to_ms(value_ns: float) -> float:
    return value_ns / 1_000_000.0


def sec_to_us(value_sec: float) -> int:
    return int(round(value_sec * 1_000_000.0))


def safe_float(value: Any) -> float | None:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip()
    if not text or text == "N/A":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def safe_int(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            return None
        return int(round(value))
    text = str(value).strip()
    if not text:
        return None
    try:
        return int(text)
    except ValueError:
        try:
            return int(round(float(text)))
        except ValueError:
            return None


def parse_rate(rate_text: str | None) -> float | None:
    if rate_text is None:
        return None
    text = rate_text.strip()
    if not text or text == "0/0":
        return None
    if "/" in text:
        left, right = text.split("/", 1)
        try:
            num = float(left)
            den = float(right)
        except ValueError:
            return None
        if den == 0:
            return None
        return num / den
    return safe_float(text)


def percentile(sorted_values: list[float], p: float) -> float:
    if not sorted_values:
        return 0.0
    index = int(round((len(sorted_values) - 1) * p))
    index = max(0, min(index, len(sorted_values) - 1))
    return sorted_values[index]


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        check=False,
    )


def ensure_ffprobe() -> None:
    if shutil.which("ffprobe") is None:
        raise SystemExit("missing required binary: ffprobe")


def import_mcap_reader():
    try:
        from mcap.stream_reader import StreamReader  # type: ignore

        return StreamReader
    except Exception:
        pass

    for root in MCAP_FALLBACK_ROOTS:
        root_text = str(root)
        if root.exists() and root_text not in sys.path:
            sys.path.insert(0, root_text)
        try:
            from mcap.stream_reader import StreamReader  # type: ignore

            return StreamReader
        except Exception:
            continue
    return None


StreamReader = import_mcap_reader()


@dataclass
class Finding:
    severity: str
    code: str
    summary: str
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class PacketGap:
    gap_ms: float
    prev_pts_sec: float
    curr_pts_sec: float
    flags: str


@dataclass
class VideoStats:
    name: str
    side: str
    path: str
    codec: str | None = None
    duration_sec: float | None = None
    avg_fps: float | None = None
    packet_count: int = 0
    first_pts_sec: float | None = None
    last_pts_sec: float | None = None
    first_system_time_us: int | None = None
    last_system_time_us: int | None = None
    duplicate_pts_count: int = 0
    backward_pts_count: int = 0
    max_gap_ms: float = 0.0
    median_delta_ms: float = 0.0
    p99_delta_ms: float = 0.0
    large_gap_count: int = 0
    largest_gaps: list[PacketGap] = field(default_factory=list)
    error: str | None = None


@dataclass
class SensorTopicStats:
    topic: str
    source_file: str
    count: int = 0
    first_log_time_ns: int | None = None
    last_log_time_ns: int | None = None
    median_gap_ms: float = 0.0
    p99_gap_ms: float = 0.0
    max_gap_ms: float = 0.0
    large_gap_count: int = 0
    gap_threshold_ms: float = 0.0


@dataclass
class Report:
    episode_dir: str
    status: str
    findings: list[Finding]
    videos: dict[str, VideoStats]
    sensors: dict[str, SensorTopicStats]
    summary: dict[str, Any]


def list_episode_dirs(root: Path) -> list[Path]:
    if not root.exists() or not root.is_dir():
        return []
    return sorted(
        [path for path in root.iterdir() if path.is_dir() and path.name.startswith("episode_")],
        key=lambda path: (path.name, path.stat().st_mtime_ns),
    )


def resolve_episode_dir(input_path: str, latest: bool) -> Path:
    path = Path(input_path).resolve()
    if not path.exists():
        raise SystemExit(f"path not found: {path}")

    if path.is_file():
        raise SystemExit(f"path is not a directory: {path}")

    if path.name.startswith("episode_"):
        return path

    candidates = list_episode_dirs(path)
    if not candidates:
        raise SystemExit(f"no episode_* directories found under: {path}")

    if latest:
        return candidates[-1]

    if len(candidates) == 1:
        return candidates[0]

    candidate_preview = "\n".join(f"- {candidate}" for candidate in candidates[-5:])
    raise SystemExit(
        "input is a parent directory with multiple episode_* children; "
        "pass --latest to auto-select the newest one, or pass the episode path directly.\n"
        f"recent candidates:\n{candidate_preview}"
    )


class EpisodeValidator:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.episode_dir = resolve_episode_dir(args.path, args.latest)
        self.findings: list[Finding] = []
        self.info: dict[str, Any] = {}
        self.metadata: dict[str, Any] = {}
        self.video_stats: dict[str, VideoStats] = {}
        self.sensor_stats: dict[str, SensorTopicStats] = {}

    def add_finding(self, severity: str, code: str, summary: str, **details: Any) -> None:
        self.findings.append(Finding(severity=severity, code=code, summary=summary, details=details))

    def severity_rank(self, severity: str) -> int:
        return {"FAIL": 3, "WARN": 2, "INFO": 1}.get(severity, 0)

    def validate(self) -> Report:
        self.check_episode_dir()
        self.check_required_files()
        self.load_metadata()
        self.load_info()
        self.validate_info_fields()
        self.scan_videos()
        self.scan_sensors()
        self.check_video_alignment()
        self.check_first_frame_sync()
        self.check_stereo_session_consistency()
        self.check_sensor_alignment()
        self.check_video_sensor_alignment()

        status = "PASS"
        if any(f.severity == "FAIL" for f in self.findings):
            status = "FAIL"
        elif any(f.severity == "WARN" for f in self.findings):
            status = "WARN"

        self.findings.sort(key=lambda item: (-self.severity_rank(item.severity), item.code, item.summary))
        summary = {
            "finding_counts": {
                "fail": sum(1 for item in self.findings if item.severity == "FAIL"),
                "warn": sum(1 for item in self.findings if item.severity == "WARN"),
                "info": sum(1 for item in self.findings if item.severity == "INFO"),
            },
            "video_count": len(self.video_stats),
            "sensor_topic_count": len(self.sensor_stats),
        }
        return Report(
            episode_dir=str(self.episode_dir),
            status=status,
            findings=self.findings,
            videos=self.video_stats,
            sensors=self.sensor_stats,
            summary=summary,
        )

    def check_episode_dir(self) -> None:
        if not self.episode_dir.exists():
            raise SystemExit(f"episode dir not found: {self.episode_dir}")
        if not self.episode_dir.is_dir():
            raise SystemExit(f"episode path is not a directory: {self.episode_dir}")

    def check_required_files(self) -> None:
        for name in REQUIRED_FILES:
            path = self.episode_dir / name
            if not path.exists():
                self.add_finding("FAIL", "missing_file", f"缺少关键文件: {name}", path=str(path))
                continue
            if path.stat().st_size <= 0:
                self.add_finding("FAIL", "empty_file", f"关键文件为空: {name}", path=str(path))

    def load_json_file(self, name: str) -> dict[str, Any]:
        path = self.episode_dir / name
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            self.add_finding("FAIL", "json_parse_error", f"{name} 解析失败", path=str(path), error=str(exc))
            return {}

    def load_metadata(self) -> None:
        self.metadata = self.load_json_file("metadata.json")

    def load_info(self) -> None:
        self.info = self.load_json_file("info.json")

    def validate_info_fields(self) -> None:
        boot_offset = safe_float(self.info.get("boot_time_offset"))
        boot_offset_us = safe_int(self.info.get("boot_time_offset_us"))
        if boot_offset is None or boot_offset <= 0:
            self.add_finding("FAIL", "info_field", "info.json 缺少合法的 boot_time_offset")
        if boot_offset_us is None or boot_offset_us <= 0:
            self.add_finding("FAIL", "info_field", "info.json 缺少合法的 boot_time_offset_us")
        if boot_offset is not None and boot_offset_us is not None:
            if abs(boot_offset * 1_000_000.0 - boot_offset_us) > 1.0:
                self.add_finding(
                    "FAIL",
                    "boot_offset_mismatch",
                    "boot_time_offset 与 boot_time_offset_us 不一致",
                    boot_time_offset=boot_offset,
                    boot_time_offset_us=boot_offset_us,
                )

        for video_name, _, _ in VIDEO_FILES:
            field_name = f"{video_name}_record_time_offset_us"
            value = safe_int(self.info.get(field_name))
            if value is None or value <= 0:
                self.add_finding("FAIL", "info_field", f"info.json 缺少合法字段: {field_name}")

    def ffprobe_stream_info(self, path: Path) -> tuple[str | None, float | None, float | None]:
        result = run_command(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=codec_name,avg_frame_rate,r_frame_rate:format=duration",
                "-of",
                "json",
                str(path),
            ]
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "ffprobe stream probe failed")
        payload = json.loads(result.stdout or "{}")
        streams = payload.get("streams") or []
        stream = streams[0] if streams else {}
        codec = stream.get("codec_name")
        avg_fps = parse_rate(stream.get("avg_frame_rate")) or parse_rate(stream.get("r_frame_rate"))
        duration_sec = safe_float((payload.get("format") or {}).get("duration"))
        return codec, duration_sec, avg_fps

    def ffprobe_packets(self, path: Path) -> list[dict[str, Any]]:
        result = run_command(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_packets",
                "-show_entries",
                "packet=pts_time,dts_time,flags",
                "-of",
                "json",
                str(path),
            ]
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "ffprobe packet probe failed")
        return (json.loads(result.stdout or "{}").get("packets") or [])

    def scan_videos(self) -> None:
        for video_name, file_name, side in VIDEO_FILES:
            path = self.episode_dir / file_name
            stats = VideoStats(name=video_name, side=side, path=str(path))
            self.video_stats[video_name] = stats
            if not path.exists() or path.stat().st_size <= 0:
                stats.error = "missing or empty file"
                continue

            try:
                stats.codec, stats.duration_sec, stats.avg_fps = self.ffprobe_stream_info(path)
            except Exception as exc:
                stats.error = str(exc)
                self.add_finding("FAIL", "video_unreadable", f"视频不可读: {file_name}", error=str(exc))
                continue

            record_offset_us = safe_int(self.info.get(f"{video_name}_record_time_offset_us"))
            if self.args.skip_video_packets:
                if stats.duration_sec is not None and record_offset_us is not None:
                    stats.first_pts_sec = 0.0
                    stats.last_pts_sec = max(0.0, stats.duration_sec)
                    stats.first_system_time_us = record_offset_us
                    stats.last_system_time_us = record_offset_us + sec_to_us(stats.duration_sec)
                continue

            try:
                packets = self.ffprobe_packets(path)
            except Exception as exc:
                stats.error = str(exc)
                self.add_finding("FAIL", "video_packet_probe_failed", f"视频逐包扫描失败: {file_name}", error=str(exc))
                continue

            pts_list: list[float] = []
            deltas_ms: list[float] = []
            largest_gaps: list[PacketGap] = []
            prev_pts: float | None = None
            for packet in packets:
                pts = safe_float(packet.get("pts_time"))
                if pts is None:
                    pts = safe_float(packet.get("dts_time"))
                if pts is None:
                    continue
                pts_list.append(pts)
                if prev_pts is not None:
                    delta_sec = pts - prev_pts
                    if abs(delta_sec) < 1e-9:
                        stats.duplicate_pts_count += 1
                    elif delta_sec < 0:
                        stats.backward_pts_count += 1
                    else:
                        delta_ms = delta_sec * 1000.0
                        deltas_ms.append(delta_ms)
                        if delta_ms > stats.max_gap_ms:
                            stats.max_gap_ms = delta_ms
                        largest_gaps.append(
                            PacketGap(
                                gap_ms=delta_ms,
                                prev_pts_sec=prev_pts,
                                curr_pts_sec=pts,
                                flags=str(packet.get("flags", "")),
                            )
                        )
                prev_pts = pts

            stats.packet_count = len(pts_list)
            if pts_list:
                stats.first_pts_sec = pts_list[0]
                stats.last_pts_sec = pts_list[-1]
                if record_offset_us is not None:
                    stats.first_system_time_us = record_offset_us + sec_to_us(stats.first_pts_sec)
                    stats.last_system_time_us = record_offset_us + sec_to_us(stats.last_pts_sec)
            if deltas_ms:
                sorted_deltas = sorted(deltas_ms)
                stats.median_delta_ms = median(sorted_deltas)
                stats.p99_delta_ms = percentile(sorted_deltas, 0.99)
                gap_floor_ms = self.args.video_gap_min_ms
                threshold_ms = max(gap_floor_ms, stats.median_delta_ms * self.args.video_gap_factor)
                stats.large_gap_count = sum(1 for value in deltas_ms if value > threshold_ms)
                stats.largest_gaps = sorted(largest_gaps, key=lambda item: item.gap_ms, reverse=True)[: self.args.top]

                if stats.max_gap_ms > threshold_ms:
                    severity = "FAIL" if stats.max_gap_ms >= self.args.video_gap_fail_ms else "WARN"
                    self.add_finding(
                        severity,
                        "video_gap",
                        f"{file_name} 存在异常视频 gap",
                        max_gap_ms=round(stats.max_gap_ms, 3),
                        threshold_ms=round(threshold_ms, 3),
                        median_delta_ms=round(stats.median_delta_ms, 3),
                        large_gap_count=stats.large_gap_count,
                    )
            if stats.packet_count <= 1:
                self.add_finding("FAIL", "video_too_few_packets", f"{file_name} 视频包数量过少", packet_count=stats.packet_count)
            if stats.duplicate_pts_count > 0:
                self.add_finding(
                    "WARN",
                    "video_duplicate_pts",
                    f"{file_name} 存在重复时间戳",
                    duplicate_pts_count=stats.duplicate_pts_count,
                )
            if stats.backward_pts_count > 0:
                self.add_finding(
                    "FAIL",
                    "video_backward_pts",
                    f"{file_name} 存在回退时间戳",
                    backward_pts_count=stats.backward_pts_count,
                )

    def iter_mcap_messages(self, mcap_path: Path):
        if StreamReader is None:
            raise RuntimeError(
                "python mcap package is unavailable; prefer running this script via `uv run python ...`"
            )
        with mcap_path.open("rb") as handle:
            channels: dict[int, Any] = {}
            for record in StreamReader(handle).records:
                record_name = type(record).__name__
                if record_name == "Channel":
                    channels[record.id] = record
                    continue
                if record_name != "Message":
                    continue
                channel = channels.get(record.channel_id)
                if channel is None:
                    continue
                yield str(channel.topic), int(record.log_time)

    def scan_sensors(self) -> None:
        if StreamReader is None:
            self.add_finding(
                "WARN",
                "mcap_dependency_missing",
                "无法执行 sensor 深度分析，当前 Python 环境缺少可用的 mcap 读取器，建议改用 `uv run python`",
                fallback_roots=[str(path) for path in MCAP_FALLBACK_ROOTS],
            )
            return

        topic_times: dict[str, list[int]] = defaultdict(list)
        topic_sources: dict[str, str] = {}
        for _, file_name, expected_topics in SENSOR_FILES:
            path = self.episode_dir / file_name
            if not path.exists() or path.stat().st_size <= 0:
                continue
            try:
                for topic, log_time_ns in self.iter_mcap_messages(path):
                    topic_times[topic].append(log_time_ns)
                    topic_sources[topic] = str(path)
            except Exception as exc:
                self.add_finding("FAIL", "mcap_read_error", f"读取 {file_name} 失败", error=str(exc))
                continue

            for topic in expected_topics:
                if topic not in topic_times:
                    self.add_finding("FAIL", "missing_sensor_topic", f"{file_name} 缺少 topic: {topic}", path=str(path))

        for topic, timestamps in sorted(topic_times.items()):
            timestamps.sort()
            stats = SensorTopicStats(topic=topic, source_file=topic_sources.get(topic, ""))
            self.sensor_stats[topic] = stats
            stats.count = len(timestamps)
            stats.first_log_time_ns = timestamps[0]
            stats.last_log_time_ns = timestamps[-1]
            if len(timestamps) <= 1:
                self.add_finding("FAIL", "sensor_too_few_samples", f"{topic} 样本数过少", count=len(timestamps))
                continue

            gaps_ms = [
                (timestamps[index] - timestamps[index - 1]) / 1_000_000.0
                for index in range(1, len(timestamps))
                if timestamps[index] >= timestamps[index - 1]
            ]
            if not gaps_ms:
                self.add_finding("FAIL", "sensor_bad_timestamps", f"{topic} 时间戳异常，无法建立 gap 统计")
                continue

            sorted_gaps = sorted(gaps_ms)
            stats.median_gap_ms = median(sorted_gaps)
            stats.p99_gap_ms = percentile(sorted_gaps, 0.99)
            stats.max_gap_ms = max(sorted_gaps)
            min_gap_floor = self.args.imu_gap_min_ms if topic.startswith("imu_") else self.args.encoder_gap_min_ms
            stats.gap_threshold_ms = max(min_gap_floor, stats.median_gap_ms * self.args.sensor_gap_factor)
            stats.large_gap_count = sum(1 for value in gaps_ms if value > stats.gap_threshold_ms)

            if stats.max_gap_ms > stats.gap_threshold_ms:
                severity = "FAIL" if stats.max_gap_ms >= self.args.sensor_gap_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "sensor_gap",
                    f"{topic} 存在异常 gap",
                    max_gap_ms=round(stats.max_gap_ms, 3),
                    threshold_ms=round(stats.gap_threshold_ms, 3),
                    median_gap_ms=round(stats.median_gap_ms, 3),
                    large_gap_count=stats.large_gap_count,
                )

    def check_video_alignment(self) -> None:
        starts = {
            name: stats.first_system_time_us
            for name, stats in self.video_stats.items()
            if stats.first_system_time_us is not None
        }
        ends = {
            name: stats.last_system_time_us
            for name, stats in self.video_stats.items()
            if stats.last_system_time_us is not None
        }
        if len(starts) >= 2:
            start_range_ms = us_to_ms(max(starts.values()) - min(starts.values()))
            if start_range_ms > self.args.video_start_warn_ms:
                severity = "FAIL" if start_range_ms >= self.args.video_start_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_start_alignment",
                    "多路视频起始时间对齐偏差过大",
                    start_range_ms=round(start_range_ms, 3),
                    per_camera_ms={name: round(us_to_ms(value - min(starts.values())), 3) for name, value in starts.items()},
                )
        if len(ends) >= 2:
            end_range_ms = us_to_ms(max(ends.values()) - min(ends.values()))
            if end_range_ms > self.args.video_end_warn_ms:
                severity = "FAIL" if end_range_ms >= self.args.video_end_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_end_alignment",
                    "多路视频结束时间对齐偏差过大",
                    end_range_ms=round(end_range_ms, 3),
                    earliest_end_us=min(ends.values()),
                    latest_end_us=max(ends.values()),
                )

    def check_first_frame_sync(self) -> None:
        firsts = {
            name: stats.first_system_time_us
            for name, stats in self.video_stats.items()
            if stats.first_system_time_us is not None
        }
        if len(firsts) < 2:
            return
        sync_error_ms = us_to_ms(max(firsts.values()) - min(firsts.values()))
        if sync_error_ms > self.args.first_frame_warn_ms:
            severity = "FAIL" if sync_error_ms >= self.args.first_frame_fail_ms else "WARN"
            self.add_finding(
                severity,
                "first_frame_sync",
                "首帧同步误差过大",
                sync_error_ms=round(sync_error_ms, 3),
            )

    def check_stereo_session_consistency(self) -> None:
        stereo_session = self.info.get("stereo_session")
        if not isinstance(stereo_session, dict):
            self.add_finding("WARN", "missing_stereo_session", "info.json 缺少 stereo_session，无法做双目会话一致性校验")
            return

        start_us = safe_int(stereo_session.get("start_system_time_us"))
        stop_us = safe_int(stereo_session.get("stop_system_time_us"))
        if start_us is None or stop_us is None or stop_us <= start_us:
            self.add_finding("FAIL", "stereo_session", "stereo_session 的 start/stop_system_time_us 非法")
            return

        cameras = stereo_session.get("cameras")
        if not isinstance(cameras, dict):
            self.add_finding("FAIL", "stereo_session", "stereo_session 缺少 cameras 字段")
            return

        for camera_name in ("left_stereo", "right_stereo"):
            entry = cameras.get(camera_name)
            if not isinstance(entry, dict):
                self.add_finding("FAIL", "stereo_session", f"stereo_session 缺少 {camera_name} entry")
                continue
            first_pts_us = safe_int(entry.get("first_written_frame_pts_us"))
            first_sys_us = safe_int(entry.get("first_written_frame_system_time_us"))
            last_pts_us = safe_int(entry.get("last_written_frame_pts_us"))
            last_sys_us = safe_int(entry.get("last_written_frame_system_time_us"))
            offset_us = safe_int(self.info.get(f"{camera_name}_record_time_offset_us"))
            if None in (first_pts_us, first_sys_us, last_pts_us, last_sys_us, offset_us):
                self.add_finding("FAIL", "stereo_session", f"{camera_name} 的 stereo_session 首尾字段不完整")
                continue
            expected_first_sys_us = offset_us + first_pts_us
            expected_last_sys_us = offset_us + last_pts_us
            first_error_ms = us_to_ms(abs(expected_first_sys_us - first_sys_us))
            last_error_ms = us_to_ms(abs(expected_last_sys_us - last_sys_us))
            if first_error_ms > self.args.stereo_consistency_warn_ms:
                severity = "FAIL" if first_error_ms >= self.args.stereo_consistency_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "stereo_first_consistency",
                    f"{camera_name} 首帧时间与顶层 offset 不自洽",
                    error_ms=round(first_error_ms, 3),
                )
            if last_error_ms > self.args.stereo_consistency_warn_ms:
                severity = "FAIL" if last_error_ms >= self.args.stereo_consistency_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "stereo_last_consistency",
                    f"{camera_name} 尾帧时间与顶层 offset 不自洽",
                    error_ms=round(last_error_ms, 3),
                )

    def check_sensor_alignment(self) -> None:
        for topic_prefix in ("imu", "encoder"):
            left = self.sensor_stats.get(f"{topic_prefix}_left")
            right = self.sensor_stats.get(f"{topic_prefix}_right")
            if left is None or right is None:
                continue
            if left.first_log_time_ns is None or right.first_log_time_ns is None:
                continue
            start_diff_ms = ns_to_ms(abs(left.first_log_time_ns - right.first_log_time_ns))
            end_diff_ms = ns_to_ms(abs((left.last_log_time_ns or 0) - (right.last_log_time_ns or 0)))
            if start_diff_ms > self.args.sensor_lr_start_warn_ms:
                severity = "FAIL" if start_diff_ms >= self.args.sensor_lr_start_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "sensor_lr_start_alignment",
                    f"{topic_prefix} 左右起始同步偏差过大",
                    start_diff_ms=round(start_diff_ms, 3),
                )
            if end_diff_ms > self.args.sensor_lr_end_warn_ms:
                severity = "FAIL" if end_diff_ms >= self.args.sensor_lr_end_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "sensor_lr_end_alignment",
                    f"{topic_prefix} 左右结束同步偏差过大",
                    end_diff_ms=round(end_diff_ms, 3),
                )

    def side_video_window_us(self, side: str) -> tuple[int | None, int | None]:
        starts = [
            stats.first_system_time_us
            for stats in self.video_stats.values()
            if stats.side == side and stats.first_system_time_us is not None
        ]
        ends = [
            stats.last_system_time_us
            for stats in self.video_stats.values()
            if stats.side == side and stats.last_system_time_us is not None
        ]
        return (min(starts) if starts else None, max(ends) if ends else None)

    def side_sensor_window_ns(self, side: str) -> tuple[int | None, int | None]:
        topics = [self.sensor_stats.get(f"imu_{side}"), self.sensor_stats.get(f"encoder_{side}")]
        starts = [topic.first_log_time_ns for topic in topics if topic and topic.first_log_time_ns is not None]
        ends = [topic.last_log_time_ns for topic in topics if topic and topic.last_log_time_ns is not None]
        return (min(starts) if starts else None, max(ends) if ends else None)

    def check_video_sensor_alignment(self) -> None:
        for side in ("left", "right"):
            video_start_us, video_end_us = self.side_video_window_us(side)
            sensor_start_ns, sensor_end_ns = self.side_sensor_window_ns(side)
            if None in (video_start_us, video_end_us, sensor_start_ns, sensor_end_ns):
                continue
            start_diff_ms = abs(video_start_us * 1000 - sensor_start_ns) / 1_000_000.0
            end_diff_ms = abs(video_end_us * 1000 - sensor_end_ns) / 1_000_000.0
            if start_diff_ms > self.args.video_sensor_start_warn_ms:
                severity = "FAIL" if start_diff_ms >= self.args.video_sensor_start_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_sensor_start_alignment",
                    f"{side} 侧视频与 sensor 起始窗口错位",
                    start_diff_ms=round(start_diff_ms, 3),
                )
            if end_diff_ms > self.args.video_sensor_end_warn_ms:
                severity = "FAIL" if end_diff_ms >= self.args.video_sensor_end_fail_ms else "WARN"
                self.add_finding(
                    severity,
                    "video_sensor_end_alignment",
                    f"{side} 侧视频与 sensor 结束窗口错位",
                    end_diff_ms=round(end_diff_ms, 3),
                )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate one ugripper episode directory in read-only mode.")
    parser.add_argument("path", help="Episode directory, or a parent directory that contains episode_* children.")
    parser.add_argument(
        "--latest",
        action="store_true",
        help="When PATH is a parent directory, auto-select the newest episode_* child.",
    )
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON.")
    parser.add_argument("--top", type=int, default=5, help="Max retained gap samples per stream.")
    parser.add_argument("--skip-video-packets", action="store_true", help="Skip ffprobe packet scan and only use container-level timing.")
    parser.add_argument("--video-gap-factor", type=float, default=3.0)
    parser.add_argument("--sensor-gap-factor", type=float, default=3.0)
    parser.add_argument("--video-gap-min-ms", type=float, default=80.0)
    parser.add_argument("--video-gap-fail-ms", type=float, default=150.0)
    parser.add_argument("--imu-gap-min-ms", type=float, default=20.0)
    parser.add_argument("--encoder-gap-min-ms", type=float, default=30.0)
    parser.add_argument("--sensor-gap-fail-ms", type=float, default=120.0)
    parser.add_argument("--video-start-warn-ms", type=float, default=80.0)
    parser.add_argument("--video-start-fail-ms", type=float, default=150.0)
    parser.add_argument("--video-end-warn-ms", type=float, default=150.0)
    parser.add_argument("--video-end-fail-ms", type=float, default=300.0)
    parser.add_argument("--first-frame-warn-ms", type=float, default=33.0)
    parser.add_argument("--first-frame-fail-ms", type=float, default=80.0)
    parser.add_argument("--stereo-consistency-warn-ms", type=float, default=10.0)
    parser.add_argument("--stereo-consistency-fail-ms", type=float, default=30.0)
    parser.add_argument("--sensor-lr-start-warn-ms", type=float, default=30.0)
    parser.add_argument("--sensor-lr-start-fail-ms", type=float, default=80.0)
    parser.add_argument("--sensor-lr-end-warn-ms", type=float, default=50.0)
    parser.add_argument("--sensor-lr-end-fail-ms", type=float, default=120.0)
    parser.add_argument("--video-sensor-start-warn-ms", type=float, default=120.0)
    parser.add_argument("--video-sensor-start-fail-ms", type=float, default=300.0)
    parser.add_argument("--video-sensor-end-warn-ms", type=float, default=150.0)
    parser.add_argument("--video-sensor-end-fail-ms", type=float, default=400.0)
    return parser


def print_text_report(report: Report) -> None:
    print(f"Episode: {report.episode_dir}")
    print(f"Status:  {report.status}")
    print()

    if report.findings:
        print("Findings:")
        for item in report.findings:
            print(f"- [{item.severity}] {item.summary}")
            if item.details:
                details_text = ", ".join(f"{key}={value}" for key, value in item.details.items())
                print(f"  {details_text}")
    else:
        print("Findings:")
        print("- [INFO] 未发现异常")
    print()

    if report.videos:
        print("Videos:")
        for name in sorted(report.videos):
            item = report.videos[name]
            print(
                f"- {name}: codec={item.codec} duration_sec={item.duration_sec} "
                f"packet_count={item.packet_count} max_gap_ms={round(item.max_gap_ms, 3)} "
                f"dup={item.duplicate_pts_count} back={item.backward_pts_count}"
            )
    if report.sensors:
        print()
        print("Sensors:")
        for name in sorted(report.sensors):
            item = report.sensors[name]
            print(
                f"- {name}: count={item.count} median_gap_ms={round(item.median_gap_ms, 3)} "
                f"max_gap_ms={round(item.max_gap_ms, 3)} large_gap_count={item.large_gap_count}"
            )


def report_to_json(report: Report) -> str:
    return json.dumps(
        {
            "episode_dir": report.episode_dir,
            "status": report.status,
            "summary": report.summary,
            "findings": [asdict(item) for item in report.findings],
            "videos": {key: asdict(value) for key, value in report.videos.items()},
            "sensors": {key: asdict(value) for key, value in report.sensors.items()},
        },
        ensure_ascii=False,
        indent=2,
    )


def main() -> int:
    ensure_ffprobe()
    args = build_parser().parse_args()
    validator = EpisodeValidator(args)
    report = validator.validate()
    if args.json:
        print(report_to_json(report))
    else:
        print_text_report(report)
    return 2 if report.status == "FAIL" else 1 if report.status == "WARN" else 0


if __name__ == "__main__":
    raise SystemExit(main())
