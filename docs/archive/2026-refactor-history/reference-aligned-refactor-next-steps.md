# Reference-Aligned Refactor Next Steps After A3

本文档用于把以下 4 份 reference 文档的新增要求，与当前已经执行到 `A3` 的主重构计划合并：

- `docs/ppmain-ugripper-test-refactor-reference.md`
- `docs/ppmain-ugripper-camera-refactor-reference.md`
- `docs/ppmain-ugripper-naming-yaml-refactor-reference.md`
- `docs/ppmain-ugripper-process-thread-refactor-reference.md`

它不是替代：

- `docs/ugripper-refactor-plan.md`
- `docs/standalone-merge-plan.md`

而是给出一版更保守、可回溯、可以直接接在 `A3` 之后执行的“合并版步骤表”。

## 1. 当前基线

截至当前工作树，主计划进度可视为：

- `A1` 基线冻结：已完成
- `A2` 最小 `src/utils`：已完成
- `A3` C++ 日志统一：已完成
- `Post-A3 Step 1` Reference Freeze And Traceability Alignment：已完成
- `Post-A3 Step 2` 正式测试入口最小落地：已完成
- `Post-A3 Step 3` 路径台账与真源目录冻结：已完成
- `Post-A3 Step 4` Naming / YAML Contract Freeze：已完成
- `Post-A3 Step 5` Sensor Common 提升为可测试模块：已完成
- `Post-A3 Step 6` Camera Pure Logic First：已完成
- `Post-A3 Step 7` Runtime Process Boundary Refactor：已完成
- `Post-A3 Step 8` Runtime Domain Split：已完成
- `Post-A3 Step 9` Scripts And Field-Test Boundary Cleanup：已完成

当前代码现状里，有 4 个非常关键的事实需要作为后续步骤设计依据：

1. `record_runtime` 仍然是主控 / supervisor，但已具备第一版 process boundary 与 control-plane 协作者
   - 见 `src/record_runtime/include/record_runtime.h`
   - 见 `src/record_runtime/src/record_runtime.cpp`
   - 见 `src/record_runtime/include/record_runtime/process_supervisor.h`
   - 见 `src/record_runtime/include/record_runtime/audio_coordinator.h`
   - 见 `src/record_runtime/include/record_runtime/stereo_session_client.h`
   - 见 `src/record_runtime/include/record_runtime/hmi_controller.h`
   - 见 `src/record_runtime/include/record_runtime/health_monitor.h`
   - 见 `src/record_runtime/include/record_runtime/recording_orchestrator.h`
2. `camera_recorder` 仍保留 app 壳和 runtime 逻辑，但已具备第一版可测试 camera 纯逻辑模块
   - 见 `src/camera_recorder/CMakeLists.txt`
   - 见 `src/camera_recorder/src/camera_recorder.cpp`
   - 见 `src/camera_recorder/include/camera_recorder/camera_config.h`
   - 见 `src/camera_recorder/include/camera_recorder/camera_command_builder.h`
   - 见 `src/camera_recorder/include/camera_recorder/camera_registry.h`
   - 见 `src/camera_recorder/include/camera_recorder/stereo_control_json.h`
3. `sensor_recorder` 已经具备第一版正式可测试库，但 transport / MCAP / runtime 壳仍留在应用侧
   - 见 `src/sensor_recorder/CMakeLists.txt`
   - 见 `src/sensor_recorder/include/sensor_recorder/sensor_protocol.h`
   - 见 `src/sensor_recorder/include/sensor_recorder/sensor_domain.h`
4. 仓库已经具备正式 host-only 测试入口，camera、runtime process boundary 与 runtime domain split 也已进入正式单测集合
   - 顶层 `CMakeLists.txt` 已启用 `BUILD_TESTING`
   - `test/src/` 已承载 `GoogleTest` host-only 单测
   - `test/src/camera/` 已覆盖 config、command builder 和 stereo control/status JSON
   - `test/src/record_runtime/` 已覆盖 process policy、audio coordinator、stereo session client、button logic、runtime health 和 recording orchestrator

这意味着：

- `A1-A3` 不需要推翻
- `Post-A3 Step 1-8` 可以继续保留
- 后续重点转向主仓库并仓前的集成准备与 ARM 验证

## 2. 合并后的总原则

结合 reference 文档和当前代码，后续步骤统一遵守下面 8 条原则。

### 2.1 不推翻 `A1-A3`

已完成的：

- 基线冻结
- 最小 `utils`
- 日志统一

都应保留，不回滚，不重做。

### 2.2 测试体系要前置，但不一次性铺满

不要等 camera / runtime 全部拆完才补测试。

正确顺序是：

1. 先建立正式测试入口
2. 先测现成的纯逻辑和现有库目标
3. 再拆巨型模块

### 2.3 不先做大规模 rename

命名和 YAML 要先收“规则”和“schema”，再做迁移。

当前阶段不做：

- 全仓文件 rename
- 一次性清空旧符号
- 一次性把 `main.cpp` 改成 `main.cc`

### 2.4 保留 supervisor 多进程模型

当前阶段不把 `ugripper` 改造成“单进程大应用”。

Phase 1 仍保留：

- `record_runtime` 作为主控
- `camera_recorder` / `sensor_recorder` 作为独立 worker

学习 `pp_main` 的重点是：

- 进程内生命周期组织
- 线程和任务边界

不是：

- 强行取消 worker 进程边界

补充约束：

