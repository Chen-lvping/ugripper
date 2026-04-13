#!/usr/bin/env python3
from __future__ import annotations
import argparse
import json
import math
import re
import struct
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from statistics import median

DEPENDENCY_ERRORS = []

try:
    import cv2
except ImportError:
    cv2 = None
    DEPENDENCY_ERRORS.append("缺少 OpenCV：请安装 `python3-opencv`。")

try:
    import numpy as np
except ImportError:
    np = None
    DEPENDENCY_ERRORS.append("缺少 NumPy：请安装 `python3-numpy`。")

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
if StreamReader is None:
    DEPENDENCY_ERRORS.append("缺少 mcap：请执行 `python3 -m pip install --user mcap`。")

from path_utils import resolve_desktop_subdir, resolve_home_subdir, resolve_v2_backup_prefix


TACTILE_DAMAGE_LOG_ROOT_DIR = resolve_desktop_subdir(
    "tactile_damage_check",
    "USB_TACTILE_DAMAGE_LOG_DIR",
)
TACTILE_DAMAGE_BASELINE_DIR = resolve_home_subdir(
    "usb_tactile_damage_baseline",
    "USB_TACTILE_DAMAGE_BASELINE_DIR",
)

SCAN_SKIP_NAMES = {"logs", "runtime", "__pycache__"}
CROP_MARGIN_RATIO = 0.08
DEFAULT_STABLE_SAMPLE_COUNT = 200
DEFAULT_ENCODER_WINDOW_MS = 800
DEFAULT_DIFF_BINARY_THRESHOLD = 30.0
DEFAULT_MASK_RATIO_THRESHOLD = 0.05
DEFAULT_COMPONENT_RATIO_THRESHOLD = 0.03
DEFAULT_MEAN_ABS_THRESHOLD = 10.0
DEFAULT_CORRELATION_THRESHOLD = 0.97

ROOT_CANDIDATE_PREFIXES = [
    resolve_v2_backup_prefix("v2_usb_backups", "USB_V2_BACKUP_PREFIX"),
    resolve_v2_backup_prefix("V2_usb_backups", "USB_V2_LEGACY_BACKUP_PREFIX"),
    resolve_home_subdir("v1_3_usb_backups", "USB_V1_BACKUP_PREFIX"),
    resolve_home_subdir("usb_backups", "USB_V1_LEGACY_BACKUP_PREFIX"),
]


@dataclass(frozen=True)
class StreamSpec:
    name: str
    video_name: str
    info_key: str
    mcap_name: str


@dataclass
class StreamCapture:
    spec: StreamSpec
    frame: np.ndarray
    frame_index: int
    frame_count: int
    fps: float
    encoder_raw: int
    encoder_rad: float
    encoder_log_time_ns: int
    video_start_ns: int
    video_path: Path
    mcap_path: Path


@dataclass
class EpisodeCapture:
    episode_dir: Path
    metadata: dict
    info: dict
    layout_name: str
    captures: dict


LEGACY_STREAMS = [
    StreamSpec("tact_left", "tact_left.mkv", "tact_left_record_time_offset_us", "sensor_data.mcap"),
    StreamSpec("tact_right", "tact_right.mkv", "tact_right_record_time_offset_us", "sensor_data.mcap"),
]

V2_STREAMS = [
    StreamSpec("left_tcam_l", "left_tcam_l.mkv", "left_tcam_l_record_time_offset_us", "sensor_data_left.mcap"),
    StreamSpec("left_tcam_r", "left_tcam_r.mkv", "left_tcam_r_record_time_offset_us", "sensor_data_left.mcap"),
    StreamSpec("right_tcam_l", "right_tcam_l.mkv", "right_tcam_l_record_time_offset_us", "sensor_data_right.mcap"),
    StreamSpec("right_tcam_r", "right_tcam_r.mkv", "right_tcam_r_record_time_offset_us", "sensor_data_right.mcap"),
]

STREAM_LAYOUTS = [
    ("v2", V2_STREAMS),
    ("legacy", LEGACY_STREAMS),
]


class TactileCheckError(RuntimeError):
    pass


