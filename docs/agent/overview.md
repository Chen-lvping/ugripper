# Ugripper V2 Overview

本文档是当前仓库在交付、开发、排障和验收时的**唯一主说明**。
它只描述当前仍成立的系统实现、运行方式、部署入口和运维口径；不再拆分独立的运维文档、差异文档或遗留清单文档。

## 1. 当前系统一句话
- 当前系统默认以**单机双手、本地控制**方式运行。
- 主控制链路已经收口到 `build/src/record_runtime/record_runtime`，`run_record.sh` 负责等待数据盘、维护 v1 风格运行日志，并拉起该二进制。
- 默认录制产物为 **2 路主相机 + 2 路 stereo + 4 路触觉相机 + 左右两份传感器 MCAP**。
- HMI 按键、RGB 灯效、提示音、pre/post 音频录制、停录校验和双键关机请求都已纳入当前运行时。
- U 盘流程统一负责 `deb` 升级、`config.txt` 导入、标定数据导入和 encoder 零位校准触发；当前自动安装名单已覆盖 `ugripper-usb-updater`、`ugripper`、`bluetooth-gatt-server`、`databot-device-joint` 与 `device-ota-mender`，并会在整批安装完成后通过 PulseAudio 播放 `upgrade_completed.wav`。

## 2. 安装布局与入口
- 安装目录：`/opt/ugripper`
- 主服务：`pack_script/ugripper.service`
- 主入口：`/opt/ugripper/run_record.sh`
- 主运行时：`/opt/ugripper/build/src/record_runtime/record_runtime`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- 持久化标定目录：`/etc/ugripper/config/calibration`
- 日志目录：`/var/log/ugripper`
- 运行日志镜像目录：`/mnt/data_disk/logs`
- U 盘升级入口：`auto_update/usb_auto_update.sh`

## 3. 模块总览
| 模块 | 入口 | 当前职责 | 关键输入/输出 |
| --- | --- | --- | --- |
| systemd 主服务 | `pack_script/ugripper.service` | 以 `ubuntu` 用户拉起录制服务 | `/opt/ugripper/run_record.sh` |
| 薄壳启动脚本 | `run_record.sh` | 切到安装目录、等待 `/mnt/data_disk` 可写、维护本地 `/tmp` 到 `/mnt/data_disk/logs` 的增量日志同步，再拉起 `record_runtime` | `/tmp/umi_sys_<sn>_<date>.log`、`/mnt/data_disk/logs/umi_sys_<sn>_<date>.log` |
| 主运行时 | `build/src/record_runtime/record_runtime` | HMI 按键状态机、LED 灯效、提示音、pre/post 音频、camera/sensor 子进程管理、停录校验、关机请求 | episode 目录、`/tmp/umi_shutdown_request` |
| 相机录制 | `build/src/camera_recorder/camera_recorder` | 普通录制模式下负责主摄/触觉会话录制；`--stereo-daemon` 模式下负责双目常驻预热、热插拔恢复与 session finalize | 8 路 `mkv`（默认） |
| 传感器录制 | `build/src/sensor_recorder/sensor_recorder` | 录制左右 IMU/encoder，按“采样入队 + 每侧独立 MCAP 写线程”分别输出 MCAP | `sensor_data_left.mcap`、`sensor_data_right.mcap` |
| HMI 类库 | `src/gripper_hmi` | 读取夹爪按键，并在驱动内部以单线程 owner 线程完成状态查询、灯效生成与 RGB 指令发送；默认由状态机切灯效，必要时仍可直接下发 RGB；当前也提供 SN 与 1024-byte 标定参数读写 API | 按键快照、RGB 指令、SN/标定参数读写 |
| 音频播放 | `audio/audio_play.py` | 优先绑定受支持 USB 耳机、无耳机时回退系统默认声卡；播放提示音并处理耳机 HID 音量键；初始化阶段受控处理 idle suspend | `/tmp/umi_audio_pipe` |
| 音频采集 | `audio/record_usb_audio.py` | 优先从受支持 USB 耳机麦克风录音，无耳机时回退系统默认 source，供 pre/post 处理链路使用 | 临时 wav 文件 |
| 数据盘挂载 | `config/99-fixed-usb-map.rules` | 限定允许物理 USB 口，使用 `systemd-mount` 将数据盘挂到 `/mnt/data_disk`，并在同一 udev 事件里拉起 updater | `/mnt/data_disk`、`usb-auto-update@<dev>.service` |
| USB 导入/升级 | `auto_update/usb_auto_update.sh` | 处理 `deb` 升级、配置导入、标定数据导入、encoder 校准触发；当前会按固定名单自动安装 `ugripper-usb-updater`、`ugripper`、`bluetooth-gatt-server`、`databot-device-joint`、`device-ota-mender`，全部安装完成后再通过 PulseAudio 播放 `upgrade_completed.wav`；其中标定导入会同时刷新主机侧主相机参数和夹爪侧 RGB/stereo/IMU payload | `/etc/environment`、`calibration.json`、夹爪 HMI |
| 校准执行 | `auto_calibration/run_calibration.sh` | 在 `calibration.txt` 存在时停止业务、复用 `/mnt/data_disk` 触发左右编码器并行 zeroing、恢复服务 | `build/src/sensor_recorder/zeroing` |

