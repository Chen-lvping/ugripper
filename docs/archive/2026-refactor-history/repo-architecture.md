# Repo Architecture Draft

本文档用于定义 `ugripper` 仓库的重构边界、模块分层、迁移顺序与验收标准。
它不是“理想目录树展示”，而是面向当前仓库现实状态的渐进式重构约定。

## 1. 文档目标

本次重构优先解决 4 个问题：

- 明确哪些内容属于产品主链，哪些只是部署、工具或历史残留。
- 把“目录职责”和“是否随包交付”拆开，避免目录位置直接决定打包结果。
- 先收口边界和交付清单，再做目录迁移和模块拆分。
- 在不改变业务语义的前提下，逐步把仓库变成易于维护、易于 review 的形态。

本次重构暂不追求：

- 一次性改成 `apps/`、`libs/`、`deploy/` 的理想目录树。
- 一次性拆完所有大文件。
- 一次性清空所有历史内容。

## 2. 当前仓库分层

当前仓库按职责可分为 5 类。

### 2.0 真源目录与非真源目录

在当前阶段，先把“目录是不是当前正式真源”说清楚，再讨论迁移。

最核心的实现真源目录是：

- `standalone/`
- `src/utils/`
- `src/third_party/`
- `config/`
- `docs/`
- `test/`

这 4 类目录分别承担：

- `standalone/`：四个主链 app 的 C++ 实现真源
- `src/utils/`：当前仍在复用的公共工具层真源
- `src/third_party/`：当前主构建仍在使用的第三方接入层
- `config/`：配置和标定基线真源
- `docs/`：计划、baseline 和变更口径真源
- `test/`：正式单测与 field test 资产真源

同时，下列目录虽然不属于“核心实现真源”，但仍是当前正式交付或资源真源：

- `audio/`
- `audio_en/`
- `auto_update/`
- `auto_calibration/`
- `pack_script/`
- `py_script/`（仅工具真源，不默认视为产品主链）

下列目录明确不是当前真源：

- `build/`：构建产物
- `tmp/`：运行/临时产物
- `.local-deps/`：本地验证依赖缓存

下列目录视为冻结候选，不再作为当前主链真源：

- `ugripper/`：嵌套历史副本
- `ASR/`
- `time_sync/`

执行规则：

- 代码、路径、配置重构默认只改真源目录。
- `build/`、`tmp/`、`.local-deps/` 只能作为事实来源，不作为重构落点。
- `ugripper/` 停止继续叠加主链逻辑，后续先断引用再决定归档或删除。

### 2.1 产品主链

这些内容直接参与 `ugripper.service` 主运行链路，应作为第一等公民维护：

- `standalone/UgripperRuntime`
- `standalone/CameraRecorder`
- `standalone/SensorRecorder`
- `standalone/GripperHmiTool`
- `src/utils`
- `src/third_party/mcap_builder`
- `run_record.sh`
- `auto_update/`
- `auto_calibration/`
- `config/`
- `audio/`
- `audio_en/`

说明：

- `audio/` 当前是混合目录，既包含运行时代码，也包含运行时资源和共享调优资产，因此 Phase 1 不做物理迁移，只先做职责标注。
- `audio_en/` 当前是运行时资源目录，和 `audio/` 有强耦合；Phase 1 不单独拆分。

### 2.2 部署与交付

这些内容不属于产品业务逻辑，但决定系统能否正确安装、启动和升级：

- `pack_script/`
- `build_deb.sh`
- `usb_updater_build.sh`

说明：

- `pack_script/` 是“来源目录”，不是最终目标结构。后续迁移时按文件职责拆分，不整目录搬迁。

### 2.3 工具与现场辅助

这些内容默认不属于主链，但可能被开发、排障、回归或现场人员显式执行：

- `py_script/`
- `test/`

说明：

- 工具默认不进主包。
- `test/` 当前已是正式测试真源的一部分，其中 `test/src/` 用于 host-only 单测，`test/scripts/` 用于 field test。
- 如果个别工具必须随包交付，必须通过 `manifest` 显式声明，而不是依赖目录位置。

### 2.4 冻结区候选

这些内容目前没有进入当前主链的明确证据，应先做 `freeze`，而不是立刻删掉或归档：

- `ASR/`
- `time_sync/`

说明：

