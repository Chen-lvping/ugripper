# Ugripper V2 Overview

本文档是当前仓库在交付、开发、排障和验收时的**唯一主说明**。
它只描述当前仍成立的系统实现、运行方式、部署入口和运维口径；不再拆分独立的运维文档、差异文档或遗留清单文档。

## 1. 当前系统一句话
- 当前系统默认以**单机双手、本地控制**方式运行。
- 主控制链路已经收口到 `build/src/record_runtime/record_runtime`，`run_record.sh` 只负责进入安装目录并 `exec` 该二进制。
- 默认录制产物为 **2 路主相机 + 2 路 stereo + 4 路触觉相机 + 左右两份传感器 MCAP**。
- HMI 按键、RGB 灯效、提示音、pre/post 音频录制、停录校验和双键关机请求都已纳入当前运行时。
- U 盘流程统一负责 `deb` 升级、`config.txt` 导入、主相机标定导入和 encoder 零位校准触发。

## 2. 安装布局与入口
- 安装目录：`/opt/ugripper`
- 主服务：`pack_script/ugripper.service`
- 主入口：`/opt/ugripper/run_record.sh`
- 主运行时：`/opt/ugripper/build/src/record_runtime/record_runtime`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- 持久化标定目录：`/etc/ugripper/config/calibration`
- 日志目录：`/var/log/ugripper`
- U 盘升级入口：`auto_update/usb_auto_update.sh`

## 3. 模块总览
| 模块 | 入口 | 当前职责 | 关键输入/输出 |
| --- | --- | --- | --- |
| systemd 主服务 | `pack_script/ugripper.service` | 以 `ubuntu` 用户拉起录制服务 | `/opt/ugripper/run_record.sh` |
| 薄壳启动脚本 | `run_record.sh` | 切到安装目录并直接 `exec record_runtime` | 无 |
| 主运行时 | `build/src/record_runtime/record_runtime` | HMI 按键状态机、LED 灯效、提示音、pre/post 音频、camera/sensor 子进程管理、停录校验、关机请求 | episode 目录、`/tmp/umi_shutdown_request` |
| 相机录制 | `build/src/camera_recorder/camera_recorder` | 按 YAML 配置启动独立相机录制子进程 | 8 路 `mkv`（默认） |
| 传感器录制 | `build/src/sensor_recorder/sensor_recorder` | 录制左右 IMU/encoder，分别输出 MCAP | `sensor_data_left.mcap`、`sensor_data_right.mcap` |
| HMI 类库 | `src/gripper_hmi` | 读取夹爪按键，并在驱动内部以单线程 owner 线程完成状态查询、灯效生成与 RGB 指令发送；默认由状态机切灯效，必要时仍可直接下发 RGB | 按键快照、RGB 指令 |
| 音频播放 | `audio/audio_play.py` | 优先绑定受支持 USB 耳机、无耳机时回退系统默认声卡；播放提示音并处理耳机 HID 音量键；初始化阶段受控处理 idle suspend | `/tmp/umi_audio_pipe` |
| 音频采集 | `audio/record_usb_audio.py` | 优先从受支持 USB 耳机麦克风录音，无耳机时回退系统默认 source，供 pre/post 处理链路使用 | 临时 wav 文件 |
| 数据盘挂载 | `config/99-fixed-usb-map.rules` | 限定允许物理 USB 口，使用 `systemd-mount` 将数据盘挂到 `/mnt/data_disk`，并在同一 udev 事件里拉起 updater | `/mnt/data_disk`、`usb-auto-update@<dev>.service` |
| USB 导入/升级 | `auto_update/usb_auto_update.sh` | 处理 `deb` 升级、配置导入、主相机标定导入、encoder 校准触发 | `/etc/environment`、`calibration.json` |
| 校准执行 | `auto_calibration/run_calibration.sh` | 在 `calibration.txt` 存在时停止业务、复用 `/mnt/data_disk` 触发左右编码器并行 zeroing、恢复服务 | `build/src/sensor_recorder/zeroing` |

