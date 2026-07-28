# Changelog

## Unreleased

- Ego 绑定改为开机周期状态：绑定文件迁移到 `/dev/shm/ugripper/ego_binding.json`，同次开机内服务重启保持绑定，整机重启或断电开机后自动解绑；升级时清理旧持久化绑定文件。
- 提高 Ego 绑定/解绑反馈区分度：绑定成功改为双手亮黄色三闪与 `1000/2000/4000Hz` 升调，解绑成功改为双手紫色三闪与 `4000/2000/1000Hz` 降调；继续复用蜂鸣状态重试和结束静音机制。
- 修复夹爪蜂鸣反馈偶发持续鸣响：HMI 驱动对蜂鸣开启和关闭状态切换统一执行 `5` 次、`20ms` 间隔重发，并在连接建立与驱动退出时主动重发关闭状态；Ego 绑定、任务打标等按侧反馈不再依赖单次关闭帧。
- 修正 stereo 元数据时间基准：Fays finalize 后，runtime 通过现有 MCAP reader 轻量读取 summary 与首尾 chunk，将首末 camera `publishTime` 和消息数写入现有 timing 缓存；`video_details[].start_offset_us/duration_s` 不再使用 Ego 等待前的 session START 时间或固定帧率 MKV 的压紧时长。metadata 写入只消费 timing 缓存，不改变原有原子写入与 flush 顺序。
- Episode 深度校验器按 `software_version` 的 `+nostereo` 构建标记动态选择产物清单，当前 schema 同步到 `data_version=3.1`，并兼容历史 `3.0` 以及缺少可选 `task_start_s/task_stop_s` 的数据；仅有开始标记时按无回环数据处理。Ego 绑定/解绑与录制任务打标新增青色灯光反馈，有效任务标记改为双手同步显示，失败和拒绝动作分别沿用红色、橙色语义；所有周期灯效统一按绝对单调时钟计算，避免左右夹爪在单侧提示恢复或重连后错相。
- 新增 SXR Ego 显式绑定：空闲时左右上键同时长按 `2s` 可绑定唯一在线 Ego 或解除现有绑定，绑定结果通过不同蜂鸣节奏提示并持久化；绑定后断连、起录失败或停录 finalize/拉取/远端本地大小核对不完整统一进入 `ERROR_5`。普通单键长按统一调整为 `1000ms`，左上仅用于标记上一条 episode 失败，左下预留；录制中右上长按 `800ms` 依次写入 `task_start_s` / `task_stop_s`，第三次拒绝。metadata `data_version` 更新为 `3.1`，主包版本提升到 `2.1.9`。
- 修正绑定 Ego 起录竞态：worker 在校时、编码设置和广播阶段使用非终态 `starting`，避免运行时把准备中的设备误判为 `not_found`，导致 UGripper 起录被拒绝而 Ego 已开始录制。
- 修复 auto-release manifest 扫描构建目录和归档虚拟环境导致的长时间卡顿：基线迁移到独立状态目录，改用 Git 文件集和批量哈希，并增加原子写入与超时保护。
- Fays VIKit SDK 从 `3.5.1` 更新到上游 `main` 的 `3.8.0`（`b1d74499`），同步 aarch64/x86_64 运行库与公开 API/版本头；FT602 `1.0.17` 运行库保持不变。主包版本同步提升到 `2.1.8`。
- 扩展四口手部 USB2 hub 映射：支持 `.1=主摄/.2=tactile_r/.3=tactile_l/.4=CH9344`，并由 `.4` 自动生成左右 gripper/encoder symlink，同时保留既有 hub 拓扑兼容。
- 新增构建期开关 `UGRIPPER_ENABLE_STEREO`，默认 `ON` 并沿用普通版本/包名；显式设为 `OFF` 时生成 `+nostereo` 包变体，运行时不启动 Fays daemon、不检查 stereo/Fays 健康、不执行双目 session/finalize，也不生成或校验 `stereo_left/right.mkv` 与 `fays_data_left/right.mcap`。该开关不提供 `/etc/environment`、`config.txt` 或命令行运行时覆盖。
- HMI 物理按键在动作状态机前新增按下、松开各 `40ms` 的双向稳定滤波，过滤短暂电平毛刺；原有 `80ms` 短按动作间隔保持不变。
- UgripperRuntime、SensorRecorder 与 encoder zeroing 的信号处理路径只设置停止标志，不再在信号上下文执行日志、子进程等待或复杂对象清理；SensorRecorder 退出时先等待编码器读写线程停止，再关闭串口，避免服务停止或停录时发生死锁、资源竞态及 sensor MCAP Footer 未封口。