- 并仓后仍保留 `record_runtime` 作为 supervisor 主控的形态
- 运行时拆分可以继续做，但实现文件粒度保持克制，`record_runtime` 最终以 `2-3` 个 `.cpp` 为宜
- `pp_main` 的 `TaskScheduler` 只作为设计参考，不作为当前阶段的直接落地依赖

### 2.5 camera 重构优先拆“域模型”，不先碰深层设备访问

当前最先该抽的是：

- `camera_types`
- `camera_config`
- `camera_command_builder`
- `camera_registry`

不是先重写：

- V4L2 采集实现
- stereo 全部采集路径

补充约束：

- `camera_recorder` 允许继续细分头文件，但实现文件总数以 `2-3` 个 `.cpp` 为宜
- 纯逻辑模块可以维持独立头文件，但小体量实现优先归并到少数职责清晰的 `.cpp`

### 2.5.1 logger / 通信 / 调度的靠拢方式

后续向 `pp_main` 靠拢时，统一按下面 3 条执行：

- 日志：先对齐 `DM_LOG_*` API 与调用语义，后对齐 `pp_main/src/utils/logger.h` 的真实后端
- 调度：先参考 `TaskScheduler` 的生命周期和周期任务组织方式，不直接搬入其当前实现和依赖栈
- 通信：先识别 transport 无关的控制通道，再分阶段从 pipe / file 迁到 ZMQ；不在当前步骤直接切换主链 transport

### 2.6 YAML 先兼容，后升级

Phase 1 允许：

- 新 parser 同时兼容旧格式和 `schema_version: 1`
- `output_files` 继续兼容旧写法

但要明确：

- `output_files[0]` 是唯一受主链保证的 primary output

### 2.7 每一步只做一类事情

继续沿用现有主计划约束：

- 测试入口 PR 不混 camera 拆分
- YAML/schema PR 不混路径替换
- runtime 进程监督 PR 不混脚本治理

### 2.8 每一步必须可回溯

每一步至少留下：

- 1 个明确 PR/提交范围
- 1 条 `docs/REFACTOR_LOG.md` 记录
- 1 组已执行测试
- 1 组未执行测试与原因
- 如涉及真源/路径/配置口径变化，补充 `docs/archive/2026-refactor-history/baseline/*` 或相关计划文档

## 3. 对当前主计划的调整结论

不建议直接按原来的：

- `A4 路径台账与路径封装`
- `A5 低风险 helper 收口`
- `A6 camera 抽象收口`

线性继续往下走。

更稳的做法是把 `A3` 之后拆成下面 9 步。

其中：

- 路径治理仍然保留
- 但测试入口、命名/YAML 契约、camera 纯逻辑拆分要提前插入
- runtime 进程/线程收口要晚于 camera 纯逻辑和基础测试入口

## 4. A3 之后的保守执行步骤

下面的步骤是推荐的新顺序。

### Step 1：Reference Freeze And Traceability Alignment

目标：

- 把 reference 文档带来的新增约束固化进执行口径
- 不改业务代码

主要动作：

- 固定本文件作为 `A3` 之后的参考入口
- 在主计划文档中明确：
  - 保留 supervisor 模型
  - 测试入口前置
  - naming/YAML 先 schema 后迁移
  - camera 先抽域模型
- 明确每一步都要更新 `REFACTOR_LOG`

建议影响范围：

- `docs/standalone-merge-plan.md`
- `docs/ugripper-refactor-plan.md`
- `docs/REFACTOR_LOG.md`

完成标准：

- 已新增单一入口文档：`docs/reference-aligned-refactor-next-steps.md`
- `docs/ugripper-refactor-plan.md` 与 `docs/standalone-merge-plan.md` 已吸收以下约束：
  - 保留 supervisor 多进程模型
  - 测试入口前置
  - naming/YAML 先 schema 后迁移
  - camera 先抽域模型
  - camera / runtime 的实现文件粒度保持克制
  - logger / 调度 / 通信按“先接口、后后端”的方式向 `pp_main` 靠拢
- `docs/REFACTOR_LOG.md` 已新增一条“reference 合并”记录
- 下列 reference 文档已被正式标记为 `still authoritative reference input`：
  - `docs/ppmain-ugripper-test-refactor-reference.md`
  - `docs/ppmain-ugripper-camera-refactor-reference.md`
  - `docs/ppmain-ugripper-naming-yaml-refactor-reference.md`
  - `docs/ppmain-ugripper-process-thread-refactor-reference.md`
- 后续执行顺序已固定，不再“边做边猜”

测试标准：

- 文档交叉核对
- 确认与 `A1-A3` 现有记录无冲突

本步骤不做：

- 代码重构
- CMake 改造
- YAML 迁移

### Step 2：正式测试入口最小落地

目标：

- 让仓库从“有一些 smoke 工具”升级为“有正式测试入口”

主要动作：

- 顶层引入 `BUILD_TESTING`
- 增加 `enable_testing()`
- 新建 `test/CMakeLists.txt`
- 新建 `test/src/CMakeLists.txt`
- 固定正式单测框架口径：
  - host-only 单元测试默认统一使用 `GoogleTest`
  - host-only 单元测试与后续 target/ARM 测试允许并存，但必须通过目录、标签和文档明确区分
  - 涉及硬件、设备节点、真实外部进程依赖的测试默认不进入 host-only 单测集合
- 把当前测试分层明确化：
  - 单元测试：进入 `test/src/*`
  - smoke/tool：保留现状，但不再冒充主测试体系
  - field script：继续留在 `test/scripts`

建议第一批测试目标：

