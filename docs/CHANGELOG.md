# Changelog

> 说明：`docs/CHANGELOG.md` 只保留历史变更追溯；当前系统说明请优先阅读 `docs/agent/overview.md`。
>
> 本文件中的条目只用于追溯发布与实现演进；请不要直接把单条历史记录当作“当前系统行为”。

## Unreleased - 2026-03-26
- `record_runtime` 恢复 v1 风格的运行时 tactile serial 注入：每次生成 episode `calibration.json` 时，都会从现场 `/dev/left_tcam_*`、`/dev/right_tcam_*` 向上读取 USB `ATTRS{serial}`，并写入当前 episode；当前按 `*_l` 优先、`*_r` 回退，并在同侧两路 serial 不一致时输出 warning。
- 当持久化 calibration 中的 tactile serial 与现场硬件不一致时，当前口径为“修正 episode、输出明确日志、不回写 persist calibration”。
- episode `calibration.json` 的 tactile 标定项扩展为 4 路独立输出：`left_tcam_l`、`left_tcam_r`、`right_tcam_l`、`right_tcam_r` 会各自带上参数和现场 serial；若源 calibration 仍是旧的左右各 1 路结构，运行时会按左右侧复制参数并保留旧键兼容。
- episode `calibration.json` 进一步移除了 `gripper_left_tactile` / `gripper_right_tactile` 两个旧键，最终输出只保留 4 路真实 tactile 相机标定项。

## v1.2.6 - 2026-03-20
- 翻转左右主摄画面，恢复正常拍摄方向。

## v1.2.5 - 2026-03-19
- 优化了启动、挂盘和日志同步流程，提升整机运行稳定性。
- 改善了提示音和 USB 耳机的兼容性，减少播放异常和热插拔后的恢复问题。
- 调整 HMI 灯效调度：呼吸灯与闪烁改为基于单调时钟渲染，RGB 写串口改成“颜色变化优先 + 低频补发”，主动状态查询降为低频探活，降低呼吸抖动和录制态丢闪。
- 参考 v1 的后台监控模式，为 v2 补回低频硬件健康监控：持续检查数据盘可写性、关键 camera/imu/encoder 设备节点以及 HMI 连接/活跃状态；发现异常时进入错误灯效并播报 `error`，恢复后在空闲态补回 `ready`、录制态恢复 `RECORDING` 灯效。
- 优化了相机录制与停录阶段的稳定性，降低偶发缺文件或录制异常的概率。
- 加强了标定、安装和恢复流程的容错，减少坏配置或异常环境带来的启动失败。
- `build_deb.sh` 的打包 staging 改为增量复用：项目主体与 `.venv` 分开同步，默认 `dpkg-deb` 压缩口径改为 `xz -1`，并支持通过环境变量复用外部 `.venv` / `build` 产物、覆盖压缩参数。
- 整理了仓库内测试脚本和相关文档，方便现场排查与回归验证。

## v1.2.2 - 2026-03-16
- `camera_recorder` 内部改为并发拉起全部选中的相机子进程；`sensor_recorder` 改为并发初始化左右 IMU / encoder，并收紧 IMU 配置阶段固定等待，减少首样本启动散布。
- 修复 HMI 录制灯效切换偶发失效：`gripper_hmi` 现在会串行化同一串口上的状态查询与 RGB 指令发送，避免 `RECORDING` 1Hz 闪烁在左右手出现亮灭切换失败。
- 继续收口 HMI 控制模型：单个 gripper 串口的状态查询与 LED 下发改为进入驱动内部单线程 owner 队列，按“状态查询优先、LED 刷新次之”调度，降低录制过程中的可感知控制干扰。
- 录制灯效边沿生成进一步收口到 HMI 驱动 owner 线程：`record_runtime` 只切换 `READY/RECORDING/...` 状态，不再跨线程下发 500ms 亮灭边沿，降低边沿在软件侧被覆盖或错过的概率。
- 参考 V1 口径进一步收束灯效职责：HMI 驱动接管全部常规灯效生成，主进程只负责状态切换；同时保留 direct RGB 通道，便于专项调试或临时直控覆盖当前效果。

