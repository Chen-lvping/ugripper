# ugripper 项目架构（可复用模板）

本文档总结 `ugripper` 数采夹爪系统在 `/opt/ugripper` 部署后的整体架构，可作为后续“多传感器并发录制 + 自动部署”项目的蓝本。内容按运行时组件、支撑服务、数据流、打包链路和可扩展性组织。

## 1. 系统概览
- **目标**：在 Radxa 嵌入式平台上可靠同步录制视觉、触觉、FaysSense 双目、IMU、编码器等多模态数据，并提供现场 LED/音频反馈。
- **运行角色**：双臂协同，物理侧与控制角色解耦。`DEVICE_SIDE` 表示物理 `left/right`，`DEVICE_ROLE` 表示控制角色 `master/slave`（未配置时兼容旧逻辑：Right=Master、Left=Slave）。
- **部署形态**：脚本、C++ 二进制和资源打包到 `/opt/ugripper`，通过 systemd 与 udev 触发器驱动；数据写入 `/mnt/data_disk/<device_id>/`。

## 2. 核心运行组件

### 2.1 主业务服务（`ugripper.service` -> `run_record.sh`）
- 由 `pack_script/ugripper.service` 安装并常驻运行，工作目录 `/opt/ugripper`，用户 `radxa`。
- 负责：
  - 磁盘/目录准备（`metadata`、`calibration`、`data`）。
  - 元数据与校准文件初始化（`config/fake*Calib.json` -> `/mnt/data_disk/.../calibration/`）。
  - 触觉相机序列号校验，替换 JSON 中 `{{TACTILE_*_SERIAL}}` 占位符。
  - PTP 状态读取（`/dev/shm/umi_ptp_status`）并驱动 LED/AUDIO 状态。
  - 物理按钮状态机：
    - 上键短按：启动/停止一次录制。
    - 上键长按：录制下一条任务 pre 语音。
    - 下键长按：录制上一条任务 post 语音。
    - 双键长按：请求关机（通过 systemd path/service 提权执行）。
  - 录制进程编排（见 2.2），退出时统一校验并执行 `sync`。

### 2.2 录制进程编排
| 模块 | 所在目录 | 启动方式 | 主要职责 | 主要输出 |
| --- | --- | --- | --- | --- |
| 三路相机录制 | `camera_record/triple_camera_record.py` | `uv run ... --codec <h264|h265> --output-dir <episode>` | 使用 GStreamer 原生管线录制主摄 + 左右触觉视频；主摄走 `ffmpeg v4l2(NV12 1920x1080)->h26x_rkmpp`，触觉走 `v4l2src(MJPEG)->mppjpegdec->mpph26xenc`。编码器由 `run_record.sh` 从 `/etc/environment` 的 `CAMERA_CODEC` 读取（默认 H264）。录制不再输出 CSV，帧系统时间通过 `info.json` 中各相机的 `*_record_time_offset_us` 与视频帧 `PTS` 恢复。 | `cam.mkv`, `tact_left.mkv`, `tact_right.mkv`, `info.json` |
| FaysSense 常驻录制 | `faysSense_vi_kit/scripts/run_fays_record.sh` + `fays_record_example` | 检测到 FTDI 时启动 `daemon`，录制时 `start <episode>`，停止时 `stop` | 启动时支持延时拉起 daemon（默认 3 秒）以规避开机上电早期枚举抖动；运行中持续刷新 Fays 在位 + USB 速率缓存。若速率跌落到 480Mb/s（USB2）及以下，health check 置 `ERROR_5`。视频编码器由 `fays_record_example` 直接读取 `/etc/environment` 的 `CAMERA_CODEC`（`mpph264enc`/`mpph265enc`），并通过 `gst-launch-1.0` 的 `fdsrc -> videoparse -> videoconvert -> mpph26xenc -> h26xparse -> matroskamux` 管线写盘。时间戳统一写入 `fays_data.mcap`：topic `i` 为 IMU，topic `c` 为相机时间戳；`logTime` 使用 Fays 时钟，`publishTime` 由 IMU 对齐到系统时钟。 | `fays_stereo_output.mkv`, `fays_data.mcap`（启用 Fays 时） |
| 统一传感器录制 | `src/sensor_recorder` | `./build/src/sensor_recorder/sensor_recorder <episode>` | 同时采集串口 IMU + 编码器，写统一 MCAP。 | `sensor_data.mcap` (`imu_raw`/`encoder`) |
| Encoder 校准 | `src/sensor_recorder` | `./build/src/sensor_recorder/zeroing` | 执行编码器归零，供自动校准流程调用。 | 无（设备状态变更） |