## 4. 启动链路
1. `pack_script/postinst` 在安装或升级 `ugripper` 时会尝试把 `/home/user/lib/exfat.ko` 复制到 `/lib/modules/<kernel>/extra/`，写入 `/etc/modules-load.d/ugripper-exfat.conf`，并在安装当次立即尝试加载 `exfat`。
2. 后续开机时，`systemd-modules-load` 会优先加载 `exfat`；允许的 USB 数据盘分区再由 `config/99-fixed-usb-map.rules` 直接触发 `systemd-mount` 挂载到 `/mnt/data_disk`。
3. systemd 启动 `ugripper.service`。
4. 服务进入 `/opt/ugripper/run_record.sh`。
5. `run_record.sh` 读取 `/etc/environment` 中的 `DEVICE_SN`，建立当天运行日志文件名 `umi_sys_<device_sn_lower>_<YYYYMMDD>.log`。
6. `run_record.sh` 检查 `./build/src/record_runtime/record_runtime` 是否存在，并等待 `/mnt/data_disk` 成为真实可写挂载点。
7. 数据盘可写后，`run_record.sh` 启动 v1 风格日志维护：业务 stdout/stderr 先落到 `/tmp`；当收到视频停录、音频停录请求或运行时退出时，再按增量方式同步到 `/mnt/data_disk/logs`，并清理本地与数据盘上同 SN 的非当天日志。
8. `run_record.sh` 再拉起 `record_runtime`；运行时退出前会额外补一次日志同步。
9. `record_runtime` 启动后读取 `/etc/environment`，至少关注：
   - `DEVICE_SN`：决定数据目录。
   - `CAMERA_CODEC`：非法值会回退到 `h264`。
   - `UGRIPPER_LANG`：决定提示音语言。
10. 运行时连接左右夹爪 HMI、启动 LED 渲染线程、尝试拉起音频守护进程。
- HMI 状态查询与 LED RGB 下发会在单个 gripper 串口内由驱动 owner 线程统一调度；`record_runtime` 只切换灯效模式，不再跨线程推送录制态的 500ms 亮灭边沿。当前驱动参考旧版 `driver_origin/led_manager` 的职责分离思路做了收敛：按键输入仍以 HMI 主动上报 `KeyReport` 为主，串口主动查询已降为约 `1s` 一次的低频探活/状态刷新；LED 仅在颜色变化、状态切换或低频补发时下发，避免高频状态查询与 RGB 指令互相抢占；若有专项诊断或直控需求，仍可走 direct RGB 通道覆盖当前效果。
- `src/gripper_hmi` 当前新增了 UMI SN / 标定参数协议封装：SN 固定为 32-byte 字段（当前现场 SN 文本示例为 16-char，尾部补 `0x00`），标定参数固定为 `1024 byte` 严格对齐结构；当前 payload 已覆盖 RGB 主相机、双目 `cam0/cam1`、`cam->imu` 外参、IMU 离散噪声/随机游走与残差统计，其中 header 会保留内部有效数据长度，但当前 `V1.1` 固件写入时仍必须补满 `64 x 16B` 数据包，具体协议见 `docs/umi_calibration_protocol.md`。
- UMI 标定写入当前增加了异常恢复口径：若写入阶段收到 `0xFE`（当前 chunk 零数据校验错误），驱动会优先重发当前 chunk；若连续出现 `0xFE`，或后续读 SN / 读标定返回 `0xF3/0xFE`，则会发送一次 `AbortWriteInData` 清理固件残留写入状态后再重试。
11. 初始化成功后进入 `READY` 状态并等待右手夹爪按键事件。
12. `record_runtime` 初始化阶段会额外拉起一个常驻 stereo warmup daemon；其在空闲态持续维持左右双目 MJPEG 取流预热，并通过 `/tmp/umi_stereo_camera_status.json` 暴露 `ready/not-ready` 状态。