> 说明：本文件只保留 v2.0.0 以来的高信号发布变更；当前系统行为以 `docs/agent/overview.md` 为准。
>
> 仓库没有 `v2.0.x` git tag。下面的发布边界按 `build_deb.sh` / `scripts/build_arm_deb_in_pp_arm_dev.sh` 中 `BASE_VERSION` 的提交记录推定：`2caf6e0` 为 v2.0.0，`d81c3c1` 为 v2.0.1。

## v2.1.5 - 2026-07-13

- 物理 `BTN_UP` / `BTN_DOWN` 录制启停改为 `1s` 内同键双击触发，软件 FIFO 控制命令仍保持单次触发；物理按键释放防抖由 `250ms` 调整为 `80ms`，避免第二次短按被过滤。
- 新增停录后人工失败补标：空闲时左手单键长按可将上一条完成 episode 的 `metadata.json` 标记为 `operator_marked_failed`，同步写入 `validation_error.log`；成功后紫灯闪烁与双手蜂鸣器两次提示同步。ACI 多文件 episode 保持四个独立 sensor/Fays MCAP，不向数据 MCAP 注入 metadata topic。
- 触觉状态比较统一将画面左侧裁剪比例由 `12%` 调整为 `18%`，使运行时抽帧与离线残差检查使用相同有效区域。
- 新增 encoder 命令 ACK 延迟诊断，以及触觉相机 FFmpeg 单路反复 STREAMON、四路并行启停压力测试脚本。
- 增强 240 现场安全 soak 与掉线复现流程，补充自动录制、日志抓取和 Foxglove episode 打开辅助入口。
- 新增 HMI 链路压力、CH9344 分组干扰和 encoder 到 HMI 反向干扰诊断工具，用于区分单 UART、夹爪控制板、CH9344、Hub 与 USB 上行链路故障。
- 构建脚本默认主包版本调整到 `2.1.5`，并已完成 ARM64 打包与 240 设备安装验证。

## v2.1.4 - Unreleased

- 主摄压缩码流在写入最终容器前等待包含参数集与 IDR 的 clean keyframe，避免设备冷启动坏首包进入 MKV；停录时若 EOS 已被轮询线程消费，则不再重复等待并误报 `bus_wait_timeout`。
- HMI 蜂鸣关闭改为发送命令后回读确认，状态不为 `0/0` 时最多重试 `5` 次。
- CameraRecorder、Fays recorder 及运行时直接启动的辅助子进程在 `exec` 前关闭非标准 fd；Fays daemon 启动左右 recorder 时不再向下继承顶层控制 FIFO，避免 ffmpeg 或 recorder 额外持有 CH9344、相机、IMU 等无关硬件资源。
- 构建脚本默认主包版本调整到 `2.1.4`。

## v2.1.3 - Unreleased

- 子进程启动前主动关闭未白名单继承的 fd，并为主摄 XU 与夹爪串口 fd 设置 close-on-exec，避免 Fays recorder 继承主摄、串口等父进程外设句柄。
- 新 hub 评估板 udev rules 保留板端已验证映射：右侧主摄 `1-1.4`、左侧主摄 `9-1.4`，四路触觉分别覆盖 `1-1.3.1/1-1.3.6/9-1.3.1/9-1.3.6`。
- 数据盘未挂载或不可写时只保持等待/`ERROR_3`，不进入 USB restore 复位流程。
- 构建脚本默认主包版本调整到 `2.1.3`。