编排要点：
- `run_record.sh` 启动后先检测 FTDI：存在则延时 `FAYS_STARTUP_DELAY_SEC`（默认 3 秒）后拉起 Fays daemon；缺失则持续 `ERROR_2` 报错并等待恢复（服务不退出）。检测依据为 udev 固定映射的 `/dev/fays_stereo` 与 `/dev/fays_imu`。
- 后台监控每秒刷新 `/dev/shm/umi_fays_present` 与 `/dev/shm/umi_fays_usb_speed_mbps`，不做全量 USB 枚举；速率跌落到 `FAYS_USB_ERROR5_MBPS`（默认 480Mb/s）及以下时上报 `ERROR_5`。
- Master 侧 `start_recording()` 会先向 Slave 发送 `START|<episode_dir>|<master_sn>`，Slave 在本次 episode 内记录该 `master_sn`。
- `stop_recording()` 仅在本次 Fays 会话 active 时发送 `STOP`，停止 `PID_CAM` 与 `PID_SENSOR`，Fays 进程保持常驻。
- `cleanup()` 才发送 `EXIT` 关闭 Fays 常驻进程。
- 录制后校验固定覆盖 `cam/tact` 视频、`sensor_data.mcap`、`fays_stereo_output.mkv` 与 `fays_data.mcap`。时长一致性阈值统一为 5 秒：`cam` vs `fays`（仅判定 Fays 不可短于 cam 超阈值）与 `tact_left/right` vs `sensor_data.mcap`（绝对误差）；`fays_data.mcap` 额外检查末尾几帧相机数据是否仍有 IMU 覆盖。

### 2.3 Master/Slave 协同控制
- 通信：TCP `12345` 端口。
- 指令：`START|episode_xxxx|master_sn` / `STOP|0`。
- 机制：Master 侧发送指令，Slave 侧 `nc -l -p 12345` 监听后执行本地 `start_recording/stop_recording`，并在停录后将 `paired_master_sn` 写入当前 episode 的 `info.json`。

### 2.4 反馈与状态通道
- `led_manager.py`：监听 `/tmp/umi_led_pipe`，呈现 `INIT/READY/RECORDING/ERROR/EXIT` 与 `CALIB_PRE/CALIB_RUN/CALIB_DONE`。
- `audio/audio_play.py`：监听 `/tmp/umi_audio_pipe`，播放提示音。
- `run_record.sh` 与校准流程均通过 FIFO 向 LED/AUDIO 发状态，形成跨进程低耦合通信。

## 3. 支撑服务与运维流程

### 3.1 自动校准
- `auto_update/99-usb-auto-update.rules` 触发 `usb-auto-update@.service`，由 `auto_update/usb_auto_update.sh` 检查 U 盘根目录是否存在 `calibration.txt`。
- 同一入口新增“标定导入”分支：若 U 盘存在 `ugripper_calib/<DEVICE_SN>/`，执行 `auto_calibration/import_camera_calibration.sh`，将主摄/Fays/IMU 参数写入 `/etc/ugripper/config/calibration/calibration.json`（支持重复导入覆盖更新），并可与 `config.txt` 配置导入在同一次流程中兼容执行。
- 存在 `calibration.txt` 时触发 `ugripper-calibration.service`（`run_calibration.sh`）：
  - 停止主服务。
  - 启动 LED/音频提示。
  - 执行 `build/src/sensor_recorder/zeroing`。
- `auto_calibration/monitor_network.sh` 仍常驻监听 `end0` 网线插拔，但仅负责 `ugripper.service` 重启，不再触发校准。
- 升级期间若检测到 `/run/ugripper_installing_from_usb.lock`，network monitor 跳过插拔动作，避免升级中的伪上升沿与二次触发。

### 3.2 时间同步
- `ugripper-ptp-monitor.service`：运行 `time_sync/ptp_monitor.sh`，持续写 `/dev/shm/umi_ptp_status`。
- `ugripper-ntp-sync.service`：运行 `time_sync/safe_ntp_sync.sh`，仅在无录制锁（`/tmp/umi_recording.lock`）时短暂开 NTP。
- `postinst` 按 `DEVICE_SIDE` 生成并启用 `ptp4l.service`、`phc2sys.service`，并对 PTP 启动执行重试检查。
- `postinst/prerm` 均采用“短超时 stop + kill 兜底”清理旧录制/音频/灯光进程，避免升级后残留占用。

