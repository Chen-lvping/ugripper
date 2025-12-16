import os
import subprocess
from pydub import AudioSegment
from pydub.generators import WhiteNoise

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(SCRIPT_DIR)

# -----------------------------------------
# 模型路径
# -----------------------------------------
MODEL_DIR = (
    "./model/sherpa-onnx-rk3576-20-seconds-sense-voice-zh-en-ja-ko-yue-2024-07-17/"
)
SENSE_VOICE_MODEL = os.path.join(MODEL_DIR, "model.rknn")
TOKENS = os.path.join(MODEL_DIR, "tokens.txt")

# -----------------------------------------
# 音频目录
# -----------------------------------------
AUDIO_DIR = "./audio"

# -----------------------------------------
# 查找音频文件并转换 MP3 -> WAV（并做简单增强）
# -----------------------------------------
audio_files = []

TARGET_DBFS = -20  # 统一音量到 -20 dBFS
PADDING_MS = 300  # 开头/结尾加 300ms 静音
NOISE_LEVEL_DB = -50  # 背景噪声幅度，可调为 -60 ~ -40

for f in os.listdir(AUDIO_DIR):
    full_path = os.path.join(AUDIO_DIR, f)
    name, ext = os.path.splitext(f)
    ext = ext.lower()

    if ext not in [".wav", ".mp3"]:
        continue

    wav_path = os.path.join(AUDIO_DIR, name + ".wav")
    if ext == ".mp3" or not os.path.exists(wav_path):
        # 加载音频
        if ext == ".mp3":
            audio = AudioSegment.from_mp3(full_path)
        else:
            audio = AudioSegment.from_wav(full_path)

        # 转为 16k 单声道
        audio = audio.set_frame_rate(16000).set_channels(1)

        # 音量归一化
        change_db = TARGET_DBFS - audio.dBFS
        audio = audio.apply_gain(change_db)

        # 加前后静音 padding
        padding = AudioSegment.silent(duration=PADDING_MS)
        audio = padding + audio + padding

        # 可选：加轻微背景噪声
        noise = WhiteNoise().to_audio_segment(duration=len(audio))
        noise = noise - abs(NOISE_LEVEL_DB)
        audio = audio.overlay(noise)

        # 导出 wav
        audio.export(wav_path, format="wav")
        print(f"已处理并导出: {wav_path}")

    audio_files.append(wav_path)

if not audio_files:
    print("未找到任何音频文件！")
    exit(0)

# -----------------------------------------
# 批量调用 sherpa-onnx-offline
# -----------------------------------------
for audio_file in audio_files:
    print(f"正在识别: {audio_file}")

    cmd = [
        "sherpa-onnx-offline",
        "--num-threads=1",
        "--provider=rknn",
        f"--sense-voice-model={SENSE_VOICE_MODEL}",
        f"--tokens={TOKENS}",
        audio_file,
    ]

    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    print("识别结果:")
    print(result.stdout.strip())

    if result.stderr.strip():
        print("警告/错误:")
        print(result.stderr.strip())