## v2.1.2 - Unreleased

- 自动复位 symlink 齐全后的 ready 等待窗口从约 `25s` 提到约 `45s`，降低 Fays stereo daemon 重启和主摄缓存刷新接近边界时被误判为复位失败的概率。
- Fays stereo daemon 不再按单侧 SDK startup complete 串行启动左右 recorder；同轮维护中会直接拉起可用侧 recorder，ready 状态继续由健康检查收敛。
- 录制中新增 CameraRecorder 主摄 V4L2/USB 故障直达 `ERROR_2`：主摄启动阶段若返回 `VIDIOC_*`、I/O error、设备消失或超时，CameraRecorder 会写入 `/dev/shm` 故障证据，`record_runtime` 立即中断录制并触发现有 USB 复位流程；主摄录制窗口达到约 `5s` 后若仍只有明显短流，也按同一路径处理，`5s` 内快速启停不触发短流判定；CameraRecorder 内核 D 状态兜底阈值同步收敛为 `5s`，用于应用层无法返回错误的 ioctl 卡死场景。
- 构建脚本默认主包版本调整到 `2.1.2`。

## v2.1.1 - Unreleased

- 新增错误态夹爪传感器复位入口：`record_runtime` 会在 `ERROR_2`、`ERROR_4` 以及主摄相关 `ERROR_1` 时异步触发 `/usr/local/sbin/ugripper_restore_usb`，`ERROR_3` 磁盘类错误和 `ERROR_5` 运行时兜底错误不触发；触发复位前会先暂停 Fays stereo daemon/recorder，避免 SDK 在 USB 断电重枚举窗口占用双目节点；每次复位完成后会按约 `1s` 周期检查全部关键传感器 symlink，symlink 齐后立即重启 Fays daemon 并开始健康恢复计时，最多等约 `25s` 仍缺 symlink 才重试；symlink 齐后最多继续等约 `25s` 检查健康状态与主摄缓存，仍未恢复则重试；等待窗口内如果健康状态提前恢复，会立即认定本轮复位成功并清除等待窗口，后续再异常按新的故障窗口处理；同一错误窗口连续 `3` 次失败后通过夹爪 HMI 蜂鸣器报警，单次蜂鸣最长 `5s` 后自动关闭。sudoers 允许 `ubuntu` 用户免密执行固定 root wrapper；wrapper 会在存在 `bluetooth_gatt.service` 时先停止以释放通信接口，不存在或停止失败不阻断复位主流程。
- 自动复位断电前新增数据盘保护：录制中先按错误停录完成 episode 收尾，随后同步运行日志并通过 root system action 卸载 `/mnt/data_disk`；数据盘卸载失败时跳过本次复位且不消耗复位次数，避免 U 盘读写中被 USB 供电复位硬断。
- 自动复位新增人工插拔保护：某侧夹爪完全未枚举时只保留错误提示，不触发软件复位且不消耗复位次数；检测到该侧任一关键节点或 HMI 重新出现时会重置复位次数，并给该侧 `20s` 插入稳定窗口，窗口内健康检查错误不触发软件复位。
- 自动复位触发日志收敛为 `restore usb requested cause=... evidence=... side=...`；symlink/硬件健康缺失类触发会先进入约 `6s` 自恢复缓冲窗口，窗口内同一证据持续存在才执行复位，避免 symlink/udev 短暂抖动或可自恢复重枚举直接触发 USB 断电；主摄校验类一次性证据仍会直接触发。录制中硬件故障停录后，即使本条 episode 质量校验失败，只要录制已经停住，也会进入该缓冲窗口等待确认。恢复等待阶段只在缺失列表变化或约 `5s` 间隔输出 symlink 缺失证据。
- `run_record.sh` 默认导出 `mpp_debug=0`、`mpp_log_level=2`、`mpp_syslog_perror=0`，压低 Rockchip MPP 编码库的 `mpp_info`/`mpp_enc` 正常配置噪声，避免录制时 journal 被编码器初始化信息刷屏。
- 删除 Fays recorder 控制 FIFO 启动时的 `[Control] Entering command loop` 与 `Supported commands` 提示日志；保留 START/STOP、FIFO 读写失败和未知命令等有效事件。
- 自动复位跳过人工插拔稳定窗口或整侧未枚举时不再按轮询周期重复输出 `skip restore usb ...`；保留人工插入、拔出、稳定窗口结束、复位请求、symlink 缺失/超时与最终恢复/失败日志，便于直接看到复位原因及证据。
- 自动复位在等待本轮复位 settle、已达到最大尝试次数、preflight cooldown 或复位命令仍在运行时不再按轮询周期重复输出 `skip restore usb ...`；`restore command` 缺失或不存在仅在原因变化时输出一次。
- 夹爪 HMI 串口驱动不再按 `30s` 周期输出 `[GRIPPER_DIAG] category=io_summary reason=periodic`；保留断连、IO 失败和独占命令失败摘要，降低正常待机日志噪声。
- 删除 Fays stereo daemon 健康维护循环中的 `stereo recorder start order` 调试日志；保留 recorder 启停、SDK startup、unhealthy/restarting 等有效事件日志。
- 修正夹爪供电复位写寄存器方式：电源板 `0x10` 寄存器恢复按低 4 bit 逐位写入，上电阶段保留短间隔错峰上电；每个 bit 写入后读回校验，整组完成后再次校验，失败时有限重试，避免偶发读写失败直接终止复位。
- 收口系统动作触发单元归属：`umi-shutdown-trigger.path/service` 改为由 `ugripper` 主包唯一交付并在安装后显式启用/启动，`ugripper.service` 启动时也会补拉 path listener；`das-usb-updater` 不再打包同名 unit，避免 `/lib` 与 `/etc` 下的同名单元互相覆盖导致旧版系统动作请求文件无人消费。
- 系统动作文件触发通道从 `/tmp/umi_system_action_*` 迁移到 `/run/ugripper/system_action_*`，并通过 tmpfiles.d 在开机阶段创建 `/run/ugripper`，规避系统镜像中 `/tmp` tmpfs 与 swapfile 链路的 systemd boot transaction ordering cycle 导致 path unit 激活遗漏。
- Python 运行环境新增 `pymodbus`，并将默认打包 `.venv` 切到 `py311_v2.1.0/ugripper_venv_20260623_102954_arm64.tar.gz`；`pyserial` 继续锁定在现有依赖中。
- 构建脚本默认主包版本调整到 `2.1.1`。

