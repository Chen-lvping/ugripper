# `pp_main` 合并后 `ugripper` 测试结构与当前结果简表

这份文档是历史全量测试计划的压缩版索引，用来快速说明现在的测试结构、有哪些主要测试项，以及当前已拿到的结果。完整历史计划已归档到 [ppmain-ugripper-test-plan.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/ppmain-ugripper-test-plan.md)。

## 1. 测试结构

当前测试大致分成 `5` 层：

1. `host-only / gtest / ctest`
   - 目标：验证运行时编排、helper、边界条件和合成输入
   - 特点：快、适合回归，但不能单独证明真机录制链路
2. `离线 analyzer`
   - 目标：对 `episode` 产物做结构化检查
   - 典型内容：`MCAP` topic/span/gap、主摄视频窗口检查、gripper/HMI 日志归档
3. `板端标准脚本`
   - 目标：把板上常见验证路径脚本化，形成可复跑入口
   - 典型入口：
     - `board_sensor_smoke.sh`
     - `board_camera_stress.sh`
     - `board_service_integration_check.sh`
     - `board_service_episode_loop_check.sh`
4. `板端人工主动刺激`
   - 目标：覆盖 `gripper/HMI/button` 这类必须结合人眼/人耳/按键动作确认的路径
   - 典型入口：`board_gripper_hmi_active_check.sh`
5. `长录 / 压力 / release gate`
   - 目标：验证长时运行稳定性、CPU 压力下录制、以及多 suite 汇总结论
   - 典型内容：`20min/1h service soak`、`120s camera stress`、统一汇总 JSON

## 2. 主要测试项

当前已经稳定在用的测试项可以按功能看：

- `Camera`
  - `board_camera_stress.sh`
  - 主摄窗口分析 `check_video_windows.py`
- `Sensor`
  - `board_sensor_smoke.sh`
  - `check_sensor_mcap`
- `Service`
  - `board_service_integration_check.sh`
  - `board_service_episode_loop_check.sh`
- `Gripper / HMI / Button`
  - `board_gripper_hmi_active_check.sh`
  - `check_gripper_ack_log.py`
  - `check_hmi_event_log.py`
- `统一汇总`
  - 多 suite 结果汇总 JSON
  - 文档回填到测试计划和 `REFACTOR_LOG`

## 3. 当前结果快照

截至 `2026-04-22`，当前最有代表性的板端结果是 `merge16` 这批样本：

- `gripper_hmi` 人工交互增强回归
  - 结果目录：`tmp/board_results/gripper_hmi_active_merge16_manual_20260422_134215`
  - 结论：`ok=true`
  - 已证明：`READY / RECORDING / ERROR_1 / beep`、`ShortUpPressed / ShortDownPressed / ShutdownPromptRequested`、日志 analyzer 收口都能通过
- `camera` 长时压力复核
  - 结果目录：`tmp/board_results/camera_stress_long_merge16_20260422_135009`
  - 结论：`ok=true`
  - 已证明：`120s`、`90%+ CPU`、`6` 路录制下，主摄 host 全量窗口复核通过
- `service` `20min` soak
  - 结果目录：`tmp/board_results/service_soak20_merge16_20260422_152341`
  - 结论：`ok=true`
  - 已证明：`service` 入口在 `20min / CPU pressure` 条件下，runtime validation、sensor analyzer、主摄宿主机窗口检查都通过
- `service` `1h` soak
  - 结果目录：`tmp/board_results/service_soak1h_merge16_20260422`
  - 结论：`validation_ok=true`
  - 已证明：约 `1h 06m` 长录、CPU 平均 busy `95.2%` 条件下，未出现 `motion alert`、`health fault`、`recorder exited`
- `service` `1h` soak 离线归档
  - 结果目录：`tmp/board_results/service_soak1h_merge16_20260422_analysis`
  - 结论：`episode_summary_ok=true`
  - 已证明：`episode_20260422_0050` 的 `sensor/gripper/hmi` 离线 analyzer 通过，左右主摄 host full sweep 通过，且已在宿主机直接复用 `check_video_windows.py` 完成整段窗口复核
  - 补充结果：
    - `left_cam_main_host_full -> ok=true, windows=1987, min_frame_count=97, max_frame_count=123`
    - `right_cam_main_host_full -> ok=true, windows=1987, min_frame_count=102, max_frame_count=122`
    - `left_cam_main_host_script_full -> ok=true, windows=1987, min_frame_count=100, max_frame_count=123`
    - `right_cam_main_host_script_full -> ok=true, windows=1987, min_frame_count=102, max_frame_count=123`
- `motion alert` 偶发复测
  - 结果目录：`tmp/board_results/motion_alert_reprobe_merge16_20260422`
  - 结论：`5/5` 静止短录未复现
  - 已证明：当前更接近低概率偶发现象，而不是稳定必现问题

## 4. 现在可以怎么理解这些结果

当前可以比较有把握地说：

- `merge16` 不是“短录一好一坏”的不稳定状态了，至少核心链路已经有多组真机正样本
- `camera / service / gripper_hmi` 三条主路径都已经有板端实测证据，不再只是本地静态分析
- 高 CPU 条件已经被实际覆盖，不是只在空载环境下通过

但现在还不建议过度外推：

- `service 1h` 这轮虽然已经在宿主机直接复用了板端默认 `check_video_windows.py` 做完整窗口检查，但运行位置仍然是宿主机，不是板端原地跑满 `1h`
- `motion alert` 的一次启动期异常目前只能归类为低概率偶发现象，不能直接宣称根因已关闭
- 若后面要做最终 release gate，仍应把当前 `merge16` 各 suite 再汇成一份统一报告

## 5. 推荐把它和哪份文档一起看

- 需要看完整边界、历史回归、每轮结果目录时：看 [ppmain-ugripper-test-plan.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/ppmain-ugripper-test-plan.md)
- 需要看这几轮是怎么一步步推进到当前状态时：看 [REFACTOR_LOG.md](/home/songwl/swl_ws/ugripper/docs/REFACTOR_LOG.md)