## v1.2.1 - 2026-03-14
- episode 补回 V1 风格的 `metadata.json` / `calibration.json`：`metadata.json` 恢复 `collector`、`data_path` 等兼容字段，并补写 `UGRIPPER_LANG`；`calibration.json` 改为在保留主摄/触觉结构的同时，把旧 Fays 标定块替换成 `left_stereo` / `right_stereo` 占位标定。
- 清理旧录制角色残留：`record_runtime` 不再写角色字段，`usb_auto_update.sh` 也不再从 `config.txt` 导入角色配置。
- 调整 HMI `READY` 呼吸灯手感：完整呼吸周期从 `3.0s` 放慢到约 `4.5s`，LED 渲染/发送节拍从 `50ms` 提升到 `20ms`，减少肉眼可见的亮度阶梯抖动。
- 放宽 `udev` 口位映射：左右 stereo 与 CH9344 串口桥允许同侧 hub 的内部端口 `.1/.2` 互换；同时按当前现场布线对调左右 tactile 的 `tcam_l/tcam_r` 内部端口映射。
- 录制启动改为并行拉起 `camera_recorder` 与 `sensor_recorder`，消除 `record_runtime` 在业务层面对相机与传感器的串行启动偏差；不引入统一 `start_time` 或额外起录同步协议。
- 主包安装/卸载不再托管有线静态 IP：`postinst/prerm` 去掉 `ugripper-static-*` 的创建、删除与回退逻辑，安装升级默认沿用系统现有 NetworkManager 配置，避免覆盖现场自带的 `192.168.2.240` 等板卡网络策略。
- 修正主相机直录链路：`CAMERA_CODEC=h264` 时直接拉主摄 H.264 码流，`CAMERA_CODEC=h265` 时直接拉主摄 H.265 码流，避免环境里的 codec 误把主摄固定成错误的输入格式。
- 修复 USB 数据盘拔插后 `/mnt/data_disk` 残留 stale mount 的问题：`mount_data_disk.sh` 现在会识别“当前挂载源设备节点已失效或 `/dev/sdX` 漂移”的场景，并在 add/remove 路径优先清理旧挂载，再挂上当前真实分区。
- 为 V2 恢复 V1 风格 `info.json` 时间偏移字段：改为由 `camera_recorder` 在内部按每路首个 pre-mux packet 原位锁定 `PTS + 系统时间`，统一生成 `boot_time_offset` / `boot_time_offset_us` 和 8 路默认相机的 `<camera>_record_time_offset_us`。
- V2 episode 停录校验升级为消费式稳定校验：`record_runtime` 不再在校验阶段回填时间字段，而是强校验 `camera_recorder` 已写出的 `info.json` 字段，并用轻量 `ffprobe` 检查 8 路视频可读性与基础时长合理性。
- 标定导入升级为双夹爪口径：`ugripper_calib/<DEVICE_SN>/` 目录下支持同次存在 `_left` / `_right` 后缀的 `camchain` 文件，分别导入左右主相机参数，单侧缺失不覆盖另一侧。
- 编码器校准升级为左右并行 zeroing：`calibration.txt` 触发后会同时启动左右 zeroing，并在 `zeroing` 成功后读取 0.5 秒状态，输出最后一个位置值供现场判断是否校准成功。