def ensure_runtime_dependencies():
    if not DEPENDENCY_ERRORS:
        return
    interpreter = Path(sys.executable).resolve() if sys.executable else "python3"
    raise TactileCheckError(
        f"运行触觉损坏校验缺少依赖（当前解释器: {interpreter}）：\n- "
        + "\n- ".join(DEPENDENCY_ERRORS)
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "清洗后校验触觉是否损坏："
            "读取首个基准开爪触觉帧，与最后一个有效 episode 的同类触觉帧做布尔差异检测。"
        )
    )
    parser.add_argument(
        "root_dir",
        nargs="?",
        help=(
            "批次目录、设备目录或 data 目录。"
            "不传时自动尝试选择最新的 v1/v2 备份批次。"
        ),
    )
    parser.add_argument(
        "--baseline-dir",
        default=str(TACTILE_DAMAGE_BASELINE_DIR),
        help=f"长期基准帧目录，默认 {TACTILE_DAMAGE_BASELINE_DIR}",
    )
    parser.add_argument(
        "--report-root",
        default=str(TACTILE_DAMAGE_LOG_ROOT_DIR),
        help=f"本次校验日志根目录，默认 {TACTILE_DAMAGE_LOG_ROOT_DIR}",
    )
    parser.add_argument(
        "--refresh-baseline",
        action="store_true",
        help="强制用本次首个有效 episode 重新生成长期基准帧。",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="只做校验与日志输出，不写入或更新长期基准帧。",
    )
    parser.add_argument(
        "--encoder-window-ms",
        type=int,
        default=DEFAULT_ENCODER_WINDOW_MS,
        help=f"开爪状态搜索窗口，默认 {DEFAULT_ENCODER_WINDOW_MS}ms。",
    )
    parser.add_argument(
        "--stable-sample-count",
        type=int,
        default=DEFAULT_STABLE_SAMPLE_COUNT,
        help=f"encoder 前段稳定样本数，默认 {DEFAULT_STABLE_SAMPLE_COUNT}。",
    )
    parser.add_argument(
        "--diff-binary-threshold",
        type=float,
        default=DEFAULT_DIFF_BINARY_THRESHOLD,
        help=f"二值差异阈值，默认 {DEFAULT_DIFF_BINARY_THRESHOLD}。",
    )
    parser.add_argument(
        "--mask-ratio-threshold",
        type=float,
        default=DEFAULT_MASK_RATIO_THRESHOLD,
        help=f"异常像素占比阈值，默认 {DEFAULT_MASK_RATIO_THRESHOLD:.2f}。",
    )
    parser.add_argument(
        "--component-ratio-threshold",
        type=float,
        default=DEFAULT_COMPONENT_RATIO_THRESHOLD,
        help=f"最大连通域占比阈值，默认 {DEFAULT_COMPONENT_RATIO_THRESHOLD:.2f}。",
    )
    parser.add_argument(
        "--mean-abs-threshold",
        type=float,
        default=DEFAULT_MEAN_ABS_THRESHOLD,
        help=f"平均绝对差阈值，默认 {DEFAULT_MEAN_ABS_THRESHOLD:.1f}。",
    )
    parser.add_argument(
        "--correlation-threshold",
        type=float,
        default=DEFAULT_CORRELATION_THRESHOLD,
        help=f"相关性阈值，默认 {DEFAULT_CORRELATION_THRESHOLD:.2f}。",
    )
    return parser.parse_args()


def _safe_iter_dirs(path):
    try:
        return sorted(
            [child for child in path.iterdir() if child.is_dir()],
            key=lambda child: child.name,
        )
    except Exception:
        return []


def _list_episode_dirs(path):
    if not path.is_dir():
        return []
    try:
        return sorted(
            [child for child in path.iterdir() if child.is_dir() and child.name.startswith("episode_")],
            key=lambda child: child.name,
        )
    except Exception:
        return []


def _is_episode_data_dir(path):
    return len(_list_episode_dirs(path)) > 0


def _add_data_root(found, seen, path):
    data_path = None
    if _is_episode_data_dir(path):
        data_path = path
    elif _is_episode_data_dir(path / "data"):
        data_path = path / "data"
    if data_path is None:
        return
    try:
        key = str(data_path.resolve())
    except Exception:
        key = str(data_path)
    if key in seen:
        return
    seen.add(key)
    found.append(data_path)