## 5. 状态机与按键行为
### 5.1 空闲态与阈值
- `READY`：绿色呼吸灯，提示系统可开始录制。
- 当前 `READY` 呼吸灯周期约 `4.5s`，LED 渲染线程约每 `20ms` 按单调时钟刷新一次；驱动只在亮度实际变化时发送 RGB，并以约 `250ms` 的低频做灯效补发、约 `1s` 的低频做串口探活，降低肉眼可见抖动和录制态丢闪。
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
3. 写入 `calibration.json`：当前优先使用持久化标定 `/etc/ugripper/config/calibration/calibration.json`；若该文件缺失、为空、非法 JSON 或顶层不是 object，则回退仓库根目录样例 `calibration.json`，再缺失时回退 `config/fakeCamCalib.json`。持久化与 episode `calibration.json` 当前都会保留左右主摄、四路 tactile、`observation.images.left_stereo/right_stereo` 以及 `observation.imu.left_imu/right_imu`；每侧 stereo / IMU 的 payload 子集会额外挂到 `calibration_info.stereo_imu_bundles.<side>`。起录时仍会用 `udevadm` 从现场 `/dev/left_tcam_l`、`/dev/left_tcam_r`、`/dev/right_tcam_l`、`/dev/right_tcam_r` 分别回填 4 路真实 tactile USB serial；若持久化 calibration 里还只有旧的 `gripper_left_tactile` / `gripper_right_tactile`，运行时会按左右侧复制参数到四路独立项，但 episode 输出不再保留旧键。不回写持久化 calibration。
4. `record_runtime` 不再预写 `info.json`；最终 `info.json` 由 `camera_recorder` 在每路首个 pre-mux packet 锁定 `PTS + 系统时间` 后统一生成。
5. 若已准备 pre audio，则移动到本次 episode 的 `audio_pre.wav`。
6. 并行启动：
   - `camera_recorder --codec <codec> --output-dir <episode> --only left_cam_main,right_cam_main,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r`
   - `sensor_recorder <episode_dir>`
7. 同时向 stereo warmup daemon 写入本次 session 控制文件；双目不重启采集管线，只把本次 session 窗口内的帧纳入当前 episode，由 session writer 抽帧并编码落盘。
8. 切换到 `RECORDING` 状态并播放开始提示音。

### 6.2 相机链路
- 配置入口：`config/camera_recorder.yaml`。
- 当前 YAML 定义 8 路相机：左右主摄、左右 stereo、4 路触觉。
- `udev` 口位策略当前口径：
  - stereo 与 CH9344 串口桥允许同侧 hub 的内部端口 `.1/.2` 互换；
  - tactile `l/r` 的内部端口映射已按当前现场布线对调；
  - tactile 仍按左右侧固定 kernel 路径命名，不做跨侧互换。
