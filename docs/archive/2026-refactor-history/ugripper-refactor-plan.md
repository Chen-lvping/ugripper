# Ugripper Refactor Plan

本文档只描述 `ugripper` 仓库自身的重构阶段计划。
它不处理并入主仓库 `standalone` 的问题，只关注把当前仓库重构到“边界清楚、可维护、可测试、可合并”的状态。

相关文档：

- 当前系统说明：[docs/agent/overview.md](/home/songwl/swl_ws/ugripper/docs/agent/overview.md)
- 仓库架构规则：[docs/repo-architecture.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/repo-architecture.md)
- 目录映射表：[docs/repo-migration-map.md](/home/songwl/swl_ws/ugripper/docs/repo-migration-map.md)
- 并仓两阶段计划：[docs/standalone-merge-plan.md](/home/songwl/swl_ws/ugripper/docs/standalone-merge-plan.md)
- 命名与 YAML 契约：[docs/naming-yaml-contract.md](/home/songwl/swl_ws/ugripper/docs/archive/2026-refactor-history/naming-yaml-contract.md)

## 1. 目标

本计划解决的是 `ugripper` 自身的 9 个问题：

- 主链入口、关键路径、配置入口不够显式
- 日志格式和日志入口不统一
- helper 散落在 shell、C++、Python 中重复实现
- `camera_recorder` 中 camera 相关职责混杂，缺少清晰抽象边界
- 路径硬编码过多
- 高风险脚本职责过重
- 打包与运行时边界不清晰
- 单元测试和可回归测试不足
- 整体代码可读性、可维护性不足

本计划暂不做：

- 并入 `pp_main/standalone`
- 切换安装前缀
- 切换 service 新路径
- 强推理想化目录树
- 一次性复刻完整 `src/utils` 基础设施

## 2. 重构原则

### 2.0 C++ 标准默认约束

由于主仓库 `pp_main` 默认使用 `C++20`，`ugripper` 后续重构与并仓准备也应默认对齐 `C++20`。

约束如下：

- 新建 C++ 模块默认使用 `C++20`
- 现有模块重构时，不再引入新的 `C++17` 局部例外
- 若第三方依赖或交叉编译链对 `C++20` 存在约束，必须显式记录为例外，而不是默认回退
- 主仓库阶段继续验证本地编译、Docker ARM 交叉编译与板端环境是否完整接受该约束

这条规则的目标不是“为了用新特性而升级”，而是避免仓库内部继续维持多套 C++ 标准，降低后续并仓时的构建分叉风险

### 2.1 先收边界，再动结构

优先搞清楚：

- 哪些是产品主链
- 哪些会影响线上
- 哪些路径是运行时关键路径
- 哪些脚本和模块必须保行为等价

不要一开始就大量搬目录。

### 2.2 先行为等价，再做整理

允许做的变更：

- 日志入口替换
- helper 收口
- camera 抽象收口
- 路径访问封装
- 脚本拆层
- 打包边界收口

不允许在同一阶段混入：

- 新功能
- 状态机语义变化
- 录制流程变化
- 新硬件兼容行为

### 2.3 先最小 utils，再重型基础设施

第一阶段只允许建立最小 `src/utils`：

- `logger`
- `file utils`
- `env/config` 小工具
- 必要时的 `single process helper`

第一阶段不引入：

- 重型 `yaml_parser`
- 重型 `config_manager`
- 重型 `task_scheduler`
- 主动流程控制类
- 复杂生命周期对象

补充约束：

- `src/utils` 只做被动工具层，不承接主流程编排
- `src/utils` 可以被业务模块依赖，但不能反向依赖业务模块
- 多线程与周期任务设计可参考 `pp_main` 的 `TaskScheduler` 思路，但当前阶段不直接引入其实现、依赖栈或全局单例模式
- 若需要收口周期任务，先在业务模块内部形成轻量、可测试、可回退的局部边界

进程内线程调度的落地原则：

- 参考 `/home/songwl/swl_ws/pp_main/standalone` 的薄入口、总装对象、显式 `Init/Start/Stop` 与明确退出顺序
- 区分“周期任务”和“阻塞 I/O”两类线程：
  - 周期任务：健康检查、状态刷新、deadline/retry/timeout 检查
  - 阻塞 I/O：设备 `poll()`、串口读写、子进程等待、pipe/file 控制面等待
