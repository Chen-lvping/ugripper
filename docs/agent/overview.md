# Ugripper 项目总览（LLM 检索版）

> 目的：作为项目唯一总览入口，反映当前代码实际状态，便于快速检索与维护交接。

## 1. 项目目标与部署边界
- 目标：在 Radxa 平台实现多模态同步录制（主/触觉相机 + IMU + 编码器）并提供现场交互反馈（LED/音频）。
- 部署路径：应用安装到 `/opt/ugripper`，由 systemd 常驻运行（`pack_script/ugripper.service`）。
- 数据路径：录制数据写入 `/mnt/data_disk/<device_sn>/`（`run_record.sh` 读取 `/etc/environment` 的 `DEVICE_SN` 和 `DEVICE_SIDE`）。
- 双臂角色：`DEVICE_SIDE=Right|Left`。
  - Right：主控，处理按键与音频录制，并通过 TCP 向 Left 同步 START/STOP。
  - Left：从控，仅接受网络命令执行录制。

## 2. 当前仓库与运行入口（按优先级）
| 模块 | 关键文件 | 作用 |
| --- | --- | --- |
| 主编排脚本 | `run_record.sh` | 录制主状态机：硬件检查、目录初始化、按键逻辑、启动/停止传感器与相机、校验、落盘 sync。 |
| 主服务 | `pack_script/ugripper.service` | systemd 启动入口，工作目录 `/opt/ugripper`，执行 `run_record.sh`。 |
| 统一传感器录制（C++） | `src/sensor_recorder/src/main.cpp` | 同时采集 IMU + Encoder 并写入同一个 `sensor_data.mcap`（topic: `imu_raw` / `encoder`）。 |
| 相机录制（Python） | `camera_record/triple_camera_record_h265.py` | 录制 `cam.mkv`, `tact_left.mkv`, `tact_right.mkv` 及时间戳 CSV。 |
| 反馈通道 | `led_manager.py`, `audio/audio_play.py` | 通过 FIFO (`/tmp/umi_led_pipe`, `/tmp/umi_audio_pipe`) 播放灯光/音频状态。 |
| 打包脚本 | `build_deb.sh` | 根目录 CMake 构建 + deb 组包 + systemd/udev 安装资源注入。 |

## 3. 运行时架构

### 3.1 主流程（`run_record.sh`）
1. 初始化环境与目录：
   - 读取 `DEVICE_SN`/`DEVICE_SIDE`。
   - 初始化数据目录：`metadata/`, `calibration/`, `data/`。
   - 将 `config/fake*Calib.json` 拷贝为运行校准文件。
2. 启动反馈进程：
   - `uv run led_manager.py`
   - `uv run audio/audio_play.py`
   - 通过 FIFO 发 `INIT/READY/RECORDING/ERROR/EXIT` 等状态。
3. 录制控制：
   - 启动相机：`uv run camera_record/triple_camera_record_h265.py --output-dir <episode>`。
   - 启动传感器：`./build/src/sensor_recorder/sensor_recorder <episode_dir>`。
4. 停止录制后：
   - 发送 `SIGINT` 给子进程，等待退出。
   - 对数据盘执行 `sync` + `blockdev --flushbufs`。
   - 校验关键产物（尤其 `sensor_data.mcap` 非空）。

### 3.2 Right/Left 协同
- TCP 端口：`12345`。
- 命令格式：`START|episode_xxxx` / `STOP|0`。
- Right 发送，Left 使用 `nc -l -p 12345` 监听并调用本地 `start_recording/stop_recording`。

### 3.3 按键与音频交互（Right）
- 上键短按：开始/停止数据录制。
- 上键长按：录制 pre 标注语音（保存为下一条 episode 的 `audio_pre.wav`）。
- 下键长按：录制 post 标注语音（保存为上一条 episode 的 `audio_post.wav`）。
- 双键长按 4 秒：触发关机请求；按住 3 秒先播放关机提示音。

### 3.4 关机权限隔离机制（新增）
- 业务脚本不直接执行 `poweroff`，而是写触发文件 `/tmp/umi_shutdown_request`。
- `umi-shutdown-trigger.path` 监听触发文件。
- `umi-shutdown-trigger.service` 执行 `auto_update/trigger_shutdown.sh`，由 systemd 以 root 调用 `systemctl poweroff`。

## 4. 传感器录制（`sensor_recorder`）
- 统一二进制：`build/src/sensor_recorder/sensor_recorder`。
- 串口：
  - IMU: `/dev/ttyS2`
  - Encoder: `/dev/ttyS7`
- 采样与写入：
  - Encoder 请求周期目标 1kHz；IMU 按驱动更新写入。
  - MCAP 单文件输出：`sensor_data.mcap`。
  - Topic：`imu_raw`（40B float32[10]）与 `encoder`（int32 + float32）。
- MCAP 写入策略：
  - chunk 开启（`noChunking=false`）
  - LZ4 压缩
  - `chunkSize=256KiB`
  - 关闭 message index，关闭重复 schema/channel 记录以减小体积

## 5. 数据与配置流

### 5.1 目录结构
根目录：`/mnt/data_disk/<device_sn_lower>/`
- `metadata/metadata.json`
- `calibration/cam.json`, `encoder.json`, `imu.json`
- `data/episode_YYYYMMDD_NNNN/`
  - `cam.mkv`, `tact_left.mkv`, `tact_right.mkv`
  - `cam.csv`, `tact_left.csv`, `tact_right.csv`
  - `sensor_data.mcap`
  - `audio_pre.wav` / `audio_post.wav`（可选）
  - `validation_error.log`（失败时）

