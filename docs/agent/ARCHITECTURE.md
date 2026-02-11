# ugripper 项目架构（可复用模板）

本文档总结 `ugripper` 数采夹爪系统在 /opt/ugripper 部署后的整体架构，可作为今后类似“多传感器数据录制 + 自动部署”项目的蓝本。内容按运行时组件、支撑服务、数据流、打包链路和可扩展性组织。

## 1. 系统概览
- **目标**：在 Radxa 等嵌入式平台上，可靠地同步录制视觉、触觉、IMU、编码器等多模态数据，并对现场操作员提供灯光/音频反馈。
- **运行角色**：双臂协同（Right=主控, Left=从控）。两侧通过 `DEVICE_SIDE` 环境变量区分，右臂负责按键交互、音频提示并驱动左臂同步录制。
- **部署形态**：所有脚本、C++ 二进制和资源都打包到 `/opt/ugripper`，由 systemd 服务和 udev 触发器驱动；数据保存到 `/mnt/data_disk/<device_id>/`。

## 2. 核心运行组件
### 2.1 主业务服务 (`ugripper.service` → `run_record.sh`)
- 由 `pack_script/ugripper.service` 安装并常驻运行，工作目录 `/opt/ugripper`，用户 `radxa`。
- 负责：
  - 磁盘/日志准备（本地 `/tmp` + 硬盘镜像备份）。
  - 元数据与校准文件初始化（`config/fake*Calib.json` → `/mnt/data_disk/.../calibration/`）。
  - 触觉相机序列号校验，自动替换 JSON 中的 `{{TACTILE_*_SERIAL}}` 占位符。
  - PTP 状态读取(`/dev/shm/umi_ptp_status`)并驱动 LED/AUDIO 状态。
  - 物理按钮状态机：
    - **单击**：启动/停止一次多模态录制，右臂会通过 `nc` 向左臂发送 `START|episode_xxxx` / `STOP|0`。
    - **长按**：录制下一条任务的“pre”语音描述。
    - **双击**：录制上一条任务的“post”语音备注。
  - 录制进程编排（见 2.2），退出时做数据校验 & `sync`。
  - 健康监控后台线程：磁盘、USB 相机、对端可达性、PTP 偏差 → 控制 LED/AUDIO。

### 2.2 传感器采集进程
| 模块 | 所在目录 | 启动方式 | 主要职责 |
| --- | --- | --- | --- |
| 相机三路录制 | `camera_record/triple_camera_record_h265.py` | `uv run ... --output-dir <episode>` | 使用 PyAV + Rockchip 硬编录制主摄 + 左右触觉，输出 `.mkv + .csv`；需要 `99-fixed-usb-map.rules` 将 UVC 映射为 `/dev/{left,right}_tcam`。|
| 编码器采集 | `encoder_refactor/build/main` | 直接执行 | C++17 + `libserialport`，以 1 kHz 轮询 `/dev/ttyS7`，将 `EncoderDriver` 数据写入 `encoder_data.mcap`（Schema: position_raw/rad）。|
| IMU 采集 | `im648_imu_alone/build/im648_imu` | 直接执行 | C++ 通过 `/dev/ttyS2` 读取 DM/IM648 IMU，1 kHz 写 `imu_data.mcap`（foxglove.Imu JSON）。|
| （可选）`dm_imu_alone/dm_imu` | `run_calibration.sh` 使用 | 提供校准和单独记录版本。|

所有二进制均由 `build_deb.sh` 触发 `cmake .. && make -j` 构建，并随 deb 包分发。

### 2.3 反馈与交互
- `led_manager.py`：通过 `/sys/class/pwm/pwmchip*/` 控制 RGB；监听 `/tmp/umi_led_pipe`，支持 `INIT/READY/RECORDING/ERROR/CALIB_*` 等状态及进度。
- `audio/audio_play.py`：`pygame` 播放提示音，监听 `/tmp/umi_audio_pipe`，同时负责设置 Rockchip 声卡音量。
- `run_record.sh` 和 `auto_calibration/run_calibration.sh` 通过 `uv run` 启动上述脚本并写入 FIFO，实现跨语言通信。

## 3. 支撑服务 & 运维流程
### 3.1 自动校准
- `auto_calibration/monitor_network.sh` 以 `ugripper-network-monitor.service` 常驻，监听 `end0` 网线插拔：
  - 上升沿：触发 `udevadm trigger --subsystem-match=block` 并 `systemctl start ugripper-calibration.service`。
  - 下降沿：尝试 `umount /mnt/data_disk` 保护硬盘。
