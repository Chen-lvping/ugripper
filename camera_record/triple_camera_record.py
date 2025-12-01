#!/usr/bin/env python3
"""
修改版说明:
1. 适配 Udev 固定的设备路径 (/dev/cam_port*)
2. 适配 USB 自动挂载路径 (/mnt/data_disk)
"""
import av
import threading
import time
import sys
import os
import csv
import argparse
from datetime import datetime
from threading import Barrier
from fractions import Fraction


class TripleCameraRecorder:
    def __init__(self, output_dir):
        self.output_dir = output_dir
        # 确保输出目录存在 (如果 /mnt/data_disk 没挂载，这里会在根目录创建文件夹，建议运行前检查挂载)
        os.makedirs(output_dir, exist_ok=True)
        self.stop_event = threading.Event()
        self.start_event = threading.Event()
        self.start_system_time = 0.0
        self.threads = []
        self.barrier = None

    def record_camera(self, config):
        # 修改点：直接读取设备路径字符串，而不是数字索引
        device_path = config["device"]
        device_name = config["name"]

        output_file = config["output"]
        csv_file = os.path.splitext(output_file)[0] + ".csv"
        width = config["width"]
        height = config["height"]
        fps = config["fps"]
        duration = config["duration"]

        print(f"[{device_name}] 初始化 ({device_path})...")

        input_container = None
        output_container = None
        csv_f = None

        try:
            # 打开输入设备
            options = {
                "framerate": str(fps),
                "video_size": f"{width}x{height}",
                "input_format": "mjpeg",
            }

            # 修改点：av.open 直接打开 /dev/cam_portX
            input_container = av.open(device_path, format="v4l2", options=options)
            input_stream = input_container.streams.video[0]

            actual_width = input_stream.width
            actual_height = input_stream.height
            if actual_width != width or actual_height != height:
                print(
                    f"[{device_name}] 警告: 请求 {width}x{height}, 实际 {actual_width}x{actual_height}"
                )

            # 打开输出文件
            output_container = av.open(output_file, "w")

            csv_f = open(csv_file, "w", newline="", buffering=1)
            csv_writer = csv.writer(csv_f)
            csv_writer.writerow(["Frame", "PTS_us", "SystemTime_s"])

            output_stream = output_container.add_stream("mjpeg", rate=fps)
            output_stream.width = actual_width
            output_stream.height = actual_height
            output_stream.pix_fmt = "yuvj420p"
            output_stream.time_base = Fraction(1, 1000000)

            print(f"[{device_name}] 就绪，等待同步...")

            if self.barrier:
                try:
                    self.barrier.wait()
                except threading.BrokenBarrierError:
                    return

            print(f"[{device_name}] 缓冲区清空模式...")

            frames_count = 0
            has_started_log = False

            for packet in input_container.demux(input_stream):
                if self.stop_event.is_set():
                    break

                if packet.pts is None:
                    continue

                if not self.start_event.is_set():
                    continue

                if not has_started_log:
                    print(f"[{device_name}] 同步启动! 开始写入...")
                    has_started_log = True

                current_time = time.time()
                rel_pts = int((current_time - self.start_system_time) * 1000000)

                if rel_pts < 0:
                    continue

                if rel_pts > duration * 1000000:
                    break

                packet.dts = rel_pts
                packet.pts = rel_pts
                packet.stream = output_stream

                output_container.mux(packet)
                csv_writer.writerow([frames_count, rel_pts, f"{current_time:.6f}"])
                frames_count += 1

            print(f"[{device_name}] 录制结束. 帧数: {frames_count}")

        except Exception as e:
            print(f"[{device_name}] 错误: {e}")
            if self.barrier:
                self.barrier.abort()
        finally:
            if csv_f:
                csv_f.close()
            if input_container:
                input_container.close()
            if output_container:
                output_container.close()

    def start_all(self, configs, duration):
        print("=" * 80)
        print("三相机同步录制 (PyAV - 系统时间戳版)")
        print("=" * 80)
        print(f"输出目录: {self.output_dir}")
        print(f"录制时长: {duration} 秒")
        print("=" * 80)

        for config in configs:
            config["duration"] = duration

        self.barrier = Barrier(len(configs) + 1)
        self.start_event.clear()

        self.threads = []
        for config in configs:
            t = threading.Thread(target=self.record_camera, args=(config,))
            t.start()
            self.threads.append(t)

        print("主线程: 等待相机初始化...")
        try:
            self.barrier.wait(timeout=10)
        except threading.BrokenBarrierError:
            print(
                "错误: 相机初始化超时或失败 (请检查USB连接和 /dev/cam_port* 是否存在)"
            )
            self.stop_event.set()
            for t in self.threads:
                t.join(timeout=1)
            return

        print("主线程: 等待缓冲区清空 (2秒)...")
        time.sleep(2)

        self.start_system_time = time.time()
        self.start_event.set()

        print(
            f"所有相机同步启动! 系统时间: {datetime.fromtimestamp(self.start_system_time)}"
        )

        try:
            while True:
                elapsed = time.time() - self.start_system_time
                if not any(t.is_alive() for t in self.threads):
                    break
                if elapsed > duration + 5:
                    print("\n超时，强制停止...")
                    self.stop_event.set()
                    break
                print(f"\r录制中: {elapsed:.1f}/{duration}s", end="", flush=True)
                time.sleep(0.5)

        except KeyboardInterrupt:
            print("\n\n收到停止信号...")
            self.stop_event.set()

        print("\n\n等待线程结束...")
        for t in self.threads:
            t.join()

        print("\n录制完成")
        self.verify_videos(configs)

    def verify_videos(self, configs):
        print("\n验证视频文件:")
        print("-" * 60)
        for config in configs:
            path = config["output"]
            name = config["name"]
            if os.path.exists(path):
                try:
                    with av.open(path) as container:
                        stream = container.streams.video[0]
                        duration_sec = (
                            float(container.duration / av.time_base)
                            if container.duration
                            else 0
                        )

                        print(
                            f"[{name}]: ✓ {stream.width}x{stream.height} @ {stream.average_rate}fps"
                        )
                        print(f"  - 时长: {duration_sec:.2f}s")
                        print(f"  - 路径: {path}")
                except Exception as e:
                    print(f"[{name}]: 无法读取 ({e})")
            else:
                print(f"[{name}]: 文件不存在")


