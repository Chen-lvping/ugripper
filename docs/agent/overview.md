# Ugripper V2 Overview

本文档是当前仓库在交付、开发、排障和验收时的**唯一主说明**。
它只描述当前仍成立的系统实现、运行方式、部署入口和运维口径；不再拆分独立的运维文档、差异文档或遗留清单文档。

## 1. 当前系统一句话
- 当前系统默认以**单机双手、本地控制**方式运行。
- 主控制链路当前以 `run_record.sh -> /opt/ugripper/bin/UgripperRuntime/UgripperRuntime` 作为安装入口。
- 默认录制产物为 **2 路主相机 + 2 路 stereo + 4 路触觉相机 + 左右两份传感器 MCAP**。
- HMI 按键、RGB 灯效、提示音、pre/post 音频录制、停录校验，以及右手双键关机/左手双键卸载数据盘请求都已纳入当前运行时。
- U 盘流程统一负责 `deb` 升级/重装、`config.txt` 导入、标定数据导入和 encoder 零位校准触发；当前自动安装名单已覆盖 `das-usb-updater`、`ugripper`、`bluetooth-gatt-server`、`databot-device-joint` 与 `device-ota-mender`，deb 安装窗口会复用 HMI 灯效提示安装中/失败/完成状态，并会在整批安装完成后通过 PulseAudio 播放 `upgrade_completed.wav`。

## 2. 安装布局与入口
- 安装目录：`/opt/ugripper`
- 主服务：`pack_script/ugripper.service`
- 主入口：`/opt/ugripper/run_record.sh`
- 主运行时：`/opt/ugripper/bin/UgripperRuntime/UgripperRuntime`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- 持久化标定目录：`/etc/ugripper/config/calibration`
- 日志目录：`/var/log/ugripper`
- 运行日志镜像目录：`/mnt/data_disk/logs`
- 终端硬件检查：`/usr/local/bin/hws`
- U 盘升级入口：`auto_update/usb_auto_update.sh`

## 3. 模块总览
| 模块 | 入口 | 当前职责 | 关键输入/输出 |
| --- | --- | --- | --- |
| systemd 主服务 | `pack_script/ugripper.service` | 以 `ubuntu` 用户拉起录制服务 | `/opt/ugripper/run_record.sh` |
| 薄壳启动脚本 | `run_record.sh` | 切到安装目录、维护本地 `/tmp` 到 `/mnt/data_disk/logs` 的增量日志同步，再拉起 `record_runtime`；数据盘等待由 runtime 处理 | `/tmp/umi_sys_<sn>_<date>.log`、`/mnt/data_disk/logs/umi_sys_<sn>_<date>.log` |
| 主运行时 | `bin/UgripperRuntime/UgripperRuntime` | HMI 按键状态机、软件录制控制、LED 灯效、提示音、pre/post 音频、camera/sensor 子进程管理、停录校验、关机请求 | episode 目录、`/dev/shm/ugripper/umi_record_control.pipe`、`/tmp/umi_shutdown_request` |
| 安全 NTP 同步 | `time_sync/safe_ntp_sync.sh` / `ugripper-ntp-sync.service` | 开机或安装后只在非录制态执行一次性时间同步，优先使用板端现有 `sntp -S`，再 fallback 到 `ntpd -q -g` / `timedatectl`；同步完成、超时或录制开始后停止常驻 NTP 服务 | `/tmp/umi_recording.lock`、`sntp`/`ntp` |
| 相机录制 | `bin/CameraRecorder/CameraRecorder` | 普通录制模式下负责主摄/触觉会话录制；`--stereo-daemon` 模式下负责双目常驻预热、热插拔恢复与 session finalize | 8 路 `mkv`（默认） |
| 传感器录制 | `bin/SensorRecorder/SensorRecorder` | 录制左右 IMU/encoder，按“采样入队 + 每侧独立 MCAP 写线程”分别输出 MCAP | `sensor_left.mcap`、`sensor_right.mcap` |
| Ego 联动采集 | `bin/UgripperRuntime/ego/ego_recording_worker.py` + `bin/UgripperRuntime/adb/adb` | 录制起停阶段通过内置 ADB 联动 SXR ego app，起录前同步 ego 系统时间并按增量方式同步 ego episode 到当前 UGripper episode；当前不主动启动 app、不参与强校验 | `ego/`、`ego/ego_sync.json` |
| HMI 类库 | `standalone/GripperHmiTool` | 读取夹爪按键，并在驱动内部以单线程 owner 线程完成状态查询、灯效生成与 RGB 指令发送；默认由状态机切灯效，必要时仍可直接下发 RGB；当前也提供 SN 与 1024-byte 标定参数读写 API | 按键快照、RGB 指令、SN/标定参数读写 |
| 音频播放 | `bin/UgripperRuntime/audio/audio_play.py` | 优先绑定受支持 USB 耳机、无耳机时回退系统默认声卡；播放提示音并处理耳机 HID 音量键；初始化阶段受控处理 idle suspend | `/dev/shm/ugripper/umi_audio_pipe` |
| 音频采集 | `audio/record_usb_audio.py` | 优先从受支持 USB 耳机麦克风录音，无耳机时回退系统默认 source，供 pre/post 处理链路使用 | 临时 wav 文件 |
| 数据盘挂载 | `config/99-fixed-usb-map.rules` | 限定允许物理 USB 口，使用独立 mount helper 将数据盘挂到 `/mnt/data_disk`；主包只负责挂载/卸载，不再直接拉起 updater | `/mnt/data_disk` |
| 硬件健康检查 | `scripts/hws` / `/usr/local/bin/hws` | 一行命令列出左右手全部传感器、胸部相机与数据盘 symlink 的在线状态，便于现场快速确认是否全部在线 | 终端状态表 |
| USB 导入/升级 | `auto_update/usb_auto_update.sh` | 处理 `deb` 升级/重装、配置导入、标定数据导入、encoder 校准触发；当前会按固定名单自动安装 `das-usb-updater`、`ugripper`、`bluetooth-gatt-server`、`databot-device-joint`、`device-ota-mender`，只要 U 盘根目录存在名单内包就执行安装，同版本也会强制重装；deb 安装窗口会显示 `CALIB_RUN` 黄灯快闪且至少保持 `3s`，任一包失败显示 `ERROR_1` 红灯 `3s`，全部包处理成功显示 `CALIB_DONE` 绿灯 `3s`；全部安装完成后再通过 PulseAudio 播放 `upgrade_completed.wav`；其中标定导入会同时刷新主机侧主相机参数和夹爪侧 RGB/stereo/IMU payload。该脚本及其 `/usr/local/bin` + systemd unit 触发链归属可选独立包 `das-usb-updater` | `/etc/environment`、`calibration.json`、夹爪 HMI |
| 校准执行 | `auto_calibration/run_calibration.sh` | 在 `calibration.txt` 存在时停止业务、复用 `/mnt/data_disk` 触发左右编码器并行 zeroing、恢复服务 | `bin/SensorRecorder/zeroing` |

## 4. 启动链路
1. `pack_script/postinst` 在安装或升级 `ugripper` 时会尝试把 `/home/user/lib/exfat.ko` 复制到 `/lib/modules/<kernel>/extra/`，写入 `/etc/modules-load.d/ugripper-exfat.conf`，并在安装当次立即尝试加载 `exfat`。
2. 后续开机时，`systemd-modules-load` 会优先加载 `exfat`；允许的 USB 数据盘分区再由 `config/99-fixed-usb-map.rules` 按物理 USB 父链路匹配，在 `add/change` 事件里触发 helper 挂载到 `/mnt/data_disk`。
3. systemd 启动 `ugripper.service`。
4. 服务进入 `/opt/ugripper/run_record.sh`。
5. `run_record.sh` 读取 `/etc/environment` 中的 `DEVICE_SN`，建立当天运行日志文件名 `umi_sys_<device_sn_lower>_<YYYYMMDD>.log`。
6. `run_record.sh` 检查 `./bin/UgripperRuntime/UgripperRuntime`，然后等待 `/mnt/data_disk` 成为真实可写挂载点。
7. 数据盘可写后，`run_record.sh` 启动 v1 风格日志维护：业务 stdout/stderr 先落到 `/tmp`；当收到视频停录、音频停录请求或运行时退出时，再按增量方式同步到 `/mnt/data_disk/logs`，并清理本地与数据盘上同 SN 的非当天日志。
8. `run_record.sh` 再拉起 `record_runtime`；运行时退出前会额外补一次日志同步。
9. `record_runtime` 启动后读取 `/etc/environment`，至少关注：
   - `DEVICE_SN`：决定数据目录。
   - `CAMERA_CODEC`：非法值会回退到 `h264`。
   - `UGRIPPER_LANG`：决定提示音语言。
10. 运行时连接左右夹爪 HMI、启动 LED 渲染线程、尝试拉起音频守护进程。
- HMI 状态查询与 LED RGB 下发会在单个 gripper 串口内由驱动 owner 线程统一调度；`record_runtime` 只切换灯效模式，不再跨线程推送录制态的 500ms 亮灭边沿。当前驱动参考旧版 `driver_origin/led_manager` 的职责分离思路做了收敛：按键输入仍以 HMI 主动上报 `KeyReport` 为主，串口主动查询已降为约 `1s` 一次的低频探活/状态刷新；LED 仅在颜色变化、状态切换或低频补发时下发，避免高频状态查询与 RGB 指令互相抢占；若有专项诊断或直控需求，仍可走 direct RGB 通道覆盖当前效果。
- `standalone/GripperHmiTool` 当前新增了 UMI SN / 标定参数协议封装：SN 固定为 32-byte 字段（当前现场 SN 文本示例为 16-char，尾部补 `0x00`），标定参数固定为 `1024 byte` 严格对齐结构；当前 payload 已覆盖 RGB 主相机、双目 `cam0/cam1`、`cam->imu` 外参、IMU 离散噪声/随机游走与残差统计，其中 header 会保留内部有效数据长度，但当前 `V1.1` 固件写入时仍必须补满 `64 x 16B` 数据包，具体协议见 `docs/umi_calibration_protocol.md`。
- UMI 标定写入当前增加了异常恢复口径：若写入阶段收到 `0xFE`（当前 chunk 零数据校验错误），驱动会优先重发当前 chunk；若连续出现 `0xFE`，或后续读 SN / 读标定返回 `0xF3/0xFE`，则会发送一次 `AbortWriteInData` 清理固件残留写入状态后再重试。
11. 初始化成功后进入空闲状态机；只有数据盘和 Fays recorder 都 ready 时才进入 `READY` 绿灯并接受右手夹爪短按起录，仍在初始化等待时保持 `INIT` 蓝灯闪烁。
12. `record_runtime` 初始化阶段会额外拉起一个常驻 Fays stereo daemon；该 daemon 分别以左右配置启动两份 Fays recorder，通过 `/tmp/umi_stereo_camera_status.json` 暴露 `ready/not-ready` 状态。当前 `ready=true` 需要左右 recorder 进程/FIFO 在线、左右 calibration 有效、左右 SDK serial 非空且不同、左右 stereo/IMU symlink 实时存在；Fays SDK 仍按其限制使用启动时解析出的 `/dev/videoN` 端口，但 handle 创建后会复查 symlink 目标，若初始化期间 videoN 漂移则让当前 recorder 失败退出并由外层 daemon 重启。服务启动或数据盘晚就绪后，Fays recorder 尚未达到 `ready=true` 时会保持 `INIT` 蓝灯闪烁等待且短按起录会被忽略；该启动等待窗口最长约 `40s`，超时后沿用健康监控的 `stereo_status_missing` / `stereo_not_ready` 等路径进入 `ERROR_2`。外层 daemon 还会按低频检查每侧 recorder 工作状态：进程、FIFO、calibration/serial、stereo/IMU symlink 和最近实际 warmup frame 刷新任一异常都会按单侧重启；若异常发生在录制中，则只记录当前 session 错误并等待停录快速失败，不把重连后的设备续写回本条 episode。若左右 serial 重复，则重启两侧。左右 calibration JSON 由对应的常驻 recorder handle 写出，运行时 frame 刷新状态写到 `/dev/shm/umi_left_fays_runtime_status.json` / `/dev/shm/umi_right_fays_runtime_status.json`，不在外层脚本中额外创建短生命周期 SDK probe；已有 calibration JSON 只有在 `device_info.serial_number` 与当前 recorder handle 读到的 SDK serial 一致时才复用，否则启动时会丢弃旧 JSON 并从当前 handle 重新拉取标定，避免热插拔或 videoN 漂移后视频与旧标定错配。Fays `VideoFrameQueue` 默认容量为 32，ffmpeg rawvideo 输入 `thread_queue_size` 为 64，用于限制 raw frame 队列内存占用。
- Fays stereo daemon 在同一轮维护中会分别拉起可用侧的左右 recorder，不再等待单侧 SDK startup complete 后才启动另一侧；左右 recorder 的最终 ready 状态继续由运行态健康检查收敛。
13. `record_runtime` 当前按 recorder 进程组而不是单一父 PID 回收 `camera_recorder` / `sensor_recorder`；当停录或异常收尾时，会向整组发送退出信号，降低内部 `ffmpeg`/`gst` 子进程残留导致后续卡死的概率。
14. `ugripper-ntp-sync.service` 属于独立 oneshot 辅助服务：安装后和开机后异步启动，不阻塞 `ugripper.service` 主链路；脚本先停止 `ntp.service` / `chrony.service` / `systemd-timesyncd.service` 这类常驻校时服务，再检查 `/tmp/umi_recording.lock`，录制中直接跳过，空闲时优先用 `sntp -S` 做一次性校时，再 fallback 到 `ntpd -q -g` 或 `timedatectl`。同步完成、超时、失败或检测到录制开始后都会再次停止常驻 NTP 服务，避免录制时间轴被系统时间校准跳变影响。

