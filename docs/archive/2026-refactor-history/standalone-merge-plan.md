# Ugripper Refactor And Standalone Integration Plan

本文档定义 `ugripper` 的两阶段执行方案：

1. `ugripper internal refactor`
2. `standalone integration`

核心原则：

- 先把 `ugripper` 在本仓库内重构到“可合并”
- 再把已经收口后的主链逐步并入 `/home/songwl/swl_ws/pp_main/standalone`

这份文档不再把“最终并仓形态”当成前期执行压力。
前半段只解决 `ugripper` 自身的边界、日志、helper、路径和脚本治理问题。

参照基线：

- `standalone` 参照：`/home/songwl/swl_ws/pp_main/standalone`
- `utils` 参照：`/home/songwl/swl_ws/pp_main/src/utils`

## 1. 为什么要拆成两阶段

当前如果直接按“并入 standalone”推进，会过早引入以下高风险改动：

- 目录迁移
- 顶层 CMake 接入
- 安装路径切换
- service 入口切换
- 打包清单切换
- 资源路径切换

这些成本不属于 `ugripper` 当前最急需解决的问题。

`ugripper` 现在最需要先解决的是：

- 主链入口是否清楚
- 哪些路径影响线上
- 日志是否统一
- helper 是否重复散落
- Shell / Python 是否可控
- 硬编码路径是否可收口

所以执行顺序必须是：

1. 先在 `ugripper` 仓库内完成“可合并重构”
2. 再开始并入 `pp_main/standalone`

## 2. 最终目标

最终目标仍然成立，但不是前期强制执行对象。

最终形态应满足：

- `ugripper` 的业务入口以 standalone app 方式落入 `standalone/<AppName>/`
- 公共能力收口到 `src/utils/`
- camera 相关职责有清晰抽象边界，不再长期堆在单个巨大实现中
- C++ 主链统一接入 `logger.h` / `DM_LOG_*`
- 关键 helper 不再散落在 shell、大型 C++ 文件和 Python 脚本中
- 路径访问不再依赖脆弱的相对路径
- 可测试的纯逻辑有单元测试或等价无硬件测试覆盖
- 打包、安装、service 以新路径工作

## 3. 当前代码现状

当前主链仍然是“模块基本收口，但规范和公共能力未统一”的状态。

### 3.1 当前主链入口

- `run_record.sh`
- `build/src/record_runtime/record_runtime`
- `build/src/camera_recorder/camera_recorder`
- `build/src/sensor_recorder/sensor_recorder`

### 3.2 当前主链模块

- `src/record_runtime`
- `src/camera_recorder`
- `src/sensor_recorder`
- `src/gripper_hmi`
- `auto_update/`
- `auto_calibration/`
- `config/`
- `audio/`
- `audio_en/`

### 3.3 当前主要问题

#### 日志体系不统一

当前并存：

- Shell：`echo "[INFO] ..."`
- C++：`std::cout` / `std::cerr`
- 模块自定义前缀：`[camera_recorder] ...`
- Python：`print(f"[INFO] ...")`

#### helper 散落且重复

当前重复能力包括：

- Shell
  - `trim_text`
  - `read_env_value`
  - `normalize_language`
  - `normalize_camera_codec`
- C++
  - `readEnvValue`
  - `currentSteadyMs`
  - `currentEpochMs`
  - `fileExistsAndNotEmpty`
  - `runCommandSync`
  - `joinArguments`
  - `joinStrings`
- Python
  - 日志输出
  - 路径定位
  - PulseAudio 环境探测

#### camera 处理职责混杂

当前 `src/camera_recorder` 同时混有：

- camera 配置解析
- mode 选择
- UVC / V4L2 设备细节
- ffmpeg / gstreamer 命令拼装
- stereo session 业务协调

这会让 `camera_recorder.cpp` 继续变大，也会提高后续改 camera 行为时的回归风险。

#### 自动化测试不足

当前纯逻辑和可拆分逻辑的测试沉淀不足：

- 低风险 helper 缺少单元测试
- camera 模式映射和命令拼装缺少无硬件测试
- 路径 helper 和配置解析缺少稳定回归点

#### 硬编码路径较多

主要包括：

- `./build/src/...`
- `./audio/...`
- `./config/...`

#### 高风险逻辑仍集中在脚本

最需要优先治理的不是叶子 C++ 模块，而是：

- `run_record.sh`
- `auto_update/usb_auto_update.sh`
- `auto_calibration/run_calibration.sh`
- `audio/*.py`

## 4. 命名、日志、依赖、utils、camera、测试 的目标规则

这些规则在第一阶段就应生效，但不要求第一阶段就完成并仓。

## 4.0 C++ 标准规则

由于目标主仓库 `pp_main` 默认使用 `C++20`，`ugripper` 在重构阶段和并仓阶段均默认对齐 `C++20`。

执行规则：

- 顶层和子模块的 C++ 标准默认维持 `C++20`
- 新增 C++ 代码、camera 抽象、utils、测试代码默认按 `C++20` 编写
- 不再新增 `C++17` 专用模块或新的标准分裂
- 若后续出现交叉编译工具链、第三方依赖或板端环境对 `C++20` 的限制，需作为显式例外记录到基线或重构日志

这条规则属于默认约束，而不是后置优化项

## 4.1 命名规则

### standalone app 目录命名

对齐 `pp_main/standalone` 风格：

- 目录名使用 `UpperCamelCase`
- 表达应用入口，而不是表达零碎功能

示例：

- `UgripperRuntime`
- `CameraRecorder`
- `SensorRecorder`
- `GripperHmiTool`

### C++ 文件命名

- 迁移到 standalone 后优先统一为 `.cc/.h`
- 文件名使用 `lower_snake_case`
- app 入口使用 `main.cc`

### C++ 标识符命名

对齐现有 `pp_main/src/utils` 风格，避免引入第三套命名体系：

- 类型：`UpperCamelCase`
- 常量：`kCamelCase`
- utils 接口允许保持 `Load/Get/Reset/Shutdown` 风格
- 不强行为了“风格纯洁”重命名现有可复用接口

### Shell / Python 命名

- 函数名：`lower_snake_case`
- 环境变量：`UPPER_SNAKE_CASE`
- CLI 参数：`--kebab-case`

`camera_recorder` 的具体 naming/YAML 兼容策略见：

- `docs/archive/2026-refactor-history/naming-yaml-contract.md`

## 4.2 日志规则

### C++ 最终规则

最终统一接入 `logger.h`：

- `DM_LOG_DEBUG(...)`
- `DM_LOG_INFO(...)`
- `DM_LOG_WARN(...)`
- `DM_LOG_ERROR(...)`
- `DM_LOG_CRITICAL(...)`

禁止长期保留：

- `std::cout << "[INFO] ..."`
- `std::cerr << "[ERROR] ..."`

### 与 `pp_main` logger 的对齐方式

最终并入主仓库后，日志能力应以 `/home/songwl/swl_ws/pp_main/src/utils/logger.h` 为准。

第一阶段的落地口径是：

- 优先对齐 `DM_LOG_INIT`、`DM_LOG_*`、`DM_LOG_*_STREAM()` 这类 API 形态
- 优先保证调用点、宏语义和接线方式可迁移
- 不在 `ugripper` 阶段直接搬入完整 ROS1/ROS2、`spdlog`、file logger 依赖栈

也就是说：

- 当前 `src/utils/include/utils/logger.h` 属于过渡实现
- 第一阶段追求的是“API 兼容收口”
- 真正的日志后端统一，在并入 `pp_main` 后完成

### Shell / Python 过渡规则

第一阶段不强制接入 C++ logger 实现，但要统一格式：

```text
[INFO] [module] ...
[WARN] [module] ...
[ERROR] [module] ...
```

### 日志改造顺序

1. `src/record_runtime`
2. `src/camera_recorder`
3. `src/sensor_recorder`
4. `src/gripper_hmi`
5. `run_record.sh`
6. `auto_update/usb_auto_update.sh`
7. `auto_calibration/run_calibration.sh`
8. `audio/audio_play.py`
9. `audio/record_usb_audio.py`

## 4.3 依赖规则

### 第一阶段

先做依赖显式化，不切主仓库路径：

- 明确模块依赖谁
- 明确运行时依赖哪些配置、资源和工具
- 明确打包依赖哪些文件

### 第二阶段

再做：

- standalone 顶层 CMake 接入
- 安装路径切换
- service 路径切换
- 打包路径切换

## 4.4 utils 规则

`src/utils` 的目标不是照抄 `pp_main` 全量能力，而是先补齐 `ugripper` 当前最需要的最小公共层。

### 第一批只收最小 utils

只优先收这些低风险能力：

- `logger`
- `file utils`
- `env/config` 小工具
- 必要时的 `single process helper`

第一阶段不建议上重型公共层：

- 完整 `yaml_parser`
- 完整 `config_manager`
- 完整 `task_scheduler`

原因：

- 这些一旦做重，就会从“收口工具”变成“替换基础设施”
- 会大幅增加 review 成本和行为变化风险

### 哪些函数适合先收进 utils

- `trim_text`
- `read_env_value`
- `currentSteadyMs`
- `currentEpochMs`
- `fileExistsAndNotEmpty`
- 路径拼接 / 文件存在判断
- 统一日志封装

### 哪些函数先不要急着抽

- `runCommandSync`
- 复杂 YAML/配置管理体系
- 调度器 / 定时任务体系
- 带明显业务语义的 helper

原因：

- 这类 helper 往往自带外部行为或隐式副作用
- 过早抽成“通用接口”，容易引入新的抽象债务

### 多线程与调度能力约束

`ugripper` 的多线程和周期任务管理，需要参考 `pp_main/standalone` 与 `/home/songwl/swl_ws/pp_main/src/utils/task_scheduler.*` 的设计目标，但当前阶段不直接照搬实现。

第一阶段的规则是：

- 借鉴生命周期组织、周期任务边界和停止语义
- 不直接把 `pp_main` 当前 `TaskScheduler` 原样搬入 `ugripper`
- 不提前引入 `oneTBB`、全局单例调度器或新的重型基础设施依赖
- 先在 `record_runtime` 内形成本地可测试的周期任务 / 控制面边界，再决定后续如何向主仓库能力对齐

进程内线程调度的参考口径：

- 参考 `pp_main/standalone` 的“薄 main + 总装对象 + 明确 Init/Start/Stop + 显式 Stop 顺序”
- 参考 `TaskScheduler` 所体现的“周期任务集中治理”目标，而不是直接复刻其 `oneTBB + 单例 + timer thread` 实现
- 区分两类线程：
  - 周期任务：health check、状态刷新、重试窗口判断、超时检查
  - 阻塞 I/O：设备 `poll()`、串口读写、`waitpid`、pipe/file 控制面等待、ffmpeg/stdin 链路
- 周期任务可以逐步收口为局部 loop / policy；阻塞 I/O 线程仍由各自对象管理，不强行 scheduler 化

为什么不能直接复用 `pp_main` 当前 `TaskScheduler`：

