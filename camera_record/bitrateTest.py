#!/usr/bin/env python3
import argparse
import os
import subprocess
import time
import sys
import termios
import tty
import logging
from multiprocessing import Barrier, Event, Process

# 尝试导入 pyav
try:
    import av
except ImportError:
    print("错误: 缺少 'av' 库。请运行: pip3 install av")
    sys.exit(1)

# ================================================================
# 配置区域
# ================================================================

# 1. 码率梯度配置 (Target, Max)
# 主相机 (VBR模式: Target为平均目标, Max为峰值限制)
MAIN_LADDER = [
    ("4M", "6M"),
    ("60M", "80M"),
]

# 触觉相机
TACTILE_LADDER = [
    ("2M", "4M"),
    ("45M", "60M"),
]

# 2. 设备配置
DEVICES = [
    {
        "name": "cam",  # 注意：这里名字是 cam
        "device": "/dev/video11",
        "width": 1920,
        "height": 1080,
        "fps": 60,
        "type": "nv12_direct",
    },
    {
        "name": "tact_left",
        "device": "/dev/left_tcam",
        "width": 640,
        "height": 480,
        "fps": 120,
        "type": "pyav_pipe",
    },
    {
        "name": "tact_right",
        "device": "/dev/right_tcam",
        "width": 640,
        "height": 480,
        "fps": 120,
        "type": "pyav_pipe",
    },
]

DURATION = 10  # 录制时长 (秒)
LOG_FILE = "bitrate_test_vbr.log"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(message)s",
    handlers=[logging.FileHandler(LOG_FILE), logging.StreamHandler(sys.stdout)],
)

# ================================================================
# 工具函数
# ================================================================


def wait_for_space(step_idx, total_steps, main_rates, tact_rates):
    """监听空格键"""
    print(f"\n" + "=" * 70)
    print(f"【准备录制场景 {step_idx}/{total_steps}】")
    print(f"  - 主相机: Target={main_rates[0]}, Max={main_rates[1]} (VBR / Auto QP)")
    print(f"  - 触觉组: Target={tact_rates[0]}, Max={tact_rates[1]}")
    print("-" * 70)
    print(">>> 请按 [空格键] 开始录制 (按 Ctrl+C 退出)...", end="", flush=True)

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setraw(sys.stdin.fileno())
        while True:
            ch = sys.stdin.read(1)
            if ch == " ":
                print()
                return
            if ch == "\x03":
                print("\n用户终止")
                termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                sys.exit(0)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)


def get_file_stats(filepath, duration):
    """计算文件统计信息"""
    if os.path.exists(filepath):
        size_bytes = os.path.getsize(filepath)
        size_mb = size_bytes / (1024 * 1024)
        if duration > 0:
            actual_bitrate_mbps = (size_mb * 8) / duration
        else:
            actual_bitrate_mbps = 0
        return size_mb, actual_bitrate_mbps
    return 0, 0


def calc_minrate(target_str):
    """根据 Target 计算 Minrate (约 0.66 * Target)"""
    try:
        if target_str.endswith("M"):
            val = int(target_str[:-1])
            min_val = int(val * 0.66)
            if min_val < 1:
                min_val = 1
            return f"{min_val}M"
    except:
        pass
    return target_str


# ================================================================
# 录制逻辑
# ================================================================


def _record_direct_nv12(config, bitrates, barrier, start_event, stop_event):
    """
    主相机逻辑: VBR + Auto QP
    """
    cam_name = config["name"]
    device_path = config["device"]
    output_file = config["output"]

    # 动态计算 minrate
    min_rate_val = calc_minrate(bitrates["target"])

    # === 构建 FFmpeg 命令 ===
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
        "fps=60",  # 必须指定fps才能动态码率压缩
        "-c:v",
        "hevc_rkmpp",
        "-rc_mode",
        "VBR",  # 指定使用 VBR
        "-b:v",
        bitrates["target"],
        "-minrate",
        min_rate_val,  # VBR 下通常需要设置 minrate 避免完全掉底
        "-maxrate",
        bitrates["max"],
        "-profile:v",
        "main",
        "-level",
        "5.1",
        output_file,
        "-y",
    ]

    print(f"[{cam_name}] 启动 FFmpeg (VBR Mode)...")

    process = None

    try:
        if barrier:
            barrier.wait()

        process = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            bufsize=1,
        )

        while not stop_event.is_set():
            if process.poll() is not None:
                logging.error(f"[{cam_name}] FFmpeg 意外退出")
                break

            line = process.stderr.readline()
            if not line:
                if process.poll() is not None:
                    break
                continue

            if not start_event.is_set():
                start_event.set()

    except Exception as e:
        logging.error(f"[{cam_name}] 异常: {e}")
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutError:
                process.kill()


# --- 触觉相机相关类 ---


