#!/usr/bin/env python3
"""
三相机同步录制工具 (H.265 硬件加速版 - Shell脚本适配)

基于 RK3576 MPP 硬件编解码器优化:
- 使用 mjpeg_rkmpp 硬件解码
- 使用 hevc_rkmpp 硬件编码
- 文件体积减少 90%+，CPU 占用极低
"""

import argparse
import csv
import multiprocessing
import os
import signal
import statistics
import subprocess
import time
from datetime import datetime
from multiprocessing import Barrier, Event, Manager, Process

import av


class FFmpegHardwareEncoder:
    """
    封装 FFmpeg 硬件编码管线，对外提供类似 PyAV 的写入接口。
    实现: Pipe(MJPEG) -> HW Decode(mjpeg_rkmpp) -> HW Encode(hevc_rkmpp) -> File
    """

    def __init__(self, output_file, width, height, fps):
        self.output_file = output_file
        # 动态调整码率: 1080p 用 10M, 480p 用 4M
        bitrate = "10M" if width >= 1920 else "4M"

        # 构建全硬件加速命令
        self.cmd = [
            "ffmpeg",
            "-y",
            "-r",
            str(fps),
            "-c:v",
            "mjpeg_rkmpp",
            "-f",
            "mjpeg",
            "-i",
            "pipe:0",
            "-c:v",
            "hevc_rkmpp",
            "-b:v",
            bitrate,
            "-maxrate",
            bitrate,
            "-bufsize",
            str(int(bitrate[:-1]) * 2) + "M",
            "-r",
            str(fps),
            output_file,
        ]

        self.process = subprocess.Popen(
            self.cmd, stdin=subprocess.PIPE, stderr=subprocess.DEVNULL
        )
        print(f"[{os.getpid()}] 硬件编码器已启动: HEVC (H.265)")

    def write(self, packet):
        """写入原始 MJPEG 数据包"""
        try:
            self.process.stdin.write(bytes(packet))
        except BrokenPipeError:
            print(f"[{os.getpid()}] 错误: 编码器管道已断开")

    def close(self):
        """关闭编码器"""
        if self.process:
            if self.process.stdin:
                self.process.stdin.close()
            self.process.wait()
            print(f"[{os.getpid()}] 硬件编码器已关闭")