- 可参考 `TaskScheduler` 的设计目标去收口周期任务，但不直接复制 `/home/songwl/swl_ws/pp_main/src/utils/task_scheduler.*` 的 `oneTBB + 单例 + timer thread` 实现
- 当前阶段不把阻塞 I/O 强行改造成统一 timer task，而是让各自对象继续持有自己的后台线程或等待链路

推荐目标线程模型：

- `record_runtime`
  - 主线程负责 CLI、信号处理、顶层 `Init/Run/Stop`
  - supervisor/control 主循环负责 worker 状态刷新、HMI 输入消费、录制状态推进
  - audio、stereo、health 等周期逻辑逐步收口为局部可测试 loop / policy
- `camera_recorder`
  - 主线程负责参数、配置、session 装配与 stop 传播
  - 采集线程继续负责设备 `poll()` / 取帧
  - 写入线程继续负责 frame queue 消费与 writer flush
  - 仅 warmup、stop timeout、restart/backoff 这类控制逻辑向 `pp_main` 风格靠拢

为什么不直接复用 `pp_main` 当前调度器：

- 它解决的是单进程应用里的周期 Run/Publish/Check 任务，不是当前 `ugripper` 的 supervisor 多进程监督问题
- 它会直接引入新的依赖栈和全局生命周期复杂度
- 当前 `TaskScheduler` 的取消/退出语义本身不适合直接接现有 stop escalation 路径

### 2.3.1 通信通道先做 ledger，再做接口

控制面通道治理默认分两步：

1. 先冻结事实台账
2. 再抽 transport 无关接口

第一轮台账以：

- `docs/archive/2026-refactor-history/baseline/control-channel-ledger.md`

为准。

当前冻结的首批接口边界为：

- `AudioCommandPort`
- `StereoSessionPort`
- `ShutdownRequestPort`

当前明确不纳入首批 ZMQ 迁移的对象为：

- worker stdout/stderr 管道
- `waitpid` / 进程组 signal
- runtime 日志同步 `.pos/.lock`
- `/tmp/umi_recording.lock`

执行约束：

- 未完成 ledger 前，不开始改 transport
- transport 抽象 PR 不混调度收口、路径治理或日志迁移
- file / pipe backend 在当前阶段继续保留为默认实现

### 2.4 Shell 先做“编排化”

高风险 shell 脚本先拆成：

- 编排层
- helper 层
- 配置读取层

而不是继续在一个脚本里堆所有逻辑。

### 2.5 PR 只做一类事情

明确约束：

- 日志 PR 只改日志
- helper PR 只改 helper 归属
- 路径 PR 只改路径台账和访问方式
- 脚本拆分 PR 只做拆层

禁止在一个 PR 中混做：

- 日志替换
- helper 抽取
- 行为修复

### 2.6 Camera 抽象按“参考设计，不照搬实现”推进

`ugripper` 中的 camera 处理需要新增独立抽象层，但不能机械照搬 `pp_main/src/device/cameras`。

参考对象：

- `/home/songwl/swl_ws/pp_main/src/device/cameras/README.md`
- `/home/songwl/swl_ws/pp_main/src/device/cameras/camera_base.h`
- `/home/songwl/swl_ws/pp_main/src/device/cameras/camera_types.h`

应借鉴的是这些设计目标：

- camera 类型与配置单独表达
- camera 接口与具体后端实现分离
- 错误语义和能力边界显式化
- 业务编排层不要直接堆设备细节

不应直接照搬的原因：

- `pp_main` 的 camera 抽象层级更靠近设备访问
- `ugripper` 当前更偏录制编排、配置解析、pipeline 组装与会话管理
- 若直接复制 `pp_main` 结构，容易引入与当前职责层级不匹配的抽象

因此本仓库的目标是：

- 建立适合 `ugripper` 当前层级的轻量 camera 抽象
- 拆开 camera 配置、camera 设备能力、pipeline 组装、stereo 会话协调等职责
- 让后续新增或修改 camera 类型时，不必继续改一个巨大的 `camera_recorder.cpp`

补充约束：

- 允许细分头文件边界，但 `camera_recorder` 的实现文件数量应尽量控制在 `2-3` 个
- `record_runtime` 后续继续拆分时，也以 `2-3` 个实现文件为宜
- 推荐按“app 壳 / process or runtime boundary / domain logic”归并实现，而不是把每个小概念都做成单独 `.cpp`
- 若某块逻辑体量很小，优先并入相邻职责实现文件，避免为了形式上的分层制造新的维护负担