## 4. 启动链路
1. `pack_script/postinst` 在安装或升级 `ugripper` 时会尝试把 `/home/user/lib/exfat.ko` 复制到 `/lib/modules/<kernel>/extra/`，写入 `/etc/modules-load.d/ugripper-exfat.conf`，并在安装当次立即尝试加载 `exfat`。
2. 后续开机时，`systemd-modules-load` 会优先加载 `exfat`；允许的 USB 数据盘分区再由 `config/99-fixed-usb-map.rules` 直接触发 `systemd-mount` 挂载到 `/mnt/data_disk`。
3. systemd 启动 `ugripper.service`。
4. 服务进入 `/opt/ugripper/run_record.sh`。
5. `run_record.sh` 检查 `./build/src/record_runtime/record_runtime` 是否存在，并等待 `/mnt/data_disk` 成为真实可写挂载点后再 `exec`。
6. `record_runtime` 启动后读取 `/etc/environment`，至少关注：
   - `DEVICE_SN`：决定数据目录。
   - `CAMERA_CODEC`：非法值会回退到 `h264`。
   - `UGRIPPER_LANG`：决定提示音语言。
7. 运行时连接左右夹爪 HMI、启动 LED 渲染线程、尝试拉起音频守护进程。
   - HMI 状态查询与 LED RGB 下发会在单个 gripper 串口内由驱动 owner 线程统一调度；`record_runtime` 只切换灯效模式，不再跨线程推送录制态的 500ms 亮灭边沿；若有专项诊断或直控需求，仍可走 direct RGB 通道覆盖当前效果。
8. 初始化成功后进入 `READY` 状态并等待右手夹爪按键事件。

## 5. 状态机与按键行为
### 5.1 空闲态与阈值
- `READY`：绿色呼吸灯，提示系统可开始录制。
- 当前 `READY` 呼吸灯周期约 `4.5s`，LED 渲染线程约每 `20ms` 刷新一次，降低肉眼可见亮度抖动。
- 主循环轮询周期约 `20ms`。
- 长按判定阈值 `800ms`，双键关机提示阈值 `2000ms`，双键执行阈值 `4000ms`。

### 5.2 按键动作
- `BTN_UP` 短按释放：
  - 空闲时开始普通录制。
  - 录制中停止当前录制。
- `BTN_DOWN` 短按释放：
  - 空闲且存在上一条 episode 时开始 reset 录制。
  - 空闲但无上一条 episode 时只播报 `no_reset_needed`。
  - 录制中停止当前录制。
- `BTN_UP` 长按：空闲时录制 pre audio；录制中忽略。
- `BTN_DOWN` 长按：空闲时录制 post audio；录制中忽略。
- 双键长按：
  - 2 秒时播放 `shutdown` 提示音。
  - 4 秒时进入 `EXIT`，必要时先停录，然后写 `/tmp/umi_shutdown_request`。

## 6. 录制生命周期
### 6.1 开始录制
1. 在 `/mnt/data_disk/<device_sn_lower>/data` 下创建新的 `episode_YYYYMMDD_NNNN`。
2. 写入 `metadata.json`，当前固定包含：
   - `device_type=UMI`
   - `device_model=ugripper`
   - `device_id=<DEVICE_SN>`
   - `collector=default_user`
   - `data_path=data/episode_{date:08d}_{episode_index:04d}`
   - `camera_codec=<h264|h265>`
   - `ugripper_lang=<zh|en>`
   - `ugripper_version=<deb version>`
   - `ugripper_usb_updater_version=<deb version>`
   - `data_format_version=1`
   - `record_runtime=cpp`
   - `reset_recording=<true|false>`
3. 写入 `calibration.json`：当前优先使用持久化标定 `/etc/ugripper/config/calibration/calibration.json`；若该文件缺失、为空、非法 JSON 或顶层不是 object，则回退仓库根目录样例 `calibration.json`，再缺失时回退 `config/fakeCamCalib.json`。episode 内会保留主摄与触觉相机标定，并将旧 Fays 标定块替换为 `left_stereo` / `right_stereo` 占位标定。
4. `record_runtime` 不再预写 `info.json`；最终 `info.json` 由 `camera_recorder` 在每路首个 pre-mux packet 锁定 `PTS + 系统时间` 后统一生成。
5. 若已准备 pre audio，则移动到本次 episode 的 `audio_pre.wav`。
6. 并行启动：
   - `camera_recorder --codec <codec> --output-dir <episode> --only left_cam_main,right_cam_main,left_stereo,right_stereo,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r`
   - `sensor_recorder <episode_dir>`
7. 切换到 `RECORDING` 状态并播放开始提示音。