- `/home/songwl/swl_ws/pp_main/src/utils/task_scheduler.h`、`task_scheduler.cc` 依赖 `oneTBB`、全局单例和专门 timer thread，超出当前仓内保守重构所需
- `pp_main/standalone` 中它主要承载进程内周期 `Run/Publish/Check` 任务，不负责 `ugripper` 当前 supervisor 子进程监督、pipe/file 控制面轮询和阻塞设备 I/O
- 当前 `ugripper` 仍保留 `record_runtime` supervisor + worker 进程模型，若直接照搬，会把“进程监督问题”和“进程内周期调度问题”混层
- `TaskScheduler::CancelTimerTask()` 当前取消语义偏弱，内部还有 `sleep_for(100ms)`；直接复用不能改善现有 stop/retry 路径，反而可能引入新的退出时序风险

当前阶段推荐的目标线程模型：

- `record_runtime`
  - 主线程：CLI、信号处理、顶层 `Init/Run/Stop`
  - supervisor/control 主循环：worker 状态刷新、HMI 输入消费、录制状态推进
  - 局部周期逻辑：health check、audio ready/retry、stereo finalize wait
  - 原则：参考 `pp_main` 的生命周期组织方式，但不把 `waitpid`、pipe/file 轮询硬塞进统一调度器
- `camera_recorder`
  - 主线程：参数解析、配置加载、recorder/session 装配、stop 信号传播
  - 采集线程：设备 `poll()`、取帧、设备错误检测
  - 写入线程：frame queue 消费、ffmpeg/stdin 或 writer flush
  - 控制/状态逻辑：warmup deadline、stop timeout、session restart/backoff
  - 原则：只让控制类周期逻辑向 `pp_main` 风格靠拢，不改变阻塞采集线程模型

### 通信与控制面通道约束

控制面通道的第一轮冻结以：

- `docs/archive/2026-refactor-history/baseline/control-channel-ledger.md`

为准。

当前已冻结的首批控制面通道：

- `/tmp/umi_audio_pipe`
- `/tmp/umi_audio_ready`
- `/tmp/umi_stereo_camera_control.json`
- `/tmp/umi_stereo_camera_status.json`
- `/tmp/umi_shutdown_request`

当前已冻结的未来 transport 无关接口边界：

- `AudioCommandPort`
- `StereoSessionPort`
- `ShutdownRequestPort`

明确不纳入首批 ZMQ 迁移的对象：

- worker stdout/stderr 管道
- `waitpid` / 进程组 signal
- runtime 日志同步 `.pos/.lock`
- `/tmp/umi_recording.lock`

这条规则的目的是：

- 先把真正的业务控制面和本地实现细节分开
- 避免后续在“通信重构”名义下把进程控制、日志同步、安装窗口清理逻辑一起混进来

这一条的目标是：

- 先把 `ugripper` 的线程职责和停止语义收清
- 避免在当前阶段把“参考设计”误做成“直接替换底座”

## 4.5 Camera 抽象规则

`ugripper` 需要新增 camera 抽象层，但只参考 `pp_main/src/device/cameras` 的设计目标，不照搬其实现。

参考对象：

- `/home/songwl/swl_ws/pp_main/src/device/cameras/README.md`
- `/home/songwl/swl_ws/pp_main/src/device/cameras/camera_base.h`
- `/home/songwl/swl_ws/pp_main/src/device/cameras/camera_types.h`

应借鉴的点：

- camera 类型、配置、错误语义单独表达
- 抽象接口与具体实现分离
- 业务编排层不直接堆设备细节

不应照搬的点：

- `pp_main` 的抽象层级更靠近通用设备访问
- `ugripper` 当前更偏录制编排、配置解析、命令组装和会话管理
- 若完全照搬，容易把不需要的设备层能力一起带进来

第一阶段的目标是：

- 让 `camera_recorder` 的职责边界清楚
- 让 camera 相关改动不再集中到一个巨型实现
- 为后续并仓前的可维护性打底

### 实现文件粒度约束

允许为 camera 和 runtime 拆出更多头文件，但实现文件不要无限细分。

当前阶段和并仓准备阶段都遵守：

- `camera_recorder` 最终以 `2-3` 个 `.cpp` 为宜
- `record_runtime` 最终以 `2-3` 个 `.cpp` 为宜
- 可以按职责拆分，但要避免因为代码量有限而形成过度笨重的碎片化实现文件布局

推荐口径是：

- 头文件可以继续细分为类型、配置、命令、注册表、控制面等边界
- `.cpp` 侧优先按“app 壳 / process or runtime boundary / domain logic”归并
- 如果某个纯逻辑模块体量很小，优先并入相邻职责实现文件，而不是再新增一个独立 `.cpp`

## 4.5.1 Runtime 边界与通信演进规则

并入 standalone 后，`record_runtime` 仍然是 supervisor 形态的主控，负责拉起、协调、停止多个独立功能块；这条边界在当前阶段不改变。

后续通信能力的目标方向，是逐步向 `pp_main` 的 ZMQ / message-hub 风格靠拢，而不是长期停留在分散的 pipe / file 控制面上。

但当前阶段只做保守收口：

- 先识别哪些控制面通道将来应升级为消息通道
- 先把 transport 无关的接口和通道职责抽出来
- 现阶段继续保留现有 pipe / file 实现作为默认后端
- ZMQ 后端作为后续独立子阶段渐进引入，不与当前重构步骤或当前 package 主线混做

这一步不做：

- 一次性替换全部 pipe / file 通信
- 在 `ugripper` 阶段直接引入完整 ZMQ 运行时依赖并切主链
- 在 `B4` 打包 / 安装 / service 主线上同步引入 ZMQ backend 迁移

## 4.6 测试与可维护性规则

第一阶段开始就把“可读性、可维护性、可测试性”作为验收条件，而不是附加项。

默认要求：

- 新增的纯逻辑、低风险 helper、路径 helper 应补单元测试或等价无硬件测试
- camera 抽象相关的配置解析、模式映射、命令拼装应优先有单元测试
- 每次重构至少显式改善一项：职责边界、函数长度、命名一致性、错误路径可读性、日志可读性

不接受的结果：

- 只是把代码搬位置，但理解成本没有下降
- 抽出了新类，但仍然把多种职责塞进一个新巨类
- 继续让可测试纯逻辑长期没有自动化验证

## 5. 第一阶段：Ugripper Internal Refactor

这一阶段只在 `ugripper` 仓库内完成，不碰主仓库 standalone 目录落位。

### A1. 基线冻结

#### 内容

先明确以下内容：

- 当前主链入口
- 当前打包入口
- 当前 service 入口
- 当前关键资源路径
- 当前配置入口
- 当前需要保行为等价的链路

#### 输出物

- 主链入口清单
- 运行时路径清单
- 打包清单
- 风险清单

A1 的输出必须落为可维护文件，建议固定为：

- `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`
- `docs/archive/2026-refactor-history/baseline/service-map.md`
- `docs/archive/2026-refactor-history/baseline/config-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/equivalence-checklist.md`

其中 `equivalence-checklist` 至少应覆盖：

- 正常启动
- 正常开始录制
- 正常停止录制
- 音频播放正常
- 相机录制正常
- 传感器录制正常
- 升级脚本能执行
- 校准脚本能执行

baseline 文档里的每个条目都必须注明事实来源，至少记录以下一种：

- 代码路径
- 脚本路径
- service 文件
- 打包脚本
- 实际运行命令

建议在条目中显式增加 `source` 字段，例如：

- `source: run_record.sh`
- `source: pack_script/ugripper.service`
- `source: build_deb.sh`
- `source: src/record_runtime/src/record_runtime.cpp`

#### 完成标准

- 可以明确回答“改哪些会影响线上”
- 可以明确回答“哪些路径是运行时关键路径”
- 行为等价检查不再停留在口头层面，而是有文档可执行清单

#### 测试策略

测试目标：

- 冻结当前事实
- 建立后续所有重构步骤的对照基线

测试环境：

- 本地 Linux

验证动作：

- 逐条核对 baseline 文档中的入口、路径、service、配置和打包信息
- 为每个条目补齐 `source`
- 实际运行本地最小可运行基线中的命令
- 检查关键脚本的 `bash -n`
- 检查关键 Python 文件的 `python3 -m py_compile`

这一阶段不做：

- Docker ARM 构建
- ARM 板 smoke
- 主仓库路径验证

### A2. 建最小 `src/utils`

#### 内容

只建立最小公共层，不做完整 `pp_main utils` 体系复刻。

建议第一批文件：

- `src/utils/CMakeLists.txt`
- `src/utils/logger.h`
- `src/utils/file_utils.hpp`
- `src/utils/env_utils.h/.cc` 或等价轻量实现
- 如确有需要，再补 `single_process_instance`

#### 不在这一阶段做

- 重型 `yaml_parser`
- 重型 `config_manager`
- 重型 `task_scheduler`

第一阶段的 `utils` 只允许引入“被动工具”，不允许引入“主动流程工具”或复杂生命周期对象。

允许的能力包括：

- 字符串处理
- 路径处理
- 文件判断
- 环境变量读取
- 时间函数
- 轻量 logger 封装

不允许的能力包括：

- 自动初始化器
- 全局配置管理器
- 复杂生命周期对象
- 任务编排器

依赖方向必须写死：

- `src/utils` 可以被业务模块依赖
- `src/utils` 不允许反向依赖 `record_runtime`、`camera_recorder`、`sensor_recorder`、`gripper_hmi` 等业务模块

#### 完成标准

- `src/utils` 可以独立构建
- 至少一个业务模块开始依赖 `utils`
- 没有引入业务语义变化

#### 测试策略

测试目标：

- 验证最小 `utils` 能被引入且不会反向污染业务模块

测试环境：

- 本地 Linux

验证动作：

- 编译 `src/utils`
- 编译第一个接入 `utils` 的业务目标
- 对新引入的轻量 helper 做最小输入输出验证
- 检查 `src/utils` 是否误依赖业务模块

这一阶段不做：

- Docker ARM 构建
- ARM 板 smoke
- 大范围主链回归

### A3. 统一 C++ 日志

#### 内容

按统一日志入口改造：

- `src/record_runtime`
- `src/camera_recorder`
- `src/sensor_recorder`
- `src/gripper_hmi`

要求：

- 只改日志接入
- 不顺手改 helper 归属
- 不顺手改状态机逻辑
- 只允许改 include、宏调用和消息格式
- 不新增新的关键日志点
- 不删除已有对排障有价值的日志语义

明确约束：

- 日志替换、helper 抽取、行为修复不得混在同一 PR 中

#### 完成标准

- 新增 C++ 主链日志统一走 `DM_LOG_*`
- 主链关键模块不再大量直接写裸 `std::cout/std::cerr`
- review 可以清楚聚焦“日志替换”，而不是混杂逻辑迁移

#### 测试策略

测试目标：

- 验证日志入口替换完成
- 验证关键日志语义未丢失

测试环境：

- 本地 Linux

验证动作：

- 编译被修改的 C++ 目标
- 运行相关二进制的 `--help`、最小启动命令或轻量 smoke
- 检查关键日志仍然存在
- 检查没有新增裸 `std::cout/std::cerr` 作为主链日志入口

建议覆盖：

- `record_runtime`
- `camera_recorder`
- `sensor_recorder`
- `gripper_hmi` 相关工具

这一阶段不做：

- helper 迁移验证
- 路径切换验证
- Docker ARM 构建
- ARM 板 smoke

### A4. 收低风险 helper

#### 内容

这一阶段只迁移低风险、纯工具型 helper。

优先迁移：

