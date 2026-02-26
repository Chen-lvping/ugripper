# Ugripper 项目总览（LLM 检索版）

> 目的：作为项目唯一总览入口，反映当前代码实际状态，便于快速检索与维护交接。

## 1. 项目目标与部署边界
- 目标：在 Radxa 平台实现多模态同步录制（主/触觉相机 + FaysSense 双目 + IMU + 编码器）并提供现场交互反馈（LED/音频）。
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
| 统一传感器录制（C++） | `src/sensor_recorder/src/main.cpp`, `src/sensor_recorder/src/zeroing.cpp` | 统一管理 IMU+Encoder 录制与 Encoder 归零工具。 |
| 三路相机录制（Python） | `camera_record/triple_camera_record_h265.py` | 录制 `cam.mkv`, `tact_left.mkv`, `tact_right.mkv` 及时间戳 CSV。 |
| FaysSense 常驻录制（C++ + Shell） | `faysSense_vi_kit/scripts/run_fays_record.sh`, `faysSense_vi_kit/example/record.cpp` | 开机后常驻占用相机并预热；录制时通过命令触发写文件，停止时仅停写不退出进程。 |
| 反馈通道 | `led_manager.py`, `audio/audio_play.py` | 通过 FIFO (`/tmp/umi_led_pipe`, `/tmp/umi_audio_pipe`) 播放灯光/音频状态。 |
| 打包脚本 | `build_deb.sh` | 根目录 CMake 构建 + deb 组包 + systemd/udev 安装资源注入。 |
| 发布收尾自动化（Skill） | `.codex/skills/auto-release-deb/scripts/auto_release_deb.sh` | 代码修改后自动升级 `build_deb.sh` 版本（默认 +.z，显式指定才 +.x/+ .y），并自动选择是否使用 `-q` 打包。 |

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
   - 开机初始化阶段检测 Fays FTDI 设备：
     - 若存在：启动 FaysSense 常驻进程 `./build/faysSense_vi_kit/scripts/run_fays_record.sh daemon`。
     - 若不存在：进入无 Fays 模式（视为正常，不阻断主流程）。
   - 真正开始某条 episode 时：
     - 若 Fays 已启用：发送 `./build/faysSense_vi_kit/scripts/run_fays_record.sh start <episode_dir>`。
     - 若未启用：跳过 Fays 录制，仅保留三路相机+传感器录制。
   - 停止 episode 时在 Fays 已启用且会话 active 的情况下发送 `./build/faysSense_vi_kit/scripts/run_fays_record.sh stop`（进程不退出，继续预热）。
   - 运行中支持 Fays 热插拔自动重连：检测到 FTDI 重新插入后会自动重启 daemon；若正在录制则自动补发 START 续写当前 episode。
   - 并行启动三路相机与统一传感器：
     - `uv run camera_record/triple_camera_record_h265.py --output-dir <episode>`
     - `./build/src/sensor_recorder/sensor_recorder <episode_dir>`
4. 停止录制后：
   - 对 `PID_CAM/PID_SENSOR` 发送 `SIGINT`，并向 FaysSense 发送 `STOP` 命令。
   - 对数据盘执行 `sync` + `blockdev --flushbufs`。
   - 校验关键产物（固定包含 `cam*.mkv`、`tact_*.mkv`、`sensor_data.mcap`；若该 episode 期望 Fays 数据，则同时校验 `fays_stereo_output.mkv`、`fays_data.mcap`）。

### 3.2 Right/Left 协同
- TCP 端口：`12345`。
- 命令格式：`START|episode_xxxx` / `STOP|0`。
- Right 发送，Left 使用 `nc -l -p 12345` 监听并调用本地 `start_recording/stop_recording`。

### 3.3 按键与音频交互（Right）
- 上键短按：开始/停止数据录制。
- 上键长按：录制 pre 标注语音（保存为下一条 episode 的 `audio_pre.wav`）。
- 下键长按：录制 post 标注语音（保存为上一条 episode 的 `audio_post.wav`）。
- pre/post 录音后处理：固定提取 ch1 为单声道，执行 `sox noisered + norm` 输出；若降噪不可用则回退为 ch1 归一化。
- 双键长按 4 秒：触发关机请求；按住 3 秒先播放关机提示音。
- 启动前置硬件检查：Right 侧在 `INIT` 期间持续校验 `PIN_36/PIN_38`（可读且为释放态）；未通过则保持 `ERROR` 并停在磁盘检查前，直到恢复。