### 6.2 相机链路
- 配置入口：`config/camera_recorder.yaml`。
- 当前 YAML 定义 8 路相机：左右主摄、左右 stereo、4 路触觉。
- `udev` 口位策略当前口径：
  - stereo 与 CH9344 串口桥允许同侧 hub 的内部端口 `.1/.2` 互换；
  - tactile `l/r` 的内部端口映射已按当前现场布线对调；
  - tactile 仍按左右侧固定 kernel 路径命名，不做跨侧互换。
- 当前运行时默认录制全部 8 路：左右主摄 + 左右 stereo + 4 路触觉。
- `camera_recorder` 当前会并发拉起全部已选中的相机子进程，不再在管理层按 YAML 顺序逐路等待启动返回。
- 停录阶段也会并发向各路相机子进程发 stop，并在全部 stop 返回后统一 poll 状态，降低多路顺序收尾导致 `info.json` 缺失或容器未 finalize 的风险。
- 主相机模式是 `direct-copy-h265`：主摄始终走相机原生码流直封装，不做二次编码；实际拉流格式跟随 `CAMERA_CODEC` 选择 `h264` 或 `h265`。
- 触觉 / 双目模式保留 `hybrid-decode-encode` / `stereo-hybrid-decode-encode`。
- stereo 当前默认按 `1280x400@60` 录制，并在 YAML 内使用更激进的 HEVC CQP（`qp_init=34`、`qp_min=28`、`qp_max=42`、`qp_min_i=24`、`qp_max_i=42`）；该组参数基于同一段 `1280x400@60` 原始 MJPEG 样本压测，可将单路码率压到 `1Mbps` 以下。
- 每路相机由独立子进程承载；单路失败不会由 `camera_recorder` 主动连带停掉其他相机。
- `info.json` 的时间字段由 `camera_recorder` 负责写出，而不是在停录校验阶段回填：
  - `boot_time_offset`
  - `boot_time_offset_us`
  - 8 路 `<camera>_record_time_offset_us`
- 当前实现参考 V1 语义，在每路相机首个 pre-mux packet 到达时原位锁定 `PTS/timebase + 系统时间`，用于保持 `frame_system_time_us = frame_pts_us + <camera>_record_time_offset_us` 的对齐方式。

### 6.3 传感器链路
- `sensor_recorder` 固定录制：
  - 右手：`/dev/right_imu`、`/dev/right_encoder`
  - 左手：`/dev/left_imu`、`/dev/left_encoder`
- 启动阶段会并发初始化左右 IMU 和左右 encoder，减少首样本被串行初始化链路拉长。
- IMU 配置阶段保留固定 settle wait，但已从旧的长等待收敛到更短窗口，优先压缩起录前空转时间。
- 输出拆成两份 MCAP：
  - `sensor_data_right.mcap`
  - `sensor_data_left.mcap`
- encoder 连接会优先尝试 `1Mbps`，失败后回退 `115200`。

### 6.4 停止录制
1. 停止 `sensor_recorder` 与 `camera_recorder`。
2. 先发送 `recording_stop`，随后立即切到 `writing`；提示音采用“后触发抢占前触发”的语义，因此 `writing` 会直接打断仍在播放的上一条提示。
3. 进入 `writing` 阶段：切换 `INIT` 蓝灯并执行 `sync`。
4. 执行稳定校验：强校验 `camera_recorder` 已写出的 `info.json` 时间字段，并用轻量 `ffprobe` 检查 8 路视频可读性与时长合理性。
5. 成功则回到 `READY` 并播放 `ready`；完整性失败则进入 `ERROR_1` 并播放 `validation_failed`；运行时异常进入 `ERROR_5` 并播放 `error`。这些后续提示同样会直接抢占当前播放中的 `writing`。

## 7. Episode 产物与检查
### 7.1 默认产物
- 视频：
  - `left_cam_main.mkv`
  - `right_cam_main.mkv`
  - `left_stereo.mkv`
  - `right_stereo.mkv`
  - `left_tcam_l.mkv`
  - `left_tcam_r.mkv`
  - `right_tcam_l.mkv`
  - `right_tcam_r.mkv`
- 传感器：
  - `sensor_data_left.mcap`
  - `sensor_data_right.mcap`
- 元数据：
  - `metadata.json`
  - `calibration.json`
- 条件产物：
  - `audio_pre.wav`
  - `audio_post.wav`
  - `info.json`
    - `boot_time_offset`
    - `boot_time_offset_us`
    - 8 路 `<camera>_record_time_offset_us`

