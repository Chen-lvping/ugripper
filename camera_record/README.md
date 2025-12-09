# Camera Record

## 安装依赖

安装ffmpeg-rockchip
https://github.com/nyanmisaka/ffmpeg-rockchip/wiki/Compilation


## 录制工具

### 1. 硬件加速版 (推荐) - `triple_camera_record_h265.py`

针对 Rockchip RK3576 平台优化的版本，使用全链路硬件加速。

*   **编码格式**: H.265 (HEVC)
*   **优势**: CPU 占用极低，文件体积小 (1080p60 约 1MB/s)，支持断电保护 (.mkv)。


**使用方法：**

```bash
# 录制 60 秒
python triple_camera_record_h265.py -d 60

# 指定输出目录
python triple_camera_record_h265.py -d 60 -o raw_data
```

### 2. 通用版 (旧版) - `triple_camera_record.py`

使用 CPU 进行 MJPEG 透传或软件编码。

*   **编码格式**: MJPEG (原始流)
*   **缺点**: 文件体积巨大 (1080p60 约 8MB/s)，CPU 负载较高。
*   **适用场景**: 调试、非 Rockchip 平台。

**使用方法：**

```bash
python triple_camera_record.py -d 60
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
