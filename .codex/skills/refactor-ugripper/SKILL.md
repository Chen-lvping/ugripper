---
name: refactor-ugripper
description: 为 ugripper 项目执行重构（refactor）、收口、并仓准备类需求的标准工作流。用于用户提出“重构 / 收口 / 整理结构 / 合并到 standalone / 并仓准备 / 抽 utils / 统一日志 / 路径治理 / 拆脚本”等请求时：先阅读 docs/archive/2026-refactor-history/ 下的历史规划文档与相关 baseline/reference 文档，确认当前阶段边界，再输出“阶段定位 + 影响范围 + 风险点 + 测试分层 + 执行顺序”；只有在用户明确要求开始后，才落代码。默认优先做 ugripper 仓库内的可合并重构，不提前切主仓库安装路径、service 或最终 standalone 落位。
---

# refactor-ugripper

## 适用场景

当用户需求属于以下任一类时，优先使用本 skill：

- 重构、收口、整理模块边界
- 为并入主仓库 `standalone` 做准备
- 抽公共 helper / 建最小 `src/utils`
- 建立 camera 抽象边界
- 统一日志入口或日志格式
- 路径台账、路径封装、硬编码路径治理
- 拆 `run_record.sh`、`usb_auto_update.sh`、`run_calibration.sh`
- 打包边界收口、目录温和收口
- 为重构补单元测试或等价无硬件测试

以下场景不应优先使用本 skill：

- 纯新增功能：改用 `add-feature` 或 `test-new-feature`
- 只读故障排查：改用 `debug-ugripper`
- 仅管理 TODO / worktree / 子会话：改用 `todo-hub`

## 固定执行顺序

1. 先阅读以下文档建立上下文：
   - `docs/agent/overview.md`
   - `docs/agent/current-status.md`
   - `docs/archive/2026-refactor-history/repo-migration-map.md`
   - `docs/archive/2026-refactor-history/ugripper-refactor-plan.md`
   - `docs/archive/2026-refactor-history/standalone-merge-plan.md`
   - `docs/archive/2026-refactor-history/reference-aligned-refactor-next-steps.md`
2. 若需要追溯旧 baseline 冻结事实，按需补读：
   - `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
   - `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
   - `docs/archive/2026-refactor-history/baseline/package-contents.md`
   - `docs/archive/2026-refactor-history/baseline/service-map.md`
   - `docs/archive/2026-refactor-history/baseline/config-entrypoints.md`
   - `docs/archive/2026-refactor-history/baseline/deploy-script-boundaries.md`
   - 若需要追溯旧的 host-Linux 等价验证口径，再读 `docs/archive/2026-refactor-history/baseline/equivalence-checklist.md`
3. 若当前工作已经到 `A3` 之后，或需求涉及 testing / camera / naming-YAML / runtime process-thread 收口，继续补读以下 4 份 reference 文档，并视为 `still authoritative reference input`：
   - `docs/archive/2026-refactor-history/ppmain-ugripper-test-refactor-reference.md`
   - `docs/archive/2026-refactor-history/ppmain-ugripper-camera-refactor-reference.md`
   - `docs/archive/2026-refactor-history/ppmain-ugripper-naming-yaml-refactor-reference.md`
   - `docs/archive/2026-refactor-history/ppmain-ugripper-process-thread-refactor-reference.md`
4. 若需求还涉及以下任一项，继续按需补读主仓库参照实现，只读到足以约束当前方案为止：
   - logger 对齐：`/home/songwl/swl_ws/pp_main/src/utils/logger.h`
   - 任务调度 / 生命周期：`/home/songwl/swl_ws/pp_main/src/utils/task_scheduler.h`、`/home/songwl/swl_ws/pp_main/src/utils/task_scheduler.cc`
   - ZMQ / message-hub 迁移方向：`/home/songwl/swl_ws/pp_main/src/communication/` 与 `standalone/Puppetry/communication/`
5. 先判断当前需求落在哪个阶段或步骤：
   - `Phase 0` 架构建模 / 计划对齐 / reference 冻结
   - `A1` 基线冻结
   - `A2` 最小 `src/utils`
   - `A3` C++ 日志统一
   - `Post-A3 Step 1` Reference Freeze And Traceability Alignment
   - `Post-A3 Step 2` 正式测试入口最小落地
   - `Post-A3 Step 3` 路径台账与真源目录冻结
   - `Post-A3 Step 4` Naming / YAML Contract Freeze
   - `Post-A3 Step 5` Sensor Common Logic Extraction
   - `Post-A3 Step 6` Camera Pure Logic Extraction
   - `Post-A3 Step 7` Runtime Process Boundary Refactor
   - `Post-A3 Step 8` Runtime Domain Split
   - `Post-A3 Step 9` Scripts And Field-Test Boundary Cleanup
   - `Stage B` standalone integration / packaging / service / install-path switch