### 3.3 自动更新与自愈
- `auto_update/99-usb-auto-update.rules` + `usb-auto-update@.service`：U 盘插入自动触发统一入口脚本（先判定 `calibration.txt` 是否触发校准，再进入升级逻辑）。
- 升级窗口内 `usb_auto_update.sh` 会创建 `/run/ugripper_installing_from_usb.lock`，暂停 network monitor，并在结束后恢复。
- `usb_auto_update.sh` 挂载后会读取升级盘根目录 `config.txt`：`LANGUAGE/VOICE_LANG` 写入 `UGRIPPER_LANG`（`zh|en`）；`CAMERA_CODEC/VIDEO_CODEC/TRIPLE_CAMERA_CODEC/CODEC` 写入 `CAMERA_CODEC`（`h264|h265`）；`DEVICE_ROLE/ROLE` 写入 `DEVICE_ROLE`（`master|slave`）。
- 统一导入时序（配置 + 标定）：先停止 `ugripper.service`，启动独立 LED helper，进入 `CALIB_RUN` 黄灯快闪并至少持续 2 秒；全部导入成功时亮一次 `CALIB_DONE` 绿灯完成态 1 秒，随后仅重启一次 `ugripper.service`。
- 若导入阶段任一项失败，切换 `ERROR_1` 红灯错误态提示后再重启 `ugripper.service`，避免服务停滞。
- `auto_update/boot_check_install.sh` + `ugripper-boot-install.service`：开机先比较 `/opt/backup` 中 `ugripper-usb-updater` 版本并在更高时先升级 updater，再检查 `ugripper` 是否缺失/异常并执行自恢复。

### 3.4 关机权限隔离
- 业务脚本只写 `/tmp/umi_shutdown_request`。
- `umi-shutdown-trigger.path` 监听请求文件。
- `umi-shutdown-trigger.service` 以 root 执行 `systemctl poweroff`，避免业务进程持有直接关机权限。

## 4. 数据与配置流

### 4.1 目录结构
`/mnt/data_disk/<device_sn_lower>/`
- `metadata/metadata.json`
- `data/episode_YYYYMMDD_NNNN/`
  - `calibration.json`（当前 episode 实际使用的 Lerobot 标定快照）
  - `cam.mkv`, `tact_left.mkv`, `tact_right.mkv`
  - `info.json`（相机进程写入基础时间字段；Slave 停录后追加 `paired_master_sn`）
  - `fays_stereo_output.mkv`, `fays_data.mcap`（仅在该 episode 启用 Fays 时）
  - `sensor_data.mcap`
  - `audio_pre.wav`, `audio_post.wav`（可选）
  - `validation_error.log`（校验失败时）

`/etc/ugripper/config/calibration/`
- `calibration.json`（持久化主摄 + Fays 双目 + IMU 标定）
- `imported/<DEVICE_SN>/<timestamp>/`（导入原始文件归档）

### 4.2 配置注入
- 主标定持久化：`/etc/ugripper/config/calibration/calibration.json`。`postinst` 仅首次缺失时用 `config/fakeCamCalib.json` 初始化，不覆盖已有文件。
- 主相机/触觉配置：`run_record.sh` 在每次开录前读取持久化 `calibration.json`，动态替换主相机与触觉序列号占位符，再写入当前 episode 的 `calibration.json`。
- FaysSense 配置：`build/faysSense_vi_kit/config/fays_vikit.yaml` 使用固定 `/dev/fays_stereo` 与 `/dev/fays_imu`，`run_fays_record.sh` 启动前只做一致性校验，不动态改写配置文件。

### 4.3 状态文件与锁
- `/tmp/umi_recording.lock`：录制锁，保护 NTP 与其他高风险操作。
- `/dev/shm/umi_ptp_status`：PTP 监控 JSON。
- `/tmp/umi_led_pipe`, `/tmp/umi_audio_pipe`：状态通知 FIFO。
- `/tmp/umi_fays_cmd`：FaysSense 常驻进程命令 FIFO（`START|...`/`STOP`/`EXIT`）。
- `/dev/shm/umi_fays_present`：Fays 在位缓存（`1/0`），由后台监控循环定期刷新。
- `/dev/shm/umi_fays_usb_speed_mbps`：Fays USB 速率缓存（Mb/s），由后台监控循环定期刷新。
- `/run/ugripper_installing_from_usb.lock`：升级保护锁。