## v1.2.0 - 2026-03-14
- 收紧数据盘挂载保护：`mount_data_disk.sh` 为 add/remove 引入锁并只在当前移除的就是已挂载设备时才执行卸载，避免竞态把 `/mnt/data_disk` 删除；安装阶段会固定预创建只读挂载点，`run_record.sh` 的等待日志也改为限频输出，避免数据盘未上线时刷屏。
- `run_record.sh` 启动时改为等待 `/mnt/data_disk` 真正成为可写挂载点后再拉起 `record_runtime`，避免数据盘缺失或尚未挂载时由 systemd 持续 crash loop 重启业务。
- 修复 USB 数据盘自动挂载 exFAT 失败：`config/99-fixed-usb-map.rules` 改为通过 `systemd-run` 调用 `auto_update/mount_data_disk.sh`，由 helper 在 `ID_FS_TYPE=exfat` 时显式走 `mount.exfat-fuse`，其他文件系统回退到 `systemd-mount` 自动探测，避免 udev 直接承载 FUSE 挂载进程。
- 数据盘 helper 补齐失败回收与拔盘清理：挂载失败时会清掉空的 `/mnt/data_disk`，并在 USB 分区 `remove` 事件触发时主动卸载并删除空挂载点，避免拔盘后残留 `root:root` 空目录。
- 主包依赖补齐 `exfat-fuse` 与 `exfatprogs`，确保新装机或升级后具备 exFAT 挂载与校验工具。
- 主服务与校准/USB 更新辅助流程的运行用户从 `radxa` 统一切到 `ubuntu`，与当前系统账号保持一致。
- 修复 CH9344 `udev` 规则里的 shell 变量转义：对 `PROGRAM==...` 中的 `$$` 做显式转义，避免 `systemd-udevd` 先把 `$()` / `$node` / `$pick` 误解析成自身替换语法并报 `invalid substitution type`，确保 `right_gripper` / `left_gripper` / `*_encoder` / `*_imu` 节点稳定生成。
- 主包安装/卸载的有线网托管改为“接管并可回退”：首次安装前会备份当前激活的手工以太网连接，卸载时删除 `ugripper-static-*` 后恢复原先手动 NetworkManager 配置，避免现场先手工设为 `192.168.1.110` 时被破坏。
- 收敛 `auto_calibration/monitor_network.sh` 的网线边沿行为：插线时仅记录日志，不再触发 `udevadm trigger` 或重启 `ugripper.service`；拔线时仍只重启 `ugripper.service`。
- `record_runtime` 默认录制集合从 6 路扩为 8 路：启动 `camera_recorder` 时纳入 `left_stereo` / `right_stereo`，episode 强校验也同步要求两路 stereo `mkv` 落盘。
- `camera_recorder` 的相机列表从代码内置 `BuildDefaultConfigList()` 改为外部 YAML：默认读取 `config/camera_recorder.yaml`，并支持 `--config-yaml <path>` 切换配置文件。
- 录制缓冲参数 `input_thread_queue_size` 也下沉到 YAML；当前默认收紧为 stereo `128`、tactile `64`，便于后续现场按配置调优内存占用。
- `audio/audio_play.py` 移除常驻静音 keepalive，只保留启动短预热与每段提示音前导静音，避免长期占用 USB headset sink 扩大热状态行为面。
- `audio/pulse_audio_utils.py` 在播放/录音初始化前收敛执行 `pactl unload-module module-suspend-on-idle`：仅在 USB 音频链路建链时关闭 idle suspend，降低长时间空闲或热插拔后的首段吞音。
- 补充作用边界：`module-suspend-on-idle` 的卸载应作为 USB 音频初始化动作，而不是每次提示音/录音时都重复切换；现场仍可手工执行一次该命令做临时验证。
- 删除旧 `led_manager.py`：夹爪灯效统一收口到 `src/gripper_hmi` 的状态渲染器，`record_runtime` 与校准/USB 导入流程都改为直接驱动 HMI RGB。
- 新 HMI 灯效补齐旧语义：支持 `INIT`、`READY`、`RECORDING`、`CALIB_PRE`、`CALIB_RUN[:progress]`、`CALIB_DONE`、`ERROR_1~ERROR_5`、`EXIT`，并复现错误红灯长短码。
- `record_runtime` 补回语音/落盘节奏：停录后先播 `recording_stop`，再播 `writing` 并显示 `INIT` 蓝灯；校验通过后重新播 `ready`。
- `audio/audio_play.py` 改回 `pygame.mixer` 常驻播放方案：启动时强制绑定型号为 `0020:0b21` 的 USB 耳机 sink，预热 mixer 后通过 FIFO 播放提示音，并继续在同一进程中监听耳机 HID 音量键；`record_runtime` 与校准脚本优先使用 `.venv/bin/python3`，无本地虚拟环境时回退 `uv run python3`。
- episode 输出收口到现场所需最小集合：保留 `info.json`、`metadata.json`、`calibration.json`、六路视频和左右传感器 MCAP，不再生成 `cam.mkv` / `tact_left.mkv` / `tact_right.mkv` 兼容 symlink。
- `record_runtime` 补齐完整按键状态机：`BTN_UP/BTN_DOWN` 长短按、pre/post 音频录制与双键长按关机请求均已迁入 C++。
- 录制触发时当前启动左右主摄 + 四路触觉相机，继续不录制左右 stereo；传感器数据拆为 `sensor_data_left.mcap` 与 `sensor_data_right.mcap`，episode 校验同步检查双文件。
- `src/camera_recorder` 重构为“`main.cpp` + config list + 录制类”模式：按配置选择主相机直录、触觉混合链路、双目混合链路，替代原来集中式命令拼接。
- 主相机链路改为新相机直出 H.265：`camera_recorder` 直接用 GStreamer 把 `/dev/*_cam_main` 的 H.265 码流封装成 `mkv`，不再做二次编解码。
- 修正触觉与双目录制链路：放弃 `gst 解码 -> rawvideo pipe -> ffmpeg` 方案，改为 `ffmpeg -f v4l2 -input_format mjpeg -> hevc_rkmpp` 直接链路；双目继续单文件输出，不再做左右拆分。
- `camera_recorder` 各路视频现为独立子进程/pipe；单路掉线或启动失败只标记本路失败，不会主动连带停掉其他相机录制。
- 新增 `src/record_runtime`：用 C++ 重写 `run_record.sh` 的核心录制控制，直接用类管理夹爪 HMI 按键、夹爪 LED、`camera_recorder` 与 `sensor_recorder` 子进程。
- `run_record.sh` 收缩为薄壳启动脚本，默认直接拉起 `build/src/record_runtime/record_runtime`；旧网络同步链路不再纳入主流程。
- `record_runtime` 当前保留短按录制控制：`BTN_UP` 启停普通录制，`BTN_DOWN` 在空闲时触发 reset 录制、在录制中触发停录；退出前会主动熄灭左右夹爪 LED。
- `gripper_hmi_test` 默认端口改为 `/dev/right_gripper` 与 `/dev/left_gripper`，并改为纯 udev 规则：按左右 USB kernel 路径锁定 CH9344 设备组，再在同组 tty 中取内核排序最前的口作为 gripper HMI；不再依赖额外 role probe。
- `right_imu/right_encoder/left_imu/left_encoder` 也切换为纯 udev 规则：按左右 USB kernel 路径锁定 CH9344 设备组，再按组内固定顺序映射（第 2 路 encoder、第 3 路 IMU），不再依赖 `ch9344_role_probe.py`。
- `GripperHmiDriver` 在断开连接前会主动发送熄灯命令，避免测试程序退出后夹爪 LED 保持常亮。
- `run_record.sh` 切到单机双手固定布局：不再读取 `DEVICE_SIDE`，相机录制统一一次启动 `left_cam_main/right_cam_main/left_tcam_l/left_tcam_r/right_tcam_l/right_tcam_r` 六路流。
- episode 校验同步升级为单机双手六路视频模型：要求两路主相机跨度可读、四路触觉视频与 `sensor_data.mcap` 的跨度分别对齐；同时保留 `cam.mkv` / `tact_left.mkv` / `tact_right.mkv` 兼容 symlink（当前指向右手流）。
- `pack_script/postinst` 与设计文档同步收口旧 `DEVICE_SIDE` 表述，明确当前默认部署为单机双手。