- `trim_text`
- `read_env_value`
- `currentSteadyMs`
- `currentEpochMs`
- `fileExistsAndNotEmpty`

暂缓迁移：

- `runCommandSync`
- 带外部依赖的命令执行工具
- 复杂配置管理能力

明确约束：

- helper PR 只允许改函数归属和调用点
- 不得与日志替换、行为修复混在同一 PR 中

#### 完成标准

- 至少两个主链文件中的重复 helper 被收口
- 低风险 helper 已进入 `src/utils`
- 行为保持等价

#### 测试策略

测试目标：

- 验证 helper 收口后，调用点行为不变

测试环境：

- 本地 Linux

验证动作：

- 编译受影响模块
- 对替换 helper 的模块做最小 smoke
- 对文本处理、环境变量读取、时间函数、文件判断做定向检查
- 确认未引入新副作用

这一阶段不做：

- Docker ARM 构建
- ARM 板 smoke
- 带副作用 helper 的统一抽象

### A5. 建立 camera 抽象层

#### 内容

`src/camera_recorder` 的 camera 相关逻辑需要单独收口为更清晰的抽象边界。

参考方式：

- 参考 `pp_main/src/device/cameras` 的设计目标
- 不照搬其目录结构、接口颗粒度和实现细节

建议优先拆开的职责：

- camera 配置 / 类型定义
- camera 模式选择与参数校验
- UVC / V4L2 / 设备细节
- 录制命令或 pipeline 拼装
- stereo session 业务协调

目标不是建立“万能 camera 框架”，而是让 `ugripper` 当前 camera 代码更可维护。

#### 完成标准

- `camera_recorder` 不再长期依赖单个巨型实现承载全部 camera 逻辑
- 新增或修改 camera 类型时，影响范围更可控
- camera 相关纯逻辑已开始进入可测试边界
- `camera_recorder` 的实现文件粒度保持克制，优先收成 `2-3` 个 `.cpp` 级别的职责块

#### 测试策略

测试目标：

- 验证 camera 抽象收口后，行为未变且可测试性提高

测试环境：

- 本地 Linux

验证动作：

- 编译 `camera_recorder` 相关目标
- 运行 `camera_recorder --help`、`--dry-run` 或等价最小命令
- 为拆出的纯逻辑补单元测试，优先覆盖：
  - camera 配置解析
  - mode 到实现策略的映射
  - 命令拼装 / 参数生成
  - 错误分支与边界条件
- 如引入 backend interface，优先使用 fake / stub 做无硬件测试

这一阶段不做：

- Docker ARM 构建
- ARM 板真实 camera 验证
- 为了抽象统一而提前改主仓库目录落位

### A6. 清理硬编码路径

#### 内容

在替换代码前，必须先产出运行时路径台账。

A6 必须拆成两个子步骤：

- `A6-1`：先做路径清单归一
- `A6-2`：再做路径封装和代码替换

把下列硬编码路径收口成统一访问方式：

- `./build/src/...`
- `./audio/...`
- `./config/...`

这一步的目标不是切新路径，而是先消灭“散落硬编码”。

A6 的目标是消灭散落硬编码，不在第一阶段引入新的安装前缀或主仓库绝对路径。

路径清单至少要分成：

- 二进制路径
- 音频资源路径
- 配置路径
- 临时文件路径
- 日志路径
- 校准资产路径

建议方式：

- 引入路径 helper
- 用统一根路径对象或函数封装
- 把业务逻辑与具体物理路径解耦

#### 完成标准

- 核心模块不再到处直接写旧相对路径
- 后续切路径时只需改少数集中点

#### 测试策略

测试目标：

- 验证本地逻辑上的路径解析已统一
- 为后续安装路径切换做准备

测试环境：

- 本地 Linux

验证动作：

- 对照路径台账检查关键路径是否都已归类
- 验证本地最小命令仍能找到资源、配置和二进制
- 检查路径 helper 是否覆盖原有硬编码点

说明：

- 在 `ugripper` 阶段，这一步只验证“逻辑上的路径解析是否统一”
- “安装后的真实路径是否正确”后移到主仓库 / ARM 阶段

这一阶段不做：

- 主仓库安装前缀验证
- Docker ARM 构建
- ARM 板路径验证

### A7. 拆高风险脚本

#### 内容

优先拆：

- `run_record.sh`
- `auto_update/usb_auto_update.sh`
- `auto_calibration/run_calibration.sh`

目标是先把它们变成：

- 编排层
- helper 层
- 配置读取层

而不是继续让主逻辑堆在单个脚本里。

此外，`audio/*.py` 在第一阶段至少要完成两类收口：

- 日志格式收口
- 路径访问收口

建议顺序：

- `A7.1` `run_record.sh`
- `A7.2` `usb_auto_update.sh`
- `A7.3` `run_calibration.sh`
- `A7.4` `audio/*.py` 的日志和路径收口

#### 完成标准

- 重复逻辑明显下降
- 主脚本职责更偏编排
- 行为等价 smoke 可通过

#### 测试策略

测试目标：

- 验证脚本拆层后仍保持基本行为
- 优先证明“没写炸”，再证明“本地可控”

测试环境：

- 本地 Linux

验证动作：

- Shell 语法检查：`bash -n`
- Python 语法检查：`python3 -m py_compile`
- 对支持 `--help`、`--dry-run` 的脚本跑最小命令
- 对不能真实运行的脚本至少检查路径、日志和关键分支
- 对主链外壳脚本做最小 smoke

分项建议：

- `A7.1 run_record.sh`
  - `bash -n`
  - 检查关键日志与路径解析
  - 如环境允许，做最小启动 smoke
- `A7.2 usb_auto_update.sh`
  - `bash -n`
  - 检查配置解析与日志分支
  - 对可 mock / dry-run 分支做最小验证
- `A7.3 run_calibration.sh`
  - `bash -n`
  - 检查路径、日志、调用链完整性
- `A7.4 audio/*.py`
  - `python3 -m py_compile`
  - 检查日志前缀和路径访问
  - 对非硬件依赖分支做最小运行

这一阶段不做：

- Docker ARM 构建
- 板端真实音频验证
- 真实升级流程验证
- 真实校准流程验证

### 第一阶段总体验收标准

- 主链入口、路径、资源、配置边界已清楚
- 最小 `src/utils` 已建立
- C++ 主链日志基本统一
- 低风险 helper 已收口
- camera 抽象边界已开始建立
- 高风险脚本开始模块化
- 可测试纯逻辑已开始补单元测试或等价无硬件测试
- 旧入口继续工作

#### 阶段提醒

当第一阶段任一子步骤完成时，应明确提醒：

- 当前完成的是哪一步
- 已执行了哪些本地验证
- 哪些验证后移到主仓库 / Docker / ARM 阶段
- 是否可以进入下一步

## 6. 第二阶段：Standalone Integration

从这一节开始，才真正进入并仓阶段。

`integration starts here`

### 6.0 进入第二阶段的前提

只有满足下面条件，才进入 `Stage B`：

- `A1-A3` 已完成
- `Post-A3 Step 1-9` 已完成
- `8.2-8.6` 并仓前收口已完成
- `docs/REFACTOR_LOG.md`、`docs/ugripper-refactor-architecture.md`、`docs/reference-aligned-refactor-next-steps.md` 已同步当前状态
- 当前仓内新增需求如果不直接阻塞并仓，不再继续扩展新的重构主题

当前建议视为已满足的前提：

- `CameraRecorder` / `SensorRecorder` / `record_runtime` 的边界已经收口到可迁移状态
- `AudioCommandPort` / `StereoSessionPort` / `ShutdownRequestPort` 已完成第一轮代码级抽象
- host-only 单测入口已具备，且能覆盖 camera / runtime 的主要纯逻辑和控制面

进入第二阶段后，默认目标从：

- `重构门禁`

转为：

- `集成门禁`
- `交付门禁`

### 6.0.1 第二阶段统一门禁

第二阶段每个节点都固定按下面顺序推进：

1. 主仓库侧信息补齐或代码迁入
2. 主仓库本地 Linux 编译
3. 主仓库 Docker ARM 交叉编译
4. 视风险等级决定是否上 ARM 板

硬规则：

- 主仓库本地编译不过，不进入 Docker
- Docker ARM 不通过，不进入 ARM 板
- 本节点关键测试未通过，不进入下一个节点
- 若关键测试未执行，必须在 `docs/REFACTOR_LOG.md` 或对应并仓记录中写明原因

ARM smoke 分级继续沿用：

- `L1` 轻量 smoke：程序能起、日志正常、无明显报错
- `L2` 主链 smoke：主链能起、资源路径正确、关键流程基本可跑
- `L3` 交付 smoke：包可装、service 可起、更新/校准/安装链路基本可跑

### B1. 并仓准备

#### 内容

在开始迁入 standalone 前，先补齐这些映射关系：

- 每个模块的构建依赖
- 每个模块的运行时资源
- 安装路径映射
- service 对应关系
- 哪些脚本属于 app，哪些属于 deploy

#### 完成标准

- 每个待迁模块都能回答“迁入 standalone 后依赖什么、安装什么、由谁启动”

#### 测试策略

测试目标：

- 验证并仓输入信息完整
- 不在这个步骤提前做真实并仓

测试环境：

- 主仓库本地 Linux

验证动作：

- 核对每个待迁模块的构建依赖
- 核对运行时资源和安装映射
- 核对 service 与脚本归属
- 必要时做主仓库侧静态路径检查

这一阶段不做：

- Docker ARM 构建
- ARM 板 smoke
- 真正切主路径

#### 细化执行清单

建议输出一份主仓库侧并仓输入表，至少覆盖 4 个对象：

1. `CameraRecorder`
2. `SensorRecorder`
3. `GripperHmiTool`
4. `UgripperRuntime`

每个对象至少补齐以下字段：

- 目标 standalone 目录名
- 目标 CMake target 名
- 直接源码来源
- 依赖的 `src/utils` / 第三方库 / 资源文件
- 安装后需要保留的配置、脚本、模板、音频、service
- 启动方式：CLI / service / 被谁拉起
- 哪些仍保留在 deploy/script 区域，不进入 app 目录

这一节点的产出不是代码迁移，而是“并仓输入已经稳定，不会边迁边猜”。

建议固定输出文件：

- `docs/archive/2026-refactor-history/baseline/standalone-integration-inputs.md`

### B2. 先迁叶子模块

推荐顺序：

1. `CameraRecorder`
2. `SensorRecorder`
3. `GripperHmiTool`
4. `UgripperRuntime`

原因：

- `record_runtime` 最后迁，路径和依赖改动更可控
- 叶子模块先迁，能减少反复调整

#### 完成标准

- 至少一个叶子模块已进入 `standalone/<AppName>/`
- 有独立 `CMakeLists.txt`
- 可以单独 build / install

#### 测试策略

测试目标：

- 验证叶子模块可进入主仓库 standalone 构建体系

测试环境：

- 主仓库本地 Linux
- 主仓库 Docker ARM 构建环境
- ARM 板（按风险等级决定）

验证动作：

1. 主仓库本地编译
2. 主仓库 Docker ARM 交叉编译
3. 按风险等级决定是否上板：
   - `L1`：日志迁移、低风险 helper、轻量路径改动
   - `L2`：涉及运行时资源、配置路径、实际模块启动