## 5. 状态机与按键行为
### 5.1 空闲态与阈值
- `READY`：绿色呼吸灯，提示系统可开始录制。
- 当前 `READY` 呼吸灯周期约 `4.5s`，LED 渲染线程约每 `20ms` 按单调时钟刷新一次；驱动只在亮度实际变化时发送 RGB，并以约 `250ms` 的低频做灯效补发、约 `1s` 的低频做串口探活，降低肉眼可见抖动和录制态丢闪。
- 主循环轮询周期约 `20ms`。
- HMI 按键原始状态在进入动作状态机前，对按下和松开分别执行 `40ms` 稳定时间滤波；只有状态连续稳定满阈值才更新有效按键状态。短按事件之间仍保留 `80ms` 动作间隔限制，两层机制分别处理电平抖动与重复动作限频。
- 长按判定阈值 `800ms`，右手双键关机提示阈值 `2000ms`，双键执行阈值 `4000ms`。

### 5.2 按键动作
- `BTN_UP` 短按释放：物理按键需同键在 `1s` 内双击才触发，第一次短按只进入待确认窗口。
  - 空闲时开始普通录制。
  - 录制中停止当前录制。
- `BTN_DOWN` 短按释放：物理按键需同键在 `1s` 内双击才触发，第一次短按只进入待确认窗口。
  - 空闲且存在上一条 episode 时开始 reset 录制。
  - 空闲但无上一条 episode 时只播报 `no_reset_needed`。
  - 录制中停止当前录制。
- `BTN_UP` 长按：空闲时录制 pre audio；录制中忽略。
- `BTN_DOWN` 长按：空闲时录制 post audio；录制中忽略。
- 右手双键长按：
  - 2 秒时播放 `shutdown` 提示音。
  - 4 秒时进入 `EXIT`，必要时先停录，然后写 `/run/ugripper/system_action_request=shutdown`。
- 左手双键长按：
  - 仅在停止录制状态下生效；录制中忽略并播报 `error`。
  - 4 秒时先触发 `writing` 并刷写运行日志，然后写 `/run/ugripper/system_action_request=umount`。
  - root helper 卸载 `/mnt/data_disk` 成功后播放 `umount`；失败播放 `error`。
- 左手单键长按：
  - 仅空闲态生效，将上一条完成 episode 的 `metadata.json` 原子更新为 `quality_check_status=fail`、`quality_check_err_type=operator_marked_failed`，并写入 `validation_error.log`。
  - ACI episode 保持四个独立 sensor/Fays MCAP，不向这些数据 MCAP 注入 metadata topic。
  - 标记成功后紫灯按两次 `120ms` 脉冲闪烁，并与左右蜂鸣器两次提示同步，结束后恢复当前空闲灯效。

## 6. 录制生命周期
### 6.1 开始录制
1. 在 `/mnt/data_disk/<device_sn_lower>/data` 下创建新的 `episode_YYYYMMDD_NNNN-temp`；目录名带 `-temp` 表示采集中或停录收尾中。
2. `metadata.json` 不在起录阶段预写；停录完成媒体 finalize、强校验和轻量探测后，才一次性写入最终结构，确保 `collection_duration_s`、`video_details[].duration_s/fps/start_offset_us` 与质量检查结果来自本次最终数据。
3. 一进入起录流程就写入 `/tmp/umi_recording.lock`，内容包含 `record_runtime` 的 `pid`、episode 目录（创建前可为空）与起录时间；episode 目录创建后会更新锁内容。该锁会覆盖 episode 准备、recorder 启动、录制、停录写盘和校验阶段，供 NTP 同步等系统级辅助动作避让录制窗口。若准备或启动失败会立即清锁；停录收尾完成后清锁。
4. 写入 `calibration.json`：当前不再从旧持久化 `calibration.json` 迁移或清洗后输出，而是按运行时设备缓存、Fays daemon 标定状态和 tactile USB serial 运行时缓存从零组装，避免旧标定字段、旧命名或旧表述混入 episode。`calibration.json` 的输出格式当前已锁定：
   - 这是锁定格式，顶层字段集合不得增加，已有字段的职责不得漂移；若必须调整，必须先更新本节文档，再同步修改生成代码、持久化刷新逻辑与 episode 校验口径。
   - 顶层只保留 `calibration_info/observation`。
   - `calibration_info` 只保留 `calibration_status/format_version`，其中 `format_version=3.0`。
   - `observation.images` 使用新范本 key：`cam_left_main/cam_right_main/cam_chest_main/stereo_left/stereo_right/tcam_left_l/tcam_left_r/tcam_right_l/tcam_right_r`。
   - `observation.imu` 使用 `imu_left/imu_right`。
   - 主摄字段只保留 `camera_model/distortion_coeffs/distortion_model/fps/intrinsics/names/shape`；缺缓存时写默认占位，停录强校验再判失败。
   - tactile 字段只保留 `names/serial/shape`，serial 来自运行时在 `/dev/tcam_*` 插入或 symlink 目标变化时维护的 USB sysfs serial 缓存；episode 生成只消费缓存，不再停录阶段额外执行 `udevadm` 探测。
   - stereo 字段按范本保留 `cam0/cam1/camera_model/distortion_model/extrinsics/fps/names/shape`；IMU 字段按范本保留 `accelerometer/gyroscope/update_rate_hz`。
   - 左右主摄、胸部主摄 SN 与主摄标定参数由运行时在相机 symlink 插入/目标变化时通过 Yuzhou UVC XU 异步刷新缓存，拔出时清空；episode 生成只消费缓存，不再停录后同步读 XU。若在线主摄缺少合法 `FE...` SN 或 `MCAL` 标定 payload，本条 episode 会在停录校验阶段失败。
   - 左右 stereo 与 IMU 标定由 Fays daemon 状态填充；不再把 gripper payload 或旧持久化 calibration 作为 episode `calibration.json` 的来源。
5. `record_runtime` 不再产出最终 `info.json`；camera/stereo 录制链路只在 `/dev/shm/ugripper_recording_timing_*.json` 维护内部 timing 缓存，用于 stereo offset 合并、并行视频探测结果缓存和最终 `metadata.json.video_details[].start_offset_us/duration_s` 生成，episode 完成 rename 前会清理该内部文件。停录硬校验只面向最终媒体、MCAP、`calibration.json` 和最终 `metadata.json` 语义，不再把该 shm 缓存作为 episode 产物或硬校验对象：
   - 内部文件顶层临时保留下面这些字段；`stereo_session` 可作为双目会话摘要存在，其他临时调试字段不得写入。
   - `boot_time_offset`
   - `boot_time_offset_us`
   - 8 路基础 `<camera>_record_time_offset_us`，启用 `chest_cam_main` 时额外包含该路 offset。
   - `video_probes`：停录校验阶段并行 `ffprobe` 后写入的内部缓存，仅供同次 `metadata.json` 生成复用，不进入最终 episode。
   - 若现场日志出现 timing 打开失败，优先按日志里的真实 `/dev/shm/ugripper_recording_timing_*.json` 路径排查；旧字符串 `.recording_timing.json` 只保留为兼容性提示，不代表当前会在 episode 目录生成该文件。
6. 若已准备 pre audio，则移动到本次 episode 的 `audio_pre.wav`。
7. 起录前 `record_runtime` 会先用随包 ADB 做一次短窗口在线设备预检；若明确没有任何 `adb devices` 的 `device` 状态设备，则直接跳过 ego sidecar，避免未连接 ego 时首次录制被 ADB/SXR 识别拖慢。若预检发现设备或预检结果不确定，则保持原 ego 流程：在 gripper 相机/传感器启动前拉起 `ego_recording_worker.py start`，等待 worker 完成 ADB 检测、起录前系统时间同步、发送 `START_RECORDING` 广播且拿到本次远端 `episode_*-temp` 后再继续本机录制链路；若短等待窗口内仍未拿到远端 episode，则记录告警但仍不阻塞 UGripper 主录制。worker 默认优先使用随包安装的 `bin/UgripperRuntime/adb/adb`：设备识别要求 `ro.product.manufacturer=SXR` 且 `ro.product.model/ro.product.device/ro.product.name/ro.build.product` 均为 `SXR_1`，随后通过 `cmd alarm set-time` 对 ego 执行一次短校时，并按主机 `CAMERA_CODEC` 先向 `com.ssnwt.helloxr` 广播 `SET_VIDEO_CODEC`（`h264 -> avc`，`h265 -> hevc`），再向已运行的 app 广播 `START_RECORDING`，并把远端新生成的 `episode_*-temp` 文件扁平同步到本条 UGripper episode 的 `ego/` 目录；`ego/ego_sync.json` 只保留 ego `serial`、起停状态、远端/本地 episode、时间同步摘要、finalize 状态、视频编码设置结果和远端清理状态，时间同步或编码设置失败不阻塞主录制。当前不主动启动 ego app，找不到 ego 只写 `ego/ego_sync.json` 或日志，不阻塞 UGripper 主录制。
8. 并行启动：
   - `camera_recorder --codec <codec> --output-dir <episode> --only left_cam_main,right_cam_main[,chest_cam_main],left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r`
   - `sensor_recorder <episode_dir>`
9. 同时由 `record_runtime` 直接向左右 Fays recorder FIFO 写入本次 session `START` 命令；双目不重启采集管线，只把本次 session 窗口内的帧纳入当前 episode，由 session writer 抽帧并编码落盘。
10. 切换到 `RECORDING` 状态并播放开始提示音。

