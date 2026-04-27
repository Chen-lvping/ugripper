# `pp_main` 合并后 `ugripper` 正式测试方案

本文档用于定义 `ugripper` 合并到 `pp_main` 后的软件侧正式测试范围、测试分层、测试清单、执行方式与补测优先级。

本文档只讨论软件侧测试，不把硬件结构整改、线缆接触、供电不稳等物理问题当成测试设计本身的一部分。

如需先快速看“测试结构 + 主要测试项 + 当前结果快照”，可先读 [ppmain-ugripper-test-summary.md](/home/songwl/swl_ws/ugripper/docs/ppmain-ugripper-test-summary.md)。

## 0. 当前本地现状

截至 `2026-04-20`，本文档已按当前本地仓库状态重新对照：

- `pp_main/test`
- `pp_main/standalone`
- `pp_main/test/scripts`

本轮文档校对的目标不是再次确认 `v2` 同步，而是确认：

- 当前测试方案文档是否和本地 `pp_main` 里的测试代码、分析器、板端脚本一致
- 哪些测试资产已经落地并接入 `CMake/CTest`
- 哪些测试资产已经存在但仍主要停留在脚本/人工触发层
- 哪些更新后的运行时逻辑仍缺少对应测试

当前应明确区分 3 种状态，避免后续口径混淆：

- `已落地`
  - 代码、脚本或 analyzer 已存在，并已进入仓库
- `已接线`
  - 已被 `CMake/CTest` 或板端标准脚本入口纳入，可重复执行
- `已重跑验证`
  - 已在当前代码版本上实际执行并确认通过

当前测试工作的主要目标已经从“补同步”转为“把已有测试资产盘清、接线规范化，并补齐更新后代码的关键空白测试”。

### 0.1 最近一次板端重跑结果

截至 `2026-04-27`，最新一次板端包安装后最小 smoke 已对 `1.2.8` 包完成实时复核；`2026-04-21` 的 `merge8/merge13/merge14` 结果仍保留为历史分项基线，不能与 `1.2.8` 直接混作同一 release gate 结论。

本轮已确认通过的板端检查如下：

- `ugripper_1.2.8_arm64.deb` 安装后最小 smoke（`2026-04-27`）
  - 板端：`ubuntu@192.168.2.240`，主机名 `HSD-RB1021`，架构 `aarch64`
  - 结果目录：`tmp/board_results/package_smoke_20260427_rerun`
  - 汇总：`tmp/board_results/package_smoke_20260427_rerun/test_summary.json`
  - 原始输出：`tmp/board_results/package_smoke_20260427_rerun/ssh_smoke_output.txt`
  - 结论：`ok=true`
  - 说明：
    - `dpkg -s ugripper -> Version: 1.2.8`
    - `ugripper.service -> active`
    - `UgripperRuntime`、`audio_play.py`、`CameraRecorder --stereo-daemon` 均在运行
    - `/tmp/umi_stereo_camera_status.json` 存在，左右 stereo `ready=true`
    - 最近日志包含 `[HMI_DIAG]`、`[GRIPPER_DIAG]` 与 `validation phase end ... valid=true`
  - 边界：
    - 本轮是安装后最小运行 smoke，不等同于完整 `T16 release gate`
    - 尚未覆盖同一 `1.2.8` 包版本下的 camera / sensor / service / gripper_hmi 全套脚本重跑
- `ugripper 1.2.8` 分项 smoke 补跑（`2026-04-27`）
  - 板端：`ubuntu@192.168.2.240`，主机名 `HSD-RB1021`，架构 `aarch64`
  - 结果目录：`tmp/board_results/full_smoke_20260427`
  - 汇总：`tmp/board_results/full_smoke_20260427/test_summary.json`
  - 结论：`ok=true`，但存在板端测试工具链问题
  - 已通过：
    - `board_camera_stress.sh --duration-sec 10 --cpu-workers 0`：左右主摄约 `9.77s`，窗口检查与左右 span 对齐通过
    - `board_sensor_smoke.sh --duration-sec 6`：录制产物已生成；拉回本地后，当前仓库 `check_sensor_mcap` 可解析左右 MCAP，`imu/encoder` topic、sequence gap、timestamp regression 基本检查通过
    - `board_service_integration_check.sh --skip-sensor-check`：episode 文件齐全，左右主摄视频检查通过，`gripper_report.json` 与 `hmi_button_report.json` 均通过
  - 当前阻塞：
    - 板端 `/home/ubuntu/pp_main/test/scripts/check_sensor_mcap` 报 `unsupported compression: lz4`
    - 因此不带 `--skip-sensor-check` 的 `board_sensor_smoke.sh` 与 `board_service_integration_check.sh` 在板端原地 analyzer 阶段失败
  - 边界：
    - 本轮 camera 为 `10s / no cpu load` 快速 smoke，不等同于标准压力参数
    - 本轮没有重跑人工视觉确认型 `gripper_hmi_active_check`
    - 完整 `T16 release gate` 仍需先更新板端 `check_sensor_mcap`，再按同一包版本重跑统一汇总
- `ugripper 1.2.8` sensor analyzer 修复后重跑（`2026-04-27`）
  - 板端：`ubuntu@192.168.2.240`，主机名 `HSD-RB1021`，架构 `aarch64`
  - 变更：
    - 使用 `ugripper-arm` 容器交叉编译当前仓库 `check_sensor_mcap`
    - 替换板端 `/home/ubuntu/pp_main/test/scripts/check_sensor_mcap`
    - 旧 checker 已备份到 `/home/ubuntu/pp_main/test/scripts/check_sensor_mcap.nolz4.bak_20260427`
  - 结果目录：`tmp/board_results/full_smoke_lz4_checker_20260427`
  - 汇总：`tmp/board_results/full_smoke_lz4_checker_20260427/test_summary.json`
  - 结论：`ok=true`
  - 已通过：
    - 新 checker 在板端原地解析 `lz4` MCAP 通过，`board_sensor_analyzer_lz4_supported=true`
    - `board_sensor_smoke.sh --duration-sec 6` 通过，左右 `sensor_data_*.mcap` 均通过 topic、message count、sequence gap 与 timestamp regression 基本检查
    - `board_service_integration_check.sh` 不再需要 `--skip-sensor-check`，episode 文件、sensor MCAP、左右主摄视频、gripper/HMI 日志分析均通过
  - 边界：
    - 本轮仍未补跑标准 CPU 压力 camera stress，也未执行人工视觉确认型 `gripper_hmi_active_check`
    - 当前已经关闭 `1.2.8` 包在 sensor analyzer 上的 `lz4` 阻塞点，但还不能直接等同于完整 `T16 release gate`
- `ugripper 1.2.8` 标准 camera stress 重跑（`2026-04-27`）
  - 板端：`ubuntu@192.168.2.240`，主机名 `HSD-RB1021`，架构 `aarch64`
  - 结果目录：`tmp/board_results/camera_stress_lz4_checker_20260427`
  - 汇总：`tmp/board_results/camera_stress_lz4_checker_20260427/test_summary.json`
  - 结论：`ok=true`
  - 执行参数：
    - `duration_sec=30`
    - `cpu_workers=7`
    - `only=left_cam_main,right_cam_main,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r`
  - 说明：
    - `left_cam_main.mkv duration_sec=29.800595`
    - `right_cam_main.mkv duration_sec=29.817262`
    - 左右 span gap `0.017s`
    - 左右主摄全部 `2s` 窗口检查通过，`validation_reason=ok`
  - 边界：
    - 当前同一 `1.2.8` 包版本下，camera stress、sensor smoke、service integration 均已有通过记录
    - 仍缺人工视觉确认型 `gripper_hmi_active_check` 和统一 release gate 汇总入口
- `ugripper 1.2.8` 自动化 suite 统一汇总（`2026-04-27`）
  - 结果目录：`tmp/board_results/release_gate_1_2_8_20260427`
  - 汇总：`tmp/board_results/release_gate_1_2_8_20260427/test_summary.json`
  - 纳入 suite：
    - `camera -> ok=true`
    - `sensor -> ok=true`
    - `service -> ok=true`
  - 结论：
    - `ok=true`
    - `release_gate_ready=false`
    - `validation_reason=missing_required_suite`
    - `missing_suites=["gripper_hmi"]`
    - `uniform_package_version=true`
    - `package_versions=["1.2.8"]`
  - 边界：
    - 这份汇总说明自动化三项已经同包版本通过
    - 仍需现场人工执行 `board_gripper_hmi_active_check.sh` 后，才能把 `gripper_hmi` suite 纳入完整 release gate
- `ugripper 1.2.8` gripper/HMI 人工 suite 与完整 release gate（`2026-04-27`）
  - 板端：`ubuntu@192.168.2.240`，主机名 `HSD-RB1021`，架构 `aarch64`
  - gripper/HMI 结果目录：`tmp/board_results/gripper_hmi_active_1_2_8_manual_20260427`
  - gripper/HMI 汇总：`tmp/board_results/gripper_hmi_active_1_2_8_manual_20260427/test_summary.json`
  - 完整 release gate 汇总：`tmp/board_results/release_gate_1_2_8_20260427/test_summary_with_gripper_hmi.json`
  - gripper/HMI 结论：`ok=true`
  - 已通过：
    - direct HMI 人工确认：`READY / RECORDING / ERROR_1 / beep` 均通过
    - button phase analyzer：`ShortUpPressed`、`ShortDownPressed`、`ShutdownPromptRequested` 均命中
    - service 录制样本：`episode_20260427_0003`，日志显示 `validation phase end ... valid=true`
    - `gripper_report.json -> ok=true`
    - `hmi_button_report.json -> ok=true`
  - 完整 release gate 结论：
    - `ok=true`
    - `release_gate_ready=true`
    - `validation_reason=ok`
    - `missing_suites=[]`
    - `uniform_package_version=true`
    - `package_versions=["1.2.8"]`
  - 边界：
    - 原 `board_gripper_hmi_active_check.sh` 在 direct 阶段后，进入 button 阶段前卡在 `board_service_restart_check.sh` 的瞬时 stereo status 判定；随后确认服务与 stereo status 实际恢复为 ready
    - button phase 因此采用人工动作 + `board_gripper_hmi_log_check.sh --journal-since "2026-04-27 16:38:21"` 收口
    - 后续应修正 `board_service_restart_check.sh`：status 文件存在但内容尚未 ready 时应继续等待，而不是立即失败
- `board_camera_stress.sh`
  - 结果目录：`tmp/board_results/camera_stress_20260421_110053`
  - 汇总：`tmp/board_results/camera_stress_20260421_110053/test_summary.json`
  - 结论：`ok=true`，`validation_reason=ok`
  - 说明：左右主摄约 `29s` 压力录制均通过窗口级帧数与解码检查，`failure_types={}`，未复现主摄约 `20s` 坏段问题
- `board_sensor_smoke.sh`
  - 结果目录：`tmp/board_results/sensor_smoke_20260421_111554`
  - 汇总：`tmp/board_results/sensor_smoke_20260421_111554/test_summary.json`
  - 结论：`ok=true`，`validation_reason=ok`
  - 说明：左右 `MCAP` 均通过 analyzer 检查；本轮样本中 `sequence_gap_count=0`、`timestamp_regression_count=0`，未发现短时录制下的 `sensor frame` 丢失
- `board_service_integration_check.sh`
  - 结果目录：`tmp/board_results/service_integration_20260421_111733`
  - 汇总：`tmp/board_results/service_integration_20260421_111733/episode_summary.json`
  - 结论：`ok=true`，`validation_reason=ok`
  - 说明：真实 service episode 产物齐全，视频、sensor、gripper、hmi 汇总均为通过；主摄 episode 时长约 `164.87s`，左右 span 差约 `0.004s`
- `board_gripper_hmi_active_check.sh` 第一轮非交互 smoke
  - 结果目录：`tmp/board_results/gripper_hmi_active_smoke_20260421`
  - 汇总：`tmp/board_results/gripper_hmi_active_smoke_20260421/direct_hmi_visual.txt`
  - 执行方式：`--non-interactive --skip-button-phase`
  - 结论：direct HMI `READY / RECORDING / ERROR_1 / beep` 命令链执行完成，脚本结束后 `ugripper.service` 恢复为 `active`
  - 说明：这轮是板端脚本机制 smoke，不包含人工视觉确认，也不包含 button 序列与 analyzer 收口
- `board_gripper_hmi_active_check.sh` 第一轮人工交互
  - 结果目录：`tmp/board_results/gripper_hmi_active_20260421_144024`
  - 汇总：`tmp/board_results/gripper_hmi_active_20260421_144024/test_summary.json`
  - 结论：`ok=false`，`validation_reason=direct_hmi_led_not_visible_and_diag_not_emitted`
  - 说明：
    - direct HMI `READY` 命令已成功发送，`direct_ready.log` 显示 `Connected 2 gripper HMI device(s)` 且 `LED state mode: READY`，但人工观察结果为 `step=ready status=failed`
    - button phase 的 raw service log 已记录 `BTN_UP short press`、`BTN_DOWN short press`、`recording started`、`validation phase end valid=true`、`dual-button chord armed` 与 `playing: shutdown`
    - 但当前板端安装包的 `journalctl` 中未出现 `[HMI_DIAG]` / `[GRIPPER_DIAG]` 行，导致 `board_gripper_hmi_log_check.sh` 无法用 analyzer 直接收口 `ShortUpPressed / ShortDownPressed / ShutdownPromptRequested / Recording`
    - 已进一步确认：板端已安装 `1.2.8+merge8` 的 `/opt/ugripper/bin/UgripperRuntime/UgripperRuntime` 与 `/opt/ugripper/bin/GripperHmiTool/GripperHmiTool` 二进制中均不包含 `HMI_DIAG / GRIPPER_DIAG` 字符串；而本地当前 `pp_main/build/arm/standalone` 与 `build/package/arm/ugripper_stage` 产物中包含这些字符串，说明当前板端包落后于本地代码，不能用来完成 analyzer 口径的 `T15`
