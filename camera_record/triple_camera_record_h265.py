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
        # 动态调整码率: 1080p用10M, 480p用4M (提高码率以防止花屏)
        bitrate = "10M" if width >= 1920 else "4M"

        # 构建全硬件加速命令
        self.cmd = [
            "ffmpeg",
            "-y",  # 覆盖输出文件
            "-r",
            str(fps),  # 强制输入帧率
            "-c:v",
            "mjpeg_rkmpp",  # 硬件解码器 (输入)
            "-f",
            "mjpeg",  # 输入格式
            "-i",
            "pipe:0",  # 从管道读取
            "-c:v",
            "hevc_rkmpp",  # 硬件编码器 (输出)
            "-b:v",
            bitrate,  # 目标码率
            "-maxrate",
            bitrate,  # 最大码率限制
            "-bufsize",
            str(int(bitrate[:-1]) * 2) + "M",  # 缓冲区大小 (2倍码率)
            "-r",
            str(fps),  # 输出帧率
            output_file,
        ]

        # 启动子进程，重定向标准输入
        # stderr=subprocess.DEVNULL 可以屏蔽 FFmpeg 的刷屏日志，如果需要调试可改为 None
        self.process = subprocess.Popen(
            self.cmd, stdin=subprocess.PIPE, stderr=subprocess.DEVNULL
        )
        print(f"[{os.getpid()}] 硬件编码器已启动: HEVC (H.265)")

    def write(self, packet):
        """写入原始 MJPEG 数据包"""
        try:
            # 直接将 PyAV 的 packet 转换为字节流写入管道
            # 这一步非常快，不涉及任何解码/重编码计算
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


def _record_camera_process(
    config: dict, barrier, start_event, stop_event, first_frame_info
):
    """单相机录制进程 (优化版: 硬件编码)"""
    cam_name = config["name"]
    video_idx = config["index"]
    width, height = config["width"], config["height"]
    fps = config["fps"]
    duration = config["duration"]
    output_file = config["output"]

    csv_file = os.path.splitext(output_file)[0] + ".csv"

    print(f"[{cam_name}] 初始化 (PID: {os.getpid()})...")

    input_container = None
    encoder = None
    csv_f = None

    try:
        # 1. 打开摄像头 (V4L2 + MJPEG)
        input_options = {
            "framerate": str(fps),
            "video_size": f"{width}x{height}",
            "input_format": "mjpeg",
        }
        input_container = av.open(
            f"/dev/video{video_idx}", format="v4l2", options=input_options
        )
        input_stream = input_container.streams.video[0]
        input_time_base = float(input_stream.time_base)

        # 2. 创建硬件编码器 (替代原来的 output_container)
        encoder = FFmpegHardwareEncoder(output_file, width, height, fps)

        # 3. CSV 文件记录
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

        print(f"[{cam_name}] 开始录制...")

        frames_count = 0
        first_kernel_pts_us = None
        last_rel_pts_us = 0
        dropped_frames = 0

        expected_interval_us = 1_000_000 // fps
        min_fresh_interval_us = expected_interval_us * 0.5
        last_system_time = None
        found_fresh = False

        # === 主循环 ===
        for packet in input_container.demux(input_stream):
            if stop_event.is_set():
                break

            if packet.pts is None:
                continue

            kernel_pts_us = int(packet.pts * input_time_base * 1_000_000)

            if not start_event.is_set():
                continue

            current_time = time.time()

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

            if rel_pts_us > duration * 1_000_000:
                break

            # === 核心逻辑 ===
            # 这里的写法和原来的透传几乎一样简单
            # 只是把 output_container.mux(packet) 换成了 encoder.write(packet)
            encoder.write(packet)

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
        print(f"[{cam_name}] 严重错误: {e}")
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
    """三相机同步录制器 (多进程版)"""

    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)

        # 进程同步
        self.manager = Manager()
        self.first_frame_info = self.manager.dict()

        self.stop_event = Event()
        self.start_event = Event()
        self.processes = []
        self.barrier = None

        # 信号处理
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        print(f"\n收到信号 {signum}，正在安全停止...")
        self.stop_event.set()

    def start_all(self, configs: list, duration: int):
        """启动所有相机录制"""
        print("=" * 70)
        print("三相机同步录制 (RK3576 硬件编码 H.265 - 优化版)")
        print("=" * 70)
        print(f"输出目录: {self.output_dir}")
        print(f"录制时长: {duration} 秒")
        print("=" * 70)

        for config in configs:
            config["duration"] = duration

        # 初始化同步原语
        self.barrier = Barrier(len(configs) + 1)
        self.start_event.clear()
        self.first_frame_info.clear()

        # 启动录制进程
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

        # 等待所有相机初始化完成
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

        print("主进程: 清空缓冲区 (1秒)...")
        time.sleep(1)

        self.global_start_time = time.time()
        self.start_event.set()
        print(">>> 所有相机同步启动! <<<")

        try:
            while any(p.is_alive() for p in self.processes):
                elapsed = time.time() - self.global_start_time

                if elapsed > duration + 5:
                    print("\n超时，强制停止...")
                    self.stop_event.set()
                    break

                print(f"\r录制中: {elapsed:.1f}/{duration}s", end="", flush=True)
                time.sleep(0.5)
        except KeyboardInterrupt:
            print("\n\n接收到中断信号...")
            self.stop_event.set()

        print("\n\n等待进程结束...")
        for p in self.processes:
            p.join(timeout=5)
            if p.is_alive():
                print(f"警告: 进程 {p.pid} 未正常退出，强制终止")
                p.terminate()

        print("\n录制完成")
        self._show_summary(configs)

    def _show_summary(self, configs: list):
        """显示录制摘要和同步分析"""
        print("\n" + "=" * 60)
        print("视频文件检查")
        print("=" * 60)

        for config in configs:
            cam_name = config["name"]
            video_file = config["output"]
            # 自动修正后缀检查
            # if video_file.endswith('.mkv'):
            #     video_file = video_file[:-4] + '.ts'

            if os.path.exists(video_file):
                try:
                    with av.open(video_file) as c:
                        s = c.streams.video[0]
                        codec_name = s.codec_context.name
                        print(
                            f"{cam_name}: {s.frames} 帧 | 编码: {codec_name} | {s.width}x{s.height}"
                        )
                except Exception as e:
                    print(f"{cam_name}: 文件损坏或无法读取 - {e}")
            else:
                print(f"{cam_name}: 文件不存在")

        print("\n" + "=" * 60)
        print("同步分析 (基于内核时间戳)")
        print("=" * 60)

        # 将 Manager dict 转换为普通 dict 以便处理
        first_frame_info = dict(self.first_frame_info)

        if first_frame_info:
            print("\n【首帧对齐情况】")
            print("-" * 50)

            min_pts = min(info["kernel_pts_us"] for info in first_frame_info.values())

            for cam_name, info in sorted(first_frame_info.items()):
                offset_ms = (info["kernel_pts_us"] - min_pts) / 1000
                print(
                    f"  {cam_name}: 偏移 = +{offset_ms:.2f}ms (丢弃{info['dropped']}帧)"
                )

        print("\n【录制稳定性 (Jitter)】")
        print("-" * 50)

        for config in configs:
            cam_name = config["name"]
            fps = config["fps"]
            output_file = config["output"]
            # if output_file.endswith('.mkv'):
            #     output_file = output_file[:-4] + '.ts'
            csv_file = os.path.splitext(output_file)[0] + ".csv"

            if not os.path.exists(csv_file):
                continue

            intervals = []
            try:
                with open(csv_file, "r") as f:
                    reader = csv.DictReader(f)
                    for row in reader:
                        interval = int(row["kernel_interval_us"])
                        if interval > 0:
                            intervals.append(interval)
            except Exception:
                continue

            if intervals:
                avg = statistics.mean(intervals)
                std = statistics.stdev(intervals) if len(intervals) > 1 else 0
                expected = 1_000_000 / fps
                jitter = (std / expected) * 100

                print(
                    f"  {cam_name} ({fps}fps): 平均={avg:.0f}us, "
                    f"标准差={std:.0f}us, 抖动={jitter:.1f}%"
                )


