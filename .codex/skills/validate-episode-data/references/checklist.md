# Episode 校验清单

## 1. 必查文件

- `metadata.json`
- `calibration.json`
- `cam_left.mkv`
- `cam_right.mkv`
- `tcam_left_l.mkv`
- `tcam_left_r.mkv`
- `tcam_right_l.mkv`
- `tcam_right_r.mkv`
- `sensor_left.mcap`
- `sensor_right.mcap`

普通构建额外必查：

- `stereo_left.mkv`
- `stereo_right.mkv`
- `fays_data_left.mcap`
- `fays_data_right.mcap`

`software_version` 带 `+nostereo` 时不要求以上四个文件，且 `require_files` / `video_details` 不应声明它们。

## 1.1 推荐附加检查

- `validation_error.log` 是否存在
- `audio_pre.wav` / `audio_post.wav` 是否按预期出现
- `metadata.json` 是否符合当前 `data_version=3.1` 字段约定，并兼容历史 `3.0` 数据
- 可选 `task_start_s/task_stop_s` 是否为非负数且顺序有效；只有开始标记时按无回环数据处理
- `metadata.json` 顶层字段、`hardware_list` 字段和 `video_details[]` 字段顺序是否符合标准范本
- `metadata.json` / `calibration.json` 的顶层 key 集合是否仍符合约定
- 关键 JSON 字段类型是否仍符合约定，防止“有字段但类型偷偷变了”
- `video_details` 是否不再重复写 `serial`，且 `start_offset_us` 为视频 `PTS=0` 对应的 Unix 微秒 offset，不允许最早一路写 `0` 后按 episode 做相对归一化
- `calibration.json.observation.images` 是否覆盖 8 路图像
- Fays 侧 `fays_data_left.mcap` / `fays_data_right.mcap` 是否头尾 magic 完整、可解析、包含 `i/c` 数据，且覆盖时长接近对应 stereo 视频
- Fays `c.publishTime` 是否单调，每侧首个 `publishTime / 1000` 是否与对应 stereo metadata `start_offset_us` 一致；`c` 消息数、frameIndex 与 stereo MKV packet 数是否一一对应
- stereo `duration_s` 是否使用首末 `c.publishTime` 跨度；MKV 固定帧率 PTS 因真实 camera gap 被压紧时，只报告 gap/缺帧，不把容器短时长误当作系统时钟偏移
- 主摄专项扫描是否发现 `backward_dts` / `decode_non_monotonic_dts` / `decode_error`
- 是否输出统一的首帧对齐表，覆盖全部启用视频、本机 MCAP 实际发现的全部 sensor topic、左右 Fays IMU/camera 和可选 `ego/sensor.mcap` 全部 topic，并列出各流 Unix 时间、换算方法及相对最早流的误差
- Ego `head_pose/controller_pose/imu` 等设备单调时钟 topic 是否用 `ego/metadata.json.head_pose_details[name=head_pose].start_offset_us` 计算统一 offset 后换算；已是 Unix `publishTime` 的 `*_metainfo` 是否保持原值而未二次偏移

## 2. 默认阈值建议

这些阈值是“录后体检”的默认经验值，适合作为首轮筛查；真正判责时仍要结合现场机型、录制时长和日志交叉确认。

- 视频起始对齐 warn/fail：`500ms / 1000ms`，即使未超阈值也必须输出各路起始偏移表
- 视频结束对齐 warn/fail：`150ms / 300ms`
- 全局首帧同步硬阈值：最大相对误差达到 `1000ms` 时 fail；无论是否超阈值都输出逐流相对误差表
- 视频或同类传感器的低阈值偏差可继续作为 warn，供后续启动时序优化，不提前改变全局首帧同步的 `1000ms` fail 口径
- 左右成对视频起始对齐 warn/fail：`500ms / 1000ms`
- 左右成对视频结束对齐 warn/fail：`80ms / 180ms`
- 单视频 gap 告警：`max(3 x 中位帧间隔, 80ms)`
- 容器 duration 与包级 span 偏差 warn/fail：`80ms / 150ms`
- 单 IMU topic gap 告警：`max(3 x 中位间隔, 20ms)`
- 单 encoder topic gap 告警：`max(3 x 中位间隔, 30ms)`
- 左右同类 sensor 起始同步 warn/fail：`30ms / 1000ms`
- 左右同类 sensor 结束同步 warn/fail：`50ms / 120ms`
- sensor 覆盖率相对最长视频 warn/fail：`0.90 / 0.75`
- `stereo -> tactile -> main` 分组错峰起录 warn/fail：`80ms / 150ms`

## 3. 常见异常模式

- `单路视频 max gap 很大，其他流正常`
  - 更像单设备掉流、编码卡顿或 mux 异常。
- `多路视频在相近时间点同时出现大 gap`
  - 更像系统抖动、IO 抢占或 stop 边界卡顿。
- `左右 sensor 同时早停，但视频完整`
  - 更像 sensor 进程或串口链路提前结束。
- `视频首帧范围很大，但后续 gap 正常`
  - 更像起录边界没对齐，而不是中途掉流。
- `四个 sensor topic 内部首帧对齐正常，但统一首帧表里 sensor 整体相对视频偏移很大`
  - 更像视频与 sensor 使用了不同时间基准，或某一侧时间戳换算错误。
- `sensor 起始时间整体晚于视频`
  - 更像 sensor_recorder 启动滞后。
- `sensor 结束时间整体早于视频`
  - 更像 sensor_recorder 提前停录或尾部写入失败。
- `stereo_session 边界和实际 stereo 文件边界差很多`
  - 更像 warmup/session 合并边界异常，需要继续看 `camera_recorder` / daemon 日志。
- `metadata / calibration 缺字段，但视频和 sensor 文件齐全`
  - 更像 episode 元数据落盘不完整，而不是录制流本身坏掉。
- `左右主摄都有 decode_non_monotonic_dts，但只有单侧 backward_dts`
  - 更像主摄时间戳链路的共性 + 单侧额外容器排序抖动。