def discover_data_roots(root_dir):
    found = []
    seen = set()
    _add_data_root(found, seen, root_dir)
    if found:
        return found

    for child in _safe_iter_dirs(root_dir):
        if child.name in SCAN_SKIP_NAMES or child.name.endswith("_err"):
            continue
        _add_data_root(found, seen, child)
        for sub in _safe_iter_dirs(child):
            if sub.name in SCAN_SKIP_NAMES or sub.name.endswith("_err"):
                continue
            _add_data_root(found, seen, sub)
    return found


def resolve_data_source_root(root_dir):
    search_roots = [root_dir]

    if not root_dir.name.endswith("_err"):
        err_root = root_dir.parent / f"{root_dir.name}_err"
        if err_root.exists():
            general_root = err_root / "general"
            if general_root.exists():
                search_roots.append(general_root)
            search_roots.append(err_root)

    seen = set()
    for search_root in search_roots:
        try:
            key = str(search_root.resolve())
        except Exception:
            key = str(search_root)
        if key in seen or not search_root.exists():
            continue
        seen.add(key)
        data_roots = discover_data_roots(search_root)
        if data_roots:
            return search_root, data_roots

    return root_dir, []


def detect_latest_root():
    candidates = []
    seen = set()
    for prefix in ROOT_CANDIDATE_PREFIXES:
        parent = prefix.parent
        if parent.exists():
            for path in parent.glob(f"{prefix.name}_*"):
                _add_data_root(candidates, seen, path)
            if prefix.exists():
                _add_data_root(candidates, seen, prefix)
    if not candidates:
        return None
    return max(candidates, key=lambda path: path.stat().st_mtime)


def resolve_root_dir(root_dir_arg):
    if root_dir_arg:
        return Path(root_dir_arg).expanduser().resolve()
    detected = detect_latest_root()
    return detected.resolve() if detected else None


def sanitize_name(name):
    cleaned = re.sub(r"[^0-9A-Za-z._-]+", "_", name.strip())
    return cleaned.strip("._") or "unknown"


def read_json(path):
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        raise TactileCheckError(f"JSON 解析失败: {path}: {exc}") from exc
    except OSError as exc:
        raise TactileCheckError(f"读取失败: {path}: {exc}") from exc


def detect_layout(episode_dir):
    for layout_name, specs in STREAM_LAYOUTS:
        if all((episode_dir / spec.video_name).exists() for spec in specs):
            return layout_name, specs
    raise TactileCheckError(f"未识别的触觉视频布局: {episode_dir}")


def load_encoder_samples(mcap_path):
    channel_ids = set()
    samples = []
    try:
        with mcap_path.open("rb") as stream:
            for record in StreamReader(stream).records:
                record_type = type(record).__name__
                if record_type == "Channel" and "encoder" in record.topic:
                    channel_ids.add(record.id)
                elif record_type == "Message" and record.channel_id in channel_ids:
                    if len(record.data) < 8:
                        continue
                    raw, rad = struct.unpack("<if", record.data[:8])
                    samples.append((int(record.log_time), int(raw), float(rad)))
    except OSError as exc:
        raise TactileCheckError(f"读取 mcap 失败: {mcap_path}: {exc}") from exc
    if not samples:
        raise TactileCheckError(f"未在 mcap 中读取到 encoder 数据: {mcap_path}")
    return samples


def select_open_encoder_sample(samples, video_start_ns, encoder_window_ms, stable_sample_count):
    window_end_ns = video_start_ns + int(max(1, encoder_window_ms) * 1_000_000)
    window_samples = [sample for sample in samples if sample[0] <= window_end_ns]
    if not window_samples:
        window_samples = samples[: max(1, stable_sample_count)]

    nonzero_samples = [
        sample
        for sample in window_samples
        if not (sample[1] == 0 and abs(sample[2]) < 1e-6)
    ]
    if nonzero_samples:
        window_samples = nonzero_samples

    stable_samples = window_samples[: max(1, stable_sample_count)]
    target_raw = int(round(median(sample[1] for sample in stable_samples)))
    spread = max(abs(sample[1] - target_raw) for sample in stable_samples) if stable_samples else 0
    tolerance = max(50, spread + 20)
    chosen = next(
        (sample for sample in window_samples if abs(sample[1] - target_raw) <= tolerance),
        stable_samples[0],
    )
    return chosen, target_raw, tolerance