def find_next_episode_dir(base_dir: str, device_id: str) -> str:
    """生成下一个 episode 目录路径"""
    os.makedirs(base_dir, exist_ok=True)

    today = datetime.now().strftime("%Y%m%d")
    prefix = f"episode_{today}_{device_id}_"

    existing_nums = []
    for d in os.listdir(base_dir):
        if d.startswith(prefix):
            try:
                num = int(d.split("_")[-1])
                existing_nums.append(num)
            except ValueError:
                pass

    next_num = max(existing_nums, default=-1) + 1
    return os.path.join(base_dir, f"{prefix}{next_num:04d}")


def main():
    parser = argparse.ArgumentParser(
        description="三相机同步录制工具 (RK3576 硬件加速版)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "-d", "--duration", type=int, default=10, help="录制时长(秒), 默认 10"
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default="raw_data",
        help="输出基础目录, 默认 raw_data",
    )
    parser.add_argument(
        "--device-id",
        type=str,
        default="ugripper_001",
        help="设备ID, 默认 ugripper_001",
    )
    args = parser.parse_args()

    # 生成 episode 目录
    output_dir = find_next_episode_dir(args.output, args.device_id)

    # 相机配置
    configs = [
        {
            "name": "cam",
            "index": 4,
            "width": 1920,
            "height": 1080,
            "fps": 60,
            "output": os.path.join(output_dir, "cam.mkv"),
        },
        {
            "name": "tact_left",
            "index": 2,
            "width": 640,
            "height": 480,
            "fps": 120,
            "output": os.path.join(output_dir, "tact_left.mkv"),
        },
        {
            "name": "tact_right",
            "index": 0,
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