### 2.6.1 日志能力先对齐 API，再对齐后端

当前 `ugripper` 的 `src/utils/include/utils/logger.h` 是阶段性过渡实现。

重构与并仓准备阶段的目标是：

- 先统一调用入口到 `DM_LOG_INIT`、`DM_LOG_*`、`DM_LOG_*_STREAM()`
- 先减少裸 `std::cout/std::cerr` 作为运行日志入口
- 为后续并入 `/home/songwl/swl_ws/pp_main/src/utils/logger.h` 做 API 兼容准备

当前阶段不做：

- 在 `ugripper` 仓库内直接移植完整 ROS1/ROS2 + `spdlog` 日志后端
- 为了提前复刻主仓库实现而引入新的运行依赖或构建复杂度

### 2.6.2 通信演进按 staged migration 推进

后续需要尽量向 `pp_main` 的 ZMQ 通信模型靠拢，但这不是当前阶段立即切换的目标。

当前阶段的要求是：

- 先识别现有 pipe / file 控制面的通道类型和职责
- 先把 transport 无关的控制接口收清
- 保留当前 transport 作为默认实现
- 将 ZMQ 后端引入、通道替换和板端验证放到后续独立阶段推进

### 2.7 重构默认补测试

重构不只要求“编译通过”，还要求把可测试的逻辑沉淀为测试资产。

默认应补测试的对象：

- 新增的 `src/utils` 纯函数和轻量 helper
- camera 抽象中的类型转换、配置校验、命令拼装、模式选择
- 路径 helper
- 低风险 helper 收口后的关键语义

优先补单元测试，不要求第一阶段就建立完整测试体系，但不能继续让纯逻辑长期无测试覆盖。

### 2.8 可读性与可维护性是验收条件

每次重构除行为等价外，还应显式改善以下至少一项：

- 文件职责更单一
- 类边界更清楚
- 巨型函数被拆开
- 隐式副作用减少
- 命名、日志、错误路径更一致
- 新人能更快定位 camera / 路径 / 配置 / 脚本入口

如果改动只是“换个地方放代码”，却没有提升理解成本或维护成本，就不算高质量重构。

## 3. 当前主链范围

当前视为产品主链的内容：

- `src/record_runtime`
- `src/camera_recorder`
- `src/sensor_recorder`
- `src/gripper_hmi`
- `run_record.sh`
- `auto_update/`
- `auto_calibration/`
- `config/`
- `audio/`
- `audio_en/`

当前视为部署与交付链路的内容：

- `pack_script/`
- `build_deb.sh`
- `usb_updater_build.sh`

当前视为工具或现场辅助的内容：

- `py_script/`
- `test/`

当前视为冻结候选的内容：

- `ASR/`
- `time_sync/`

## 4. 阶段计划

## Phase 0：架构建模

### 目标

- 固定重构边界
- 定义模块角色
- 定义依赖方向

### 已有文档

- `docs/archive/2026-refactor-history/repo-architecture.md`
- `docs/repo-migration-map.md`

### 测试策略

这一阶段不做代码运行验证，只做文档和事实一致性检查。

测试环境：

- 本地 Linux

验证动作：

- 核对文档中的模块划分是否与当前目录结构一致
- 核对主链、工具、部署、冻结候选的定义是否与现状一致
- 对照 `docs/agent/overview.md` 检查是否存在明显冲突

这一阶段不做：

- Docker ARM 构建
- ARM 板运行
- 代码编译或运行 smoke

### 完成标准

- 所有一级目录有角色定义
- 主链 / 工具 / 部署 / 冻结候选边界清楚

## Phase 1：基线冻结

### 目标

在动代码前，把“当前事实”冻结成可审计清单。

### 输出物

- `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`
- `docs/archive/2026-refactor-history/baseline/service-map.md`
- `docs/archive/2026-refactor-history/baseline/config-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/equivalence-checklist.md`

### 约束

baseline 文档里的每个条目都必须写明事实来源：

- `source: <code path>`
- `source: <script path>`
- `source: <service file>`
- `source: <package script>`
- `source: <runtime command>`

### 行为等价检查至少覆盖

- 正常启动
- 正常开始录制
- 正常停止录制
- 音频播放正常
- 相机录制正常
- 传感器录制正常
- 升级脚本能执行
- 校准脚本能执行