- `freeze` 的含义是：不再作为当前能力继续演进，不进入默认打包，不允许新增主链引用，但暂时保留目录与文件。
- 只有在完成断引用检查和 smoke 验证后，才考虑迁入 `archive/`。

### 2.5 文档区

这些内容负责定义当前口径、变更记录和重构计划：

- `docs/agent/overview.md`
- `docs/CHANGELOG.md`
- `docs/todo_list.md`
- `docs/repo-architecture.md`
- `docs/archive/2026-refactor-history/repo-migration-map.md`

## 3. 重构原则

### 3.1 先分层，再美化命名

Phase 1 保留当前 `src/` 框架，不急于改成 `apps/` / `libs/`。

优先做的事：

- 给模块明确职责标签。
- 给打包建立显式交付清单。
- 把工具、部署、冻结内容从认知上收口。

后续若 Phase 1 到 Phase 3 稳定，再评估是否值得继续做 `src -> apps/libs`。

### 3.2 目录表达职责，manifest 决定交付

后续仓库需要遵守两条并行规则：

- 目录只表达“它是什么”。
- `manifest` 决定“它是否默认进入产品包”。

禁止继续依赖下列隐式规则：

- “放在仓库根目录所以会被打包”
- “放在 `py_script/` 所以一定不打包”
- “放在 `src/` 所以一定是产品主链”

### 3.3 先 freeze，再 archive

对于历史内容或可疑残留内容，处理顺序固定为：

1. 搜索主链引用
2. 从默认打包和当前文档中去主链化
3. 标记为 `frozen`
4. 做一次行为等价 smoke 验证
5. 再决定是否迁入 `archive/`

### 3.4 Shell 只做流程编排

Shell 脚本允许保留流程控制，但以下内容必须逐步下沉：

- 重复出现的日志和错误处理
- 配置文件读写
- 挂载状态判断
- 服务启停与清理逻辑
- 独立步骤的业务判断

目标是把大脚本拆成：

- 一个主控脚本
- 若干 `lib/*.sh`
- 若干 `steps/*.sh`

### 3.5 先做行为等价迁移，不做业务语义变化

目录迁移、路径迁移和职责收口阶段，不应顺手引入新的业务行为。

允许做的变更：

- 路径修复
- 文件归位
- 打包边界收口
- 逻辑分区

不应在同一批 PR 混入：

- 状态机语义调整
- 录制行为变化
- 新配置项
- 新硬件兼容逻辑

### 3.6 先统一职责，再考虑统一格式

代码格式化、lint 收口、风格统一，应排在模块边界稳定之后。
避免出现“格式变更 + 目录迁移 + 逻辑拆分”混在一个 PR 里的情况。

## 4. 依赖方向规则

为防止仓库再次变脏，后续必须满足以下依赖方向：

- 产品主链应用可以依赖主链公共库。
- 主链公共库不能反向依赖主链应用。
- 运行时脚本可以调用产品二进制、运行时资源和配置。
- 更新与校准脚本可以调用产品二进制和部署资源，但不能依赖工具目录。
- 工具目录不能被产品运行时直接调用。
- 冻结区和归档区不允许被主链引用。
- 文档不作为运行时依赖。

结合当前仓库，对应为：

- `standalone/UgripperRuntime` 可以依赖 `standalone/GripperHmiTool`
- `standalone/GripperHmiTool` 不能反向依赖 `standalone/UgripperRuntime`
- `run_record.sh`、`auto_update/`、`auto_calibration/` 可以调用主链二进制
- `py_script/` 和 `test/` 不应被 `ugripper.service` 主链依赖
- `ASR/`、`time_sync/` 不应再新增主链引用

## 5. Phase 1 目标结构

Phase 1 不追求“最终理想目录”，只做温和收口。

建议逐步建立但不强推一次性落地的目录：

- `tools/`
- `deploy/`
- `configs/`
- `scripts/`
- `frozen/`

Phase 1 的关键点：

- `standalone/UgripperRuntime`
- `standalone/CameraRecorder`
- `standalone/SensorRecorder`
- `standalone/GripperHmiTool`
- `src/utils`

以上目录是当前主链真源。

## 6. 特殊模块规则

### 6.1 audio / audio_en

`audio/` 当前不是纯代码目录，也不是纯资源目录。
Phase 1 采用“职责标注，不急于物理拆分”的策略。

当前应按 3 类理解：

- 运行时代码：`audio/audio_play.py`、`audio/record_usb_audio.py`、音频工具模块
- 运行时资源：`audio/*.wav`、`audio_en/*.wav`
- 共享调优资产：`audio/noise.prof`