### 6.2 相机链路
- 配置入口：`config/camera_recorder.yaml`。
- 当前 YAML 定义 9 路相机：左右主摄、可选胸部主摄、左右 stereo、4 路触觉。
- `udev` 口位策略当前口径：
  - stereo 与 CH9344 串口桥允许同侧 hub 的内部端口 `.1/.2` 互换；
  - tactile `l/r` 仍按左右侧固定 kernel 路径命名，不做跨侧互换；
  - 左侧更换新 hub 后，左主摄与左触觉 `l` 不再只按 `.4.2/.4.4` 固定口位判断，当前优先按设备类型识别：`27c2:0530` 归 `/dev/cam_left`，`0bda:5846` 归 `/dev/tcam_left_l`，USB2 口位仅用于限定属于左侧链路；
  - 左手新 hub 的 tactile 口位与右手保持同构：`.3 -> /dev/tcam_left_l`、`.4 -> /dev/tcam_left_r`；同时继续兼容旧特殊 hub 的 `/dev/tcam_left_r <- .3`。
- 当前运行时默认录制全部 8 路：左右主摄 + 左右 stereo + 4 路触觉。
- 普通录制阶段的 `camera_recorder` 当前会直接起左右主摄与 4 路触觉；左右 stereo 继续由单独的 warmup daemon 常驻管理。
- 夹爪热插拔恢复完成后，`record_runtime` 当前会在该侧关键设备全部 ready、并完成 gripper runtime refresh 之后，只按 tactile 相机 USB `serial` 标记该侧实时参考帧待更新；服务初始化阶段不会直接打开触觉相机抓参考帧。待更新实时参考帧统一由后续首个可用 episode 的触觉视频截帧生成，若该 episode 缺失、损坏或截帧失败则顺延到下一条 episode。持久化 baseline 不因夹爪重连刷新，避免覆盖关机期间发生的盖板损坏。
- warmup daemon 在空闲态持续常驻打开需要预热的相机设备；当前仅左右双目继续消费 `MJPEG 60fps` 预热流。开始录制时只为 stereo 新建 session writer，把会话窗口内帧写入最终 `mkv`；主摄则在普通录制阶段直接冷启动采集并写入最终文件。
- warmup daemon 当前按单实例口径运行；若服务内 daemon 尚未退出又手工再起第二个 `camera_recorder --stereo-daemon` 去抢同一批双目设备，可能诱发设备忙、节点缺失或整条 USB 链路重枚举。当前实现已增加 `/tmp/umi_camera_warmup_daemon.lock` 单实例锁，第二个 warmup daemon 会直接拒绝启动。
- 停录阶段也会并发向各路相机子进程发 stop，并在全部 stop 返回后统一 poll 状态，降低多路顺序收尾导致内部 timing 文件缺失或容器未 finalize 的风险。
- `camera_recorder` 当前对 ffmpeg 子进程采用统一的正确口径：录制器对象逐路启动，但每路采集/编码仍在各自子进程或内部线程里并发运行；普通触觉/主摄 shell recorder 与 stereo session ffmpeg 都会保留独立进程组，供正常 stop 路径按组发信号；同时启用父进程死亡自动终止保护，避免 `camera_recorder` 本体异常退出后遗留孤儿 `ffmpeg` 长时间占住 `/dev/left_tcam_*`、`/dev/right_tcam_*`，也避免从短生命周期启动线程里 `fork()` 导致 `PR_SET_PDEATHSIG` 被误触发。直接 `fork/exec` 的子进程在标准流重定向完成后会关闭其余 fd；Fays daemon 启动左右 recorder 时也会关闭 daemon 顶层控制 FIFO，使 recorder/ffmpeg 只持有自身实际需要的本侧 Fays 设备、控制与输入输出资源，不继承 CH9344、其他相机或其他侧硬件句柄。
- 主相机模式是压缩码流直封装：主摄始终走相机原生 `H.264/H.265` 码流，不做二次编码；当前实现已收口为 `camera_recorder` 进程内的 `V4L2 MMAP capture -> appsrc -> h26xparse -> matroskamux -> filesink`。`VIDIOC_STREAMON` 后会先检查压缩 access unit 的 NAL 结构，只在读到包含参数集与 IDR 的 clean keyframe 后开始写入 `appsrc`，超出最大等待帧数或约 `1.5s` 仍未满足则明确报错；若 EOS 已在轮询路径中被消费，停录线程会识别该状态并跳过重复 bus 等待。
- 主相机时间戳当前优先取 `VIDIOC_DQBUF` 返回的 `v4l2_buffer.timestamp`，若驱动标记为 monotonic 则在进程内通过 `boot_time_offset_us` 转成 unix 时间；这样 `system_time_us` 的打点位置尽量前移到内核缓冲出队附近，而不是依赖后置日志解析。
- 主相机 `PTS/DTS` 当前按“相对首帧 system time 的增量”在进程内生成，并做单调钳制；内部 `<camera>_record_time_offset_us` 的语义保持为 `first_frame_unix_time_us - first_frame_pts_us`，停录后折算进 `metadata.json.video_details[].start_offset_us`。
- 主相机链路当前使用可配置的 V4L2 MMAP 压缩缓冲池，`v4l2_buffer_count` 默认按 `16` 个 buffer 的折中口径运行，避免回退到历史上更容易触发主摄短时背压/坏流的 8-buffer 口径，同时降低 RK3588 上多路相机反复起停时的 CMA/连续 DMA 内存压力；`appsrc`/`queue` 仍在背压时阻塞等待，避免下游繁忙时主动丢弃编码包。
- 录制中 `CameraRecorder` 若在左/右/胸部主摄启动阶段拿到 V4L2/USB 硬错误（例如 `VIDIOC_*`、`Input/output error`、设备消失或超时），会在 `/dev/shm/ugripper_camera_recorder_fault_<parent>_<episode>.json` 写入一次性故障证据；`record_runtime` 读到后立即按主摄 V4L2/USB 启动故障归类到 `ERROR_2`，中断当前录制并触发现有 USB 复位流程，避免等到停录校验阶段才因 timing/media 缺失落到 `ERROR_1`。主摄录制窗口达到约 `5s` 后，若输出帧跨度仍不超过约 `2s` 或帧数明显过少，也会写入 `main_camera_short_stream` 故障并按同一路径处理；用户在 `5s` 内快速启停不会触发短流判定。若 ioctl 卡在内核而应用层没有返回错误，`record_runtime` 还会只读检查 `CameraRecorder` 进程的 `/proc/<pid>/task/*/stat`；线程连续处于内核 `D` 状态超过 `5s` 时，同样按主摄 V4L2/USB 内核等待故障进入 `ERROR_2`。该检查不打开相机设备、不占用 V4L2 fd。
- 主摄 YAML 不再支持 `uvc_roll_absolute`。主摄厂商扩展控制工具 `main_camera_xu_tool` 与 `ensure_main_camera_packet_size_once.sh` 当前随包保留，但 udev 自动触发暂时停用；普通录制阶段不做 UVC 控制写入。`main_camera_xu_tool` 支持 `--read-sn` 仅读取 SN 所在 chunk、`--read-calib`/`--validate` 仅读取当前 V1 标定结构所需前 10 个 chunk，用于快速验证 SN 与 `MCAL` 标定是否可读。
- 触觉 / 双目模式保留 `hybrid-decode-encode` / `stereo-hybrid-decode-encode`。
- stereo 当前默认按设备 `1280x400@60` 常驻采集 MJPEG，session writer 按 `30fps` 抽帧后再编码成 `H.265` 写入 `mkv`；当前不再依赖后台 live encode + UDP relay。
- 每路相机由独立子进程承载；单路失败不会由 `camera_recorder` 主动连带停掉其他相机。
- stereo 热插拔语义：
  - 单侧设备缺失时只停止该侧 Fays recorder，保留另一侧 recorder 与 daemon 主循环；状态降为 `not-ready`，双手模式下仍要求左右都 ready 才允许录制。
  - 设备重新枚举后自动重建该侧 MJPEG warmup 取流并重新进入预热态；若掉线发生在录制中，则当前 stereo session 会被标记失败并在停录阶段显式报错，避免静默产出错位文件。
  - recorder 会低频刷新 `/dev/shm/umi_left_fays_runtime_status.json` / `/dev/shm/umi_right_fays_runtime_status.json`，只记录最近 warmup frame 时间、最近编码 frame 时间和当前录制 session id 等事实调试变量；daemon 用最近 frame freshness 判断 warmup 拉流是否真的存活，不额外引入“session writer 确认”状态。
  - 录制中若某侧 recorder 工作状态异常，daemon 会立即记录当前 session 的单侧 stereo 控制错误，停录阶段快速返回失败；finalize 等待视频/MCAP 文件期间也会被已知错误打断，不再等缺失文件触发完整超时。若该侧 Fays 相机/IMU symlink 本身识别不到，仍按关键设备缺失进入 `ERROR_2`；若设备已在线但 recorder/FIFO/control 链路失效，则进入 `ERROR_4` 并提示拔插对应侧夹爪，软件复位不作为有效恢复手段。
  - 每次拉起单侧 Fays recorder 前会先等待该侧 stereo/IMU symlink 解析目标稳定，随后默认再延迟 `3s`，可通过 `FAYS_STEREO_START_DELAY_SEC` 覆盖，避免设备刚枚举完成或 videoN 仍在漂移时被 SDK 过快打开。
  - 左右 Fays recorder 不并行拉起：daemon 会按左右 stereo symlink 当前解析到的 `/dev/videoN` 顺序启动，较小 videoN 视为更早插入/枚举的一侧；单侧启动后会等待 SDK handle 完成启动（FIFO 在线、calibration serial 可读且 warmup frame fresh）或默认 `10s` 超时，再拉起另一侧，可通过 `FAYS_STEREO_START_COMPLETE_TIMEOUT_SEC` 覆盖该等待窗口。
  - 单侧 recorder 清理优先走 FIFO `EXIT` 触发进程内 `Stop()`，该路径会唤醒视频、IMU、MCAP 和编码队列；若 SDK 线程仍未退出，再依次用 `SIGTERM` 和 `SIGKILL` 兜底，避免外层 daemon 因无界 `wait` 卡住。
  - Fays recorder 重启不会主动执行 USB `unbind/bind`、USB reset 或 udev 规则重载；它会关闭旧进程持有的 SDK/video/IMU handle，再重新拉起 recorder 并由 SDK 打开当前 symlink 指向的设备节点。外层 daemon 在启动/重启前会用 `fuser` 检查该侧 stereo/IMU 实际节点是否仍被占用，必要时会终止占用这些节点的进程；因此日志中可能看到 video port 占用清理，但这不是内核 USB 设备 reset。
  - Fays 相关关键事件日志统一带 `[FAYS_TS <HH:MM:SS.usec>]` 前缀，便于和 `dmesg -T` 的内核日志对齐。当前覆盖 stereo daemon 的 recorder start/stop/restart、video port busy/forced cleanup、session start/stop/finalize error，以及 `fays_record_example` 的进程启动、SDK handle 创建失败、控制 FIFO `START/STOP/EXIT` 和进程退出。
  - 最终 `stereo_left.mkv` / `stereo_right.mkv` 始终直接由录制会话写入单文件；不会在 episode 目录生成 `stereo_info.json`、`.concat.txt` 或后台 segment cache 文件。
- 内部 `/dev/shm/ugripper_recording_timing_*.json` 的时间字段由 `camera_recorder` 负责写出，而不是在停录校验阶段回填：
  - `boot_time_offset`
  - `boot_time_offset_us`
  - 8 路 `<camera>_record_time_offset_us`
  - 若写出失败，`camera_recorder` 会区分记录“打开文件失败 / flush-close 失败 / 缺少哪一路 `record_time_offset_us`”，便于直接判断是 shm 写失败还是某路 recorder 未产出 offset。