6. 输出“阶段定位 + 影响文件 + 风险点 + 测试策略 + 是否需要主仓库/ARM 后续验证”，进入等待开工状态。
7. 只有收到用户**明确的确认并开始指令**后，才开始修改代码/文档。
8. 若本次修改了代码文件，最终汇报前必须运行 `graphify update .`，用于 AST-only 增量更新项目知识图谱，不触发语义/API 抽取；若本地 graphify 版本不支持该别名，则运行等价的 `graphify . --update`。
9. 落地修改后，只执行与当前阶段匹配的最小验证，不越级扩大测试范围。

## 阶段边界规则

### 第一阶段默认边界

默认优先做 `ugripper` 仓库内的可合并重构，不提前做：

- `standalone/<AppName>/` 最终落位
- 主仓库安装前缀切换
- service 新路径切换
- 最终主仓库打包形态切换

若用户要求提前做上述内容，必须在计划中显式标出“已跨入并仓阶段”。

### A3 之后的默认执行顺序

当 `A1-A3` 已完成时，后续不再默认沿用旧的 `A4/A5/A6` 线性顺序。

默认改为遵循 `docs/archive/2026-refactor-history/reference-aligned-refactor-next-steps.md` 的 9 步顺序：

1. Reference Freeze And Traceability Alignment
2. 正式测试入口最小落地
3. 路径台账与真源目录冻结
4. Naming / YAML Contract Freeze
5. Sensor Common Logic Extraction
6. Camera Pure Logic Extraction
7. Runtime Process Boundary Refactor
8. Runtime Domain Split
9. Scripts And Field-Test Boundary Cleanup

除非用户明确要求偏离，或本地现状已证明该顺序不再适用，否则不要跳回旧顺序推进。

### 第二阶段提示

涉及以下事项时，必须在计划里明确提示“这部分需要主仓库阶段继续验证”：

- Docker ARM 交叉编译
- ARM 板 smoke
- 安装后真实路径验证
- service / 打包 / 安装路径切换

## 计划反馈阶段处理（强约束）

1. 给出计划后，默认状态为 `WAIT_FOR_EXPLICIT_START`。
2. 在该状态下，用户提出修改意见、补充约束、质疑、讨论顺序时，只能更新计划，**不得**开始代码或文档落地修改。
3. 仅以下类型指令可解除等待状态并开工：`确认并开始`、`开始执行`、`按这个计划做`、`可以开改了`（或同等明确语义）。
4. 若用户表达模糊（如“先这样”“看起来可以”），先追问一次“是否现在开始实施”。

## 实施规则

### 1. 一次只做一个阶段目标

每次修改必须明确只服务于当前阶段目标，例如：

- 日志统一 PR 只改日志
- helper 收口 PR 只改 helper 归属
- camera 抽象 PR 只改 camera 职责边界与测试
- 路径 PR 只改路径台账与访问方式
- 脚本 PR 只做拆层与低风险收口

不得在一个 PR 中混做：

- 日志替换
- helper 抽取
- 行为修复
- 路径切换

### 1.0 图谱维护

每次重构落地后，只要实际修改了代码文件，就必须在最终汇报前运行 `graphify update .`，保持 `graphify-out/` 中的项目图谱同步。

- 该步骤只作为 AST-only 增量更新，不应触发语义/API 抽取。
- 若本地 graphify 版本不支持 `graphify update .` 别名，使用等价的 `graphify . --update`。
- 若执行失败，不回滚业务修改，但必须在最终汇报中说明失败命令和关键错误。

### 1.1 关键测试门禁

每一步进入下一步前，都必须先判断本步的关键测试是否通过。

默认规则：

- 本步计划中定义的关键测试未通过，不进入下一步
- 若关键测试未执行，必须在 `docs/REFACTOR_LOG.md` 明确记录原因
- 只有存在显式记录的例外说明时，才允许带着未完成项进入下一步

这条规则适用于：

- `A1-A3`
- `Post-A3 Step 1-9`
- 后续 `standalone integration` 阶段

### 1.2 可读性 / 可维护性必须显式改善

重构不能只做“代码搬家”。

每次重构至少应显式改善一项：

- 文件或类职责更单一
- 巨型函数被拆开
- 隐式副作用减少
- 命名 / 日志 / 错误路径更一致
- 新人更容易定位 camera / 路径 / 配置 / 脚本入口