- 当前运行时默认录制全部 8 路：左右主摄 + 左右 stereo + 4 路触觉。
- 普通录制阶段的 `camera_recorder` 当前只会起左右主摄 + 4 路触觉；左右 stereo 由单独的 `camera_recorder --stereo-daemon` 常驻管理。
- stereo daemon 在空闲态持续常驻打开双目设备并消费 `MJPEG 60fps` 帧；开始录制时只新建 session writer，对会话窗口内帧做 `1/2` 抽帧并编码成 `H.265 30fps` 写入最终 `mkv`，不重启双目采集进程。
- `camera_recorder` 当前会并发拉起全部已选中的相机子进程，不再在管理层按 YAML 顺序逐路等待启动返回。
- 停录阶段也会并发向各路相机子进程发 stop，并在全部 stop 返回后统一 poll 状态，降低多路顺序收尾导致 `info.json` 缺失或容器未 finalize 的风险。
- 主相机模式是 `direct-copy-h265`：主摄始终走相机原生码流直封装，不做二次编码；实际拉流格式跟随 `CAMERA_CODEC` 选择 `h264` 或 `h265`。
- 主摄 YAML 现支持可选 `uvc_roll_absolute`：若配置了标准 UVC `Roll Absolute` 目标值，`camera_recorder` 会在起录前先读取当前值；仅当当前值与目标值不一致时才通过 `libusb` 下发更新，并在设备节点恢复后继续走原有码流直封装，避免为翻转引入二次编解码。
- 触觉 / 双目模式保留 `hybrid-decode-encode` / `stereo-hybrid-decode-encode`。
- stereo 当前默认按设备 `1280x400@60` 常驻采集 MJPEG，session writer 按 `30fps` 抽帧后再编码成 `H.265` 写入 `mkv`；当前不再依赖后台 live encode + UDP relay。
- 每路相机由独立子进程承载；单路失败不会由 `camera_recorder` 主动连带停掉其他相机。
- stereo 热插拔语义：
  - 设备缺失时 daemon 状态降为 `not-ready/recovering`，运行日志会记录恢复前最后一帧时刻。
  - 设备重新枚举后自动重建该路 MJPEG warmup 取流并重新进入预热态；若掉线发生在录制中，则当前 stereo session 会被标记失败并在停录阶段显式报错，避免静默产出错位文件。
  - 最终 `left_stereo.mkv` / `right_stereo.mkv` 始终直接由录制会话写入单文件；不会在 episode 目录生成 `stereo_info.json`、`.concat.txt` 或后台 segment cache 文件。
- `info.json` 的时间字段由 `camera_recorder` 负责写出，而不是在停录校验阶段回填：
  - `boot_time_offset`
  - `boot_time_offset_us`
  - 8 路 `<camera>_record_time_offset_us`
- `info.json` 现额外包含 `stereo_session`，用于描述左右双目本次 session 的开启/停止系统时间，以及每路实际写入首尾帧的 PTS / 系统时间。
- 当前实现参考 V1 语义：双目在后台常驻消费设备 MJPEG 帧，录制开始后只把 session 窗口内实际送入 writer 的首尾帧系统时间与 PTS 写入 `stereo_session`，用于保持 `frame_system_time_us = frame_pts_us + <camera>_record_time_offset_us` 的对齐方式。
- 双目当前实现为“后台 MJPEG warmup + 录制时编码写最终文件”：空闲态不再保留 UDP / MPEG-TS live relay；按下录制后，session writer 只消费当前会话的 MJPEG 帧，抽帧后编码写入 `left/right_stereo.mkv`。
- stereo 顶层 `<camera>_record_time_offset_us` 与主摄/触觉保持同一语义：都以“本次最终输出首个写入帧”的 `PTS -> 系统时间` 映射为准。若需要和其他传感器做更精确的边界对齐，可结合 `info.json.stereo_session.start_system_time_us / stop_system_time_us` 以及 `cameras.<stereo>.first_written_frame_*` 字段使用。

### 6.3 传感器链路
- `sensor_recorder` 固定录制：
  - 右手：`/dev/right_imu`、`/dev/right_encoder`
  - 左手：`/dev/left_imu`、`/dev/left_encoder`
- 启动阶段会并发初始化左右 IMU 和左右 encoder，减少首样本被串行初始化链路拉长。
- IMU 配置阶段保留固定 settle wait，但已从旧的长等待收敛到更短窗口，优先压缩起录前空转时间。
- IMU 与 encoder 当前都会先按“解析/读取后入本地队列 -> 主循环批量消费 -> 每侧写线程落 MCAP”的方式输出；主循环不再直接同步阻塞 `McapWriter::write`。
- 左右 `sensor_data_*.mcap` 现各自由单独写线程落盘，降低 chunk 压缩或磁盘抖动对采样节奏的反压影响；若写队列持续堆积，日志会输出 backlog warning 便于现场判断是否存在写盘瓶颈。
- 输出拆成两份 MCAP：
  - `sensor_data_right.mcap`
  - `sensor_data_left.mcap`
