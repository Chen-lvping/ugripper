# Ugripper 项目总览（精简版）

## 1. 目标与部署
- 目标：在 Radxa 上完成多源同步录制（主相机、触觉相机、Fays 双目+IMU、本体传感器）。
- 安装路径：`/opt/ugripper`。
- systemd 入口：`pack_script/ugripper.service`，主脚本为 `run_record.sh`。
- 数据根目录：`/mnt/data_disk/<device_sn>/data/episode_*`。

## 2. 角色分工（双臂）
- `DEVICE_SIDE`：物理侧（`left|right`）。
- `DEVICE_ROLE`：控制角色（`master|slave`，未配置时兼容旧逻辑：Right=Master、Left=Slave）。
- Master：按键控制、音频提示、发网络命令给对端。
- Slave：监听 `START|episode_xxx|master_sn` / `STOP|0`，按命令同步录制。
- 同步端口：`12345`（`nc`）。

## 3. 主流程（`run_record.sh`）
1. 启动时初始化目录、LED/Audio FIFO、GPIO、Fays 可用性。
 - 加载持久化标定文件：`/etc/ugripper/config/calibration/calibration.json`（缺失时回退 fake 模板）。
2. 后台 `monitor_loop` 运行健康检查（约 50ms 一次）并维护错误灯效状态。
3. `start_recording`：
- Master 先下发网络 `START|<episode_dir>|<master_sn>` 给 Slave。
- 开录前刷新一次持久化标定（支持 U 盘重复导入后立即生效）。
- 将当前运行时元数据复制到 `episode/metadata.json`，其中包含 `device_role`、`camera_codec`、`ugripper_version`、`ugripper_usb_updater_version` 与 `data_format_version`，便于后处理识别数据来源与结构版本。
- 将当前有效标定复制到 `episode/calibration.json`（Lerobot 风格）。
- 可用时启动 Fays 当前 episode（`run_fays_record.sh start <dir>`）。
- 启动相机录制：`build/src/camera_recorder/camera_recorder --codec <h264|h265> --output-dir <episode> --only <current-side streams>`。当前默认按 `DEVICE_SIDE` 仅录制本侧 `cam_main + tcam_l + tcam_r`，并自动生成兼容旧校验链路的 `cam.mkv`、`tact_left.mkv`、`tact_right.mkv` 与占位 `info.json`。
- 启动传感器：`build/src/sensor_recorder/sensor_recorder`。
- `sensor_recorder` 在录制开始后写入首条 encoder 样本时，会打印一次 `raw/rad/speed/timestamp` 到服务日志，便于现场快速确认编码器链路是否正常。
4. `stop_recording`：
- Master 下发 `STOP` 给 Slave。
- Slave 在停录后会将本次配对到的 `master_sn` 写入当前 episode 的 `info.json.paired_master_sn`。
- 停止 Fays 当前会话（daemon 保持常驻）。
- 停止相机与传感器进程，落盘 `sync`，执行录制完整性校验。
- `stop_recording` 会输出分阶段耗时日志，区分 `fays_stop`、`process_stop`、`metadata_finalize`、`data_sync_before_validation`、`validation`、`log_sync`、`state_cleanup`、`data_sync_final` 与 `ready_notify`，便于现场判断停录慢点落在“写盘”还是“校验”。

## 4. Fays 当前实现
### 4.1 进程与命令
- 脚本：`faysSense_vi_kit/scripts/run_fays_record.sh`
- 模式：`daemon | start <dir> | stop | exit`
- 控制 FIFO：`/tmp/umi_fays_cmd`

### 4.2 运行维护策略
- 启动时若未检测到 FTDI，服务保持运行并持续报错（ERROR_2）；Fays 恢复后自动拉起 daemon。
- 启动若检测到 Fays，会先延时 `FAYS_STARTUP_DELAY_SEC`（默认 3 秒）再启动 daemon，降低开机早期枚举抖动导致的异常。
- FTDI 在位检测由 `run_record.sh` 直接检查 `/dev/fays_stereo` 与 `/dev/fays_imu`（由 udev 规则固定映射）并写入 `/dev/shm/umi_fays_present`。
- 低开销链路速率检测：后台每秒刷新 `/dev/shm/umi_fays_usb_speed_mbps`（从 sysfs `speed` 读取，不做全量 USB 枚举）。
- health check 会把 Fays USB 速率跌落到 `FAYS_USB_ERROR5_MBPS`（默认 480Mb/s）及以下判为 `ERROR_5`。
- 运行中检测到 Fays 插入会自动拉起 daemon。
- 录制中若 daemon 恢复，仅恢复就绪，不补发当前 episode 的 `START`。

### 4.3 `fays_record_example` 线程模型（`record.cpp`）
- `ImgOnlineCapture`：读取双目帧，`VideoEncodeThread` 通过 `ffmpeg (rawvideo bgr24 -> h26x_rkmpp CQP)` 写 `fays_stereo_output.mkv`（编码器按 `/etc/environment` 的 `CAMERA_CODEC` 选择：`h264_rkmpp`/`hevc_rkmpp`）。
- `ImuOnlineCapture`：读取 IMU 并入队。
- `McapWriteThread`：统一处理 `fays_data.mcap` 的 `Open/Close/Log`（按 session 隔离）。
- `UsbConnectionWatchdog`：监控配置中的视频节点，断连时报错并退出进程。

