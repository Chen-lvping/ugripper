# Changelog

## v1.1.6 - 2026-02-27
- Fays 视频编码从 `libx264` 回退为 RK3576 硬编码 `h264_rkmpp`，降低 CPU 占用。
- 三路相机录制脚本统一为 `camera_record/triple_camera_record.py`，新增 `--codec <h264|h265>` 可选编码，`run_record.sh` 默认使用 `h264`。
- `fays_record_example` 视频编码从 `hevc_rkmpp` 切换为 `libx264` CPU 软编码，输出 H.264，降低硬件编码器占用。
- 调整 Fays 维护策略：支持运行时自动拉起 daemon/FIFO；若在录制中恢复，仅恢复就绪，不补发 `START`、不续写当前 episode。
- `run_record.sh` 数据完整性校验增强：统一时长误差阈值为 5 秒；`cam.mkv` 与 `fays_stereo_output.mkv` 的短缺阈值从 10 秒收紧到 5 秒，并新增 `tact_left.mkv`/`tact_right.mkv` 与 `sensor_data.mcap` 的时长误差校验（MCAP 仅读取 summary 起止时间，不做全量扫描）。
- Fays 缺失策略调整：不再视为"正常无 Fays 模式"；启动或运行中缺失时持续触发 `ERROR_2` 报错并保持服务运行（不修改 `start_recording` 现有启动路径）。
- 重构 `fays_record_example` 录制链路：IMU/视频读取线程只负责采集和入队，`fays_data.mcap` 的 `Open/Close/Log` 统一由独立 MCAP 写线程处理，并按 session 隔离写入。
- 入队路径改为“本地 pending + 非阻塞批量重试”，锁竞争时先缓存后重试，降低高负载下的样本丢失风险。
- 增加 Fays USB 节点监控：`record.cpp` 轮询配置中的视频节点，断连时打印错误并主动退出，由上层维护流程重建。
- `led_manager.py` 中 `RECORDING` 状态调整为 1Hz 绿色闪烁。
- USB 自动升级新增语言配置读取：若升级 U 盘根目录存在 `config.txt` 且配置 `LANGUAGE/VOICE_LANG`，会更新 `/etc/environment` 的 `UGRIPPER_LANG`（`zh|en`）；`audio_play.py` 按该变量优先播放 `audio_en`，缺失文件自动回退中文目录。

## v1.1.5 - 2026-02-26
- 新增报错灯效分级：`ERROR_1~ERROR_5`，按严重度区分并使用红灯“长+短码”循环编码（`ERROR_1`=长短，`ERROR_2`=长短短，依次类推）。
- `run_record.sh` 按错误类型映射错误码并按严重度选最高优先级展示；`ERROR` 注释同步维护了各错误分级说明。
- 修复 Left 侧 PTP 同步超时告警刷屏：同一错误内容仅在状态变化或错误内容变化时再次打印，并避免重复触发灯效/音频。
- 将错误码灯效节奏调整为“可读性优先”：显著拉长长亮时长并增大脉冲/轮次间隔，便于肉眼明确读数错误码。
- `RECORDING` 状态灯由绿常亮改为绿闪烁（2Hz），提升录制进行中的视觉可感知性。

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
