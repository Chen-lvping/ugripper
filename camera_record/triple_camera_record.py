#!/usr/bin/env python3
"""
三相机同步录制工具（主摄 FFmpeg + 触觉 GStreamer 混合实现）。

- 主相机：FFmpeg `v4l2(NV12) -> h26x_rkmpp -> mkv`
- 触觉相机：GStreamer `v4l2src(MJPEG) -> mppjpegdec -> mpph26xenc -> mkv`
- 时间戳恢复方式：frame_system_time_us = frame_pts_us + <camera>_record_time_offset_us

说明：
- 主摄回退到旧 FFmpeg 链路，避免 Gst 主摄码率偏高且不涉及硬件解码收益。
- 触觉相机继续使用 Gst，以保留 MJPEG 硬件解码带来的 CPU 收益。
- 不再输出 CSV。
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, Optional

try:
    import gi
except ModuleNotFoundError:
    system_python = "/usr/bin/python3"
    if sys.executable != system_python and os.path.exists(system_python):
        os.execv(system_python, [system_python, *sys.argv])
    raise SystemExit(
        "当前 Python 环境缺少 gi；请使用 /usr/bin/python3 运行，"
        "或安装 python3-gi / gir1.2-gstreamer-1.0"
    )

gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import GLib, Gst

BOOT_TIME_OFFSET_US = int((time.time() - time.monotonic()) * 1_000_000)
FFMPEG_PTS_RE = re.compile(r"pts:\s*(\d+)\s+pts_time:")


def get_gst_encoder_element(codec: str) -> str:
    return "mpph264enc" if codec == "h264" else "mpph265enc"


def get_gst_parser_element(codec: str) -> str:
    return "h264parse" if codec == "h264" else "h265parse"


def get_ffmpeg_encoder_name(codec: str) -> str:
    return "h264_rkmpp" if codec == "h264" else "hevc_rkmpp"


def gst_quote(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


@dataclass
class CameraConfig:
    name: str
    device: str
    width: int
    height: int
    fps: int
    format: str
    output: str


@dataclass
class CameraRuntime:
    config: CameraConfig
    frame_count: int = 0
    first_pts_ns: Optional[int] = None
    first_system_time_us: Optional[int] = None
    first_system_time_str: Optional[str] = None
    record_time_offset_us: Optional[int] = None


class TripleCameraRecorder:
    def __init__(self, output_dir: str, codec: str, duration: int):
        self.output_dir = output_dir
        self.codec = codec
        self.duration = duration
        self.loop = GLib.MainLoop()
        self.pipeline: Optional[Gst.Pipeline] = None
        self.bus = None
        self.stop_requested = False
        self.eos_sent = False
        self.force_quit_source = 0
        self.started_all = False
        self.info_written = False
        self.error_message: Optional[str] = None
        self.global_start_time = 0.0
        self.cam_process: Optional[subprocess.Popen] = None
        self.cam_log_thread: Optional[threading.Thread] = None
        self.cam_stop_event = threading.Event()
        self.cam_failed = False
        self.lock = threading.Lock()

        os.makedirs(self.output_dir, exist_ok=True)
        self.configs = self._build_configs()
        self.cameras: Dict[str, CameraRuntime] = {
            config.name: CameraRuntime(config=config) for config in self.configs
        }

        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _build_configs(self):
        return [
            CameraConfig(
                name="cam",
                device="/dev/video11",
                width=1920,
                height=1080,
                fps=60,
                format="nv12",
                output=os.path.join(self.output_dir, "cam.mkv"),
            ),
            CameraConfig(
                name="tact_left",
                device="/dev/left_tcam",
                width=640,
                height=480,
                fps=120,
                format="mjpeg",
                output=os.path.join(self.output_dir, "tact_left.mkv"),
            ),
            CameraConfig(
                name="tact_right",
                device="/dev/right_tcam",
                width=640,
                height=480,
                fps=120,
                format="mjpeg",
                output=os.path.join(self.output_dir, "tact_right.mkv"),
            ),
        ]

    def _signal_handler(self, signum, _frame):
        sig_name = "SIGINT" if signum == signal.SIGINT else "SIGTERM"
        print(f"\n[Camera] 收到 {sig_name} 信号，正在安全停止录制...", flush=True)
        GLib.idle_add(self.request_stop, False)

    def _get_gst_encoder_properties(self, camera_name: str) -> str:
        return "rc-mode=fixqp qp-init=30 qp-max=38 qp-min=24 qp-max-i=38 qp-min-i=20"

    def _build_tactile_branch(self, config: CameraConfig) -> str:
        encoder = get_gst_encoder_element(self.codec)
        parser = get_gst_parser_element(self.codec)
        enc_props = self._get_gst_encoder_properties(config.name)
        probe_name = f"probe_{config.name}"
        caps = (
            f"image/jpeg,width={config.width},height={config.height},"
            f"framerate={config.fps}/1"
        )
        return (
            f"v4l2src device={config.device} do-timestamp=false ! "
            f"{caps} ! "
            f"identity name={probe_name} signal-handoffs=true silent=true ! "
            f"queue ! jpegparse ! mppjpegdec ! "
            f"video/x-raw,format=NV12 ! "
            f"{encoder} {enc_props} ! "
            f"{parser} ! "
            f"matroskamux ! filesink location={gst_quote(config.output)}"
        )

    def _build_tactile_pipeline_description(self) -> str:
        return " ".join(
            self._build_tactile_branch(config)
            for config in self.configs
            if config.format == "mjpeg"
        )

    def _build_cam_ffmpeg_cmd(self, config: CameraConfig):
        encoder = get_ffmpeg_encoder_name(self.codec)
        return [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "info",
            "-nostats",
            "-y",
            "-f",
            "v4l2",
            "-input_format",
            "nv12",
            "-video_size",
            f"{config.width}x{config.height}",
            "-copyts",
            "-i",
            config.device,
            "-vf",
            f"showinfo,fps={config.fps}",
            "-c:v",
            encoder,
            "-rc_mode",
            "CQP",
            "-qp_init",
            "30",
            "-qp_max",
            "35",
            "-qp_min",
            "20",
            "-qp_max_i",
            "35",
            "-qp_min_i",
            "18",
            config.output,
        ]

    def _write_info_json(self):
        info_path = os.path.join(self.output_dir, "info.json")
        data = {
            "boot_time_offset": BOOT_TIME_OFFSET_US / 1_000_000,
            "boot_time_offset_us": BOOT_TIME_OFFSET_US,
        }
        for camera_name, runtime in self.cameras.items():
            if runtime.record_time_offset_us is not None:
                data[f"{camera_name}_record_time_offset_us"] = runtime.record_time_offset_us
        with open(info_path, "w") as file_obj:
            json.dump(data, file_obj, indent=4)
        self.info_written = True
        print(f"[Info] 偏移量已写入: {info_path}", flush=True)

    def _maybe_write_camera_offsets(self):
        if self.info_written:
            current = {}
            info_path = os.path.join(self.output_dir, "info.json")
            if os.path.exists(info_path):
                try:
                    with open(info_path, "r") as file_obj:
                        current = json.load(file_obj)
                except Exception:
                    current = {}
            current["boot_time_offset"] = BOOT_TIME_OFFSET_US / 1_000_000
            current["boot_time_offset_us"] = BOOT_TIME_OFFSET_US
            for camera_name, runtime in self.cameras.items():
                if runtime.record_time_offset_us is not None:
                    current[f"{camera_name}_record_time_offset_us"] = runtime.record_time_offset_us
            with open(info_path, "w") as file_obj:
                json.dump(current, file_obj, indent=4)
            return

        if any(runtime.record_time_offset_us is None for runtime in self.cameras.values()):
            return
        self._write_info_json()

    def _mark_first_frame(self, camera_name: str, pts_ns: int, system_time_us: int):
        runtime = self.cameras[camera_name]
        if runtime.first_pts_ns is not None:
            return
        runtime.frame_count += 1
        runtime.first_pts_ns = pts_ns
        runtime.first_system_time_us = system_time_us
        runtime.first_system_time_str = datetime.fromtimestamp(
            system_time_us / 1_000_000
        ).strftime("%H:%M:%S.%f")
        runtime.record_time_offset_us = system_time_us - (pts_ns // 1000)
        print(
            f"[{camera_name}] 首帧锁定: PTS={pts_ns} ns | Sys={runtime.first_system_time_str} | Offset={runtime.record_time_offset_us}us",
            flush=True,
        )
        self._maybe_write_camera_offsets()
        self._maybe_mark_started()

    def _on_handoff(self, _identity, buffer, camera_name: str):
        pts_ns = int(buffer.pts)
        if pts_ns < 0:
            return
        runtime = self.cameras[camera_name]
        runtime.frame_count += 1
        if runtime.first_pts_ns is None:
            self._mark_first_frame(camera_name, pts_ns, time.time_ns() // 1000)

    def _cam_log_loop(self):
        process = self.cam_process
        if process is None or process.stderr is None:
            return
        runtime = self.cameras["cam"]
        while not self.cam_stop_event.is_set():
            line = process.stderr.readline()
            if not line:
                if process.poll() is not None:
                    break
                continue
            match = FFMPEG_PTS_RE.search(line)
            if match:
                pts_us = int(match.group(1))
                runtime.frame_count += 1
                if runtime.first_pts_ns is None:
                    system_time_us = BOOT_TIME_OFFSET_US + pts_us
                    self._mark_first_frame("cam", pts_us * 1000, system_time_us)
        if process.poll() not in (None, 0) and not self.stop_requested:
            self.cam_failed = True
            self.error_message = f"cam ffmpeg exited with code {process.poll()}"
            print(f"[FFmpeg][ERROR] {self.error_message}", flush=True)
            GLib.idle_add(self.loop.quit)

    def _start_cam_ffmpeg(self):
        config = next(config for config in self.configs if config.name == "cam")
        cmd = self._build_cam_ffmpeg_cmd(config)
        print(f"[cam] 启动 FFmpeg: {' '.join(cmd)}", flush=True)
        self.cam_process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            bufsize=1,
        )
        self.cam_log_thread = threading.Thread(target=self._cam_log_loop, daemon=True)
        self.cam_log_thread.start()

    def _stop_cam_ffmpeg(self):
        self.cam_stop_event.set()
        if self.cam_process is None:
            return
        process = self.cam_process
        if process.poll() is None:
            try:
                if process.stdin:
                    process.stdin.write("q\n")
                    process.stdin.flush()
            except Exception:
                pass
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if self.cam_log_thread is not None:
            self.cam_log_thread.join(timeout=1)
        self.cam_process = None

    def _maybe_mark_started(self):
        if self.started_all:
            return
        if any(runtime.first_pts_ns is None for runtime in self.cameras.values()):
            return
        self.started_all = True
        self.global_start_time = time.time()
        print(f"录制开始! 系统时间: {datetime.fromtimestamp(self.global_start_time)}", flush=True)
        self._show_summary()

    def _show_summary(self):
        print("\n" + "=" * 60, flush=True)
        print("同步分析报告", flush=True)
        print("=" * 60, flush=True)
        first_system_values_us = [
            runtime.first_system_time_us
            for runtime in self.cameras.values()
            if runtime.first_system_time_us is not None
        ]
        if not first_system_values_us:
            print("尚未捕获到首帧", flush=True)
            return
        min_system_us = min(first_system_values_us)
        print("\n【首帧同步性】", flush=True)
        print("-" * 50, flush=True)
        for camera_name, runtime in sorted(self.cameras.items()):
            if runtime.first_pts_ns is None:
                print(f"  {camera_name:<12}: 未收到首帧", flush=True)
                continue
            offset_ms = (runtime.first_system_time_us - min_system_us) / 1000.0
            print(
                f"  {camera_name:<12}: 延迟 +{offset_ms:.2f}ms | Sys={runtime.first_system_time_str} | RecordOffset={runtime.record_time_offset_us}us",
                flush=True,
            )

    def _on_bus_message(self, _bus, message):
        if message.type == Gst.MessageType.ERROR:
            err, debug_info = message.parse_error()
            self.error_message = f"{err} | {debug_info}"
            print(f"[GST][ERROR] {self.error_message}", flush=True)
            self.loop.quit()
        elif message.type == Gst.MessageType.EOS:
            print("[GST] 收到 EOS，准备退出", flush=True)
            self.loop.quit()
        return True

    def _force_quit(self):
        print("[GST] 等待 EOS 超时，强制退出主循环", flush=True)
        self.loop.quit()
        self.force_quit_source = 0
        return False

    def request_stop(self, _from_timeout: bool):
        if self.stop_requested:
            return False
        self.stop_requested = True
        self._stop_cam_ffmpeg()
        if self.pipeline is not None and not self.eos_sent:
            self.eos_sent = True
            print("[GST] 发送 EOS 停止录制", flush=True)
            self.pipeline.send_event(Gst.Event.new_eos())
            self.force_quit_source = GLib.timeout_add_seconds(5, self._force_quit)
        return False

    def run(self) -> int:
        Gst.init(None)
        self._write_info_json()

        print("=" * 70, flush=True)
        print(f"三相机同步录制系统 ({self.codec.upper()} 混合链路: CAM=FFmpeg, TACT=GST)", flush=True)
        print("=" * 70, flush=True)
        print(f"输出目录: {self.output_dir}", flush=True)
        print(f"主进程 PID: {os.getpid()}  (Shell可用 kill -2 {os.getpid()} 停止)", flush=True)
        if self.duration > 0:
            print(f"录制时长: {self.duration} 秒", flush=True)
        else:
            print("录制时长: 无限制 (等待外部信号停止)", flush=True)
        print("=" * 70, flush=True)

        pipeline_desc = self._build_tactile_pipeline_description()
        print("[GST] 触觉 Pipeline 已创建", flush=True)
        try:
            self.pipeline = Gst.parse_launch(pipeline_desc)
        except GLib.Error as error:
            print(f"[GST][ERROR] Pipeline 创建失败: {error}", flush=True)
            return 1

        self.bus = self.pipeline.get_bus()
        self.bus.add_signal_watch()
        self.bus.connect("message", self._on_bus_message)

        for config in self.configs:
            if config.format != "mjpeg":
                continue
            identity = self.pipeline.get_by_name(f"probe_{config.name}")
            if identity is None:
                print(f"[GST][ERROR] 未找到 identity: probe_{config.name}", flush=True)
                return 1
            identity.connect("handoff", self._on_handoff, config.name)

        if self.duration > 0:
            GLib.timeout_add_seconds(self.duration, self.request_stop, True)

        result = self.pipeline.set_state(Gst.State.PLAYING)
        if result == Gst.StateChangeReturn.FAILURE:
            print("[GST][ERROR] Pipeline 启动失败", flush=True)
            self.pipeline.set_state(Gst.State.NULL)
            return 1

        self._start_cam_ffmpeg()

        try:
            self.loop.run()
        finally:
            if self.force_quit_source:
                GLib.source_remove(self.force_quit_source)
                self.force_quit_source = 0
            self._stop_cam_ffmpeg()
            if self.pipeline is not None:
                self.pipeline.set_state(Gst.State.NULL)
            self._maybe_write_camera_offsets()

        return 1 if self.error_message else 0


def main():
    parser = argparse.ArgumentParser(
        description="三相机同步录制工具 (主摄 FFmpeg + 触觉 GStreamer 混合实现)"
    )
    parser.add_argument("-d", "--duration", type=int, default=0, help="录制时长(秒)，0表示无限制")
    parser.add_argument("--output-dir", type=str, required=True, help="数采统一的完整输出目录路径")
    parser.add_argument("--codec", type=str, choices=["h264", "h265"], default="h264", help="硬件编码格式，默认 h264")
    args = parser.parse_args()
    recorder = TripleCameraRecorder(args.output_dir, args.codec, args.duration)
    sys.exit(recorder.run())


if __name__ == "__main__":
    main()