- `board_gripper_hmi_active_check.sh` / 手动按键重跑（`1.2.8+merge9`）
  - 板端现状：
    - 已重新安装 `1.2.8+merge9`
    - `/opt/ugripper/bin/UgripperRuntime/UgripperRuntime` 与 `/opt/ugripper/bin/GripperHmiTool/GripperHmiTool` 已确认包含 `HMI_DIAG / GRIPPER_DIAG`
  - 当前重跑观察：
    - direct HMI `READY` 视觉确认仍失败，但 runtime 已稳定输出 `HMI_DIAG led_target` 与 `GRIPPER_DIAG io_summary`
    - 第 1 次按键重跑在 `ShortUpPressed -> Recording` 后出现 `camera recorder exited unexpectedly`，随后 `stereo finalize failed` / `episode validation failed`，LED 切到 `Error5`
    - 第 2 次按键重跑在进入 `Recording` 后出现瞬时 `hardware health fault (hmi_ports_inactive): HMI ports inactive: /dev/left_gripper`，随后 `health recovered recording=1`
    - 第 3 次按键重跑保留样本：
      - 现场 episode：`/mnt/data_disk/dap912263b000689/data/episode_20260421_0006`
      - 现象：短按 `up` 后约 `220ms` 即出现 `camera recorder exited, last_exit=1`，runtime 立即执行 `camera or sensor recorder exited unexpectedly` 停录回滚，最终 `episode validation failed` 且 LED 进入 `Error5`
      - 产物：episode 中仅保留 `left/right_stereo.mkv`、左右 `sensor_data_*.mcap` 与元数据文件；`left/right_cam_main.mkv` 和全部常规 tactile `mkv` 均缺失
      - 结论：这次已经不是“短录后校验失败”，而是 `camera_recorder` 启动早期直接退出；`ShortUpPressed` 已命中，因此按键链路本身不是当前主阻塞点
    - 当前只读排查补充：
      - `/dev/left_cam_main`、`/dev/right_cam_main` 与安装后的 `camera_recorder.yaml` 均存在，当前不是简单的设备节点缺失或打包错文件
      - 板端 `/opt/ugripper/bin/CameraRecorder/CameraRecorder` 未链接 `gstreamer` 库，当前主摄实际走 shell `gst-launch-1.0` 路径，触觉/双目走 `ffmpeg`
      - 当前板端没有残留常规 `CameraRecorder/gst-launch/ffmpeg` 占用主摄或 tactile 设备，只有 stereo daemon 持有 `/dev/left_stereo`、`/dev/right_stereo`
- `board_gripper_hmi_active_check.sh` / 人工交互重跑（`1.2.8+merge13`）
  - 结果目录：`tmp/board_results/gripper_hmi_active_merge13_manual`
  - 汇总：
    - `tmp/board_results/gripper_hmi_active_merge13_manual/test_summary.json`
    - `tmp/board_results/gripper_hmi_active_merge13_manual/gripper_report.json`
    - `tmp/board_results/gripper_hmi_active_merge13_manual/hmi_button_report.json`
  - 结论：`ok=true`，`validation_reason=pass`
  - 说明：
    - direct HMI 人工观察：
      - `READY` 看到绿色
      - `RECORDING` 看到亮灭交替
      - `ERROR_1` 看到红灯闪烁
      - `beep` 已听到
    - button phase analyzer 收口：
      - `ShortUpPressed`、`ShortDownPressed`、`ShutdownPromptRequested` 已命中
      - `Ready`、`Recording` LED target 已命中
      - 本轮 service 录制样本 `episode_20260421_0019` `validation phase end ... valid=true`
      - `gripper_report.json -> ok=true`
      - `hmi_button_report.json -> ok=true`
    - 仍需注意的边界：
      - 日志里仍出现过一次 `health_fault key=stereo_status_missing led_state=Error2`，随后 `health_recovered recording=0`；本轮未阻塞 `T15` 通过，但后续若要做更严格 release gate，仍应继续观察这类瞬时 fault 的稳定性
- `T16` 第一版统一验收汇总
  - 结果目录：`tmp/board_results/release_gate_merge13_20260421`
  - 汇总：`tmp/board_results/release_gate_merge13_20260421/test_summary.json`
  - 结论：
    - `ok=true`
    - `release_gate_ready=false`
    - `validation_reason=mixed_package_versions`
  - 说明：
    - 当前统一汇总入口已落地，能把 `camera / sensor / service / gripper_hmi` 四类 suite 收成单一 JSON
    - 这轮汇总明确显示：当前纳入的 suite 都已 individually pass，但证据仍混合了
      - `camera/sensor/service -> 1.2.8+merge8`
      - `gripper_hmi -> 1.2.8+merge13`
    - 因此它可以作为“当前软件侧测试现状”的统一入口，但还不能当作“单一安装包版本已完整通过 release gate”的最终结论
      - 已手动补触发 `apply_main_camera_roll_once.sh`，`/run/ugripper/uvc_roll` 已生成左右 stamp，说明 roll helper 本身可执行
      - 补触发后直接运行底层主摄命令：
        - `gst-launch-1.0 ... device=/dev/left_cam_main ...` 可稳定生成 `~16s` `mkv`
        - `gst-launch-1.0 ... device=/dev/right_cam_main ...` 也可稳定生成 `~2.4s` `mkv`
        - 结论：主摄设备本身、HEVC 取流和底层 `gst-launch` 命令链都不是当前主根因
      - 进一步对 `CameraRecorder --only left_cam_main` 做 `strace -ff`：
        - 父进程在 `StartAll()` 的启动线程里 `fork()` 出 shell child
        - child 会执行 `ConfigureManagedChildProcessGroup()`，其中设置 `prctl(PR_SET_PDEATHSIG, SIGKILL)`
        - 启动线程返回后，`wait4()` 很快观察到该 child 被 `SIGKILL`
      - 当前定位边界：
        - 这条 `strace` 现象说明 `camera_recorder` 启动早退确实需要继续盯 `shell child` 启动链
        - 但相关实现早于 `merge8` 已存在，不能直接当作 `merge9` 才引入的根因
        - 因此下一步应优先回到 `merge8 -> merge9` 的安装包与脚本差异，确认真正回归点
  - 结论边界：
    - analyzer 诊断日志链路已恢复，`T15` 不再被“板端包不含诊断日志”阻塞
    - 当前剩余阻塞点已经转为真实运行时问题：
      - LED 视觉仍不生效
      - 手动录制链路在板端重跑中仍存在 `camera recorder unexpected exit` 与 `hmi_ports_inactive` 瞬时故障
      - 其中当前最新主阻塞点已经从“HMI/button 是否生效”收敛到 `camera_recorder` 启动早退，但具体引入点仍待 `merge8 -> merge9` 差异确认
- `T16` / `merge13` 同包重跑补充（`2026-04-21`）
  - 结果目录：
    - `tmp/board_results/sensor_smoke_merge13_20260421`
    - `tmp/board_results/service_integration_merge13_20260421`
    - `tmp/board_results/camera_stress_merge13_20260421`
    - `tmp/board_results/release_gate_merge13_rerun_20260421`
  - 汇总：
    - `tmp/board_results/sensor_smoke_merge13_20260421/test_summary.json`
    - `tmp/board_results/service_integration_merge13_20260421/episode_summary.json`
    - `tmp/board_results/camera_stress_merge13_20260421/test_summary.json`
    - `tmp/board_results/release_gate_merge13_rerun_20260421/test_summary.json`
  - 结论：
    - `sensor -> ok=true`
    - `service -> ok=true`
    - `gripper_hmi -> ok=true`
    - `camera -> ok=false`, `validation_reason=missing_main_videos`
    - 统一汇总：
      - `ok=false`
      - `release_gate_ready=false`
      - `validation_reason=suite_failed`
      - `uniform_package_version=true`
      - `package_versions=["1.2.8+merge13"]`
  - 说明：
    - 当前 `merge13` 已不再是“证据混合不同包版本”的问题，而是同一安装包版本下真实存在 `camera` suite 失败
    - 这轮 `board_camera_stress.sh` 重跑只留下：
      - `camera_stress_run.log` 空文件
      - `info.json`
      - 缺失 `left_cam_main.mkv`、`right_cam_main.mkv`
    - 进一步做板端最小复现后，现象收敛为：
      - `left/right main` 双路直录在无压力和 `7` 个 CPU busy-loop 压力下都能生成正常 `mkv`
      - 一旦切回 `6` 路常规视频组合
        - 无压力：仅生成 `info.json`
        - `7` 个 CPU busy-loop 压力：也仅生成 `info.json`
      - 即使先停掉 `ugripper.service`，`6` 路常规视频组合仍然只生成 `info.json`
    - 因此当前最新阻塞点不是 `main-only` 主摄起录，也不是单纯 `90%+ CPU` 压力，而是 `6` 路常规视频组合路径本身仍未稳定

本轮结果已经可以确认：

- `merge8` 阶段的板端重跑已经证明：独立主摄压力录制、sensor smoke 与 service 集成录制这三条链路都可以在板端真实跑通
- `merge13` 同包重跑已经证明：`sensor`、`service`、`gripper_hmi/button` 这三条链路在当前安装包上可以独立收口通过
- `merge13` 当前不能被视为 release gate 通过，因为 `camera` suite 在同包重跑中真实失败，且失败已收敛到 `6` 路常规视频组合路径
- 当前最新 `camera` 失败形态不是窗口级帧数坏段，也不是 `validation_error.log` 里的短录 metadata 问题，而是直录阶段就没有生成主摄与 tactile `mkv`
- 已具备可追溯的结构化 JSON 结果，不再只依赖临时命令行输出
- `merge14` / 回退版最小板端验证（`2026-04-22`）
  - 板端安装包：
    - `dpkg -s ugripper -> Version: 1.2.8+merge14`
    - `ugripper.service -> active`
  - 重要边界修正：
    - 后续核对发现，这轮 `merge14` 实际安装到板端的 `CameraRecorder/UgripperRuntime` 仍对应旧的 `install/arm` 产物
    - 原因是打包脚本读取 `install/arm`，而当时 `install/arm` 尚未同步到本轮刚编出的回退版二进制
    - 因此 `merge14` 不应被当作“正式安装包已包含回退修复”的最终证据
  - direct `CameraRecorder` 对照：
    - 临时回退版二进制：`/tmp/CameraRecorder_merge9rollback`
    - 安装版二进制：`/opt/ugripper/bin/CameraRecorder/CameraRecorder`
    - 同一条 direct 命令：
      - `timeout -s INT 8s ... --only left_cam_main,right_cam_main,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r`
    - 结果：
      - 回退版生成 `info.json + 6` 路常规视频
      - 安装版仍只生成 `info.json`
    - 当前意义：
      - 这进一步证明当前 `merge13` 的 `camera` suite 失败，至少一部分收敛在 `CameraRecorder` 实现差异，而不只是 service 外层 stop policy
  - 最短 service 动作验证：
    - 人工动作：
      - 短按 `UP` 开始录制
      - 短按 `DOWN` 停止录制
    - episode：
      - `/mnt/data_disk/dap912263b000689/data/episode_20260422_0001`
    - 日志关键结果：
      - `recording started`
      - `stop camera_recorder done: ok=true elapsed_ms=250`
      - `stop sensor_recorder done: ok=true elapsed_ms=50`
      - `validation phase end: ... valid=true elapsed_ms=988`
    - episode 抽样：
      - `validation_error.log` 不存在
      - `left_cam_main.mkv start_time=0.502 duration=3.720598 nb_read_packets=224`
      - `right_cam_main.mkv start_time=0.521 duration=3.720560`
      - `left_tcam_l.mkv start_time=0.000 duration=4.008000 nb_read_packets=480`
      - episode 目录内 `camera/sensor/stereo` 产物齐全
    - 当前意义：
      - `merge14` 最短 service 录制已恢复通过
      - 但这还不能替代正式的 `board_camera_stress.sh` / `board_service_integration_check.sh` 收口