说明：

- 若主仓库本地编译不过，不进入 Docker
- 若 Docker 不通过，不进入板端验证

#### B2.1 `CameraRecorder`

当前状态（2026-04-17 更新）：

- 已完成主仓库侧第一版迁移落位：
  - `pp_main/standalone/CameraRecorder/`
  - `pp_main/test/src/camera_recorder/`
- 已完成验证：
  - 主仓库本地 x86 编译通过
  - `CameraRecorder --help` 通过
  - 基于 sample YAML 的最小 `--dry-run` 通过
  - 3 个 host-only 单测通过
  - Docker ARM 交叉编译通过，产物确认为 `AArch64`
  - 带外设 ARM 板软件侧 `L2` smoke 已完成主链闭环：
    - `ugripper.service` 可直接拉起 `CameraRecorder --stereo-daemon`
    - `/tmp/umi_stereo_camera_status.json` 中左右 stereo 可进入 `ready=true`
    - 板端已完成多轮真实普通录制，episode 产物完整且 stop 校验通过
- 当前未闭环项：
  - ARM 容器当前缺少 `libusb-1.0` 开发包；现已允许在无 `libusb` 时完成编译，但若运行配置使用 `uvc_roll_absolute`，仍需板端补验并最终补齐依赖
  - `CameraRecorder` 独立叶子验证仍可补充：
    - 板端 `--dry-run`
    - 单独 direct smoke

迁移目标：

- 先把 `camera_recorder` 作为第一批 standalone app 试点迁入主仓库
- 保持当前仓内形成的物理形态：
  - `main.cpp`
  - `camera_recorder.cpp`
  - `camera_domain.cpp`

迁移范围：

- `src/camera_recorder/include/camera_recorder/*`
- `src/camera_recorder/src/main.cpp`
- `src/camera_recorder/src/camera_recorder.cpp`
- `src/camera_recorder/src/camera_domain.cpp`
- 直接依赖的最小 `utils` 能力

当前阶段不一起迁：

- `run_record.sh`
- `Updater / Calibration`
- 主仓库最终安装路径切换

完成标准：

- 主仓库已有 `standalone/CameraRecorder/` 或等价 app 目录
- 可在主仓库本地独立编译
- config / YAML / `--dry-run` / `--help` 行为保持可用
- 与 stereo control/status 相关的 file backend 口径未被误改

测试标准：

- 主仓库本地 Linux：
  - 编译通过
  - 相关 host-only 单测通过
  - `CameraRecorder --help`
  - 如主仓库保留 `--dry-run` 场景，则跑一遍最小 dry-run
- Docker ARM：
  - 交叉编译通过
  - 产物存在且路径正确
- ARM 板：
  - 默认按 `L2` 做
  - 至少验证进程可启动、配置能读到、camera 资源路径无明显错误

本次实际执行记录（2026-04-13）：

- 主仓库本地 Linux：
  - `CameraRecorder` target 编译通过
  - `test_camera_config`
  - `test_camera_command_builder`
  - `test_stereo_control_json`
  - `CameraRecorder --help`
  - `CameraRecorder --output-dir /tmp/pp_camera_dry_run --config-yaml test/src/camera_recorder/config_samples/schema_v1_valid.yaml --dry-run`
- Docker ARM：
  - `CameraRecorder` target 交叉编译通过
  - `file` / `readelf -h` 确认为 `ELF 64-bit` / `AArch64`
- 未执行：
  - 第一轮记录中未执行 ARM 板 `L2` smoke
  - 原因：当时只推进到主仓库代码接入与交叉编译门禁，板端启动验证留作后续节点补齐

补充板端软件侧验证（2026-04-16 ~ 2026-04-17）：

- 带外设 ARM 板上，`ugripper.service` 已可直接拉起 `CameraRecorder --stereo-daemon`
- `/tmp/umi_stereo_camera_status.json` 已确认：
  - `left_stereo.ready = true`
  - `right_stereo.ready = true`
  - 顶层 `service_state = ready`
- 数据盘 `/mnt/data_disk` 已在软件侧验证窗口内保持 `rw`
- 已完成连续多段真实普通录制，episode 产物完整，且校验日志明确为 `valid=true`
- 最近一轮连续短录复测中，已确认：
  - `episode_20260417_0007`
  - `episode_20260417_0008`
  - `episode_20260417_0009`
  - `episode_20260417_0010`
  - `episode_20260417_0011`
  均完成 stop 后 validation，未出现 `validation_failed`、`disk_not_writable`、`critical_devices_missing`、`stereo session failed`
- 后续板端软件侧又补了一轮更长时长录制：
  - `episode_20260417_0014`
  - 录制时长约 `165s`
  - 8 路视频、左右 `sensor_data_*.mcap`、`metadata.json`、`calibration.json`、`info.json` 全部存在
  - journal 已确认 `validation phase end ... valid=true`
- 同一轮日志里出现过一次 `playing: validation_failed`，但已定位为更早的短录 `episode_20260417_0012`：
  - 失败原因为 `left_cam_main.mkv span=0.112s, expected >=0.200s`
  - 该条属于超短录制触发的校验失败，不影响后续 `episode_20260417_0013` 与 `episode_20260417_0014` 的主链通过结论
- 启动早期仍可能短暂出现一次 `stereo_status_missing`，但后续会随 stereo status 文件生成自动恢复；当前按软件侧验收口径视为启动瞬态，不作为主链阻塞项

失败止损：

- 若 camera config / resource path 在主仓库侧尚未稳定，只修集成接线，不在同一 PR 顺手重构 camera 逻辑

#### B2.2 `SensorRecorder`

当前状态（2026-04-17 更新）：

- 已完成主仓库侧第一版迁移落位：
  - `pp_main/standalone/SensorRecorder/`
  - `pp_main/test/src/sensor_recorder/`
- 已完成验证：
  - 主仓库本地 x86 编译通过
  - `SensorRecorder --help` 通过
  - `zeroing --help` 通过
  - 3 个 host-only 单测通过
  - Docker ARM 交叉编译通过，`SensorRecorder` 与 `zeroing` 产物确认为 `AArch64`
  - 带外设 ARM 板独立 `L2` smoke 已完成：
    - `SensorRecorder --help` 正常
    - `zeroing --help` 正常
    - `timeout -s INT 6s ./bin/SensorRecorder/SensorRecorder /tmp/sensor_direct_smoke` 已成功产出左右 `sensor_data_*.mcap`
- 当前未闭环项：
  - x86 主机当前没有系统级 `libserialport-dev`；本次接入先使用 sibling `ugripper/.local-deps/libserialport` 作为本地验证 fallback
  - `mcap` 目前在 `standalone/SensorRecorder/third_party/mcap/` 内以最小自带方式接入，后续仍应评估是否收敛为主仓库统一第三方依赖入口

迁移目标：

- 把 `sensor_recorder` 作为第二批 standalone app 迁入
- 继续复用当前已形成的 `sensor_protocol` / `sensor_domain` 可测试边界

迁移范围：

- `src/sensor_recorder/include/sensor_recorder/*`
- `src/sensor_recorder/src/main.cpp`
- `src/sensor_recorder` 中仍属于 app 壳的 transport / writer / runtime 逻辑

完成标准：

- 主仓库已有 `standalone/SensorRecorder/` 或等价 app 目录
- 本地可独立编译
- 与串口、协议、时间戳、MCAP 写入相关的接线口径清楚

测试标准：

- 主仓库本地 Linux：
  - 编译通过
  - `sensor_protocol` / `sensor_domain` 相关 host-only 单测通过
  - `SensorRecorder --help`
- Docker ARM：
  - 交叉编译通过
  - 产物路径正确
- ARM 板：
  - 默认按 `L2` 做
  - 至少验证程序可启动、串口设备路径解析正确、无立即崩溃

本次实际执行记录（2026-04-13）：

- 主仓库本地 Linux：
  - `SensorRecorder` target 编译通过
  - `zeroing` helper target 编译通过
  - `test_bsp_crc`
  - `test_encoder_protocol`
  - `test_imu_batch_timestamp`
  - `SensorRecorder --help`
  - `zeroing --help`
  - `cmake --install build/x86/standalone_ros2`
- Docker ARM：
  - `SensorRecorder` / `zeroing` 交叉编译通过
  - `file` / `readelf -h` 确认产物为 `ELF 64-bit` / `AArch64`
- 未执行：
  - 第一轮记录中未执行 ARM 板 `L2` smoke
  - 原因：当时按节奏把 `CameraRecorder` 与 `SensorRecorder` 的板端验证统一后置

补充板端软件侧验证（2026-04-17）：

- 带外设 ARM 板上已执行：
  - `./bin/SensorRecorder/SensorRecorder --help`
  - `./bin/SensorRecorder/zeroing --help`
  - `timeout -s INT 6s ./bin/SensorRecorder/SensorRecorder /tmp/sensor_direct_smoke`
- 结果：
  - `SensorRecorder --help` 输出 `Usage: SensorRecorder [OUTPUT_DIR]`
  - `zeroing --help` 输出 `Usage: zeroing <left|right|/dev/encoder_path>`
  - `timeout` 返回 `124`，属于定时中断退出的预期行为，不按失败处理
  - `/tmp/sensor_direct_smoke/` 下已成功产出：
    - `sensor_data_left.mcap`
    - `sensor_data_right.mcap`
  - 两个文件大小均约 `300K`，说明独立 direct smoke 下左右传感器录制链可运行并写出 MCAP

失败止损：

- 若 sensor transport 依赖主仓库现有串口或采集框架，要优先做适配层，不在同一步里重写协议层

#### B2.3 `GripperHmiTool`

当前状态（2026-04-17 更新）：

- 已完成主仓库侧第一版迁移落位：
  - `pp_main/standalone/GripperHmiTool/`
  - `pp_main/test/src/gripper_hmi/`
- 已完成归属确认：
  - 当前按“`standalone` 目录下的 helper tool + `gripper_hmi` 运行时库”落位
  - 不把它提前升级成独立 service 或复杂 UI app
- 已完成验证：
  - 主仓库本地 x86 编译通过
  - `test_hmi_protocol` / `test_hmi_led_effects` 通过
  - `GripperHmiTool --help` 通过
  - `cmake --install build/x86/standalone_ros2`
  - Docker ARM 交叉编译通过，产物确认为 `AArch64`
  - 带外设 ARM 板独立 `L1-L2` smoke 已完成：
    - `GripperHmiTool --help` 正常
    - 停止 `ugripper.service` 后，工具可同时独立打开 `/dev/right_gripper` 与 `/dev/left_gripper`
    - `--duration 3 --poll-ms 100` 可正常打印双手状态并以 `exit_code=0` 退出
    - `--read-sn` 可分别成功读出左右夹爪 SN
    - 重新启动 `ugripper.service` 后主链可恢复
- 当前未闭环项：
  - 现场脚本当前仍使用历史名字 `gripper_hmi_test`；本步先不切 script，后续 `B3` 再决定是保留兼容名还是加适配层
  - `GripperHmiTool` 当前仍保留 `libserialport` 依赖：
    - ARM 侧使用系统 `libserialport-dev:arm64`
    - x86 本地在缺少系统开发包时，临时回退到 sibling `ugripper/.local-deps/libserialport`

迁移目标：