## v2.0.13 - Unreleased

- 主摄 V4L2 MMAP 缓冲池改为 `v4l2_buffer_count` 可配置，并将默认主摄缓冲数从 24 收敛为 16，降低 RK3588 现场 `alloc_contig_range ... PFNs busy` / CMA 压力导致主摄 UVC 起流失败的概率，同时避免回退到历史 8-buffer 口径带来的短时背压/坏流风险。
- Fays stereo raw frame 队列收敛：`VideoFrameQueue` 默认容量从 128 降为 32，ffmpeg rawvideo `thread_queue_size` 从 512 降为 64，降低双路 stereo 常驻/录制切换期间的大帧内存占用。
- 触觉相机 SN 改为运行时缓存：在 `/dev/tcam_*` 插入或 symlink 目标变化时通过 USB sysfs serial 刷新，metadata、calibration 与后台 tactile 校验只消费缓存，减少停录阶段外部探测操作。
- 触觉传感器持久化 baseline 存储目录由 `/tmp/umi_tactile_state` 改为 `/var/lib/ugripper/tactile_state`，避免重启后 persistent baseline 被清除导致无法检测关机期间发生的盖板损伤。
- 触觉软告警黄灯改为按侧别与传感器位置编码：左侧分别使用一长一短/一长两短，双路异常时使用两长，左右两侧可同时显示各自编码。
- 升级/重装时在 `prerm` 阶段自动清除触觉传感器基线目录，避免旧版本基线持续污染新版本。
- 录制起停的 stereo 控制改为 `record_runtime` 直接写左右 Fays recorder FIFO，停录时并发发送左右 `STOP`，并统一 C++ FIFO 完整行读写工具，避免顶层 shell FIFO 超时读半行导致 `STOP` 被截断。
- 双目 stereo 的时长与 span gap 校验改用对应 Fays MCAP camera 帧首尾时间跨度，MKV 仅保留存在性与可读性检查，避免轻微丢帧造成容器时长偏短时误判数据失败。
- 触觉实时参考帧与持久化 baseline 改为夹爪重连后只标记待更新，由后续首个可用 episode 的触觉视频截帧生成，避免服务初始化或插爪阶段直接打开触觉相机。
- 单侧 Fays stereo 控制链路失效改为 `stereo_control_failed` / `ERROR_4` 侧别提示，识别不到 Fays 相机仍保持 `ERROR_2`；FIFO 启动超时日志补充 side、pid、设备路径和 runtime status 证据。
- 主包 postinst 安装窗口改为先停止并等待 `ugripper.service` 完全停稳，再重放 udev trigger，最后手动 start 并确认 active，降低 Fays warmup daemon 持有设备时触发 udev 导致 stereo USB 掉线的风险。
- Fays stereo daemon、wrapper 与 recorder 补充 `[FAYS_TS <HH:MM:SS.usec>]` 事件日志，覆盖 recorder 启停、健康重启、video port 占用清理、session start/stop/finalize 错误和 recorder 进程异常退出，便于和 `dmesg -T` USB 断连时间线直接对齐。
- Fays stereo daemon 改为按 stereo `/dev/videoN` 枚举顺序串行启动左右 SDK recorder，单侧等待启动完成或 `10s` 超时后再拉起另一侧，避免双侧 SDK 并行初始化互相干扰。
- 新增 `py_script/read_ugripper_mcap.txt` 通用 MCAP 读取示例及配套 README，供数据使用者直接解析当前 UGripper episode 中的 sensor、Fays 与可选 ego MCAP；使用 `.txt` 后缀便于发送。
- `[PERF]` 日志改为由编译包决定，默认发布包关闭；运行时不再读取 `UGRIPPER_PERF_LOG`，需要开启时由构建定义 `UGRIPPER_ENABLE_PERF_LOG=1`；Fays 错误、重启和必要状态日志不受该开关影响。
- ego 起录前按 `CAMERA_CODEC` 同步视频编码：`h264` 广播为 `avc`，`h265` 广播为 `hevc`，保持 ego 与背包编码格式一致。
- 构建脚本默认主包版本调整到 `2.0.13`。