### 3.4 关机权限隔离机制（新增）
- 业务脚本不直接执行 `poweroff`，而是写触发文件 `/tmp/umi_shutdown_request`。
- `umi-shutdown-trigger.path` 监听触发文件。
- `umi-shutdown-trigger.service` 执行 `auto_update/trigger_shutdown.sh`，由 systemd 以 root 调用 `systemctl poweroff`。

## 4. 录制链路（核心）

### 4.1 统一传感器录制（`sensor_recorder`）
- 统一二进制：`build/src/sensor_recorder/sensor_recorder`。
- 校准二进制：`build/src/sensor_recorder/zeroing`（供 `run_calibration.sh` 调用）。
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

### 4.2 FaysSense 双目录制（`faysSense_vi_kit`）
- 启动入口：`build/faysSense_vi_kit/scripts/run_fays_record.sh`。
- 当前控制模式：
  - `daemon`：启动常驻进程，自动扫描 `FTDI Superspeed Video Bridge`，并改写 `fays_vikit.yaml` 端口字段。
  - `start <episode_dir>`：通过 FIFO 命令触发当前 episode 开始写文件。
  - `stop`：停止当前 episode 写文件，但保持进程与相机句柄。
  - `exit`：退出常驻进程（通常在 `run_record.sh` 清理阶段调用）。
- 运行策略：
  - 启动时若未检测到 Fays FTDI，系统进入无 Fays 模式并继续正常录制。
  - 运行中若发生 Fays 插拔，`run_record.sh` 会自动检测并尝试重连 daemon；重连成功后可在当前 episode 内恢复 Fays 录制。
- 常驻命令通道：`/tmp/umi_fays_cmd`（`START|<dir>` / `STOP` / `EXIT`）。
- 当前默认输出：
  - `fays_stereo_output.mkv`
  - `fays_data.mcap`（统一记录 Fays IMU + 相机时间戳）
    - topic `i`：IMU（`logTime=Fays IMU 时间`，`publishTime=接收 IMU 时系统时间`）
    - topic `c`：相机时间戳（`logTime=Fays 相机时间`，`publishTime=基于最新 IMU 偏移对齐后的系统时间`）

### 4.3 三路相机录制（`triple_camera_record_h265.py`）
- 输出：`cam.mkv`, `tact_left.mkv`, `tact_right.mkv` 及对应 `*.csv`。
- 与 FaysSense 录制并行运行，由 `run_record.sh` 统一生命周期管理。

## 5. 数据与配置流

### 5.1 目录结构
根目录：`/mnt/data_disk/<device_sn_lower>/`
- `metadata/metadata.json`
- `calibration/cam.json`, `encoder.json`, `imu.json`
- `data/episode_YYYYMMDD_NNNN/`
  - `cam.mkv`, `tact_left.mkv`, `tact_right.mkv`
  - `cam.csv`, `tact_left.csv`, `tact_right.csv`
  - `fays_stereo_output.mkv`, `fays_data.mcap`（仅在该 episode 启用 Fays 时产出）
  - `sensor_data.mcap`
  - `audio_pre.wav` / `audio_post.wav`（可选）
  - `validation_error.log`（失败时）

### 5.2 配置注入
- `run_record.sh` 会处理 `cam.json` 占位符：
  - `{{CAM_MAIN}}`
  - `{{TACTILE_LEFT_SERIAL}}`, `{{TACTILE_RIGHT_SERIAL}}`
- 触觉相机序列号通过 `udevadm` 从设备树读取并写入 JSON。
- FaysSense 在 `run_fays_record.sh daemon` 阶段会动态修正 Fays YAML 端口字段，后续录制阶段只发送控制命令。