若没有带来理解成本下降，就不算高质量重构。

### 2. 最小 `src/utils` 约束

第一阶段只允许引入“被动工具”，例如：

- logger
- file/path utils
- env/config 小工具
- 时间函数
- 字符串处理
- 必要时的 single process helper

第一阶段不应引入：

- 重型 config manager
- yaml/parser 体系化改造
- task scheduler
- 带明显副作用的通用执行封装
- 主动流程控制类

并且必须满足：

- `src/utils` 可以被业务模块依赖
- `src/utils` 不能反向依赖业务模块

### 2.1 C++20 默认约束

默认对齐主仓库 `pp_main` 的 `C++20` 基线：

- 新建或改造 C++ 模块时，默认保持 `C++20`
- 不主动新增新的 `C++17` 例外
- 若发现交叉编译链、第三方依赖或板端工具链不接受 `C++20`，必须在计划和重构日志中显式记录

### 2.2 logger 对齐约束

凡是涉及日志体系、日志替换、日志抽象或并仓前日志清理时，必须遵守：

- 当前 `ugripper` logger 视为过渡实现
- 当前阶段优先对齐 `DM_LOG_INIT`、`DM_LOG_*`、`DM_LOG_*_STREAM()` 的 API 形态与调用语义
- 并仓后的真实目标是 `/home/songwl/swl_ws/pp_main/src/utils/logger.h`
- 若发现 `pp_main` 现有 logger 无法承载当前需求，必须先列出差异和缺口，不能直接在 `ugripper` 内自行发明第四套日志接口

### 3. 路径治理约束

路径治理必须拆成两步：

1. 先做路径台账
2. 再做路径 helper / 路径封装与替换

第一阶段的目标只是消灭散落硬编码，不是切主仓库安装路径。

并且要明确区分：

- `ugripper` 阶段只验证“本地路径解析逻辑是否统一”
- 主仓库阶段才验证“安装后的真实路径是否正确”

在第一阶段，不要把 helper 提前写死为主仓库安装前缀或绝对安装路径。

### 3.1 Naming / YAML 契约约束

当需求进入 `Post-A3 Step 4` 及之后步骤时，必须遵守：

- 先冻结命名规则和 YAML schema，再做迁移
- 不先做全仓大规模 rename
- 不先做一次性旧字段清理
- 旧的扁平 camera YAML 字段视为 `deprecated but accepted`
- `Step 4` 只冻结契约和迁移边界，不新增 warning
- `Step 6` 开始允许增加一次性 deprecated warning
- 真正禁止旧格式，必须作为单独、可记录、可回退的后续动作推进

还需默认记住：

- `schema_version: 1` 是第一阶段收口目标
- `output_files[0]` 是主链保证的 primary output
- 旧格式兼容边界必须写进文档和测试，而不是只留在实现猜测里

### 4. Camera 抽象约束

当需求涉及 `camera_recorder` 或 camera 设备处理时，必须遵守：

- 参考 `/home/songwl/swl_ws/pp_main/src/device/cameras` 的设计目标，不直接照搬代码
- 优先拆开 camera 配置、camera 设备能力、命令拼装、stereo 业务协调
- 在 `Post-A3 Step 6` 默认优先拆：
  - `camera_types`
  - `camera_config`
  - `camera_command_builder`
  - `camera_registry`
- 不为了抽象而抽象，不强行复刻完整设备访问层
- 优先为可拆出的纯逻辑补单元测试或 fake/stub 测试
- 头文件数量不设硬限制，但 `camera_recorder` 的实现文件总数默认控制在 `2-3` 个
- 小体量纯逻辑实现优先并入少数职责清晰的 `.cpp`，不要把每个概念都拆成单独实现文件

### 4.1 Runtime 拆分约束

当需求涉及 `record_runtime` 拆分或控制面收口时，必须遵守：

- 当前阶段保留 supervisor 多进程模型
- 并入 standalone 后仍默认保留 `record_runtime` 作为 supervisor 主控
- 不把 `camera_recorder` / `sensor_recorder` / audio / stereo daemon 直接合成单进程
- `Post-A3 Step 7` 优先抽：
  - `SubprocessHandle`
  - `ProcessSupervisor`
  - `AudioCoordinator`
  - `StereoSessionClient`
- `Post-A3 Step 8` 再拆：
  - `HmiController`
  - `HealthMonitor`
  - `RecordingOrchestrator`
  - `RuntimeApp`