## v2.0.4 - Unreleased

- 收口 SXR ego 联动采集：起录前同步 ego 时间，等待本次 temp episode 后再启动本机录制，并将 ego 数据直接落到当前 episode 的 `ego/`。
- 增强夹爪掉线复现 SOP：支持主机侧一键 SSH 启动连续软件录制测试、本地刷新 `hws` 状态，并在错误后自动抓取现场日志后暂停等待恢复。
- 加固 ego 停录收尾：修复 MP4/M4A `moov` 与文件变小残留尾部问题，显式 flush 后再安全清理远端 finalized episode。
- 新增本机软件录制控制 FIFO `/tmp/umi_record_control.pipe`，支持 `START/STOP/SHORT_UP/SHORT_DOWN` 并复用按键停录路径。
- Fays stereo cached calibration 复用增加 SN 校验，避免热插拔或单侧重启后视频与旧标定错配。
- 修正左手新 hub 触觉 `udev` 映射，并保留旧特殊 hub 兼容规则。
- 收敛 recording timing 诊断口径，补充 shm 路径、缺失 offset 和 flush/close 失败日志。
- 错误分类改为显式 `error_type`：支持同条 episode 内多个错误并存，`metadata.json` 结构保持不变且只写主错误类型，灯效按优先级播报一个错误；移除基于错误文本或 `/mnt/data_disk` 路径的猜测分类。
- 数据盘故障通过显式 fault key 映射到 `ERROR_3`，并收敛重复错误日志。
- `das-usb-updater` 的 deb 安装窗口恢复 HMI 灯效提示：安装中黄灯快闪，安装失败红灯，安装完成绿灯。
- 构建脚本默认版本调整到 `2.0.4`。

