---
name: add-feature
description: 为 ugripper 项目执行新增功能（add feature）类需求的标准工作流。用于用户提出“add feature / 加功能 / 新增功能”等请求时：先阅读 docs/agent/overview.md 理解当前实现，再先给实现计划并等待确认，然后以 heredoc 方式修改代码，并同步创建或维护 docs 文档；收尾阶段默认沿用 build 脚本里的现有版本打包，只有用户明确要求时才更新 build_deb.sh 与 usb_updater_build.sh 版本号，并默认联动 auto-release-deb 完成建议编译与自动安装。
---

# add-feature

## 固定执行顺序

1. 先阅读 `docs/agent/overview.md`，再开始设计方案。
2. 输出“实现计划 + 影响文件列表 + 文档同步计划”，进入等待开工状态。
3. 只有收到用户**明确的确认并开始指令**后，才开始改代码与文档。
4. 代码改完后，先确认用户是否明确要求修改版本号；未明确要求时，默认沿用当前脚本版本。
5. 功能修改确认后，默认调用 `auto-release-deb`（由该 skill 完成判定、编译与自动安装）。
6. 编译/安装完成后，再给最终汇报。

## 计划反馈阶段处理（强约束）

1. 给出计划后，默认状态为 `WAIT_FOR_EXPLICIT_START`。
2. 在该状态下，用户提出修改意见、补充约束、质疑、讨论细节时，只能更新计划并继续等待，**不得**开始任何代码/文档落地修改。
3. 仅以下类型指令可解除等待状态并开工：`确认并开始`、`开始实现`、`按该计划执行`、`可以开改了`（或同等明确语义）。
4. 若用户表达模糊（如“先这样”“看起来可以”），先追问一次“是否现在开始实现”，未得到明确开工指令前保持等待。

## 代码修改规则

1. 修改范围保持最小化，只实现当前 feature，避免顺手重构。
2. 所有改动完成后，汇总可追踪 diff（按文件说明改动点）。

## 文档同步规则（docs）

1. 每次 feature 修改后，必须同步创建或维护相关文档。
2. 默认至少检查并更新 `docs/agent/overview.md` 中受影响的条目。
3. 当流程、模块边界、运行时架构变化时，统一更新 `docs/agent/overview.md` 对应章节。
4. 每次 feature 修改后，都要维护 `docs/CHANGELOG.md`，内容聚焦“软件功能改动”，避免写构建过程流水；条目尽量简洁。
5. 仅在确有新增知识且无法放入现有文档时，新增文档；避免文档碎片化。

## 版本号决策与修改规则（新增）

1. 版本字段：
   - `build_deb.sh` 的 `VERSION`（ugripper 主包）
   - `usb_updater_build.sh` 的 `PKG_VERSION`（ugripper-usb-updater）
2. 决策优先级：
   - 用户明确指示 > 默认不修改。
3. 默认行为（用户未明确要求改版本号时）：
   - 不修改任何版本字段。
   - 直接沿用 `build_deb.sh` / `usb_updater_build.sh` 里的当前版本执行打包。
4. 只有在用户明确要求“更新版本号 / bump 版本 / 发新版本”时，才执行版本修改。
5. 升级级别：
   - 默认 `.z`（patch）。
   - 跨模块功能扩展、需要运维侧额外关注时可升 `.y`（minor）。
   - 明确不兼容变更才升 `.x`（major）。
   - 无充分依据时，一律回落 `.z`。
6. 修改方式（沿用原 auto-release-deb 的 semver 规则）：
   - `bash .codex/skills/add-feature/scripts/bump_release_versions.sh --target ugripper --bump .z`
   - `bash .codex/skills/add-feature/scripts/bump_release_versions.sh --target updater --bump .z`
   - `bash .codex/skills/add-feature/scripts/bump_release_versions.sh --target both --bump .y`
   - 不改版本号：直接说明“沿用当前脚本版本打包”，无需调用 bump 脚本。
7. 汇报要求：
   - 必须说明“改/不改版本号”的决策理由。
   - 若有修改，必须给出每个脚本的旧版本 -> 新版本。
   - 若未修改，必须明确说明“沿用的脚本版本分别是什么”。
8. 构建范围判定与编译执行统一由 `auto-release-deb` 负责；`add-feature` 不在内部重复判定或重复编译。
9. 安装执行也统一由 `auto-release-deb` 负责；`add-feature` 不在内部重复执行安装命令。

## 发布与编译联动规则（新增）

1. 在 feature 改动落地并确认后，默认执行：
   - `bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh`
2. `add-feature` 只消费 `auto-release-deb` 的判定与执行结果，不再自行执行任何构建或安装命令。
3. 若判定 `scope=none`，则不执行编译/安装并在汇报中说明原因。
4. 仅当用户明确要求“只判定不编译 / 跳过编译 / 跳过安装”时，允许不执行对应步骤。
5. 任何编译失败都要汇报失败命令与关键错误，并停止后续步骤，等待用户指示。
6. 构建成功后默认继续安装对应新包；若安装失败，同样要汇报失败命令与关键错误，并停止后续步骤，等待用户指示。

## 验收与检查默认策略

1. 默认不执行重测试（集成/全量回归），由用户手动检查。
2. 默认可执行轻量静态检查（例如 shell/python 语法检查）。
3. 若用户要求测试，且改动涉及录制链路、视频/传感器时间轴、episode 校验或 `info.json` 时间偏移语义，测试内容必须覆盖：
   - 传感器和视频时间戳对齐是否正常。
   - 时间戳是否单调、时序是否稳定、无明显乱序/倒退。
   - 名义间隔附近是否存在异常 `gap`、大时间洞或缺样。
   - 启动录制阶段传感器 `offset`、`boot_time_offset(_us)`、`<camera>_record_time_offset_us` 或等价结果是否异常。
   - 优先复用仓库内现有脚本、episode 校验输出和日志，而不是临时发明一套新口径。
4. 只有用户明确要求“测试”时，才执行更完整的测试并回报结果。

## 输出格式要求

最终回复按以下顺序输出：

1. 代码改动摘要（按文件）。
2. 文档改动摘要（按文件）。
3. 版本号决策与结果（含旧版 -> 新版，或不修改原因）。
4. auto-release-deb 判定结果与实际执行的编译/安装命令（含成功/失败）。
5. 已执行检查与未执行测试说明。
6. 建议的手动验收步骤。
