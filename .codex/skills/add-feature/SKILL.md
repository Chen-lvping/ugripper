---
name: add-feature
description: 为 ugripper 项目执行新增功能（add feature）类需求的标准工作流。用于用户提出“add feature / 加功能 / 新增功能”等请求时：先阅读 docs/agent/overview.md 理解当前实现，再先给实现计划并等待确认，然后以 heredoc 方式修改代码，并同步创建或维护 docs 文档；收尾阶段需根据用户指示或改动复杂度决定是否更新 build_deb.sh 与 usb_updater_build.sh 版本号。
---

# add-feature

## 固定执行顺序

1. 先阅读 `docs/agent/overview.md`，再开始设计方案。
2. 输出“实现计划 + 影响文件列表 + 文档同步计划”，进入等待开工状态。
3. 只有收到用户**明确的确认并开始指令**后，才开始改代码与文档。
4. 代码改完后，先做版本号决策（是否修改 `build_deb.sh` / `usb_updater_build.sh`），再给最终汇报。

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
3. 当流程、模块边界、运行时架构变化时，同时更新 `docs/agent/ARCHITECTURE.md`。
4. 每次 feature 修改后，都要维护 `docs/CHANGELOG.md`，内容聚焦“软件功能改动”，避免写构建过程流水；条目尽量简洁。
5. 仅在确有新增知识且无法放入现有文档时，新增文档；避免文档碎片化。

## 版本号决策与修改规则（新增）

1. 版本字段：
   - `build_deb.sh` 的 `VERSION`（ugripper 主包）
   - `usb_updater_build.sh` 的 `PKG_VERSION`（ugripper-usb-updater）
2. 决策优先级：
   - 用户明确指示 > 自动判断。
3. 自动判断（仅在用户未明确指示时）：
   - 仅文档/注释/skill 元数据等非功能改动：默认不改版本号。
   - 仅影响主包功能或运行逻辑：只改 `build_deb.sh`。
   - 仅影响 updater（如 `auto_update/*`、`usb_updater_build.sh`）：只改 `usb_updater_build.sh`。
   - 同时影响主包与 updater：两个版本都改。
4. 升级级别：
   - 默认 `.z`（patch）。
   - 跨模块功能扩展、需要运维侧额外关注时可升 `.y`（minor）。
   - 明确不兼容变更才升 `.x`（major）。
   - 无充分依据时，一律回落 `.z`。
5. 修改方式（沿用原 auto-release-deb 的 semver 规则）：
   - `bash .codex/skills/add-feature/scripts/bump_release_versions.sh --target ugripper --bump .z`
   - `bash .codex/skills/add-feature/scripts/bump_release_versions.sh --target updater --bump .z`
   - `bash .codex/skills/add-feature/scripts/bump_release_versions.sh --target both --bump .y`
   - 不改版本号：`bash .codex/skills/add-feature/scripts/bump_release_versions.sh --no-bump --reason "docs-only change"`
6. 汇报要求：
   - 必须说明“改/不改版本号”的决策理由。
   - 若有修改，必须给出每个脚本的旧版本 -> 新版本。
7. 若用户还要求“本次是否需要全量构建/如何打包”，调用 `auto-release-deb` 仅做构建范围判定，不在 `add-feature` 内重复该判定逻辑。

## 验收与检查默认策略

1. 默认不执行重测试（集成/全量回归），由用户手动检查。
2. 默认可执行轻量静态检查（例如 shell/python 语法检查）。
3. 只有用户明确要求“测试”时，才执行更完整的测试并回报结果。

## 输出格式要求

最终回复按以下顺序输出：

1. 代码改动摘要（按文件）。
2. 文档改动摘要（按文件）。
3. 版本号决策与结果（含旧版 -> 新版，或不修改原因）。
4. 已执行检查与未执行测试说明。
5. 建议的手动验收步骤。
