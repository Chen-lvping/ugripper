# Control Channel Ledger Baseline

本文档冻结 `ugripper` 当前运行时控制面通道事实。

目标是回答两件事：

- 当前哪些 pipe / file 通道属于“未来可能演进为 transport 接口”的控制面
- 当前哪些文件、锁、stdout/stderr 只属于本地实现细节，不应被误纳入 ZMQ 迁移范围

## 1. 当前结论

当前值得重点关注的控制面通道共有 5 条：

1. 音频命令 FIFO：`/tmp/umi_audio_pipe`
2. 音频 ready 标记：`/tmp/umi_audio_ready`
3. stereo 控制文件：`/tmp/umi_stereo_camera_control.json`
4. stereo 状态文件：`/tmp/umi_stereo_camera_status.json`
5. 关机请求文件：`/tmp/umi_shutdown_request`

其中：

- `audio pipe`、`stereo control/status` 属于后续可抽 transport 无关接口的首批候选
- `shutdown request` 也是控制面通道，但更接近 systemd/file 交接点，当前不作为首批 ZMQ 目标
- worker 的 stdout/stderr 管道、`waitpid`、日志同步 `.pos/.lock` 文件、录制锁文件等只属于实现细节，不在本阶段纳入 transport 迁移

## 2. 通道台账

| 通道 | 当前 transport | producer | consumer | payload / 语义 | 失败模式 | 事实来源 |
| --- | --- | --- | --- | --- | --- | --- |
| `/tmp/umi_audio_pipe` | FIFO | `record_runtime` 通过 `AudioCoordinator::SendCommand()`；`run_calibration.sh` 通过 `notify_audio()` 直接写 FIFO | `audio/audio_play.py` | 按行文本命令；当前已知命令包括 `ready`、`writing`、`shutdown`、`error`、`pre_audio_recording`、`post_audio_recording`、`audio_recording_stop`、`no_reset_needed`、`calib_start`、`calibrating`、`calib_done` | FIFO 不存在；daemon 未 ready；写端打开失败时命令丢失；`run_calibration.sh` 会自行 `mkfifo`，属于旁路兼容约束 | `src/record_runtime/src/record_runtime.cpp`, `src/record_runtime/src/runtime_process.cpp`, `audio/audio_play.py`, `auto_calibration/run_calibration.sh` |
| `/tmp/umi_audio_ready` | 普通文件 | `audio/audio_play.py` | `record_runtime` `AudioCoordinator` | 文本 `ready\n`；表示音频后端已绑定可用 sink/source，允许发送正常音效命令 | ready 标记缺失；pipe 已存在但音频后端仍在 warmup；ready 标记丢失会触发 recovery 路径 | `audio/audio_play.py`, `src/record_runtime/src/runtime_process.cpp`, `src/record_runtime/include/record_runtime.h` |
| `/tmp/umi_stereo_camera_control.json` | JSON 文件 | `record_runtime` 通过 `StereoSessionClient::WriteControl()` | `camera_recorder --stereo-daemon` | JSON 对象：`command_seq`、`recording`、`episode_dir`、`start_system_time_us`、`stop_system_time_us`；用于开始/停止 stereo session | 文件缺失或 JSON 不合法时，daemon 跳过本次命令；重复 `command_seq` 被忽略 | `src/record_runtime/src/runtime_process.cpp`, `src/camera_recorder/src/camera_recorder.cpp`, `src/record_runtime/include/record_runtime.h`, `src/camera_recorder/include/camera_recorder/camera_recorder.h` |
| `/tmp/umi_stereo_camera_status.json` | JSON 文件 | `camera_recorder --stereo-daemon` | `record_runtime` 的 `HealthMonitor`、`waitForStereoFinalize()`、`mergeEpisodeInfo()` | JSON 对象：`ready`、`service_state`、`finalize_pending`、`active_episode_dir`、`last_finalized_episode_dir`、`last_finalize_error`、`last_session`、`cameras` | 缺失或 JSON 非法会被视为健康检查故障；session finalize 超时会阻断 stop/merge 路径 | `src/camera_recorder/src/camera_recorder.cpp`, `src/record_runtime/src/runtime_domain.cpp`, `src/record_runtime/src/record_runtime.cpp`, `src/record_runtime/include/record_runtime.h` |
| `/tmp/umi_shutdown_request` | 普通文件 + systemd path watch | `record_runtime::handleDualShutdownAction()` | `umi-shutdown-trigger.path` + `trigger_shutdown.sh` | 文本 `shutdown\n`；表示当前已进入关机流程，请 systemd 侧消费并执行 `poweroff` | 文件写入失败时无法触发 path unit；消费脚本会删除文件后执行 `systemctl poweroff` | `src/record_runtime/src/record_runtime.cpp`, `auto_update/umi-shutdown-trigger.path`, `auto_update/umi-shutdown-trigger.service`, `auto_update/trigger_shutdown.sh` |