### 5.3 状态文件
- `/tmp/umi_recording.lock`：录制期间存在，用于阻止 NTP 同步等高风险动作。
- `/dev/shm/umi_ptp_status`：PTP 监控输出 JSON（state/offset/path_delay/sys_offset）。
- `/tmp/umi_led_pipe`, `/tmp/umi_audio_pipe`：跨进程状态通知 FIFO。
- `/tmp/umi_fays_cmd`：FaysSense 常驻录制命令 FIFO。
- `/run/ugripper_installing_from_usb.lock`：U 盘升级保护锁；存在时 network monitor 跳过插拔重启动作。

## 6. 构建、打包与安装

### 6.1 C++ 构建（当前已统一到根 CMake）
- 根 `CMakeLists.txt` 包含：
  - `src/third_party/mcap_builder`
  - `faysSense_vi_kit`
  - `src/sensor_recorder`
- 构建方式：
  - `mkdir -p build && cd build`
  - `cmake .. -DCMAKE_BUILD_TYPE=Release`
  - `make -j$(nproc)`

### 6.2 deb 打包（`build_deb.sh`）
- 版本号当前由脚本 `VERSION` 维护（示例：`1.0.15`）。
- 标准模式：清理临时目录 + 重编 C++ + 全量组包。
- Quick 模式（`-q`）：跳过 C++ 编译，尽量复用临时目录中的 `.venv`。
- 打包前置检查（Quick 模式）：同时检查
  - `build/src/sensor_recorder/sensor_recorder`
  - `build/src/sensor_recorder/zeroing`
  - `build/faysSense_vi_kit/fays_record_example`
- 最终包内额外回填：
  - `build/faysSense_vi_kit/fays_record_example`
  - `build/faysSense_vi_kit/scripts/*.sh`
  - `build/faysSense_vi_kit/config/*.yaml`
  - `build/src/sensor_recorder/sensor_recorder`
  - `build/src/sensor_recorder/zeroing`
- 最终产物：`ugripper_<VERSION>_arm64.deb`。

### 6.3 安装/卸载脚本关键动作
- `pack_script/postinst`：
  - 安装窗口先快速停止 `ugripper.service` 并清理残留 `run_record/led/audio` 进程（短超时 + kill 兜底）。
  - 配置 `end0` 静态 IP（Right=`192.168.1.100`, Left=`192.168.1.101`）。
  - 生成并启用 `ptp4l.service`, `phc2sys.service`，并对 PTP 启动执行重试检查。
  - 启用 `ugripper-ptp-monitor.service`, `ugripper-ntp-sync.service`。
  - 启用 `umi-shutdown-trigger.path/.service`。
  - reload udev 规则但不再触发 `block add`，最后重启主服务与 `ugripper-network-monitor.service`。
- `pack_script/prerm`：先快速停止相关服务与残留录制/音频进程，再禁用并清理动态生成配置。

### 6.4 开发发布自动化（Skill）
- 入口脚本：`.codex/skills/auto-release-deb/scripts/auto_release_deb.sh`。
- 版本规则：默认仅升级 `build_deb.sh` 的 `.z`（patch）；仅当调用方显式指定 `--bump .y` 或 `--bump .x` 时，才升级 `.y/.x`。
- changelog：由 `add-feature` 流程维护，聚焦软件功能改动，条目保持简洁。
- 构建模式自动选择：
  - 若检测到 C/C++/CMake 改动，执行标准模式 `./build_deb.sh`。
  - 若无上述改动且关键二进制齐全，执行 Quick 模式 `./build_deb.sh -q`。
  - 若关键二进制缺失，自动回退标准模式。

## 7. 支撑服务（运维链路）
| 领域 | 关键文件 | 触发方式 | 说明 |
| --- | --- | --- | --- |
| 自动校准 | `auto_calibration/run_calibration.sh`, `src/sensor_recorder/src/zeroing.cpp` | 插入 `usb_update_stick` 且 U 盘根目录存在 `calibration.txt` 时触发 | 停主服务后执行 Encoder 归零流程。 |
| PTP 监控 | `time_sync/ptp_monitor.sh` | `ugripper-ptp-monitor.service` 常驻 | 持续写 `/dev/shm/umi_ptp_status`。 |
| 安全 NTP | `time_sync/safe_ntp_sync.sh` | `ugripper-ntp-sync.service` | 仅在无录制锁时短暂开 NTP 同步后关闭。 |
| U 盘升级 | `auto_update/usb_auto_update.sh` + udev rule | 插盘触发 `usb-auto-update@.service` | 若存在 `calibration.txt` 则优先触发校准，否则执行升级；升级期间创建 guard 文件并暂停 network monitor，安装后自动恢复。 |
| 网线监控 | `auto_calibration/monitor_network.sh` | `ugripper-network-monitor.service` 常驻 | 网线插拔时处理 `ugripper.service` 重启；检测到升级 guard 时跳过插拔动作，避免升级竞态。 |
| 开机自愈 | `auto_update/boot_check_install.sh` | `ugripper-boot-install.service` | 主包缺失时从 `/opt/backup` 自动恢复。 |

