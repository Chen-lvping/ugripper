#!/usr/bin/env python3
import os
import time
import math
import sys
import select
import errno
import signal

# ================= 配置部分 =================
PWM_CONFIG = {
    "green": {"chip": "pwmchip1", "channel": "0"},
    "blue": {"chip": "pwmchip0", "channel": "0"},
    "red": {"chip": "pwmchip3", "channel": "0"},
}
PWM_BASE_PATH = "/sys/class/pwm"
PIPE_PATH = "/tmp/umi_led_pipe"  # 通信管道路径


class PwmChannel:
    def __init__(self, chip_name, channel_id, period_ns=1000000):
        self.chip_name = chip_name
        self.channel_id = str(channel_id)
        self.chip_path = os.path.join(PWM_BASE_PATH, chip_name)
        self.pwm_path = os.path.join(self.chip_path, f"pwm{self.channel_id}")
        self.period_ns = period_ns

        if not os.path.exists(self.chip_path):
            raise FileNotFoundError(f"控制器 {self.chip_name} 不存在")

        self._export_channel()

        try:
            self.set_period(period_ns)
            self.set_enable(True)
            self.set_level(0)
        except Exception as e:
            print(f"[{self.chip_name}] 初始化失败: {e}")
            self.shutdown()
            raise

    def _write_file(self, filename, value):
        path = os.path.join(self.pwm_path, filename)
        try:
            with open(path, "w") as f:
                f.write(str(value))
        except OSError as e:
            pass

    def _export_channel(self):
        if os.path.exists(self.pwm_path):
            return
        try:
            with open(os.path.join(self.chip_path, "export"), "w") as f:
                f.write(self.channel_id)
        except OSError:
            pass

        # 等待节点生成
        timeout = 2.0
        start = time.time()
        while not os.path.exists(self.pwm_path):
            if time.time() - start > timeout:
                raise TimeoutError(f"[{self.chip_name}] 导出超时")
            time.sleep(0.05)
        time.sleep(0.1)

    def set_period(self, ns):
        self._write_file("period", ns)

    def set_duty_cycle(self, ns):
        self._write_file("duty_cycle", ns)

    def set_enable(self, enable):
        val = "1" if enable else "0"
        self._write_file("enable", val)

    def set_level(self, brightness_0_to_1):
        # 逻辑：亮度 1.0 -> duty=0; 亮度 0.0 -> duty=period
        brightness = max(0.0, min(1.0, brightness_0_to_1))
        duty = int((1.0 - brightness) * self.period_ns)
        self.set_duty_cycle(duty)

    def shutdown(self):
        try:
            self.set_level(0) # 关灯
        except:
            pass


class RgbLed:
    def __init__(self):
        self.leds = {}
        try:
            for color, cfg in PWM_CONFIG.items():
                self.leds[color] = PwmChannel(cfg["chip"], cfg["channel"])
        except Exception as e:
            print(f"LED Init Error: {e}")
            self.close()
            sys.exit(1)

    def close(self):
        for led in self.leds.values():
            led.shutdown()

    def set_rgb(self, r, g, b):
        self.leds["red"].set_level(r / 255.0)
        self.leds["green"].set_level(g / 255.0)
        self.leds["blue"].set_level(b / 255.0)

    def set_scaled_rgb(self, r, g, b, scale):
        valid_scale = max(0.0, min(1.0, scale))
        scaled_r = r * valid_scale
        scaled_g = g * valid_scale
        scaled_b = b * valid_scale
        self.set_rgb(scaled_r, scaled_g, scaled_b)