- IMU / encoder 的 MCAP 时间戳当前默认沿用主机侧原始样本时间；若主循环一次从本地缓冲区取到多帧样本，则认为出现了缓冲区 burst，会以该批最后一帧的主机时间为锚点，按各自名义频率（IMU `200Hz`、encoder `1kHz`）向前回填这批样本的伪时间戳，尽量消除追赶帧导致的时间轴挤压。
- encoder 连接会优先尝试 `1Mbps`，失败后回退 `115200`。

### 6.4 停止录制
1. `record_runtime` 会先向 stereo warmup daemon 发送 stop-session，尽早冻结本次双目 session 的收尾边界，避免 stop 命令在普通相机与传感器都停完之后才传到双目链路。
2. 在 stereo daemon 收到 stop-session 后，`record_runtime` 再停止普通录制模式下的 `camera_recorder`，最后停止 `sensor_recorder`。
3. stereo daemon 收到 stop-session 后会先一次性冻结左右双目 session 的送帧边界，再逐路 finalize 文件，避免某一路在另一侧 finalize 期间继续长出额外尾巴；收尾完成后后台 warmup 继续运行。
4. 先发送 `recording_stop`，随后立即切到 `writing`；提示音采用“后触发抢占前触发”的语义，因此 `writing` 会直接打断仍在播放的上一条提示。
5. 进入 `writing` 阶段：切换 `INIT` 蓝灯并执行 `sync`；停录收尾完成后会请求 `run_record.sh` 将当前运行日志刷写到 `/mnt/data_disk/logs/`。
6. `record_runtime` 等待 daemon 在状态文件中写出本次 `last_session`，再将其并入最终 `info.json`。
7. 执行稳定校验：强校验合并后的 `info.json` 时间字段，并用轻量 `ffprobe` 检查 8 路视频可读性与时长合理性。
8. 成功则回到 `READY` 并播放 `ready`；完整性失败则进入 `ERROR_1` 并播放 `validation_failed`；运行时异常进入 `ERROR_5` 并播放 `error`。这些后续提示同样会直接抢占当前播放中的 `writing`。

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
  - `validation_error.log`（校验失败时，记录失败原因）
  - `info.json`
    - `boot_time_offset`
    - `boot_time_offset_us`
    - 8 路 `<camera>_record_time_offset_us`
    - `stereo_session`
      - `start_system_time_us`
      - `stop_system_time_us`
      - `cameras.<stereo>.first_written_frame_pts_us`
      - `cameras.<stereo>.first_written_frame_system_time_us`
      - `cameras.<stereo>.last_written_frame_pts_us`
      - `cameras.<stereo>.last_written_frame_system_time_us`

### 7.2 停录强校验
停录后当前按以下层次校验：
- 文件存在性：八路 `mkv`、`sensor_data_left.mcap`、`sensor_data_right.mcap`、`metadata.json`、`calibration.json`、`info.json` 必须存在且非空。
- `info.json` 字段完整性：强制包含 `boot_time_offset`、`boot_time_offset_us` 与 8 路 `<camera>_record_time_offset_us`，且 `boot_time_offset` 与 `boot_time_offset_us` 必须数值一致。
- 若存在 `stereo_session`，其 `episode_dir`、`start_system_time_us`、`stop_system_time_us` 与左右 stereo camera entry 必须齐全；当前不再依赖额外 sidecar 文件来完成校验或时间合并。encoder 尾部校验对 stereo 不再直接使用 `ffprobe duration` 推断 episode 结束点，而是优先使用 `stereo_session.stop_system_time_us` 作为双目 session 的尾部边界，避免 warmup/session 预热缓存把双目文件时长放大后误伤传感器尾部校验。
- 视频可读性：每路 `mkv` 都必须能被 `ffprobe` 读出首个视频流与 `start_time/duration`。
- 时长合理性：每路视频跨度都必须大于最小阈值，且不能比本次 episode 的最长视频短超过 `5s`。
- 条件产物：若执行了 pre/post 音频录制，对应 wav 仍需存在。