- `ugripper-calibration.service` 执行 `auto_calibration/run_calibration.sh`：
  - 要求 USB U 盘节点 `/dev/usb_update_stick` 已连接且根目录存在 `calibration.txt`。
  - 按钮按下才会继续（防误触）。
  - 停止 `ugripper.service` → 启动 LED/音频守护 → 运行 IMU (`dm_imu_alone/build/imu_calib`) 与编码器 (`encoder_refactor/build/zeroing`) 校准 → 根据结果恢复服务。

### 3.2 时间同步
- `time_sync/ugripper-ptp-monitor.service`：运行 `ptp_monitor.sh`，每秒用 `pmc` & `phc_ctl` 记录 PTP 状态 JSON 至 `/dev/shm/umi_ptp_status`，供 LED/业务脚本消费。
- `time_sync/ugripper-ntp-sync.service`：运行 `safe_ntp_sync.sh`，按如下策略：
  1. 等待公网可达；
  2. 检查 `/tmp/umi_recording.lock` 确认空闲；
  3. `timedatectl set-ntp true` 触发一次同步，完成后再关闭，避免录制过程系统时间漂移；若录制中断则放弃。
- `pack_script/postinst` 依据 `DEVICE_SIDE` 自动生成 `ptp4l.service` & `phc2sys.service`：
  - Right：高优先级 Master，System Clock → PHC。
  - Left：Slave，PHC → System Clock。

### 3.3 自动更新 & 自愈
- `auto_update/99-usb-auto-update.rules` + `usb-auto-update@.service`：U 盘插入即运行 `auto_update/usb_auto_update.sh /dev/%k`。
  - 可选强校验指定 by-path；
  - 顺序：自更新 `ugripper-usb-updater` → 部署 `ugripper_*_arm64.deb` → 重启服务。
- `auto_update/ugripper-boot-install.service`：开机检测若 `ugripper` 未安装则到 `/opt/backup` 搜索 `ugripper_*_arm64.deb` 自动修复。

### 3.4 Udev 与权限
- `camera_record/99-fixed-usb-map.rules`：为主摄/触觉相机分配稳定的 `/dev/{cam_main,left_tcam,right_tcam}`，并设置 `MODE="0666"`、`SYMLINK+=`。
- `encoder_refactor/99-serial.rules`：放宽 `/dev/ttyS[2|7]` 权限供非 root 访问。
- `build_deb.sh` 在打包时复制上述规则到 `/etc/udev/rules.d/`。

## 4. 数据与配置流
1. **数据目录**：`/mnt/data_disk/<device_sn>/<role>/`
   - `metadata/metadata.json`：定义设备类型、默认 episode 模板，由 `run_record.sh` 自动生成（含 `DEVICE_SIDE`、`DEVICE_SN`）。
   - `calibration/{cam,encoder,imu}.json`：初始化自 `/config/fake*.json`，运行时根据 USB 序列、左右相机角色动态替换。
   - `data/episode_YYYYMMDD_NNNN/`
     - `cam.mkv`, `tact_left/right.mkv` + 对应 `*.csv`
     - `encoder_data.mcap`, `imu_data.mcap`
     - `audio_pre/post.wav`（如录制）
     - `validation_error.log`（若校验失败）
2. **日志**：主脚本将 stdout/err `tee` 到 `/tmp/umi_sys_<sn>_<date>.log` 并由后台任务增量同步到 `/mnt/data_disk/logs/`。
3. **状态文件**：
   - `/tmp/umi_recording.lock`：录制期间阻止 NTP/其他高 IO 操作。
   - `/dev/shm/umi_ptp_status`：PTP JSON。
4. **网络信令**：Right 侧通过 `nc` TCP 12345 发送 `START|dir` / `STOP|0` 给 Left。

## 5. 构建与打包（`build_deb.sh`）
> 目标：生成 `${APP_NAME}_${VERSION}_${ARCH}.deb`，默认路径：项目根。

1. **模式切换**：
   - 标准模式：`rm -rf build_deb_temp`、重新编译 C++、复制 `.venv`。
   - 快速模式 (`-q`/`--quick`)：
     - 保留 `build_deb_temp`（避免重复复制 Python 依赖）。
     - 跳过重新编译；若缺二进制给出警告。
     - 仅在临时目录不存在 `.venv` 时才从源码复制，自动排除 `.env`。