### 7.2 停录强校验
停录后当前按以下层次校验：
- 文件存在性：八路 `mkv`、`sensor_data_left.mcap`、`sensor_data_right.mcap`、`metadata.json`、`calibration.json`、`info.json` 必须存在且非空。
- `info.json` 字段完整性：强制包含 `boot_time_offset`、`boot_time_offset_us` 与 8 路 `<camera>_record_time_offset_us`，且 `boot_time_offset` 与 `boot_time_offset_us` 必须数值一致。
- 视频可读性：每路 `mkv` 都必须能被 `ffprobe` 读出首个视频流与 `start_time/duration`。
- 时长合理性：每路视频跨度都必须大于最小阈值，且不能比本次 episode 的最长视频短超过 `5s`。
- 条件产物：若执行了 pre/post 音频录制，对应 wav 仍需存在。

说明：当前不会为视频做全量逐帧扫描；校验只读取容器元信息并消费已有 `info.json`，优先保证现场稳定性与停录耗时可控。

## 8. 灯效、音频与关键路径
### 8.1 当前状态灯语义
- `INIT`：初始化或落盘阶段，蓝灯。
- `READY`：可录制，绿色呼吸灯。
- `RECORDING`：录制中，绿色闪烁。
- `CALIB_PRE` / `CALIB_RUN` / `CALIB_DONE`：供 USB 导入与校准脚本复用。
- `ERROR_1` ~ `ERROR_5`：红灯长短码，分别用于完整性失败到运行时错误。
- `EXIT`：关机退出阶段。

### 8.2 关键持久化与临时路径
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- 运行时音频 FIFO：`/tmp/umi_audio_pipe`
- 音频临时目录：`/tmp/umi_audio`
- 关机触发文件：`/tmp/umi_shutdown_request`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- `/mnt/data_disk` 只作为固定挂载点使用：安装阶段会预创建为 `root:root 0555`，业务不会把本地空目录当成数据目录；只有真实数据盘挂载成功后才允许继续启动录制服务。

### 8.3 音频链路关键行为
- `audio/audio_play.py` 启动时按 `UGRIPPER_LANG` 选语音，并优先绑定 PulseAudio 中受支持的 USB 音频设备；当前兼容 `0020:0b21 (liyuany USB Audio)` 与 `0023:0b23 (liyuany USB PnP Sound Device)`。若无耳机，则回退系统默认 sink/source；耳机晚于服务启动才出现时，守护进程会先启动 FIFO 和监听线程，再在耳机出现后自动切回耳机。
- 提示音主线程通过 FIFO 收命令后使用 `pygame.mixer` 播放到当前选中的 PulseAudio sink；启动时只做一次短静音预热、每段提示音前补前导静音，不再维持常驻静音 keepalive。
- `audio/audio_play.py` 只有在真实绑定到一个可用的 PulseAudio 播放目标并完成后端初始化后才会写 `/tmp/umi_audio_ready`；当前无论是受支持 USB 耳机还是系统默认声卡，都需要建好 backend 才会进入 ready，backend teardown 时会移除该标记，避免业务把“进程活着”误判成“提示音已可播放”。
- 提示音调度采用“后触发抢占前触发”的语义，不做排队串行；新的命令到达后会立即停止当前 one-shot 或 loop 提示，再播放最新命令。当前 `writing`、`calibrating` 属于 loop 提示，但同样会被后续命令直接打断。
- `record_runtime` 会在主循环内监测音频守护进程；若守护进程异常退出会按节流策略自动重拉起。正常情况下，音频守护进程会在“受支持 USB 耳机”和“系统默认声卡”之间自动切换，并在 `/tmp/umi_audio_ready` 恢复后补发空闲态 `ready` 或当前阶段提示。
- 耳机运行中被拔掉时，音频守护进程不会退出，而是自动退回系统默认声卡；耳机重新插入并重新出现在 PulseAudio 后，会自动重新绑定回耳机，后续提示音恢复。
- 播放/录音初始化前会受控执行 `pactl unload-module module-suspend-on-idle`，避免 USB 耳机或默认声卡在长时间空闲、热插拔恢复或首次切换后出现首段吞音；该动作只收敛在初始化阶段，不在每次提示音、录音或音量键事件里重复切换模块。
- `py_script/usb_audio_mic_test.py --playback` 默认只做“原生采集 + SoX 后处理导出”，不再默认硬套旧 `noise.prof`；若需去噪，先运行 `py_script/usb_audio_noise_profile.py` 生成当前环境底噪 profile，再显式传入 `--denoise --noise-profile <path>`。
- 现场回归优先覆盖两类场景：耳机长时间空闲后的首次播放 `python3 py_script/usb_audio_play_test.py`，以及长时间空闲后的首次录音 `python3 py_script/usb_audio_mic_test.py --playback`；两项测试都应在日志中看到 `Disabled PulseAudio suspend modules: ...`。
- 回滚方式：若需恢复 PulseAudio 默认模块状态，可重启当前用户的 PulseAudio 会话，或重启 `ugripper.service` 让音频守护进程重新按默认环境启动；无需在运行期反复手工切换 `module-suspend-on-idle`。
- 现场 5 步回归 SOP：1）确认耳机已识别且服务正常，观察 `journalctl -u ugripper.service -n 100` 是否出现 USB 音频初始化日志；2）空闲 3~5 分钟后执行 `python3 py_script/usb_audio_play_test.py`，确认首个测试音不吞头；3）再次空闲 3~5 分钟后执行 `python3 py_script/usb_audio_mic_test.py --playback`，确认录音回放起始段不被截断；4）若需覆盖热恢复，再做一次耳机热插拔后重复步骤 2/3；5）若结果异常，记录 `pactl list short modules`、`pactl list short sinks`、`pactl list short sources` 与 `journalctl -u ugripper.service -n 200` 作为现场。

