# ugripper 项目架构（可复用模板）

本文档总结 `ugripper` 数采夹爪系统在 `/opt/ugripper` 部署后的整体架构，可作为后续“多传感器并发录制 + 自动部署”项目的蓝本。内容按运行时组件、支撑服务、数据流、打包链路和可扩展性组织。

## 1. 系统概览
- **目标**：在 Radxa 嵌入式平台上可靠同步录制视觉、触觉、FaysSense 双目、IMU、编码器等多模态数据，并提供现场 LED/音频反馈。
- **运行角色**：双臂协同（Right=主控, Left=从控）。右臂负责按键交互、音频提示，并驱动左臂同步录制。
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
| 三路相机录制 | `camera_record/triple_camera_record_h265.py` | `uv run ... --output-dir <episode>` | 录制主摄 + 左右触觉视频（H265）并写时间戳 CSV。 | `cam.mkv`, `tact_left.mkv`, `tact_right.mkv`, 对应 `*.csv` |
| FaysSense 常驻录制 | `faysSense_vi_kit/scripts/run_fays_record.sh` + `fays_record_example` | 开机 `daemon`，录制时 `start <episode>`，停止时 `stop` | 常驻占用相机与 SDK 句柄，命令触发写文件，避免每次录制冷启动。 | `fays_stereo_output.mkv`, `fays_stereo_timestamp.csv`, `fays_imu_data.csv` |
| 统一传感器录制 | `src/sensor_recorder` | `./build/src/sensor_recorder/sensor_recorder <episode>` | 同时采集串口 IMU + 编码器，写统一 MCAP。 | `sensor_data.mcap` (`imu_raw`/`encoder`) |

编排要点：
- `run_record.sh` 启动后先拉起 FaysSense 常驻进程（`daemon`），记录 `PID_FAYS_DAEMON`。
- `start_recording()` 仅向 FaysSense 发送 `START|<episode_dir>` 命令，同时拉起 `PID_CAM` 与 `PID_SENSOR`。
- `stop_recording()` 向 FaysSense 发送 `STOP` 命令，停止 `PID_CAM` 与 `PID_SENSOR`，Fays 进程保持常驻。
- `cleanup()` 才发送 `EXIT` 关闭 Fays 常驻进程。
- 录制后校验至少覆盖 `cam/tact` 视频、`fays_stereo_output.mkv`、`sensor_data.mcap`。

### 2.3 Right/Left 协同控制
- 通信：TCP `12345` 端口。
- 指令：`START|episode_xxxx` / `STOP|0`。
- 机制：Right 侧发送指令，Left 侧 `nc -l -p 12345` 监听后执行本地 `start_recording/stop_recording`。

### 2.4 反馈与状态通道
- `led_manager.py`：监听 `/tmp/umi_led_pipe`，呈现 `INIT/READY/RECORDING/ERROR/EXIT`。
- `audio/audio_play.py`：监听 `/tmp/umi_audio_pipe`，播放提示音。
- `run_record.sh` 与校准流程均通过 FIFO 向 LED/AUDIO 发状态，形成跨进程低耦合通信。

## 3. 支撑服务与运维流程

### 3.1 自动校准
- `auto_calibration/monitor_network.sh` 由 `ugripper-network-monitor.service` 常驻，监听 `end0` 网线插拔。
- 网线插入后触发 `ugripper-calibration.service`（`run_calibration.sh`）：
  - 停止主服务。
  - 启动 LED/音频提示。
  - 执行校准流程。
- 现状：校准链路仍依赖历史目录内二进制（`dm_imu_alone`、`encoder_refactor`）。

### 3.2 时间同步
- `ugripper-ptp-monitor.service`：运行 `time_sync/ptp_monitor.sh`，持续写 `/dev/shm/umi_ptp_status`。
- `ugripper-ntp-sync.service`：运行 `time_sync/safe_ntp_sync.sh`，仅在无录制锁（`/tmp/umi_recording.lock`）时短暂开 NTP。
- `postinst` 按 `DEVICE_SIDE` 生成并启用 `ptp4l.service`、`phc2sys.service`。

### 3.3 自动更新与自愈
- `auto_update/99-usb-auto-update.rules` + `usb-auto-update@.service`：U 盘插入自动触发升级脚本。
- `auto_update/boot_check_install.sh` + `ugripper-boot-install.service`：开机检测主包缺失时从 `/opt/backup` 自恢复。