### 5.2 配置注入
- `run_record.sh` 会处理 `cam.json` 占位符：
  - `{{CAM_MAIN}}`
  - `{{TACTILE_LEFT_SERIAL}}`, `{{TACTILE_RIGHT_SERIAL}}`
- 触觉相机序列号通过 `udevadm` 从设备树读取并写入 JSON。

### 5.3 状态文件
- `/tmp/umi_recording.lock`：录制期间存在，用于阻止 NTP 同步等高风险动作。
- `/dev/shm/umi_ptp_status`：PTP 监控输出 JSON（state/offset/path_delay/sys_offset）。
- `/tmp/umi_led_pipe`, `/tmp/umi_audio_pipe`：跨进程状态通知 FIFO。

## 6. 构建、打包与安装

### 6.1 C++ 构建（当前已统一到根 CMake）
- 根 `CMakeLists.txt` 仅包含：
  - `src/third_party/mcap_builder`
  - `src/sensor_recorder`
- 构建方式：
  - `mkdir -p build && cd build`
  - `cmake .. -DCMAKE_BUILD_TYPE=Release`
  - `make -j$(nproc)`

### 6.2 deb 打包（`build_deb.sh`）
- 版本号当前由脚本 `VERSION` 维护（示例：`1.0.13`）。
- 标准模式：清理临时目录 + 重编 C++ + 全量组包。
- Quick 模式（`-q`）：跳过 C++ 编译，尽量复用临时目录中的 `.venv`。
- 最终产物：`ugripper_<VERSION>_arm64.deb`。

### 6.3 安装/卸载脚本关键动作
- `pack_script/postinst`：
  - 配置 `end0` 静态 IP（Right=`192.168.1.100`, Left=`192.168.1.101`）
  - 生成并启用 `ptp4l.service`, `phc2sys.service`
  - 启用 `ugripper-ptp-monitor.service`, `ugripper-ntp-sync.service`, `ugripper-network-monitor.service`
  - 启用 `umi-shutdown-trigger.path/.service`
  - reload udev 规则并重启主服务
- `pack_script/prerm`：禁用以上服务并清理动态生成配置。

## 7. 支撑服务（运维链路）
| 领域 | 关键文件 | 触发方式 | 说明 |
| --- | --- | --- | --- |
| 自动校准 | `auto_calibration/monitor_network.sh`, `run_calibration.sh` | 网线插入触发 `ugripper-calibration.service` | 停主服务后执行校准流程（目前仍调用历史校准二进制路径）。 |
| PTP 监控 | `time_sync/ptp_monitor.sh` | `ugripper-ptp-monitor.service` 常驻 | 持续写 `/dev/shm/umi_ptp_status`。 |
| 安全 NTP | `time_sync/safe_ntp_sync.sh` | `ugripper-ntp-sync.service` | 仅在无录制锁时短暂开 NTP 同步后关闭。 |
| U 盘升级 | `auto_update/usb_auto_update.sh` + udev rule | 插盘触发 `usb-auto-update@.service` | 可先更新 updater 自身，再更新主包。 |
| 开机自愈 | `auto_update/boot_check_install.sh` | `ugripper-boot-install.service` | 主包缺失时从 `/opt/backup` 自动恢复。 |

## 8. 依赖与硬件约束
- 系统依赖：`linuxptp`, `netcat-openbsd`, `jq`, `sox`, `alsa-utils`, `libserialport`, `uv` 等。
- Python 依赖见 `pyproject.toml`（`av`, `mcap`, `pygame`, `pyserial` 等）。
- 关键硬件映射：
  - `/dev/cam_main`, `/dev/left_tcam`, `/dev/right_tcam`
  - `/dev/ttyS2`(IMU), `/dev/ttyS7`(Encoder)
  - 音频卡：`rockchipes8388`
  - 按键：`PIN_36`（上），`PIN_38`（下）

## 9. 已知现状与迁移提示
- 运行时录制已切换到 `sensor_recorder`，不再由 `im648_imu_alone` + `encoder_refactor` 分别产出 mcap。
- 但仓库内仍保留历史目录（`dm_imu_alone`, `encoder_refactor`, `im648_imu_alone`），并且部分运维流程（如校准、udev 规则）仍引用其中内容。
- 若后续要彻底删除历史目录，需要同步迁移：
  - 校准二进制路径（`auto_calibration/run_calibration.sh`）
  - 串口规则来源（`build_deb.sh` 复制的 `encoder_refactor/99-serial.rules`）

## 10. 快速排障清单
- 服务：`systemctl status ugripper.service`，`journalctl -u ugripper.service -f`
- 录制状态：检查 `/tmp/umi_recording.lock` 是否异常残留
- 时间同步：检查 `/dev/shm/umi_ptp_status`
- 音频/灯光：检查 FIFO 是否存在（`/tmp/umi_audio_pipe`, `/tmp/umi_led_pipe`）
- 更新日志：`/var/log/ugripper/usb_auto_update.log`, `/var/log/ugripper/boot_install.log`
- 数据异常：查看 episode 下 `validation_error.log`，并用 `py_script/mcap_viewer.py` 统计 topic 频率与数据量

---

## 检索建议（给后续 Agent）
- 找“录制主逻辑”：优先看 `run_record.sh`。
- 找“传感器频率/MCAP格式”：看 `src/sensor_recorder/src/main.cpp` 与 `py_script/mcap_viewer.py`。
- 找“安装后系统行为”：看 `pack_script/postinst`。
- 找“关机权限路径”：看 `auto_update/umi-shutdown-trigger.path` + `auto_update/trigger_shutdown.sh`。
- 找“打包内容变化”：看 `build_deb.sh` 的 `EXCLUDE_LIST` 与 systemd/udev 复制段。
