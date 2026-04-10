# Episode 校验清单

## 1. 必查文件

- `metadata.json`
- `calibration.json`
- `info.json`
- `left_cam_main.mkv`
- `right_cam_main.mkv`
- `left_stereo.mkv`
- `right_stereo.mkv`
- `left_tcam_l.mkv`
- `left_tcam_r.mkv`
- `right_tcam_l.mkv`
- `right_tcam_r.mkv`
- `sensor_data_left.mcap`
- `sensor_data_right.mcap`

## 2. 默认阈值建议

这些阈值是“录后体检”的默认经验值，适合作为首轮筛查；真正判责时仍要结合现场机型、录制时长和日志交叉确认。

- 视频起始对齐 warn/fail：`80ms / 150ms`
- 视频结束对齐 warn/fail：`150ms / 300ms`
- 首帧同步误差 warn/fail：`33ms / 80ms`
- 单视频 gap 告警：`max(3 x 中位帧间隔, 80ms)`
- 单 IMU topic gap 告警：`max(3 x 中位间隔, 20ms)`
- 单 encoder topic gap 告警：`max(3 x 中位间隔, 30ms)`
- 左右同类 sensor 起始同步 warn/fail：`30ms / 80ms`
- 左右同类 sensor 结束同步 warn/fail：`50ms / 120ms`
- 单侧视频与 sensor 起始对齐 warn/fail：`120ms / 300ms`
- 单侧视频与 sensor 结束对齐 warn/fail：`150ms / 400ms`

## 3. 常见异常模式

- `单路视频 max gap 很大，其他流正常`
  - 更像单设备掉流、编码卡顿或 mux 异常。
- `多路视频在相近时间点同时出现大 gap`
  - 更像系统抖动、IO 抢占或 stop 边界卡顿。
- `左右 sensor 同时早停，但视频完整`
  - 更像 sensor 进程或串口链路提前结束。
- `视频首帧范围很大，但后续 gap 正常`
  - 更像起录边界没对齐，而不是中途掉流。
- `sensor 起始时间整体晚于视频`
  - 更像 sensor_recorder 启动滞后。
- `sensor 结束时间整体早于视频`
  - 更像 sensor_recorder 提前停录或尾部写入失败。
- `stereo_session 边界和实际 stereo 文件边界差很多`
  - 更像 warmup/session 合并边界异常，需要继续看 `camera_recorder` / daemon 日志。
