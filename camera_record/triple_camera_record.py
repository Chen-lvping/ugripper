#!/usr/bin/env python3
"""
三相机同步录制工具
"""

import argparse
import csv
import os
import signal
import statistics
import threading
import time
import sys
from datetime import datetime
from fractions import Fraction
from threading import Barrier

import av


class TripleCameraRecorder:
    """三相机同步录制器"""

    def __init__(self, output_dir: str):
        self.output_dir = output_dir
        # 递归创建目录
        os.makedirs(output_dir, exist_ok=True)

        # 线程同步
        self.barrier = None
        self.stop_event = threading.Event()
        self.start_event = threading.Event()
        self.threads = []

        # 录制统计
        self.first_frame_info = {}
        self.info_lock = threading.Lock()
        self.global_start_time = 0.0

        # 信号处理
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        print(f"\n收到信号 {signum}，正在安全停止...")
        self.stop_event.set()

    def _record_camera(self, config: dict):
        """单相机录制线程"""
        cam_name = config["name"]
        device_path = config["device"]  # 使用设备路径字符串
        width, height = config["width"], config["height"]
        fps = config["fps"]
        duration = config["duration"]
        output_file = config["output"]
        csv_file = os.path.splitext(output_file)[0] + ".csv"

        print(f"[{cam_name}] 初始化 ({device_path})...")

        input_container = None
        output_container = None
        csv_f = None

        try:
            # 打开摄像头 (V4L2 + MJPEG)
            # 使用固定路径而非索引
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

            # 创建输出容器
            output_container = av.open(output_file, "w")
            output_stream = output_container.add_stream("mjpeg", rate=fps)
            output_stream.width = input_stream.width
            output_stream.height = input_stream.height
            output_stream.pix_fmt = "yuvj420p"
            output_stream.time_base = Fraction(1, 1000000)  # 微秒精度

            # CSV 文件 (行缓冲，崩溃安全)
            csv_f = open(csv_file, "w", newline="", buffering=1)
            csv_writer = csv.writer(csv_f)
            csv_writer.writerow(
                [
                    "frame_id",
                    "kernel_pts_us",  # 相对 PTS (从0开始)
                    "kernel_pts_absolute_us",  # 绝对 PTS (用于跨相机对齐)
                    "kernel_interval_us",  # 帧间隔
                    "system_time",  # Unix 时间戳
                ]
            )

            print(f"[{cam_name}] 就绪，等待同步...")

            # 同步屏障：等待所有相机就绪
            if self.barrier:
                try:
                    self.barrier.wait()
                except threading.BrokenBarrierError:
                    return

            print(f"[{cam_name}] 清空缓冲区...")

            # 录制状态
            frames_count = 0
            first_kernel_pts_us = None
            last_rel_pts_us = 0
            dropped_frames = 0

            # 新鲜帧检测参数
            expected_interval_us = 1_000_000 // fps
            min_fresh_interval_us = expected_interval_us * 0.5
            last_system_time = None
            found_fresh = False

            # 主录制循环
            for packet in input_container.demux(input_stream):
                if self.stop_event.is_set():
                    break

                if packet.pts is None:
                    continue

                # 等待全局启动信号
                if not self.start_event.is_set():
                    continue

                current_time = time.time()
                # 计算内核时间戳 (微秒)
                kernel_pts_us = int(packet.pts * time_base * 1_000_000)

                # === 新鲜帧检测 ===
                # 丢弃缓冲区中积压的旧帧，确保录制的第一帧是刚刚从传感器出来的
                if not found_fresh:
                    if last_system_time is not None:
                        sys_interval_us = (current_time - last_system_time) * 1_000_000

                        # 如果系统时间流逝 >= 50% 帧间隔，说明这是新捕获的帧，而不是瞬间读出的旧缓存
                        if sys_interval_us >= min_fresh_interval_us:
                            found_fresh = True
                            first_kernel_pts_us = kernel_pts_us

                            with self.info_lock:
                                self.first_frame_info[cam_name] = {
                                    "kernel_pts_us": first_kernel_pts_us,
                                    "dropped": dropped_frames,
                                }

                            print(
                                f"[{cam_name}] 首帧锁定: PTS={first_kernel_pts_us}, "
                                f"丢弃旧帧={dropped_frames}"
                            )
                        else:
                            dropped_frames += 1

                    last_system_time = current_time
                    if not found_fresh:
                        continue

                # === 正常录制 ===
                rel_pts_us = kernel_pts_us - first_kernel_pts_us
                interval_us = rel_pts_us - last_rel_pts_us if frames_count > 0 else 0

                # 检查录制时长
                if rel_pts_us > duration * 1_000_000:
                    break

                # 写入视频帧
                packet.dts = rel_pts_us
                packet.pts = rel_pts_us
                packet.stream = output_stream
                output_container.mux(packet)

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
            if self.barrier:
                self.barrier.abort()
        finally:
            if csv_f:
                csv_f.close()
            if input_container:
                input_container.close()
            if output_container:
                output_container.close()

    def start_all(self, configs: list, duration: int):
        """启动所有相机录制"""
        print("=" * 70)
        print("三相机同步录制系统 (Kernel PTS + 固定路径版)")
        print("=" * 70)
        print(f"输出目录: {self.output_dir}")
        print(f"录制时长: {duration} 秒")
        print("=" * 70)

        # 配置录制时长
        for config in configs:
            config["duration"] = duration

        # 初始化同步原语
        self.barrier = Barrier(len(configs) + 1)
        self.start_event.clear()
        self.first_frame_info.clear()

        # 启动录制线程
        self.threads = []
        for config in configs:
            t = threading.Thread(target=self._record_camera, args=(config,))
            t.start()
            self.threads.append(t)

        # 等待所有相机初始化完成
        print("主线程: 等待相机初始化...")
        try:
            self.barrier.wait(timeout=15)
        except threading.BrokenBarrierError:
            print("错误: 相机初始化超时或失败 (请检查 /dev/xxx 路径是否存在)")
            self.stop_event.set()
            for t in self.threads:
                t.join(timeout=1)
            return

        # 等待缓冲区清空
        print("主线程: 清空缓冲区 (2秒)...")
        time.sleep(2)

        # 发送启动信号
        self.global_start_time = time.time()
        self.start_event.set()
        print(
            f"所有相机同步启动! 系统时间: {datetime.fromtimestamp(self.global_start_time)}"
        )

        # 监控录制进度
        try:
            while any(t.is_alive() for t in self.threads):
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

        # 等待线程结束
        print("\n\n等待线程结束...")
        for t in self.threads:
            t.join(timeout=2)

        print("\n录制完成")
        self._show_summary(configs)

    def _show_summary(self, configs: list):
        """显示录制摘要和同步分析"""
        print("\n" + "=" * 60)
        print("同步分析报告")
        print("=" * 60)

        if self.first_frame_info:
            print("\n【首帧同步性】")
            print("-" * 50)
            # 计算各相机首帧相对于最早首帧的时间差
            pts_values = [
                info["kernel_pts_us"] for info in self.first_frame_info.values()
            ]
            if pts_values:
                min_pts = min(pts_values)
                for cam_name, info in sorted(self.first_frame_info.items()):
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
                        # 过滤掉首帧间隔
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