### 本地最小可运行基线

在开始重构前，必须记录一份当前本地 Linux 环境下的“最小可运行基线”。

至少要写清楚：

- 哪个命令能启动 `record_runtime`
- 哪个命令能启动 `camera_recorder`
- 哪个命令能启动 `sensor_recorder`
- 哪些脚本支持 `--help`、`--dry-run` 或等价最小运行方式
- 哪些日志代表正常启动
- 哪些资源路径当前是可用的

目的不是做完整测试，而是为后续每次重构提供固定对照物，避免只能靠印象判断“是不是没坏”。

### 测试策略

这一阶段的测试目标是“冻结现状”，不是“证明未来架构可行”。

测试环境：

- 本地 Linux

验证动作：

- 验证 baseline 文档中的命令、路径和入口能够在当前环境复现
- 对每个 baseline 条目补齐 `source`
- 实际跑一遍本地最小可运行基线中的命令
- 记录哪些命令可运行、哪些只能静态分析

建议至少覆盖：

- `record_runtime` 的最小启动命令
- `camera_recorder` 的 `--help`、`--dry-run` 或等价最小命令
- `sensor_recorder` 的最小启动命令或 `--help`
- `run_record.sh` 的存在性和入口关系
- 关键脚本的 `bash -n`
- 关键 Python 文件的 `python3 -m py_compile`

这一阶段不做：

- Docker ARM 构建
- ARM 板 smoke
- 新路径验证

### 完成标准

- 可以明确回答“改哪些会影响线上”
- 可以明确回答“哪些路径是运行时关键路径”
- 行为等价不再只是口头描述

## Phase 2：建立最小 `src/utils`

### 目标

建立最小公共层，先收低风险被动工具。

### 第一批内容

- `src/utils/CMakeLists.txt`
- `src/utils/logger.h`
- `src/utils/file_utils.hpp`
- `src/utils/env_utils.h/.cc` 或等价轻量实现
- 如确有需要，再补 `single_process_instance`

### 依赖约束

- `src/utils` 可以被业务模块依赖
- `src/utils` 不允许反向依赖 `record_runtime`、`camera_recorder`、`sensor_recorder`、`gripper_hmi`

### 不做的事

- 不复刻完整 `pp_main/src/utils`
- 不引入主动流程控制
- 不引入复杂生命周期管理

### 测试策略

这一阶段只验证“最小公共层是否能被安全引入”。

测试环境：

- 本地 Linux

验证动作：

- 编译 `src/utils` 自身
- 编译第一个接入 `utils` 的业务模块
- 若有轻量 helper，可增加最小单测或示例调用
- 检查 `src/utils` 是否错误依赖业务模块

最低测试要求：

- 受影响目标可编译
- include 路径正确
- 替换前后行为等价

这一阶段不做：

- Docker ARM 构建
- ARM 板运行
- 大范围主链 smoke

### 完成标准

- `src/utils` 可独立构建
- 至少一个业务模块开始依赖它
- 不引入行为变化

## Phase 3：统一 C++ 日志

### 目标

让核心 C++ 主链统一到一个日志入口。

### 改造范围

- `src/record_runtime`
- `src/camera_recorder`
- `src/sensor_recorder`
- `src/gripper_hmi`

### 改造规则

- 只改 include、宏调用、消息格式
- 不新增关键日志点
- 不删除已有排障语义
- 不顺手迁移 helper
- 不顺手修业务逻辑

### 测试策略

这一阶段只验证“日志接入是否完成且语义未丢”。

测试环境：

- 本地 Linux

验证动作：

- 编译被改的 C++ 目标
- 运行相关二进制的最小命令、`--help` 或最小 smoke
- 检查关键日志仍然存在
- 检查日志入口已统一，且没有出现新的裸 `std::cout/std::cerr`
- 对照 `/home/songwl/swl_ws/pp_main/src/utils/logger.h` 检查当前宏接口是否仍保持可迁移

建议覆盖：

- `record_runtime`
- `camera_recorder`
- `sensor_recorder`
- `gripper_hmi` 相关可执行或测试工具

这一阶段不做：

- helper 迁移验证
- 路径切换验证
- Docker ARM 构建
- ARM 板 smoke

### 完成标准