- 收束 U 盘导入与校准时序：同一次插入同时包含 `config.txt`、`ugripper_calib/<DEVICE_SN>/` 与根目录 `calibration.txt` 时，先导入配置与主相机标定，再串行触发 `ugripper-calibration.service` 完成 encoder 零位校准；存在 `calibration.txt` 时不再做中间重启。
- 明确校准触发边界：`ugripper_calib/` 仅导入主相机标定，不会误触发 encoder 零位校准；encoder 校准仅由 U 盘根目录 `calibration.txt` 触发。
- 清理 `docs/agent/overview.md` 中旧 Fays 回归测试项：标定导入与相机硬件测试点改为仅覆盖当前主相机/触觉相机链路，并删除已完成的清理 TODO，避免按已移除能力误判失败。
- 完成 V2 pre/post 音频链路迁移：`run_record.sh` 不再调用 `arecord -D hw:rockchipes8388,0`，改为通过新增 `audio/record_usb_audio.py` 走 USB 耳机麦克风 + PulseAudio 采集，再沿用原 SoX 后处理。
- `auto_calibration/run_calibration.sh` 移除 `rockchipes8388` 的 `amixer` 初始化残留，校准流程与 V2 USB 音频方案保持一致。
- 修复 `auto_calibration/monitor_network.sh` 的网线“伪上升沿”：改为等当前物理网卡与 `carrier` 首次可读后再建立基线，并在网卡切换时重新建基线，不再把服务重启后的已插线状态误判成新插入事件。
- 主包安装链路恢复网络配置：`pack_script/postinst` 会自动识别当前物理网卡，并通过 NetworkManager 配置静态 IP `192.168.1.110/24`；`prerm` 对应清理 `ugripper-static-*` 连接。
- `run_record.sh` 的 V2 默认行为改为本地控制模式：未配置或非法旧角色配置时默认按 `master` 启动，不再隐式假设旧 Right/Left 对端 IP；旧双机 `SYNC_TARGET_IP`/`MASTER_IP` 仅作兼容保留。
- 新增 `src/gripper_hmi` 模块：参考 `ref_src` 的夹爪串口协议，在项目源码内用 `libserialport` 重写按键读取与 RGB LED 控制类，不再引入 `RingBuffer` / `SerialPort` / `SerialDevice` 抽象。
- 新增 `gripper_hmi_test` 测试程序，可独立验证 `/dev/ttyCH9344USB0`、`/dev/ttyCH9344USB8` 上的按键上报与 LED 控制；当前尚未替换 `run_record.sh` 的旧 GPIO/PWM 主流程。