### 8.4 相关辅助单元
- `auto_update/umi-shutdown-trigger.path`：监控 `/tmp/umi_shutdown_request`。
- `auto_update/umi-shutdown-trigger.service`：检测到触发文件后执行 `systemctl poweroff`。
- `auto_calibration/ugripper-network-monitor.service`：监听网线插拔，当前仅在拔线时重启 `ugripper.service`。
- `auto_update/boot_check_install.sh`：开机时检查 `/opt/backup` 中的 `deb` 是否需要恢复或升级。

## 9. 配置、安装与 U 盘流程
### 9.1 当前主要配置入口
当前主要配置来自 `/etc/environment`。

| 键 | 当前用途 | 备注 |
| --- | --- | --- |
| `DEVICE_SN` | 决定数据路径与标定导入匹配目录 | 建议视为必填 |
| `UGRIPPER_LANG` | 提示音语言 | 由 `config.txt` 导入 |
| `CAMERA_CODEC` | `camera_recorder` 启动参数 | 仅支持 `h264` / `h265` |

说明：当前录制与 U 盘导入流程都不再使用角色环境变量；episode `metadata.json` 也不再写角色字段。

### 9.2 当前默认项
| 项目 | 当前默认口径 |
| --- | --- |
| 部署形态 | 单机双手、本地录制 |
| 录制相机集合 | 左右主摄 + 左右 stereo + 4 路触觉 |
| 网络 | 业务可在无对端设备时启动 |
| 静态 IP | 主包不托管，沿用系统现有有线配置 |

### 9.3 U 盘支持内容
- 主包升级：根目录放置 `ugripper_*_arm64*.deb`。
- updater 自升级：根目录放置 `ugripper-usb-updater*.deb`。
- 配置导入：根目录 `config.txt`。
- 主相机标定导入：`ugripper_calib/<DEVICE_SN>/`，通过文件名后缀 `_left` / `_right` 区分左右夹爪参数文件。
- encoder 零位校准触发：根目录 `calibration.txt`。

### 9.4 U 盘同次插入顺序
当同一次 U 盘插入同时包含 `config.txt`、`ugripper_calib/` 和 `calibration.txt` 时，当前顺序是：
1. 导入 `config.txt`
2. 导入左右主相机标定（按 `_left` / `_right` 文件后缀区分；缺失的一侧保持原值）
3. 若存在 `calibration.txt`，则跳过中间重启，直接进入左右编码器并行校准流程
4. 校准完成后恢复 `ugripper.service`

### 9.5 安装脚本与网络行为
`pack_script/postinst` 当前会：
- 停掉旧的录制相关进程。
- 初始化持久化标定目录；若 `calibration.json` 缺失、空文件或非法 JSON，则自动用 `config/fakeCamCalib.json` 修复。
- 自动配置 exfat 提前加载：若现场仍使用 `/home/user/lib/exfat.ko` 外部模块，安装时会把它复制进 `/lib/modules/<kernel>/extra/`、写入 `/etc/modules-load.d/ugripper-exfat.conf`，并在本次安装窗口内尝试立即加载。
- 重新加载 udev 规则。
- 启用 `umi-shutdown-trigger.path`。
- 启用并重启 `ugripper.service`。
- 启用并重启 `ugripper-network-monitor.service`。