- 新增 C++ 主链日志统一走同一入口
- 主链中裸 `std::cout/std::cerr` 明显减少
- review 能聚焦“日志迁移”本身

## Phase 4：路径台账与路径封装

### 目标

先搞清所有运行时关键路径，再逐步消灭散落硬编码。

### 子步骤

#### Phase 4.1 路径台账

把运行时关键路径分成：

- 二进制路径
- 音频资源路径
- 配置路径
- 临时文件路径
- 日志路径
- 校准资产路径

#### Phase 4.2 路径封装

在有台账的前提下，逐步替换：

- `./build/src/...`
- `./audio/...`
- `./config/...`

### 约束

- 这一阶段不切安装前缀
- 不引入主仓库绝对路径
- 目标是消灭散落硬编码，不是提前并仓

必须明确区分两类验证：

- 在 `ugripper` 阶段，只验证“本地逻辑上的路径解析是否统一”
- 在主仓库阶段，才验证“安装后的真实路径是否正确”

也就是说，在 `ugripper` 阶段本地能找到 `./audio/...` 或等价路径，只能说明本地重构可控，不能说明安装路径问题已经彻底解决。

### 完成标准

- 关键路径已集中表达
- 后续切路径只需改少数集中点

### 测试策略

这一阶段分两层验证。

#### 本地 Linux 验证

目标：

- 验证本地逻辑上的路径解析是否统一

验证动作：

- 对照路径台账检查所有关键路径是否已归类
- 运行本地最小命令，确认程序仍能找到资源、配置和二进制
- 检查路径 helper 是否覆盖原本的硬编码点

#### 主仓库 / ARM 阶段验证

目标：

- 验证安装后的真实路径是否正确

说明：

- 这部分不在 `ugripper` 仓库阶段执行
- 作为后续并仓阶段的集成 / 交付验证

这一阶段在 `ugripper` 仓库内不做：

- 安装前缀切换验证
- 主仓库绝对路径验证
- ARM 板路径验证

## Phase 5：收口低风险 helper

### 目标

把重复且低风险的 helper 收回公共层。

### 优先迁移

- `trim_text`
- `read_env_value`
- `currentSteadyMs`
- `currentEpochMs`
- `fileExistsAndNotEmpty`

### 暂缓迁移

- `runCommandSync`
- 带外部行为的进程执行 helper
- 复杂配置管理
- 调度器类能力

### 测试策略

这一阶段只验证“低风险 helper 收口后，调用点行为不变”。

测试环境：

- 本地 Linux

验证动作：

- 编译被影响的目标
- 对替换 helper 的模块做最小 smoke
- 对关键 helper 做最小输入输出检查
- 确认没有引入新的副作用

建议重点检查：

- 文本裁剪和环境变量读取
- 时间函数是否仍返回可用值
- 文件存在性判断是否保持原语义

这一阶段不做：

- Docker ARM 构建
- ARM 板运行
- 带外部行为 helper 的统一抽象

### 完成标准

- 至少两个主链文件的重复 helper 被消除
- 迁移后的 helper 集中在 `src/utils`
- 保行为等价

## Phase 6：Camera 抽象收口

### 目标

把 `src/camera_recorder` 中混杂的 camera 相关职责拆开，建立适合 `ugripper` 的轻量 camera 抽象。

### 参考与边界

参考：

- `/home/songwl/swl_ws/pp_main/src/device/cameras/README.md`
- `/home/songwl/swl_ws/pp_main/src/device/cameras/camera_base.h`
- `/home/songwl/swl_ws/pp_main/src/device/cameras/camera_types.h`

边界：

- 参考目标类的设计思想，不照搬目录结构、命名和实现细节
- 不强行把 `ugripper` 改造成完整设备访问库
- 不为了抽象而抽象，不引入当前主链不需要的后端能力

### 要拆开的职责

至少要识别并逐步拆开：

- camera 配置与类型定义
- camera 模式选择与参数校验
- camera 设备能力 / UVC / V4L2 相关操作
- ffmpeg / gstreamer / 录制命令拼装
- stereo session 业务协调

推荐结果不是“一个超大 camera 基类”，而是多个边界清楚的小组件。

实现文件粒度要求：

- 头文件边界可以细分
- `camera_recorder` 的实现文件总数以 `2-3` 个为宜
- 后续 `record_runtime` 的实现文件总数也以 `2-3` 个为宜
- 优先形成“入口壳 + 边界层 + 领域逻辑”这类克制的实现布局