- 把 `gripper_hmi` 作为第三批迁入
- 明确它在主仓库里更接近“工具 / 驱动侧支持模块”还是独立 app

迁移范围：

- `src/gripper_hmi/*`
- 与 LED effect、按钮输入、串口依赖相关的最小闭环

完成标准：

- 主仓库侧归属明确
- 构建依赖和运行依赖清楚
- 不要求先把它做成复杂 UI 或新的交互外壳

测试标准：

- 主仓库本地 Linux：
  - 编译通过
  - `gripper_hmi` 相关 host-only 单测通过
- Docker ARM：
  - 交叉编译通过
- ARM 板：
  - 默认按 `L1-L2` 之间执行
  - 至少验证基础连通、无立即异常退出

本次实际执行记录（2026-04-13）：

- 主仓库本地 Linux：
  - `cmake -S standalone -B build/x86/standalone_ros2 -DBUILD_TARGETS=GripperHmiTool`
  - `cmake --build build/x86/standalone_ros2 --target GripperHmiTool`
  - `cmake -S test -B build/x86/test`
  - `cmake --build build/x86/test --target test_hmi_protocol test_hmi_led_effects`
  - `build/x86/test/src/gripper_hmi/test_hmi_protocol`
  - `build/x86/test/src/gripper_hmi/test_hmi_led_effects`
  - `build/x86/standalone_ros2/GripperHmiTool/GripperHmiTool --help`
  - `cmake --install build/x86/standalone_ros2`
- Docker ARM：
  - `docker exec pp-arm-dev ... cmake -S standalone -B build/arm/standalone -DBUILD_TARGETS=GripperHmiTool`
  - `docker exec pp-arm-dev ... cmake --build build/arm/standalone --target GripperHmiTool`
  - `file` / `readelf -h` 确认产物为 `ELF 64-bit` / `AArch64`
- 未执行：
  - 第一轮记录中未执行 ARM 板 `L1-L2` smoke
  - 原因：当时按节奏把 `CameraRecorder` / `SensorRecorder` / `GripperHmiTool` 的板端验证统一后置

补充板端软件侧验证（2026-04-17）：

- 带外设 ARM 板上已执行：
  - `./bin/GripperHmiTool/GripperHmiTool --help`
  - `sudo systemctl stop ugripper.service`
  - `./bin/GripperHmiTool/GripperHmiTool --duration 3 --poll-ms 100`
  - `./bin/GripperHmiTool/GripperHmiTool --port /dev/right_gripper --read-sn`
  - `./bin/GripperHmiTool/GripperHmiTool --port /dev/left_gripper --read-sn`
  - `sudo systemctl start ugripper.service`
- 结果：
  - `--help` 正常输出 usage
  - 双手状态读取期间工具报告 `Connected 2 gripper HMI device(s)`，并持续打印左右手状态
  - `--duration 3 --poll-ms 100` 以 `exit_code=0` 正常退出
  - `--read-sn` 已确认：
    - `/dev/right_gripper` -> `DAG912263B0059BA`
    - `/dev/left_gripper` -> `DAG912263B00528B`
  - 重启 `ugripper.service` 后服务重新进入 `active (running)`，说明工具验证未破坏主链恢复

失败止损：

- 若 `GripperHmiTool` 在主仓库里更适合作为 `UgripperRuntime` 的配套库，而非独立 app，应先固定归属，再继续迁移

#### B2.4 `UgripperRuntime`

当前状态（2026-04-17 更新）：

- 已完成第一版接入：
  - `pp_main/standalone/UgripperRuntime/` 已落位
  - `pp_main/test/src/record_runtime/` 已落位
  - 保持 `supervisor + worker` 形态，没有提前把运行时改写成单进程大应用
- 已完成验证：
  - 主仓库本地 x86 编译通过
  - runtime host-only 单测通过
  - `UgripperRuntime --help` 通过
  - `cmake --install build/x86/standalone_ros2` 通过
  - Docker ARM 交叉编译通过，产物经 `file` / `readelf -h` 确认为 `AArch64`
  - 带外设 ARM 板软件侧 `L2-L3` smoke 已完成主链闭环：
    - `ugripper.service` 可正常进入 `active (running)`
    - `audio_play.py` 可绑定 USB headset sink 并播放 `ready`
    - `CameraRecorder --stereo-daemon` 常驻后，`/tmp/umi_stereo_camera_status.json` 中左右 stereo 可进入 `ready=true`
    - `/mnt/data_disk` 在验证窗口内保持 `rw`
    - 已完成连续多段真实录制，episode 产物完整且 validation 通过
- 仍后移的验证：
  - 若需要更强验收口径，可继续补更多轮次或更长时长的长期稳定性观察

迁移目标：

- 最后迁 `record_runtime`
- 保留 supervisor 主控形态，不改成单进程大应用

迁移范围：

- `src/record_runtime/include/record_runtime/*`
- `src/record_runtime/include/record_runtime.h`
- `src/record_runtime/src/main.cpp`
- `src/record_runtime/src/record_runtime.cpp`
- `src/record_runtime/src/runtime_process.cpp`
- `src/record_runtime/src/runtime_domain.cpp`

必须一并考虑的依赖：

- `AudioCommandPort`
- `StereoSessionPort`
- `ShutdownRequestPort`
- HMI、audio、stereo daemon、episode 路径、service / script 交接点

完成标准：

- 主仓库已有 `standalone/UgripperRuntime/` 或等价 app 入口
- 能在主仓库内拉起和协调其余模块
- 保持当前“supervisor + worker”模型
- 路径、控制面、停止顺序没有明显退化

测试标准：

- 主仓库本地 Linux：
  - 编译通过
  - runtime 相关 host-only 单测通过
  - `UgripperRuntime --help`
  - 如环境允许，做最小无硬件启动 smoke
- Docker ARM：
  - 交叉编译通过
  - 关键产物位置正确
- ARM 板：
  - 至少按 `L2`
  - 若涉及 service、安装路径、交付链路，则直接按 `L3`
  - 验证内容至少包括：
    - 主控能起
    - 日志正常
    - audio / stereo / shutdown request 路径正确
    - 最小录制流程不立即失败

补充板端软件侧验证（2026-04-16 ~ 2026-04-17）：

- 带外设 ARM 板上已确认：
  - `ugripper.service` 处于 `active (running)`
  - 运行时进程组包含：
    - `bin/UgripperRuntime/UgripperRuntime`
    - `bin/UgripperRuntime/audio/audio_play.py`
    - `bin/CameraRecorder/CameraRecorder --stereo-daemon`
  - 音频日志已确认：
    - `audio backend bound to USB headset sink`
    - `audio player recovered and is ready`
    - `playing: ready`
  - stereo 状态文件已确认：
    - `left_stereo.ready = true`
    - `right_stereo.ready = true`
    - 顶层 `service_state = ready`
  - 数据盘已确认：
    - `/mnt/data_disk` 挂载为 `rw`
    - episode 根目录可持续写入
- 录制链软件侧结果：
  - 已完成连续短录 `episode_20260417_0007` ~ `episode_20260417_0011`
  - 已完成一轮约 `165s` 的较长录制 `episode_20260417_0014`
  - 上述 episode 均已确认：
    - 8 路视频、左右 `sensor_data_*.mcap`、`metadata.json`、`calibration.json`、`info.json` 全部存在
    - journal 中 `validation phase end ... valid=true`
    - 未出现 `disk_not_writable`、`critical_devices_missing`、`stereo session failed`
- 例外说明：
  - `episode_20260417_0012` 曾出现一次 `playing: validation_failed`
  - 已定位为超短录制导致 `left_cam_main.mkv span=0.112s, expected >=0.200s`
  - 该条不影响后续 `episode_20260417_0013` 与 `episode_20260417_0014` 的主链通过结论
- 启动早期仍可能短暂出现一次 `stereo_status_missing`，但后续会在 stereo status 文件生成后自动恢复；当前按软件侧验收口径视为启动瞬态，不作为主链阻塞项

失败止损：

- `UgripperRuntime` 迁移 PR 不混入 `Updater / Calibration` app 化
- 若 runtime 集成暴露路径或部署问题，先修部署适配，不在同一 PR 再做新的内部重构

### B3. Updater / Calibration 先按 deploy 链路治理

当前状态：

- 已完成第一版归属冻结：
  - `docs/archive/2026-refactor-history/baseline/deploy-script-boundaries.md` 已明确 update / calibration / shutdown 三条链路的保留边界
  - 已明确这一步只冻结“谁属于 deploy/script、谁属于 app”，不提前做 app 化
- 当前结论：
  - `Updater` 先归未来主仓库 `deploy/update`
  - `Calibration` 先归未来主仓库 `deploy/calibration`
  - `trigger_shutdown` 与 `umi-shutdown-trigger.*` 先归未来主仓库 `deploy/systemd`
- 仍后移的动作：
  - 主仓库真实目录落位
  - 安装路径、打包、service 切换
  - `L3` 交付 smoke

这两块不应过早 app 化。

原因：

- 它们本质更像交付 / 运维链路
- Shell 比重高
- 与安装路径、service、现场 SOP 强绑定

所以第二阶段前半段建议：

- 先按脚本 / deploy / calibration 链路治理
- 先落到主仓库对应脚本区域
- 等逻辑稳定后，再评估是否需要独立成 standalone app

临时归属建议直接写死：

- `Updater` 先归为 `deploy/update` 链路
- `Calibration` 先归为 `deploy/calibration` 或 `scripts/calibration` 链路
- 二者都先不纳入首批 standalone app 成功标准

#### 完成标准

- `Updater` / `Calibration` 已在主仓库中有清晰归属
- 但不强制把它们立即变成 standalone app

#### 测试策略

测试目标：

- 验证 deploy/script 归属清晰
- 验证不提前 app 化不会阻塞主链迁移

测试环境：

- 主仓库本地 Linux
- 按需使用 Docker ARM 构建
- ARM 板（仅在涉及运行时脚本、路径、交付行为时）

验证动作：

- 核对 deploy/update 与 deploy/calibration 的路径和归属
- 验证脚本入口仍清楚
- 涉及运行时、路径、service、打包时，按 `L3` 做交付 smoke

这一阶段不做：

- 强制把二者做成 standalone app

#### 细化执行清单

这一节点要明确两类边界：

1. 哪些文件仍属于 deploy/script
2. 哪些只是被 `UgripperRuntime` 或其他 app 调用

建议至少补齐：

- update 入口脚本归属
- calibration 入口脚本归属
- service / path unit / postinst / prerm 的对应关系
- 安装后真实路径
- 哪些行为必须等最终安装环境才能验证

这一步的目标是“先不误 app 化”，而不是“长期不治理”。

### B4. 最后切构建、打包、安装路径、service

当前状态：

- `B4.1` 顶层构建入口接入已完成第一版：
  - `pp_main/standalone` 的默认 `BUILD_TARGETS=all` 入口已能覆盖 `CameraRecorder`、`SensorRecorder`、`GripperHmiTool`、`UgripperRuntime`
  - x86 / Docker ARM 下都已验证这四个目标可从默认 standalone 入口被解析并编译
  - `pp_main/all` superbuild 已补上先接 `src` 再接 `standalone/test` 的顺序，并补齐 superbuild 复用 `PP::` targets 的兼容逻辑