# ================= 状态机逻辑 =================
class LedStateMachine:
    def __init__(self):
        self.led = RgbLed()
        self.state = "INIT"
        self.tick = 0
        self.running = True
        self.sync_progress = 0.0  # 0.0 ~ 1.0

        # 确保管道存在
        if not os.path.exists(PIPE_PATH):
            try:
                os.mkfifo(PIPE_PATH)
            except OSError as e:
                print(f"Failed to create fifo: {e}")

        # 非阻塞方式打开管道
        try:
            self.pipe_fd = os.open(PIPE_PATH, os.O_RDONLY | os.O_NONBLOCK)
            print(f"LED Controller Started. Listening on {PIPE_PATH}")
        except Exception as e:
            print(f"Failed to open pipe: {e}")
            self.pipe_fd = None

    def read_command(self):
        """尝试从管道读取最新的状态命令（支持 CALIB_RUN:progress）"""
        if self.pipe_fd is None:
            return

        try:
            data = os.read(self.pipe_fd, 1024).decode().strip()
            if not data:
                return

            # 取最后一条非空命令
            lines = [l.strip() for l in data.split("\n") if l.strip()]
            if not lines:
                return

            raw_cmd = lines[-1]
            cmd = raw_cmd.upper()

            valid_states = {
                "INIT", "READY", "RECORDING", "ERROR", "EXIT",
                "ERROR_1", "ERROR_2", "ERROR_3", "ERROR_4", "ERROR_5",
                "CALIB_PRE", "CALIB_RUN", "CALIB_DONE"
            }

            # --- CALIB_RUN:progress ---
            if cmd.startswith("CALIB_RUN"):
                parts = raw_cmd.split(":", 1)
                self.state = "CALIB_RUN"

                if len(parts) == 2:
                    try:
                        p = float(parts[1])
                        self.sync_progress = max(0.0, min(1.0, p))
                    except ValueError:
                        pass  # 忽略非法进度
                else:
                    self.sync_progress = 1

                print(f"State switched to: CALIB_RUN (progress={self.sync_progress:.2f})")
                return

            # --- 普通状态 ---
            if cmd in valid_states:
                self.state = cmd
                print(f"State switched to: {self.state}")

        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                pass
            else:
                print(f"Pipe error: {e}")

    def run(self):
        while self.running:
            self.read_command()
            self.update_effect()

            if self.state == "EXIT":
                print("EXIT command received via pipe.")
                self.running = False
                break

            time.sleep(0.02)  # 50Hz 刷新率
            self.tick += 1

    def cleanup(self):
        """清理资源并关闭灯光"""
        print("Cleaning up resources...")
        self.running = False
        if self.pipe_fd is not None:
            try:
                os.close(self.pipe_fd)
                self.pipe_fd = None
            except:
                pass
        
        if hasattr(self, 'led'):
            self.led.close()

    def get_error_level(self):
        """返回错误等级（1~5，数字越小越严重）"""
        if self.state == "ERROR":
            # 兼容旧状态，归并到最低优先级编码
            return 5

        if self.state.startswith("ERROR_"):
            try:
                level = int(self.state.split("_", 1)[1])
                if 1 <= level <= 5:
                    return level
            except ValueError:
                pass

        return 5

    def render_error_pattern(self, t):
        """
        错误编码灯效：固定红色，按“长 + N个短”循环闪烁。
        ERROR_1: 长短
        ERROR_2: 长短短
        ERROR_3: 长短短短
        ERROR_4: 长短短短短
        ERROR_5: 长短短短短短
        """
        level = self.get_error_level()
        pulse_defs = ["long"] + ["short"] * level

        # 为了可肉眼读码，节奏改为更慢且间隔更清晰
        long_on_ticks = 35      # 700ms
        short_on_ticks = 11     # 220ms
        pulse_gap_ticks = 15    # 脉冲间隔 300ms
        sequence_gap_ticks = 60 # 轮次间隔 1200ms

        segments = []
        for idx, pulse_type in enumerate(pulse_defs):
            on_ticks = long_on_ticks if pulse_type == "long" else short_on_ticks
            segments.append((True, on_ticks))
            off_ticks = sequence_gap_ticks if idx == (len(pulse_defs) - 1) else pulse_gap_ticks
            segments.append((False, off_ticks))

        cycle_ticks = sum(duration for _, duration in segments)
        phase = t % cycle_ticks

        for is_on, duration in segments:
            if phase < duration:
                if is_on:
                    self.led.set_scaled_rgb(255, 0, 0, 0.8)
                else:
                    self.led.set_rgb(0, 0, 0)
                return
            phase -= duration

        self.led.set_rgb(0, 0, 0)

    def update_effect(self):
        """根据当前状态渲染灯效"""
        t = self.tick

        if self.state == "INIT":
            # 蓝色常亮
            self.led.set_rgb(0, 122, 255)

        elif self.state == "READY":
            # 绿色呼吸 (周期约 3秒)
            # 使用 sin 函数生成 0.0 到 1.0 的平滑曲线
            brightness = (math.sin(t * 0.04) + 1) / 2

            self.led.set_scaled_rgb(0, 255, 20, brightness)

        elif self.state == "RECORDING":
            # 录制中：绿色闪烁（2Hz）
            if (t % 25) < 12:
                self.led.set_rgb(0, 255, 0)
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "CALIB_DONE":
            # 绿色闪烁 (周期 1秒: 0.5亮 0.5灭)
            if (t % 50) < 25:
                self.led.set_rgb(0, 255, 20)
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "ERROR" or self.state.startswith("ERROR_"):
            self.render_error_pattern(t)

        elif self.state == "CALIB_PRE":
            # 准备校准: 黄灯慢闪 (1Hz)
            if (t % 50) < 25:
                self.led.set_rgb(255, 80, 0)
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "CALIB_RUN":
            # 同步中：黄灯，随进度加快闪烁
            # 进度 0.0 -> 1Hz，1.0 -> 8Hz
            freq = 1.0 + self.sync_progress * 7.0
            period_ticks = max(1, int(50 / freq))

            if (t % period_ticks) < (period_ticks // 2):
                self.led.set_rgb(255, 80, 0)
            else:
                self.led.set_rgb(0, 0, 0)

        else:
            self.led.set_rgb(0, 0, 0)


# ================= 全局实例与信号处理 =================
app = None

def signal_handler(signum, frame):
    """处理系统信号 (SIGINT, SIGTERM)"""
    sig_name = "SIGTERM" if signum == signal.SIGTERM else "SIGINT"
    print(f"\nReceived signal: {sig_name}. Shutting down...")
    
    if app:
        app.cleanup()
    
    sys.exit(0)

if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        app = LedStateMachine()
        app.run()
    except Exception as e:
        print(f"Main Loop Error: {e}")
    finally:
        # 兜底清理，防止异常退出时没关灯
        if app:
            app.cleanup()
