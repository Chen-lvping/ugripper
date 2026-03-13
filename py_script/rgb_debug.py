#!/usr/bin/env python3
import argparse
import os
import signal
import sys
import time

PWM_BASE_PATH = "/sys/class/pwm"
PWM_CONFIG = {
    "green": {"chip": "pwmchip1", "channel": "0"},
    "blue": {"chip": "pwmchip0", "channel": "0"},
    "red": {"chip": "pwmchip3", "channel": "0"},
}


class PwmChannel:
    def __init__(self, chip_name, channel_id, period_ns=1_000_000):
        self.chip_name = chip_name
        self.channel_id = str(channel_id)
        self.chip_path = os.path.join(PWM_BASE_PATH, chip_name)
        self.pwm_path = os.path.join(self.chip_path, f"pwm{self.channel_id}")
        self.period_ns = period_ns

        if not os.path.exists(self.chip_path):
            raise FileNotFoundError(f"PWM chip not found: {self.chip_path}")

        self._export_channel()
        self.set_period(period_ns)
        self.set_enable(True)
        self.set_level(0.0)

    def _write(self, filename, value):
        with open(os.path.join(self.pwm_path, filename), "w") as f:
            f.write(str(value))

    def _export_channel(self):
        if os.path.exists(self.pwm_path):
            return

        try:
            with open(os.path.join(self.chip_path, "export"), "w") as f:
                f.write(self.channel_id)
        except OSError:
            pass

        deadline = time.time() + 2.0
        while not os.path.exists(self.pwm_path):
            if time.time() > deadline:
                raise TimeoutError(f"Export timeout: {self.chip_name}/pwm{self.channel_id}")
            time.sleep(0.02)

    def set_period(self, ns):
        self._write("period", int(ns))

    def set_enable(self, enable):
        self._write("enable", "1" if enable else "0")

    def set_duty_cycle(self, ns):
        self._write("duty_cycle", int(ns))

    def set_level(self, brightness):
        brightness = max(0.0, min(1.0, float(brightness)))
        duty = int((1.0 - brightness) * self.period_ns)
        self.set_duty_cycle(duty)

    def shutdown(self):
        try:
            self.set_level(0.0)
        except Exception:
            pass


class RgbLed:
    def __init__(self):
        self.leds = {
            color: PwmChannel(cfg["chip"], cfg["channel"])
            for color, cfg in PWM_CONFIG.items()
        }

    def set_rgb(self, r, g, b, scale=1.0):
        scale = max(0.0, min(1.0, float(scale)))
        r = max(0, min(255, int(r)))
        g = max(0, min(255, int(g)))
        b = max(0, min(255, int(b)))

        self.leds["red"].set_level((r / 255.0) * scale)
        self.leds["green"].set_level((g / 255.0) * scale)
        self.leds["blue"].set_level((b / 255.0) * scale)

    def off(self):
        self.set_rgb(0, 0, 0)

    def close(self):
        for channel in self.leds.values():
            channel.shutdown()


def interactive_loop(led: RgbLed, init_rgb, init_scale):
    r, g, b = init_rgb
    scale = init_scale
    led.set_rgb(r, g, b, scale)

    print("RGB debug interactive mode")
    print("Commands:")
    print("  r g b [scale]      Set static color, e.g. '255 32 0 0.7'")
    print("  blink r g b hz sec Blink color, e.g. 'blink 255 0 0 2 3'")
    print("  off                Turn LED off")
    print("  show               Print current value")
    print("  q                  Quit")

    while True:
        try:
            raw = input("rgb> ").strip()
        except EOFError:
            break

        if not raw:
            continue

        if raw in {"q", "quit", "exit"}:
            break

        if raw == "off":
            led.off()
            r, g, b, scale = 0, 0, 0, 1.0
            continue

        if raw == "show":
            print(f"current: r={r} g={g} b={b} scale={scale:.2f}")
            continue

        parts = raw.split()

        if parts[0] == "blink":
            if len(parts) < 6:
                print("usage: blink r g b hz sec")
                continue
            try:
                br, bg, bb = int(parts[1]), int(parts[2]), int(parts[3])
                hz = max(0.1, float(parts[4]))
                sec = max(0.1, float(parts[5]))
            except ValueError:
                print("invalid blink args")
                continue

            period = 1.0 / hz
            end_ts = time.time() + sec
            on = True
            while time.time() < end_ts:
                if on:
                    led.set_rgb(br, bg, bb)
                else:
                    led.off()
                on = not on
                time.sleep(period / 2.0)
            led.off()
            continue

        if len(parts) < 3:
            print("usage: r g b [scale]")
            continue

        try:
            r, g, b = int(parts[0]), int(parts[1]), int(parts[2])
            scale = float(parts[3]) if len(parts) >= 4 else 1.0
            led.set_rgb(r, g, b, scale)
        except ValueError:
            print("invalid color args")


def main():
    parser = argparse.ArgumentParser(description="Manual RGB debug tool for Ugripper PWM LED")
    parser.add_argument("--r", type=int, default=0, help="Red [0-255]")
    parser.add_argument("--g", type=int, default=0, help="Green [0-255]")
    parser.add_argument("--b", type=int, default=0, help="Blue [0-255]")
    parser.add_argument("--scale", type=float, default=1.0, help="Brightness scale [0.0-1.0]")
    parser.add_argument(
        "--mode",
        choices=["interactive", "static"],
        default="interactive",
        help="interactive: REPL, static: set once and hold",
    )
    parser.add_argument("--hold", type=float, default=3.0, help="Static mode hold seconds")
    parser.add_argument("--keep", action="store_true", help="Keep LED state on exit")
    args = parser.parse_args()

    led = None

    def cleanup(signum=None, frame=None):
        if led is not None and not args.keep:
            led.off()
            led.close()
        if signum is not None:
            sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    try:
        led = RgbLed()
        if args.mode == "static":
            led.set_rgb(args.r, args.g, args.b, args.scale)
            time.sleep(max(0.0, args.hold))
        else:
            interactive_loop(led, (args.r, args.g, args.b), args.scale)
    except PermissionError:
        print("Permission denied: need access to /sys/class/pwm (root or proper udev rules)")
        return 1
    except Exception as exc:
        print(f"RGB debug failed: {exc}")
        return 1
    finally:
        cleanup()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