- `merge15` / 正式安装包板端验证（`2026-04-22`）
  - 打包与安装修正：
    - 先重新执行 `cmake --build build/arm/standalone --target CameraRecorder UgripperRuntime install -j8`
    - 再生成并安装 `1.2.8+merge15`
    - 重新核对哈希后确认：
      - 板端 `/opt/ugripper/bin/CameraRecorder/CameraRecorder`
      - 板端 `/opt/ugripper/bin/UgripperRuntime/UgripperRuntime`
      - 已对应本轮更新后的 `install/arm` 产物
  - `board_camera_stress.sh`
    - 结果目录：`tmp/board_results/board_camera_stress_merge15_20260422`
    - 汇总：`tmp/board_results/board_camera_stress_merge15_20260422/test_summary.json`
    - 结论：
      - `ok=true`
      - `validation_reason=ok`
      - `left_cam_main duration_sec=29.339781`
      - `right_cam_main duration_sec=29.399918`
      - `max span gap ≈ 0.06s`
    - 说明：
      - `30s`、`7` 个 CPU busy-loop、开启 `decode-check` 条件下左右主摄窗口级帧数与解码检查均通过
      - 这轮已经覆盖了你最初关心的“主摄约 20s 坏段 + 高 CPU 压力”场景的第一版正式回归
  - `board_service_integration_check.sh`
    - 结果目录：`tmp/board_results/board_service_integration_merge15_20260422`
    - 汇总：`tmp/board_results/board_service_integration_merge15_20260422/episode_summary.json`
    - episode：`/mnt/data_disk/dap912263b000689/data/episode_20260422_0002`
    - 结论：
      - `ok=true`
      - `validation_reason=ok`
      - `sensor/video/gripper/hmi` 全部通过
      - `left_cam_main duration_sec=3.38462`
      - `right_cam_main duration_sec=3.420601`
    - 说明：
      - 这轮已经把当前正确安装的 `merge15` 包在板端的最短 service 路径收口成正式 summary，而不再只依赖人工观察和日志摘录
  - `board_sensor_smoke.sh`
    - 结果目录：`tmp/board_results/board_sensor_smoke_merge15_20260422`
    - 汇总：`tmp/board_results/board_sensor_smoke_merge15_20260422/test_summary.json`
    - 结论：
      - `ok=true`
      - `validation_reason=ok`
      - `encoder_left/right messages=5712`
      - `imu_left/right messages=1163`
    - 说明：
      - 左右 `MCAP` 都通过 analyzer 检查
      - 本轮样本中未出现 `sequence gap`、`timestamp regression` 或左右跨度不一致
  - `board_gripper_hmi_active_check.sh` / 人工交互重跑（`1.2.8+merge15`）
    - 结果目录：`tmp/board_results/gripper_hmi_active_merge15_manual`
    - 汇总：
      - `tmp/board_results/gripper_hmi_active_merge15_manual/test_summary.json`
      - `tmp/board_results/gripper_hmi_active_merge15_manual/gripper_report.json`
      - `tmp/board_results/gripper_hmi_active_merge15_manual/hmi_button_report.json`
    - 结论：
      - `ok=true`
      - `validation_reason=pass`
    - 说明：
      - direct HMI 人工观察：
        - `READY` 看到绿色；体感持续时间偏短，但状态可见
        - `RECORDING` 看到绿灯闪烁
        - `ERROR_1` 看到红灯闪烁
        - `beep` 已听到
      - button phase analyzer 收口：
        - `ShortUpPressed`、`ShortDownPressed`、`ShutdownPromptRequested` 已命中
        - `Ready`、`Recording` LED target 已命中
        - 本轮 service 录制样本 `episode_20260422_0004` 无 `validation_error.log`
        - `journalctl` 显示 `validation phase end ... valid=true`
      - 中断样本边界：
        - 首次尝试中的 `episode_20260422_0003` 因测试过程中人工 `restart service` 被中断，最终报 `timed out waiting for stereo session metadata`
        - 该样本只用于保留一次瞬时 `hmi_ports_inactive` 观测，不计入正式 button-phase 通过/失败结论
  - `T16` / `merge15` 同包统一汇总
    - 结果目录：`tmp/board_results/release_gate_merge15_20260422`
    - 汇总：`tmp/board_results/release_gate_merge15_20260422/test_summary.json`
    - 结论：
      - `ok=true`
      - `release_gate_ready=true`
      - `validation_reason=ok`
      - `uniform_package_version=true`
      - `package_versions=["1.2.8+merge15"]`
    - 说明：
      - 当前测试计划中定义的 4 个 release-gate suite
      - `camera`
      - `sensor`
      - `service`
      - `gripper_hmi`
      - 已全部在同一安装包版本 `1.2.8+merge15` 上收口通过
      - 这意味着当前 `pp_main` 合并后的 `ugripper` 第一版正式板端回归已经完成闭环，不再停留在“不同包版本的混合证据”
  - `merge15` / `gripper_hmi` 瞬时 fault probe soak（`2026-04-22`）
    - 结果目录：`tmp/board_results/gripper_hmi_fault_soak_merge15_probe_3cycles_20260422`
    - 汇总：`tmp/board_results/gripper_hmi_fault_soak_merge15_probe_3cycles_20260422/health_soak_summary.json`
    - 覆盖范围：
      - 连续 `3` 轮人工 `UP -> DOWN` 录制循环
      - 样本：`episode_20260422_0005`、`episode_20260422_0006`、`episode_20260422_0007`
    - 结论：
      - `ok=true`
      - `recording_started_count=3`
      - `validation true count=3`
      - `total_health_faults=0`
      - `hmi_ports_inactive=0`
    - 说明：
      - 这轮 probe soak 没有复现此前出现过的瞬时 `hmi_ports_inactive`
      - 因此当前更合理的判断是：该问题属于低频/间歇性现象，尚不能靠短轮次人工循环稳定复现
      - 下一步增强回归应继续扩大轮次，必要时叠加 CPU 压力或更长时长观察
  - `merge15` / `gripper_hmi` 10-cycle soak（`2026-04-22`）
    - 结果目录：`tmp/board_results/gripper_hmi_fault_soak_merge15_10cycles_20260422`
    - 汇总：`tmp/board_results/gripper_hmi_fault_soak_merge15_10cycles_20260422/health_soak_summary.json`
    - 覆盖范围：
      - 连续 `10` 轮人工 `UP -> DOWN` 录制循环
      - 样本：`episode_20260422_0008` 到 `episode_20260422_0017`
    - 结论：
      - `ok=true`
      - `recording_started_count=10`
      - `validation true count=10`
      - `ShortUpPressed=10`
      - `ShortDownPressed=10`
      - `total_health_faults=0`
      - `hmi_ports_inactive=0`
    - 说明：
      - 这轮比前面的 `3-cycle probe` 更长，但仍未复现瞬时 `hmi_ports_inactive`
      - 因此当前软件侧最稳妥的结论是：
      - 该 fault 不是“常规 1 轮或 10 轮录制循环必现”的确定性问题
      - 更接近低频、间歇性、可能受现场时序或运行负载影响的现象
      - 如果后续还要继续收根因，优先方向应从“单纯多轮次”转向“更长时长或叠加压力/串口观测”
  - `merge16` / `gripper_hmi` CPU 压力循环（`2026-04-22`）
    - 结果目录：`tmp/board_results/gripper_hmi_cpu_stress_merge16_20260422`
    - 汇总：`tmp/board_results/gripper_hmi_cpu_stress_merge16_20260422/stress_summary.json`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - 新增观测：`hmi_input_inactive / hmi_ports_inactive` 日志 detail 带 `input_age_ms` 与 `port_activity`
      - 板端 `7` 个 CPU busy-loop worker 压力下连续人工 `UP -> DOWN` 循环
      - 样本：`episode_20260422_0018` 到 `episode_20260422_0031`
    - 结论：
      - `recording_started_count=14`
      - `validation_true_count=12`
      - `validation_false_count=2`
      - `health_fault_count=0`
      - `hmi_ports_inactive_count=0`
    - 失败样本：
      - `episode_20260422_0026`
        - `left_cam_main.mkv span=0.046s`
      - `episode_20260422_0028`
        - `left_cam_main.mkv span=0.097s`
    - 说明：
      - 这轮在 CPU 压力下仍未复现 `hmi_ports_inactive`
      - 但复现了新的短录失败形态：主摄 `left_cam_main` 在极短录制窗口内 span 低于 `0.200s` 校验门限
      - 从日志看，这两次失败都发生在 `recording started` 后很短时间内就进入 `stop phase`
      - 因此当前更像“高负载 + 极短录制时长”下的主摄起录/停录边界问题，而不是 HMI 活跃性 fault
  - `merge16` / `service` 增强回归（`2026-04-22`）
    - 结果目录：`tmp/board_results/service_enhanced_merge16_20260422_133510`
    - 汇总：`tmp/board_results/service_enhanced_merge16_20260422_133510/service_enhanced_summary.json`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - 正常人工 `ShortUp -> wait 3-5s -> ShortDown`
      - 使用增强后的 `gripper io totals` 门禁复核：
        - `tx_led`
        - `tx_state_req`
        - `rx_frames`
        - `rx_key_reports`
    - 结论：
      - `ok=true`
      - 新样本：`episode_20260422_0032`
      - `validation_error.log` 不存在
      - `ShortUpPressed`、`ShortDownPressed`、`Ready`、`Recording` 均命中
      - `gripper_report.json` 中 `io_failures=0`
    - 说明：
      - 本轮已把增强后的 `gripper` 周期控制流量门禁接到真实板端 service 样本上，而不再只是离线日志重放
      - 主摄抽样：
        - `left_cam_main duration_sec=5.884300`
        - `right_cam_main duration_sec=5.776605`
  - `merge16` / `service` 4-cycle soak（`2026-04-22`）
    - 结果目录：`tmp/board_results/service_soak_merge16_20260422_144920`
    - 汇总：
      - `tmp/board_results/service_soak_merge16_20260422_144920/service_soak_summary.json`
      - `tmp/board_results/service_soak_merge16_20260422_144920/cycle_01/episode_summary.json`
      - `tmp/board_results/service_soak_merge16_20260422_144920/cycle_02/episode_summary.json`
      - `tmp/board_results/service_soak_merge16_20260422_144920/cycle_03/episode_summary.json`
      - `tmp/board_results/service_soak_merge16_20260422_144920/cycle_04/episode_summary.json`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - `ugripper.service` 入口
      - `6` 个 CPU busy-loop worker 压力
      - 连续 `4` 轮人工 `ShortUp -> wait 3-5s -> ShortDown`
      - 样本：
        - `episode_20260422_0034`
        - `episode_20260422_0035`
        - `episode_20260422_0036`
        - `episode_20260422_0037`
    - 结论：
      - `service_soak_summary.json -> all_ok=true`
      - 四轮 `episode_summary.json` 均为：
        - `ok=true`
        - `validation_reason=ok`
      - 本轮未出现：
        - `validation_failed`
        - `episode validation failed`
        - 主摄窗口坏段
        - gripper/HMI blocking failure
    - 说明：
      - 这轮比单次 `service enhanced` 更接近真实使用链路，已经把 `service -> episode -> sensor/video/gripper/hmi summary` 连续压了 `4` 轮
      - `hmi_button_report` 中持续可见 `LongDownPressed count=1` 的累计现象，但没有阻塞 `ShortUpPressed / ShortDownPressed` 主路径，也没有导致任何一轮失败
    - 追溯边界：
      - 本轮执行中暴露出两个板端脚本环境问题：
        - 多数据盘场景下 `find_default_episode_root()` 会误选到“字典序第一个” `episode_root`
        - `board_service_integration_check.sh` 默认仍优先找 `build/x86/test/.../check_sensor_mcap`，不适配板端常见的 `test/scripts/check_sensor_mcap`
      - 当前本地脚本已修正这两个默认行为，并给 `board_service_episode_loop_check.sh` 补了 `--checker-bin` 透传
      - 但这轮板端 `cycle_01-04` 校验本身是在旧板端脚本基础上人工补齐的，因此应把“脚本默认环境已修”与“本轮样本已通过”视为两件独立事实
  - `merge16` / `service` 20min soak（`2026-04-22`）
    - 结果目录：`tmp/board_results/service_soak20_merge16_20260422_152341`
    - 汇总：
      - `tmp/board_results/service_soak20_merge16_20260422_152341/test_summary.json`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - `ugripper.service` 入口
      - `6` 个 CPU busy-loop worker 压力
      - 连续单轮长录约 `20` 分钟
      - 样本：`episode_20260422_0040`
    - 结论：
      - `test_summary.json -> ok=true, validation_reason=ok`
      - `runtime_validation_ok=true`
      - `runtime_stop_elapsed_ms=2267`
      - `sensor_left_ok=true`
      - `sensor_right_ok=true`
      - `left_video_ok=true`
      - `right_video_ok=true`
      - 主摄 host `10s` 窗口检查：
        - `left_windows=126`
        - `right_windows=126`
        - 全部通过
      - 主摄时长：
        - `left_duration_sec=1257.845985`
        - `right_duration_sec=1257.729992`
    - 说明：
      - 这轮把当前 `merge16` 的 `service` 入口从短循环推进到了 `20min` 长录，并在 stop 后同时满足：
        - runtime 内部 validation 通过
        - 左右 `MCAP` 通过
        - 左右主摄宿主机窗口检查通过
      - 当前至少可以确认：`merge16` 在这组 `20min / service / CPU pressure` 条件下，没有复现主摄坏段、`validation_failed` 或 HMI/gripper blocking failure
    - 长录脚本边界：
      - `board_service_episode_loop_check.sh` 虽已修复“发现新 episode 目录后抢跑校验”的问题，但它仍然更适合“短循环录制”
      - 对长录场景，episode 目录会在录制开始时就创建，因此 loop 脚本会过早进入“等待产物齐”阶段
      - 当前更合理的长录执行口径是：
        - 先开始长录
        - 到时手动 stop
        - 再对目标 `episode_*` 单独执行 `board_service_integration_check.sh` 或宿主机分析
  - `merge16` / `service` 1h soak（`2026-04-22`）
    - 结果目录：`tmp/board_results/service_soak1h_merge16_20260422`
    - 汇总：
      - `tmp/board_results/service_soak1h_merge16_20260422/test_summary.json`
      - `tmp/board_results/service_soak1h_merge16_20260422/notes.txt`
    - 补充离线归档：
      - `tmp/board_results/service_soak1h_merge16_20260422_analysis/test_summary.json`
      - `tmp/board_results/service_soak1h_merge16_20260422_analysis/notes.txt`
      - `tmp/board_results/service_soak1h_merge16_20260422_analysis/video_report_left_cam_main_host_full.json`
      - `tmp/board_results/service_soak1h_merge16_20260422_analysis/video_report_right_cam_main_host_full.json`
      - `tmp/board_results/service_soak1h_merge16_20260422_analysis/video_report_left_cam_main_host_script_full.json`
      - `tmp/board_results/service_soak1h_merge16_20260422_analysis/video_report_right_cam_main_host_script_full.json`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - `ugripper.service` 入口
      - 板端 `7` 个 CPU busy-loop worker
      - 录制前已用 `top` 确认整机 busy 约 `90%+`
      - 长录监控目录（板端）：
        - `/tmp/service_long_pressure_1h_20260422_165216`
      - 样本：`episode_20260422_0050`
    - 结论：
      - `recording_started_at=2026-04-22 16:52:33 CST`
      - `recording_stopped_at=2026-04-22 17:58:47 CST`
      - `recording_duration_sec=3974`
      - `validation_ok=true`
      - `validation_elapsed_ms=1621`
      - service journal 中未出现：
        - `motion overspeed detected`
        - `health fault`
        - `camera recorder exited`
        - `sensor recorder exited`
        - `episode validation failed`
      - CPU 监控统计：
        - `samples=3184`
        - `avg_busy_percent=95.2`
        - `min_busy_percent=81.7`
        - `max_busy_percent=100.0`
    - 说明：
      - 这轮把当前 `merge16` 的 `service` 压力长录从 `20min` 继续推进到约 `1h 06m`
      - 在这组样本上，已经可以确认：
        - 录制全过程维持在高 CPU 压力区间
        - stop 后 runtime validation 正常通过
        - 未复现 motion alert、health fault 或 recorder 提前退出
      - 本轮已额外补齐 `episode_20260422_0050` 的离线归档：
        - `board_service_integration_check.sh --skip-video-check` 汇总为 `ok=true`
        - 左右 `sensor` analyzer 均通过，`sequence_gap_count=0`、`timestamp_regression_count=0`
        - `gripper/hmi` analyzer 均通过
        - 左右主摄 `ffprobe` 时长分别为 `3973.643885s`、`3973.691914s`，与 runtime 记录的约 `3974s` 对齐
        - 宿主机主摄 full sweep 也已完成：
          - `left_cam_main_host_full -> ok=true, windows=1987, min_frame_count=97, max_frame_count=123`
          - `right_cam_main_host_full -> ok=true, windows=1987, min_frame_count=102, max_frame_count=122`
        - 宿主机又直接复用了板端 `check_video_windows.py` 做整段窗口复核：
          - `left_cam_main_host_script_full -> ok=true, windows=1987, min_frame_count=100, max_frame_count=123, elapsed≈11m14s`
          - `right_cam_main_host_script_full -> ok=true, windows=1987, min_frame_count=102, max_frame_count=123, elapsed≈11m57s`
      - 当前这轮已经同时覆盖：
        - `service` 入口
        - `CPU 90%+`
        - 约 `1h 06m` 长录
        - `sensor/gripper/hmi` 离线 analyzer
        - 左右主摄宿主机 full sweep
      - 仍需注意：
        - 最新补跑已经在宿主机直接复用 `check_video_windows.py`，窗口/帧数口径与板端默认视频检查一致，但运行位置仍是宿主机，不是板端原地跑满 `1h`
        - 若后续要作为更正式 release gate，仍建议把当前 `merge16` 多 suite 结果统一汇总成单一 release gate 结论
  - `merge16` / `service` 默认脚本环境修正验证（`2026-04-22`）
    - 结果目录：`tmp/board_results/service_env_fix_verify_merge16_20260422_152000`
    - 汇总：
      - `tmp/board_results/service_env_fix_verify_merge16_20260422_152000/episode_summary.json`
    - 覆盖范围：
      - 板端已同步修正后的：
        - `board_test_common.sh`
        - `board_service_integration_check.sh`
        - `board_service_episode_loop_check.sh`
      - 验证口径：
        - 不显式传 `--episode-root`
        - 不显式传 `--checker-bin`
        - 直接执行 `board_service_integration_check.sh`
    - 结论：
      - `find_default_episode_root()` 已自动选中 `/mnt/data_disk/dap912263b000689/data`
      - `board_service_integration_check.sh` 已自动解析板端 `test/scripts/check_sensor_mcap`
      - `episode_summary.json -> ok=true, validation_reason=ok`
    - 说明：
      - 这轮证明前面暴露出的两个环境问题已经在板端脚本默认路径上关闭，不再需要人工补 `episode_root` 或 `checker-bin`
  - `merge16` / `service` default loop ready-fix 验证（`2026-04-22`）
    - 结果目录：`tmp/board_results/service_loop_readyfix_verify_merge16_20260422_151736`
    - 汇总：
      - `tmp/board_results/service_loop_readyfix_verify_merge16_20260422_151736/cycle_01/episode_summary.json`
      - `tmp/board_results/service_loop_readyfix_verify_merge16_20260422_151736/loop_summary.txt`
    - 覆盖范围：
      - 板端直接执行修正后的 `board_service_episode_loop_check.sh`
      - 不显式传 `--episode-root`
      - 不显式传 `--checker-bin`
      - 使用默认 validator 链自动观察新 episode 并立即校验
    - 结论：
      - 新样本：`episode_20260422_0039`
      - `cycle_01/episode_summary.json -> ok=true, validation_reason=ok`
      - 日志中已出现 `Wait For Episode 1 Artifacts`，说明 loop 脚本会先等待 `info.json/metadata.json/*.mkv/*.mcap` 等产物齐全，再进入 validator
    - 说明：
      - 这轮关闭了此前“发现新 episode 目录后过早开始校验，导致 `info.json` 尚未落盘就误报失败”的抢跑问题
  - `merge16` / `gripper_hmi` 人工交互增强回归（`2026-04-22`）
    - 结果目录：`tmp/board_results/gripper_hmi_active_merge16_manual_20260422_134215`
    - 汇总：
      - `tmp/board_results/gripper_hmi_active_merge16_manual_20260422_134215/log_check_full/gripper_report.json`
      - `tmp/board_results/gripper_hmi_active_merge16_manual_20260422_134215/log_check_full/hmi_button_report.json`
    - direct HMI 人工观察：
      - `READY` 看到绿色，但体感亮灯时长不到 `1s`
      - `RECORDING` 看到绿色闪烁，约 `3` 次
      - `ERROR_1` 看到红灯闪烁
      - `beep` 已听到
    - button phase / analyzer：
      - 新样本：`episode_20260422_0033`
      - `validation_error.log` 不存在
      - `validation phase end ... valid=true`
      - `ShortUpPressed`、`ShortDownPressed`、`ShutdownPromptRequested`、`Ready`、`Recording` 全部命中
      - `gripper io totals` 在该短窗口样本上通过增强门禁：
        - `tx_led>=600`
        - `tx_state_req>=50`
        - `rx_frames>=50`
        - `rx_key_reports>=4`
    - 追溯修正：
      - 本轮第一次抓取的 `ugripper_service.log` 截在双键长按之后不久，尚未包含 `ShutdownPromptRequested`
      - 随后补抓完整日志 `ugripper_service_full.log` 后已确认：
        - `left dual-button chord armed`
        - `ShutdownPromptRequested`
        - `playing: shutdown`
      - 因此“缺少 `ShutdownPromptRequested`”不是用户动作未完成，而是第一次日志截取过早
    - 仍需记录的边界：
      - 这轮启动初期仍出现一次 `stereo_status_missing -> recovered`
      - 当前不阻塞 `gripper_hmi/button` 增强回归通过，但后续若做更严格 release gate，仍应继续观察
  - `merge16` / `camera` 长时压力复核（`2026-04-22`）
    - 结果目录：`tmp/board_results/camera_stress_long_merge16_20260422_135009`
    - 汇总：
      - `tmp/board_results/camera_stress_long_merge16_20260422_135009/test_summary.json`
      - `tmp/board_results/camera_stress_long_merge16_20260422_135009/video_report_left_cam_main_host_full.json`
      - `tmp/board_results/camera_stress_long_merge16_20260422_135009/video_report_right_cam_main_host_full.json`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - 板端 `120s`、`7` 个 CPU busy-loop worker、`6` 路相机同时录制
      - 板端先完成主摄 spot decode 与窗口统计
      - 宿主机再补跑主摄整段 `2s window + decode-check` 全量复核
    - 结论：
      - `left_cam_main duration_sec=119.429741`
      - `right_cam_main duration_sec=119.429922`
      - 宿主机整段复核：
        - `left_cam_main_host_full -> ok=true, validation_reason=ok, windows=60`
        - `right_cam_main_host_full -> ok=true, validation_reason=ok, windows=60`
    - 说明：
      - 这轮比之前板端 `30s` 压测更强，且补上了宿主机整段解码证据
      - 因此当前至少可以确认：`merge16` 这组 `120s / 90%+ CPU / 6` 路录制样本上，主摄没有复现持续坏段或整段解码失败
      - 仍不能单独证明所有 service 入口、极短录边界或更长时长 soak 都已经完全无问题
  - `merge16` / `motion alert` 偶发复测（`2026-04-22`）
    - 结果目录：`tmp/board_results/motion_alert_reprobe_merge16_20260422`
    - 汇总：
      - `tmp/board_results/motion_alert_reprobe_merge16_20260422/test_summary.json`
      - `tmp/board_results/motion_alert_reprobe_merge16_20260422/notes.txt`
    - 覆盖范围：
      - 安装包版本：`1.2.8+merge16`
      - 在 service restart 后，设备保持静止，连续执行 `5` 轮人工短录：
        - `ShortUp -> wait a few seconds -> ShortDown`
      - 样本：
        - `episode_20260422_0043`
        - `episode_20260422_0044`
        - `episode_20260422_0045`
        - `episode_20260422_0046`
        - `episode_20260422_0047`
    - 结论：
      - `5/5` 轮 `validation phase end ... valid=true`
      - `motion_overspeed_detected_count=0`
      - 本轮人工观察 `beep_heard_count=0`
      - `reproduced=false`
    - 说明：
      - 这轮复测是针对先前一次偶发现象补做的定向验证；先前证据为 `2026-04-22 16:12:37` 在录制启动早期出现：
        - `motion overspeed detected: side=right reason=accel gyro=0.000 accel_excess=9.807`
        - `motion overspeed detected: side=left reason=accel gyro=0.000 accel_excess=9.807`
      - 结合当前代码实现，现阶段更合理的解释仍是：`SensorRecorder` 启动早期 IMU 瞬时异常样本偶发触发了 motion alert，而不是常规静止短录必现问题
      - 但在这轮 `5` 组静止短录复测里没有再次命中，因此当前只能把它归类为“已观察到一次、尚未稳定复现的低概率偶发现象”，不能当作稳定可复现缺陷