### 测试策略

这一阶段要把“可测试逻辑”真正沉淀成单元测试。

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
- 如引入 backend interface，优先用 fake / stub 做无硬件测试

这一阶段不做：

- Docker ARM 构建
- ARM 板真实 camera 验证
- 为了测试而强行改造业务主流程

### 完成标准

- `camera_recorder` 不再把 camera 设备细节、pipeline 拼装和会话协调长期混在一个巨型实现里
- 新增或修改 camera 类型时，改动点明显收敛
- 至少一批 camera 相关纯逻辑已有单元测试
- 不引入业务语义变化

## Phase 7：脚本与 Python 收口

### 目标

优先治理最容易失控的外壳层。

### 顺序

#### Phase 7.1 `run_record.sh`

- 拆分日志、配置读取、路径访问、主流程编排

#### Phase 7.2 `auto_update/usb_auto_update.sh`

- 拆分 helper、配置解析、步骤编排

#### Phase 7.3 `auto_calibration/run_calibration.sh`

- 收口日志、路径、重复 helper

#### Phase 7.4 `audio/*.py`

- 至少完成日志和路径收口

### 完成标准

- 高风险脚本职责更偏编排
- 重复逻辑减少
- 行为等价 smoke 通过

### 测试策略

这一阶段是 `ugripper` 仓库内最需要做本地 smoke 的阶段。

测试环境：

- 本地 Linux

验证动作：

- Shell 语法检查：`bash -n`
- Python 语法检查：`python3 -m py_compile`
- 对支持 `--help`、`--dry-run` 的脚本跑最小命令
- 对不能真实运行的脚本至少检查路径、日志和流程分支是否完整
- 对主链外壳脚本做最小 smoke

建议按子步骤执行：

#### Phase 7.1 `run_record.sh`

- `bash -n run_record.sh`
- 检查关键路径和日志输出
- 如环境允许，做最小启动 smoke

#### Phase 7.2 `usb_auto_update.sh`

- `bash -n auto_update/usb_auto_update.sh`
- 检查配置解析与日志分支
- 对能 mock / dry-run 的分支做最小验证

#### Phase 7.3 `run_calibration.sh`

- `bash -n auto_calibration/run_calibration.sh`
- 检查路径、日志、调用链是否完整

#### Phase 7.4 `audio/*.py`

- `python3 -m py_compile audio/*.py`
- 检查日志前缀和路径访问
- 对支持最小运行的脚本做非硬件依赖分支验证

这一阶段在 `ugripper` 仓库内不做：

- 板端真实音频验证
- 真实升级流程验证
- 真实校准流程验证

这些验证后移到主仓库 / ARM 阶段。

## Phase 8：打包边界收口

### 目标

让打包从“排除式收包”走向“显式清单”。

### 重点

- 为主包建立最小 manifest
- 默认不带工具 / 测试 / 冻结区
- 需要随包的例外文件显式声明

### 测试策略

这一阶段主要做“本地打包可读性和收包正确性”验证。

测试环境：

- 本地 Linux

验证动作：

- 运行现有 `build_deb.sh -q` 或等价轻量打包命令
- 检查收包内容是否符合 manifest
- 检查工具 / 测试 / 冻结区是否默认未被带入
- 检查例外显式声明文件是否确实进入包

这一阶段不做：

- Docker ARM 打包验证
- 板端安装验证

这些在并仓后再做集成 / 交付验证。

### 完成标准

- `build_deb.sh -q` 可继续工作
- 打包边界可读、可审计
- 不再主要依赖“大排除 + 手工补回”

## Phase 9：温和目录收口

### 目标

在不改业务语义的前提下，逐步把外围内容归位。

### 优先处理

- `pack_script/` 按职责拆分
- `py_script/` 和 `test/` 归到工具/测试落点
- `ASR/`、`time_sync/` 做 freeze

### 测试策略

这一阶段主要验证“目录整理是否误伤主链和默认交付边界”。

测试环境：

- 本地 Linux

验证动作：

- 核对主链入口和关键路径仍然有效
- 检查主文档是否仍描述正确入口
- 检查默认打包是否没有重新捞入冻结区
- 检查移动后的工具和测试脚本仍能按新路径找到

这一阶段不做：

- Docker ARM 构建
- ARM 板运行
- service 新路径切换

