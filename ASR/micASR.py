import os
import subprocess
import time
from pathlib import Path
from pydub import AudioSegment
from pydub.generators import WhiteNoise
import atexit
import signal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(SCRIPT_DIR)

# ---------------- GPIO 配置 ----------------
PIN_HIGH = "PIN_32"  # 置高使能
PIN_BTN = "PIN_36"  # 按钮输入
BTN_ACTIVE_LEVEL = 1
DEBOUNCE_S = 0.03
CLICK_INTERVAL = 0.5
MAX_AUDIO_FILES = 5  # 最多保留的音频文件数
AUDIO_PREFIX = "usb_record_"
AUDIO_SUFFIX = ".wav"


# ---------------- 音频文件管理 ----------------
def maintain_audio_limit_by_timestamp(
    audio_dir: Path, prefix=AUDIO_PREFIX, suffix=AUDIO_SUFFIX
):
    files = []
    for f in audio_dir.glob(f"{prefix}*{suffix}"):
        try:
            ts_str = f.stem.replace(prefix, "")
            ts = int(ts_str)
            files.append((ts, f))
        except ValueError:
            continue

    files.sort(key=lambda x: x[0])

    while len(files) > MAX_AUDIO_FILES:
        ts, oldest = files.pop(0)
        print(f"Removing old audio file: {oldest}")
        oldest.unlink()


# ---------------- 通用进程管理 ----------------
def start_process(cmd, use_shell=False):
    """启动进程并创建新进程组，以便 kill 时能杀掉子进程"""
    if use_shell:
        proc = subprocess.Popen(
            cmd,
            shell=True,
            preexec_fn=os.setsid,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    else:
        proc = subprocess.Popen(
            cmd,
            preexec_fn=os.setsid,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    return proc


def stop_process(proc):
    """优雅结束进程及其子进程"""
    if proc and proc.poll() is None:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=1)
        except Exception:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        finally:
            if proc.stdout:
                proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()


# ---------------- GPIO 初始化 ----------------
def setup_gpio_high(pin):
    try:
        subprocess.run("pkill -f gpioset", shell=True)
        subprocess.run("pkill -f arecord", shell=True)
        pin_signal = (
            subprocess.check_output(f'gpiofind "{pin}"', shell=True).decode().strip()
        )
        proc = start_process(f"gpioset -m signal {pin_signal}=1", use_shell=True)
        print(f"GPIO {pin} set HIGH, PID: {proc.pid}")
        return proc
    except Exception as e:
        print(f"Failed to set GPIO {pin} high: {e}")
        return None


def read_gpio(pin):
    try:
        pin_signal = (
            subprocess.check_output(f'gpiofind "{pin}"', shell=True).decode().strip()
        )
        val = (
            subprocess.check_output(f"gpioget {pin_signal}", shell=True)
            .decode()
            .strip()
        )
        return int(val)
    except Exception:
        return 0


gpio_high_proc = setup_gpio_high(PIN_HIGH)
atexit.register(stop_process, gpio_high_proc)

# ---------------- ASR & 音频设置 ----------------
MODEL_DIR = (
    "./model/sherpa-onnx-rk3576-20-seconds-sense-voice-zh-en-ja-ko-yue-2024-07-17/"
)
SENSE_VOICE_MODEL = os.path.join(MODEL_DIR, "model.rknn")
TOKENS = os.path.join(MODEL_DIR, "tokens.txt")
SILERO_VAD_MODEL = "./model/silero_vad.onnx"
SILERO_VAD_THRESHOLD = 0.4

AUDIO_DIR = Path("./audio")
AUDIO_DIR.mkdir(exist_ok=True)
print("Audio dir:", AUDIO_DIR.resolve())

TARGET_DBFS = -20
PADDING_MS = 300
NOISE_LEVEL_DB = -50

# ---------------- 状态 ----------------
is_recording_mode = False
click_count = 0
last_click_time = 0
record_proc = None
current_raw_path = None


# ---------------- 音频处理 ----------------
def enhance_audio(input_path: Path) -> Path:
    wav_path = input_path.with_suffix(".wav")
    audio = AudioSegment.from_file(input_path)
    audio = audio.set_frame_rate(16000).set_channels(1)
    audio = audio.apply_gain(TARGET_DBFS - audio.dBFS)
    padding = AudioSegment.silent(duration=PADDING_MS)
    audio = padding + audio + padding
    noise = WhiteNoise().to_audio_segment(duration=len(audio)) - abs(NOISE_LEVEL_DB)
    audio = audio.overlay(noise)
    audio.export(wav_path, format="wav")
    return wav_path


# ---------------- ASR 识别 ----------------
def run_asr(wav_path: Path):
    cmd = [
        "sherpa-onnx-vad-with-offline-asr",
        "--num-threads=1",
        "--provider=rknn",
        f"--silero-vad-model={SILERO_VAD_MODEL}",
        f"--silero-vad-threshold={SILERO_VAD_THRESHOLD}",
        f"--sense-voice-model={SENSE_VOICE_MODEL}",
        f"--tokens={TOKENS}",
        str(wav_path),
    ]
    result = subprocess.run(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    print(result.stdout.strip())


# ---------------- 主循环 ----------------
print("System ready. Double press button to start/stop recording mode.")

try:
    while True:
        btn_val = read_gpio(PIN_BTN)

        if btn_val == BTN_ACTIVE_LEVEL:
            now = time.time()
            if now - last_click_time < CLICK_INTERVAL:
                click_count += 1
            else:
                click_count = 1
            last_click_time = now

            # 等待按钮释放
            while read_gpio(PIN_BTN) == BTN_ACTIVE_LEVEL:
                time.sleep(0.01)

            if click_count == 2:
                is_recording_mode = not is_recording_mode
                state = "ON" if is_recording_mode else "OFF"
                print(f"Recording mode toggled {state}")
                click_count = 0

                if is_recording_mode:
                    timestamp = int(time.time())
                    current_raw_path = AUDIO_DIR / f"usb_record_{timestamp}.wav"
                    print(f"Start recording to {current_raw_path} ...")
                    record_proc = start_process(
                        f"arecord -Dhw:2,0 -f S16_LE -r 44100 -c 1 {current_raw_path}",
                        use_shell=True,
                    )
                else:
                    if record_proc:
                        print("Stop recording...")
                        stop_process(record_proc)
                        record_proc = None

                        # 音频增强
                        wav_path = enhance_audio(current_raw_path)
                        print(f"Audio saved: {wav_path}")

                        # 管理音频文件数量
                        maintain_audio_limit_by_timestamp(AUDIO_DIR, suffix=".wav")

                        # ASR
                        print("Running ASR...")
                        run_asr(wav_path)
                        print("Ready for next recording. Press button twice to start.")

        time.sleep(DEBOUNCE_S)

except KeyboardInterrupt:
    print("Exiting...")
finally:
    stop_process(record_proc)
    stop_process(gpio_high_proc)