- 双目当前实现为“后台 MJPEG warmup + 录制时编码写最终文件”：空闲态不再保留 UDP / MPEG-TS live relay；按下录制后，session writer 只消费当前会话的 MJPEG 帧，抽帧后编码写入 `stereo_left.mkv` / `stereo_right.mkv`。
- stereo 内部 `<camera>_record_time_offset_us` 与主摄/触觉保持同一语义：都以“本次最终输出首个写入帧”的 `PTS -> 系统时间` 映射为准。

### 6.3 传感器链路
- `sensor_recorder` 固定录制：
  - 右手：`/dev/right_encoder`
  - 左手：`/dev/left_encoder`
- 新版不再接板载 IM648，因此 `sensor_recorder` 不再打开 `/dev/left_imu` / `/dev/right_imu`，也不再写 `imu_left` / `imu_right` topic。
- encoder 当前按“解析/读取后入本地队列 -> 主循环批量消费 -> 每侧写线程落 MCAP”的方式输出；主循环不再直接同步阻塞 `McapWriter::write`。
- 左右 `sensor_*.mcap` 现各自由单独写线程落盘，降低 chunk 压缩或磁盘抖动对采样节奏的反压影响；若写队列持续堆积，日志会输出 backlog warning 便于现场判断是否存在写盘瓶颈。
- 录制态的 IMU 超阈值检测当前直接下沉在 `sensor_recorder`：复用其现有 IMU 消费路径完成左右手独立的 `gyro`/`accel` 阈值、去抖、cooldown 与最短保持时长判定，不再把原始 IMU 样本转发给 `record_runtime`。
- `sensor_recorder` 与 `record_runtime` 当前只通过单向本地 `pipe` 交换轻量告警状态消息；`record_runtime` 不接收原始 IMU 流，只在收到“进入告警”状态变化时写一次 warning，并统一控制左右夹爪 HMI 蜂鸣。
- 输出拆成两份 MCAP：
  - `sensor_right.mcap`
  - `sensor_left.mcap`
- IMU / encoder 的 MCAP 时间戳当前默认沿用主机侧原始样本时间；若主循环一次从本地缓冲区取到多帧样本，则认为出现了缓冲区 burst，会以该批最后一帧的主机时间为锚点，按各自名义频率（IMU `200Hz`、encoder `1kHz`）向前回填这批样本的伪时间戳，尽量消除追赶帧导致的时间轴挤压。
- encoder 连接会优先在 `1Mbps` 下做 3 次快速验证重试（总验证窗口约 `150ms`，目标控制在 `200ms` 内），仍无响应才回退 `115200`；若 `115200` 可响应，则切回 `1Mbps` 后再次验证。

### 6.4 停止录制
1. `record_runtime` 会先并发向左右 Fays recorder FIFO 发送 `STOP`，尽早冻结本次双目 session 的收尾边界，避免 stop 命令在普通相机与传感器都停完之后才传到双目链路；单侧 FIFO 写入失败不会阻塞另一侧 stop 命令发送。
2. 在双侧 stop 命令发出后，`record_runtime` 并发停止普通录制模式下的 `camera_recorder` 与 `sensor_recorder`，降低两条独立链路顺序收尾带来的蓝灯等待。
3. 左右 Fays recorder 收到 `STOP` 后分别 finalize 本侧 `stereo_*.mkv` 与 `fays_data_*.mcap`；后台 warmup daemon 继续负责 recorder 常驻、健康检查与热插拔恢复。
4. 先发送 `recording_stop`，随后立即切到 `writing`；提示音采用“后触发抢占前触发”的语义，因此 `writing` 会直接打断仍在播放的上一条提示。
5. 若本条 episode 已拉起 ego sidecar，停录进入 `writing` 后会停止后台增量同步进程，向 ego app 广播 `STOP_RECORDING`，等待 ego 侧目录从 `episode_*-temp` rename 为最终目录并补齐最后一轮文件；若起录阶段还未写出本次 `remote_episode`，stop 只会从 START 前不存在的新 `episode_*-temp` 中选择，避免把 ego 设备上的历史遗留 temp 目录同步进本条 episode。随后 worker 会重新拉取 MP4/M4A finalize 后的头部 `moov` 区域并覆盖本地差异段，再修复 ego MP4 中 stop 后才可确定的 extended-size `mdat` 大小，使本地同步文件可被播放器按 box 边界读到。ego 失败当前只更新 `ego/ego_sync.json` 与日志，不改变 UGripper `quality_check_status`。
6. 进入 `writing` 阶段：切换 `WRITING` 蓝灯常亮并执行分阶段文件级 flush。所有 episode 产物都必须在所属阶段显式执行文件级 flush，再刷 episode 目录项；`pre_stereo_finalize` 只刷普通相机视频、左右 sensor MCAP、`calibration.json` 和可选音频；等待 ego/stereo finalize 并合并 session 信息后，`final` 刷 `stereo_*.mkv`、`fays_data_*.mcap` 与 `ego/` 下已同步文件，并额外刷 ego 子目录目录项；`final` flush 完成后才允许 worker 删除 ego 设备上的同名远端原始 episode，且只删除已 finalize、非 `-temp`、远端/本地文件大小一致的 `episode_*` 目录；删除结果写入 `ego/ego_sync.json.remote_cleanup` 后会再补刷 `ego/ego_sync.json` 和 `ego/` 目录项。`metadata_final` 只刷最终 `metadata.json`、失败时的 `validation_error.log` 和目录项，避免停录路径重复刷同一批媒体文件。停录收尾完成后会请求 `run_record.sh` 将当前运行日志刷写到 `/mnt/data_disk/logs/`。
7. `record_runtime` 在 writing 阶段等待左右 `stereo_*.mkv` 非空且 `fays_data_*.mcap` 完整后，本地生成本次 stereo session 摘要并并入 `/dev/shm` 内部 timing 缓存；该等待发生在原有 stereo finalize 阶段，不阻塞前面的 stop 命令并发发送。
8. 先生成最终 `metadata.json`，其中 `video_details[].start_offset_us/duration_s` 来自 `/dev/shm` 内部 timing 缓存和视频轻量探测；随后执行稳定校验，校验只读取最终 `metadata.json` 与最终媒体/MCAP/`calibration.json`，不再直接依赖 shm 缓存。视频探测结果会尽量写入 `/dev/shm` 内部 timing 缓存的 `video_probes`，供同次 metadata 生成复用；若缓存写入失败，只记录告警，不作为 episode 硬校验失败原因。
9. 停录硬校验完成后，触觉状态抽检改为后台慢校验，不阻塞当前 stop 返回，也不反改本条 episode 的 `quality_check_status`。后台任务会执行两轮轻量 tactile 处理：其一是“本次起录附近单帧 vs 同 `serial` 实时参考帧”的比较；其二是“本次起录附近单帧 vs 同 `serial` 持久化 baseline”的慢变量比较。若夹爪刚重连或实时参考帧缺失，则优先用本次 episode 的起录附近单帧初始化实时参考帧，本轮不计入实时 damaged 窗口；若持久化 baseline 缺失，则用本帧初始化持久化 baseline。若视频缺失、损坏或截帧失败，则保留待更新标记并顺延到下一条 episode。两者都不会扫描整段视频，也不会重新读取 MCAP 做 encoder 对齐。
10. 实时触觉抽检当前属于软告警而不是完整性失败：单次异常只更新该 tactile `serial` 的近期历史；当同一 `serial` 最近 `3` 个 episode 都判为异常时，空闲态切到黄灯闪烁，并播放对应 `left/right_tcam_*_damaged` 提示音。后台 tactile 校验最多保留一个待处理 episode；若下一次录制开始，会请求当前后台校验停止并清空待处理任务，避免干扰下一次录制。
11. 持久化 baseline 用于覆盖关机期间发生的盖板损坏：缺失时会由首个可用 episode 截帧初始化；已存在时不会因夹爪重连、服务重启或固定时间到期自动刷新。persistent 告警连续 `3` 个 episode 异常后置位，并记录当前系统 `boot_id`；只有该告警触发后，且设备经历一次重新开关机导致 `boot_id` 变化，下一条可用 episode 才会刷新同 `serial` 的持久化 baseline。连续 `3` 个 clean episode 会清除该 persistent 告警，但不会自动刷新 baseline。
12. 停录收尾完成后将 `episode_YYYYMMDD_NNNN-temp` rename 为 `episode_YYYYMMDD_NNNN`；无论质量成功或失败，只要收尾已完成就去掉 `-temp`，质量结果由 `metadata.json` 和 `validation_error.log` 表达。运行时内部允许同一条 episode 同时携带多个显式 `error_type`，不再从错误文本或路径猜测类别；`metadata.json.quality_check_err_type` 仍保持旧字段，只写最高优先级的一个主错误类型。完整性成功且无触觉软告警则回到 `READY` 并播放 `ready`；完整性失败进入 `ERROR_1` 并播放 `validation_failed`；关键设备/HMI/stereo 缺失或不活跃进入 `ERROR_2` 并播放 `error`；设备已识别但单侧 stereo/Fays recorder、FIFO 或控制链路失效进入 `ERROR_4` 并播放 `error`；数据盘挂载丢失 / 不可写 / 写满进入 `ERROR_3` 并播放 `error`；其他运行时异常进入 `ERROR_5` 并播放 `error`。这些后续提示同样会直接抢占当前播放中的 `writing`。

## 7. Episode 产物与检查
### 7.1 默认产物
- 视频：
  - `cam_left.mkv`
  - `cam_right.mkv`
  - `cam_chest.mkv`（启用胸部主摄时）
  - `stereo_left.mkv`
  - `stereo_right.mkv`
  - `tcam_left_l.mkv`
  - `tcam_left_r.mkv`
  - `tcam_right_l.mkv`
  - `tcam_right_r.mkv`
- 传感器：
  - `sensor_left.mcap`
  - `sensor_right.mcap`
  - `fays_data_left.mcap`
  - `fays_data_right.mcap`
- 元数据：
  - `metadata.json`
  - `calibration.json`
- 条件产物：
  - `audio_pre.wav`
  - `audio_post.wav`
  - `validation_error.log`（校验失败时，记录失败原因）
  - `ego/ego_sync.json`、`ego/rgb.mp4`、`ego/tracking.mp4`、`ego/ctrl.mp4`、`ego/audio.m4a`、`ego/sensor.mcap`、`ego/calibration.json`、`ego/metadata.json`（检测到 SXR ego 并完成或尝试联动采集时；当前不进入强校验与 `require_files`）

### 7.2 metadata.json
停录校验完成后，`record_runtime` 写入最终 `metadata.json`。当前顶层字段固定为：
`device_sn / device_type / device_mode / camera_codec / hardware_version / software_version / das_usb_updater_version / data_version / hardware_list / episode_name / data_uuid / audio_uuid / quality_check_status / quality_check_err_type / collection_duration_s / require_files / video_details`。