网络相关当前行为：
- 主包不再创建、更新或删除有线 NetworkManager 连接。
- 主包安装/卸载默认沿用系统原有的有线 IP 配置，不对现场网络拓扑做接管。
- 当前录制启动不依赖对端网络存在。

数据盘挂载当前行为：
- `config/99-fixed-usb-map.rules` 会在允许的物理 USB 口上直接调用 `systemd-mount --owner=ubuntu --options=noatime <dev> /mnt/data_disk`，固定挂载点仍然是 `/mnt/data_disk`。
- 同一条 udev 规则会通过 `SYSTEMD_WANTS` 拉起 `usb-auto-update@<dev>.service`；`usb_auto_update.sh` 只会在确认 `/mnt/data_disk` 当前挂载源就是该设备后，才扫描 deb / config / calibration 文件。
- 允许的 USB 分区在 `remove` 事件里会显式对 `/mnt/data_disk` 执行卸载清理，避免拔盘后残留 stale mount；`run_record.sh` 也会把“挂载点只读”或“挂载源设备节点已不存在”视为未就绪状态。

### 9.6 网线监测行为
`auto_calibration/monitor_network.sh` 当前行为：
- 网线拔出：仅重启 `ugripper.service`
- 网线插入：不重启 `ugripper.service`，也不执行其他额外动作
- 若存在 `/run/ugripper_installing_from_usb.lock`，则跳过升级窗口内的边沿动作

## 10. 常用检查与排障入口
### 10.1 服务管理
```bash
sudo systemctl status ugripper.service
sudo systemctl restart ugripper.service
sudo journalctl -u ugripper.service -f
```

### 10.2 录制相关进程
- 主运行时：`record_runtime`
- 相机录制：`camera_recorder`
- 传感器录制：`sensor_recorder`
- 音频播放：`audio/audio_play.py`

### 10.3 U 盘流程日志
```bash
sudo tail -n 200 /var/log/ugripper/usb_auto_update.log
sudo tail -n 200 /var/log/ugripper/boot_install.log
```

### 10.4 设备映射优先检查
优先检查以下 symlink / 设备是否存在且方向正确：
- `/dev/left_cam_main`
- `/dev/right_cam_main`
- `/dev/left_tcam_l`
- `/dev/left_tcam_r`
- `/dev/right_tcam_l`
- `/dev/right_tcam_r`
- `/dev/left_imu`
- `/dev/right_imu`
- `/dev/left_encoder`
- `/dev/right_encoder`
- `/dev/left_gripper`
- `/dev/right_gripper`

### 10.5 快速定位建议
- 不能启动：先看 `ugripper.service` 日志和 `record_runtime` 是否成功拉起。
- 能录不能停：优先检查 HMI 按键事件、状态机和 `sensor_recorder` / `camera_recorder` 退出路径。
- 少文件或校验失败：先核对六路视频、双 MCAP、`metadata.json`、`calibration.json` 是否完整。
- 音频异常：优先看 USB 耳机枚举、PulseAudio sink/source、`module-suspend-on-idle` 是否已在初始化阶段被卸载。
- U 盘流程异常：先看 `/var/log/ugripper/usb_auto_update.log`，再区分是包安装、配置导入、标定导入还是 encoder 校准失败。

## 11. 当前使用注意点
- `config/camera_recorder.yaml` 当前 8 路配置全部默认启用；若现场需要裁剪录制集合，应明确同步调整 `record_runtime` 的 `--only` 参数与 episode 校验清单。
- 仓库内仍有部分后处理脚本依赖旧输出命名或旧假设，不能默认视为当前主链路的一部分。
- `info.json` 会生成，但当前不属于最小强校验集合。
- 现场调试若需要同时访问不同子网设备，应由系统侧或人工维护额外地址/路由；主包不负责托管这些网络策略。
- 校准、导入与恢复动作仍分散在多个 shell 脚本和 service 中；当前功能可用，但维护时需要注意入口分散。
- 若设备已被错误部署且不希望覆盖当前 SN，可在交付侧仓库执行 `../firmwareburner/repair_v2_env.sh`，该脚本会重装环境与 `ugripper` 并修复持久化 `calibration.json`。