## 8. 依赖与硬件约束
- 系统依赖：`linuxptp`, `netcat-openbsd`, `jq`, `sox`, `alsa-utils`, `libserialport`, `uv` 等。
- Python 依赖见 `pyproject.toml`（`av`, `mcap`, `pygame`, `pyserial` 等）。
- FaysSense 运行依赖：
  - `faysSense_vi_kit/lib/fays_atrak/aarch64/Release/libfays_vikit.so`
  - `/usr/local/lib/libft602.so`
  - `/usr/local/opencv-4.2.0-linux-aarch64/lib`（由脚本加入 `LD_LIBRARY_PATH`）
- 关键硬件映射：
  - `/dev/cam_main`, `/dev/left_tcam`, `/dev/right_tcam`
  - FaysSense FTDI 设备对应 `/dev/video*`（运行时自动探测）
  - Fays FTDI 权限规则由 `camera_record/99-fixed-usb-map.rules` 提供，当前设备按 `VID:PID=0403:602e` 放开 `video4linux`/`usb` 访问权限。
  - `/dev/ttyS2`(IMU), `/dev/ttyS7`(Encoder)
  - 音频卡：`rockchipes8388`
  - 按键：`PIN_36`（上），`PIN_38`（下）

## 9. 已知现状与迁移提示
- 运行时录制已统一为三路并发：`triple_camera_record` + `faysSense_vi_kit` + `sensor_recorder`。
- 历史目录 `dm_imu_alone`、`encoder_refactor`、`im648_imu_alone` 已移除。
- 校准二进制与串口规则均已迁移到 `src/sensor_recorder`（`zeroing` + `99-serial.rules`）。

## 10. 快速排障清单
- 服务：`systemctl status ugripper.service`，`journalctl -u ugripper.service -f`
- 录制状态：检查 `/tmp/umi_recording.lock` 是否异常残留
- 时间同步：检查 `/dev/shm/umi_ptp_status`
- 音频/灯光：检查 FIFO 是否存在（`/tmp/umi_audio_pipe`, `/tmp/umi_led_pipe`）
- FaysSense：
  - 检查 `build/faysSense_vi_kit/fays_record_example` 与 `build/faysSense_vi_kit/scripts/run_fays_record.sh`
  - 检查 `/usr/local/lib/libft602.so` 与 OpenCV 目录是否存在
- 更新日志：`/var/log/ugripper/usb_auto_update.log`, `/var/log/ugripper/boot_install.log`
- 数据异常：查看 episode 下 `validation_error.log`，并用 `py_script/mcap_viewer.py` 统计 topic 频率与数据量（已支持 `fays_data.mcap` 的 `i/c` 二进制解析与 `publishTime-logTime` 差值显示）

---

## 检索建议（给后续 Agent）
- 找“录制主逻辑”：优先看 `run_record.sh`。
- 找“FaysSense 启停与输出”：看 `faysSense_vi_kit/scripts/run_fays_record.sh` 与 `faysSense_vi_kit/example/record.cpp`。
- 找“传感器频率/MCAP格式”：看 `src/sensor_recorder/src/main.cpp` 与 `py_script/mcap_viewer.py`。
- 找“安装后系统行为”：看 `pack_script/postinst`。
- 找“关机权限路径”：看 `auto_update/umi-shutdown-trigger.path` + `auto_update/trigger_shutdown.sh`。
- 找“打包内容变化”：看 `build_deb.sh` 的 `EXCLUDE_LIST` 与二进制回填段。