## 3. 不纳入首批 ZMQ 迁移的实现细节

下列对象虽然使用 pipe / file / 临时路径，但当前不应被纳入“通信通道迁移”：

| 对象 | 原因 | 事实来源 |
| --- | --- | --- |
| worker stdout/stderr 管道 | 属于 `fork/exec` 后的本地进程控制细节，不是业务控制面协议 | `src/camera_recorder/src/camera_recorder.cpp`, `src/record_runtime/src/runtime_process.cpp` |
| `waitpid` / 进程组 signal | 属于 supervisor 子进程生命周期管理，不是消息 transport | `src/record_runtime/src/runtime_process.cpp` |
| `/tmp/umi_sys_<device>_<date>.pos`、`.lock` | 属于日志增量同步状态，不是运行时控制协议 | `run_record.sh` |
| `/tmp/umi_recording.lock` | 属于安装/升级窗口的残留文件清理，不是主控制面交互 | `pack_script/postinst`, `pack_script/prerm`, `auto_update/usb_auto_update.sh` |

## 4. 冻结后的接口边界建议

当前阶段不改 transport 实现，只冻结未来的接口边界。

### 4.1 `AudioCommandPort`

建议职责：

- 发送音频命令
- 查询后端是否 ready
- 表达“ready 标记”和“命令写入”是同一控制面的两个观察面

当前后端：

- FIFO：`/tmp/umi_audio_pipe`
- ready file：`/tmp/umi_audio_ready`

当前代码锚点：

- `src/record_runtime/src/runtime_process.cpp` 中的 `AudioCoordinator`
- `src/record_runtime/include/record_runtime/audio_command_port.h`
- `auto_calibration/run_calibration.sh` 中直接写 FIFO 的旁路调用，后续也应归一到同一协议口径

### 4.2 `StereoSessionPort`

建议职责：

- 写入开始/停止 session 控制
- 读取 service status
- 等待 finalize 完成

当前后端：

- control file：`/tmp/umi_stereo_camera_control.json`
- status file：`/tmp/umi_stereo_camera_status.json`

当前代码锚点：

- `src/record_runtime/src/runtime_process.cpp` 中的 `StereoSessionClient`
- `src/record_runtime/include/record_runtime/stereo_session_port.h`
- `src/camera_recorder/src/camera_recorder.cpp` 中的 stereo daemon loop

### 4.3 `ShutdownRequestPort`

建议职责：

- 发出“请求关机”信号
- 让运行时逻辑不直接耦合 systemd path unit 细节

当前后端：

- request file：`/tmp/umi_shutdown_request`

当前结论：

- 这是控制面通道，但当前优先级低于 `AudioCommandPort` 和 `StereoSessionPort`
- 第一阶段可继续保留 file backend，不要求立即进入 ZMQ 迁移路线

当前代码锚点：

- `src/record_runtime/include/record_runtime/shutdown_request_port.h`
- `src/record_runtime/src/record_runtime.cpp` 中的 `RecordRuntime::handleDualShutdownAction()`
- `auto_update/trigger_shutdown.sh`

## 5. 与 `pp_main` 的对齐方式

后续若进入主仓库阶段，通信侧应优先对齐：

- `/home/songwl/swl_ws/pp_main/src/communication/publisher.cc`
- `/home/songwl/swl_ws/pp_main/src/communication/subscriber.cc`
- `/home/songwl/swl_ws/pp_main/standalone/Puppetry/communication/message_hub.h`

当前阶段只学习这些设计目标：

- 先定义通信边界，再决定 transport
- 通信对象自己管理生命周期
- 业务层依赖“端口语义”，而不是依赖 pipe/file 细节

当前阶段不做：

- 在 `ugripper` 仓库里直接引入 `zmqpp`
- 直接复刻 `pp_main` 的 `publisher/subscriber/message_hub`
- 把 file / pipe backend 一次性替换掉

## 6. 当前完成口径

`8.6` 第一轮完成的标准定义为：

- 通道台账已冻结
- 每条通道都明确了 producer / consumer / payload / failure mode / source
- 已区分“未来 transport 候选”和“本地实现细节”
- 已冻结 `AudioCommandPort` / `StereoSessionPort` / `ShutdownRequestPort` 这 3 类接口边界

后续步骤再进入：

- 接口代码抽象
- fake/stub 单测
- 主仓库阶段的 ZMQ backend 实现与节点级 smoke

当前 `8.6` 进度补充：

- `AudioCommandPort` 代码级抽象已完成
- `StereoSessionPort` 代码级抽象已完成
- `ShutdownRequestPort` 代码级抽象已完成