2. **C++ 构建**：分别进入 `dm_imu_alone`、`encoder_refactor` 执行 `cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)`。
3. **资源装配**：
   - `rsync -av --exclude {ASR,py_script,...}` -> `/opt/ugripper`；之后显式把 `build/*` 二进制回填。
   - 复制 udev 规则、systemd 单元（`pack_script/ugripper.service`、`auto_calibration/*.service`、`time_sync/*.service`）和 `DEBIAN` 控制脚本。
4. **变量注入**：对 `control/postinst/prerm/postrm/ugripper.service` 统一替换 `{{APP_NAME}}/{{VERSION}}/{{ARCH}}/{{INSTALL_DIR}}`。
5. **打包**：`dpkg-deb --build build_deb_temp ${APP_NAME}_${VERSION}_${ARCH}.deb`。

### Postinst 关键动作
- 根据 `DEVICE_SIDE` 设置 `end0` 静态 IP（192.168.1.100/101）。
- 生成、启用 `ptp4l.service`、`phc2sys.service`、`ugripper-ptp-monitor.service`、`ugripper-ntp-sync.service`、`ugripper-network-monitor.service`。
- 触发 `udevadm control --reload-rules`。
- 保证所有脚本可执行 (`chmod 755`).

## 6. 依赖与运行环境
- **系统包**：`sox`, `libsox-fmt-all`, `netcat-openbsd`, `linuxptp`, `libserialport`, `alsa-utils`, `jq`, `uv`（或 Python 3.11 + pipx 以运行 `uv`）。
- **Python**：由 `pyproject.toml` 声明，包含 `av`, `mcap`, `pydub`, `pygame`, `pyserial`, `pytest` 等；脚本用 `uv run <script>` 自动解析虚拟环境。
- **C++**：依赖 `libserialport`, `pthread`, `mcap`（C++ 版）。
- **硬件**：
  - UVC 摄像头 ×3（主摄 + 左右触觉），USB 序列用于绑定。
  - RS485/串口 IMU `/dev/ttyS2`，编码器 `/dev/ttyS7`。
  - Rockchip 声卡 `rockchipes8388`，RGB LED PWM 控制。
  - 物理录制按钮（GPIO PIN_36）。

## 7. 复用 / 定制清单
1. **移植到新硬件**：
   - 更新 `auto_calibration/run_calibration.sh`、`run_record.sh` 中的串口、GPIO、音频设置。
   - 调整 `pack_script/postinst` 静态 IP、PTP 接口，以及 `auto_update/99-usb-auto-update.rules` 的 by-path。
2. **添加新传感器**：
   - 新建采集进程（可借鉴 C++ MCAP 模板或 Python `camera_record`），在 `start_recording/stop_recording/validate_recording` 中注册即可。
   - 在 `calibration/` 中添加占位 JSON，并在 `handle_global_placeholders` 里扩展。
3. **替换反馈通道**：
   - 修改 `led_manager.py` PWM 映射或 `audio/audio_play.py` 声卡检测逻辑即可，不影响主服务。
4. **打包更新**：
   - `VERSION` 单点维护于 `build_deb.sh`。
   - 按需扩展 `EXCLUDE_LIST` 与 `FILES_TO_PATCH`，支持更多 systemd/udev/配置模板。
5. **灾难恢复**：
   - 确保在 `/opt/backup` 或 U 盘根目录保留至少一个 `ugripper_*_arm64.deb`，让 boot-self-heal / USB auto update 可以回滚。

## 8. 快速排障指引
- **服务状态**：`systemctl status ugripper.service`、`journalctl -u ugripper.service -f`。
- **LED/音频卡死**：确认 `/tmp/umi_led_pipe`、`/tmp/umi_audio_pipe` 是否存在，必要时 `pkill -f led_manager.py`。
- **PTP**：查看 `/dev/shm/umi_ptp_status`；运行 `pmc -u -b 0 'GET CURRENT_DATA_SET'` 验证偏移。
- **更新失败**：检查 `/var/log/ugripper/usb_auto_update.log` 与 `/var/log/ugripper/boot_install.log`。
- **数据校验失败**：阅读 `episode_xxxx/validation_error.log` 并复查对应传感器日志。

> 本架构文档涵盖大部分自治式数采项目所需的流程，可在相同模板上替换传感器、部署介质或更新策略，以快速构建面向生产的多模态采集终端。
