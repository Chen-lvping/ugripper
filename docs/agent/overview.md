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
 - 加载持久化标定文件：`/etc/ugripper/config/calibration/calibration.json`（缺失时回退 fake 模板）。
2. 后台 `monitor_loop` 运行健康检查（约 50ms 一次）并维护错误灯效状态。
3. `start_recording`：
- Right 先下发网络 `START` 给 Left。
- 开录前刷新一次持久化标定（支持 U 盘重复导入后立即生效）。
- 将当前有效标定复制到 `episode/calibration.json`（Lerobot 风格）。
- 可用时启动 Fays 当前 episode（`run_fays_record.sh start <dir>`）。
- 启动三路相机：`camera_record/triple_camera_record.py --codec <h264|h265>`（从 `/etc/environment` 的 `CAMERA_CODEC` 加载，默认 `h264`）。
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
- 启动时若未检测到 FTDI，服务保持运行并持续报错（ERROR_2）；Fays 恢复后自动拉起 daemon。
- FTDI 在位检测由 `run_record.sh` 直接检查 `/dev/fays_stereo` 与 `/dev/fays_imu`（由 udev 规则固定映射）并写入 `/dev/shm/umi_fays_present`。
- 运行中检测到 Fays 插入会自动拉起 daemon。
- 录制中若 daemon 恢复，仅恢复就绪，不补发当前 episode 的 `START`。

### 4.3 `fays_record_example` 线程模型（`record.cpp`）
- `ImgOnlineCapture`：读取双目帧并写 `fays_stereo_output.mkv`（ffmpeg 编码器按 `/etc/environment` 的 `CAMERA_CODEC` 选择：`h264_rkmpp`/`hevc_rkmpp`）。
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
- Fays 失败条件：Fays 比 cam 短超过 5 秒，或任一跨度读取失败。
- tact 时长校验：读取 `sensor_data.mcap` summary 的消息起止时间，与 `tact_left.mkv`、`tact_right.mkv` 的时间戳跨度分别比较，任一绝对误差超过 5 秒判失败。

## 6. 状态与告警
- LED 状态：`INIT`、`READY`、`RECORDING`、`ERROR_1~ERROR_5`。
- `READY`：绿色呼吸灯（约 3 秒周期，基于系统时间相位）。
- `RECORDING`：1Hz 绿色闪烁（基于系统时间相位）。
- 左右臂系统时间同步后，`READY/RECORDING` 灯效可保持同相。
- 错误等级：`ERROR_1`（数据完整性）到 `ERROR_5`（运行时错误）。

## 7. 构建与打包
- CMake 构建：根目录 `CMakeLists.txt` 聚合 `sensor_recorder` 与 `faysSense_vi_kit`。
- deb 打包脚本：`build_deb.sh`。
- 关键二进制：
- `build/src/sensor_recorder/sensor_recorder`
- `build/src/sensor_recorder/zeroing`
- `build/faysSense_vi_kit/fays_record_example`

## 8. U 盘标定导入
- 入口：`auto_update/usb_auto_update.sh`（由 udev + `usb-auto-update@.service` 触发）。
- 检测目录：`ugripper_calib/<DEVICE_SN>/`（按设备 SN 匹配）。
- 文件：`*camchain*.yaml`（主摄）+ `*imucam*.txt`（Fays 双目 + IMU）。
- 导入脚本：`auto_calibration/import_camera_calibration.sh`。
- 持久化输出：`/etc/ugripper/config/calibration/calibration.json`。
- 支持多次导入覆盖更新；后续 episode 在开录时读取最新持久化参数。
## 9. 常用排障入口
- 服务日志：`journalctl -u ugripper.service -f`
- 内核 USB/UVC：`journalctl -k -f | egrep 'usb|uvcvideo|xhci|reset|disconnect|error -71'`
- Fays 校验失败：查看 episode 下 `validation_error.log`
- 录制锁：`/tmp/umi_recording.lock`
- PTP 状态：`/dev/shm/umi_ptp_status`
- 开机自恢复日志：`/var/log/ugripper/boot_install.log`（先比较/升级 backup 中的 `ugripper-usb-updater`，再检查 `ugripper` 恢复）

## 10. USB 升级语言与编码器配置
- `auto_update/usb_auto_update.sh` 挂载升级 U 盘后会检查根目录 `config.txt`。
- 支持配置键：`LANGUAGE`/`VOICE_LANG`（大小写不敏感），支持值：`zh|cn|chinese|中文` 与 `en|english`。
- 识别成功后写入 `/etc/environment`：`UGRIPPER_LANG=<zh|en>`。
- 支持编码器配置键：`CAMERA_CODEC`/`VIDEO_CODEC`/`TRIPLE_CAMERA_CODEC`/`CODEC`（大小写不敏感），支持值：`h264|h265`。
- 编码器识别成功后写入 `/etc/environment`：`CAMERA_CODEC=<h264|h265>`。
- `run_record.sh` 使用 `CAMERA_CODEC` 驱动三路相机编码参数。
- `fays_record_example` 直接读取 `/etc/environment` 的 `CAMERA_CODEC`，不依赖进程继承环境变量。
- `audio/audio_play.py` 启动时按 `UGRIPPER_LANG` 选语音：`en` 优先 `audio_en/`，文件缺失时回退 `audio/`。