## 5. 构建与打包链路（`build_deb.sh`）

### 5.1 C++ 统一构建
根 `CMakeLists.txt` 统一纳入：
- `src/third_party/mcap_builder`
- `faysSense_vi_kit`
- `src/sensor_recorder`

标准流程：
1. `mkdir -p build && cd build`
2. `cmake .. -DCMAKE_BUILD_TYPE=Release`
3. `make -j$(nproc)`

### 5.2 deb 组包关键点
- Quick 模式检查以下二进制是否已存在：
  - `build/src/sensor_recorder/sensor_recorder`
  - `build/src/sensor_recorder/zeroing`
  - `build/faysSense_vi_kit/fays_record_example`
- `rsync` 主体时排除源码目录 `faysSense_vi_kit` 与 `src/sensor_recorder`，随后手动回填产物：
  - `build/faysSense_vi_kit/fays_record_example`
  - `build/faysSense_vi_kit/scripts/*.sh`
  - `build/faysSense_vi_kit/config/*.yaml`
  - `build/src/sensor_recorder/sensor_recorder`
  - `build/src/sensor_recorder/zeroing`
- 打包产物：`ugripper_<VERSION>_arm64.deb`。

### 5.3 安装后行为（postinst）
- 安装窗口先快速停止 `ugripper.service` 并清理残留录制/音频/灯光进程（短超时 + kill 兜底）。
- 配置 `end0` 静态 IP（Right=`192.168.1.100`, Left=`192.168.1.101`）。
- 重建并启动 PTP 栈（`ptp4l`/`phc2sys`）并执行重试检查。
- 重新加载 udev 规则（不触发 `block add`），重启 `ugripper.service`，最后启动 network monitor 与关机触发 path/service。

## 6. 依赖与硬件约束
- 系统依赖：`linuxptp`, `netcat-openbsd`, `jq`, `sox`, `alsa-utils`, `libserialport`, `uv`。
- Python 依赖：`pyproject.toml`（`av`, `mcap`, `pygame`, `pyserial` 等）。
- FaysSense 额外依赖：
  - `faysSense_vi_kit/lib/fays_atrak/aarch64/Release/libfays_vikit.so`
  - `/usr/local/lib/libft602.so`
  - `/usr/local/opencv-4.2.0-linux-aarch64/lib`
- 硬件映射：
  - `/dev/cam_main`, `/dev/left_tcam`, `/dev/right_tcam`
  - FaysSense FTDI 设备固定映射为 `/dev/fays_stereo` 与 `/dev/fays_imu`（底层仍映射到动态 `/dev/video*`）
  - `/dev/ttyS2`（IMU）, `/dev/ttyS7`（Encoder）
  - 音频卡 `rockchipes8388`
  - 按键 `PIN_36` / `PIN_38`

## 7. 迁移与扩展注意事项
1. 若新增传感器，优先沿用 `run_record.sh` 的并发启动/统一停止/统一校验模式。
2. 历史目录（`dm_imu_alone`、`encoder_refactor`、`im648_imu_alone`）已清理，相关能力已并入 `src/sensor_recorder`。
3. FaysSense 相关依赖（OpenCV/FTDI/libfays）需在目标机预装并保持版本兼容，否则会在运行时动态链接失败。

## 8. 快速排障指引
- 服务：`systemctl status ugripper.service`，`journalctl -u ugripper.service -f`
- 录制状态：检查 `/tmp/umi_recording.lock`
- PTP：检查 `/dev/shm/umi_ptp_status`
- FaysSense：
  - `build/faysSense_vi_kit/fays_record_example` 是否存在
  - `build/faysSense_vi_kit/scripts/run_fays_record.sh` 是否可执行
  - `/usr/local/lib/libft602.so` 与 OpenCV 库目录是否存在
- 数据校验失败：查看 `episode_xxxx/validation_error.log`

> 当前架构已经从“IMU/Encoder 分离录制”迁移为“统一传感器 MCAP + FaysSense 双目并发 + 三路触觉并发”的组合模式。后续扩展建议在此编排框架内演进，避免重新分叉录制状态机。