- `test/src/utils/test_env_utils.cc`
- `test/src/utils/test_file_utils.cc`
- `test/src/utils/test_time_utils.cc`
- `test/src/gripper_hmi/test_hmi_led_effects.cc`
- `test/src/gripper_hmi/test_hmi_protocol.cc`

完成标准：

- `ctest` 有可执行目标
- 至少有 2 个以上正式单元测试目标
- `utils_smoke_test` 与 `gripper_hmi_test` 的定位被文档化
- `GoogleTest` 被固定为正式单测默认框架
- host-only / field / 后续 ARM 测试的边界被文档化

测试标准：

- `cmake -S . -B build -DBUILD_TESTING=ON`
- `cmake --build build -j$(nproc)`
- `ctest --test-dir build --output-on-failure`
- 原有 `utils_smoke_test` 仍可运行
- 至少有 1 个测试目标带清晰标签或目录归属，表明其属于 host-only 单测

本步骤不做：

- 重写 `camera_recorder`
- 重写 `record_runtime`

### Step 3：路径台账与真源目录冻结

目标：

- 把原计划中的 `A4` 保留下来
- 同时吸收 naming/YAML reference 中“唯一真源目录”约束

主要动作：

- 更新或补充：
  - `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
  - `docs/archive/2026-refactor-history/repo-architecture.md`
  - `docs/repo-migration-map.md`
- 明确真源目录：
  - `src/`
  - `config/`
  - `docs/`
  - `test/`
- 明确非真源目录：
  - `build/`
  - `tmp/`
- 明确冻结候选：
  - `ugripper/`

完成标准：

- 路径台账覆盖运行时关键路径
- 真源/非真源/冻结候选有清晰定义
- 后续重构不再把 `build/`、`tmp/`、历史副本误当真源

测试标准：

- 文档核对
- 关键入口 `--help` / dry-run 输出不变
- 不要求行为级变化验证，因为此步以文档收口为主

本步骤不做：

- 批量替换路径
- 安装前缀切换

### Step 4：Naming/YAML Contract Freeze

目标：

- 不大规模 rename
- 先固定命名口径、namespace 口径、YAML schema 演进策略

主要动作：

- 固定新增公共符号 namespace 目标：
  - `ugripper::camera`
  - `ugripper::runtime`
  - `ugripper::sensor`
  - `ugripper::hmi`
  - `ugripper::config`
  - `ugripper::utils`
- 固定 include 风格优先使用模块前缀
- 为 `camera_recorder.yaml` 设计兼容型 schema：
  - 允许旧格式
  - 支持 `schema_version: 1`
  - 明确 `output_files[0]` 是 primary output
- 固定 deprecated but accepted 的旧字段集合，至少包括：
  - `name`
  - `device`
  - `mode`
  - `uvc_roll_absolute`
  - `width`
  - `height`
  - `fps`
  - `output_files`
  - `qp_init`
  - `qp_max`
  - `qp_min`
  - `qp_max_i`
  - `qp_min_i`
- 固定旧格式迁移边界：
  - `Step 4` 只成文化，不发 parser warning
  - `Step 6` 开始，兼容 parser 对旧平铺字段输出一次性 deprecation warning
  - 在默认配置、测试资产、field 配置样例全部迁移完成前，不禁止旧格式
  - 真正禁用旧格式必须单独立项，并在 `REFACTOR_LOG` 中显式记录
- 新建测试样例目录：
  - `test/src/camera/config_samples/`
- 新增单独 contract 文档：
  - `docs/archive/2026-refactor-history/naming-yaml-contract.md`

完成标准：

- 命名规范和 YAML 契约成文
- parser 兼容策略成文
- deprecated 字段、warning 时点、禁止时点已成文
- `test/src/camera/config_samples/` 已落地样例资产
- 不发生全仓 rename

测试标准：

- 新增 YAML 合法/非法样例
- 当前可执行验证：
  - 旧格式可读
  - 非法字段能稳定报错
- `schema_version: 1` 样例在 Step 4 先作为 contract 资产冻结
- `schema_version: 1` 的可执行 parser 兼容验证后移到 Step 6

本步骤不做：

- 批量重命名文件
- 立即切到新的 YAML 目录布局

### Step 5：Sensor Common 提升为可测试模块

目标：

- 利用 `SENSOR_COMMON_SOURCES` 的现有基础，先拿下一块低风险、可测试收益高的区域

主要动作：

- 从 `sensor_recorder` 中固定抽出两类第一版模块：
  - `sensor_protocol`
  - `sensor_domain`
- 第一批优先抽：
  - `sensor_protocol`：CRC、编码器帧解析、IMU 协议解析辅助
  - `sensor_domain`：timestamp smoothing、样本结构、回压/队列规则中的纯逻辑
- `sensor_recorder` 与 `zeroing` 继续复用该库

完成标准：

- `sensor_recorder` 不再只靠“共享源文件复用”
- `sensor_protocol` 和 `sensor_domain` 至少有一个已成为可被测试目标直接链接的正式库
- `sensor_core` 这类总称不再作为第一版实施命名，避免 reviewer 重新讨论命名

测试标准：

- `test/src/sensor/test_bsp_crc.cc`
- `test/src/sensor/test_encoder_protocol.cc`
- `test/src/sensor/test_imu_batch_timestamp.cc`
- 原有：
  - `sensor_recorder` 本地编译通过
  - `zeroing` 本地编译通过

本步骤不做：

- 串口 transport 深度重写
- MCAP writer 全部重写

### Step 6：Camera Pure Logic First

目标：

- 把 `camera_recorder` 里最容易测的纯逻辑先拆出来
- 不先深挖设备采集实现

主要动作：

- 新建最小 camera 域模型：
  - `camera_domain_types`
  - `camera_config`
  - `camera_command_builder`
  - `camera_registry`
- `camera_recorder.cpp` 保留 app 壳和 runtime 逻辑
- 先把 YAML 解析、mode 选择、命令拼装、输出语义转移到可测试模块

完成标准：

- `camera_recorder` 不再把配置解析、命令拼装、session 语义全部堆在一个 `.cpp`
- `record_runtime` 中与 camera identity 强耦合的常量开始具备迁往 registry 的条件

测试标准：

- `test/src/camera/test_camera_config.cc`
- `test/src/camera/test_camera_command_builder.cc`
- `test/src/camera/test_stereo_control_json.cc`
- `camera_recorder --help`
- `camera_recorder --dry-run`

本步骤不做：

- stereo 采集全部重写
- 新设备后端引入

### Step 7：Runtime Process Boundary Refactor

目标：

- 吸收 process/thread reference
- 但保持 `record_runtime` 的 supervisor 模型

主要动作：

- 从 `record_runtime` 先抽薄执行层：
  - `SubprocessHandle`
  - `ProcessSupervisor`
- 再抽外围协作者：
  - `AudioCoordinator`
  - `StereoSessionClient`
- 保持：
  - `camera_recorder`
  - `sensor_recorder`
  - audio player
  - stereo daemon
  仍是 worker / 外部进程
- 新增约束：
  - 新增 runtime 逻辑不得继续回写到 `record_runtime.cpp`
  - 新功能若属于已抽出职责，必须优先落到 `SubprocessHandle`、`ProcessSupervisor`、`AudioCoordinator`、`StereoSessionClient` 等新模块

完成标准：

- `RecordRuntime` 不再直接持有过多进程执行细节
- worker 启停/超时/升级信号逻辑有单独归属
- `record_runtime.cpp` 不再继续吸收新的进程执行细节

测试标准：

- `test/src/record_runtime/test_process_policy.cc`
- `test/src/record_runtime/test_audio_coordinator.cc`
- `test/src/record_runtime/test_stereo_session_client.cc`
- 本地编译通过
- `record_runtime --help`

本步骤不做：

- 单进程化
- IPC 协议重写

### Step 8：Runtime Domain Split

目标：

- 在保留 supervisor 模型的前提下，把 `record_runtime` 巨型控制面逐步拆成业务模块

主要动作：

- 按 process/thread reference 的保守顺序拆：
  - `HmiController`
  - `HealthMonitor`
  - `RecordingOrchestrator`
- 顶层入口先收成 `RuntimeApp` 风格委托壳，但不在本步做物理类名/文件名替换
- `EpisodeManager` Phase 1 只抽调用边界，不重写内部细节
- 新增约束：
  - 新增 runtime 业务逻辑不得继续写入 `record_runtime.cpp`
  - 被抽出的模块一旦存在，同类新逻辑必须优先落入对应模块，而不是回流到旧文件

完成标准：

- 入口壳更薄
- stop 顺序、故障传播、HMI 输入、健康状态汇总有清晰边界
- `record_runtime.cpp` 不再承载全部控制面逻辑
- 已抽出的控制面模块不出现明显回流

测试标准：

- `test/src/record_runtime/test_button_logic.cc`
- `test/src/record_runtime/test_runtime_health.cc`
- `test/src/record_runtime/test_recording_orchestrator.cc`
- 既有 `test_process_policy` / `test_audio_coordinator` / `test_stereo_session_client` 继续通过
- 本地最小 smoke：
  - `record_runtime --help`
  - 编译通过
  - 无硬件假实现测试通过

本步骤不做：

- 立即切 standalone 目录
- 立即删 `run_record.sh`

### Step 9：Scripts And Field-Test Boundary Cleanup

目标：

- 把 shell/python 从“主链隐式逻辑”降到“明确编排层或 field tool”

主要动作：

- `run_record.sh`
  - 先薄化
  - 保留兼容入口
- `auto_update/usb_auto_update.sh`
  - 先做编排/步骤/错误码收口
- `auto_calibration/run_calibration.sh`
  - 先做日志和路径收口
- `audio/*.py`
  - 先做路径和日志收口

完成标准：

- 高风险脚本职责更清晰
- `test/scripts` 的 field-test 身份更明确
- 不把 field script 混入单元测试构建

测试标准：

- `bash -n` 对相关 shell
- `python3 -m py_compile` 对相关 Python
- 文档化 field test 执行入口
- 必要时保留有限本地 smoke，不要求在此步完成 ARM 验证

执行结果：

- 已新增轻量 shell 公共 helper，统一：
  - 日志前缀
  - `/etc/environment` 读取
  - Python 解释器选择
- `run_record.sh` 已切换到显式 `project_root` / `RUNTIME_BIN_OVERRIDE`，保留兼容入口但不再依赖隐式当前目录
- `auto_calibration/run_calibration.sh` 已切换到显式项目根路径与统一日志输出
- `auto_update/usb_auto_update.sh` 已切换到基于 `PROJECT_ROOT` 的默认脚本/音频路径，减少 `/opt/ugripper` 硬编码扩散
- `audio/*.py` 已新增轻量公共模块，统一 env 读取与日志前缀
- `test/README.md` 已补充主链 shell/python 与 field script 的边界说明

本步骤不做：

- 一次性彻底删除 shell 包装
- 一次性把升级/校准 app 化

## 5. 每一步的记录要求

为保证“操作可回溯”，每一步必须同步留下如下记录。

### 5.1 必须更新

- `docs/REFACTOR_LOG.md`

### 5.2 按需更新

- `docs/archive/2026-refactor-history/baseline/*`
- `docs/ugripper-refactor-plan.md`
- `docs/standalone-merge-plan.md`
- `docs/archive/2026-refactor-history/repo-architecture.md`
- `docs/repo-migration-map.md`
- `test/README.md`

### 5.3 每步记录最少应包含

- 步骤名
- 影响范围
- 主要改动
- 已执行测试
- 未执行测试与原因
- 后续待验证

### 5.4 失败门禁规则

如果某一步计划中的关键测试未通过，则不能进入下一步。

执行规则：

- 只能在当前主题范围内继续补齐、修复或回退
- 不允许带着关键失败进入下一步“先做后补”
- 若需保留例外，必须在 `REFACTOR_LOG` 中显式写明：
  - 未通过的关键测试
  - 失败原因
  - 为什么仍允许继续
  - 谁批准该例外

## 6. 建议的 PR 粒度

为了保持保守和可 review，建议 PR 顺序如下：

1. 测试入口骨架 PR
2. 路径/真源冻结 PR
3. naming/YAML 契约 PR
4. sensor 可测试库 PR
5. camera 纯逻辑 PR
6. runtime process boundary PR
7. runtime domain split PR
8. scripts/field-test 收口 PR

每个 PR 只解决一类问题，不跨主题混改。

## 7. 与 standalone 并仓计划的关系

这份步骤表的目标不是延迟并仓，而是降低并仓风险。

达到下面状态后，再进入 `standalone integration` 会更稳：

- 有正式测试入口
- camera 域模型和 registry 成形
- runtime 保持 supervisor，但控制面已拆清
- naming/YAML 契约已固定
- 脚本和 field test 边界已明确

只有达到这个状态后，再去做：

- `CameraRecorder`
- `SensorRecorder`
- `GripperHmiTool`
- `UgripperRuntime`

迁入 `pp_main/standalone`，才不会把当前仓库的路径硬编码、弱测试、巨型实现和脚本复杂度整体搬过去。

## 8. Step 9 之后、Stage B 之前的新增收口项

`Post-A3 Step 1-9` 已完成，但结合最新要求，在真正进入 `Stage B` 之前，还需要补一轮“向 `pp_main` 靠拢、但不提前并仓”的收口。

这一轮不是推翻已有重构，而是把当前已拆出的边界收成更适合并仓的最终形态。

### 8.1 目标与边界

这一轮只做 4 类事：

1. 收紧 `camera_recorder` 的实现文件粒度
2. 收紧 `record_runtime` 的实现文件粒度
3. 明确 logger 与 `pp_main` 的兼容差距和收口顺序
4. 明确线程调度与通信向 `pp_main` 靠拢的渐进路线

这一轮不做：

- 提前进入 `standalone/<AppName>/`
- 提前切安装路径、service、打包入口
- 直接引入 `pp_main` 的 ROS / `spdlog` / `oneTBB` / ZMQ 运行时依赖
- 一次性把所有 pipe / file 控制面直接改成 ZMQ

### 8.2 `camera_recorder` 的最终目标形态

当前实现文件分布：

- `main.cpp`
- `camera_recorder.cpp`
- `camera_types.cpp`
- `camera_config.cpp`
- `camera_command_builder.cpp`
- `camera_registry.cpp`
- `stereo_control_json.cpp`

这对当前代码量来说过细。

建议目标形态：

- `src/camera_recorder/src/main.cpp`
  - 只保留 CLI 入口和异常返回
- `src/camera_recorder/src/camera_recorder.cpp`
  - 保留 app 壳
  - 保留 V4L2 / UVC / subprocess / stereo runtime / device warmup 等运行时逻辑
- `src/camera_recorder/src/camera_domain.cpp`
  - 合并承载：
    - `camera_types`
    - `camera_config`
    - `camera_command_builder`
    - `camera_registry`
    - `stereo_control_json`

约束说明：

- 默认按“`main.cpp` 不计入职责实现文件数量”理解
- 也就是说，`camera_recorder` 的职责实现文件收成 `2` 个，含入口总数约 `3` 个
- 头文件仍可继续保持细分，不要求同步合并

这样做的理由是：

- 继续保留清晰头文件边界
- 避免把少量纯逻辑拆成 5 个很薄的 `.cpp`
- 让 `CameraRecorder` 后续并仓时更像一个“入口壳 + 领域实现”的简洁 standalone app

测试标准：

- `cmake --build build --target camera_recorder -j$(nproc)`
- `ctest --test-dir build --output-on-failure -L host-only`
- `./build/src/camera_recorder/camera_recorder --help`
- `./build/src/camera_recorder/camera_recorder --output-dir /tmp/ugripper_camera_stageb_ready --config-yaml test/src/camera/config_samples/schema_v1_valid.yaml --dry-run --allow-missing`
- `test/src/camera/test_camera_config.cc`
- `test/src/camera/test_camera_command_builder.cc`
- `test/src/camera/test_stereo_control_json.cc`

完成标志：

- `camera_recorder` 不再依赖 5 个以上小体量职责 `.cpp`
- 头文件边界不丢
- dry-run 与 host-only camera 单测继续通过

当前进度：

- 已完成第一轮物理归并：
  - 删除 `camera_types.cpp`
  - 删除 `camera_config.cpp`
  - 删除 `camera_command_builder.cpp`
  - 删除 `camera_registry.cpp`
  - 删除 `stereo_control_json.cpp`
  - 新增 `camera_domain.cpp`
- `camera_recorder` 与 camera host-only 单测已改为统一链接 `camera_domain`

### 8.3 `record_runtime` 的最终目标形态

当前实现文件分布：

- `main.cpp`
- `record_runtime.cpp`
- `subprocess_handle.cpp`
- `process_supervisor.cpp`
- `audio_coordinator.cpp`
- `stereo_session_client.cpp`
- `hmi_controller.cpp`
- `health_monitor.cpp`
- `recording_orchestrator.cpp`

这同样已经超过当前阶段所需的实现粒度。

建议目标形态：

- `src/record_runtime/src/main.cpp`
  - 只保留 CLI 入口和参数解析结果接线
- `src/record_runtime/src/record_runtime.cpp`
  - 保留 supervisor 入口壳
  - 保留与主链装配、配置读取、顶层 stop/start 生命周期有关的内容
- `src/record_runtime/src/runtime_process.cpp`
  - 合并承载：
    - `subprocess_handle`
    - `process_supervisor`
    - `audio_coordinator`
    - `stereo_session_client`
- `src/record_runtime/src/runtime_domain.cpp`
  - 合并承载：
    - `runtime_types`
    - `hmi_controller`
    - `health_monitor`
    - `recording_orchestrator`

约束说明：

- 同样默认按“`main.cpp` 不计入职责实现文件数量”理解
- 也就是说，`record_runtime` 的职责实现文件收成 `3` 个，含入口总数约 `4` 个
- 若后续实际收口后 `runtime_domain.cpp` 仍过大，再内部重排，但不继续向更多碎片 `.cpp` 扩张

这样做的理由是：

- 保留现有 Step 7/8 已经形成的 process boundary 与 domain boundary
- 避免在代码量还不大的阶段把每个协作者都物理分裂成独立 `.cpp`
- 保持后续并仓时的可读性和 review 成本可控

测试标准：

- `cmake --build build --target record_runtime -j$(nproc)`
- `ctest --test-dir build --output-on-failure -L host-only`
- `./build/src/record_runtime/record_runtime --help`
- `test/src/record_runtime/test_process_policy.cc`
- `test/src/record_runtime/test_audio_coordinator.cc`
- `test/src/record_runtime/test_stereo_session_client.cc`
- `test/src/record_runtime/test_button_logic.cc`
- `test/src/record_runtime/test_runtime_health.cc`
- `test/src/record_runtime/test_recording_orchestrator.cc`

完成标志：

- `record_runtime` 不再依赖 7 个以上小体量职责 `.cpp`
- supervisor 壳、process boundary、domain logic 三层保持可辨认
- 既有 runtime host-only 单测继续全部通过

当前进度：

- 已完成第一轮物理归并：
  - 删除 `subprocess_handle.cpp`
  - 删除 `process_supervisor.cpp`
  - 删除 `audio_coordinator.cpp`
  - 删除 `stereo_session_client.cpp`
  - 删除 `hmi_controller.cpp`
  - 删除 `health_monitor.cpp`
  - 删除 `recording_orchestrator.cpp`
  - 新增 `runtime_process.cpp`
  - 新增 `runtime_domain.cpp`
- `runtime_process_boundary` 与 `runtime_control_plane` 两个库目标保留不变，只调整为单一实现文件

### 8.4 logger 与 `pp_main` 的差距判断

当前 `ugripper` logger：

- 位置：`src/utils/include/utils/logger.h`
- 形态：基于 `iostream + mutex` 的轻量宏
- 优点：
  - 宏名已经对齐 `DM_LOG_*`
  - 无额外依赖
  - 便于当前仓库本地构建

但它和 `pp_main` logger 仍有 5 个关键差距：

1. 缺少 `DM_LOG_INIT(...)`
2. 缺少 `DM_LOG_TRACE(...)`
3. 缺少 `source_location` / `__FILE__` / file logger 能力
4. 缺少 ROS1/ROS2/default backend 切换能力
5. 当前仓库大量依赖 `DM_LOG_*_STREAM()`，而 `pp_main/src/utils/logger.h` 默认并不提供这组宏

结论：

- `pp_main` logger 满足最终统一日志体系的方向要求
- 但以当前 `ugripper` 的调用现状，还不能“直接无缝 drop-in”

保守收口顺序应是：

1. 先冻结规则：
   - 新代码优先少用新增 `DM_LOG_*_STREAM()` 调用
   - 能用普通 `DM_LOG_*` 的地方，不再继续扩大 stream 风格依赖面
2. 再做 focused cleanup：
   - 评估把高频 `_STREAM()` 调用收成普通宏调用
   - 或在并仓适配层提供有限的 stream 兼容封装
3. 最后在主仓库完成真实 logger 后端接管

当前阶段不建议：

- 在 `ugripper` 仓库内完整复刻 `pp_main` logger 后端
- 现在就发起大规模日志文案迁移 PR

测试标准：

- 文档核对：
  - `src/utils/include/utils/logger.h`
  - `/home/songwl/swl_ws/pp_main/src/utils/logger.h`
- `rg -n "DM_LOG_.*_STREAM\\(" src test`
- 受影响目标重新编译通过
- `record_runtime --help`
- `camera_recorder --help`

完成标志：

- 已明确记录 logger 差距，不再假设“直接并入就能过”
- 新增代码默认不继续扩大 `_STREAM()` 依赖面
- 后续若进入 logger 兼容清理，可单独做一类 PR

当前进度：

- 已完成第一轮 logger 兼容收口：
  - 在 `src/utils/include/utils/logger.h` 补齐过渡期 `DM_LOG_INIT(...)` 与 `DM_LOG_TRACE(...)`
  - 新增 `DM_LOG_TRACE_STREAM()`，保持当前仓库内 stream 宏族完整
  - 明确当前 logger 仍是过渡 shim，不在仓内复刻 `pp_main` 的 ROS/file backend
- 已完成第一轮 focused cleanup：
  - `src/camera_recorder/src/camera_domain.cpp`
  - `src/record_runtime/src/main.cpp`
  - `src/record_runtime/src/runtime_process.cpp`
  以上新归并或新建实现文件，默认改用普通 `DM_LOG_*`，不再继续扩大 `_STREAM()` 依赖面
- 旧的高密度 `_STREAM()` 调用仍保留在历史大文件中，后续若继续收口，单独按 logger 兼容 PR 处理

### 8.5 任务调度与线程边界的收口路线

当前代码现状：

- `record_runtime` 与 `camera_recorder` 中已有多处 `poll + sleep_for + atomic stop` 风格循环
- `pp_main` 的 `TaskScheduler` 提供了统一线程池和 timer task 组织方式
- 但其当前实现依赖：
  - `oneTBB`
  - 单例全局生命周期
  - 现阶段仍偏重型

结论：

- `TaskScheduler` 的“统一生命周期和周期任务边界”值得借鉴
- 当前实现不适合直接照搬到 `ugripper`

当前阶段建议做的，是把这些周期逻辑继续收口成局部边界：

- HMI button poll loop
- health monitor poll loop
- audio ready / retry wait loop
- stereo finalize wait loop
- camera 侧设备 warmup / stop timeout wait loop

推荐动作：

1. 继续把周期逻辑留在各自模块，不上升到 `src/utils`
2. 为 deadline、retry、stop 语义形成局部一致的 helper 或 policy
3. 用 host-only 单测锁定“停止、超时、重试”语义
4. 等进入主仓库时，再决定哪些边界值得映射到 `TaskScheduler` 风格接口

测试标准：

- 既有 runtime/camera host-only 单测继续通过
- 若新加局部 loop/policy helper，则补对应单测
- 不引入新的 `oneTBB` 依赖
- 不把 `TaskScheduler` 直接加入 `ugripper` 构建图

完成标志：

- 周期任务边界更一致，但仍保持局部、轻量、可回退
- 当前阶段不引入新的重型调度基础设施

当前进度：

- 已完成第一轮局部收口：
  - 在 `src/record_runtime/src/runtime_process.cpp` 引入局部 `PollUntilReady(...)` 与 `RetryIntervalElapsed(...)`
  - 统一 `SubprocessHandle::Wait()`、`AudioCoordinator::StartAudioPlayer()`、`AudioCoordinator::MaintainAudioPlayer()`、`StereoSessionClient::MaintainDaemon()`、`StereoSessionClient::WaitForFinalize()` 的 deadline/retry/poll 语义
  - 当前 helper 只留在 runtime process boundary，不上升到 `src/utils`
- 已固定进程内线程调度参考口径：
  - `record_runtime` 参考 `pp_main/standalone` 的薄入口、总装对象、显式 Stop 顺序，但保留 supervisor/control 主循环与局部周期逻辑分层
  - `camera_recorder` 参考 `pp_main` 的线程职责清晰化思路，但保留“采集线程 / 写入线程 / 控制逻辑”分层，不把阻塞采集链路 scheduler 化
- 为什么不直接搬 `pp_main` 的 `TaskScheduler`：
  - `pp_main/src/utils/task_scheduler.h` / `.cc` 依赖 `oneTBB`、全局单例和单独 timer thread；这会直接扩大 `ugripper` 当前仓库依赖栈和生命周期复杂度
  - `pp_main/standalone/Puppetry/puppetry.cc`、`pp_main/standalone/Puppeteer/puppeteer.cc` 里的 `TaskScheduler` 主要调度的是进程内周期任务，不负责 `fork/exec` 子进程监督、pipe/file 控制面轮询、阻塞串口/设备 I/O
  - `ugripper` 当前仍是 `record_runtime` supervisor + worker 进程模型，`runtime_process.cpp` 里的等待链路直接绑定 stop escalation、worker exit、audio pipe、stereo finalize；这类逻辑若强塞进全局 timer task，会把阻塞等待和进程边界语义混进调度器
  - `TaskScheduler::CancelTimerTask()` 当前取消语义本身偏弱，且内部有 `sleep_for(100ms)`；直接照搬不能提升 `ugripper` 现有 stop/retry 路径，反而会引入新的退出时序风险
- 当前阶段继续复用的是设计方向：
  - 学习 `pp_main/standalone` 的“薄 main + 业务总装 + 明确 Stop 顺序”
  - 学习“周期任务与通信/阻塞 I/O 分层”
  - 不直接复刻其重型调度实现
- 后续代码推进的线程模型目标：
  - `record_runtime`：主线程 + supervisor/control 主循环 + 局部周期逻辑；不把 `waitpid`、pipe/file 轮询硬塞进统一调度器
  - `camera_recorder`：主线程 + 采集线程 + 写入线程 + 控制类周期逻辑；只让 warmup/stop/restart 这类控制逻辑向 `pp_main` 风格靠拢

### 8.6 通信向 ZMQ 靠拢的渐进路线

当前 `ugripper` 中，值得关注的控制面通道主要有：

- audio command pipe：`/tmp/umi_audio_pipe`
- stereo control file：`/tmp/umi_stereo_camera_control.json`
- stereo status file：`/tmp/umi_stereo_camera_status.json`
- shutdown request file
- runtime 与脚本之间的若干路径 / 文件约定

这些通道里，有两类要区分：

- 进程内部或子进程内部的实现细节：
  - stdout/stderr 管道
  - `fork/exec` 后的进程控制
  - 这类不需要强行迁到 ZMQ
- 适合长期演进为消息通道的控制面：
  - audio control
  - stereo control / status
  - 部分 runtime health / orchestration 状态广播

保守迁移顺序建议是：

1. 先做通道台账
   - 每条通道写清楚 producer / consumer / payload / failure mode
2. 再抽 transport 无关接口
   - 例如 `AudioCommandPort`
   - `StereoSessionPort`
3. 保留 file / pipe backend 作为默认实现
4. 在主仓库或并仓准备后期增加 ZMQ backend
5. 用 shadow / 双实现对照或节点级 smoke 完成切换

当前阶段不做：

- 全量替换 file / pipe backend
- 在 `ugripper` 仓库里直接引入完整 ZMQ runtime

测试标准：

- 通道台账文档核对
- transport 无关接口的 host-only fake/stub 测试
- 现有 file / pipe backend 行为继续通过本地 smoke
- 真正的 ZMQ backend 测试后移到主仓库 / Docker / ARM 阶段

完成标志：

- 已明确哪些通道值得迁往 ZMQ，哪些不需要
- 已具备“先接口、后传输实现”的收口基础

当前进度：

- 已完成第一轮通道台账冻结：
  - 新增 `docs/archive/2026-refactor-history/baseline/control-channel-ledger.md`
  - 明确冻结以下控制面通道：
    - `/tmp/umi_audio_pipe`
    - `/tmp/umi_audio_ready`
    - `/tmp/umi_stereo_camera_control.json`
    - `/tmp/umi_stereo_camera_status.json`
    - `/tmp/umi_shutdown_request`
- 已完成首批接口边界冻结：
  - `AudioCommandPort`
  - `StereoSessionPort`
  - `ShutdownRequestPort`
- 已完成第一条代码级抽象：
- 新增 `src/record_runtime/include/record_runtime/audio_command_port.h`
- `AudioCoordinator` 已改为依赖 `AudioCommandPort`
- 默认 file backend 仍使用 `/tmp/umi_audio_pipe` + `/tmp/umi_audio_ready`
- 新增注入式 host-only 单测，锁定“业务层依赖接口而不是直接依赖 FIFO/ready 文件细节”
- 已完成第二条代码级抽象：
  - 新增 `src/record_runtime/include/record_runtime/stereo_session_port.h`
  - `StereoSessionClient` 已改为依赖 `StereoSessionPort`
  - 默认 file backend 仍使用 `/tmp/umi_stereo_camera_control.json` + `/tmp/umi_stereo_camera_status.json`
  - 新增注入式 host-only 单测，锁定“session 业务层依赖接口而不是直接依赖 control/status 文件和 JSON 细节”
- 已完成第三条代码级抽象：
  - 新增 `src/record_runtime/include/record_runtime/shutdown_request_port.h`
  - `RecordRuntime::handleDualShutdownAction()` 已改为通过 `ShutdownRequestPort` 发出关机请求
  - 默认 file backend 仍使用 `/tmp/umi_shutdown_request`，继续兼容 `umi-shutdown-trigger.path` + `trigger_shutdown.sh`
  - 新增 host-only 单测，锁定当前 file backend 的请求文件写入语义
- 已明确下列对象不纳入首批 ZMQ 迁移：
  - worker stdout/stderr 管道
  - `waitpid` / 进程组 signal
  - runtime 日志同步 `.pos/.lock`
  - `/tmp/umi_recording.lock`
- 当前阶段的收口状态是：
  - 已完成 ledger 和 future port boundary 冻结
  - 已完成 `AudioCommandPort` / `StereoSessionPort` / `ShutdownRequestPort` 三条代码级抽象
  - 当前 `8.6` 的第一轮 transport-agnostic 接口收口已完成

### 8.7 建议执行顺序

在真正进入 `Stage B` 之前，新增收口项按下面顺序推进更稳：

1. `camera_recorder` 实现文件归并
2. `record_runtime` 实现文件归并
3. logger 兼容差距清单与 focused cleanup 方案冻结
4. 周期任务 / 停止语义局部收口
5. 通信通道台账与 transport 无关接口冻结

### 8.7.1 当前进度判断

当前可以认为：

- “基础重构”已经完成
- `8.2 camera_recorder` 实现文件归并已完成
- `8.3 record_runtime` 实现文件归并已完成
- `8.4 logger` 第一轮兼容收口已完成
- `8.5 调度边界` 第一轮局部收口已完成
- `8.6 通信台账` 第一轮冻结已完成

也就是说，当前状态适合继续做仓内保守收口，不适合直接跳到主仓库并仓和 ARM 集成验证。

### 8.8 与测试门禁的关系

这 5 项仍然属于 `ugripper` 仓库内的 `重构门禁`，不是 `集成门禁` 或 `交付门禁`。

因此默认测试只做到：

- 本地 Linux 编译
- host-only 单测
- `--help` / `--dry-run`
- shell / Python 语法检查
- 无硬件最小 smoke

以下验证继续后移：

- 主仓库本地编译
- Docker ARM 交叉编译
- ARM 板 smoke