- `device_sn` 使用主控 `DEVICE_SN`，写出前统一转大写；`device_type=ugripper`，`device_mode=dual`，`data_version=3.0`。
- `metadata.json` 使用范本顺序写出顶层字段、`hardware_list` 字段和 `video_details[]` 字段；校验脚本会把 key 顺序漂移作为格式错误。
- `data_uuid` 使用系统随机 UUID；若 `/proc/sys/kernel/random/uuid` 不可用则回退 `libuuid` 的 `uuid_generate/uuid_unparse`。无音频文件时 `audio_uuid` 允许为空字符串。
- `hardware_version` 默认 `v2.5`，可通过 `UGRIPPER_HARDWARE_VERSION` 覆盖；`software_version` 来自 ugripper 主包版本并带 `v` 前缀；`das_usb_updater_version` 优先读取 `das-usb-updater` 包版本。
- `hardware_list` 只存各硬件 SN：左右 gripper、左右主摄、可选胸部主摄、四路 tactile、左右 stereo。左右 gripper SN 在 gripper 插入并完成运行时 refresh 后缓存，拔出后清理；写 metadata 时只使用该缓存，不再额外同步读串口。gripper HMI 不再参与 calibration 读取，后续标定完全从相机侧读取。主摄/胸部相机 SN 同样只使用插入/目标变化时通过 Yuzhou UVC XU 维护的运行时缓存，读取失败写空字符串，不回退 USB serial；tactile SN 由运行时在 `/dev/tcam_*` 插入或 symlink 目标变化时读取 USB sysfs `serial` 并缓存，metadata/calibration/后台 tactile 校验只消费缓存，不再每条 episode 执行 `udevadm`；stereo 使用 Fays daemon 状态中的 SDK serial。`video_details` 不再重复写 `serial`。
- `quality_check_status` 取值为 `success` / `fail` / 空字符串；`quality_check_err_type` 保持单字段兼容，只写本次失败的主 `error_type`。常见取值包括 `missing_file`、`collection_duration_too_short`、`frame_loss`、`finalize_error`、`stereo_control_failed`、`calibration_error`、`runtime_error`、`device_disconnected`、人工补标的 `operator_marked_failed`，以及健康监控直接上报的 `disk_mount_lost`、`disk_not_writable`、`disk_full`、`critical_devices_missing`、`stereo_not_ready`、`hmi_*` 等 fault key；无法归类时写 `unknown`。
- `collection_duration_s` 取最终视频有效时长最大值，保留 1 位小数；`video_details[].fps` 与 `duration_s` 也保留 1 位小数；普通相机与触觉相机的时长优先复用停录校验阶段的 `video_probes` 缓存，缺失时兜底重新探测视频；左右 stereo 的 `duration_s` 使用对应 `fays_data_*.mcap` 中 camera 帧首尾 logTime 跨度，避免轻微双目丢帧导致 MKV 容器时长偏短时误判轨迹数据不可用。
- `video_details[].start_offset_us` 由内部 timing 字段折算而来，单位为微秒，字段名带 `_us` 后缀；计算时以本 episode 最早一路视频 offset 为 0。
- `require_files` 使用当前新命名文件：`metadata.json`、`calibration.json`、八路基础视频、左右 sensor、左右 Fays MCAP；启用胸部主摄时追加 `cam_chest.mkv`。

### 7.3 停录强校验
停录后当前按以下层次校验：
- 若 stereo daemon 已明确返回 finalize/session 错误，则直接记录失败、写 `metadata.json` 与 `validation_error.log`，跳过后续完整性强校验，避免对已知缺失的 stereo 文件重复等待。
- 在线主摄、胸部主摄必须已有合法运行时缓存：SN 必须为 Yuzhou XU 中的 `FE...` 字段，标定必须为合法 `MCAL` V1 payload。缺任一项即校验失败，并写 `quality_check_err_type=calibration_error`。
- 文件存在性：八路 `mkv`、`sensor_left.mcap`、`sensor_right.mcap`、`fays_data_left.mcap`、`fays_data_right.mcap`、`calibration.json`、`metadata.json` 必须存在且非空；其中 `metadata.json` 会先于稳定校验写出，供校验阶段读取最终 metadata 语义。
- 内部 timing 缓存：停录收尾期间由 `/dev/shm/ugripper_recording_timing_*.json` 暂存 `boot_time_offset`、`boot_time_offset_us`、各路 `<camera>_record_time_offset_us` 和 `video_probes`，只用于生成最终 metadata；该文件不落到 episode，不作为硬校验对象，完成后清理。若 `mergeEpisodeInfo()` 提示 timing 打开失败，应优先结合 `camera_recorder` 的“wrote timing file / missing_record_time_offset_us / flush-close failed”日志一起判断根因。
- metadata 与视频可读性：最终 `metadata.json` 必须存在且包含可用的 `video_details[].start_offset_us/duration_s`；每路 `mkv` 都必须能被并行 `ffprobe` 读出首个视频流与 `start_time/duration`，但左右 stereo 的后续时长判断不再使用 MKV 容器时长。
- 时长合理性：每路视频有效跨度都必须大于最小阈值，且不能比本次 episode 的最长有效视频跨度短超过 `5s`；其中普通相机与触觉相机使用 MKV probe span，左右 stereo 使用对应 Fays MCAP camera 帧首尾 logTime 跨度。
- Fays MCAP 轻量完整性：左右 `fays_data_*.mcap` 必须能读取 summary，`i/c` 两类消息计数都必须非零，并且 summary/chunk 索引给出的消息覆盖跨度与 camera 帧覆盖跨度都不能过短；该检查只读 MCAP summary、头部首个 camera 帧和尾部少量 chunk，不允许 fallback 全量扫描消息。
- 条件产物：若执行了 pre/post 音频录制，对应 wav 仍需存在。
- 触觉软校验：
  - 每路 tactile 只取起录附近单帧；预处理只裁掉左侧约 `12%` 光源区域，不裁上边、右边和下边，也不做额外模糊，避免漏掉上方或右上方盖板损坏。若同 `serial` 实时参考帧缺失或处于夹爪重连待更新状态，则用本帧建立参考帧并跳过本轮实时 damaged 对比，失败则顺延到下一条 episode。已有实时参考帧时，按 baseline 对当前帧做全局亮度/对比度配准，再做直接 residual 差分；residual mask 经过 `3x3` 邻域投票、连通域过滤，以及“小连通域 + 低频变化小”的边界纹理误差过滤，单次异常不让当前 episode 失败，仅用于连续 `3` 个 episode 的损坏提示。
  - 同时还会与同 `serial` 的持久化 baseline 比较；若 baseline 缺失，则用本帧初始化 baseline 并跳过本轮 persistent 对比，失败则顺延到下一条 episode。已有 baseline 不会因为夹爪重连或时间到期刷新；persistent 比较独立维护最近 `3` 次窗口，连续 `3` 次异常才播放对应 damaged 语音并置位，并登记“下次重新开关机后允许刷新 baseline”。重新开关机后，下一条可用 episode 会刷新该路持久化 baseline；连续 `3` 次 clean 后清除告警但不刷新 baseline。
- 当前运行时轻量阈值口径：
  - `robust_residual_area >= 0.003`
  - 动态残差阈值为 `max(10, median(residual) + 6 * 1.4826 * MAD(residual))`
  - residual mask 过滤为：`3x3` 内异常像素数至少 `3`，且 `8` 连通域面积至少 `8 px`；其中面积不超过 `25 px` 且 `5x5` 低频差分均值小于 `5` 的连通域会作为高反差纹理边缘误差丢弃
  - 该口径用于过滤轻微灰度波动、孤立点和边缘残差；不再启用 small-damage 兜底，避免边线清晰或光源残留图像误触发。

说明：当前不会为视频做全量逐帧扫描；强校验只读取容器元信息并消费内部 timing 字段，触觉软校验也只做单帧快速比较，优先保证现场稳定性与停录耗时可控。

## 8. 灯效、音频与关键路径
### 8.1 当前状态灯语义
- `INIT`：初始化或等待 Fays recorder ready，蓝灯闪烁。
- `WRITING`：停录收尾、文件 flush、左手双键卸载数据盘等写盘/收尾阶段，蓝灯常亮。
- `READY`：可录制，绿色呼吸灯；当前基于单调时钟渲染，避免系统校时导致相位突变。
- `WARNING`：触觉软告警，黄灯按侧别与传感器位置编码闪烁；当前用于同一 tactile serial 的实时 reference 窗口或 persistent baseline 窗口连续 `3` 个 episode 异常。故障侧夹爪显示黄灯：`*_tcam_l` 为一长一短，`*_tcam_r` 为一长两短，同侧两路都异常为两长；左右侧都异常时两侧分别显示各自编码，未异常侧保持 READY 绿呼吸。若后续连续 `3` 次 clean，相关窗口会清除告警并回到 READY。
- `RECORDING`：录制中，绿色闪烁；当前只在亮灭边沿和低频补发时下发 RGB，避免高频重复写串口造成丢闪。
- `CALIB_PRE` / `CALIB_RUN` / `CALIB_DONE`：供 USB 导入、deb 安装与校准脚本复用；deb 安装窗口使用 `CALIB_RUN` 表示安装中、`CALIB_DONE` 表示全部包处理完成。
- `ERROR_1` ~ `ERROR_5`：红灯长短码。当前口径下，`ERROR_1` 用于完整性/校验失败，`ERROR_2` 用于关键设备/HMI/stereo 缺失或不活跃，`ERROR_3` 用于数据盘挂载丢失 / 不可写 / 写满，`ERROR_4` 用于 Fays 相机已识别但单侧 stereo/Fays recorder、FIFO 或控制链路失效，`ERROR_5` 用于其他运行时异常。同一条 episode 可同时存在多个内部 `error_type`，最终灯效只按优先级播报一个：磁盘类优先 `ERROR_3`，关键设备缺失优先 `ERROR_2`，stereo 控制失效为 `ERROR_4`，运行时异常为 `ERROR_5`，普通校验失败为 `ERROR_1`。硬件缺失类 `ERROR_2` 和控制链路类 `ERROR_4` 都会按侧别提示：故障侧夹爪闪烁对应错误码，另一侧红灯常亮；左右都故障则两侧一起闪烁；`ERROR_2` 无法归属左右侧时，两侧同步先闪一次完整序列，再红灯常亮相同时间并循环。
- `EXIT`：关机退出阶段。

### 8.2 关键持久化与临时路径
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- USB 标定导入阶段会在 `/etc/ugripper/config/calibration/.import_stage.*` 下生成临时 `calibration.json` 与夹爪 `payload bin`；由于 HMI helper 当前会以 `ubuntu` 用户运行，stage 目录需保持可遍历、payload bin 需保持可读，否则会出现“bin 已生成但 helper 无法读取”的写入失败。
- 运行时音频 FIFO：`/dev/shm/ugripper/umi_audio_pipe`
- 运行时软件录制控制 FIFO：`/dev/shm/ugripper/umi_record_control.pipe`
- 音频临时目录：`/tmp/umi_audio`
- 系统动作请求文件：`/run/ugripper/system_action_request`
- 系统动作结果文件：`/run/ugripper/system_action_result`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- 运行日志：`/tmp/umi_sys_<device_sn_lower>_<YYYYMMDD>.log`
- 数据盘日志镜像：`/mnt/data_disk/logs/umi_sys_<device_sn_lower>_<YYYYMMDD>.log`
- stereo daemon 状态：`/tmp/umi_stereo_camera_status.json`
- 左右 Fays recorder 控制 FIFO：`/dev/shm/ugripper/umi_left_fays_cmd`、`/dev/shm/ugripper/umi_right_fays_cmd`
- stereo daemon 顶层控制 FIFO：`/dev/shm/ugripper/umi_stereo_camera_control.pipe`（保留兼容入口，当前普通录制起停由 `record_runtime` 直接写左右 Fays recorder FIFO）
- 触觉传感器持久化状态：`/var/lib/ugripper/tactile_state`（包含 `reference/`、`persistent/`子目录与 `history.json`；`persistent/` 存放跨关机有效的长周期 baseline）
- `/mnt/data_disk` 只作为固定挂载点使用：安装阶段会预创建为 `root:root 0555`，业务不会把本地空目录当成数据目录；只有真实数据盘挂载成功后才允许继续启动录制服务。
- 运行日志维护当前参考 V1 口径：本地先写 `/tmp`，在视频停录、音频停录和运行时退出时增量同步到 `/mnt/data_disk/logs`，并只保留当天同 SN 日志。