def _record_camera_process(config, barrier, start_event, stop_event, first_frame_info):
    """单相机录制进程"""
    cam_name = config["name"]
    device_path = config["device"]
    width, height = config["width"], config["height"]
    fps = config["fps"]
    duration = config["duration"]
    output_file = config["output"]
    csv_file = os.path.splitext(output_file)[0] + ".csv"

    print(f"[{cam_name}] 初始化 ({device_path})...")

    input_container = None
    encoder = None
    csv_f = None

    try:
        # 打开摄像头 (V4L2 + MJPEG)
        input_container = av.open(
            device_path,
            format="v4l2",
            options={
                "framerate": str(fps),
                "video_size": f"{width}x{height}",
                "input_format": "mjpeg",
            },
        )
        input_stream = input_container.streams.video[0]
        time_base = float(input_stream.time_base)

        # 检查实际分辨率
        if input_stream.width != width or input_stream.height != height:
            print(
                f"[{cam_name}] 警告: 请求 {width}x{height}, 实际 {input_stream.width}x{input_stream.height}"
            )

        # 创建硬件编码器
        encoder = FFmpegHardwareEncoder(output_file, width, height, fps)

        # CSV 文件
        csv_f = open(csv_file, "w", newline="", buffering=1)
        csv_writer = csv.writer(csv_f)
        csv_writer.writerow(
            [
                "frame_id",
                "kernel_pts_us",
                "kernel_pts_absolute_us",
                "kernel_interval_us",
                "system_time",
            ]
        )

        print(f"[{cam_name}] 就绪，等待同步...")

        # 同步屏障
        if barrier:
            try:
                barrier.wait()
            except multiprocessing.BrokenBarrierError:
                return

        # 录制状态变量
        frames_count = 0
        first_kernel_pts_us = None
        last_rel_pts_us = 0
        dropped_frames = 0

        # 新鲜帧检测参数
        expected_interval_us = 1_000_000 // fps
        min_fresh_interval_us = expected_interval_us * 0.5
        last_system_time = None
        found_fresh = False

        # === 主录制循环 ===
        for packet in input_container.demux(input_stream):
            # 检查退出标志
            if stop_event.is_set():
                break

            if packet.pts is None:
                continue

            # 等待全局启动信号 (清空缓冲区阶段)
            if not start_event.is_set():
                continue

            current_time = time.time()
            kernel_pts_us = int(packet.pts * time_base * 1_000_000)

            # --- 新鲜帧检测逻辑 ---
            if not found_fresh:
                if last_system_time is not None:
                    sys_interval_us = (current_time - last_system_time) * 1_000_000
                    if sys_interval_us >= min_fresh_interval_us:
                        found_fresh = True
                        first_kernel_pts_us = kernel_pts_us
                        first_frame_info[cam_name] = {
                            "kernel_pts_us": first_kernel_pts_us,
                            "dropped": dropped_frames,
                        }
                        print(f"[{cam_name}] 首帧锁定: PTS={first_kernel_pts_us}")
                    else:
                        dropped_frames += 1
                last_system_time = current_time
                if not found_fresh:
                    continue

            rel_pts_us = kernel_pts_us - first_kernel_pts_us
            interval_us = rel_pts_us - last_rel_pts_us if frames_count > 0 else 0

            # 检查录制时长 (仅当 duration > 0 时)
            if duration > 0 and rel_pts_us > duration * 1_000_000:
                break

            # 写入视频帧 (硬件编码)
            encoder.write(packet)

            # 写入 CSV
            csv_writer.writerow(
                [
                    frames_count,
                    rel_pts_us,
                    kernel_pts_us,
                    interval_us,
                    f"{current_time:.6f}",
                ]
            )

            last_rel_pts_us = rel_pts_us
            frames_count += 1

        print(f"[{cam_name}] 录制结束，总帧数: {frames_count}")

    except Exception as e:
        print(f"[{cam_name}] 错误: {e}")
        import traceback

        traceback.print_exc()
        if barrier:
            barrier.abort()
    finally:
        if csv_f:
            csv_f.close()
        if input_container:
            input_container.close()
        if encoder:
            encoder.close()