说明：当前不会为视频做全量逐帧扫描；校验只读取容器元信息并消费已有 `info.json`，优先保证现场稳定性与停录耗时可控。

## 8. 灯效、音频与关键路径
### 8.1 当前状态灯语义
- `INIT`：初始化或落盘阶段，蓝灯。
- `READY`：可录制，绿色呼吸灯；当前基于单调时钟渲染，避免系统校时导致相位突变。
- `RECORDING`：录制中，绿色闪烁；当前只在亮灭边沿和低频补发时下发 RGB，避免高频重复写串口造成丢闪。
- `CALIB_PRE` / `CALIB_RUN` / `CALIB_DONE`：供 USB 导入与校准脚本复用。
- `ERROR_1` ~ `ERROR_5`：红灯长短码，分别用于完整性失败到运行时错误。
- `EXIT`：关机退出阶段。

### 8.2 关键持久化与临时路径
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- 运行时音频 FIFO：`/tmp/umi_audio_pipe`
- 音频临时目录：`/tmp/umi_audio`
- 关机触发文件：`/tmp/umi_shutdown_request`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- 运行日志：`/tmp/umi_sys_<device_sn_lower>_<YYYYMMDD>.log`
- 数据盘日志镜像：`/mnt/data_disk/logs/umi_sys_<device_sn_lower>_<YYYYMMDD>.log`
- stereo daemon 状态：`/tmp/umi_stereo_camera_status.json`
- stereo daemon 控制：`/tmp/umi_stereo_camera_control.json`
- `/mnt/data_disk` 只作为固定挂载点使用：安装阶段会预创建为 `root:root 0555`，业务不会把本地空目录当成数据目录；只有真实数据盘挂载成功后才允许继续启动录制服务。
- 运行日志维护当前参考 V1 口径：本地先写 `/tmp`，在视频停录、音频停录和运行时退出时增量同步到 `/mnt/data_disk/logs`，并只保留当天同 SN 日志。

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

### 8.5 硬件健康监控
- `record_runtime` 当前参考 v1 口径保留低频硬件健康监控，约每 `1s` 检查一次关键硬件状态，而不是在主循环里做高频主动轮询。
- 当前监控项包括：`/mnt/data_disk` 是否仍可写、8 路相机设备节点、左右 IMU/encoder 设备节点、stereo daemon `ready/not-ready` 状态，以及左右 HMI 串口是否仍连接、输入侧 HMI 是否持续有响应。
- 发现磁盘异常时进入 `ERROR_1`；发现关键设备节点缺失、HMI 断连或 HMI 长时间无响应时进入 `ERROR_2`，并通过音频守护进程播报 `error`。
- 若异常恢复：录制中仅恢复 `RECORDING` 灯效，不打断当前录制；空闲态恢复 `READY` 并补播 `ready`。

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
- 自动安装名单：
  - `ugripper-usb-updater`
  - `ugripper`
  - `bluetooth-gatt-server`
  - `databot-device-joint`
  - `device-ota-mender`
- updater 自升级：根目录放置 `ugripper-usb-updater*.deb`；若本次先装的是新版 updater，安装后的新脚本会在同一次插盘流程里继续按最新名单扫描剩余 `.deb`。
- 主包升级：根目录放置 `ugripper_*_arm64*.deb`。
- 配置导入：根目录 `config.txt`。
- 标定数据导入：`ugripper_calib/<DEVICE_SN>/`，通过文件名后缀 `_left` / `_right` 区分左右主相机 `camchain`；对应夹爪侧 RGB/stereo/IMU payload 则按现场读出的 gripper SN 文本在该目录下递归匹配，优先 `.bin`，其次包含 `summary/imucam` 关键词的 `.md` 或当前 raw 目录（`rgb_video_ros-camchain.yaml + output-results-imucam.txt`），并按 `rgb_video_ros_imucam_parameter_summary.md` 口径生成 `1024-byte` 对齐数据结构；当前固件写入时固定补满 `64 x 16B` 传输窗口。
- encoder 零位校准触发：根目录 `calibration.txt`。
- 安装完成提示音：当本次 U 盘里实际需要升级的目标软件包全部处理结束后，`usb_auto_update.sh` 会直接从新安装的 `/opt/ugripper/audio*/upgrade_completed.wav` 里选取对应语言资源，并以 `paplay` + PulseAudio 播放升级完成提示音；当前不要求自动安装名单里的包必须全部同时出现在 U 盘。若现场没有可用 PulseAudio sink，则只记日志，不把安装流程判失败。