# Changelog

## v1.1.19 - 2026-03-11
- 合并音频守护进程：耳机音量键监听已并入 `audio/audio_play.py` 的后台线程，不再额外依赖 `ugripper-usb-headset-keys.service`，减少一个 systemd service。
- 安装/卸载链路同步简化：`postinst/prerm/build_deb.sh` 移除独立耳机音量键 service 的启停、禁用与打包逻辑。
- 将仓库内默认 SoX `audio/noise.prof` 更新为当前现场在 `/tmp/ug_test_noise4.prof` 生成的新底噪 profile。

## v1.1.18 - 2026-03-11
- 修复 `py_script/usb_audio_mic_test.py` 的短录问题：放弃直接以 `16k/mono` 从 PulseAudio source 采集，改为按 source 原生参数录音后再用 SoX 转成目标格式，避免耳机麦克风链路被截断。
- 新增 `py_script/usb_audio_noise_profile.py`：先采集当前环境底噪并生成 SoX `noise.prof`，再由 `usb_audio_mic_test.py --denoise` 显式使用，避免默认硬套旧 profile 把人声一起削掉。
- `usb_audio_mic_test.py` 改为默认不启用去噪；只有显式传入 `--denoise` 且提供当前环境生成的 noise profile 时，才执行 `noisered + norm`。

## Unreleased - 2026-03-11
- 移除 Fays 相机相关运行链路：`run_record.sh` 不再检测、拉起或校验 Fays 录制进程与数据文件。
- 构建/打包链路移除 `faysSense_vi_kit` 与 `fays_record_example`，主包仅保留 `camera_recorder` 与 `sensor_recorder` 相关产物。
- U 盘标定导入改为仅处理主相机 `camchain`，不再依赖 `imucam` 或写入 Fays/IMU 标定字段。

## v1.1.17 - 2026-03-11
- 新增 USB 耳机音量键服务：通过 `audio/usb_headset_volume_keys.py` 监听这款耳机的 HID 按键事件，并用 `pactl` 调整 PulseAudio sink 音量，不再依赖桌面环境的媒体键守护进程。
- `config/99-fixed-usb-map.rules` 新增 `/dev/input/ugripper_usb_audio_keys` 输入设备别名，支持耳机热插拔后稳定定位音量键输入节点。
- `py_script/usb_audio_mic_test.py` 接入旧 SoX 去噪链路：录音后优先执行 `remix 1 -> noisered noise.prof 0.15 -> norm`，若系统未安装 `sox` 则降级为保留原始录音并打印告警。

## v1.1.16 - 2026-03-11
- 新增 `py_script/v2_serial_probe.py`：面向 v2 迁移场景，枚举 USB 串口拓扑并按现有协议自动探测 `IM648` 与 `encoder` 所在串口。
- `v2_serial_probe.py` 默认按 `115200` 验证 encoder，并支持可选 `--set-encoder-1m` 下发切到 `1Mbps` 的配置命令；脚本会明确提示该变更需断电重上电后再验证。