### 完成标准

- 主链和外围内容的认知边界清晰
- 默认打包、主文档、主服务都不再误依赖冻结区

## 5. 推荐执行顺序

1. 基线冻结
2. 最小 `src/utils`
3. C++ 日志统一
4. 路径台账与路径封装
5. 低风险 helper 收口
6. camera 抽象收口
7. `run_record.sh` / `usb_auto_update.sh` / `run_calibration.sh` / `audio/*.py` 收口
8. 打包边界收口
9. 温和目录收口

## 6. 推荐 PR 顺序

1. 文档与 baseline 清单 PR
2. 最小 `src/utils` PR
3. C++ 日志统一 PR
4. 路径台账 / 路径 helper PR
5. 低风险 helper 收口 PR
6. camera 抽象收口 PR
7. `run_record.sh` / `usb_auto_update.sh` / `run_calibration.sh` / `audio/*.py` 收口 PR
8. manifest / 打包收口 PR
9. 目录温和收口 PR

## 7. 测试与门禁策略

测试必须按仓库阶段和目标分层理解，不能混为一谈。

### 7.1 三类门禁

- `重构门禁`
  - 发生在 `ugripper` 仓库内
  - 目标是确认重构过程可控，没有把现有行为明显改坏
- `集成门禁`
  - 发生在主仓库迁移阶段
  - 目标是确认改动能进入主仓库构建体系
- `交付门禁`
  - 发生在 ARM 板验证阶段
  - 目标是确认产物能在真实设备上启动和运行

三者不是一回事。

### 7.2 `ugripper` 仓库阶段的测试要求

每个 PR 只要求做本地 Linux 验证：

- 本地编译
- 单元测试或等价无硬件测试
- Shell 语法检查
- Python 语法检查
- 最小运行 / `--help` / `--dry-run`
- 日志检查
- 路径检查
- 基本 smoke

规则：

- `ugripper` 仓库 PR 不要求 ARM 验证
- 不要在 `ugripper` 阶段试图把 ARM 问题一次性验证完
- 不要只靠 `ugripper` 本地 Linux 就宣布“跨仓库迁移已经安全”

### 7.3 主仓库阶段的测试要求

任何进入主仓库的迁移节点，都按以下顺序验证：

1. 主仓库本地编译
2. 主仓库 Docker ARM 交叉编译
3. ARM 板 smoke

规则：

- 凡是涉及运行时、路径、脚本、交付的改动，进入主仓库后必须做 ARM smoke
- ARM smoke 只在节点做，不在每个小 PR 做

### 7.4 ARM 板 smoke 分级

为避免每次上板都做同样重的验证，ARM smoke 固定分成三档。

#### `L1` 轻量 smoke

适用于：

- 日志迁移
- 低风险 helper
- 轻量路径改动
- 轻量 camera 抽象内部整理

检查项：

- 程序能起
- 日志正常
- 无明显报错

#### `L2` 主链 smoke

适用于：

- `run_record.sh`
- `record_runtime`
- `camera_recorder` 关键路径调整
- 音频脚本
- 配置路径

检查项：

- 主链能起
- 关键资源能找到
- 录制流程基本正常

#### `L3` 交付 smoke

适用于：

- 打包改动
- service 改动
- 安装路径改动
- 更新脚本
- 校准脚本

检查项：

- 包能装
- service 能起
- 更新 / 校准基本流程能跑

## 8. 完成标志

当以下条件满足时，可以认为 `ugripper` 已达到“可合并重构完成”的状态：

- 基线文档齐全且可审计
- 最小 `src/utils` 已建立
- 主链 C++ 日志入口统一
- 关键硬编码路径已收口
- camera 抽象边界已建立
- 高风险脚本已拆层
- 关键纯逻辑已有单元测试或等价无硬件测试
- 打包边界已显式化
- 主链 / 工具 / 部署 / 冻结区边界清楚
- `camera_recorder` 与 `record_runtime` 的职责边界已清楚，且实现文件粒度没有失控
- 日志调用入口已对齐 `pp_main` 风格，后续只需在主仓库切日志后端
- 多线程 / 周期任务边界已可测试，且未在当前阶段强行引入重型调度基础设施
- 现有 pipe / file 控制面已具备后续迁往 ZMQ 的分阶段收口基础

达到这个状态后，再进入 `standalone integration`，风险会显著低于直接并仓。