### 3.4 关机权限隔离
- 业务脚本只写 `/tmp/umi_shutdown_request`。
- `umi-shutdown-trigger.path` 监听请求文件。
- `umi-shutdown-trigger.service` 以 root 执行 `systemctl poweroff`，避免业务进程持有直接关机权限。

## 4. 数据与配置流

### 4.1 目录结构
`/mnt/data_disk/<device_sn_lower>/`
- `metadata/metadata.json`
- `calibration/cam.json`, `encoder.json`, `imu.json`
- `data/episode_YYYYMMDD_NNNN/`
  - `cam.mkv`, `tact_left.mkv`, `tact_right.mkv`
  - `cam.csv`, `tact_left.csv`, `tact_right.csv`
  - `fays_stereo_output.mkv`, `fays_stereo_timestamp.csv`
  - `fays_imu_data.csv`（`fays_imu_data.mcap` 默认关闭）
  - `sensor_data.mcap`
  - `audio_pre.wav`, `audio_post.wav`（可选）
  - `validation_error.log`（校验失败时）

### 4.2 配置注入
- 主相机/触觉配置：`run_record.sh` 动态替换 `cam.json` 中主相机与触觉序列号占位符。
- FaysSense 配置：`run_fays_record.sh daemon` 阶段根据设备探测结果修改 `build/faysSense_vi_kit/config/fays_vikit.yaml`，后续录制阶段仅发送命令。

### 4.3 状态文件与锁
- `/tmp/umi_recording.lock`：录制锁，保护 NTP 与其他高风险操作。
- `/dev/shm/umi_ptp_status`：PTP 监控 JSON。
- `/tmp/umi_led_pipe`, `/tmp/umi_audio_pipe`：状态通知 FIFO。
- `/tmp/umi_fays_cmd`：FaysSense 常驻进程命令 FIFO（`START|...`/`STOP`/`EXIT`）。

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
  - `build/faysSense_vi_kit/fays_record_example`
- `rsync` 主体时排除源码目录 `faysSense_vi_kit` 与 `src/sensor_recorder`，随后手动回填产物：
  - `build/faysSense_vi_kit/fays_record_example`
  - `build/faysSense_vi_kit/scripts/*.sh`
  - `build/faysSense_vi_kit/config/*.yaml`
  - `build/src/sensor_recorder/sensor_recorder`
- 打包产物：`ugripper_<VERSION>_arm64.deb`。

### 5.3 安装后行为（postinst）
- 配置 `end0` 静态 IP（Right=`192.168.1.100`, Left=`192.168.1.101`）。
- 启用 PTP/NTP 监控相关服务。
- 启用网络监控、关机触发 path/service。
- 重新加载 udev 规则并重启 `ugripper.service`。

## 6. 依赖与硬件约束
- 系统依赖：`linuxptp`, `netcat-openbsd`, `jq`, `sox`, `alsa-utils`, `libserialport`, `uv`。
- Python 依赖：`pyproject.toml`（`av`, `mcap`, `pygame`, `pyserial` 等）。
- FaysSense 额外依赖：
  - `faysSense_vi_kit/lib/fays_atrak/aarch64/Release/libfays_vikit.so`
  - `/usr/local/lib/libft602.so`
  - `/usr/local/opencv-4.2.0-linux-aarch64/lib`
- 硬件映射：
  - `/dev/cam_main`, `/dev/left_tcam`, `/dev/right_tcam`
  - FaysSense FTDI 设备对应 `/dev/video*`（运行时自动分配）
  - `/dev/ttyS2`（IMU）, `/dev/ttyS7`（Encoder）
  - 音频卡 `rockchipes8388`
  - 按键 `PIN_36` / `PIN_38`

## 7. 迁移与扩展注意事项
1. 若新增传感器，优先沿用 `run_record.sh` 的并发启动/统一停止/统一校验模式。
2. 若清理历史目录（`dm_imu_alone`、`encoder_refactor`、`im648_imu_alone`），需先迁移：
   - `run_calibration.sh` 中的旧校准二进制路径。
   - `build_deb.sh` 中 `encoder_refactor/99-serial.rules` 的来源。
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
