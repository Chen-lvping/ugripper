# Ugripper 项目总览（精简版）

## 1. 目标与部署
- 目标：在 Radxa 上完成多源同步录制（主相机、触觉相机、Fays 双目+IMU、本体传感器）。
- 安装路径：`/opt/ugripper`。
- systemd 入口：`pack_script/ugripper.service`，主脚本为 `run_record.sh`。
- 数据根目录：`/mnt/data_disk/<device_sn>/data/episode_*`。

## 2. 角色分工（双臂）
- Right（Master）：按键控制、音频提示、发网络命令给 Left。
- Left（Slave）：监听 `START|episode_xxx` / `STOP|0`，按命令同步录制。
- 同步端口：`12345`（`nc`）。

## 3. 主流程（`run_record.sh`）
1. 启动时初始化目录、LED/Audio FIFO、GPIO、Fays 可用性。
2. 后台 `monitor_loop` 运行健康检查（约 50ms 一次）并维护错误灯效状态。
3. `start_recording`：
- Right 先下发网络 `START` 给 Left。
- 可用时启动 Fays 当前 episode（`run_fays_record.sh start <dir>`）。
- 启动三路相机：`camera_record/triple_camera_record.py --codec <h264|h265>`（默认 `h264`）。
- 启动传感器：`build/src/sensor_recorder/sensor_recorder`。
4. `stop_recording`：
- Right 下发 `STOP` 给 Left。
- 停止 Fays 当前会话（daemon 保持常驻）。
- 停止相机与传感器进程，落盘 `sync`，执行录制完整性校验。

## 4. Fays 当前实现
### 4.1 进程与命令
- 脚本：`faysSense_vi_kit/scripts/run_fays_record.sh`
- 模式：`daemon | start <dir> | stop | exit`
- 控制 FIFO：`/tmp/umi_fays_cmd`

### 4.2 运行维护策略
- 启动时若未检测到 FTDI，可无 Fays 继续录制。
- 运行中检测到 Fays 插入会自动拉起 daemon。
- 录制中若 daemon 恢复，仅恢复就绪，不补发当前 episode 的 `START`。

### 4.3 `fays_record_example` 线程模型（`record.cpp`）
- `ImgOnlineCapture`：读取双目帧并写 `fays_stereo_output.mkv`（ffmpeg `h264_rkmpp` 硬编码，H.264）。
- `ImuOnlineCapture`：读取 IMU 并入队。
- `McapWriteThread`：统一处理 `fays_data.mcap` 的 `Open/Close/Log`（按 session 隔离）。
- `UsbConnectionWatchdog`：监控配置中的视频节点，断连时报错并退出进程。

### 4.4 录制数据
- `fays_stereo_output.mkv`
- `fays_data.mcap`：
- topic `i`：Fays IMU
- topic `c`：Fays 相机帧序号+时间戳

## 5. 校验规则（episode 结束）
- 基础校验：`cam.mkv`、`tact_left.mkv`、`tact_right.mkv`、`sensor_data.mcap` 必须存在且有效。
- 若该次期望 Fays：还需 `fays_stereo_output.mkv`、`fays_data.mcap`。
- Fays 时长校验：使用 `ffprobe` 的时间戳跨度（`end-start`）比较 `cam.mkv` 与 `fays_stereo_output.mkv`。
- 失败条件：Fays 比 cam 短超过 10 秒，或任一跨度读取失败。

## 6. 状态与告警
- LED 状态：`INIT`、`READY`、`RECORDING`、`ERROR_1~ERROR_5`。
- `RECORDING`：1Hz 绿色闪烁。
- 错误等级：`ERROR_1`（数据完整性）到 `ERROR_5`（运行时错误）。

## 7. 构建与打包
- CMake 构建：根目录 `CMakeLists.txt` 聚合 `sensor_recorder` 与 `faysSense_vi_kit`。
- deb 打包脚本：`build_deb.sh`。
- 关键二进制：
- `build/src/sensor_recorder/sensor_recorder`
- `build/src/sensor_recorder/zeroing`
- `build/faysSense_vi_kit/fays_record_example`

## 8. 常用排障入口
- 服务日志：`journalctl -u ugripper.service -f`
- 内核 USB/UVC：`journalctl -k -f | egrep 'usb|uvcvideo|xhci|reset|disconnect|error -71'`
- Fays 校验失败：查看 episode 下 `validation_error.log`
- 录制锁：`/tmp/umi_recording.lock`
- PTP 状态：`/dev/shm/umi_ptp_status`
