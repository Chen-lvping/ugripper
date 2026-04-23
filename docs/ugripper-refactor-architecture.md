# Ugripper 重构后架构设计与当前实现

本文档用于汇报 `ugripper` 重构工作的最终设计意图，以及这些设计现在如何落在当前 `ugripper` 仓库的回迁实现中。

这份实现最初在 `pp_main` 中完成合并与验证；截至 2026-04-23，四个核心对象与对应测试入口已经同步回本仓，当前应基于下面这个现实前提展开：

- `ugripper` 的四个核心目标对象已经回迁到本仓 `standalone/`
- 当前主代码实现真源已经在 `standalone + src/utils + test`
- `ugripper` 仓库同时继续承担部署、打包、文档、板端结果归档，以及后续作为 `pp_main` submodule 使用的源码真源角色

本文档回答 4 个问题：

1. 这轮重构真正想收什么边界
2. 这些边界现在在本仓里怎么落地
3. 当前还保留在哪些 `ugripper` 仓库资产里
4. 现在还差哪些后续收口项

相关文档：

- [docs/agent/current-status.md](/home/songwl/swl_ws/ugripper/docs/agent/current-status.md)
- [docs/agent/overview.md](/home/songwl/swl_ws/ugripper/docs/agent/overview.md)
- [docs/ugripper-refactor-plan.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/ugripper-refactor-plan.md)
- [docs/standalone-merge-plan.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/standalone-merge-plan.md)
- [docs/reference-aligned-refactor-next-steps.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/reference-aligned-refactor-next-steps.md)
- [docs/REFACTOR_LOG.md](/home/songwl/swl_ws/ugripper/docs/REFACTOR_LOG.md)

## 1. 当前总体判断

当前不应再把系统理解成：

- “当前真源还在 `pp_main`，本仓只是打包壳”
- “四个对象还在等待迁回”
- “测试入口还没有回到本仓”

按当前文档和实现事实，正确判断应是：

- `pp_main` 合并已经完成，且本仓已完成按现状回迁
- 当前设计的主要实现已经落在本仓 `standalone/`、`src/utils/` 与 `test/`
- `ugripper` 仓库中同时保留：
  - 主实现源码
  - 打包与部署资产
  - 运行脚本与资源真源
  - 文档与测试结论口径

因此，讨论“当前设计和实现”时，默认应区分两类真源：

### 1.1 本仓中的实现真源

这些是当前已落地的主实现：

- `standalone/UgripperRuntime`
- `standalone/CameraRecorder`
- `standalone/SensorRecorder`
- `standalone/GripperHmiTool`
- `src/utils`
- `test/src`
- `test/scripts`

### 1.2 本仓中的部署与口径真源

这些仍以本仓库为真源：

- `run_record.sh`
- `auto_update/`
- `auto_calibration/`
- `config/`
- `audio/`
- `audio_en/`
- `pack_script/`
- `build_deb.sh`
- `usb_updater_build.sh`
- `docs/`
- `tmp/board_results/`

## 2. 重构目标现在应如何理解

这轮重构的目标，从来都不是把 `ugripper` 改造成单进程大应用，也不是在本仓库里长期维护一套和主仓库平行的第二套主实现。

它实际完成的是 5 件事：

1. 保留 supervisor 多进程模型，但把 `record_runtime` 的进程边界和控制面拆清
2. 把 `camera_recorder` 的纯逻辑和 app 壳边界拆清
3. 把 `sensor_recorder` 的 protocol/domain 与设备/runtime 壳边界拆清
4. 把 `gripper_hmi` 的 protocol / led effects / driver 边界拆清
5. 建立正式 host-only 测试入口和板端 analyzer / script 入口

这些目标现在都已经不只是“仓内计划”，而是已经在当前仓库中形成实际落地结构。

## 3. 当前总体结论

截至当前状态，可以把整体结论概括为：

- 基线、路径、配置、行为等价口径已经冻结
- 最小公共层和日志 API 兼容收口已经完成
- camera / sensor / runtime / gripper_hmi 的第一轮边界拆分已经完成
- 四个核心对象已迁回本仓 `standalone/`
- 共用基础能力已对齐到本仓 `src/utils`
- 正式 host-only 单测与板端 analyzer / script 入口已迁回本仓 `test`
- `ugripper` 仓库当前继续统一承载主实现、打包、部署、运行脚本、资源和文档真源