- `config/99-fixed-usb-map.rules` 新增 `ttyCH9344USB*` 权限规则：对 `1a86:55d9` 多串口桥统一放开 `0666` + `uaccess`，便于现场直接运行 IMU / encoder 探测脚本。
- `config/99-fixed-usb-map.rules` 升级为基于 USB2 hub 路径 + 串口协议探测的动态 symlink 规则：为左右两侧自动生成 `right_imu` / `left_imu` / `right_encoder` / `left_encoder`，不再直接依赖漂移的 `ttyCH9344USBx` 命名。
- `sensor_recorder` 改为单机双手采集：同时打开 `/dev/right_imu`、`/dev/right_encoder`、`/dev/left_imu`、`/dev/left_encoder`，并将左右 IMU / encoder 分别写入 `sensor_data.mcap` 的独立 channel；`zeroing` 改为显式参数选择 `left` / `right` / 设备路径，不再依赖 `DEVICE_SIDE` 或旧 `/dev/ttyS2`、`/dev/ttyS7`。
## v1.1.15 - 2026-03-11
- 音频播放/录音链路改为优先走 PulseAudio：`audio/audio_play.py` 与 USB 耳机测试脚本不再直接绑定 ALSA `hw/plughw`，而是强制选择型号为 `0020:0b21` 的 USB 耳机 sink/source。
- USB 耳机匹配策略从“指定 serial”放宽为“同型号即命中”：udev 规则与运行时检测均仅校验 `VID:PID=0020:0b21`，支持同款耳机替换。
- 修复 `py_script/usb_audio_mic_test.py`：改用 PulseAudio `parecord/paplay`，避免该耳机仅支持 48k 双声道 ALSA `hw` 采集时的参数不兼容问题。

## v1.1.14 - 2026-03-11
- v2 音频硬件切换为外接 USB 耳机/麦克风（`liyuany USB Audio`, `0020:0b21`）：新增 udev 稳定映射 `/dev/snd/ugripper_usb_audio_{control,playback,capture}`，不再依赖已不存在的 v1 `rockchipes8388`。
- `audio/audio_play.py` 改为强制使用新 USB 音频设备；未识别到耳机时直接报错，不再回退默认声卡。
- 新增 `py_script/usb_audio_play_test.py` 与 `py_script/usb_audio_mic_test.py`，便于现场分别验证耳机播放与麦克风录音/回放。

## v1.1.13 - 2026-03-09
- 三路相机录制链路调整：主摄使用 `ffmpeg v4l2(NV12 1920x1080)->h26x_rkmpp`，左右触觉改为 `v4l2src(MJPEG)->mppjpegdec->appsink->ffmpeg h26x_rkmpp(CQP)`。
- Fays 录制链路回退为 `ffmpeg rawvideo -> h26x_rkmpp`，并修复标定导入时 Fays 图像 `fps` 的来源：优先读取 `fays_vikit.yaml` 的 `stereo_fps`，缺失时写 `unknown`，不再默认写 `60`。
- 修复 `import_camera_calibration.sh` 的 `Residuals` 提取逻辑：按行解析带单位结果，避免导入后 `residuals` 变成 `null`。
- `metadata.json` 新增角色字段、`camera_codec`、`ugripper_version`、`ugripper_usb_updater_version` 与 `data_format_version`，便于后处理识别数据来源、编码配置与版本信息。
- 开机自恢复增强：`boot_check_install.sh` 会像 `ugripper-usb-updater` 一样比较 `/opt/backup` 中的 `ugripper` 版本，备份包更高时自动升级；主包缺失或状态异常时继续执行恢复安装。
- 录制附加信息与停录日志增强：`info.json` 新增各路视频 `*_record_time_offset_us`，`stop_recording` 新增分阶段耗时日志，便于定位写盘与校验耗时。

## v1.1.12 - 2026-03-05
- `sensor_recorder` 新增“开录首条 encoder 样本”日志：首次写入 encoder 数据时打印 `raw/rad/speed/timestamp`，便于快速确认编码器链路已工作。

## v1.1.11 - 2026-03-05
- `run_record.sh` 新增 Fays 开机启动防抖：若开机检测到 Fays，先延时 `FAYS_STARTUP_DELAY_SEC`（默认 3 秒）再拉起 daemon，降低上电早期枚举抖动带来的失败概率。
- 新增低开销 Fays USB 速率检测：通过 sysfs `speed` 每秒刷新 `/dev/shm/umi_fays_usb_speed_mbps`，不做全量 USB 枚举。
- health check 增强：Fays 在位但速率跌落到 `FAYS_USB_ERROR5_MBPS`（默认 480Mb/s）及以下时，上报 `ERROR_5`。