本轮结果尚不能单独证明：

- 高 CPU `90%+` 且更长时长条件下主摄坏段问题已经被完全消除
- gripper 控制帧“不会丢”
- HMI 灯色“该变一定会变”
- 按键短按、长按、双键事件在长时运行中“不会漏报”
- 在完成 `merge8 -> merge9` 差异确认前，不能断言某条单独代码路径就是 `camera recorder exit=1` 的引入点

因此，当前文档中的 `L4 stress`、`Gripper/HMI` 真机主动刺激项仍然保留，不应因为这轮 `merge8` 板端重跑通过就删除。

## 1. 范围与目标

当前目标对象是 `pp_main/standalone` 下这 4 个已合并目标：

- `CameraRecorder`
- `SensorRecorder`
- `GripperHmiTool`
- `UgripperRuntime`

对应测试目标不再按旧 `ugripper/test` 口径组织，而是以 `pp_main/test` 当前已经落地的测试体系为基准。

本阶段测试目标不是“把所有问题一次测完”，而是建立一套分层、可重复、可回归、能和现场命令行验证对接起来的测试体系。

## 2. 当前 `pp_main` 已有测试设计

`pp_main/test` 当前正式测试体系已经不只是 `GoogleTest`。

从本地仓库现状看，当前测试资产实际由 3 层组成：

- `host-only gtest/ctest`
- 离线 analyzer / synthetic fixture 测试
- 板端标准脚本入口

因此，当前正式测试体系应表述为：

- `GoogleTest + CTest`
- Python analyzer + synthetic fixture
- board smoke / integration / stress 脚本

### 2.1 当前已有测试模块

#### `camera_recorder`

当前已有：

- `test_camera_config`
- `test_camera_command_builder`
- `test_stereo_control_json`
- `test_check_video_windows_rules`
- `check_video_windows.py`
- `board_camera_stress.sh`

当前覆盖重点：

- YAML/config 契约
- camera registry / 类型映射
- 录制命令拼装
- stereo 控制/状态 JSON 读写
- 主摄窗口级低帧/解码失败判定规则
- 板端独立压力录制入口

标签：

- `host-only`
- `unit`
- `camera`
- `board`
- `stress`

#### `sensor_recorder`

当前已有：

- `test_bsp_crc`
- `test_encoder_protocol`
- `test_imu_batch_timestamp`
- `test_sensor_mcap_checker`
- `sensor_mcap_checker`
- `check_sensor_mcap`
- `board_sensor_smoke.sh`

当前覆盖重点：

- CRC
- encoder 协议解析
- IMU 批时间戳回填/平滑逻辑
- `MCAP` 非空、topic 存在、`sequence gap`、timestamp regression
- 板端独立录制 smoke 与 analyzer 串接