- `B4.2` 安装路径切换已完成最终纯新布局：
  - 4 个迁移对象当前都只向 `bin/<AppName>/...` 新布局安装
  - `UgripperRuntime` 的 `audio/`、`audio_en/`、`config/fakeCamCalib.json` 只落到 `bin/UgripperRuntime/...`
  - legacy `build/src/...` 兼容路径和顶层 `audio/` / `audio_en/` / `config/` 安装逻辑已移除
  - 当前额外限制：
    - 本地 x86 若仍依赖 sibling `ugripper/.local-deps/libserialport` fallback，则 install tree 下执行 `UgripperRuntime --help` 仍需显式补 `LD_LIBRARY_PATH`
    - `pp_main/standalone` 新建干净 build 目录时仍依赖外部传入 `CMAKE_TOOLCHAIN_FILE`；这属于主仓库现有构建前提，不是本次安装布局改动引入的问题
- `B4.3` 打包清单切换已完成最终纯新布局 package staging：
  - 主仓库新增 `pp_main/package_ugripper_stage.sh`，以 `pp_main install tree` 作为 merged app payload 真源，组装 `build/package/<platform>/ugripper_stage`
  - 当前 staging 只收 `CameraRecorder`、`SensorRecorder`、`GripperHmiTool`、`UgripperRuntime` 四个 app 的新布局内容、`run_record.sh`、`auto_update/`、`auto_calibration/`、`scripts/`、`py_script/` 和两条 udev 规则
  - 当前 staging 已显式排除 `Explorer`、`Puppetry`、`MessageBridge` 等非 `ugripper` app，避免把主仓库其他 standalone 目标误打进同一个交付根目录
  - 当前 package staging 仍未做：
    - `DEBIAN/control` / maintainer scripts 迁入
    - systemd unit 的最终安装落位
    - 真实 `.deb` 产出
- `B4.4` service / path unit 切换已完成第一版交付 staging：
  - `pp_main/package_ugripper_stage.sh` 已把当前 legacy deb 真正安装的 unit 一并纳入 staging：
    - `ugripper.service`
    - `ugripper-calibration.service`
    - `ugripper-network-monitor.service`
    - `umi-shutdown-trigger.service`
    - `umi-shutdown-trigger.path`
  - 同一入口已生成 `DEBIAN/control`、`postinst`、`prerm`、`postrm`，并完成 `APP_NAME` / `VERSION` / `ARCH` / `INSTALL_DIR` 替换
  - 当前仍刻意不把以下对象纳入这一步的 service 切换：
    - `usb-auto-update@.service`
    - `ugripper-boot-install.service`
    - `/usr/local/bin/usb_auto_update.sh`
    - `/usr/local/bin/boot_check_install.sh`
  - 原因：
    - 它们本来就不在当前 legacy `ugripper` deb 安装清单中
    - 当前已决定把这条 `/usr/local/bin/...` + systemd unit 侧链收敛为可选独立包 `ugripper-usb-updater`
    - 因此它们不应并入当前主 `ugripper` package 主线，也不应让主包对其形成硬依赖
  - 当前边界口径：
    - 主 `ugripper` 包负责 `/opt/ugripper` 下的运行主链、主 service、校准链与数据盘主规则
    - 可选 `ugripper-usb-updater` 包负责：
      - `usb-auto-update@.service`
      - `ugripper-boot-install.service`
      - `/usr/local/bin/usb_auto_update.sh`
      - `/usr/local/bin/boot_check_install.sh`
      - 与这两条 unit 对应的可选 USB 升级 / 开机自恢复触发链
  - 当前已完成的实现层收口：
    - 主包 `config/99-fixed-usb-map.rules` 已不再通过 `SYSTEMD_WANTS` 拉起 `usb-auto-update@.service`
    - `auto_update/mount_data_disk.sh` 已不再在挂载成功后主动 `systemctl start usb-auto-update@...`
    - `auto_update/99-usb-auto-update.rules` 已恢复为 updater 包自己的真实触发规则
  - 板端已完成的验证：
    - “只装主包”场景下，`/mnt/data_disk` 挂载与写入正常，且系统不会尝试拉起不存在的 `usb-auto-update@.service`
    - “主包 + updater 同装”场景下，`usb-auto-update@<dev>.service` 已能由热插拔自动触发，并正常完成无安装包场景的 no-op 扫描流程
    - updater 侧初始缺失 `ugripper_shell_common.sh` 与资源根误判问题已修复；重装后板端复测通过
    - `ugripper-boot-install.service` 已完成手动 `systemctl start` smoke：
      - 在 `/opt/backup` 无候选包时可正常 no-op 退出并写入 `boot_install.log`
      - 在 `/opt/backup` 存在同版本 `ugripper` / `ugripper-usb-updater` 包时可正确识别并输出“无需升级”
    - `ugripper-usb-updater` 的卸载 / 重装链路已验证通过：unit、udev 规则会随包生命周期正确删除与恢复
  - 当前这条侧链剩余待验证：
    - `ugripper-boot-install.service` 的真实开机恢复路径时序
    - 带真实 `ugripper*.deb` / `ugripper_calib` / `calibration.txt` 的完整 USB 交付 smoke
  - 推荐验证分层：
    - `L1` 可模拟、低风险：
      - 在板端已挂载 `/mnt/data_disk` 的前提下，手动执行 `systemctl start usb-auto-update@<dev>.service`
      - 先仅放 `config.txt`，验证 `UGRIPPER_LANG` / `CAMERA_CODEC` 导入和日志输出
        - 已在板端验证通过：`UGRIPPER_LANG` 从 `zh` 更新为 `en`，`CAMERA_CODEC=h265` 保持一致，日志显示进入保护窗口、更新 `/etc/environment` 并单次重启 `ugripper.service`
      - 再单独放 `ugripper_calib/`，验证相机标定导入链
    - `L2` 半模拟、仍低于真实交付风险：
      - 通过人工准备 `/opt/backup` 候选包，并手动执行 `systemctl start ugripper-boot-install.service`
      - 可验证版本比较、恢复安装、同版本 no-op 与日志链
        - 已在板端验证通过：
          - `/opt/backup` 同版本包场景下正确输出“无需升级”
          - `dpkg -r ugripper` 后，检测到 `deinstall ok config-files` 并成功执行恢复安装
          - 恢复安装后，`ugripper.service`、`ugripper-network-monitor.service` 与 `umi-shutdown-trigger.path` 均已重新建立并恢复到 active/enabled 状态
      - 这一步可以覆盖脚本主逻辑，但不能替代真实开机时序验证
    - `L3` 必须真机场景：
      - 根目录 `calibration.txt` 会触发 `ugripper-calibration.service`，涉及 encoder 零位校准，当前无硬件时不设为门禁
      - `ugripper-boot-install.service` 的真实 boot 恢复路径仍需要至少一次真实重启 smoke
  - 当前建议执行顺序：
    - `config.txt` 模拟导入 smoke 已通过
    - 下一步做 `ugripper_calib/` 导入 smoke
    - `calibration.txt` 与真实 boot 恢复留到硬件或更合适窗口再补
- `B4.5` 真实 deb 产出已完成第一版：
  - 主仓库新增 `pp_main/package_ugripper_deb.sh`
  - 当前已能基于 `package_ugripper_stage.sh` 直接产出：
    - `build/package/x86/ugripper_1.2.8_amd64.deb`
  - 已通过 `dpkg-deb -I`、`dpkg-deb -c`、`dpkg-deb -x` 验证 control metadata、关键 payload、systemd unit 与 x86 解包结果
  - `B4.6` x86 测试环境真实安装验证已完成第一轮：
    - 测试机上已执行 `dpkg -i`，确认包状态为 `install ok installed`
    - 初次安装暴露真实根因：`ugripper.service` 写死 `User=ubuntu`，在当前测试机上触发 `status=217/USER`
    - 已用测试环境兼容补丁验证安装链路：`postinst` 按 `ubuntu -> uid 1000 -> root` 解析本机可用用户，并重写安装后的 `ugripper.service`
    - 二次安装后，当前测试机 service 以 `User=songwl` 成功进入 `active (running)`；`umi-shutdown-trigger.path` 处于 `active (waiting)`
  - 当前仍未完成：
    - `prerm` / 卸载回滚链路验证
    - Docker ARM / ARM 板安装验证
  - 明确约束：
    - 先前的动态 service 用户改写已降为测试环境显式 override，不再是生产包默认行为
    - 当前生产口径恢复为固定 `ubuntu`；若测试环境需要非 `ubuntu` 账户，需显式提供 `/etc/ugripper/service_user_override`
- `B4.7` ARM `.deb` 产出已完成第一轮：
  - 已直接基于现有 `install/arm` 产出：
    - `build/package/arm/ugripper_1.2.8_arm64.deb`
  - 已通过 `dpkg-deb -I`、`dpkg-deb -c`、`dpkg-deb -x` 和 `readelf -h` 验证：
    - 包元数据为 `Architecture: arm64`
    - 关键 payload 已进入包内
    - `UgripperRuntime` 二进制确认为 `AArch64`
  - 当前仍未完成：
    - ARM 环境真实 `dpkg -i`
    - ARM 环境 `postinst` / `prerm` 执行验证
    - ARM 板 `L3` 交付 smoke
  - 当前额外说明：
    - 这一步直接复用已有 `install/arm`，没有重跑本轮 `arm_build.sh`
    - `pp-arm-dev` 当前运行镜像标签显示为 `pp-arm-builder:latest`，而目标镜像为 `pp-arm-builder:arm-ready`；本次不阻塞打包，但后续 ARM 构建与安装复现前应统一
- `B4.8` ARM 安装验证已推进到 `postinst/systemd` 边界：
  - 在一次性 `pp-arm-builder:arm-ready` 容器中，ARM 包可被 `dpkg` 正常识别和解包
  - 在补齐依赖后，安装流程已推进到 `postinst` 执行阶段
  - 当前首个真实阻塞点是容器并非以 `systemd` 作为 `PID 1` 启动，因此：
    - `systemctl daemon-reload`
    - `systemctl enable/restart`
    - `udevadm ...`
    这类安装后动作无法在该容器里代表真实系统执行
  - 结论：
    - ARM 打包产物本身没有再暴露新的格式级问题
    - ARM 安装链的下一步验证必须转入 `systemd-capable` ARM 环境或直接上板
- `B4.11` 板端运行验证已进一步收敛：
  - 修复后的 ARM 包已在板端成功安装，`ugripper.service` 已越过旧的 `build/src/record_runtime` 缺失问题
  - 当前板端首个真实阻塞点来自设备存储环境：`run_record.sh` 等待 `/mnt/data_disk` 成为真实可写挂载点
  - 板端当前缺少原生 `exfat` 内核支持；手动加载 `exfat.ko` 后可挂载 `exfat` U 盘
  - 这部分被归类为设备镜像/内核环境问题，不再继续作为 `ugripper` 并仓代码问题追踪
  - 对 `ugripper` 主线的结论：
    - package / install / service / legacy 路径兼容修复已得到板端实证
    - `exfat` 不计当前阶段门禁；板端验证对 `ugripper` 主线可视为已完成
    - 后续优先回到生产 service 用户策略、旧入口清理和并仓收尾，而不是继续围绕 `exfat` 深挖
