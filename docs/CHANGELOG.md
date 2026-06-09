# Changelog

> 说明：本文件只保留 v2.0.0 以来的高信号发布变更；当前系统行为以 `docs/agent/overview.md` 为准。
>
> 仓库没有 `v2.0.x` git tag。下面的发布边界按 `build_deb.sh` / `scripts/build_arm_deb_in_pp_arm_dev.sh` 中 `BASE_VERSION` 的提交记录推定：`2caf6e0` 为 v2.0.0，`d81c3c1` 为 v2.0.1；v2.0.8 为当前发布收口版本。

## v2.0.8 - Unreleased

- 双目 stereo 的时长与 span gap 校验改用对应 Fays MCAP camera 帧首尾时间跨度，MKV 仅保留存在性与可读性检查，避免轻微丢帧造成容器时长偏短时误判数据失败。
- 触觉实时参考帧与持久化 baseline 改为夹爪重连后只标记待更新，由后续首个可用 episode 的触觉视频截帧生成，避免服务初始化或插爪阶段直接打开触觉相机。
- 单侧 Fays stereo 控制链路失效改为 `stereo_control_failed` / `ERROR_4` 侧别提示，识别不到 Fays 相机仍保持 `ERROR_2`；FIFO 启动超时日志补充 side、pid、设备路径和 runtime status 证据。
- 主包 postinst 安装窗口改为先停止并等待 `ugripper.service` 完全停稳，再重放 udev trigger，最后手动 start 并确认 active，降低 Fays warmup daemon 持有设备时触发 udev 导致 stereo USB 掉线的风险。
- Fays stereo daemon、wrapper 与 recorder 补充 `[FAYS_TS <HH:MM:SS.usec>]` 事件日志，覆盖 recorder 启停、健康重启、video port 占用清理、session start/stop/finalize 错误和 recorder 进程异常退出，便于和 `dmesg -T` USB 断连时间线直接对齐。
- 新增 `py_script/read_ugripper_mcap.txt` 通用 MCAP 读取示例及配套 README，供数据使用者直接解析当前 UGripper episode 中的 sensor、Fays 与可选 ego MCAP；使用 `.txt` 后缀便于发送。
- `[PERF]` 日志改为由编译包决定，默认发布包关闭；运行时不再读取 `UGRIPPER_PERF_LOG`，需要开启时由构建定义 `UGRIPPER_ENABLE_PERF_LOG=1`；Fays 错误、重启和必要状态日志不受该开关影响。
- 构建脚本默认主包版本调整到 `2.0.8`。

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