标签：

- `host-only`
- `unit`
- `sensor`
- `board`
- `smoke`

#### `gripper_hmi`

当前已有：

- `test_hmi_protocol`
- `test_hmi_led_effects`
- `test_check_gripper_ack_log_pass`
- `test_check_gripper_ack_log_fail`
- `check_gripper_ack_log.py`
- `board_gripper_hmi_log_check.sh`

当前覆盖重点：

- HMI 协议编解码
- LED 灯效状态表达
- gripper 诊断日志中的 `ack / timeout / retry / failure` 汇总规则
- 板端日志采集与 gripper/HMI analyzer 串接

标签：

- `host-only`
- `unit`
- `gripper_hmi`
- `board`

#### `record_runtime`

当前已有：

- `test_audio_coordinator`
- `test_button_logic`
- `test_process_policy`
- `test_recording_orchestrator`
- `test_runtime_health`
- `test_shutdown_request_port`
- `test_stereo_session_client`
- `test_check_hmi_event_log_pass`
- `test_check_hmi_event_log_fail`
- `check_hmi_event_log.py`
- `board_service_integration_check.sh`
- `board_service_restart_check.sh`
- `board_service_episode_loop_check.sh`

当前覆盖重点：

- 进程监督策略
- 音频协调
- 按键逻辑
- 录制编排
- 健康检查
- 关机请求端口
- stereo session 控制面
- HMI 日志事件链一致性规则
- service 集成、恢复、循环观察脚本入口

标签：

- `host-only`
- `unit`
- `record_runtime`
- `board`
- `integration`

### 2.2 当前实际覆盖边界

当前已经落地并接线的自动化测试主要覆盖：

- 协议与编解码
- CRC 与时间戳逻辑
- YAML/config 契约
- 命令构造
- JSON 状态文件
- 控制面策略
- `MCAP` 连续性基础规则
- 视频窗口级低帧/解码失败基础规则
- 录制编排、健康检查、按键逻辑
- service 日志上的 gripper/HMI 基础一致性规则

当前已经有第一版入口、但仍偏脚本化或人工触发的部分：

- `board_sensor_smoke.sh`
- `board_camera_stress.sh`
- `board_gripper_hmi_log_check.sh`
- `board_service_integration_check.sh`
- `board_service_restart_check.sh`
- `board_service_episode_loop_check.sh`

这些脚本已经属于正式测试资产，但当前多数仍处于以下状态之一：

- 只做过 `bash -n` / `--help` / synthetic fixture 验证
- 已有标准入口，但还未在最新板端包上形成稳定的批量重跑记录
- 已能串接 analyzer，但还没有统一汇总成 release gate

### 2.3 当前与更新后代码未完全对齐的测试空白

以下逻辑已经在 `pp_main/standalone` 中存在，但测试还没有跟上到同等粒度：

- `CameraRecorder`
  - `--apply-uvc-roll-only`
  - 主摄 `UVC roll` 相关 CLI / mode 选择
  - 更完整的 validation reason mapping
- `SensorRecorder`
  - motion alert 状态机
  - 写队列、flush、stop 尾段边界
- `GripperHmiTool`
  - `readSerialNumberAndCalibration`
  - abort recovery / missing-chunk sweep
  - exclusive transaction / reconnect 相关高价值路径
- `UgripperRuntime`
  - tactile warning
  - `umount` system action
  - gripper reconnect refresh / pending refresh

当前尚未形成正式自动化覆盖的重点风险包括：

- 板端真实设备枚举与设备节点稳定性
- 串口抖动、坏帧、USB 热插拔与重枚举恢复
- 长时录制稳定性
- CPU 90%+ 条件下的视频坏段、丢帧、尾段损坏
- HMI 真机灯效异常、按键失灵、gripper 控制帧漏发

## 3. 测试分层

建议把合并后的测试固定为 5 层。

### 3.1 `L0`：Host-only Unit

目的：

- 保护纯逻辑
- 快速回归

典型对象：

- 协议编解码
- CRC
- YAML/config 解析
- 命令构造
- JSON 状态文件
- 状态机
- stop/validate policy

执行位置：

- 开发机
- CI

工具：

- `gtest`
- `ctest`

### 3.2 `L1`：Host-only Component Smoke

目的：

- 验证 standalone 二进制最小可用
- 验证 CLI、参数、配置样例、dry-run

典型对象：

- `CameraRecorder --help`
- `CameraRecorder --dry-run`
- `SensorRecorder --help`
- `zeroing --help`
- `GripperHmiTool --help`

执行位置：

- 开发机
- 也可在板端做一次

工具：

- 直接命令行

### 3.3 `L2`：Board Binary Smoke

目的：

- 验证真实设备节点、真实进程、真实输出路径

典型对象：

- `CameraRecorder` 独立起录
- `SensorRecorder` 独立录制
- `GripperHmiTool` 读取 SN / 轮询状态
- 音频 sink 绑定

执行位置：

- 目标板

工具：

- 命令行
- `journalctl`
- `ffprobe`
- `find`
- `pactl`

### 3.4 `L3`：Board Service Integration

目的：

- 验证完整服务链

典型对象：

- `ugripper.service` 拉起
- stereo daemon ready
- 按键开始/停止
- episode 产物齐全
- validation 成功
- ready / writing / validation_failed 音效和灯效正确

执行位置：

- 目标板

工具：

- `systemctl`
- `journalctl`
- episode 目录检查

### 3.5 `L4`：Board Stress / Soak

目的：

- 把现场偶发问题打出来

典型对象：

- 高 CPU 占用下录制，重点覆盖主摄约 `20s` 问题段
- 高频开始/停止循环
- USB 设备重枚举恢复
- 长时间连续录制
- 音频、HMI、相机、传感器同时工作下的稳定性

执行位置：

- 目标板

工具：

- 压力工具
- 长时录制脚本
- 日志扫描
- 产物校验脚本

## 4. 当前完整测试清单

下面的清单按“已有”和“建议新增”分开写。

### 4.1 `CameraRecorder`

#### 当前已有

- 配置解析正确
- 命令拼装正确
- stereo 控制/状态 JSON 读写正确
- `--help`
- `--dry-run`
- 板端独立 `--stereo-daemon` 拉起与状态文件检查
- 经 `UgripperRuntime` 集成后 episode 产物检查

#### 建议新增 `L0/L1`

- `test_camera_validation_policy`
  - 校验“视频跨度过短/缺文件/空文件/时长落后太多”时的失败原因映射
- `test_camera_output_integrity`
  - 校验 episode 中文件清单与主输出约束
- `test_camera_restart_backoff`
  - 校验设备短暂掉线后的退避重试策略
- `test_camera_schema_compat`
  - 校验旧 YAML 与 `schema_version: 1` 兼容

#### 建议新增 `L2/L3`

- 主摄独立短录 5s / 30s / 180s
- stereo daemon 独立空闲运行 10min
- 热插拔/设备重枚举后恢复检查
- 开始录制前 stereo 已 ready，开始后首帧时间合理
- stop 后 `info.json`、`stereo_session`、`validation_error.log` 行为正确

#### 建议新增 `L4`

- 高 CPU `90%+` 下主摄压力录制，重点检查约 `20s` 问题段
- 录制中反复 stop/start 循环 50 次以上
- 压力下 `ffprobe` 全量检查 8 路视频 span、duration、可读性
- 压力下检查 `validation_failed` 是否误报

#### 重点风险

- 主摄 20s 左右坏段
- stop 前 flush 不完整
- 压力下主摄 span 变短
- stereo session 与主摄时间边界不一致

### 4.2 `SensorRecorder`

#### 当前已有

- CRC
- encoder 协议
- IMU batch timestamp
- `SensorRecorder --help`
- `zeroing --help`
- 板端独立录制 smoke，生成左右 MCAP

#### 建议新增 `L0/L1`

- `test_sensor_sequence_continuity`
  - 校验 frame/seq 连续性检查逻辑
- `test_sensor_write_queue`
  - 校验写队列堆积、丢弃、背压阈值行为
- `test_sensor_mcap_integrity`
  - 校验 writer 结束时 header/index 完整性
- `test_sensor_timestamp_reorder`
  - 校验 burst / reorder / delayed batch 时的时间戳回填

#### 建议新增 `L2/L3`

- 板端 5s / 60s / 10min 独立录制
- 录制结束后解析左右 MCAP，检查：
  - topic 存在
  - message 数量合理
  - 时间单调
  - seq 连续
  - 左右手都非空
- zeroing 前后 encoder 行为检查

#### 建议新增 `L4`

- 与相机同时录制时连续 30min / 2h
- 写盘压力下 MCAP 连续性检查
- 高频 start/stop 下尾包完整性检查

#### 重点风险

- MCAP 某个 frame 丢失
- burst 回填时间错误
- stop 时最后一批样本没刷盘
- 左右写线程之一卡住

### 4.3 `Gripper` / `HMI`

#### 当前已有

- HMI 协议
- LED 灯效纯逻辑
- `GripperHmiTool --help`
- 板端轮询状态
- 左右读取 SN

#### 建议新增 `L0/L1`

- `test_hmi_transport_resync`
  - 串口坏帧、错位、半包后能否重新同步
- `test_hmi_command_retry`
  - 指令超时/失败后的重试策略
- `test_hmi_key_report_sequence`
  - 连续按键上报序列是否正确
- `test_led_state_bridge`
  - runtime 状态到 HMI 灯效状态的桥接是否正确

#### 建议新增 `L2/L3`

- 真机灯效状态切换检查：
  - `INIT`
  - `READY`
  - `RECORDING`
  - `WRITING`
  - `ERROR_1`
  - `ERROR_5`
- 真机按键检查：
  - 单击开始
  - 单击停止
  - 长按无误触
  - 双键关机触发
- gripper 控制帧收发成功率检查

#### 建议新增 `L4`

- 高频 RGB 更新与状态轮询并发压力
- 长时轮询下按键丢触发检查
- 串口噪声/短断开/重连恢复检查

#### 重点风险

- 控制帧漏发
- 灯应该变但没有变
- 按键失灵或漏报
- 轮询和控制争用导致状态异常

### 4.4 `UgripperRuntime`

#### 当前已有

- process policy
- audio coordinator
- button logic
- recording orchestrator
- runtime health
- shutdown request port
- stereo session client

#### 建议新增 `L0/L1`

- `test_button_event_timing`
  - 防抖、短按、长按、双键窗口
- `test_runtime_fault_recovery`
  - stereo ready 缺失、audio sink 缺失、camera stop 失败等故障恢复
- `test_runtime_validation_reason_mapping`
  - 各类 validation 失败映射到 HMI / audio / log 的策略

#### 建议新增 `L2/L3`

- service 冷启动
- 未插数据盘时等待逻辑
- stereo status file 缺失后恢复
- 音频 sink 切换与恢复
- 开始/停止录制全链路
- validation 成功/失败时音效、灯效、日志都一致

#### 建议新增 `L4`

- 长时运行 8h / 24h
- 每隔 1min 录一次，共 100 次 stop/start
- 压力下检查僵尸子进程、文件句柄、日志增长

#### 重点风险

- 子进程 stop 顺序不对
- validation 误判
- 故障恢复后状态没回到 ready
- 音频/HMI/相机三者状态不一致

### 4.5 交叉模块与交付链

#### 建议新增

- 安装后路径验证
- `systemd` 服务拉起验证
- `postinst/prerm/postrm` 行为验证
- U 盘升级后服务恢复验证
- calibration 导入后运行时读取验证
- 板端重启后自动恢复验证

说明：

- `calibration` 昨天已经确认卡在设备本身问题上，所以当前文档里它仍应保留为测试项，但状态应标成“软件链路待二次确认，硬件前置问题未清”。

## 5. 针对已知问题的专项补测清单

这部分直接对应你列出的历史问题。

### 5.1 Sensor 录制丢帧

要补的测试：

- `L0`
  - seq 连续性检测逻辑
  - burst timestamp 回填逻辑
- `L2`
  - 独立录制后解析 MCAP，检查 seq 是否连续
- `L4`
  - 与相机同时录制、写盘有压力时连续 30min+，检查左右 MCAP 是否出现洞

验收重点：

- 不只看文件存在
- 必须看 message 数量、seq、timestamp 单调性

### 5.2 相机数据流损坏

要补的测试：

- `L0`
  - validation policy 单测
- `L2`
  - 主摄独立录制并做窗口级 `ffprobe/ffmpeg` 检查
- `L4`
  - CPU 90%+ 下主摄 + stereo + tactile + sensor 全链路录制

验收重点：

- 不只看 stop 是否成功
- 必须对主摄做窗口级坏段检查，重点覆盖约 `20s` 问题段

### 5.3 Gripper 控制丢控制帧

要补的测试：

- `L0`
  - transport resync / retry
- `L2`
  - 板端控制命令回包统计
- `L4`
  - 高频轮询 + 灯效切换 + 控制并发

验收重点：

- 要统计成功率和重试次数
- 不能只靠“肉眼看起来没问题”

### 5.4 HMI 灯效异常

要补的测试：

- `L0`
  - LED effect 纯逻辑
  - runtime state -> LED state bridge
- `L2`
  - 真机状态切换逐项检查

验收重点：

- 区分“逻辑没算对”还是“串口下发失败”

### 5.5 按键失灵

要补的测试：

- `L0`
  - 防抖、短按、双击、长按窗口
- `L2`
  - 真机按键事件统计