class TripleCameraRecorder:
    """三相机同步录制器"""

    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)

        # 进程同步
        self.manager = Manager()
        self.first_frame_info = self.manager.dict()

        self.barrier = None
        self.stop_event = Event()
        self.start_event = Event()
        self.processes = []

        self.global_start_time = 0.0

        # 信号处理: 捕获 Shell 脚本发送的 kill -2 (SIGINT)
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        print(f"\n[Camera] 收到信号 {signum}，正在安全停止录制...")
        self.stop_event.set()

    def start_all(self, configs: list, duration: int = 0):
        """启动所有相机录制"""
        print("=" * 70)
        print("三相机同步录制系统 (H.265 硬件加速)")
        print("=" * 70)
        print(f"输出目录: {self.output_dir}")
        print("=" * 70)

        for config in configs:
            config["duration"] = duration

        self.barrier = Barrier(len(configs) + 1)
        self.start_event.clear()
        self.first_frame_info.clear()

        self.processes = []
        for config in configs:
            p = Process(
                target=_record_camera_process,
                args=(
                    config,
                    self.barrier,
                    self.start_event,
                    self.stop_event,
                    self.first_frame_info,
                ),
            )
            p.start()
            self.processes.append(p)

        print("主进程: 等待相机初始化...")
        try:
            self.barrier.wait(timeout=15)
        except multiprocessing.BrokenBarrierError:
            print("错误: 相机初始化超时或失败")
            self.stop_event.set()
            for p in self.processes:
                p.join(timeout=1)
                if p.is_alive():
                    p.terminate()
            return

        print("主进程: 清空缓冲区 (2秒)...")
        time.sleep(2)

        self.global_start_time = time.time()
        self.start_event.set()
        print(f"录制开始! 系统时间: {datetime.fromtimestamp(self.global_start_time)}")

        # 监控循环
        try:
            while any(p.is_alive() for p in self.processes):
                elapsed = time.time() - self.global_start_time

                # 检查是否要求停止
                if self.stop_event.is_set():
                    break

                # 仅当 duration > 0 时才进行超时判断
                if duration > 0 and elapsed > duration + 5:
                    print("\n超时，自动停止...")
                    self.stop_event.set()
                    break

                time.sleep(0.5)

        except KeyboardInterrupt:
            print("\n主进程捕获中断...")
            self.stop_event.set()

        print("\n正在停止进程...")
        for p in self.processes:
            p.join(timeout=5)
            if p.is_alive():
                print(f"警告: 进程 {p.pid} 未正常退出，强制终止")
                p.terminate()

        print("相机录制完成")
        self._show_summary(configs)

    def _show_summary(self, configs: list):
        """显示录制摘要和同步分析"""
        print("\n" + "=" * 60)
        print("同步分析报告")
        print("=" * 60)

        first_frame_info = dict(self.first_frame_info)
        if first_frame_info:
            print("\n【首帧同步性】")
            print("-" * 50)
            pts_values = [info["kernel_pts_us"] for info in first_frame_info.values()]
            if pts_values:
                min_pts = min(pts_values)
                for cam_name, info in sorted(first_frame_info.items()):
                    offset_ms = (info["kernel_pts_us"] - min_pts) / 1000
                    print(
                        f"  {cam_name:<12}: 延迟 +{offset_ms:.2f}ms (丢弃旧帧: {info['dropped']})"
                    )

        print("\n【帧间隔稳定性】")
        print("-" * 50)
        for config in configs:
            cam_name = config["name"]
            fps = config["fps"]
            csv_file = os.path.splitext(config["output"])[0] + ".csv"

            if not os.path.exists(csv_file):
                continue

            intervals = []
            try:
                with open(csv_file, "r") as f:
                    reader = csv.DictReader(f)
                    for row in reader:
                        val = int(row["kernel_interval_us"])
                        if val > 0:
                            intervals.append(val)
            except Exception:
                continue

            if intervals:
                avg = statistics.mean(intervals)
                std = statistics.stdev(intervals) if len(intervals) > 1 else 0
                expected = 1_000_000 / fps
                jitter = (std / expected) * 100
                print(
                    f"  {cam_name:<12}: 平均间隔={avg:.0f}us (目标{expected:.0f}), 抖动={jitter:.2f}%"
                )
                print(f"  {cam_name}: 数据已保存至 {csv_file}")


def main():
    parser = argparse.ArgumentParser(
        description="三相机同步录制工具 (H.265 硬件加速版)"
    )

    # 必须参数: 录制时长
    parser.add_argument(
        "-d", "--duration", type=int, required=False, default=0, help="录制时长(秒)"
    )

    # 必须参数: 输出目录 (由 Shell 脚本传入完整路径)
    parser.add_argument(
        "--output-dir",
        type=str,
        required=True,
        help="数采统一的完整输出目录路径",
    )

    args = parser.parse_args()
    output_dir = args.output_dir

    # 相机配置
    configs = [
        {
            "name": "cam",
            "device": "/dev/third_cam",
            "width": 1920,
            "height": 1080,
            "fps": 60,
            "output": os.path.join(output_dir, "cam.mkv"),
        },
        {
            "name": "tact_left",
            "device": "/dev/left_tcam",
            "width": 640,
            "height": 480,
            "fps": 120,
            "output": os.path.join(output_dir, "tact_left.mkv"),
        },
        {
            "name": "tact_right",
            "device": "/dev/right_tcam",
            "width": 640,
            "height": 480,
            "fps": 120,
            "output": os.path.join(output_dir, "tact_right.mkv"),
        },
    ]

    recorder = TripleCameraRecorder(output_dir)
    recorder.start_all(configs, args.duration)


if __name__ == "__main__":
    main()