def read_video_frame(video_path, frame_index):
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        cap.release()
        raise TactileCheckError(f"视频无法打开: {video_path}")

    fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0) or 120.0
    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    frame_count = max(frame_count, 1)
    frame_index = max(0, min(frame_count - 1, int(frame_index)))
    cap.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
    ok, frame = cap.read()
    cap.release()
    if not ok or frame is None:
        raise TactileCheckError(f"读取视频帧失败: {video_path} @ frame {frame_index}")
    return frame, fps, frame_count, frame_index


def read_video_props(video_path):
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        cap.release()
        raise TactileCheckError(f"视频无法打开: {video_path}")
    fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0) or 120.0
    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    cap.release()
    return fps, max(frame_count, 1)


def extract_stream_capture(ep_dir, info, spec, args, encoder_cache):
    video_path = ep_dir / spec.video_name
    mcap_path = ep_dir / spec.mcap_name
    if not video_path.exists() or video_path.stat().st_size <= 0:
        raise TactileCheckError(f"视频缺失或空文件: {video_path}")
    if not mcap_path.exists() or mcap_path.stat().st_size <= 0:
        raise TactileCheckError(f"mcap 缺失或空文件: {mcap_path}")
    if spec.info_key not in info:
        raise TactileCheckError(f"info.json 缺少字段 {spec.info_key}: {ep_dir}")

    if mcap_path not in encoder_cache:
        encoder_cache[mcap_path] = load_encoder_samples(mcap_path)
    samples = encoder_cache[mcap_path]

    video_start_ns = int(info[spec.info_key]) * 1000
    chosen, _, _ = select_open_encoder_sample(
        samples,
        video_start_ns,
        encoder_window_ms=args.encoder_window_ms,
        stable_sample_count=args.stable_sample_count,
    )

    fps, frame_count = read_video_props(video_path)
    frame_guess = round((chosen[0] - video_start_ns) / 1e9 * fps)
    frame_guess = max(0, min(frame_count - 1, frame_guess))
    frame, fps, frame_count, frame_index = read_video_frame(video_path, frame_guess)
    return StreamCapture(
        spec=spec,
        frame=frame,
        frame_index=frame_index,
        frame_count=frame_count,
        fps=fps,
        encoder_raw=chosen[1],
        encoder_rad=chosen[2],
        encoder_log_time_ns=chosen[0],
        video_start_ns=video_start_ns,
        video_path=video_path,
        mcap_path=mcap_path,
    )


def extract_episode_capture(ep_dir, args):
    metadata = read_json(ep_dir / "metadata.json")
    info = read_json(ep_dir / "info.json")
    layout_name, specs = detect_layout(ep_dir)
    encoder_cache = {}
    captures = {}
    for spec in specs:
        captures[spec.name] = extract_stream_capture(ep_dir, info, spec, args, encoder_cache)
    return EpisodeCapture(
        episode_dir=ep_dir,
        metadata=metadata,
        info=info,
        layout_name=layout_name,
        captures=captures,
    )


def find_valid_episode_capture(data_root, args, reverse=False):
    episodes = _list_episode_dirs(data_root)
    if reverse:
        episodes = list(reversed(episodes))
    last_error = None
    for ep_dir in episodes:
        try:
            return extract_episode_capture(ep_dir, args)
        except TactileCheckError as exc:
            last_error = exc
            continue
    if last_error is not None:
        raise TactileCheckError(f"未找到有效 episode: {data_root}: {last_error}") from last_error
    raise TactileCheckError(f"未找到任何 episode: {data_root}")


def preprocess_frame(frame):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    height, width = gray.shape
    margin_y = int(height * CROP_MARGIN_RATIO)
    margin_x = int(width * CROP_MARGIN_RATIO)
    cropped = gray[margin_y : height - margin_y, margin_x : width - margin_x]
    if cropped.size == 0:
        cropped = gray
    blurred = cv2.GaussianBlur(cropped, (9, 9), 0)
    normalized = cv2.normalize(blurred.astype(np.float32), None, 0, 255, cv2.NORM_MINMAX)
    return normalized.astype(np.uint8)