def main():
    DEFAULT_ROOT = "/mnt/data_disk"

    parser = argparse.ArgumentParser(description="三相机同步录制工具 (PyAV版)")
    parser.add_argument(
        "-d", "--duration", type=int, required=True, help="录制时长(秒)"
    )
    # 这里允许用户通过参数覆盖，但默认是 data_disk
    parser.add_argument(
        "-o", "--output", type=str, default=DEFAULT_ROOT, help="输出根目录"
    )
    parser.add_argument("--device-id", type=str, default="ugripper_001", help="设备ID")

    args = parser.parse_args()

    # 简单检查挂载点
    if args.output.startswith("/mnt/data_disk"):
        if not os.path.exists("/mnt/data_disk") or not os.path.ismount(
            "/mnt/data_disk"
        ):
            print(
                "警告: /mnt/data_disk 似乎未挂载或不存在！文件可能写入到系统分区的临时文件夹中。"
            )
            print("按 Ctrl+C 终止，或 5秒后继续...")
            try:
                time.sleep(5)
            except KeyboardInterrupt:
                sys.exit(0)

    # 构造保存路径: /mnt/data_disk/raw_data/episode_...
    base_storage_dir = os.path.join(args.output, "raw_data")

    date_str = datetime.now().strftime("%Y%m%d")

    # 自动获取 ID
    os.makedirs(base_storage_dir, exist_ok=True)
    existing = [
        d
        for d in os.listdir(base_storage_dir)
        if d.startswith("episode_") and os.path.isdir(os.path.join(base_storage_dir, d))
    ]
    episode_id = 0
    for d in existing:
        parts = d.split("_")
        if len(parts) >= 4:
            try:
                episode_id = max(episode_id, int(parts[-1]) + 1)
            except ValueError:
                pass

    output_dir = os.path.join(
        base_storage_dir, f"episode_{date_str}_{args.device_id}_{episode_id:04d}"
    )

    # 修改点：配置中指定 device 路径，而非 index
    configs = [
        {
            "device": "/dev/third_cam",
            "width": 1920,
            "height": 1080,
            "fps": 60,
            "name": "cam",
            "output": os.path.join(output_dir, "cam.mkv"),
        },
        {
            "device": "/dev/left_tcam",
            "width": 640,
            "height": 480,
            "fps": 120,
            "name": "tact_left",
            "output": os.path.join(output_dir, "tact_left.mkv"),
        },
        {
            "device": "/dev/right_tcam",
            "width": 640,
            "height": 480,
            "fps": 120,
            "name": "tact_right",
            "output": os.path.join(output_dir, "tact_right.mkv"),
        },
    ]

    recorder = TripleCameraRecorder(output_dir)
    recorder.start_all(configs, args.duration)


if __name__ == "__main__":
    main()