## v1.1.10 - 2026-03-04
- `led_manager.py` 将 `READY` 呼吸灯与 `RECORDING` 闪灯改为基于系统时间相位驱动，左右夹爪在系统时间同步后可保持同相灯效。
- `READY` 呼吸灯改为低开销三角波（整数运算），替代 `sin` 计算，降低常驻 CPU 开销。
- `run_record.sh` 扩展主从启动指令为 `START|episode_xxxx|master_sn`，Master 触发录制时会附带本机 `DEVICE_SN`。
- Left 侧录制流程不限制 Master SN，接收到任意 `master_sn` 均作为本次配对来源。
- Left 停录后会向当前 episode 的 `info.json` 写入 `paired_master_sn`，用于后处理追溯主从配对关系。
- 新增旧角色配置：`run_record.sh` 一度支持按 `master/slave` 角色分流控制逻辑，解耦物理侧 `DEVICE_SIDE` 与主从角色，允许 Left 设备作为 Master。
- `usb_auto_update.sh` 一度支持从 `config.txt` 读取角色键（值 `master|slave`）并写入旧角色环境变量。
- USB 导入流程兼容增强：`config.txt` 与 `ugripper_calib` 允许在同一次触发中统一导入，全部导入完成后仅亮一次完成灯并重启一次 `ugripper.service`。
- 导入失败反馈增强：统一导入阶段任一项失败时切换 `ERROR_1` 红灯错误态提示，再执行服务重启。
- `run_record.sh` 的 episode 结束校验改为强制检查 Fays 文件存在性：`fays_stereo_output.mkv` 与 `fays_data.mcap` 不再按“本轮是否期望 Fays”跳过。
- 新增 `fays_data.mcap` 末尾 IMU 存活校验：要求 topic `i`/`c` 存在且有消息，并检查最后几帧相机时间段内仍有 IMU；可识别 IMU 末尾断流但文件仍存在的场景。
- 修复长时录制下 Fays MCAP 末尾校验误判：`fays_tail_imu_check.py` 改为基于 summary 尾部 chunk 索引逆向读取，仅解析末尾少量 chunk，不再全量扫描导致超时。

## v1.1.9 - 2026-03-02
- Fays 端口策略改为固定 symlink：`run_fays_record.sh` 不再扫描 `/dev/video*` 并动态改写 yaml，启动前仅校验 `/dev/fays_stereo` 与 `/dev/fays_imu`。
- `fays_vikit.yaml` 固定为 `stereo_dev_port=/dev/fays_stereo`、`imu_dev_port=/dev/fays_imu`，避免热插拔后 `videoN` 漂移导致录制空文件。
- `fays_record_example` 的 USB watchdog 新增“symlink 目标漂移”检测：运行中若映射目标变化，按断连处理并退出，由上层维护流程重建。
- 强化 `fays_record_example` 的 USB watchdog 日志：异常时输出 `access/readlink/realpath` 错误码、symlink 目标、baseline 目标、录制态与 session 信息，便于定位断连原因。
- USB watchdog 新增 500ms 去抖确认：避免单次 symlink/udev 短抖动即误判退出；仅在连续异常超过窗口后才按断连退出并交由上层重建。
- 调整 `run_record.sh` 的 Fays 在位缓存读取策略：`is_fays_ftdi_present()` 仅将缓存值 `0` 视为不在位；读取失败、空值或异常值按在位（fail-open）处理。

## v1.1.8 - 2026-03-01
- 新增 U 盘标定导入与持久化：支持按 `DEVICE_SN` 导入 `camchain/imucam`，写入 `/etc/ugripper/config/calibration/calibration.json`，并在每次开录前刷新到 episode `calibration.json`。
- 强化开机自恢复：`boot_check_install.sh` 优先升级 `/opt/backup` 中更新版本的 `ugripper-usb-updater`，再执行主包恢复检查。
- Fays 在位检测改为 udev 固定映射：新增 `/dev/fays_stereo` 与 `/dev/fays_imu`，`run_record.sh` 基于 symlink 刷新 `/dev/shm/umi_fays_present`，避免周期性 `v4l2-ctl` 枚举。
- 录制链路与校验增强：Fays 缺失持续报 `ERROR_2`；时长校验统一收紧到 5 秒（含 `cam vs fays` 与 `tact vs sensor_data.mcap`）；USB `config.txt` 新增语言与编码器配置同步到 `/etc/environment`。

## v1.1.7 - 2026-02-28
- Fays 视频编码调整为 RK3576 硬编码 `h264_rkmpp`，输出 H.264，降低 CPU 占用。
- 三路相机录制脚本统一为 `camera_record/triple_camera_record.py`，新增 `--codec <h264|h265>` 参数，`run_record.sh` 默认使用 `h264`。