def compute_damage_metrics(baseline_frame, current_frame, args):
    baseline_proc = preprocess_frame(baseline_frame)
    current_proc = preprocess_frame(current_frame)

    if baseline_proc.shape != current_proc.shape:
        current_proc = cv2.resize(
            current_proc,
            (baseline_proc.shape[1], baseline_proc.shape[0]),
            interpolation=cv2.INTER_LINEAR,
        )

    diff = cv2.absdiff(baseline_proc, current_proc)
    _, mask = cv2.threshold(
        diff,
        max(0.0, float(args.diff_binary_threshold)),
        255,
        cv2.THRESH_BINARY,
    )
    kernel_open = np.ones((5, 5), np.uint8)
    kernel_close = np.ones((11, 11), np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel_open)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel_close)

    component_ratio = 0.0
    if np.any(mask):
        num_labels, _, stats, _ = cv2.connectedComponentsWithStats(mask)
        largest = max(int(stats[idx, cv2.CC_STAT_AREA]) for idx in range(1, num_labels))
        component_ratio = largest / float(mask.size)

    mask_ratio = float((mask > 0).mean())
    mean_abs = float(diff.mean())
    correlation = float(np.corrcoef(baseline_proc.flatten(), current_proc.flatten())[0, 1])
    if math.isnan(correlation):
        correlation = 1.0

    damaged = (
        mask_ratio >= float(args.mask_ratio_threshold)
        or component_ratio >= float(args.component_ratio_threshold)
        or mean_abs >= float(args.mean_abs_threshold)
        or correlation <= float(args.correlation_threshold)
    )

    overlay = cv2.cvtColor(current_proc, cv2.COLOR_GRAY2BGR)
    overlay[mask > 0] = (0, 0, 255)

    return {
        "damaged": bool(damaged),
        "mean_abs_diff": mean_abs,
        "mask_ratio": mask_ratio,
        "largest_component_ratio": component_ratio,
        "correlation": correlation,
        "mask": mask,
        "baseline_processed": baseline_proc,
        "current_processed": current_proc,
        "overlay": overlay,
    }


def add_label(image, text):
    if len(image.shape) == 2:
        vis = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
    else:
        vis = image.copy()
    pad = np.full((40, vis.shape[1], 3), 255, dtype=np.uint8)
    cv2.putText(
        pad,
        text,
        (10, 28),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (0, 0, 0),
        2,
        cv2.LINE_AA,
    )
    return np.vstack([pad, vis])


def save_preview(preview_path, baseline_frame, current_frame, metrics):
    tiles = [
        add_label(metrics["baseline_processed"], "baseline"),
        add_label(metrics["current_processed"], "current"),
        add_label(metrics["overlay"], "diff-mask"),
    ]
    canvas = np.concatenate(tiles, axis=1)
    cv2.imwrite(str(preview_path), canvas)


def save_baseline(device_baseline_dir, stream_name, capture, metadata, source_root, dry_run):
    baseline_img_path = device_baseline_dir / f"{stream_name}.png"
    baseline_meta_path = device_baseline_dir / f"{stream_name}.json"
    payload = {
        "device_id": metadata.get("device_id") or device_baseline_dir.name,
        "stream_name": stream_name,
        "source_root": str(source_root),
        "source_episode": capture.episode_dir.name,
        "video_name": capture.captures[stream_name].spec.video_name,
        "frame_index": capture.captures[stream_name].frame_index,
        "encoder_raw": capture.captures[stream_name].encoder_raw,
        "encoder_rad": capture.captures[stream_name].encoder_rad,
        "captured_at": datetime.now().isoformat(timespec="seconds"),
    }
    if not dry_run:
        device_baseline_dir.mkdir(parents=True, exist_ok=True)
        cv2.imwrite(str(baseline_img_path), capture.captures[stream_name].frame)
        baseline_meta_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2))
    return baseline_img_path, baseline_meta_path, payload


