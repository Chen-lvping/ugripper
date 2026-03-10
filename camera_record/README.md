# Camera Record

> 历史说明：当前 `run_record.sh` 默认已切到 C++ `camera_recorder`，本目录脚本保留作历史实现与调试参考。


## 录制工具

### 正式录制脚本 - `triple_camera_record.py`

面向 Rockchip RK3576 平台的三相机同步录制工具，使用主摄 FFmpeg + 触觉 Hybrid 混合链路。

- 主相机：`ffmpeg v4l2(NV12 1920x1080) -> h26x_rkmpp`
- 触觉相机：`v4l2src(MJPEG) -> mppjpegdec -> appsink -> ffmpeg h26x_rkmpp(CQP)`
- 编码格式：默认 `h264`，可选 `h265`
- 输出容器：`.mkv`

**使用方法：**

```bash
python triple_camera_record.py --output-dir raw_data
python triple_camera_record.py --output-dir raw_data -d 60 --codec h265
```

## 输出结构

```text
raw_data/
└── episode_YYYYMMDD_DEVICEID_XXXX/
    ├── cam.mkv
    ├── tact_left.mkv
    ├── tact_right.mkv
    └── info.json
```

## 时间戳恢复

脚本不会额外输出 CSV。

`info.json` 在保留原有 `boot_time_offset` / `boot_time_offset_us` 的基础上，新增：

- `cam_record_time_offset_us`
- `tact_left_record_time_offset_us`
- `tact_right_record_time_offset_us`

后处理按以下公式恢复视频帧对应的系统时间：

```text
frame_system_time_us = frame_pts_us + <camera>_record_time_offset_us
```