### 8.3 软件录制控制
- `record_runtime` 启动后会创建并非阻塞打开 `/dev/shm/ugripper/umi_record_control.pipe`，供本机受控脚本触发录制起停；若 FIFO 创建或打开失败，只记录告警并继续保留原有 HMI 按键控制能力。
- `test/src/gripper_disconnect_repro_sop/` 提供可单独打包的现场复现工具，主机侧 `./scripts/prepare_240_repro.sh` 会通过 SSH 下发板端 worker，并复用该 FIFO 做连续软件录制与错误现场抓取；系统/服务实时日志仍建议测试人员另开终端手动观察。
- FIFO 按行接收文本命令，当前支持：
  - `SHORT_UP`：严格模拟右手上键短按语义；空闲时起录，录制中停录。
  - `SHORT_DOWN`：严格模拟右手下键短按语义；空闲且存在上一条 episode 时重录，录制中停录。
  - `START`：仅空闲时接受，并映射到 `ShortUpPressed`；录制中会忽略并记录 `[CONTROL_DIAG] ... reason=already_recording`。
  - `STOP`：仅录制中接受，并映射到 `ShortDownPressed`；空闲时会忽略并记录 `[CONTROL_DIAG] ... reason=not_recording`。
- 被接受的软件命令会在同一主循环内复用 `handleShortUpAction()` / `handleShortDownAction()`，不经过物理按键 `1s` 双击防误触门槛，因此停录会进入与按键双击确认后的相同 stop、finalize、merge、validation、LED/audio 恢复路径；不要通过直接写 stereo FIFO、kill recorder 或删除 lock 文件来替代停录。
- 控制入口带 `300ms` 防抖，单轮主循环最多处理 `8` 条命令，避免外部脚本重复写入导致 start/stop 连续抖动；同一轮中若已接受软件控制命令，会跳过本轮物理按键事件处理，降低叠加触发风险。
- 建议使用示例：
  - `echo START > /dev/shm/ugripper/umi_record_control.pipe`
  - `echo STOP > /dev/shm/ugripper/umi_record_control.pipe`
  - 如需严格模拟按键：`echo SHORT_UP > /dev/shm/ugripper/umi_record_control.pipe`、`echo SHORT_DOWN > /dev/shm/ugripper/umi_record_control.pipe`

### 8.4 PERF 日志
- 停录收尾、文件 flush、视频探测和 episode validation 会输出 `[PERF]` 耗时日志，用于现场拆分蓝灯延迟。
- `[PERF]` 日志由编译包决定，默认发布包关闭；需要开启时在构建时定义 `UGRIPPER_ENABLE_PERF_LOG=1`。运行时不再读取 `/etc/environment` 开关；错误日志、校验失败记录和必要状态日志不受影响，关闭时会同步压低 ffmpeg/gst/MPP 等编码链路噪声，`[FAYS_TS ...]` 事件日志仍会输出。`run_record.sh` 还会默认导出 `mpp_debug=0`、`mpp_log_level=2`、`mpp_syslog_perror=0`，让 CameraRecorder/Fays/ffmpeg 子进程继承 MPP error-only 日志口径，避免 `mpp_info`、`mpp_enc` 初始化配置行刷屏。
- validation 耗时当前按粗粒度输出：setup、并行视频 probe、并行 tail checks 与总耗时。其中 encoder/Fays tail checks 会作为 4 个只读任务并行执行，并统一打一条总耗时与详情日志；后台 tactile 慢校验另行输出 queued/start/end/applied/cancel 相关 PERF。

### 8.5 音频链路关键行为
- `audio/audio_play.py` 启动时按 `UGRIPPER_LANG` 选语音，并优先绑定 PulseAudio 中受支持的 USB 音频设备；当前兼容 `0020:0b21 (liyuany USB Audio)` 与 `0023:0b23 (liyuany USB PnP Sound Device)`。若无耳机，则回退系统默认 sink/source；耳机晚于服务启动才出现时，守护进程会先启动 FIFO 和监听线程，再在耳机出现后自动切回耳机。
- 提示音主线程通过 FIFO 收命令后使用 `pygame.mixer` 播放到当前选中的 PulseAudio sink；启动时只做一次短静音预热、每段提示音前补前导静音，不再维持常驻静音 keepalive。
- `audio/audio_play.py` 只有在真实绑定到一个可用的 PulseAudio 播放目标并完成后端初始化后才会写 `/dev/shm/ugripper/umi_audio_ready`；当前无论是受支持 USB 耳机还是系统默认声卡，都需要建好 backend 才会进入 ready，backend teardown 时会移除该标记，避免业务把“进程活着”误判成“提示音已可播放”。
- 提示音调度采用“后触发抢占前触发”的语义，不做排队串行；新的命令到达后会立即停止当前 one-shot 或 loop 提示，再播放最新命令。当前 `writing`、`calibrating` 属于 loop 提示，但同样会被后续命令直接打断。
- `record_runtime` 会在主循环内监测音频守护进程；若守护进程异常退出会按节流策略自动重拉起。正常情况下，音频守护进程会在“受支持 USB 耳机”和“系统默认声卡”之间自动切换，并在 `/dev/shm/ugripper/umi_audio_ready` 恢复后补发空闲态 `ready` 或当前阶段提示。
- 耳机运行中被拔掉时，音频守护进程不会退出，而是自动退回系统默认声卡；耳机重新插入并重新出现在 PulseAudio 后，会自动重新绑定回耳机，后续提示音恢复。
- 播放/录音初始化前会受控执行 `pactl unload-module module-suspend-on-idle`，避免 USB 耳机或默认声卡在长时间空闲、热插拔恢复或首次切换后出现首段吞音；该动作只收敛在初始化阶段，不在每次提示音、录音或音量键事件里重复切换模块。
- `py_script/usb_audio_mic_test.py --playback` 默认只做“原生采集 + SoX 后处理导出”，不再默认硬套旧 `noise.prof`；若需去噪，先运行 `py_script/usb_audio_noise_profile.py` 生成当前环境底噪 profile，再显式传入 `--denoise --noise-profile <path>`。
- 现场回归优先覆盖两类场景：耳机长时间空闲后的首次播放 `python3 py_script/usb_audio_play_test.py`，以及长时间空闲后的首次录音 `python3 py_script/usb_audio_mic_test.py --playback`；两项测试都应在日志中看到 `Disabled PulseAudio suspend modules: ...`。
- 回滚方式：若需恢复 PulseAudio 默认模块状态，可重启当前用户的 PulseAudio 会话，或重启 `ugripper.service` 让音频守护进程重新按默认环境启动；无需在运行期反复手工切换 `module-suspend-on-idle`。
- 现场 5 步回归 SOP：1）确认耳机已识别且服务正常，观察 `journalctl -u ugripper.service -n 100` 是否出现 USB 音频初始化日志；2）空闲 3~5 分钟后执行 `python3 py_script/usb_audio_play_test.py`，确认首个测试音不吞头；3）再次空闲 3~5 分钟后执行 `python3 py_script/usb_audio_mic_test.py --playback`，确认录音回放起始段不被截断；4）若需覆盖热恢复，再做一次耳机热插拔后重复步骤 2/3；5）若结果异常，记录 `pactl list short modules`、`pactl list short sinks`、`pactl list short sources` 与 `journalctl -u ugripper.service -n 200` 作为现场。

### 8.6 相关辅助单元
- `auto_update/umi-shutdown-trigger.path`：由 `ugripper` 主包交付并启用，监控 `/run/ugripper/system_action_request`；`/run/ugripper` 由随包 tmpfiles.d 配置在开机阶段创建，避免系统镜像中 `/tmp` tmpfs 与 swapfile 链路的 systemd boot transaction ordering cycle 造成 path unit 激活遗漏。
- `auto_update/umi-shutdown-trigger.service`：由 `ugripper` 主包交付，只作为 path 触发的 oneshot helper，不单独 enable；检测到触发文件后执行统一 helper，当前支持 `shutdown` 与 `umount` 两类动作，并把执行结果写回 `/run/ugripper/system_action_result`。
- `auto_calibration/ugripper-network-monitor.service`：监听网线插拔，当前仅在拔线时重启 `ugripper.service`。
- `auto_update/boot_check_install.sh`：开机时检查 `/opt/backup` 中的 `deb` 是否需要恢复或升级。

### 8.7 硬件健康监控
- `record_runtime` 当前参考 v1 口径保留低频硬件健康监控，约每 `1s` 检查一次关键硬件状态，而不是在主循环里做高频主动轮询。
- 当前监控项包括：`/mnt/data_disk` 是否仍可写、8 路相机设备节点、左右 IMU/encoder 设备节点、stereo daemon `ready/not-ready` 状态，以及左右 HMI 串口是否仍连接、输入侧 HMI 是否持续有响应。
- 发现磁盘异常时进入 `ERROR_3`；当前会区分 `disk_mount_lost`、`disk_not_writable`、`disk_full` 等 fault key，并按“同类 fault 首次出现打错误日志、持续期间不重复刷屏、恢复时补一条 recovered”收敛日志。发现关键设备节点缺失、HMI 断连或 HMI 长时间无响应时进入 `ERROR_2`，并通过音频守护进程播报 `error`。`ERROR_2` 会根据缺失路径、stereo 状态或 HMI port 归属到左手、右手、双手或 unknown，用对应侧别灯效提示现场先看哪侧硬件。若 Fays stereo/IMU symlink 在线但单侧 recorder、控制 FIFO 或 START/STOP 控制链路失效，则进入 `ERROR_4`，提示对应侧夹爪控制链路异常，并纳入错误态自动复位策略。
- 若录制中发现任意关键设备、HMI、数据盘或 stereo daemon 健康故障，当前 episode 会立即按错误停录收尾，写失败 `metadata.json` 和 `validation_error.log`，并把健康监控 fault key 作为显式 `error_type` 进入本条 episode；若停录后又发现文件缺失、finalize 失败等问题，会同时记录内部错误类型，但 `quality_check_err_type` 只保留最高优先级主类型。设备后续恢复只影响下一次录制，不会把本条数据恢复成成功。
- 若异常恢复发生在空闲态：回到 `READY` 并补播 `ready`。
- 自动复位策略当前只覆盖可能由夹爪侧 USB/供电重枚举恢复的错误：`ERROR_2`（关键硬件/HMI/stereo 缺失或不活跃，包含主摄 V4L2/USB 启动失败、主摄明显短流和 CameraRecorder D 状态卡死）、`ERROR_4`（stereo/Fays 控制链路失效）和主摄相关 `ERROR_1`（错误文本指向 `main camera`、`cam_left/right/chest` 或 `/dev/cam_*`）。`ERROR_3` 是数据盘挂载/可写/空间问题，不触发复位；`ERROR_5` 是 `runtime_error` 或其他运行时异常兜底，也不默认复位。若某侧夹爪完全未枚举（该侧 HMI 和关键传感器 symlink 都不存在），运行时只保留错误提示，不触发软件复位且不消耗复位次数；检测到该侧任一关键节点或 HMI 重新出现时，会重置当前复位次数，并给该侧约 `20s` 插入稳定窗口，窗口内健康监控错误、Fays ready 未完成或主摄缓存未读完都不会触发软件复位。symlink/硬件健康缺失类触发进入自动复位前会先进入约 `6s` 自恢复缓冲窗口，要求同一 `cause/evidence/side` 证据持续存在才执行复位，避免 symlink/udev 瞬时抖动或可自恢复重枚举直接触发 USB 断电；窗口开始时日志输出 `restore usb pending wait cause=... evidence=... side=... stable_ms=6000`，窗口内恢复会输出 `restore usb pending cleared ...` 并取消复位。主摄 V4L2/USB 启动失败、主摄明显短流、CameraRecorder D 状态卡死和主摄校验类一次性证据会直接触发，不进入稳定等待。确认触发时日志固定输出 `restore usb requested cause=... evidence=... side=...`，其中 `evidence` 直接给出缺失节点或关键错误证据。触发命令为 `sudo -n /usr/local/sbin/ugripper_restore_usb`；真正执行断电复位前，若仍在录制会先按错误停录完成 episode 收尾，停录返回失败但录制状态已经结束时仍进入自恢复缓冲窗口；只有 D 状态卡死这类 CameraRecorder 内核等待故障会在录制无法停住时继续进入复位流程，用于释放 V4L2/USB 内核等待。随后同步运行日志并通过 `/run/ugripper/system_action_request=umount` 请求 root helper 卸载 `/mnt/data_disk`，数据盘未挂载时直接继续，卸载失败时跳过本次复位且不消耗复位次数；只有 D 状态卡死允许在卸载失败时继续复位。随后运行时会先暂停 Fays stereo daemon/recorder，并在等待窗口内阻止 supervisor 自动重启 Fays，避免 SDK 在 USB 断电重枚举期间占用 stereo/IMU 节点。每次复位完成后会按约 `1s` 周期检查全部当前启用的关键传感器 symlink；节点齐全后立即恢复 Fays daemon，并从该时间点最多继续等待约 `45s`，要求健康监控恢复且主摄相关触发时主摄 SN/标定缓存恢复；等待 symlink 阶段只在缺失列表变化或约 `5s` 间隔输出 `restore usb symlinks waiting attempt=... evidence=...`，若复位完成后约 `25s` 仍缺 symlink，或 symlink 齐后约 `45s` 仍未健康恢复，则进入下一次复位。同一错误窗口最多连续复位 `3` 次，第三次仍在 symlink 或 ready 阶段超时后，会沿用 HMI 蜂鸣器链路对故障侧夹爪报警，未知侧或双侧故障时两侧同时报警，单次蜂鸣最长 `5s` 后自动关闭；关闭命令会结合状态回读确认，状态仍非 `0/0` 时最多重试 `5` 次。等待窗口内如果健康监控提前恢复并回到空闲态，会立即认定本轮复位成功并清除等待窗口，后续再异常按新的故障窗口处理；重新开始录制也会清除本轮复位窗口并关闭蜂鸣。等待窗口内的重复硬件健康报错不会抢跑触发下一次复位。该 root wrapper 会在存在 `bluetooth_gatt.service` 时先停止服务以释放通信接口，不存在或 stop/start 失败只记录告警，不阻断 GPIO/电源板复位主流程。供电板寄存器 `0x10` 当前按低 4 bit 逐位写入：下电逐位清零，上电逐位置位并保留短间隔错峰上电；每个 bit 写入后读回校验，整组完成后再次校验，失败时有限重试。
- `camera_recorder` 仍保持“单路 recorder 失败不立即主动终止整次录制”的容错语义；本次实现只加强停录阶段的子进程组回收与 stop 日志，不把启动期短暂抖动直接升级为全量停录。