- `B4.12` 生产 service 用户策略已恢复：
  - `pack_script/postinst` 默认恢复固定 `ubuntu` 用户
  - 仅当 `/etc/ugripper/service_user_override` 存在时，才覆盖安装后的 `User=`
  - 若 override 文件内容为 `auto`，才启用测试环境动态探测逻辑
- `B4.13` 旧入口已开始降级为兼容入口：
  - `run_record.sh`、`run_calibration.sh`、`usb_auto_update.sh`、`import_camera_calibration.sh` 现已统一为“优先新安装路径，旧路径 fallback”
  - 当前默认优先使用：
    - `bin/UgripperRuntime/UgripperRuntime`
    - `bin/SensorRecorder/zeroing`
    - `bin/GripperHmiTool/GripperHmiTool`
    - `bin/UgripperRuntime/audio/...`
  - `build/src/...` 与顶层 `audio/` / `audio_en/` 仍暂时保留为兼容层，等待下一步最终清理
- `B4.14` 安装窗口残留进程清理已补齐兼容匹配：
  - `postinst`、`prerm` 与 `usb_auto_update.sh` 现在都会同时匹配：
    - `GripperHmiTool.*--state`
    - `gripper_hmi_test.*--state`
  - 目标是避免安装/卸载/USB 升级期间只清理旧 helper 名称，导致新 helper 进程残留
- `B4.15` 文档入口口径已对齐：
  - `README.md` 与 `docs/agent/overview.md` 已改为说明：
    - 主入口仍是 `run_record.sh`
    - 主运行时优先 `bin/UgripperRuntime/UgripperRuntime`
    - 旧 `build/src/...` 只作为兼容 fallback 保留
- `B4.16` `record_runtime` 自身默认路径已进入第二轮收口：
  - `RecordRuntimeOptions` 默认子进程与音频资源路径已切到 `bin/...` 优先
  - `initialize()` 会在新路径缺失时自动 fallback 到：
    - `build/src/...`
    - 顶层 `audio/...`
    - 顶层 `config/...`
  - 这意味着“新路径优先、旧路径 fallback”已不再只存在于 shell 脚本层，也进入了 C++ runtime 默认值层
  - 当前已完成：
    - 本仓 `record_runtime` 定向编译与 runtime 相关单测
    - `pp_main` x86 `UgripperRuntime` 重编、x86 `deb` 刷新与解包校验
    - `pp-arm-dev` 容器内 ARM `standalone` 重编、`install/arm` 落盘、ARM `deb` 刷新与解包校验
- `B4.17` / `B4.18` 显式 compat 开关阶段已完成并结束：
  - 这两个中间态步骤已用于验证“新布局可独立 install/package”
  - 当前主线已不再保留 `UGRIPPER_INSTALL_LEGACY_COMPAT` 与 `UGRIPPER_PACKAGE_INCLUDE_LEGACY_COMPAT`
- `B4.19` 运行链旧入口 fallback 已从默认代码路径移除：
  - `run_record.sh` 不再回退到 `build/src/record_runtime/record_runtime`
  - `record_runtime` / `pp_main/standalone/UgripperRuntime` 初始化阶段不再自动回退到：
    - `build/src/...`
    - 顶层 `audio/...`
    - 顶层 `config/...`
  - `run_calibration.sh`、`usb_auto_update.sh`、`import_camera_calibration.sh` 现已只按新安装布局查找：
    - `bin/SensorRecorder/zeroing`
    - `bin/GripperHmiTool/GripperHmiTool`
    - `bin/UgripperRuntime/audio/...`
    - `bin/UgripperRuntime/config/fakeCamCalib.json`
  - 当前已完成的板端无硬件 smoke：
    - 新 ARM `deb` 已可正常安装
    - `/mnt/data_disk` 已验证为真实可写挂载
    - `/opt/ugripper/bin/UgripperRuntime/UgripperRuntime` 已确认不再包含旧 `build/src/...`、顶层 `audio/...`、顶层 `config/...` 字符串
    - `UgripperRuntime --help` 在板端可正常输出 usage
    - `run_record.sh` 已确认实际调用新 `bin/UgripperRuntime/UgripperRuntime`
  - 当前剩余板端验证边界：
    - 因现场仅有板卡、无夹爪/HMI 等运行硬件，`runtime.initialize()` 的硬件依赖失败暂不继续追
    - 后续只需在有硬件条件时补一轮完整 runtime/service smoke
- `B4.20` dormant legacy 兼容层已从 install/package 代码中移除：
  - `pp_main/standalone/*/CMakeLists.txt` 不再生成 legacy `build/src/...` symlink，也不再安装顶层兼容资源目录
  - `pp_main/package_ugripper_stage.sh` 不再保留 legacy compat 开关、顶层兼容资源补拷贝和 `build/src/...` 兼容链接生成
  - `pack_script/postinst` 的默认 fake calibration 恢复路径也已收敛为 `bin/UgripperRuntime/config/fakeCamCalib.json`
- `B4.21` 带外设 ARM 板 `L3` 主包交付 smoke 已完成第一轮闭环：
  - 开发机已基于 ARM 板生成的 `aarch64` `.venv` 重新产出：
    - `ugripper_1.2.8+merge3_arm64.deb`
  - 带外设目标板 `192.168.2.240` 已完成真实安装验证：
    - `dpkg -s ugripper` 确认为 `Version: 1.2.8+merge3`
    - `ugripper.service` 处于 `active (running)`
    - `audio_play.py` 已确认通过 `./.venv/bin/python3` 启动，而不再依赖现场 `uv` 在线下载 Python
    - 板端日志已确认：
      - `pygame 2.6.1 (SDL 2.28.4, Python 3.11.15)`
      - `audio backend bound to USB headset sink`
      - `playing: ready`
    - 左右手固定设备节点已同时恢复：
      - `/dev/left_gripper`、`/dev/left_encoder`、`/dev/left_imu`
      - `/dev/right_gripper`、`/dev/right_encoder`、`/dev/right_imu`
  - 对当前主线的结论：
    - `postinst` 回放 `ttyCH9344USB* add` 事件的修复已在真实外设板上闭环
    - ARM `.venv` 随包交付后，`pygame` 缺失问题已闭环
    - 主包 `install -> postinst -> systemd -> runtime -> audio` 链路已完成第一轮真实板端验证
    - 后续板端带外设复测又确认出一个独立阻塞：
      - 当前 package baseline 把 camera YAML 安装在 `/opt/ugripper/bin/CameraRecorder/config/camera_recorder.yaml`
      - 但 `CameraRecorder` 默认仍按工作目录查找 `config/camera_recorder.yaml`
      - `record_runtime` 启动普通 `CameraRecorder` 和 `--stereo-daemon` 时也未显式传 `--config-yaml`
      - 结果是在 `1.2.8+merge3` 板端上，stereo daemon 会直接退出，`/tmp/umi_stereo_camera_status.json` 不生成，完整录制链因此被卡住
  - 后续 `B4.21` 板端纠偏与正式修包结论：
    - `merge5` 首轮修复包一度误判为“路径修复未生效”，但复盘后确认真正问题是打包阶段复用了陈旧的 `install/arm_camera_fix`，`deb` 中仍带旧版 `CameraRecorder`
    - 开发机随后在 `pp-arm-dev` 中重新编译 `CameraRecorder` 与 `UgripperRuntime`，重新安装到 `install/arm_camera_fix`，再产出唯一版本包：
      - `ugripper_1.2.8+merge6_arm64.deb`
    - `merge6` 板端已确认：
      - `/opt/ugripper/config/camera_recorder.yaml` 软链接可删除且无需恢复
      - package baseline 仍只安装 `/opt/ugripper/bin/CameraRecorder/config/camera_recorder.yaml`
      - `ugripper.service` 可直接拉起 `CameraRecorder --stereo-daemon`
      - `/tmp/umi_stereo_camera_status.json` 中左右 stereo 已进入 `ready=true`
      - 已完成连续两段真实普通录制：
        - `episode_20260416_0001`
        - `episode_20260416_0002`
      - 两段 episode 均已确认：
        - 8 路视频、左右 `sensor_data_*.mcap`、`metadata.json`、`calibration.json`、`info.json` 全部存在
        - `info.json` 中 8 路 `*_record_time_offset_us` 与 `stereo_session` 齐全
        - 无 `validation_error.log`
        - 停录后 stereo status 已回到 `service_state=ready`
    - 当前板端验收口径更新为：
      - 不再以手工前台再起一份 `CameraRecorder --stereo-daemon` 的 `warming/not_ready` 结果作为门禁
      - 该场景会与 service 内常驻 daemon 争抢双目设备；正式验收以 service 拉起的单实例 stereo status 为准
  - 当前仍未完成的板端/交付测试：
    - `CameraRecorder` 独立 `L2` smoke：完整录制主链与连续短录软件侧验收已通过；仍可补 `--dry-run` 与单独 direct smoke 作为叶子模块补充验证
    - `SensorRecorder` 独立 `L2` smoke：已在 ARM 板通过最小 direct smoke
      - `SensorRecorder --help` / `zeroing --help` 正常
      - `timeout -s INT 6s ./bin/SensorRecorder/SensorRecorder /tmp/sensor_direct_smoke` 已成功产出左右 `sensor_data_*.mcap`
    - `GripperHmiTool` 独立 `L1-L2` smoke：已在 ARM 板通过最小 direct smoke
      - `GripperHmiTool --help` 正常
      - 停止 `ugripper.service` 后可同时打开 `/dev/right_gripper` 与 `/dev/left_gripper`
      - `--duration 3 --poll-ms 100` 可正常打印双手状态并退出
      - `--read-sn` 可分别成功读出左右夹爪 SN
      - 重启 `ugripper.service` 后主链可恢复
    - `UgripperRuntime` 完整录制 `L2-L3` smoke：配置路径、stereo daemon 常驻、两段真实 episode 产物与 stop 校验链已在 `merge6` 闭环；后续板端软件侧复测又完成连续短录 `episode_20260417_0007` ~ `episode_20260417_0011`，以及一轮约 `165s` 的 `episode_20260417_0014`，validation 均为 `valid=true`；软件侧现仅剩更多轮次长期稳定性观察可按需要补做
    - ARM 板 `prerm` / 卸载回滚链路：已在真实板端完成
      - `dpkg -r ugripper` 后，`ugripper.service` 与 `ugripper-network-monitor.service` 被正确移除
      - 重新 `dpkg -i /tmp/ugripper_1.2.8+merge6_arm64.deb` 后，`ugripper.service`、`ugripper-network-monitor.service`、`umi-shutdown-trigger.path` 均恢复
      - 重装后 `UgripperRuntime`、audio daemon、`CameraRecorder --stereo-daemon` 再次进入运行态，`/tmp/umi_stereo_camera_status.json` 恢复 `ready=true`
    - updater 侧链真实 `L3` 交付场景：
      - `config.txt` 导入 smoke：已通过
        - `usb-auto-update@sda1.service` 可手动触发
        - `UGRIPPER_LANG=en`、`CAMERA_CODEC=h265` 已成功写入 `/etc/environment`
        - 仅一次重启 `ugripper.service`，未误触发 deb 安装
      - `ugripper_calib/` 导入 smoke：未验证
      - `calibration.txt` 触发零位校准 smoke：未验证
      - `ugripper-boot-install.service` 真实开机恢复 smoke：未验证