## v2.0.1 - 2026-05-21

- Episode 产物命名统一切到 2.0.1 口径：视频、传感器和 Fays 文件改为 `cam_left/cam_right/cam_chest`、`stereo_left/stereo_right`、`tcam_*`、`sensor_left/sensor_right`、`fays_data_left/fays_data_right` 等新命名。
- Episode 完成状态改为目录名表达：录制中使用 `episode_YYYYMMDD_NNNN-temp`，收尾完成后 rename 为最终 episode；最终产物不再包含 `info.json`，时间偏移迁移到 `metadata.json.video_details[].start_offset_us`。
- `metadata.json` 切到新 3.0 协议，增加硬件列表、必需文件、视频详情、采集时长和质量检查结果；`calibration.json` 改为按 schema 3.0 从零组装，避免旧标定字段混入。
- 主摄 SN 与标定改为设备插入/目标变化时异步维护运行时缓存，停录只消费缓存；缺少合法 SN 或标定会按 `calibration_error` 失败。
- Fays stereo daemon readiness、热插拔恢复、运行态 watchdog 和 FIFO 控制继续加固；录制中 stereo/finalize 明确失败时可快速结束并写失败 metadata。
- 停录和校验性能优化：相机与 sensor 并发 stop，视频探测和 Fays/encoder tail check 并行执行，补齐 Fays MCAP 与错误日志 flush。
- 硬件缺失类 `ERROR_2` 灯效改为侧别化提示；HMI SN payload 无效时直接拒绝，避免错误序列号进入 metadata。
- 新增 recording-safe NTP 同步链路：安装 `ugripper-ntp-sync.service`，只在非录制态校时，并通过 `/tmp/umi_recording.lock` 避免录制期间系统时间跳变。
- USB updater 迁移为独立 `das-usb-updater` 包，并保留过渡包构建入口，支持现场从旧 updater 迁移。
- 移除板载 IM648 采集链路：`sensor_recorder` 只保留左右 encoder，episode 校验不再要求板载 IMU topic。

## v2.0.0 - 2026-04-29

- 引入 Fays stereo 录制能力：新增 `standalone/FaysStereoRecorder`、Fays SDK 依赖和左右 stereo daemon，支持双侧 stereo 视频与 Fays MCAP 数据进入 episode。
- 录制 runtime 接入 Fays 预热、录制、停录与校验流程，补齐 stereo session/finalize 失败处理和 Fays tail/IMU 轻量检查。
- 相机配置迁移到 `standalone/CameraRecorder/config/camera_recorder.yaml`，Fays stereo 作为独立录制链路与主摄/触觉链路协同工作。
- ARM 构建与出包链路补齐 Fays recorder 目标、SDK 运行库和 `pp-arm-dev` 容器内一键出包脚本；默认主包版本提升到 `2.0.0`。
- 固定 USB 映射、camera recorder 配置和部署脚本同步适配 v2 Fays stereo 硬件拓扑。