## 9. 配置、安装与 U 盘流程
### 9.1 当前主要配置入口
当前主要配置来自 `/etc/environment`。

| 键 | 当前用途 | 备注 |
| --- | --- | --- |
| `DEVICE_SN` | 决定数据路径与标定导入匹配目录 | 建议视为必填 |
| `UGRIPPER_LANG` | 提示音语言 | 由 `config.txt` 导入 |
| `CAMERA_CODEC` | `camera_recorder` 启动参数，并同步 ego 视频编码 | 仅支持 `h264` / `h265`；ego 映射为 `avc` / `hevc` |
| `ENABLE_CHEST_CAM_MAIN` | `record_runtime` 读取 | 默认启用胸部主摄；显式写成 `0/false/no/off/disable/disabled` 时关闭 |
| `UGRIPPER_EGO_SERIAL` | `ego_recording_worker.py` 读取 | 默认不指定 serial，自动扫描 `adb devices`；需要固定某台 ego 时可设置 |
| `UGRIPPER_EGO_ADB` | `ego_recording_worker.py` 读取 | 覆盖 ADB 可执行文件路径；默认优先使用随包安装的 `bin/UgripperRuntime/adb/adb`，不可用时回退 `PATH` |
| `UGRIPPER_EGO_SYNC_INTERVAL_SEC` | `ego_recording_worker.py` 读取 | ego 文件增量同步间隔，默认 `1s` |

说明：当前录制与 U 盘导入流程都不再使用角色环境变量；episode `metadata.json` 也不再写角色字段。

### 9.2 当前默认项
| 项目 | 当前默认口径 |
| --- | --- |
| 部署形态 | 单机双手、本地录制 |
| 录制相机集合 | 左右主摄 + 左右 stereo + 4 路触觉；默认额外启用胸部主摄，可通过 `ENABLE_CHEST_CAM_MAIN` 关闭 |
| 网络 | 业务可在无对端设备时启动 |
| 静态 IP | 主包不托管，沿用系统现有有线配置 |

### 9.3 U 盘支持内容
- 自动安装名单：
  - `das-usb-updater`
  - `ugripper`
  - `bluetooth-gatt-server`
  - `databot-device-joint`
  - `device-ota-mender`
- updater 自升级：根目录放置 `das-usb-updater*.deb`；若本次先装的是新版 updater，安装后的新脚本会在同一次插盘流程里继续按最新名单扫描剩余 `.deb`。
- 主包升级：根目录放置 `ugripper_*_arm64*.deb`。
- 配置导入：根目录 `config.txt`。
- 标定数据导入：`ugripper_calib/<DEVICE_SN>/`，通过文件名后缀 `_left` / `_right` 区分左右主相机 `camchain`；对应夹爪侧 RGB/stereo/IMU payload 则按现场读出的 gripper SN 文本在该目录下递归匹配，优先 `.bin`，其次包含 `summary/imucam` 关键词的 `.md` 或当前 raw 目录（`rgb_video_ros-camchain.yaml + output-results-imucam.txt`），并按 `rgb_video_ros_imucam_parameter_summary.md` 口径生成 `1024-byte` 对齐数据结构；当前固件写入时固定补满 `64 x 16B` 传输窗口。
- encoder 零位校准触发：根目录 `calibration.txt`。
- deb 安装灯效：当本次 U 盘里实际存在目标软件包并进入安装窗口后，`usb_auto_update.sh` 会停止业务栈并通过 HMI helper 显示 `CALIB_RUN` 黄灯快闪；安装中灯效至少保持 `3s`，避免短安装或主包资源替换导致肉眼不可见；任一包安装或安装后置处理失败时显示 `ERROR_1` 红灯 `3s` 并退出；全部目标包处理成功后显示 `CALIB_DONE` 绿灯 `3s`，随后先恢复业务服务，确认服务恢复后再继续升级完成提示音。
- 安装完成提示音：当本次 U 盘里实际出现的目标软件包全部安装/重装完成后，`usb_auto_update.sh` 会直接从新安装的 `/opt/ugripper/audio*/upgrade_completed.wav` 里选取对应语言资源，并以 `paplay` + PulseAudio 播放升级完成提示音；当前不要求自动安装名单里的包必须全部同时出现在 U 盘。若现场没有可用 PulseAudio sink，则只记日志，不把安装流程判失败。

### 9.4 U 盘同次插入顺序
当同一次 U 盘插入同时包含 `config.txt`、`ugripper_calib/` 和 `calibration.txt` 时，当前顺序是：
1. 导入 `config.txt`
2. 按 `_left` / `_right` 文件后缀识别需要导入的左右标定目标，并逐侧读取现场 gripper SN、匹配对应夹爪标定 payload
3. 只有当本次目标侧都完成 gripper SN 匹配后，才开始写入对应夹爪的整套 RGB/stereo/IMU 标定；任一侧匹配失败或写入失败时，本次不会把旧 persist calibration 当作成功结果继续保留
4. U 盘导入当前负责“匹配并写入夹爪标定 + 保存导入归档”；主机侧持久化 `calibration.json` 由 `record_runtime` 在 gripper 后续插入/重连时按实际读回的 SN 与 calibration payload 刷新
5. 若存在 `calibration.txt`，则跳过中间重启，直接进入左右编码器并行校准流程
6. 若未触发 `calibration.txt`，则开始按固定名单依次处理 `das-usb-updater`、`ugripper`、`bluetooth-gatt-server`、`databot-device-joint`、`device-ota-mender` 的 `.deb`；每个包都会先按 Debian 包名读取版本，U 盘内若存在多个候选文件则取最高版本；只要名单内包在 U 盘根目录存在，就执行安装，同版本也会强制重装；进入 deb 安装窗口后显示 `CALIB_RUN` 黄灯快闪且至少保持 `3s`，任一包失败显示 `ERROR_1` 红灯 `3s` 并退出
7. 若 updater 在第 6 步先完成自升级，则安装后的新脚本会在同一次插盘流程里继续执行剩余自动安装名单，避免必须二次插盘才能让新名单生效
8. 全部目标软件包安装完成后，先显示 `CALIB_DONE` 绿灯 `3s`
9. 恢复 `ugripper.service` 并确认业务服务可启动；若恢复失败，本次升级按失败处理，不播放完成提示音
10. 若新主包已提供 `upgrade_completed.wav` 且现场存在可用 PulseAudio sink，则播放升级完成提示音

### 9.5 安装脚本与网络行为
`pack_script/postinst` 当前会：
- 停掉旧的录制相关进程。
- 初始化持久化标定目录；若 `calibration.json` 缺失、空文件或非法 JSON，则自动用 `bin/UgripperRuntime/config/fakeCamCalib.json` 修复。
- 自动配置 exfat 提前加载：若现场仍使用 `/home/user/lib/exfat.ko` 外部模块，安装时会把它复制进 `/lib/modules/<kernel>/extra/`、写入 `/etc/modules-load.d/ugripper-exfat.conf`，并在本次安装窗口内尝试立即加载。
- 在重放相机/音频/串口 udev 规则前再次停止 `ugripper.service` 并等待停稳，避免 Fays warmup daemon 持有 stereo/IMU video 节点时被 trigger 扰动。
- 重新加载 udev 规则，并只重放 Fays USB、video4linux、sound、input 与已枚举 CH9344 tty 事件；不主动触发 block add，避免升级期间重复拉起 USB updater。
- 创建 `/run/ugripper` 运行时目录，清理旧 `/tmp/umi_system_action_*` 与当前 `/run/ugripper/system_action_*` 残留请求，随后启用并启动 `umi-shutdown-trigger.path`；该 path/service 归 `ugripper` 主包唯一交付，`das-usb-updater` 不再打包同名单元，避免 `/lib/systemd/system` 与 `/etc/systemd/system` 的同名覆盖造成系统动作请求无人消费。
- udev trigger 完成后手动 `start ugripper.service`，并等待服务进入 active；若启动失败，主包 postinst 直接失败，由 U 盘升级流程按安装失败处理。
- 启用并重启 `ugripper-network-monitor.service`。

网络相关当前行为：
- 主包不再创建、更新或删除有线 NetworkManager 连接。
- 主包安装/卸载默认沿用系统原有的有线 IP 配置，不对现场网络拓扑做接管。
- 当前录制启动不依赖对端网络存在。