### 4.4 录制数据
- `fays_stereo_output.mkv`
- `fays_data.mcap`：
- topic `i`：Fays IMU
- topic `c`：Fays 相机帧序号+时间戳
- `info.json`：基础字段由相机录制进程写入；Slave 侧在停录后追加 `paired_master_sn`。

## 5. 校验规则（episode 结束）
- 基础校验：`cam.mkv`、`tact_left.mkv`、`tact_right.mkv`、`sensor_data.mcap`、`fays_stereo_output.mkv`、`fays_data.mcap` 必须存在且有效。
- Fays 时长校验：使用 `ffprobe` 的时间戳跨度（`end-start`）比较 `cam.mkv` 与 `fays_stereo_output.mkv`。
- Fays 失败条件：Fays 比 cam 短超过 5 秒，或任一跨度读取失败。
- Fays MCAP 末尾校验：`fays_data.mcap` 必须同时包含 topic `i`/`c` 且消息数大于 0，并检查最后几帧 `c` 所在尾段仍有 `i`（IMU）覆盖；若末尾 IMU 断流则判失败。
- Fays MCAP 读取策略：校验脚本基于 MCAP summary 的尾部 chunk 索引逆向读取，只解析末尾少量 chunk，不做全量消息扫描，避免长录制文件超时误判。
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
- 持久化输出：`/etc/ugripper/config/calibration/calibration.json`；其中 Fays 图像条目的 `fps` 优先从 `fays_vikit.yaml` 的 `stereo_fps` 读取，缺失时写为 `unknown`，不再默认填 `60`；`residuals` 则读取 `output-results-imucam.txt` 中 `Residuals` 段的带单位结果（`[px]` / `[rad/s]` / `[m/s^2]`）。
- `imucam` 中 IMU 标定会导入连续/离散噪声密度（`Noise density` / `Noise density (discrete)`）及随机游走参数。
- 支持多次导入覆盖更新；后续 episode 在开录时读取最新持久化参数。
- 可与 `config.txt` 配置导入在同一次 U 盘流程中并行执行，导入完成后统一重启一次 `ugripper.service`。
- 若任一导入项失败，会切换红灯错误态（`ERROR_1`）提示后再执行服务重启。
## 9. 常用排障入口
- 服务日志：`journalctl -u ugripper.service -f`
- 内核 USB/UVC：`journalctl -k -f | egrep 'usb|uvcvideo|xhci|reset|disconnect|error -71'`
- Fays 校验失败：查看 episode 下 `validation_error.log`
- 录制锁：`/tmp/umi_recording.lock`
- PTP 状态：`/dev/shm/umi_ptp_status`
- 开机自恢复日志：`/var/log/ugripper/boot_install.log`（先比较/升级 backup 中的 `ugripper-usb-updater`，再比较/升级 `ugripper`；若 `ugripper` 未安装或状态异常则执行恢复安装）

## 10. USB 升级配置（语言/编码器/主从角色）
- `auto_update/usb_auto_update.sh` 挂载升级 U 盘后会检查根目录 `config.txt`。
- 支持配置键：`LANGUAGE`/`VOICE_LANG`（大小写不敏感），支持值：`zh|cn|chinese|中文` 与 `en|english`。
- 识别成功后写入 `/etc/environment`：`UGRIPPER_LANG=<zh|en>`。
- 支持编码器配置键：`CAMERA_CODEC`/`VIDEO_CODEC`/`TRIPLE_CAMERA_CODEC`/`CODEC`（大小写不敏感），支持值：`h264|h265`。
- 编码器识别成功后写入 `/etc/environment`：`CAMERA_CODEC=<h264|h265>`。
- 支持角色配置键：`DEVICE_ROLE`/`ROLE`（大小写不敏感），支持值：`master|slave`。
- 角色识别成功后写入 `/etc/environment`：`DEVICE_ROLE=<master|slave>`。
- 导入时序（配置 + 标定兼容）：先停止 `ugripper.service`，复用校准黄灯快闪态（`CALIB_RUN`）并至少保持 2 秒，全部导入成功后复用校准完成绿灯态（`CALIB_DONE`）1 秒，仅重启一次 `ugripper.service`。
- 若导入阶段存在失败，会改为红灯错误态（`ERROR_1`）闪烁提示，再重启 `ugripper.service`。
- `run_record.sh` 使用 `CAMERA_CODEC` 驱动三路相机编码参数。
- `fays_record_example` 直接读取 `/etc/environment` 的 `CAMERA_CODEC`，不依赖进程继承环境变量。
- `audio/audio_play.py` 启动时按 `UGRIPPER_LANG` 选语音：`en` 优先 `audio_en/`，文件缺失时回退 `audio/`。