### 9.4 U 盘同次插入顺序
当同一次 U 盘插入同时包含 `config.txt`、`ugripper_calib/` 和 `calibration.txt` 时，当前顺序是：
1. 导入 `config.txt`
2. 按 `_left` / `_right` 文件后缀识别需要导入的左右标定目标，并逐侧读取现场 gripper SN、匹配对应夹爪标定 payload
3. 只有当本次目标侧都完成 gripper SN 匹配后，才开始写入对应夹爪的整套 RGB/stereo/IMU 标定；任一侧匹配失败或写入失败时，本次不会更新持久化 `calibration.json`
4. 左右夹爪标定都写入成功后，再原子替换持久化 `calibration.json` 中本次目标侧的主相机、stereo 与 IMU payload 子集；缺失的一侧保持原值
5. 若存在 `calibration.txt`，则跳过中间重启，直接进入左右编码器并行校准流程
6. 若未触发 `calibration.txt`，则开始按固定名单依次处理 `ugripper-usb-updater`、`ugripper`、`bluetooth-gatt-server`、`databot-device-joint`、`device-ota-mender` 的 `.deb`；每个包都会先按 Debian 包名读取版本，U 盘内若存在多个候选文件则取最高版本，已安装版本不低于 U 盘版本时跳过
7. 若 updater 在第 6 步先完成自升级，则安装后的新脚本会在同一次插盘流程里继续执行剩余自动安装名单，避免必须二次插盘才能让新名单生效
8. 全部目标软件包安装完成后，若新主包已提供 `upgrade_completed.wav` 且现场存在可用 PulseAudio sink，则播放升级完成提示音
9. 恢复 `ugripper.service`

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

### 9.7 主包打包约束
- 主包当前仍直接携带项目内 `.venv` 与 `.venv/.python-runtime`，部署后继续以 `/opt/ugripper/.venv/bin/python3` 作为首选解释器入口。
- `build_deb.sh` 的 staging 目录默认按增量方式复用：项目主体与 `.venv` 分开同步，避免每次打包都先删除再完整重拷 `.venv`。
- `build_deb.sh` 默认 `dpkg-deb` 压缩口径为 `xz -1`，兼顾构建速度与包体积；`build_deb.sh -q` 仍跳过 C++ 编译，并沿用同一默认压缩口径。如需在速度与包体积之间切换，可通过 `DPKG_DEB_COMPRESSOR`、`DPKG_DEB_LEVEL`、`DPKG_DEB_STRATEGY`、`DPKG_DEB_UNIFORM_COMPRESSION` 覆盖默认参数。
- 若当前 worktree 未自带 `.venv` 或 `build`，打包脚本可通过 `PACKAGED_VENV_SOURCE`、`PACKAGED_BUILD_DIR` 复用外部已有产物；若最终 `.venv` 来源不存在，脚本会同步移除 staging 中旧的 `.venv`，此时包仍可生成，但不再满足部署后直接运行的交付约束。

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

### 10.4 运行日志
```bash
tail -n 200 /tmp/umi_sys_<device_sn_lower>_$(date +%Y%m%d).log
tail -n 200 /mnt/data_disk/logs/umi_sys_<device_sn_lower>_$(date +%Y%m%d).log
```

### 10.5 设备映射优先检查
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