## v1.1.6 - 2026-02-27
- 调整 Fays 维护策略：支持运行时自动拉起 daemon/FIFO；若在录制中恢复，仅恢复就绪，不补发 `START`、不续写当前 episode。
- `run_record.sh` 新增按时间戳跨度的数据完整性校验：对期望 Fays 的 episode，对比 `cam.mkv` 与 `fays_stereo_output.mkv` 的 `end-start` 时长；若 Fays 短超过 10 秒或跨度无法读取则判定失败。
- 重构 `fays_record_example` 录制链路：IMU/视频读取线程只负责采集和入队，`fays_data.mcap` 的 `Open/Close/Log` 统一由独立 MCAP 写线程处理，并按 session 隔离写入。
- 入队路径改为“本地 pending + 非阻塞批量重试”，锁竞争时先缓存后重试，降低高负载下的样本丢失风险。
- 增加 Fays USB 节点监控：`record.cpp` 轮询配置中的视频节点，断连时打印错误并主动退出，由上层维护流程重建。
- `led_manager.py` 中 `RECORDING` 状态调整为 1Hz 绿色闪烁。

## v1.1.5 - 2026-02-26
- 新增报错灯效分级：`ERROR_1~ERROR_5`，按严重度区分并使用红灯“长+短码”循环编码（`ERROR_1`=长短，`ERROR_2`=长短短，依次类推）。
- `run_record.sh` 按错误类型映射错误码并按严重度选最高优先级展示；`ERROR` 注释同步维护了各错误分级说明。
- 修复 Left 侧 PTP 同步超时告警刷屏：同一错误内容仅在状态变化或错误内容变化时再次打印，并避免重复触发灯效/音频。
- 将错误码灯效节奏调整为“可读性优先”：显著拉长长亮时长并增大脉冲/轮次间隔，便于肉眼明确读数错误码。
- `RECORDING` 状态灯由绿常亮改为绿闪烁（2Hz），提升录制进行中的视觉可感知性。

## v1.1.4 - 2026-02-27
- 新增 `py_script/fays_kalibr_to_vinsfusion.py`，支持把 Kalibr 文本结果一键转换成 VINS-Fusion 所需的 3 个 YAML 文件。
- `body_T_cam0/body_T_cam1` 分别使用 `Transformation (cam0/cam1)` 的 `T_ic (camX to imu0)`，并同步输出双目 `equidistant` 相机参数。
## v1.1.4 - 2026-02-26
- 在 `faysSense_vi_kit/example/record.cpp` 新增 Fays USB 连接轮询：实时检查配置中的 `stereo_dev_port/imu_dev_port` 设备节点。
- 当检测到 Fays USB 断连（设备节点消失）时，`fays_record_example` 会记录错误并立即退出，避免断连后残留异常实例继续占用控制通道。
- 调整 `run_record.sh` 的维护调度：将 `monitor_system_health` 放回后台监控循环执行，并引入 `/dev/shm/umi_fays_present` 缓存 Fays 在位状态，主按键循环仅执行 Fays 生命周期维护，降低按键偶发不响应概率。

## v1.1.3 - 2026-02-25
- 在 `camera_record/99-fixed-usb-map.rules` 新增 Fays FTDI (`0403:602e`) 的 udev 权限规则，统一放开 `video4linux` 与 `usb` 节点访问权限。
- 便于 Fays 相机在系统启动后无需额外手工执行权限脚本即可被录制流程访问。

## v1.1.2 - 2026-02-25
- 新增 `auto-release-deb` skill：支持代码改动后自动更新 `build_deb.sh` 版本并执行打包。
- 版本策略明确为默认只升级 `.z`，仅在显式指定时升级 `.y/.x`。
- 新增自动判定 `build_deb.sh -q` 与标准模式的规则（基于改动类型与关键二进制是否存在）。
- 三路相机录制调整为主摄 FFmpeg + 触觉 Hybrid（Gst/MPP 解 MJPEG + FFmpeg rkmpp CQP 编码），以兼顾触觉 CPU 占用与 FFmpeg CQP 体积表现。
- `fays_record_example` 视频录制链路改回 FFmpeg `rawvideo -> h26x_rkmpp(CQP)`，不再使用 Gst `fixqp` 编码。
