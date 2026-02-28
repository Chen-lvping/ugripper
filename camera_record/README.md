# Camera Record

## 安装依赖

```bash
pip install av

sudo cp 99-fixed-usb-map.rules /etc/udev/rules.d/

sudo udevadm control --reload-rules
sudo udevadm trigger

```
安装ffmpeg-rockchip
https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation


## 录制工具

### 硬件加速版（推荐）- `triple_camera_record.py`

针对 Rockchip RK3576 平台优化，使用全链路硬件编码。

*   **编码格式**: 默认 H.264，可选 H.265（`--codec h264|h265`）
*   **优势**: CPU 占用低，支持断电保护容器 `.mkv`。

**使用方法：**

```bash
# 录制 60 秒，使用默认 H.264 硬编码
python triple_camera_record.py --output-dir raw_data -d 60

# 录制 60 秒，显式使用 H.265 硬编码
python triple_camera_record.py --output-dir raw_data -d 60 --codec h265
```

## 输出结构

```
raw_data/
└── episode_YYYYMMDD_DEVICEID_XXXX/
    ├── cam.mkv          # 主摄视频
    ├── cam.csv          # 主摄时间戳
    ├── tact_left.mkv    # 左触觉视频
    ├── tact_left.csv    # 左触觉时间戳
    ├── tact_right.mkv   # 右触觉视频
    └── tact_right.csv   # 右触觉时间戳
```
