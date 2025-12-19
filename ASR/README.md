参考文档
https://k2-fsa.github.io/sherpa/onnx/

`wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx`

`wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-rk3576-20-seconds-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2
tar xvf sherpa-onnx-rk3576-20-seconds-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2`

都放在ugripper/ASR/model下

音频文件放在ugripper/ASR/audio下

在ugripper路径`uv run ./ASR/test.py`