#!/usr/bin/env python3
"""
三相机同步录制工具 (H.264/H.265 硬件加速版 - 混合架构适配)

- 触觉相机 (MJPEG): 直接调用 FFmpeg
- 主相机 (NV12): 直接调用 FFmpeg

停止方式:
- 外部信号: kill -2 <PID> 或 kill -15 <PID>
- 指定时长: -d 参数
"""

import argparse
import json
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
NORMALRATE_1080p = "8M"
MAXRATE_1080p = "50M"
NORMALRATE_480p = "4M"
MAXRATE_480p = "20M"

BOOT_TIME_OFFSET_US = int((time.time() - time.monotonic()) * 1_000_000)


def get_precise_system_time(pts_us):
    """
    将 FFmpeg 的整数 PTS (微秒)(monotonic_time) 转换为高精度系统时间戳
    """
    return BOOT_TIME_OFFSET_US + pts_us


def get_encoder_name(codec):
    if codec == "h264":
        return "h264_rkmpp"
    return "hevc_rkmpp"


# 通用日志解析函数
def parse_ffmpeg_log_loop(process, cam_name, start_event, stop_event, first_frame_info):
    """
    统一的 FFmpeg 日志解析循环
    只解析整数 PTS，确保微秒级精度
    """
    # 匹配整数 PTS: "pts: 12345678"
    # 示例: [Parsed_showinfo...] n: 1 pts: 352213 pts_time:0.352213 ...
    pts_pattern = re.compile(r"pts:\s*(\d+)\s+pts_time:")

    first_pts_recorded = False
    frames_count = 0

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
                kernel_pts_abs_us = int(match.group(1))
                # print(f"[{cam_name}] 捕获帧 PTS: {kernel_pts_abs_us} (us)")

                frames_count += 1
                # 首帧锁定逻辑
                if not first_pts_recorded:
                    current_time = (
                        get_precise_system_time(kernel_pts_abs_us) / 1000000.0
                    )
                    # 触发全局开始信号
                    start_event.set()
                    first_frame_info[cam_name] = {
                        "kernel_pts_us": kernel_pts_abs_us,
                        "dropped": 0,
                        "sys_time_str": datetime.fromtimestamp(current_time).strftime(
                            "%H:%M:%S.%f"
                        ),
                    }
                    first_pts_recorded = True
                    print(
                        f"[{cam_name}] 首帧锁定: PTS={kernel_pts_abs_us} (us) | Sys={current_time:.6f}"
                    )

    return frames_count


# ================================================================
# 通用进程执行器
# ================================================================
def _run_ffmpeg_process(
    cmd, config, barrier, start_event, stop_event, first_frame_info
):
    cam_name = config["name"]
    print(f"[{cam_name}] 启动 FFmpeg (CMD模式)...")

    # 信号忽略，由 stop_event 控制
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

    process = None
    try:
        # 同步等待
        print(f"[{cam_name}] 等待同步...")
        if barrier:
            try:
                barrier.wait()
            except Exception:
                return

        process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            bufsize=1,
        )
        print(f"[{cam_name}] 录制中 (PID: {process.pid})")

        # 进入统一的日志解析循环
        frames = parse_ffmpeg_log_loop(
            process, cam_name, start_event, stop_event, first_frame_info
        )
        print(f"[{cam_name}] 录制结束，总帧数: {frames}")

    except Exception as e:
        print(f"[{cam_name}] 异常: {e}")
    finally:
        if process:
            if process.poll() is None:
                # 1) 优雅退出：发 q
                try:
                    if process.stdin:
                        process.stdin.write("q\n")
                        process.stdin.flush()
                except Exception:
                    pass

                # 2) 等待 ffmpeg 正常写尾
                try:
                    process.wait(timeout=10)   # mkv 建议给长一点
                except subprocess.TimeoutExpired:
                    # 3) 再用 terminate
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        # 4) 最后 kill
                        process.kill()
                        process.wait()

        print(f"[{cam_name}] 退出")



# ================================================================
# 模式 A: 主相机 (NV12) - 保持 FFmpeg Direct
# ================================================================
def _record_direct_nv12(config, barrier, start_event, stop_event, first_frame_info, codec):
    output_file = config["output"]
    device_path = config["device"]
    encoder = get_encoder_name(codec)

    # 注意滤镜顺序: 先 showinfo 获取原始 PTS，后 fps 重采样
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
        "showinfo,fps=60",  # <--- 关键：showinfo 会把 PTS 打印到日志,顺序不能错，不然pts会被覆盖
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
        "-profile:v",
        "main",
        "-level",
        "5.1",
        "-y",
        output_file,
    ]

    _run_ffmpeg_process(cmd, config, barrier, start_event, stop_event, first_frame_info)