- `L4`
  - 长时运行下按键漏报率

验收重点：

- 区分“固件没上报”
- 区分“驱动读到了但 runtime 没消费”
- 区分“状态机消费了但动作没触发”

## 6. 最终目标

本文档对应的最终目标不是补几个零散 case，而是形成一套可以长期使用的正式测试体系。

最终应具备以下能力：

- 运行时具备足够的观测点，能把问题定位到输入、传输、处理、落盘或状态同步链路
- 离线分析器能够自动判定 `MCAP` 丢帧、主摄坏段、控制帧丢失、灯效状态不一致和按键事件漏报
- 稳定的判定逻辑进入 `pp_main/test`，成为 `host-only` 回归测试的一部分
- 板端形成标准化 `smoke / integration / stress / soak` 脚本，不再依赖人工临时拼命令
- 发版前能产出结构化测试报告，而不是仅依赖人工翻日志

### 6.3 规范化建设目标

为了让测试体系后续更稳定、更可维护，建议把“规范化”明确收成以下目标：

- 统一资产类型
  - 每个模块都应明确区分：
    - `logic test`
    - `analyzer test`
    - `board smoke`
    - `service integration`
    - `stress/soak`
- 统一状态口径
  - 每个测试项都应标明：
    - `已落地`
    - `已接线`
    - `已重跑验证`
    - `最近一次验证时间`
- 统一命名与标签
  - `ctest` 标签建议统一为：
    - `host-only`
    - `unit`
    - `analyzer`
    - `camera`
    - `sensor`
    - `gripper_hmi`
    - `record_runtime`
    - `board`
    - `integration`
    - `stress`
- 统一报告结构
  - 所有 analyzer 和板端脚本最终都应落到统一 JSON schema，至少包含：
    - `suite`
    - `module`
    - `input`
    - `ok`
    - `failures`
    - `warnings`
    - `generated_at`
- 统一 feature-to-test 映射
  - 后续每增加一条运行时新逻辑，不应只加代码，应至少补其中 2 类：
    - 一个 host-only 规则测试
    - 一个 analyzer 或 board 脚本验收入口

### 6.4 更全面覆盖的构建原则

为了让覆盖更全面，而不是继续零散加 case，建议按下面的矩阵收口：

- 每个模块都至少覆盖 5 个维度：
  - 输入解析
  - 状态机/策略
  - 落盘/产物
  - 故障注入
  - 板端真实链路
- 每个高风险问题都至少覆盖 3 层：
  - `host-only`
  - `analyzer`
  - `board`
- 每个板端脚本都应尽量做到：
  - 一键执行
  - 自动收集输入
  - 自动调用 analyzer
  - 自动输出结构化报告
  - 明确 `PASS/FAIL`
- 每个 analyzer 都应有 synthetic fixture 测试，避免“脚本存在但规则没被回归保护”

### 6.5 建议补充的规范化资产

结合当前现状，后续最值得新增的规范化资产包括：

- `test/scripts/run_ugripper_host_only.sh`
  - 固定 `ctest -L host-only` 入口，减少人工拼命令
- `test/scripts/run_ugripper_board_suite.sh`
  - 串起 `board_sensor_smoke.sh`、`board_camera_stress.sh`、`board_service_integration_check.sh`
- `test/docs/ugripper_test_matrix.md`
  - 维护“模块 x 风险 x 测试层”的覆盖矩阵
- `test/reports/test_summary.json`
  - 汇总本轮所有 analyzer / board script 输出，形成单一验收入口

### 6.1 最终交付形态

最终交付应至少包含：

- `host-only` 自动化测试
  - 纯逻辑、协议、策略、判定规则
- 板端离线分析工具
  - `MCAP` 检查器
  - 视频窗口检查器
  - gripper ACK/重试统计汇总
  - HMI/Button 状态账本分析
- 板端标准回归脚本
  - 二进制 smoke
  - service 集成
  - 高 CPU 压测
  - 高频启停
- 结构化报告
  - `sensor_report.json`
  - `video_report.json`
  - `gripper_report.json`
  - `hmi_button_report.json`
  - `test_summary.json`

### 6.2 问题闭环目标

最终每类问题都应形成明确闭环：

- `SensorRecorder`
  - 不再停留在“怀疑丢帧”，而是能自动报出 `seq gap`、时间回退或尾包缺失
- `CameraRecorder`
  - 不再停留在“看起来某段坏了”，而是能自动报出具体坏段时间窗和失败类型
- `GripperHmiTool`
  - 不再停留在“偶尔没反应”，而是能给出 `sent / ack / retry / resync` 统计
- `HMI`
  - 不再停留在“灯没变”，而是能定位到状态计算、命令下发或设备响应哪一层
- `Button`
  - 不再停留在“按键失灵”，而是能定位到原始上报、驱动解析、runtime 消费或动作触发哪一层

## 7. 分阶段落地路线

### 7.1 Phase 0：判定标准与验收阈值冻结

目标：

- 先定义“什么叫失败”，避免后续脚本和单测口径不一致

本阶段需要完成：

- 冻结 `SensorRecorder` 的失败定义
  - `seq` 不连续
  - timestamp 回退
  - 超阈值无数据间隔
  - stop 尾包缺失
- 冻结 `CameraRecorder` 的失败定义
  - 主摄约 `20s` 问题段的坏段判定标准
  - 窗口级解码失败
  - 窗口级帧数异常
  - flush/validation 失败映射
- 冻结 `GripperHmiTool` 的失败定义
  - 发送无 ACK
  - 超时后重试失败
  - 重同步失败
- 冻结 `HMI/Button` 的失败定义
  - 状态切换后灯效不一致
  - 原始按键、解析事件、消费事件、动作触发不一致

本阶段产出：

- 故障模式表
- 每类问题的验收阈值
- 每类问题需要记录的最小日志字段

补充要求：

- 判定标准冻结后，文档、analyzer、板端脚本三者必须共用同一套失败口径
- 不再允许“文档写一种失败定义、脚本按另一种规则判断”

#### 第一轮执行阈值

第一轮实现先冻结 `FAIL` 口径，`WARN` 阈值后续再结合现场数据收紧。

- `SensorRecorder`
  - 任一 topic 为空：`FAIL`
  - 任一 `seq gap`：`FAIL`
  - 任一 timestamp 回退：`FAIL`
  - 录制结束后 `written_messages < emitted_messages`：`FAIL`
- `CameraRecorder`
  - 主摄任一检查窗口不可解码：`FAIL`
  - 主摄任一检查窗口无帧：`FAIL`
  - `validation_failed`、`last_finalize_error` 非空：`FAIL`
  - 高 CPU 压测必须覆盖主摄约 `20s` 问题段
- `GripperHmiTool`
  - 命令发送后无 ACK 且重试后仍失败：`FAIL`
  - 出现重同步失败：`FAIL`
- `HMI/Button`
  - runtime 状态切换后无对应灯效命令：`FAIL`
  - 原始按键、解析事件、消费事件、动作触发链路不一致：`FAIL`

### 7.2 Phase 1：运行时观测点补齐

目标：

- 先把关键链路变得可观测，再做分析器

本阶段需要完成：

- `SensorRecorder`
  - 记录每路接收消息数、写入消息数、最后 `seq`
  - 记录写队列峰值、flush 次数、最后成功写入时间
- `CameraRecorder`
  - 记录每路输入帧数、编码输出数、编码错误数
  - 记录 writer backlog、flush 耗时、validation 失败原因
  - 主摄单独统计
- `GripperHmiTool`
  - 为每条命令分配内部 `command_id`
  - 记录发送时间、ACK 时间、超时次数、重试次数、重同步次数
- `UgripperRuntime/HMI`
  - 记录原始按键输入、驱动解析事件、runtime 状态切换
  - 记录期望灯效与实际下发灯效命令

本阶段完成标志：

- 任一现场故障都能从日志中定位到具体链路层级

### 7.3 Phase 2：第一版离线分析器落地

目标：

- 先让问题能够被自动抓出来，不再依赖人工翻日志

本阶段需要完成：

- `T2.1` `MCAP` 检查器
  - 第一版入口：`pp_main/build/x86/test/src/sensor_recorder/check_sensor_mcap`
  - 源码位置：`pp_main/test/src/sensor_recorder/check_sensor_mcap.cc`
  - 第一版支持文件非空、`seq` 连续、timestamp 单调、左右手非空
- `T2.2` 主摄视频窗口检查器
  - 建议入口：`pp_main/test/scripts/check_video_windows.py`
  - 第一版支持窗口级解码检查、窗口级帧数统计、坏段时间窗输出
- `T2.3` gripper ACK/重试汇总
  - 第一版入口：`pp_main/test/scripts/check_gripper_ack_log.py`
  - 先以日志与汇总统计形式落地
  - 必须能输出 `sent / ack / timeout / retry / resync`
- `T2.4` HMI/Button 状态账本分析
  - 第一版入口：`pp_main/test/scripts/check_hmi_event_log.py`
  - 先把按键、状态切换、灯效下发串成统一事件链

本阶段完成标志：

- 板端录制完成后，可以直接运行分析器得到 `PASS/FAIL + 原因`

### 7.4 Phase 3：规则下沉到 `host-only`

目标：

- 把已经稳定的判定规则沉淀成正式自动化回归

本阶段需要完成：

- `SensorRecorder`
  - `test_sensor_sequence_continuity`
  - `test_sensor_timestamp_reorder`
  - `test_sensor_flush_policy`
- `CameraRecorder`
  - `test_camera_validation_policy`
  - `test_camera_output_integrity`
  - `test_camera_window_failure_mapping`
- `GripperHmiTool`
  - `test_hmi_command_retry`
  - `test_hmi_transport_resync`
- `UgripperRuntime/HMI`
  - `test_button_event_timing`
  - `test_led_state_bridge`

本阶段完成标志：

- 核心规则进入 `pp_main/test`
- 每次改动可先在开发机挡回归

补充要求：

- 下沉优先级应优先覆盖“已经进入板端 analyzer 的规则”，避免板端脚本先跑、开发机却挡不住回归

### 7.5 Phase 4：板端标准测试脚本化

目标：

- 把当前人工命令行验证沉淀成标准化板端测试流程

本阶段需要完成：

- 标准化 `SensorRecorder` 板端脚本
  - 独立录制
  - 并发录制
  - stop 边界
  - 自动跑 `MCAP` 检查器
- 标准化 `CameraRecorder` 板端脚本
  - 主摄独立录制
  - CPU `90%+` 条件下压力录制
  - 自动跑视频窗口检查器
- 标准化 `Gripper/HMI/Button` 板端脚本
  - 控制命令连续发送
  - 状态切换灯效检查
  - 按键重复触发
- 标准化 `ugripper.service` 集成脚本
  - ready
  - start
  - stop
  - validate
  - report

本阶段完成标志：

- 常见板端测试不再依赖人工临时拼命令

补充要求：

- 脚本默认输出目录、输入参数、报告文件名要统一
- 优先减少交互式手工步骤，把“人工观察”改成“自动采集 + 自动分析 + 必要人工确认”

### 7.6 Phase 5：发版前正式回归与报告化

目标：

- 让这套测试真正进入版本交付流程

本阶段需要完成：

- 固定发版前回归组合
  - 一轮 `host-only`
  - 一轮 service 集成
  - 一轮主摄高 CPU 压测
  - 一轮 sensor 并发录制
  - 一轮 gripper/HMI/button 回归
- 固定结构化报告输出
  - `sensor_report.json`
  - `video_report.json`
  - `gripper_report.json`
  - `hmi_button_report.json`
  - `test_summary.json`

本阶段完成标志：

- 测试结果可以沉淀为版本验收资产

## 8. 任务分解表

下表给出建议的实际执行顺序。

| 任务 | 目标 | 主要修改点 | 第一版交付 | 最终目标 | 依赖 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| `T1` | 冻结判定标准 | `docs/` 与测试规则台账 | 每类问题的失败定义与阈值 | 所有脚本/单测共用同一口径 | 无 | `P0` |
| `T2` | 补 `SensorRecorder` 观测点 | `standalone/SensorRecorder` | 接收数、写入数、最后 `seq`、队列峰值 | 支持完整 `MCAP` 故障归因 | `T1` | `P0` |
| `T3` | 补 `CameraRecorder` 观测点 | `standalone/CameraRecorder` | 主摄帧数、错误数、backlog、flush 时间 | 支持窗口级坏段归因 | `T1` | `P0` |
| `T4` | 补 gripper ACK/重试观测点 | `standalone/GripperHmiTool` | `sent / ack / timeout / retry / resync` | 支持控制链路可靠性报告 | `T1` | `P0` |
| `T5` | 补 HMI/Button 事件账本 | `standalone/UgripperRuntime` 与 `GripperHmiTool` | 按键、状态、灯效统一日志链 | 支持状态一致性归因 | `T1` | `P0` |
| `T6` | 实现 `MCAP` 检查器 | `test/src/sensor_recorder/check_sensor_mcap.cc` | 检查非空、`seq` 连续、timestamp 单调 | 输出结构化 `sensor_report.json` | `T2` | `P0` |
| `T7` | 实现主摄视频窗口检查器 | `test/scripts/check_video_windows.py` | 输出坏段时间窗与失败原因 | 输出结构化 `video_report.json` | `T3` | `P0` |
| `T8` | 实现 gripper 汇总工具 | `test/scripts/check_gripper_ack_log.py` | 生成控制链路统计报告 | 输出结构化 `gripper_report.json` | `T4` | `P1` |
| `T9` | 实现 HMI/Button 分析工具 | `test/scripts/check_hmi_event_log.py` | 串起按键、状态、灯效 | 输出结构化 `hmi_button_report.json` | `T5` | `P1` |
| `T10` | 下沉 `Sensor` 规则到 `host-only` | `test/src/sensor_recorder` | 连续性、回填、flush 策略单测 | 成为默认回归集合 | `T6` | `P1` |
| `T11` | 下沉 `Camera` 规则到 `host-only` | `test/src/camera_recorder` | validation 与坏段映射单测 | 成为默认回归集合 | `T7` | `P1` |
| `T12` | 下沉 `Gripper/HMI/Button` 规则到 `host-only` | `test/src/gripper_hmi`、`test/src/record_runtime` | retry、resync、button timing、LED bridge 单测 | 成为默认回归集合 | `T8`、`T9` | `P1` |
| `T13` | 板端 `Sensor` 标准脚本化 | `test/scripts/board_*` | 一键录制并跑 `MCAP` 检查 | 成为常规板端回归项 | `T6` | `P1` |
| `T14` | 板端 `Camera` 标准脚本化 | `test/scripts/board_*` | 一键压测并跑窗口检查 | 成为主摄问题标准回归项 | `T7` | `P1` |
| `T15` | 板端 `Gripper/HMI/Button` 标准脚本化 | `test/scripts/board_*` | 一键采集控制/HMI/button 统计 | 成为常规板端回归项 | `T8`、`T9` | `P2` |
| `T16` | 发版前回归报告化 | `test/scripts/` 与 CI/交付流程 | 汇总 `test_summary.json` | 成为版本验收资产 | `T13`、`T14`、`T15` | `P2` |