后续若要拆分，先改 `manifest` 和路径常量，再考虑物理迁移。

### 6.2 pack_script

`pack_script/` 不应整体平移到新目录。
后续按文件职责拆归属：

- `.service` -> `deploy/systemd/`
- `.rules` -> `deploy/udev/`
- `control/postinst/prerm/postrm` -> `deploy/deb/`
- 打包辅助 shell -> `scripts/package/`

### 6.3 ASR / time_sync

这两个目录先做 `freeze`：

- 不新增主链引用
- 不进入默认打包
- 在文档中标注“非当前主链”
- 完成断引用和 smoke 验证后，再决定是否迁入 `frozen/` 或 `archive/`

## 7. 重构阶段

### Phase 0：架构建模

目标：

- 定义模块标签
- 定义依赖方向
- 冻结主链清单

产物：

- `docs/repo-architecture.md`
- `docs/archive/2026-refactor-history/repo-migration-map.md`

完成标准：

- 所有一级目录都有明确角色
- 主链、工具、部署、冻结区候选都有文档定义

### Phase 1：manifest 收口

目标：

- 让打包由显式清单驱动
- 默认排除工具、测试和冻结区

建议的最小清单分类：

- `binaries`
- `scripts`
- `configs`
- `services`
- `udev`
- `assets`
- `optional-tools`

完成标准：

- `build_deb.sh -q` 仍可构建
- 是否随包交付由 manifest 显式控制
- 不再主要依赖“大排除 + 手工补回”的收包模式

### Phase 2：温和目录收口

目标：

- 不改业务语义
- 逐步建立 `tools/`、`deploy/`、`configs/`、`scripts/` 的落点

完成标准：

- 主服务可启动
- 录制链路 smoke 通过
- 打包结果与预期一致
- 关键路径修复完成

### Phase 3：冻结区处理

目标：

- 将 `ASR/`、`time_sync/` 去主链化
- 完成断引用验证

完成标准：

- 默认打包不再包含相关内容
- 主文档不再将其描述为当前能力
- 主链不依赖相关目录

### Phase 4：脚本拆分

目标：

- 优先拆 `auto_update/usb_auto_update.sh`
- 其次拆校准与打包脚本中的重复逻辑

完成标准：

- 主控脚本职责清晰
- 通用逻辑下沉到 `lib/*.sh`
- 重要 step 有统一返回码约定

### Phase 5：主链职责收口

目标：

- 先整理 `record_runtime` 内部职责
- 再决定最终文件边界

优先顺序建议：

- `episode_manager`
- `process_runner`
- `runtime_config`

完成标准：

- `record_runtime` 已按职责分区
- 至少拆出低耦合组件
- 不改变现有业务语义

## 8. PR 策略

建议按以下顺序拆 PR：

1. `PR1`：架构文档 + 模块映射表
2. `PR2`：manifest 骨架 + 打包收口
3. `PR3`：冻结区规则 + `ASR/time_sync` 断引用验证
4. `PR4`：温和目录收口与路径修复
5. `PR5`：`usb_auto_update.sh` 拆分
6. `PR6`：`record_runtime` 职责收口
7. `PR7`：配置统一、smoke test、lint 补齐

## 9. 验收标准

### 阶段 1 完成标准

- 所有一级目录都有标签：`product` / `deploy` / `tool` / `frozen`
- 主链清单冻结
- 架构文档和迁移映射表可被团队共同使用

### 阶段 2 完成标准

- `ugripper.service` 仍可启动
- `run_record.sh` 路径和运行时路径正确
- `deb` 可正常构建
- 核心录制链路 smoke 通过

### 阶段 3 完成标准

- `usb_auto_update.sh` 完成拆分
- `record_runtime` 完成职责收口
- 配置入口开始统一

### 阶段 4 完成标准

- 工具默认不进包
- 冻结区不再被主链引用
- 至少具备基础打包校验和主链 smoke 校验

## 10. 后续可选演进

只有当以下条件同时满足时，才建议评估 `src -> apps/libs`：

- manifest 已稳定
- 工具和冻结区已经从主链认知中分离
- 主链路径和打包路径已经稳定
- `record_runtime` 与更新脚本职责边界已明显清晰

如果以上条件尚未满足，继续推进理想化目录树只会放大迁移面和 review 成本。
