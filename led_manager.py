#!/usr/bin/env python3
import os
import time
import math
import sys
import select
import errno

# ================= 配置部分 =================
PWM_CONFIG = {
    "red": {"chip": "pwmchip1", "channel": "0"},
    "green": {"chip": "pwmchip0", "channel": "0"},
    "blue": {"chip": "pwmchip3", "channel": "0"},
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
            self.set_level(0)
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

        # 确保管道存在
        if not os.path.exists(PIPE_PATH):
            os.mkfifo(PIPE_PATH)

        # 非阻塞方式打开管道
        self.pipe_fd = os.open(PIPE_PATH, os.O_RDONLY | os.O_NONBLOCK)
        print(f"LED Controller Started. Listening on {PIPE_PATH}")

    def read_command(self):
        """尝试从管道读取最新的状态命令"""
        try:
            # 读取缓冲区所有数据，取最后一行非空行
            data = os.read(self.pipe_fd, 1024).decode().strip()
            if data:
                lines = data.split("\n")
                new_cmd = lines[-1].strip().upper()
                valid_states = [
                    "INIT", "READY", "RECORDING", "ERROR", "EXIT",
                    "CALIB_PRE", "CALIB_RUN", "CALIB_DONE"
                ]
                if new_cmd in valid_states:
                    self.state = new_cmd
                    print(f"State switched to: {self.state}")
        except OSError as e:
            if e.errno == errno.EAGAIN or e.errno == errno.EWOULDBLOCK:
                pass  # 没有数据
            else:
                print(f"Pipe error: {e}")

    def run(self):
        try:
            while True:
                self.read_command()
                self.update_effect()

                if self.state == "EXIT":
                    break

                time.sleep(0.02)  # 50Hz 刷新率
                self.tick += 1
        except KeyboardInterrupt:
            pass
        finally:
            os.close(self.pipe_fd)
            self.led.close()
            # 注意：不删除管道文件，留给 Bash 脚本处理或下次复用

    def update_effect(self):
        """根据当前状态渲染灯效"""
        t = self.tick

        if self.state == "INIT":
            # 蓝色常亮
            self.led.set_rgb(0, 0, 200)

        elif self.state == "READY":
            # 绿色呼吸 (周期约 3秒)
            # 使用 sin 函数生成 0.0 到 1.0 的平滑曲线
            brightness = (math.sin(t * 0.04) + 1) / 2

            self.led.set_scaled_rgb(255, 200, 0, brightness)

        elif self.state == "RECORDING":
            # 绿色闪烁 (周期 1秒: 0.5亮 0.5灭)
            if (t % 50) < 25:
                self.led.set_rgb(0, 255, 0)
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "ERROR":
            # 红色急促快闪
            if (t % 10) < 5:
                self.led.set_rgb(255, 0, 0)
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "CALIB_PRE":
            # 准备校准: 黄灯慢闪 (1Hz)
            if (t % 50) < 25:
                self.led.set_rgb(255, 200, 0) # 黄色
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "CALIB_RUN":
            # 校准中: 黄灯快闪 (5Hz)
            if (t % 10) < 5:
                self.led.set_rgb(255, 200, 0) # 黄色
            else:
                self.led.set_rgb(0, 0, 0)

        elif self.state == "CALIB_DONE":
            # 校准完成: 绿灯闪烁 (1Hz)
            if (t % 50) < 25:
                self.led.set_rgb(0, 255, 0) # 纯绿
            else:
                self.led.set_rgb(0, 0, 0)

        else:
            self.led.set_rgb(0, 0, 0)


if __name__ == "__main__":
    app = LedStateMachine()
    app.run()