- `B4.21` 当前 ARM 构建/打包操作约定已固定：
  - 宿主机先复用已有容器，而不是在宿主机直接交叉编译：
    - `docker start pp-arm-dev`
    - `docker exec -it pp-arm-dev bash`
  - 容器内进入主仓并准备 ARM 依赖环境后执行编译：
    - `cd /home/songwl/swl_ws/pp_main`
    - `export CMAKE_PREFIX_PATH=/opt/openrobots:${CMAKE_PREFIX_PATH:-}`
    - `export PKG_CONFIG_PATH=/opt/openrobots/lib/pkgconfig:${PKG_CONFIG_PATH:-}`
    - `export LD_LIBRARY_PATH=/opt/openrobots/lib:/usr/lib/aarch64-linux-gnu:${LD_LIBRARY_PATH:-}`
    - `bash ./arm_build.sh`
  - 当前脚本语义需要特别记住：
    - `arm_build.sh` 会清理并重建 `build/arm`
    - `arm_build.sh` 只做 `cmake --build`，不会自动执行 `cmake --install`
    - 因此打包前必须补一次 install 落盘，否则 `package_ugripper_stage.sh` 会报 `Install root not found: /home/songwl/swl_ws/pp_main/install/arm`
  - 当前已验证可复用的 install 补步：
    - 容器内执行：
      - `cd /home/songwl/swl_ws/pp_main`
      - `cmake --install build/arm/standalone`
  - `install` 目录 root ownership 的处理约定：
    - 容器默认以 `root` 运行，`cmake --install` 很容易把 `install/arm` 落成 `root:root`
    - 本轮已验证的恢复方式：
      - `docker exec pp-arm-dev bash -lc 'chown -R 1000:1000 /home/songwl/swl_ws/pp_main/install/arm'`
    - 如果已经在宿主机上留下 root 产物，也可以直接在宿主机修复：
      - `sudo chown -R $(id -u):$(id -g) /home/songwl/swl_ws/pp_main/install/arm`
    - 如果连 `build/arm` 也被 root 占有，可一起修：
      - `sudo chown -R $(id -u):$(id -g) /home/songwl/swl_ws/pp_main/build/arm /home/songwl/swl_ws/pp_main/install/arm`
  - 当前主包打包入口保持在宿主机执行：
    - `cd /home/songwl/swl_ws/pp_main`
    - `TARGET_PLATFORM=arm APP_VERSION_SUFFIX=+merge7 bash ./package_ugripper_deb.sh`
    - 本轮验证产物：
      - `/home/songwl/swl_ws/pp_main/build/package/arm/ugripper_1.2.8+merge7_arm64.deb`
  - ARM `.venv` 当前本地真源目录已确认：
    - `/home/songwl/swl_ws/ugripper/.venv`
  - ARM `.venv` 的使用约定：
    - 该目录中的 `bin/python3` 已确认是 `ARM aarch64`
    - `pp_main/package_ugripper_stage.sh` 默认通过 `PACKAGED_VENV_SOURCE=${UGRIPPER_ROOT}/.venv` 收包
    - 因此当前 ARM 主包默认会把 `/home/songwl/swl_ws/ugripper/.venv` 一并打进 `/opt/ugripper/.venv`
    - 如果后续更换 `.venv` 来源，优先改 `PACKAGED_VENV_SOURCE` 环境变量，不要手改 stage 目录
- 当前额外说明：
  - `pp_main/all` 作为 IDE/superbuild 入口，后续仍会被 `MessageBridge / exton-refactor` 的既有 `PPCoreROS1/ROS2` 依赖链卡住；这属于主仓库原有 ROS superbuild 问题，不作为当前 `ugripper` 四个迁移对象的门禁失败

这一步必须最后做。

只有满足以下前提才允许切换：

- 旧入口可回退
- 新入口已 smoke 验证
- 资源路径已统一
- 打包清单已收住
- service 指向的新路径已验证

#### 切换内容

- 顶层 CMake 接入
- 打包入口切换
- 安装路径切换
- service 入口切换
- 清理旧入口

#### 完成标准

- 新路径下可 build / package / install
- `ugripper.service` 可从新入口正常启动
- 旧路径不再是默认入口

#### 测试策略

测试目标：

- 验证新的构建、打包、安装和 service 链路全部可用

测试环境：

- 主仓库本地 Linux
- 主仓库 Docker ARM 构建环境
- ARM 板

验证动作：

1. 主仓库本地编译通过
2. Docker ARM 交叉编译通过
3. 产物生成位置正确
4. 包可安装
5. service 可启动
6. 资源路径、配置路径、脚本路径正确
7. 按 `L3` 做交付 smoke

这一步必须上板验证。

#### 细化执行清单

建议按下面顺序切，不要并行混做：

1. 顶层 CMake 接入
2. 安装路径切换
3. 打包清单切换
4. service / path unit 切换
5. 旧入口降级为兼容入口
6. 旧入口最终清理

每切一步都要回答：

- 新旧入口是否可回退
- 安装后资源路径是否真实存在
- service 是否指向了正确的可执行和脚本
- Docker ARM 产物是否与板端部署路径一致

### B4.x 后独立子阶段：通信 backend / ZMQ 对齐

这一步明确后移到当前 package 主线之后，作为独立子阶段处理，不与 `B4` 的构建、打包、安装路径、service、deploy/update 边界收口混做。

放在这里而不是提前并入 `B4` 的原因：

- 当前 `B4` 的核心目标是把主包安装布局、systemd unit、udev、deploy/update 边界和板端交付链收稳
- ZMQ backend 迁移会同时引入通信后端、运行时依赖、Docker ARM、板端联调和节点级 smoke，风险面明显大于当前 package 收口
- 当前仓内已经完成：
  - 通道台账冻结
  - `AudioCommandPort`
  - `StereoSessionPort`
  - `ShutdownRequestPort`
  - file / pipe backend 保行为等价
- 因此当前更合理的边界是：
  - 本阶段只保留 transport-agnostic 接口
  - 真正的 backend 切换在后续独立子阶段完成

这个独立子阶段的前置条件：

- `B4` 主包安装链和 service 主线已经稳定
- `usb-auto-update@.service` / `ugripper-boot-install.service` 的归属边界已经明确，不再与主包 install path 切换互相干扰
- `pp_main` 侧可复用的 communication 依赖与构建方式已确认

首批建议纳入范围：

- `AudioCommandPort`
- `StereoSessionPort`
- 必要时评估 runtime health / orchestration 状态广播是否值得进入 message channel

首批明确不纳入范围：

- `ShutdownRequestPort` 的 transport 切换
- worker stdout/stderr 管道
- `waitpid` / 进程组 signal
- runtime 日志同步 `.pos/.lock`
- `/tmp/umi_recording.lock`

建议执行顺序：

1. 在主仓库侧确认 `PP::communication` / `message-hub` 复用口径
2. 为 `AudioCommandPort` 增加可切换 backend 的接线，但默认仍保留 file backend
3. 为 `StereoSessionPort` 增加 ZMQ backend，并保留 file backend 作为回退或 shadow 对照
4. 评估是否需要首批广播 runtime health / orchestration 状态；若没有明确收益，本轮不做
5. 在主仓库本地 Linux 环境完成节点级 smoke
6. 在 Docker ARM 环境验证构建与运行依赖
7. 在 ARM 板做最小通信链路 smoke

这一子阶段的完成标准：

- 已明确首批只切哪些控制面通道，哪些继续保留 file backend
- `AudioCommandPort` / `StereoSessionPort` 至少有一条完成 backend 可切换接线与 host-side 测试
- `ShutdownRequestPort` 继续保持 file backend，不因为通信改造影响 `umi-shutdown-trigger.path/service`
- 已完成主仓库本地编译、Docker ARM 编译和最小板端 smoke
- 文档中已补齐 channel ownership、fallback 策略和回退路径

### B5. 第二阶段建议执行节奏

建议按下面节奏推进，而不是按文件数量推进：

1. `B1` 并仓准备表
2. `CameraRecorder` 试点迁移
3. `SensorRecorder` 试点迁移
4. `GripperHmiTool` 归属确认并迁移
5. `UgripperRuntime` 迁移
6. `Updater / Calibration` deploy 链路落位
7. 顶层 CMake / 打包 / 安装 / service 切换
8. 通信 backend / ZMQ 独立子阶段

每个节点都应产出：

- 1 个清晰 PR 范围
- 1 条并仓记录或 `REFACTOR_LOG` 记录
- 1 组已执行测试
- 1 组未执行测试与原因
- 1 条“是否允许进入下一节点”的结论

## 7. 可直接执行的任务分组

### 第 1 组：只在 `ugripper` 内做，不碰主仓库

- 建主链 / 入口 / 路径基线清单
- 建最小 `src/utils`
- 接统一日志
- 收低风险 helper
- 建立 camera 抽象层
- 抽路径 helper
- 拆 `run_record.sh`
- 拆 `usb_auto_update.sh`
- 收口 `run_calibration.sh`
- 收口 `audio/*.py` 的日志和路径
- 补单元测试 / 无硬件测试
- 做行为等价 smoke

### 第 2 组：开始准备并仓

- 梳理每个模块的构建依赖
- 梳理每个模块的运行时资源
- 梳理安装路径映射
- 梳理 service 对应关系
- 梳理哪些脚本属于 app，哪些属于 deploy
- 固定第一批迁移顺序和每个节点的测试门禁

### 第 3 组：并入 standalone

- 先迁 `CameraRecorder`
- 再迁 `SensorRecorder`
- 再迁 `GripperHmiTool`
- 最后迁 `UgripperRuntime`

### 第 4 组：最后切交付

- 切顶层 CMake
- 切打包
- 切安装路径
- 切 service
- 清旧入口

## 8. 步骤完成提醒规则

每完成一个步骤，都应明确提醒一次当前状态。

提醒内容至少包括：

1. 当前完成的步骤名
2. 已完成的核心改动
3. 已执行的测试
4. 未执行的测试与原因
5. 下一步建议

说明：

- 文档与 skill 会把“完成后提醒”作为输出要求
- 但若你不发起新的交互，我不能在会话外主动推送提醒
## 9. 建议的 PR 顺序

1. 文档和基线清单 PR
2. 最小 `src/utils` PR
3. C++ 日志统一 PR
4. 路径 helper / 路径台账 PR
5. 低风险 helper 收口 PR
6. camera 抽象收口 PR
7. `run_record.sh` / `usb_auto_update.sh` / `run_calibration.sh` / `audio/*.py` 收口 PR
8. `Stage B` 并仓准备表 PR
9. `CameraRecorder` 并仓 PR
10. `SensorRecorder` 并仓 PR
11. `GripperHmiTool` 并仓 PR
12. `UgripperRuntime` 并仓 PR
13. `Updater / Calibration` deploy 归属 PR
14. 顶层 CMake / 打包 / 安装 / service 切换 PR
15. 通信 backend / ZMQ 独立子阶段 PR
16. 旧入口清理 PR

## 10. 最终判断

这份计划的执行主线是：

- 先把 `ugripper` 重构到“可合并”
- 再做“合并后的漂亮形态”

如果跳过第一段，直接推进 standalone app 化，最终只会把当前的路径硬编码、重复 helper 和脚本复杂度一起搬进主仓库。