### 8.1 当前已落地进展

截至当前版本，已完成或完成第一版的内容如下：

- `T1`
  - 文档内失败口径、分层和阶段目标已经冻结第一版
- `T2/T3`
  - `SensorRecorder`、`CameraRecorder` 已补第一批 summary/诊断观测点
- `T4/T5`
  - `GripperHmiTool`、`UgripperRuntime` 已补第一版 `GRIPPER_DIAG` / `HMI_DIAG`
- `T6/T7/T8/T9`
  - 已落第一版离线分析入口：
    - `check_sensor_mcap`
    - `check_video_windows.py`
    - `check_gripper_ack_log.py`
    - `check_hmi_event_log.py`
- `T12`
  - 已落第一批 `host-only` 覆盖：
    - button debounce / dual chord / reset 边界
    - gripper 协议帧解析
    - health fault key 映射
    - recording start/stop/validate 若干错误路径
  - 本轮继续补齐：
    - `test_hmi_transport_resync`
      - 坏 checksum 帧后自动跳过并重同步到下一帧
      - 半包场景下等待完整帧再解析
    - `test_button_event_timing`
      - 自定义 debounce、long press、shutdown prompt 阈值下的时序边界
    - `test_led_state_bridge`
      - `recording / ready / error1 / error5` 到 LED 输出链路的显式 host-only 断言
    - `check_gripper_ack_log.py`
      - 新增 `retry / recovery / reconnect` 计数阈值断言
      - 已有通过/失败 fixture，覆盖“重试恢复成功”和“计数未达阈值”两类门禁
      - 本轮继续补：
        - 聚合 `tx_led / tx_beep / tx_state_req / rx_frames / rx_key_reports / rx_beep_states`
        - 已补对应 host-only 通过/失败用例，开始把“周期控制帧是否持续在跑”纳入结构化门禁，而不只看 exclusive command
    - `check_hmi_event_log.py`
      - 新增 `health fault key` 与 `key -> led_state` 门禁
      - 已覆盖 `hmi_all_disconnected / hmi_ports_inactive / stereo_daemon_not_running` 三类健康故障分支
    - `test_gripper_refresh_logic`
      - 已覆盖 gripper reconnect 后的状态推进链：
        - `connected -> reconnecting`
        - `waiting_side_devices`
        - `waiting_gripper_ready`
        - `RefreshRuntimeState` 动作切换
    - `test_hmi_driver_logic`
      - 已覆盖 `GripperHmiDriver` 近侧纯逻辑：
        - calibration `retryable / abort-recovery` 状态分类
        - calibration write `ack token` 判定
        - calibration header 解析与 `required_chunk_count` 计算
        - calibration read 的 sequential fallback 门禁
        - calibration chunk write 的 `ack / retry / abort-and-recover / fail` 决策
        - status/data frame buffer scan 规则：
          - 噪声前缀裁剪与 trailing byte 保留
          - non-exclusive status frame 整帧跳过
          - wrong-token status continue 扫描
          - raw/calibration data-or-status 的 oversized garbage drop
        - fixed-size recv frame scan 规则：
          - bad checksum 帧跳过后继续 resync 到后续有效帧
          - header 不存在时仅保留 trailing byte，避免半包头被提前丢弃
        - exclusive command drain 规则：
          - 有新字节到达时延长 idle drain 窗口
          - idle deadline / max deadline 任一到期即停止 drain
          - 负值 read 结果继续映射为 I/O failure
        - `readBytesLocked` poll/timeout 合约：
          - no-data 直到超时仍返回 success，由上层 scan loop 决定是否继续等待
          - 读到字节即 success，负值 read 映射为 I/O failure
        - calibration range collect loop 规则：
          - chunk frame 写回 `raw/received`
          - header 收齐后解析 `required_chunk_count`
          - 收齐 required chunks 后立即判定 complete
          - status frame 只更新状态并继续扫描
          - oversized garbage 按字节丢弃继续 resync
        - calibration range retry / chunk read retry 规则：
          - abort-recovery 状态优先尝试 abort-and-retry
          - 普通 retryable 状态走 delay retry
          - non-retryable 状态直接 stop failure
        - `readCalibrationFrameLocked` scan 规则：
          - wrong-token calibration frame 整帧跳过
          - 垃圾字节逐步丢弃后继续 resync 到目标 token
- `T10`
  - 已落第一批 `Sensor` analyzer 规则下沉：
    - synthetic `MCAP` 下的 expected topic 缺失
    - `sequence gap`
    - timestamp regression
    - contiguous topic pass case
  - 本轮继续补齐：
    - `max_allowed_gap_ns` 超阈值时间洞判定
    - 未配置阈值时的大 gap 放行口径
  - 本轮再补一批 stop/flush 相关基础门禁：
    - `min_message_count`，避免 topic 只写出 1 帧也被误判为通过
    - `min_span_ns`，把“录到了 topic 但整体跨度过短”的截断类问题收入口径
    - `board_sensor_smoke.sh` / `board_service_integration_check.sh` 默认增加 `--min-message-count 2`
  - 本轮继续补：
    - `max_span_gap_ns`，检查同一 `MCAP` 内 expected topics 之间的 span 对齐，避免 `imu` 还在写但 `encoder` 提前停流却被单 topic 规则放过
- `T11`
  - 已落第一批 `Camera` analyzer 规则下沉：
    - 低帧窗口判定
    - decode failure 判定
    - partial tail window 切分规则
  - 本轮继续补齐：
    - legacy / schema v1 下 `uvc_roll_absolute` 配置契约
    - `uvc_roll_absolute` 越界值拒绝
  - 本轮再补一批 validation 口径收紧：
    - partial tail window 的 `min_frames` 按实际尾段长度缩放，避免正常尾段误报
    - expected fps 已知但拿不到 `frame_count` 时显式失败，不再静默放过
    - JSON 汇总增加 `failure_types`，为后续板端汇总和 validation reason mapping 提供结构化入口
  - 本轮继续补：
    - `primary_failure` / `validation_reason` 摘要，把窗口级失败明细聚合成更稳定的坏段主因分类，便于板端脚本、汇总报告和后续 validation mapping 复用
- `T13/T14/T15`
  - 已落第一版板端脚本入口：
    - `test/scripts/board_sensor_smoke.sh`
    - `test/scripts/board_camera_stress.sh`
    - `test/scripts/board_gripper_hmi_log_check.sh`
    - `test/scripts/board_gripper_hmi_active_check.sh`
    - `test/scripts/board_service_integration_check.sh`
    - `test/scripts/board_service_restart_check.sh`
    - `test/scripts/board_service_episode_loop_check.sh`
  - 本轮继续补：
    - `test/scripts/summarize_episode_reports.py`
    - `board_service_integration_check.sh` 现在会把 `sensor/video/gripper/hmi` JSON 汇总成 `episode_summary.json`
    - 可配置 `--max-sensor-span-gap-ns` 与 `--max-video-span-gap-sec`，开始把“跨文件对齐”接入板端标准入口
  - 本轮继续补：
    - `board_sensor_smoke.sh` 现在会把左右 `sensor_report_*.json` 汇总成 `test_summary.json`
    - `board_camera_stress.sh` 现在会把左右 `video_report_*.json` 汇总成 `test_summary.json`
    - `sensor/camera/service` 三条板端入口都已具备结构化 summary 输出
  - 本轮再补第一版 `T15` 主动刺激入口：
    - `board_gripper_hmi_active_check.sh` 会先停服务执行 direct HMI `READY / RECORDING / ERROR_1 / beep` 刺激
    - 随后自动串接 `board_service_restart_check.sh` 恢复服务，再提示人工完成 `ShortUpPressed / ShortDownPressed / ShutdownPromptRequested` 按键序列
    - 最后自动调用 `board_gripper_hmi_log_check.sh` 产出 `gripper_report.json` 与 `hmi_button_report.json`
  - 本轮继续补：
    - `board_gripper_hmi_log_check.sh` 已支持 `--require-io-total-at-least FIELD:COUNT`
    - `board_gripper_hmi_active_check.sh` 已支持把上述门禁透传到 log analyzer
    - `board_service_integration_check.sh` 已支持 `--require-gripper-io-total-at-least FIELD:COUNT`
    - 这使板端脚本可以直接对 `tx_led / tx_beep / tx_state_req / rx_frames` 做门禁，而不是只要求“无 io failure”
- ARM 交叉构建门禁
  - 已完成容器内 `standalone` 目标构建、安装树刷新和 `arm64 deb` 打包

当前还没有完成的重点是：

- `T10/T11`
  - `Sensor`、`Camera` 规则还需要继续下沉为更完整的 `host-only`
  - 当前已覆盖基础 continuity / gap / min-count / min-span / inter-topic span-gap、视频窗口低帧 / decode / partial-tail / frame-count-missing / primary failure summary
  - `service` 级结构化汇总已有第一版 `episode_summary.json` 出口，但还没和运行时 validation message 做一一映射
  - 还没覆盖更完整的跨文件对齐阈值固化和窗口级坏段聚类
- `T13/T14/T15`
  - 当前已完成第一版入口和参数收口
  - 已补 service restart 恢复检查、manual episode loop 观察入口，以及 `sensor/camera/service` 三条入口的结构化 summary 输出
  - 已补第一版 `gripper/hmi/button` 交互式主动刺激脚本，但当前仍需要人工确认 direct HMI 灯效/蜂鸣器，并手工完成 button 序列
  - 当前 `merge8` 板端包已完成一轮人工交互验证：
    - raw runtime log 已证明 `ShortUp -> start`、`ShortDown -> stop(valid=true)`、`dual-button chord -> shutdown prompt audio` 基本链路存在
    - 但 direct HMI `READY` 灯效人工观察失败，且当前板端 `journalctl` 未打印 `[HMI_DIAG] / [GRIPPER_DIAG]`
    - 进一步确认当前板端 `1.2.8+merge8` 安装包二进制本体不含这些诊断日志字符串，因此 analyzer 口径暂时无法闭环；若要继续 `T15`，需先安装包含当前诊断日志实现的新包
  - 当前 `merge9` 板端包已恢复 `HMI_DIAG / GRIPPER_DIAG` 输出，但重跑中暴露出新的板端真实故障样本：
    - `camera recorder exited unexpectedly`
    - `stereo finalize failed: timed out waiting for stereo session metadata`
    - `episode validation failed`
    - `hmi_ports_inactive`
  - 后续还需要继续补更自动化的 start/stop 驱动、交互式 button 场景的结果收口与压力循环脚本
- `T16`
  - 结构化汇总报告和发版前回归入口
  - 当前已补第一版统一汇总脚本：
    - `pp_main/test/scripts/summarize_ugripper_rollout.py`
  - 当前已有统一结果出口：
    - `tmp/board_results/release_gate_merge13_20260421/test_summary.json`
  - 当前仍未完成的关闭条件：
    - 需要把 `camera/sensor/service` 三条标准脚本在同一最新安装包版本上重跑，消除 `mixed_package_versions`

- `T12`
  - 当前已覆盖：
    - `button debounce / dual chord / reset`
    - 自定义 button timing 阈值
    - protocol parser resync 基础行为
    - `recording / validation_failed / error` 到 LED 状态链路
    - gripper `retry / recovery / reconnect` 统计阈值
    - `check_gripper_ack_log.py` 的 `io_summary` 门禁
    - `check_gripper_ack_log.py` 的周期控制流量门禁：
      - `tx_led`
      - `tx_beep`
      - `tx_state_req`
      - `rx_frames`
      - `rx_key_reports`
      - `rx_beep_states`
    - `check_hmi_event_log.py` 的 `health_fault -> led_target` 对齐门禁
    - `check_hmi_event_log.py` 的 `health fault key -> led_state` 指定分支门禁
    - gripper reconnect 状态机的纯逻辑推进
    - `GripperHmiDriver` calibration `ack / retry / recovery / fallback` 判定逻辑
    - `GripperHmiDriver` status/data frame buffer scan 规则
    - `GripperHmiDriver` fixed-size frame checksum resync 规则
    - `GripperHmiDriver` exclusive drain 的 idle/max cutoff 规则
    - `GripperHmiDriver` `readBytesLocked` no-data timeout 合约
    - calibration range collect loop 的 chunk/status/garbage 推进规则
    - calibration range retry decision 与 chunk read retry-recovery 决策规则
    - `readCalibrationFrameLocked` 的 target token scan/resync 规则
  - 还未完成：
    - 更贴近 `GripperHmiDriver` 串口 calibration 读链路外壳的 very-near-I/O 测试，例如 `requestSingleChunkRead`/`readDataOrStatusFrameLocked` 组合路径
    - 更完整的 HMI 状态桥接覆盖
    - `gripper/hmi/button` 主动刺激型板端脚本化收口

