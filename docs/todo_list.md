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
title = "修复灯效呼吸灯抖动与闪烁丢闪"
type = "fix"
slug = "fix-led-breath-jitter-flash-drop"
status = "todo"
enabled = true
focus = false
depends_on = []
summary = "记录灯效异常：呼吸灯存在抖动，闪烁效果存在丢闪，暂不执行。"
acceptance = [
  "定位呼吸灯抖动的触发条件与根因",
  "定位闪烁效果丢闪的触发条件与根因",
  "修复后呼吸灯亮度变化平滑，无明显抖动",
  "修复后闪烁节奏稳定，无明显丢闪",
]
notes = """
用户反馈灯效仍有问题，当前只登记 TODO，暂不启动实现。
后续执行时优先结合实际硬件表现、时序控制逻辑和灯效状态切换链路排查。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-002"
title = "在 U 盘中维护日志并参考 v1 实现方式"
type = "feature"
slug = "maintain-usb-logs-like-v1"
status = "todo"
enabled = true
focus = false
depends_on = []
summary = "记录 U 盘日志维护需求，后续实现时参考 v1 的处理方式，当前暂不执行。"
acceptance = [
  "明确 v1 中 U 盘日志维护的触发时机与保留策略",
  "在当前版本中补齐 U 盘日志维护能力",
  "日志落盘、覆盖或清理行为与预期一致",
  "完成后可通过实际 U 盘流程验证日志可用性",
]
notes = """
用户要求先登记 TODO，不立即执行。
后续实现时需要先对照 v1 的日志维护方案，确认目录结构、写入时机和清理策略。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-003"
title = "优化携带 .venv 后的 deb 打包耗时"
type = "fix"
slug = "optimize-deb-build-time-with-packaged-venv"
status = "todo"
enabled = true
focus = false
depends_on = []
summary = "记录当前因为携带项目内 .venv 导致 deb 打包变慢的问题，后续专项优化打包耗时。"
acceptance = [
  "量化当前 .venv 进入 deb 后的主要耗时来源",
  "在不破坏部署后可运行性的前提下缩短打包耗时",
  "说明优化后的环境维护方式与打包约束",
  "完成后重新打包并验证功能与耗时改善结果",
]
notes = """
背景：当前为了保证部署后 Python 环境可用，deb 会直接携带项目内 .venv，导致打包压缩时间明显变长。
后续需要从包内容、压缩策略、环境裁剪范围或构建流程上做专项优化，但不能破坏部署后直接可运行的约束。
本次仅登记 TODO，不立即执行。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-004"
title = "优化提示音重叠播放问题"
type = "fix"
slug = "avoid-overlapped-audio-prompts"
status = "todo"
enabled = true
focus = false
depends_on = []
summary = "记录语音提示会重叠播放的问题，例如录制结束和数据写入中提示音会同时播放，后续需要做串行化或抢占策略优化。"
acceptance = [
  "明确当前提示音重叠的触发链路与复现场景",
  "设计并实现提示音队列、抢占或去重策略",
  "修复后录制结束、writing 等关键提示音不再互相重叠",
  "完成后通过实际录制流程验证提示音顺序与体验",
]
notes = """
用户反馈当前语音提示存在重叠播放，例如录制结束与数据写入中的提示会叠在一起。
后续执行时需要梳理 audio daemon 的命令队列、loop 播放与一次性提示音之间的关系，避免关键语音互相覆盖。
本次仅登记 TODO，不立即执行。
"""
```
