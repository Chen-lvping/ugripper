# TODO List

这个文件由你和 `todo-hub` 主控 agent 共同维护。你可以直接手改；skill 会按约定解析真实任务。

## 规则

- 默认不会启动全部任务；只会启动你明确点名、`focus = true` 或筛选命中的任务。
- 只有带 `<!-- TODO_HUB_TASK -->` 标记的 TOML 代码块才会被解析成真实任务。
- `slug` 使用英文短横线，用于分支名与 worktree 目录。
- 分支格式固定为：
  - `feature/zhouwu/<slug>`
  - `test/zhouwu/<slug>`
  - `fix/zhouwu/<slug>`

## 新任务模板（复制后记得补 `<!-- TODO_HUB_TASK -->` 标记）

```toml
id = "TASK-001"
title = "示例：补充 USB 音频回归测试"
type = "test"
slug = "usb-audio-regression"
status = "todo"
enabled = false
focus = false
depends_on = []
summary = ""
acceptance = [
  "写第一条验收条件",
  "写第二条验收条件",
]
notes = """
这里写上下文、限制、关注文件、排除范围等。
"""
```

## Active Tasks

在下面追加真实任务块。

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-001"
title = "确认并落地通过 pactl unload-module module-suspend-on-idle 修复音频吞音"
type = "fix"
slug = "fix-pactl-suspend-idle-audio-drop"
status = "done"
enabled = true
focus = true
depends_on = []
summary = "确认 `pactl unload-module module-suspend-on-idle` 是否是修复 USB 音频吞音的必要动作，并落地可验证方案。"
acceptance = [
  "定位吞音出现的具体链路、触发条件与受影响模块",
  "确认 `pactl unload-module module-suspend-on-idle` 是否应作为修复方案的一部分，并明确对应时序与作用边界",
  "完成至少一轮定向验证，记录修复前后差异与剩余风险",
]
notes = """
补充澄清：用户最新说明是“**需要** `pactl unload-module module-suspend-on-idle` 才能修复音频吞音”，不是“执行该命令会导致吞音”。任务应改为验证这条经验是否成立、作用条件是什么、以及如何以可维护方式落地。

建议执行：
1. 先梳理当前 V2 音频采集链路、pactl/pulseaudio 相关脚本与服务调用点。
2. 明确在什么前提下，`pactl unload-module module-suspend-on-idle` 能改善吞音；区分播放、录音、USB 耳机热插拔、空闲恢复等场景。
3. 判断该命令应作为临时 workaround、启动期初始化动作，还是运行期固定步骤；同时评估副作用与回滚方式。
4. 若修复涉及行为变更，补充对应文档与回归验证步骤。

关注范围建议：音频采集脚本、设备初始化逻辑、PulseAudio 相关启动/清理命令、USB 音频稳定性验证记录。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-002"
title = "以当前 V2 功能为基准重整 V1/V2 变更并重建 V2 文档"
type = "feature"
slug = "rebuild-v2-docs-from-current-baseline"
status = "done"
enabled = true
focus = true
depends_on = []
summary = "以 `overview` 作为唯一 V2 主文档收口当前功能说明，整理完成后删除旧 V1 文档，并列出遗留清理项。"
acceptance = [
  "梳理 V2 相对 V1 的真实功能差异，仅保留当前版本仍成立的信息",
  "按当前 V2 实现把主说明全部收敛到 `docs/agent/overview.md`，不再保留独立 `ARCHITECTURE` 作为 V2 主文档",
  "整理完成后删除旧 V1 文档，并输出一份仍需清理的遗留点清单，区分文档问题与代码/配置问题",
]
notes = """
补充澄清：用户最新要求是 **V2 文档不再需要独立 `ARCHITECTURE` 文档，全部主说明统一收敛到 `docs/agent/overview.md`**；同时旧的 V1 文档在整理完后需要删除，而不是只降级保留。

建议执行：
1. 盘点当前 docs 中描述 V1/V2 演进、迁移、历史决策的文件，识别哪些属于 V2 主说明、哪些是旧 V1 文档、哪些应直接删除。
2. 以当前代码实现和运行链路为准，把 V2 主说明统一收敛到 `docs/agent/overview.md`，不要再保留独立 `ARCHITECTURE` 作为 V2 主文档。
3. 将历史变更过程与当前系统说明分离；旧 V1 文档在完成信息迁移后直接删除。
4. 最后补一份“仍需清理的点”清单，标注优先级与归属范围。

建议优先关注：`docs/agent/`、面向交付/运维的说明文档、V1/V2 对比说明、安装部署与模块链路说明。
"""
```