class FFmpegPipeEncoder:
    def __init__(self, output_file, config, bitrates):
        fps = config["fps"]
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
            bitrates["target"],
            "-maxrate",
            bitrates["max"],
            "-bufsize",
            str(int(bitrates["target"][:-1]) * 2) + "M",
            "-r",
            str(fps),
            output_file,
        ]
        self.process = subprocess.Popen(
            self.cmd, stdin=subprocess.PIPE, stderr=subprocess.DEVNULL
        )

    def write(self, packet):
        try:
            self.process.stdin.write(bytes(packet))
        except BrokenPipeError:
            pass

    def close(self):
        if self.process:
            if self.process.stdin:
                try:
                    self.process.stdin.close()
                except:
                    pass
            self.process.wait()


def _record_via_pyav_pipe(config, bitrates, barrier, start_event, stop_event):
    cam_name = config["name"]
    device_path = config["device"]
    output_file = config["output"]
    input_container = None
    encoder = None
    try:
        input_container = av.open(
            device_path,
            format="v4l2",
            options={
                "framerate": str(config["fps"]),
                "video_size": f"{config['width']}x{config['height']}",
                "input_format": "mjpeg",
            },
        )
        input_stream = input_container.streams.video[0]
        encoder = FFmpegPipeEncoder(output_file, config, bitrates)
        if barrier:
            barrier.wait()
        for packet in input_container.demux(input_stream):
            if stop_event.is_set():
                break
            if packet.pts is None:
                continue
            if not start_event.is_set():
                start_event.set()
            encoder.write(packet)
    except Exception as e:
        logging.error(f"[{cam_name}] PyAV 错误: {e}")
        if barrier:
            barrier.abort()
    finally:
        if input_container:
            input_container.close()
        if encoder:
            encoder.close()


def _process_wrapper(config, bitrates, barrier, start_event, stop_event):
    if config["type"] == "nv12_direct":
        _record_direct_nv12(config, bitrates, barrier, start_event, stop_event)
    else:
        _record_via_pyav_pipe(config, bitrates, barrier, start_event, stop_event)


# ================================================================
# 主流程
# ================================================================


def run_test_step(output_dir, main_rates, tact_rates):
    barrier = Barrier(len(DEVICES) + 1)
    start_event = Event()
    stop_event = Event()
    processes = []
    files_info = []

    print(f"初始化设备...")
    for dev_conf in DEVICES:
        curr_conf = dev_conf.copy()
        curr_rates = {}

        # --- 修复点：这里原来写的是 "main"，导致一直进 else 分支 ---
        if curr_conf["name"] == "cam":
            curr_rates = {"target": main_rates[0], "max": main_rates[1]}
        else:
            curr_rates = {"target": tact_rates[0], "max": tact_rates[1]}

        filename = f"{curr_conf['name']}_{curr_rates['target']}.mkv"
        curr_conf["output"] = os.path.join(output_dir, filename)
        files_info.append(
            {
                "name": curr_conf["name"],
                "path": curr_conf["output"],
                "target": curr_rates["target"],
            }
        )
        p = Process(
            target=_process_wrapper,
            args=(curr_conf, curr_rates, barrier, start_event, stop_event),
        )
        p.start()
        processes.append(p)

    try:
        barrier.wait(timeout=10)
        print("所有相机就绪，开始录制...")
    except Exception:
        print("初始化超时！")
        stop_event.set()
        for p in processes:
            p.terminate()
        return

    start_time = time.time()
    try:
        while True:
            elapsed = time.time() - start_time
            if elapsed >= DURATION:
                break
            if not any(p.is_alive() for p in processes):
                print("所有进程已退出")
                break
            time.sleep(0.5)
            print(f"\r录制中... {elapsed:.1f}s / {DURATION}s", end="")
    except KeyboardInterrupt:
        print("\n用户中断")
    finally:
        stop_event.set()

    print("\n停止进程...")
    for p in processes:
        p.join(timeout=3)
        if p.is_alive():
            p.terminate()

    print("\n" + "-" * 80)
    print(f"{'相机':<12} | {'文件名':<30} | {'设定':<6} | {'实际码率'}")
    print("-" * 80)

    for info in files_info:
        size_mb, act_bitrate = get_file_stats(info["path"], DURATION)
        print(
            f"{info['name']:<12} | {os.path.basename(info['path']):<30} | {info['target']:<6} | {act_bitrate:.2f} Mbps"
        )
        logging.info(
            f"[{info['name']}] Target={info['target']}, Actual={act_bitrate:.2f}Mbps, File={info['path']}"
        )
    print("-" * 80)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="VBR模式码率测试工具")
    parser.add_argument(
        "--output-dir", type=str, default="./bitrate_test_vbr", help="输出目录"
    )
    args = parser.parse_args()

    if not os.path.exists(args.output_dir):
        os.makedirs(args.output_dir)

    max_len = max(len(MAIN_LADDER), len(TACTILE_LADDER))

    print(f"开始测试，输出目录: {args.output_dir}")
    print(f"共 {max_len} 组测试场景")

    for i in range(max_len):
        m_idx = min(i, len(MAIN_LADDER) - 1)
        t_idx = min(i, len(TACTILE_LADDER) - 1)
        main_r = MAIN_LADDER[m_idx]
        tact_r = TACTILE_LADDER[t_idx]
        wait_for_space(i + 1, max_len, main_r, tact_r)
        run_test_step(args.output_dir, main_r, tact_r)
    logging.info("全部测试结束")