def check_mount_point(path: str):
    """检查挂载点"""
    if not os.path.exists(path) or not os.path.ismount(path):
        print(f"警告: {path} 似乎未挂载或不存在！")
        print("文件可能写入到系统分区的临时文件夹中。")
        print("按 Ctrl+C 终止，或 5秒后继续...")
        try:
            time.sleep(5)
        except KeyboardInterrupt:
            sys.exit(0)


def get_next_episode_dir(base_root: str, device_id: str) -> str:
    """生成下一个 episode 目录路径 (逻辑融合)"""
    # 构造保存路径: <root>/raw_data/episode_...
    base_storage_dir = os.path.join(base_root, "raw_data")
    os.makedirs(base_storage_dir, exist_ok=True)

    today = datetime.now().strftime("%Y%m%d")
    prefix = f"episode_{today}_{device_id}_"

    max_id = -1
    for d in os.listdir(base_storage_dir):
        if d.startswith(prefix):
            try:
                num = int(d.split("_")[-1])
                if num > max_id:
                    max_id = num
            except ValueError:
                pass

    next_id = max_id + 1
    return os.path.join(base_storage_dir, f"{prefix}{next_id:04d}")


def main():
    DEFAULT_ROOT = "/mnt/data_disk"

    parser = argparse.ArgumentParser(
        description="三相机同步录制工具 (内核时间戳版)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  sudo python3 recorder.py -d 10               # 录制10秒到默认 /mnt/data_disk
  sudo python3 recorder.py -d 30 -o ./my_data  # 录制30秒到自定义目录
        """,
    )

    parser.add_argument(
        "-d", "--duration", type=int, required=True, help="录制时长(秒)"
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default=DEFAULT_ROOT,
        help=f"输出根目录, 默认 {DEFAULT_ROOT}",
    )
    parser.add_argument(
        "--device-id",
        type=str,
        default="ugripper_001",
        help="设备ID, 默认 ugripper_001",
    )
    args = parser.parse_args()

    # 1. 检查挂载点 (如果是默认路径)
    if args.output == DEFAULT_ROOT:
        check_mount_point(args.output)

    # 2. 生成输出目录
    output_dir = get_next_episode_dir(args.output, args.device_id)

    # 3. 相机配置
    configs = [
        {
            "name": "cam",
            "device": "/dev/third_cam",  # Udev 路径
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

    # 4. 启动录制
    recorder = TripleCameraRecorder(output_dir)
    recorder.start_all(configs, args.duration)


if __name__ == "__main__":
    main()