# ================================================================
# 模式 B: 触觉相机 (MJPEG) - [重构] 改为 FFmpeg Direct
# ================================================================
def _record_direct_mjpeg(config, barrier, start_event, stop_event, first_frame_info, codec):
    """
    使用 FFmpeg 硬件 MJPEG 解码 + RK3576 硬件编码（H.264/H.265 可选）
    不输出 NV12，减少 CPU 负担和延迟
    """
    output_file = config["output"]
    device_path = config["device"]
    fps = config.get("fps", 120)
    encoder = get_encoder_name(codec)

    cmd = [
        "ffmpeg",
        "-y",
        "-thread_queue_size",
        "512",  # 输入队列缓冲
        "-f",
        "v4l2",
        "-input_format",
        "mjpeg",
        "-framerate",
        str(fps),
        "-video_size",
        f"{config['width']}x{config['height']}",
        "-copyts",
        "-i",
        device_path,
        "-vf",
        f"showinfo,fps={fps}",
        "-c:v",
        encoder,
        "-rc_mode",
        "CQP",
        "-qp_init",
        "30",
        "-qp_max",
        "38",
        "-qp_min",
        "24",
        "-qp_max_i",
        "38",
        "-qp_min_i",
        "20",
        output_file,
    ]
    _run_ffmpeg_process(cmd, config, barrier, start_event, stop_event, first_frame_info)


# ================================================================
# 主控逻辑
# ================================================================
def _process_dispatch(config, barrier, start_event, stop_event, first_frame_info, codec):
    """根据格式选择录制模式"""
    fmt = config.get("format", "mjpeg")
    if fmt == "nv12":
        # 主相机: Direct FFmpeg + Log Parse
        _record_direct_nv12(config, barrier, start_event, stop_event, first_frame_info, codec)
    else:
        # 触觉相机: Direct FFmpeg + Log Parse
        _record_direct_mjpeg(config, barrier, start_event, stop_event, first_frame_info, codec)


class TripleCameraRecorder:
    """三相机同步录制器"""

    def __init__(self, output_dir: str, codec: str):
        self.output_dir = output_dir
        self.codec = codec
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
        print(f"三相机同步录制系统 ({self.codec.upper()} 硬件加速)")
        print("=" * 70)
        print(f"输出目录: {self.output_dir}")
        print(f"主进程 PID: {os.getpid()}  (Shell可用 kill -2 {os.getpid()} 停止)")

        # =================================================================
        # 写入开机时间偏移量到 info.json
        # 偏移量 = 当前Unix时间 - 系统运行时间(Monotonic)
        # 后续可用公式: 真实时间 = 视频PTS(如果为Monotonic) + offset
        # =================================================================
        try:
            offset = time.time() - time.monotonic()
            info_path = os.path.join(self.output_dir, "info.json")
            
            data = {
                "boot_time_offset": offset,                # 秒 (浮点数)
                "boot_time_offset_us": int(offset * 1e6),  # 微秒 (整数)
            }
            
            with open(info_path, "w") as f:
                json.dump(data, f, indent=4)
            print(f"[Info] 开机时间偏移量已写入: {info_path}")
            
        except Exception as e:
            print(f"[Error] 写入 info.json 失败: {e}")
        # =================================================================

        if duration > 0:
            print(f"录制时长: {duration} 秒")
        else:
            print("录制时长: 无限制 (等待外部信号停止)")
        print("=" * 70)

        for config in configs:
            config["duration"] = duration

        # ... (后续代码保持不变: barrier初始化, 进程启动循环等) ...
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
                    self.codec,
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


def main():
    parser = argparse.ArgumentParser(
        description="三相机同步录制工具 (H.264/H.265 硬件加速版)"
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
    parser.add_argument(
        "--codec",
        type=str,
        choices=["h264", "h265"],
        default="h264",
        help="硬件编码格式，默认 h264",
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

    recorder = TripleCameraRecorder(output_dir, args.codec)
    recorder.start_all(configs, args.duration)


if __name__ == "__main__":
    main()
