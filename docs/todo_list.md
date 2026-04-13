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