数据盘挂载当前行为：
- `config/99-fixed-usb-map.rules` 会在允许的物理 USB 口上调用 `mount_data_disk.sh`，固定挂载点仍然是 `/mnt/data_disk`。
- 主包规则现在只负责挂载/卸载；若额外安装了可选包 `das-usb-updater`，则由 updater 包自己的 `99-usb-auto-update.rules` 通过 `SYSTEMD_WANTS` 拉起 `usb-auto-update@<dev>.service`。
- 允许的 USB 分区在 `remove` 事件里会显式对 `/mnt/data_disk` 执行卸载清理；此外还保留了 USB block `remove` 的兜底触发，尽量覆盖 hub 断链或热插拔时分区级事件不完整的场景，避免拔盘后残留 stale mount。
- `mount_data_disk.sh` 当前除了匹配挂载源设备节点，还会把“挂载点只读”“挂载源设备节点已不存在”或“挂载点已不可访问”视为脏状态并优先清理；清理同一数据盘时会同步卸载同源的其他挂载点（例如桌面 automount 到 `/media/ubuntu/...` 后又挂到 `/mnt/data_disk` 的双挂载场景），避免复位前只卸载业务挂载点却仍有同一 USB 盘保持 mounted；它不再主动启动 updater service；`run_record.sh` 也会把这类状态视为未就绪。

### 9.6 网线监测行为
`auto_calibration/monitor_network.sh` 当前行为：
- 网线拔出：仅重启 `ugripper.service`
- 网线插入：不重启 `ugripper.service`，也不执行其他额外动作
- 若存在 `/run/ugripper_installing_from_usb.lock`，则跳过升级窗口内的边沿动作

### 9.7 主包打包约束
- 主包当前仍直接携带项目内 `.venv` 与 `.venv/.python-runtime`，部署后继续以 `/opt/ugripper/.venv/bin/python3` 作为首选解释器入口。
- `build_deb.sh` 的 staging 目录默认按增量方式复用：项目主体与 `.venv` 分开同步，避免每次打包都先删除再完整重拷 `.venv`。
- `build_deb.sh` 默认 `dpkg-deb` 压缩口径为 `xz -1`，兼顾构建速度与包体积；`build_deb.sh -q` 仍跳过 C++ 编译，并沿用同一默认压缩口径。如需在速度与包体积之间切换，可通过 `DPKG_DEB_COMPRESSOR`、`DPKG_DEB_LEVEL`、`DPKG_DEB_STRATEGY`、`DPKG_DEB_UNIFORM_COMPRESSION` 覆盖默认参数。
- 主包打包当前默认优先从 Nexus raw 仓库下载并解压归档好的 `.venv` `tar.gz`，再沿用同一套 `.venv` 架构校验与 staging 同步逻辑；当前默认归档为 `ugripper-v2-uv-venv/py311_v2.1.0/ugripper_venv_20260623_102954_arm64.tar.gz`，这条入口只针对 raw 制品下载，不走 Conan recipe。
- 若需要临时切回本地或外部目录中的 `.venv` / `build`，可显式设置 `PACKAGED_VENV_URL=''` 后再配合 `PACKAGED_VENV_SOURCE`、`PACKAGED_BUILD_DIR` 覆盖来源；若最终 `.venv` 来源不存在，脚本会同步移除 staging 中旧的 `.venv`，此时包仍可生成，但不再满足部署后直接运行的交付约束。

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
- `/dev/cam_left`
- `/dev/cam_right`
- `/dev/cam_chest`（启用胸部主摄时）
- `/dev/tcam_left_l`
- `/dev/tcam_left_r`
- `/dev/tcam_right_l`
- `/dev/tcam_right_r`
- `/dev/left_encoder`
- `/dev/right_encoder`
- `/dev/left_gripper`
- `/dev/right_gripper`

### 10.6 快速定位建议
- 不能启动：先看 `ugripper.service` 日志和 `record_runtime` 是否成功拉起。
- 能录不能停：优先检查 HMI 按键事件、状态机和 `sensor_recorder` / `camera_recorder` 退出路径。
- 少文件或校验失败：先核对默认八路视频、胸部主摄启用时的第九路视频、双 MCAP、`metadata.json`、`calibration.json` 是否完整；若已进入 `ERROR_1`，优先查看 episode 下的 `validation_error.log`；若进入 `ERROR_3`，优先检查 `journalctl -u ugripper.service` 中的 `disk_mount_lost / disk_not_writable / disk_full` 日志。
- warmup 一起双目就掉线：先查是否同时存在多份 `camera_recorder --stereo-daemon`。重复 warmup daemon 抢占同一批双目视频设备时，可能把双目打进 `recovering`，严重时会伴随 USB 侧重枚举；先清掉多余 daemon，再观察 `/tmp/umi_stereo_camera_status.json` 与 `journalctl -u ugripper.service -n 200`。
- tactile serial 不对：先看 `journalctl -u ugripper.service` 中 `tactile camera runtime serial cache` 相关日志，确认 `/dev/tcam_*` 插入、symlink target 变化和 sysfs serial 缓存刷新结果；再只读核对 `/sys/class/video4linux/<videoN>/device` 向上 USB 父节点的 `serial`，并对比 episode `calibration.json` 中 `observation.images.tcam_left_l.serial`、`tcam_left_r.serial`、`tcam_right_l.serial`、`tcam_right_r.serial`；当前口径只修正 episode，不回写 `/etc/ugripper/config/calibration/calibration.json`，且不再输出旧 tactile 键。
- 进入错误灯效但录制进程还活着：优先检查 `/mnt/data_disk` 是否仍可写、关键 `/dev/*` 设备节点是否还在，以及 HMI 是否持续响应。
- 音频异常：优先看 USB 耳机枚举、PulseAudio sink/source、`module-suspend-on-idle` 是否已在初始化阶段被卸载。
- 运行日志缺失：先看 `/tmp/umi_sys_<sn>_<date>.log` 是否生成，再看 `/mnt/data_disk/logs/` 是否存在当天镜像，最后核对 `/mnt/data_disk` 是否仍是真实可写挂载点。
- U 盘流程异常：先看 `/var/log/ugripper/usb_auto_update.log`，再区分是包安装、配置导入、标定导入还是 encoder 校准失败。

### 10.7 tactile serial 定向核对
```bash
journalctl -u ugripper.service --since "2 hours ago" | grep 'tactile camera runtime serial cache'

for dev in /dev/tcam_left_l /dev/tcam_left_r /dev/tcam_right_l /dev/tcam_right_r; do
  node="$(basename "$(readlink -f "$dev")")"
  echo "===== $dev -> $node ====="
  p="$(readlink -f "/sys/class/video4linux/$node/device")"
  while [ "$p" != "/" ] && [ -n "$p" ]; do
    if [ -f "$p/serial" ]; then
      echo "$p/serial: $(cat "$p/serial")"
      break
    fi
    p="$(dirname "$p")"
  done
done

python3 - <<'PY'
import json, pathlib
path = pathlib.Path('/mnt/data_disk/<device_sn_lower>/data/episode_<date>_<index>/calibration.json')
data = json.loads(path.read_text())
for key in [
    'observation.images.tcam_left_l',
    'observation.images.tcam_left_r',
    'observation.images.tcam_right_l',
    'observation.images.tcam_right_r',
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
  - `scan_main_camera_mkv_issues.py`：递归扫描 `cam_left.mkv` / `cam_right.mkv`，并发检查主摄 `mkv` 的包时间戳异常、显著时间洞与可疑解码错误，并对齐同目录左右主摄的异常时间点。
  - `board_gripper_hmi_link_stress.sh`：停止主服务后反复独占连接左右 HMI，验证状态回复、RGB 和蜂鸣器开关回读，并并行运行 `SensorRecorder` 检查同颗 CH9344 上的 encoder UART 是否持续产出；默认将 encoder 最大相邻时间戳 gap 限制为 `100ms`，避免平均频率掩盖秒级数据洞；结果默认写入 `/dev/shm`，便于在加密环境中中转部署和回收。
  - `board_ch9344_interference_matrix.sh`：按空载、原始 open/close、termios 配置、`TCIOFLUSH`、`TIOCEXCL`、原始状态/RGB/蜂鸣器帧、完整 HMI 重连和长连接分阶段施加干扰，每阶段独立记录左右 encoder MCAP 与 gap 事件，用于定位 CH9344 多 UART 相互影响的具体触发操作。
  - `board_encoder_to_hmi_interference_matrix.sh`：反向保持左右 HMI 长连接和状态查询，同时对指定 encoder UART 高频执行 open/close、配置、flush 与真实 1Mbps 请求，统计同侧/异侧 HMI 回复年龄和 inactive 样本，确认 encoder 操作是否会触发 HMI 无响应。
- 建议从仓库根目录显式执行 `bash test/scripts/<script>.sh`；详细参数与注意事项见 `test/README.md`。
- `py_script/read_ugripper_mcap.txt` 是给数据使用者参考的通用 MCAP 读取示例；文件后缀使用 `.txt`，方便在会拦截或加密 `.py` 的环境中发送，但内容仍是 Python 代码。它可直接输入单个 `.mcap` 或完整 episode 目录；episode 模式会自动读取 `sensor_left/right.mcap`、`fays_data_left/right.mcap` 与可选 `ego/sensor.mcap`，并解码当前 UGripper encoder 与 Fays `i/c` 二进制 payload。配套说明见 `py_script/README_read_ugripper_mcap.txt`。示例：
  `python3 py_script/read_ugripper_mcap.txt /mnt/data_disk/<device_sn_lower>/data/episode_<date>_<index> --json-out /tmp/ugripper_mcap_report.json`。
- 当前 UGripper MCAP 是通用 MCAP 容器，不是 ROS 2 bag；不要用 `ros2 bag info` 或 Foxglove 的 ROS bag 可视化入口作为数据是否正常的判断依据。若要可视化，需要按脚本里的 payload 定义转换成 Foxglove/ROS 可识别 schema 后再导入。
- 若现场需要脱离主服务、单独向夹爪 HMI 写入 SN / 标定，可使用 `py_script/hmi_sn_batch_writer.py`：脚本直接读取一个或多个 `.xlsx` 的多 sheet `SN码` 列，支持操作员输入 SN 后 4 位或任意连续片段做唯一匹配，自动扫描 CH9344 相关串口确认可用 HMI 口，并在匹配到目标 SN 后写入 SN 与可选 `1024-byte` 标定 payload。
- `py_script/hmi_sn_batch_writer.py` 默认会在启动时把自身和本次使用的 `.xlsx` 同步复制到 `/mnt/data_disk/hmi_sn_writer/`；标定源目录可通过 `--calib-root` 指向包含 `.bin`、summary `.md` 或 `rgb_video_ros-camchain.yaml + output-results-imucam.txt` 的目录。若未找到目标 SN 对应标定文件，脚本会只写 SN，并在控制台用黄色提示“未写标定”。

## 11. 当前使用注意点
- `config/camera_recorder.yaml` 当前包含 9 路配置；胸部主摄是否参与录制、校验和 health check 由 `ENABLE_CHEST_CAM_MAIN` 决定。若现场还要裁剪录制集合，应同步调整 `record_runtime` 的 `--only` 参数与 episode 校验清单。
- 仓库内仍有部分后处理脚本依赖旧输出命名或旧假设，不能默认视为当前主链路的一部分。
- `test/scripts/` 下的脚本属于仓库侧辅助工具，默认不随主包安装；若现场需要长期保留，应明确同步部署方式与使用说明。
- `info.json` 不再作为 episode 最终产物生成；时间偏移信息已迁移到 `metadata.json.video_details[].start_offset_us`。
- 现场调试若需要同时访问不同子网设备，应由系统侧或人工维护额外地址/路由；主包不负责托管这些网络策略。
- 校准、导入与恢复动作仍分散在多个 shell 脚本和 service 中；当前功能可用，但维护时需要注意入口分散。