因此，当前系统更适合被描述为：

- “重构设计已经在当前仓库中落地”
- 而不是：
- “仓内重构已经准备好，等待并仓”

## 4. 当前设计如何落到实现

### 4.1 `UgripperRuntime`

设计目标：

- 保留 `record_runtime -> camera_recorder / sensor_recorder` 的多进程 supervisor 关系
- 不把系统改成单进程大应用
- 把 process boundary 和 control-plane 从顶层运行时壳里拆出来

当前已落地实现：

- app 壳：
  - `standalone/UgripperRuntime/main.cc`
  - `standalone/UgripperRuntime/record_runtime.cc`
- process boundary：
  - `standalone/UgripperRuntime/runtime_process.cc`
- runtime domain / control plane：
  - `standalone/UgripperRuntime/runtime_domain.cc`

当前 `CMake` 结构也已经明确体现为：

- `runtime_process_boundary`
- `runtime_control_plane`
- `UgripperRuntime`

这说明 runtime 的边界已经不再只是设计稿，而是现行实现结构。

### 4.2 `CameraRecorder`

设计目标：

- 保留 app 壳和运行时逻辑
- 把 camera 类型、配置、命令拼装、registry 和 stereo control 逻辑从巨型实现里拆开
- 维持克制的物理实现文件数量

当前已落地实现：

- app 壳：
  - `standalone/CameraRecorder/main.cc`
  - `standalone/CameraRecorder/camera_recorder.cc`
- domain：
  - `standalone/CameraRecorder/camera_domain.cc`

当前 `CMake` 中已经明确：

- `camera_domain` 独立静态库已建立
- `CameraRecorder` 继续作为 app target 存在
- `libusb` / `gstreamer` 仍按可选能力接入
- 主摄在无 `gstreamer` 时仍可回退 shell-based 路径

因此，camera 这块应理解为：

- 不是“未来要拆”
- 而是“已经按 `app 壳 + camera domain` 落地”

### 4.3 `SensorRecorder`

设计目标：

- 把 protocol / domain 变成可测试公共层
- 保留设备驱动、MCAP 和 runtime 装配在应用侧

当前已落地实现：

- protocol：
  - `standalone/SensorRecorder/sensor_protocol.cc`
- domain：
  - `standalone/SensorRecorder/sensor_domain.cc`
- app 与驱动装配：
  - `standalone/SensorRecorder/main.cc`
  - `standalone/SensorRecorder/im648_driver.cc`
  - `standalone/SensorRecorder/encoder_driver.cc`
- 工具：
  - `standalone/SensorRecorder/zeroing.cc`

当前 `CMake` 中也已经清楚表达：

- `sensor_protocol`
- `sensor_domain`
- `SensorRecorder`
- `zeroing`

所以 sensor 这一块当前的准确描述应是：

- protocol/domain 已经并仓落地
- 设备 transport/runtime 壳仍然保持应用侧装配

### 4.4 `GripperHmiTool`

设计目标：

- 协议层、LED 效果层、driver 层分开
- tool 入口保持薄壳
- 让 runtime 通过库依赖接入，而不是把 HMI 细节散落到主控里

当前已落地实现：

- protocol：
  - `standalone/GripperHmiTool/gripper_hmi_protocol.cc`
- led effects：
  - `standalone/GripperHmiTool/gripper_hmi_led_effects.cc`
- driver：
  - `standalone/GripperHmiTool/gripper_hmi_driver.cc`
- tool 入口：
  - `standalone/GripperHmiTool/main.cc`

当前 `CMake` 已按分层静态库接入：

- `gripper_hmi_protocol`
- `gripper_hmi_led_effects`
- `gripper_hmi`
- `GripperHmiTool`

这意味着 HMI 相关重构现在也已经不是仓内预备状态。

### 4.5 测试体系

设计目标：

- 不再只依赖零散 smoke
- 形成正式 host-only 单测入口
- 保留板端脚本和 analyzer 作为 field test 层

当前已落地实现：

- runtime host-only 单测：
  - `test/src/record_runtime`
- camera host-only 单测：
  - `test/src/camera_recorder`
- sensor host-only 单测：
  - `test/src/sensor_recorder`
