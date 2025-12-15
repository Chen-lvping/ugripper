#!/usr/bin/env python3
"""
三相机同步录制工具 (H.265 硬件加速版 - 混合架构适配)

- 触觉相机 (MJPEG): PyAV 读取 -> Pipe -> FFmpeg
- 主相机 (NV12): 直接调用 FFmpeg (带 showinfo 滤镜) -> Python 正则抓取日志获取 PTS -> CSV

停止方式:
- 外部信号: kill -2 <PID> 或 kill -15 <PID>
- 指定时长: -d 参数
"""

import argparse
import csv
import multiprocessing
import os
import signal
import statistics
import subprocess
import time
import re
from datetime import datetime
from multiprocessing import Barrier, Event, Manager, Process

import av

MINRATE_1080p = "4M"
NORMALRATE_1080p = "10M"
MAXRATE_1080p = "30M"

MAXRATE_480p = "8M"


# ================================================================
# 模式 A: 针对主相机 (NV12 Direct Mode + Log Parsing)
# ================================================================
def _record_direct_nv12(config, barrier, start_event, stop_event, first_frame_info):
    """
    由于pyav打不开mipi相机，在这里专门处理主相机。
    不使用 PyAV，直接运行 FFmpeg，通过分析 stderr 日志获取 PTS 写 CSV。
    """
    cam_name = config["name"]
    device_path = config["device"]
    output_file = config["output"]
    csv_file = os.path.splitext(output_file)[0] + ".csv"

    cmd = [
        "ffmpeg",
        "-f",
        "v4l2",
        "-input_format",
        "nv12",
        "-video_size",
        f"{config['width']}x{config['height']}",
        "-copyts",
        "-i",
        device_path,
        "-vf",
        "showinfo",  # <--- 关键：showinfo 会把 PTS 打印到日志
        "-c:v",
        "hevc_rkmpp",
        "-rc_mode",
        "AVBR",
        "-b:v",
        NORMALRATE_1080p,
        "-minrate",
        MINRATE_1080p,
        "-maxrate",
        MAXRATE_1080p,
        "-profile:v",
        "main",
        "-level",
        "5.1",
        output_file,
        "-y",
    ]

    print(f"[{cam_name}] 启动 Direct FFmpeg (NV12)...")

    # 信号处理
    def child_signal_handler(signum, frame):
        stop_event.set()

    signal.signal(signal.SIGINT, child_signal_handler)
    signal.signal(signal.SIGTERM, child_signal_handler)

    process = None
    csv_f = None

    # 正则表达式匹配 FFmpeg showinfo 输出
    # 示例: [Parsed_showinfo...] n:   1 pts:     72533 pts_time:1.20888 ...
    pts_pattern = re.compile(r"n:\s*(\d+)\s*pts:\s*(\d+)\s*pts_time:\s*([\d\.]+)")

    try:
        # 1. 打开 CSV
        csv_f = open(csv_file, "w", newline="", buffering=1)
        csv_writer = csv.writer(csv_f)
        csv_writer.writerow(
            [
                "frame_id",
                "kernel_pts_us",  # 实际存储的是相对时间 (Rel PTS)
                "kernel_pts_absolute_us",  # 绝对时间 (Abs PTS)
                "kernel_interval_us",  # 帧间隔
                "system_time",  # 抓取时的系统时间
            ]
        )

        # 2. 同步等待
        print(f"[{cam_name}] 就绪，等待同步...")
        if barrier:
            try:
                barrier.wait()
            except Exception as e:
                if type(e).__name__ == "BrokenBarrierError":
                    print("Barrier broken")
                else:
                    raise
                return

        # 3. 等待其他相机启动
        time.sleep(0.5)

        # 4. 启动进程 (捕获 stderr)
        process = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,  # 必须捕获 stderr
            universal_newlines=True,  # 文本模式
            bufsize=1,  # 行缓冲
        )
        print(f"[{cam_name}] 录制开始 (PID: {process.pid})")

        # 5. 循环读取日志并提取 PTS
        # 状态变量初始化
        first_pts_recorded = False
        first_kernel_pts_us = 0
        last_rel_pts_us = 0
        frames_count = 0

        # 循环读取日志
        while not stop_event.is_set():
            if process.poll() is not None:
                print(f"[{cam_name}] 错误: FFmpeg 进程意外退出")
                break

            line = process.stderr.readline()
            if not line:
                if process.poll() is not None:
                    break
                continue

            if "showinfo" in line:
                match = pts_pattern.search(line)
                if match:
                    # 解析数据
                    # frame_n_raw = match.group(1) # FFmpeg 内部计数，可能不连续，建议用自己的计数器
                    frames_count += 1
                    kernel_pts_abs_us = int(match.group(2))
                    # pts_time_sec = float(match.group(3))

                    current_time = pts_to_system_time(kernel_pts_abs_us)

                    # 首帧锁定逻辑
                    if not first_pts_recorded:
                        global_start_time = time.time()
                        start_event.set()
                        print(
                            f"录制开始! 系统时间: {datetime.fromtimestamp(global_start_time)}"
                        )
                        first_kernel_pts_us = kernel_pts_abs_us
                        first_frame_info[cam_name] = {
                            "kernel_pts_us": kernel_pts_abs_us,
                            "dropped": 0,
                        }
                        first_pts_recorded = True
                        print(f"[{cam_name}] 首帧锁定: PTS={kernel_pts_abs_us}")

                    # 计算相对时间 (Rel PTS)
                    rel_pts_us = kernel_pts_abs_us - first_kernel_pts_us

                    # 计算帧间隔 (Interval)
                    if rel_pts_us == 0:
                        interval_us = 0
                    else:
                        interval_us = rel_pts_us - last_rel_pts_us

                    # 3. 写入统一格式的数据行
                    # [frame_id, rel_pts, abs_pts, interval, sys_time]
                    csv_writer.writerow(
                        [
                            match.group(
                                1
                            ),  # frame_id (从 ffmpeg 日志获取，可能会因为丢弃帧而不连续）也可以用frames_count
                            rel_pts_us,  # kernel_pts_us
                            kernel_pts_abs_us,  # kernel_pts_absolute_us
                            interval_us,  # kernel_interval_us
                            f"{current_time:.6f}",
                        ]
                    )

                    last_rel_pts_us = rel_pts_us

    except Exception as e:
        print(f"[{cam_name}] 异常: {e}")
    finally:
        if csv_f:
            csv_f.close()
        if process and process.poll() is None:
            print(f"[{cam_name}] 录制结束，总帧数: {frames_count}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutError:
                process.kill()
        print(f"[{cam_name}] 结束")


# ================================================================
# 模式 B: 针对触觉相机 (MJPEG PyAV + Pipe Mode)
# ================================================================
class FFmpegPipeEncoder:
    """模式 B 专用的 Pipe 编码器"""

    def __init__(self, output_file, config):
        width, height, fps = config["width"], config["height"], config["fps"]
        # 触觉相机码率
        bitrate = MAXRATE_480p

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
            pass  # 静默处理，避免退出时报错

    def close(self):
        """关闭编码器"""
        if self.process:
            if self.process.stdin:
                try:
                    self.process.stdin.close()
                except:
                    pass
            self.process.wait()
            print(f"[{os.getpid()}] 硬件编码器已关闭")


def _record_via_pyav_pipe(config, barrier, start_event, stop_event, first_frame_info):
    """
    触觉相机的原有逻辑: PyAV -> Pipe -> FFmpeg
    """
    cam_name = config["name"]
    device_path = config["device"]
    width, height = config["width"], config["height"]
    fps = config["fps"]
    duration = config["duration"]
    output_file = config["output"]
    csv_file = os.path.splitext(output_file)[0] + ".csv"

    # 信号处理
    def child_signal_handler(signum, frame):
        stop_event.set()

    signal.signal(signal.SIGINT, child_signal_handler)
    signal.signal(signal.SIGTERM, child_signal_handler)

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
        encoder = FFmpegPipeEncoder(output_file, config)

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
            except Exception as e:
                if type(e).__name__ == "BrokenBarrierError":
                    print("Barrier broken")
                else:
                    raise
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
            # 检查退出标志 (响应外部信号)
            if stop_event.is_set():
                break

            if packet.pts is None:
                continue

            # 等待全局启动信号 (清空缓冲区阶段)
            if not start_event.is_set():
                continue

            kernel_pts_us = int(packet.pts * time_base * 1_000_000)
            current_time = pts_to_system_time(kernel_pts_us)

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
        print(f"[{cam_name}] 结束")


# ================================================================
# 主控逻辑
# ================================================================
def _process_dispatch(config, barrier, start_event, stop_event, first_frame_info):
    """根据格式选择录制模式"""
    fmt = config.get("format", "mjpeg")
    if fmt == "nv12":
        # 主相机: Direct FFmpeg + Log Parse
        _record_direct_nv12(config, barrier, start_event, stop_event, first_frame_info)
    else:
        # 触觉相机: PyAV + Pipe
        _record_via_pyav_pipe(
            config, barrier, start_event, stop_event, first_frame_info
        )


def pts_to_system_time(kernel_pts_us):
    """
    将内核 PTS (微秒) 转换为 系统时间戳 (秒)
    """
    # 1. 获取当前时刻的 时间基准偏移量 (Offset)
    # 原理: 当前系统时间 (Realtime) - 当前单调时间 (Monotonic) = 系统启动时刻的系统时间
    # 注意: 在 Linux 上 V4L2 的 PTS 通常对应 time.monotonic()
    boot_time_offset = time.time() - time.monotonic()

    # 2. 将帧的内核 PTS 转换为秒
    frame_monotonic_sec = kernel_pts_us / 1_000_000.0

    # 3. 计算该帧的绝对系统时间
    frame_system_time = frame_monotonic_sec + boot_time_offset

    return frame_system_time


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

        # 信号处理: 捕获 Shell 脚本发送的 kill -2 (SIGINT) 或 kill -15 (SIGTERM)
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        sig_name = "SIGINT" if signum == signal.SIGINT else "SIGTERM"
        print(f"\n[Camera] 收到 {sig_name} 信号，正在安全停止录制...")
        self.stop_event.set()

    def start_all(self, configs: list, duration: int = 0):
        """启动所有相机录制"""
        print("=" * 70)
        print("三相机同步录制系统 (H.265 硬件加速)")
        print("=" * 70)
        print(f"输出目录: {self.output_dir}")
        print(f"主进程 PID: {os.getpid()}  (Shell可用 kill -2 {os.getpid()} 停止)")
        if duration > 0:
            print(f"录制时长: {duration} 秒")
        else:
            print("录制时长: 无限制 (等待外部信号停止)")
        print("=" * 70)

        for config in configs:
            config["duration"] = duration

        self.barrier = Barrier(len(configs) + 1)
        self.start_event.clear()
        self.first_frame_info.clear()

        self.processes = []
        for config in configs:
            p = Process(
                target=_process_dispatch,
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
        except Exception as e:
            if type(e).__name__ == "BrokenBarrierError":
                print("Barrier broken")
            else:
                raise
            print("错误: 相机初始化超时或失败")
            self.stop_event.set()
            for p in self.processes:
                p.join(timeout=1)
                if p.is_alive():
                    p.terminate()
            return

        self.start_event.wait()
        self.global_start_time = time.time()
        print(f"录制开始! 系统时间: {datetime.fromtimestamp(self.global_start_time)}")

        # 监控循环
        try:
            while any(p.is_alive() for p in self.processes):
                elapsed = time.time() - self.global_start_time

                # 检查是否要求停止 (外部信号触发)
                if self.stop_event.is_set():
                    break

                # 仅当 duration > 0 时才进行超时判断
                if duration > 0 and elapsed > duration + 5:
                    print("\n超时，自动停止...")
                    self.stop_event.set()
                    break

                time.sleep(0.1)  # 更快响应信号

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


def main():
    parser = argparse.ArgumentParser(
        description="三相机同步录制工具 (H.265 硬件加速版)"
    )

    # 可选参数: 录制时长 (不指定则无限录制，等待外部信号停止)
    parser.add_argument(
        "-d",
        "--duration",
        type=int,
        required=False,
        default=0,
        help="录制时长(秒)，0表示无限制",
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
            "device": "/dev/video11",
            "width": 1920,
            "height": 1080,
            "fps": 60,
            "format": "nv12",
            "output": os.path.join(output_dir, "cam.mkv"),
        },
        {
            "name": "tact_left",
            "device": "/dev/left_tcam",
            "width": 640,
            "height": 480,
            "fps": 120,
            "format": "mjpeg",
            "output": os.path.join(output_dir, "tact_left.mkv"),
        },
        {
            "name": "tact_right",
            "device": "/dev/right_tcam",
            "width": 640,
            "height": 480,
            "fps": 120,
            "format": "mjpeg",
            "output": os.path.join(output_dir, "tact_right.mkv"),
        },
    ]

    recorder = TripleCameraRecorder(output_dir)
    recorder.start_all(configs, args.duration)


if __name__ == "__main__":
    main()