def load_baseline(device_baseline_dir, stream_name):
    baseline_img_path = device_baseline_dir / f"{stream_name}.png"
    baseline_meta_path = device_baseline_dir / f"{stream_name}.json"
    if not baseline_img_path.exists():
        return None, None, None
    image = cv2.imread(str(baseline_img_path), cv2.IMREAD_COLOR)
    if image is None:
        raise TactileCheckError(f"基准帧损坏，无法读取: {baseline_img_path}")
    metadata = {}
    if baseline_meta_path.exists():
        metadata = read_json(baseline_meta_path)
    return image, baseline_img_path, metadata


def build_report_root(report_root, root_dir):
    report_root.mkdir(parents=True, exist_ok=True)
    root_name = root_dir.name
    if root_name == "data" and root_dir.parent.name:
        root_name = root_dir.parent.name
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_dir = report_root / f"tactile_damage_check_{root_name}_{timestamp}"
    report_dir.mkdir(parents=True, exist_ok=True)
    return report_dir


def device_label_from_metadata(metadata, data_root):
    device_id = str(metadata.get("device_id") or "").strip()
    if device_id:
        return device_id
    if data_root.name == "data" and data_root.parent.name:
        return data_root.parent.name
    return data_root.name


def run_for_data_root(data_root, args, baseline_dir, report_dir, source_root):
    first_capture = find_valid_episode_capture(data_root, args, reverse=False)
    last_capture = find_valid_episode_capture(data_root, args, reverse=True)

    metadata = first_capture.metadata
    device_label = device_label_from_metadata(metadata, data_root)
    device_slug = sanitize_name(device_label)
    device_baseline_dir = baseline_dir / device_slug
    device_report_dir = report_dir / device_slug
    device_report_dir.mkdir(parents=True, exist_ok=True)

    if first_capture.layout_name != last_capture.layout_name:
        raise TactileCheckError(
            f"首尾 episode 触觉布局不一致: {first_capture.episode_dir} vs {last_capture.episode_dir}"
        )

    stream_reports = []
    any_damage = False

    for stream_name, current_stream_capture in sorted(last_capture.captures.items()):
        baseline_image = None
        baseline_path = None
        baseline_meta = None
        baseline_source = "stored"

        if not args.refresh_baseline:
            baseline_image, baseline_path, baseline_meta = load_baseline(device_baseline_dir, stream_name)

        if baseline_image is None:
            baseline_source = "first_episode"
            first_stream_capture = first_capture.captures[stream_name]
            baseline_image = first_stream_capture.frame.copy()
            baseline_path, _, baseline_meta = save_baseline(
                device_baseline_dir,
                stream_name,
                first_capture,
                metadata,
                source_root,
                dry_run=args.dry_run,
            )

        metrics = compute_damage_metrics(
            baseline_frame=baseline_image,
            current_frame=current_stream_capture.frame,
            args=args,
        )
        any_damage = any_damage or metrics["damaged"]

        preview_path = device_report_dir / f"{stream_name}_preview.png"
        current_path = device_report_dir / f"{stream_name}_current.png"
        cv2.imwrite(str(current_path), current_stream_capture.frame)
        save_preview(preview_path, baseline_image, current_stream_capture.frame, metrics)

        stream_reports.append(
            {
                "stream_name": stream_name,
                "damaged": bool(metrics["damaged"]),
                "baseline_source": baseline_source,
                "baseline_image_path": str(baseline_path) if baseline_path else None,
                "baseline_meta": baseline_meta or {},
                "current_image_path": str(current_path),
                "preview_path": str(preview_path),
                "baseline_episode": (baseline_meta or {}).get("source_episode") or first_capture.episode_dir.name,
                "current_episode": last_capture.episode_dir.name,
                "current_frame_index": current_stream_capture.frame_index,
                "current_encoder_raw": current_stream_capture.encoder_raw,
                "current_encoder_rad": current_stream_capture.encoder_rad,
                "current_video_name": current_stream_capture.spec.video_name,
                "mean_abs_diff": metrics["mean_abs_diff"],
                "mask_ratio": metrics["mask_ratio"],
                "largest_component_ratio": metrics["largest_component_ratio"],
                "correlation": metrics["correlation"],
            }
        )

    return {
        "data_root": str(data_root),
        "device_id": device_label,
        "layout_name": first_capture.layout_name,
        "report_dir": str(device_report_dir),
        "baseline_dir": str(device_baseline_dir),
        "first_episode": first_capture.episode_dir.name,
        "last_episode": last_capture.episode_dir.name,
        "damaged": any_damage,
        "streams": stream_reports,
    }