### 10.6 快速定位建议
- 不能启动：先看 `ugripper.service` 日志和 `record_runtime` 是否成功拉起。
- 能录不能停：优先检查 HMI 按键事件、状态机和 `sensor_recorder` / `camera_recorder` 退出路径。
- 少文件或校验失败：先核对八路视频、双 MCAP、`metadata.json`、`calibration.json` 是否完整；若已进入 `ERROR_1`，优先查看 episode 下的 `validation_error.log`。
- tactile serial 不对：先分别用 `udevadm info --attribute-walk --name=/dev/left_tcam_l`、`/dev/left_tcam_r`、`/dev/right_tcam_l`、`/dev/right_tcam_r` 向上核对 USB `ATTRS{serial}`，再对比 episode `calibration.json` 中 `observation.images.left_tcam_l.serial`、`left_tcam_r.serial`、`right_tcam_l.serial`、`right_tcam_r.serial`；当前口径只修正 episode，不回写 `/etc/ugripper/config/calibration/calibration.json`，且不再输出 `gripper_left_tactile` / `gripper_right_tactile` 两个旧键。
- 进入错误灯效但录制进程还活着：优先检查 `/mnt/data_disk` 是否仍可写、关键 `/dev/*` 设备节点是否还在，以及 HMI 是否持续响应。
- 音频异常：优先看 USB 耳机枚举、PulseAudio sink/source、`module-suspend-on-idle` 是否已在初始化阶段被卸载。
- 运行日志缺失：先看 `/tmp/umi_sys_<sn>_<date>.log` 是否生成，再看 `/mnt/data_disk/logs/` 是否存在当天镜像，最后核对 `/mnt/data_disk` 是否仍是真实可写挂载点。
- U 盘流程异常：先看 `/var/log/ugripper/usb_auto_update.log`，再区分是包安装、配置导入、标定导入还是 encoder 校准失败。

### 10.7 tactile serial 定向核对
```bash
udevadm info --attribute-walk --name=/dev/left_tcam_l | sed -n '/SUBSYSTEMS==\"usb\"/,/ATTRS{serial}/p'
udevadm info --attribute-walk --name=/dev/left_tcam_r | sed -n '/SUBSYSTEMS==\"usb\"/,/ATTRS{serial}/p'
udevadm info --attribute-walk --name=/dev/right_tcam_l | sed -n '/SUBSYSTEMS==\"usb\"/,/ATTRS{serial}/p'
udevadm info --attribute-walk --name=/dev/right_tcam_r | sed -n '/SUBSYSTEMS==\"usb\"/,/ATTRS{serial}/p'

python3 - <<'PY'
import json, pathlib
path = pathlib.Path('/mnt/data_disk/<device_sn_lower>/data/episode_<date>_<index>/calibration.json')
data = json.loads(path.read_text())
for key in [
    'observation.images.left_tcam_l',
    'observation.images.left_tcam_r',
    'observation.images.right_tcam_l',
    'observation.images.right_tcam_r',
]:
    print(key, data[key]['serial'])
PY
```

### 10.8 仓库内测试脚本
- 仓库根目录下的 `test/scripts/` 用于存放仓库侧、现场定向验证用脚本，不属于 `ugripper.service` 默认运行链路。
- 当前目录包含：
  - `camera_test.sh`：直接拉起多路 `ffmpeg` 验证非主相机录制稳定性，并记录运行态信息。
  - `camera_crash_capture.sh`：在 `camera_test.sh` 基础上额外持续抓取内核日志、进程、中断与内存信息，适合定位 crash / hang。
  - `testVideoPipe.sh`：快速枚举指定 `/dev/video*` 节点的视频格式能力。
- 建议从仓库根目录显式执行 `bash test/scripts/<script>.sh`；详细参数与注意事项见 `test/README.md`。

## 11. 当前使用注意点
- `config/camera_recorder.yaml` 当前 8 路配置全部默认启用；若现场需要裁剪录制集合，应明确同步调整 `record_runtime` 的 `--only` 参数与 episode 校验清单。
- 仓库内仍有部分后处理脚本依赖旧输出命名或旧假设，不能默认视为当前主链路的一部分。
- `test/scripts/` 下的脚本属于仓库侧辅助工具，默认不随主包安装；若现场需要长期保留，应明确同步部署方式与使用说明。
- `info.json` 会生成，但当前不属于最小强校验集合。
- 现场调试若需要同时访问不同子网设备，应由系统侧或人工维护额外地址/路由；主包不负责托管这些网络策略。
- 校准、导入与恢复动作仍分散在多个 shell 脚本和 service 中；当前功能可用，但维护时需要注意入口分散。
- 若设备已被错误部署且不希望覆盖当前 SN，可在交付侧仓库执行 `../firmwareburner/repair_v2_env.sh`，该脚本会重装环境与 `ugripper` 并修复持久化 `calibration.json`。