- gripper/hmi host-only 单测：
  - `test/src/gripper_hmi`
- 板端脚本和 analyzer：
  - `test/scripts`

当前一些关键实现特征已经能直接从 `CMake` 看出来：

- runtime 单测会直接链接：
  - `runtime_process_boundary`
  - `runtime_control_plane`
- camera 单测会直接链接：
  - `camera_domain`
- analyzer / board script 入口已经以 `test/scripts` 为准

因此，当前测试结构应表述为：

- `test` 是测试实现真源
- `docs` 和 `tmp/board_results` 是测试计划、结论和归档真源

## 5. 当前哪些资产仍然留在 `ugripper`

虽然这份实现最初在 `pp_main` 中完成合并，但当前也不应再把本仓库当成“只有打包壳、没有主实现”的仓库。

本仓库现在仍然负责：

- 主实现源码
  - `standalone/`
  - `src/utils/`
  - `test/`
- 安装与运行入口
  - `run_record.sh`
- 打包与部署模板
  - `pack_script/`
  - `build_deb.sh`
  - `usb_updater_build.sh`
- 升级 / 校准脚本
  - `auto_update/`
  - `auto_calibration/`
- 配置与资源
  - `config/`
  - `audio/`
  - `audio_en/`
- 文档和汇报口径
  - `docs/`
- 板端结果归档
  - `tmp/board_results/`

所以当前最准确的职责边界是：

- `ugripper` 负责当前上传分支所需的主实现、测试、部署、打包、资源、文档和结果归档
- `pp_main` 后续应改为通过 submodule 或同步方式消费这里的实现

## 6. 当前进度判断

如果以“设计是否已经落地”为标准，当前状态应判断为：

| 项目 | 当前状态 | 说明 |
| --- | --- | --- |
| supervisor 多进程模型保留 | 已落地 | `UgripperRuntime` 已按 process/domain 分层接入 |
| camera 纯逻辑边界 | 已落地 | `CameraRecorder` 已形成 `app 壳 + camera_domain` |
| sensor protocol/domain 边界 | 已落地 | `SensorRecorder` 已形成 protocol/domain + app 装配 |
| gripper/hmi 分层 | 已落地 | `GripperHmiTool` 已形成 protocol/effects/driver/tool 结构 |
| host-only 单测入口 | 已落地 | `test/src/*` 已承载主测试真源 |
| 板端脚本和 analyzer 入口 | 已落地 | `test/scripts/*` 已承载主脚本真源 |
| 打包脚本对 standalone 新布局收口 | 已落地 | `build_deb.sh` 已优先打包 `bin/...` 新布局，并兼容旧 `build/src/...` fallback |
| service / packaging 最终统一到主仓库 | 未完成 | 当前上传阶段仍以本仓打包链为主 |
| 为临时上传而再拆第二套主实现 | 不应开始 | 当前没有必要 |

## 7. 当前未完成项

当前真正还没完成、但与这份架构文档相关的，是：

- `pp_main` 改用 submodule 后的拉取、构建、发布流程收口
- 本仓与 `pp_main` 之间后续同步方式的治理
- 当前各类实现、部署、测试真源之间的同步流程进一步规范

但这些都不改变一个事实：

- 当前主代码架构已经在当前仓库中落地

## 8. 汇报时可直接使用的结论

可以直接使用下面这段摘要：

`ugripper` 这轮重构的核心成果，不再只是“仓内重构准备就绪”，而是已经形成了当前仓库可独立构建、测试、打包的实际落地实现。当前 `UgripperRuntime` 保留 supervisor 多进程模型并拆分出 process boundary 与 runtime domain；`CameraRecorder` 已形成 `app 壳 + camera_domain`；`SensorRecorder` 已形成 `protocol/domain + app 装配`；`GripperHmiTool` 已形成 `protocol / led effects / driver / tool` 分层；正式 host-only 单测与板端 analyzer / script 入口也都已迁回本仓 `test`。与此同时，`build_deb.sh` 已按 `bin/UgripperRuntime`、`bin/CameraRecorder`、`bin/SensorRecorder`、`bin/GripperHmiTool` 新布局完成收口。因此当前最准确的描述不是“等待并仓”，而是“主实现已迁回 `ugripper` 仓库，后续由 `pp_main` 通过 submodule 或同步方式消费”。 