- 头文件数量不设硬限制，但 `record_runtime` 的实现文件总数默认控制在 `2-3` 个
- 若某块逻辑体量很小，优先并入 `runtime_process` 或 `runtime_domain` 一类归并实现文件，而不是继续新增碎片化 `.cpp`

一旦这些模块已存在，新增逻辑不得继续回流到 `record_runtime.cpp`。

### 4.2 调度与线程边界约束

凡是涉及线程模型、周期任务、轮询或 lifecycle 收口时，必须遵守：

- 参考 `pp_main` `TaskScheduler` 的生命周期组织方式，但当前阶段不直接移植其实现
- 不在 `ugripper` 当前阶段引入 `oneTBB`、全局单例调度器或新的重型基础设施依赖
- 先形成局部可测试的周期任务 / 停止语义边界，再决定后续如何映射到主仓库能力

### 4.3 通信演进约束

凡是涉及控制面通信、进程间协调、状态通道或并仓后通信形态时，必须遵守：

- 长期目标是尽量向 `pp_main` 的 ZMQ / message-hub 方式靠拢
- 当前阶段先识别 transport 无关的接口和通道职责
- pipe / file 仍可作为当前默认实现
- ZMQ 后端的引入和切换必须作为独立、可回退、可验证的后续阶段推进

### 5. 高风险脚本约束

高风险脚本优先关注：

- `run_record.sh`
- `auto_update/usb_auto_update.sh`
- `auto_calibration/run_calibration.sh`
- `audio/*.py`

默认先做：

- 日志收口
- 路径收口
- helper 收口

未有充分证据前，不要先大拆业务流程。

## 测试分层规则

### ugripper 仓库阶段

只做本地 Linux 重构验证：

- 本地编译
- host-only 单元测试或等价无硬件测试
- shell 语法检查
- python 语法检查
- 最小运行 / `--help` / `--dry-run`
- 日志检查
- 路径检查
- 基本 smoke

默认测试框架与分层规则：

- host-only 正式单元测试默认统一使用 `GoogleTest`
- 涉及硬件、设备节点、真实外部进程依赖的测试，不进入 host-only 单测集合
- field script 继续存在，但归类为 field test，不冒充正式单元测试
- 主仓库 Docker ARM 构建和板端 smoke 属于后续集成/交付验证，不在 `ugripper` 阶段强行完成

### 主仓库阶段

以下内容必须在计划里标记“后续需主仓库验证”：

- 主仓库本地编译
- Docker ARM 交叉编译
- ARM 板 smoke

### ARM 板 smoke 分级提示

可按 `docs/archive/2026-refactor-history/ugripper-refactor-plan.md` 中的分级提示用户：

- `L1` 轻量 smoke
- `L2` 主链 smoke
- `L3` 交付 smoke

## 文档同步规则

每次重构类修改后，至少检查并按需更新：

- `docs/archive/2026-refactor-history/ugripper-refactor-plan.md`
- `docs/archive/2026-refactor-history/standalone-merge-plan.md`
- `docs/archive/2026-refactor-history/reference-aligned-refactor-next-steps.md`
- `docs/agent/current-status.md`
- `docs/archive/2026-refactor-history/repo-migration-map.md`
- `docs/REFACTOR_LOG.md`

若进入 `Phase 1` 或路径 / service / 打包边界更新，还应同步：

- `docs/archive/2026-refactor-history/baseline/*`

当当前系统行为口径发生变化时，再更新：

- `docs/agent/overview.md`
- `docs/CHANGELOG.md`

重构日志与变更日志分工如下：

- `docs/REFACTOR_LOG.md`：默认记录每次重构的内部结构变化、阶段推进、风险与验证情况
- `docs/CHANGELOG.md`：仅在产生对外可感知的软件行为变化时更新

## 输出格式要求

计划阶段默认按以下顺序输出：

1. 当前需求对应的阶段定位
2. 影响文件范围
3. 关键风险点
4. 本地验证策略
5. 是否需要主仓库 / Docker / ARM 后续验证
6. 等待用户明确开工确认

实施完成后的最终回复按以下顺序输出：

1. 代码改动摘要（按文件）
2. 文档改动摘要（按文件）
3. graphify 增量更新结果（若本次未修改代码，说明未执行原因）
4. 已执行的本地验证
5. 未执行的验证与原因
6. 后续需要在主仓库 / ARM 阶段继续验证的内容

若本次完成了某个阶段或子步骤，还必须额外提醒：

1. 当前完成的是哪一步
2. 该步骤已完成的测试
3. 该步骤未完成的测试与原因
4. 是否建议进入下一步

若关键测试未通过或未执行，还必须明确说明：

5. 当前是否满足进入下一步的门禁条件