补充说明：

- 本节描述的是“测试资产已落地/已接线”的现状
- 不等同于“所有测试都已在最新板端包上完整重跑”
- 对外或阶段汇报时，应单独补一列“最近一次重跑验证时间”和“重跑平台”
- `2026-04-22` 补充边界：
  - 已用现有板端日志重放验证增强后的 `check_gripper_ack_log.py`
  - 例如：
    - `board_service_integration_merge15_20260422/ugripper_service.log` 可得到：
      - `tx_led=545321`
      - `tx_beep=72`
      - `tx_state_req=18670`
      - `rx_frames=18997`
    - `gripper_hmi_active_merge15_manual/ugripper_service.log` 可得到：
      - `tx_led=4310`
      - `tx_beep=4`
      - `tx_state_req=171`
      - `rx_frames=178`
  - 这些结果证明“周期控制流量统计口径”已经落地并能从真实板端日志中提取
  - 但这还不是新的板端重跑结论；若要正式关闭“gripper 控制帧稳定性”增强回归，还需要用新门禁参数补跑一轮 `service/gripper_hmi` 板端脚本
- 最近一次已确认的板端重跑：
  - `2026-04-21`，平台 `HSD-RB1021 / ugripper 1.2.8+merge8`
  - 已通过：`board_camera_stress.sh`、`board_sensor_smoke.sh`、`board_service_integration_check.sh`
  - 结果文件已回传：
    - [camera_stress_20260421_110053](/home/songwl/swl_ws/ugripper/tmp/board_results/camera_stress_20260421_110053)
    - [sensor_smoke_20260421_111554](/home/songwl/swl_ws/ugripper/tmp/board_results/sensor_smoke_20260421_111554)
    - [service_integration_20260421_111733](/home/songwl/swl_ws/ugripper/tmp/board_results/service_integration_20260421_111733)
- 最近一次已确认的本地 x86 standalone 构建验证：
  - `2026-04-21`，使用 `ppmain_build.sh` 同口径环境变量：
    - `CMAKE_PREFIX_PATH=/opt/openrobots:...`
    - `PKG_CONFIG_PATH=/opt/openrobots/lib/pkgconfig:...`
    - `LD_LIBRARY_PATH=/opt/openrobots/lib:...`
    - `-DCMAKE_TOOLCHAIN_FILE=/home/songwl/swl_ws/pp_main/cmake/x86_64-linux-toolchain.cmake`
  - 基于现有 `build/x86/core` 中的 `PPCoreConfig.cmake`，新建 `build/x86/standalone_verify`
  - `cmake -S standalone ...` 配置通过，`cmake --build ... --target UgripperRuntime -j8` 通过
  - 说明此前 `all` 入口失败的直接原因是调用方式未带标准 toolchain / 环境，不是本机缺少 `openrobots/casadi`

### 8.2 下一步执行顺序

为了尽快进入可重复回归，后续建议固定按下面顺序推进：

1. 继续补完 `T12`
   - 把 `record_runtime`、`HMI`、`gripper` 里剩余高价值策略先锁进 `host-only`
2. 开始 `T10`
   - 把 `MCAP` 连续性、timestamp、flush/stop 尾段规则从分析器下沉到单测
3. 开始 `T11`
   - 把 camera validation、坏段映射、窗口失败策略下沉到单测
4. 开始 `T13-T15`
   - 把目前已验证过的板端命令整理成标准脚本，而不是继续手工拼接
5. 最后做 `T16`
   - 汇总 `host-only + board smoke + stress` 结果，形成统一报告出口

## 9. 执行方式

### 9.1 先构建 standalone 目标

如果要验证合并后的 4 个目标先能正常编译：

```bash
cd /home/songwl/swl_ws/pp_main
cmake -S standalone -B build/x86/standalone_ros2 -DCMAKE_BUILD_TYPE=Release -DBUILD_TARGETS=CameraRecorder,SensorRecorder,GripperHmiTool,UgripperRuntime
cmake --build build/x86/standalone_ros2 --target CameraRecorder SensorRecorder GripperHmiTool UgripperRuntime -j8
```

### 9.2 构建 `pp_main/test`

如果要跑正式 `host-only` 自动化测试：

```bash
cd /home/songwl/swl_ws/pp_main
cmake -S test -B build/x86/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/x86/test -j8
```

### 9.3 用 `ctest` 批量跑

跑全部 `host-only`：

```bash
cd /home/songwl/swl_ws/pp_main
ctest --test-dir build/x86/test --output-on-failure -L host-only
```

按模块筛选：

```bash
ctest --test-dir build/x86/test --output-on-failure -L camera
ctest --test-dir build/x86/test --output-on-failure -L sensor
ctest --test-dir build/x86/test --output-on-failure -L gripper_hmi
ctest --test-dir build/x86/test --output-on-failure -L record_runtime
```

按测试名筛选：

```bash
ctest --test-dir build/x86/test --output-on-failure -R "AudioCoordinator|Button|Stereo"
```

### 9.4 直接跑单个 `gtest` 二进制

例如：

```bash
cd /home/songwl/swl_ws/pp_main
./build/x86/test/src/camera_recorder/test_camera_config
./build/x86/test/src/sensor_recorder/test_encoder_protocol
./build/x86/test/src/gripper_hmi/test_hmi_protocol
./build/x86/test/src/record_runtime/test_button_logic
```

只跑单个 case：

```bash
./build/x86/test/src/record_runtime/test_button_logic --gtest_filter='*'
```

### 9.5 Host-only 组件 smoke

这类测试适合先确认 standalone 程序壳子没坏：

```bash
cd /home/songwl/swl_ws/pp_main
./build/x86/standalone_ros2/CameraRecorder/CameraRecorder --help
./build/x86/standalone_ros2/SensorRecorder/SensorRecorder --help
./build/x86/standalone_ros2/SensorRecorder/zeroing --help
./build/x86/standalone_ros2/GripperHmiTool/GripperHmiTool --help
./build/x86/standalone_ros2/UgripperRuntime/UgripperRuntime --help
```

`CameraRecorder` 的 dry-run：

```bash
cd /home/songwl/swl_ws/pp_main
rm -rf /tmp/pp_camera_dry_run
./build/x86/standalone_ros2/CameraRecorder/CameraRecorder \
  --output-dir /tmp/pp_camera_dry_run \
  --config-yaml test/src/camera_recorder/config_samples/schema_v1_valid.yaml \
  --dry-run
```

### 9.6 板端二进制与服务 smoke

常用检查包括：

```bash
systemctl status ugripper.service --no-pager -l
journalctl -u ugripper.service --since '10 min ago' --no-pager -o short-precise -l
cat /tmp/umi_stereo_camera_status.json
findmnt /mnt/data_disk
find /mnt/data_disk/<device_sn>/data/episode_xxx -maxdepth 1 -type f | sort
```

独立二进制 smoke：

```bash
./bin/CameraRecorder/CameraRecorder --help
./bin/SensorRecorder/SensorRecorder --help
./bin/SensorRecorder/zeroing --help
./bin/GripperHmiTool/GripperHmiTool --help
timeout -s INT 6s ./bin/SensorRecorder/SensorRecorder /tmp/sensor_direct_smoke
./bin/GripperHmiTool/GripperHmiTool --duration 3 --poll-ms 100
```

### 9.7 板端压力测试

建议固定 3 类。

#### 长录压力

- 20min
- 60min
- 2h

检查项：

- service 是否还活着
- `journalctl` 是否有 `validation_failed`
- episode 文件是否齐
- `ffprobe` 是否全部可读

#### 高 CPU 压力

在系统高负载下录制，重点盯主摄和 stop/validation。

检查项：

- 主摄 span
- `validation_failed`
- stop 总耗时
- 音频/HMI 是否还能正常响应

#### 高频启停压力

- 连续开始/停止 50 次以上

检查项：

- 是否出现空 episode
- 是否出现 span 过短
- 是否出现残留子进程

### 9.8 第一版板端脚本入口

当前第一版统一入口如下：

```bash
cd /home/songwl/swl_ws/pp_main

test/scripts/board_sensor_smoke.sh \
  --output-dir /tmp/pp_board_sensor_smoke

test/scripts/board_camera_stress.sh \
  --output-dir /tmp/pp_board_camera_stress \
  --duration-sec 30 \
  --cpu-workers "$(nproc)" \
  --decode-check

test/scripts/board_gripper_hmi_log_check.sh \
  --journal-since '15 min ago' \
  --output-dir /tmp/pp_board_gripper_hmi_log_check

test/scripts/board_gripper_hmi_active_check.sh \
  --output-dir /tmp/pp_board_gripper_hmi_active_check
```

当前脚本职责边界：

- `board_sensor_smoke.sh`
  - 独立拉起 `SensorRecorder`
  - 产出左右 `MCAP`
  - 自动调用 `check_sensor_mcap`
- `board_camera_stress.sh`
  - 独立拉起 `CameraRecorder`
  - 可选启动 CPU busy-loop 压力
  - 自动调用 `check_video_windows.py`
- `board_gripper_hmi_log_check.sh`
  - 收集或复用 `ugripper.service` 日志
  - 自动调用 `check_gripper_ack_log.py`
  - 自动调用 `check_hmi_event_log.py`
- `board_gripper_hmi_active_check.sh`
  - 先停服务，调用 `GripperHmiTool` 做 direct HMI `READY / RECORDING / ERROR_1 / beep` 刺激
  - 再自动串接 `board_service_restart_check.sh` 恢复服务
  - 提示人工执行固定按键序列，并自动调用 `board_gripper_hmi_log_check.sh` 收口日志分析
  - 额外产出 `direct_hmi_visual.txt` 记录人工确认结果
  - 当前已知边界：
    - 若板端安装包未输出 `[HMI_DIAG] / [GRIPPER_DIAG]`，则 analyzer 只能报告“缺少 required event / led state”，不能单独证明按钮逻辑或 gripper 控制失败
    - `strings` 级别确认若安装包二进制本体就不含这些字符串，则问题不在 `journalctl` 参数或脚本采集方式，而在板端包版本本身
    - 对旧包或未带诊断日志的包，仍应同时保留 raw runtime 关键事件检查
- `board_service_integration_check.sh`
  - 统一收口 `ugripper.service` 集成检查
  - 自动收集 `systemctl` / `journalctl` / stereo status / latest episode
  - 自动检查 episode 文件齐套性
  - 自动调用 `check_sensor_mcap`、`check_video_windows.py`
  - 自动调用 `check_gripper_ack_log.py`、`check_hmi_event_log.py`
- `board_service_restart_check.sh`
  - 聚焦 `service restart -> active -> stereo ready` 恢复链
  - 自动收集重启后的 `systemctl` / `journalctl` / stereo status
- `board_service_episode_loop_check.sh`
  - 聚焦多轮新 episode 观察
  - 适合配合人工按键进行 start/stop 循环
  - 可对每轮新 episode 调用 `board_service_integration_check.sh`

## 10. 推荐执行顺序

建议把测试执行顺序固定成下面这样。

1. 每次改纯逻辑后先跑 `L0 host-only`
2. 每次改 CLI/配置/构建后跑 `L1 host-only smoke`
3. 每次改设备访问、stop/validate、音频/HMI 逻辑后跑 `L2 board smoke`
4. 每次准备提测或发包前跑 `L3 service integration`
5. 每轮阶段性收口前跑 `L4 stress`

## 11. 当前优先级建议

如果按风险和收益排序，优先补下面 5 组测试。

### P0

- `SensorRecorder` 的 MCAP 连续性检查
- `CameraRecorder/UgripperRuntime` 的 validation policy 单测
- 主摄高 CPU 压力测试，重点覆盖约 `20s` 问题段
- 把“当前已有板端脚本”的执行结果纳入统一汇总，而不是继续散落在临时目录

### P1

- HMI 按键时序与防抖单测
- HMI transport resync / retry 单测
- runtime state -> LED state bridge 单测
- `SensorRecorder` motion alert
- `UgripperRuntime` tactile warning / `umount` / reconnect refresh

### P2

- 高频 start/stop 集成压力测试
- service 启动/恢复/重启后的回归测试
- calibration 软件链路二次确认
- 统一 `ctest + analyzer + board script` 的 release gate

## 12. 结论

`pp_main` 合并后代码当前已经具备一层正式的 `host-only` 自动化测试基础，但对真实设备链路、长时稳定性和现场高压问题，还需要按本文档的 `L2-L4` 分层继续补齐。

后续执行原则应保持为：

- 纯逻辑优先进入 `L0`
- 组件壳层与配置入口进入 `L1`
- 板端设备链路进入 `L2-L3`
- 现场偶发问题与稳定性问题进入 `L4`