def write_damage_log(report_dir, device_reports):
    damage_log_path = report_dir / "damage.log"
    lines = []
    for device_report in device_reports:
        for stream_report in device_report["streams"]:
            if not stream_report["damaged"]:
                continue
            lines.append(
                (
                    f"[DAMAGED] device={device_report['device_id']} "
                    f"stream={stream_report['stream_name']} "
                    f"baseline={stream_report['baseline_episode']} "
                    f"current={stream_report['current_episode']} "
                    f"mean_abs={stream_report['mean_abs_diff']:.2f} "
                    f"mask_ratio={stream_report['mask_ratio']:.4f} "
                    f"component_ratio={stream_report['largest_component_ratio']:.4f} "
                    f"corr={stream_report['correlation']:.4f}"
                )
            )
    damage_log_path.write_text("\n".join(lines) + ("\n" if lines else ""))
    return damage_log_path


def main():
    try:
        ensure_runtime_dependencies()
    except TactileCheckError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    args = parse_args()
    root_dir = resolve_root_dir(args.root_dir)
    if root_dir is None:
        print("未找到可处理的批次目录。", file=sys.stderr)
        return 1
    if not root_dir.exists():
        print(f"输入目录不存在: {root_dir}", file=sys.stderr)
        return 1

    data_source_root, data_roots = resolve_data_source_root(root_dir)
    if not data_roots:
        print(f"未在目录中发现 episode 数据: {root_dir}", file=sys.stderr)
        return 1

    baseline_dir = Path(args.baseline_dir).expanduser().resolve()
    report_root = Path(args.report_root).expanduser().resolve()
    report_dir = build_report_root(report_root, root_dir)

    print(f"输入目录: {root_dir}")
    if data_source_root != root_dir:
        print(f"实际校验目录: {data_source_root}")
    print(f"发现数据目录数: {len(data_roots)}")
    print(f"长期基准目录: {baseline_dir}")
    print(f"本次日志目录: {report_dir}")

    device_reports = []
    any_damage = False

    for data_root in data_roots:
        try:
            device_report = run_for_data_root(
                data_root,
                args,
                baseline_dir,
                report_dir,
                data_source_root,
            )
        except TactileCheckError as exc:
            print(f"[ERROR] {data_root}: {exc}")
            device_reports.append(
                {
                    "data_root": str(data_root),
                    "damaged": False,
                    "error": str(exc),
                    "streams": [],
                }
            )
            continue

        device_reports.append(device_report)
        any_damage = any_damage or device_report["damaged"]

        for stream_report in device_report["streams"]:
            status = "DAMAGED" if stream_report["damaged"] else "OK"
            print(
                f"[{status}] device={device_report['device_id']} "
                f"stream={stream_report['stream_name']} "
                f"baseline={stream_report['baseline_episode']} "
                f"current={stream_report['current_episode']} "
                f"mean_abs={stream_report['mean_abs_diff']:.2f} "
                f"mask_ratio={stream_report['mask_ratio']:.4f} "
                f"component_ratio={stream_report['largest_component_ratio']:.4f} "
                f"corr={stream_report['correlation']:.4f}"
            )

    report_payload = {
        "root_dir": str(root_dir),
        "data_source_root": str(data_source_root),
        "report_dir": str(report_dir),
        "baseline_dir": str(baseline_dir),
        "refresh_baseline": bool(args.refresh_baseline),
        "dry_run": bool(args.dry_run),
        "damaged": any_damage,
        "devices": device_reports,
    }

    report_json_path = report_dir / "report.json"
    report_json_path.write_text(json.dumps(report_payload, ensure_ascii=False, indent=2))
    damage_log_path = write_damage_log(report_dir, device_reports)

    print(f"报告 JSON: {report_json_path}")
    print(f"损坏日志: {damage_log_path}")

    if any_damage:
        print("检测到触觉疑似损坏，请检查 damage.log 和预览图。")
        return 2

    print("未检测到触觉损坏。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
