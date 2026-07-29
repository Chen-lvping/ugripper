---
name: summarize-version-test-focus
description: 为 UGripper 仓库比较两个用户指定的 Git tag、commit 或 branch 引用，聚合版本间新增功能、用户可见行为调整、修复回归、兼容性及配置/协议变化，并在 tmp/version-change-summary/ 生成供测试工程师设计用例的简洁 Markdown 测试关注文档。用于用户提出“比较两个版本”“整理版本测试重点”“列出新版本要测什么”或指定范围生成测试摘要时；严格按输入大小写解析 Git ref，不自动替换或交换版本。
---

# Version Test Focus

## 输入

确认以下输入；用户未限定范围时分析整个版本区间：

- 旧版本 Git ref
- 新版本 Git ref
- 可选包含范围，例如“只分析录制功能”
- 可选排除范围，例如“忽略 updater”

保留 ref 原始文本和大小写。不得将 `V1.1.13` 改成 `v1.1.13`，也不得根据相似 tag 猜测用户意图。

## 固定流程

1. 先阅读 `graphify-out/GRAPH_REPORT.md`；若 `graphify-out/wiki/index.md` 存在，优先通过 wiki 导航。
2. 确认仓库根目录和 `tmp/` 的 Git 忽略状态：

   ```bash
   git rev-parse --show-toplevel
   git check-ignore -v tmp
   ```

3. 用用户输入的精确 ref 调用证据脚本。输出目录名只对 `/`、空格等路径字符做安全替换，文档内仍保留原始 ref：

   ```bash
   bash .codex/skills/summarize-version-test-focus/scripts/collect-version-evidence.sh \
     '<old-ref>' '<new-ref>' \
     'tmp/version-change-summary/<safe-old>_to_<safe-new>/evidence'
   ```

4. 若任一 ref 不存在，停止并明确指出缺失的原始 ref，不尝试大小写变体。若脚本判定版本顺序反转，停止并请用户确认，不自动交换。
5. 阅读 `evidence/refs.txt`、`commits.txt`、`name-status.txt`、`diff-stat.txt` 和 `changes.patch`。同时阅读版本区间涉及的 `docs/CHANGELOG.md`、相关功能文档及测试脚本。
6. 对跨模块功能使用 `graphify query`、`graphify path` 或 `graphify explain` 核对代码、配置、服务、数据文件与测试资产的关系；不要只根据提交标题下结论。
7. 将同一功能涉及的代码、配置、脚本和文档合并成一个功能项，不按 commit 或文件逐项罗列。
8. 从 `assets/test-focus-template.md` 生成 `tmp/version-change-summary/<safe-old>_to_<safe-new>/test-focus.md`。
9. 生成后确认文档仍位于 `tmp/version-change-summary/` 且被 Git 忽略：

   ```bash
   git check-ignore -v 'tmp/version-change-summary/<safe-old>_to_<safe-new>/test-focus.md'
   git status --short
   ```

## 筛选与分类

默认保留：

- 新增功能
- 用户或系统可观察的行为调整
- 需要专项回归的缺陷修复
- 硬件、服务、配置、数据格式、USB 升级或网络兼容性变化
- 默认启用、配置启用、特定设备触发或异常条件触发的行为

默认忽略：

- 格式化、注释和纯内部重构
- 构建产物、graphify 产物和单纯版本号更新
- 仅用于说明已有功能的文档更新；将其作为功能证据，不单独形成测试项
- 对用户行为和接口均无影响的测试代码整理

分类只使用：`新增功能`、`行为调整`、`修复回归`、`兼容性`、`配置/协议变化`。优先级只使用 `高`、`中`、`低`。

若区间没有有效功能变化，仍生成文档，明确写“未识别到需要新增测试设计的功能变化”，并列出比较范围和过滤依据。

## 测试信息提取

每项功能保持简洁，但必须提供可执行、可判定的信息：

- `触发条件`：说明配置、设备、运行状态、按键、异常或输入条件。
- `预期效果`：写明用户或系统能观察到的结果。若涉及灯光、蜂鸣、提示音或时序，证据充分时列出颜色、次数、持续时间、频率和恢复状态。
- `检查位置`：涉及落盘数据或配置时，给出具体文件路径和关键字段，不写笼统的“检查 metadata”。
- `测试建议`：给出正常、关闭、拒绝、异常、兼容或回归方向，不扩写为完整测试用例。
- `仓库检测入口`：复杂的数据、同步、服务或协议验证若已有脚本，给出仓库相对路径、推荐命令和通过标准。
- `旧行为与新行为`：仅在行为调整时增加。
- `变更依据`：可选列出短 commit ID 和关键文件，保持精简。

证据不足、提交说明与代码不一致或无法确认准确预期时，写入文末“待确认项”，不得补写推测行为。

## 输出约束

- 只向 `tmp/version-change-summary/` 写测试摘要和证据，不修改正式 docs。
- 文档开头写比较范围、原始 Git ref、解析后的 commit、生成时间和用户限定范围。
- 先列高优先级新增功能，再列中低优先级回归。
- 不包含安装、构建、打包和发布步骤，除非它们本身是用户指定的测试范围。
- 不把现有测试结果表述为新版本已完整通过；明确区分“已有证据”和“仍需测试”。
