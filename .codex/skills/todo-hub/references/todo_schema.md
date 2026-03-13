# TODO Schema

`todo-hub` 使用 `docs/todo_list.md` 作为人工可编辑、脚本可解析的任务源。

## 设计目标

- 让用户可以直接手改。
- 让脚本稳定解析。
- 让主控 agent 只启动用户明确选中的任务，而不是默认全量执行。

## 文件结构

`docs/todo_list.md` 由三部分组成：

1. 说明区：解释字段与分支规则。
2. 模板区：供用户复制新增任务。
3. 任务区：真正会被脚本解析的任务块。

## 任务块约定

只有同时满足以下形式的内容才会被解析为任务：

1. 先出现标记：`<!-- TODO_HUB_TASK -->`
2. 紧跟一个 TOML fenced code block：

```md
<!-- TODO_HUB_TASK -->
```toml
id = "TASK-001"
title = "补充 USB 音频回归测试"
type = "test"
slug = "usb-audio-regression"
status = "todo"
enabled = true
focus = false
depends_on = []
summary = ""
acceptance = [
  "补充回归步骤",
  "记录验证结果",
]
notes = """
可写上下文、限制、文件范围。
"""
```
```

模板示例如果没有 `<!-- TODO_HUB_TASK -->` 标记，就不会被解析成真实任务。

## 必填字段

- `id`：稳定任务 ID，例如 `TASK-001`
- `title`：任务标题
- `type`：`feature` / `test` / `fix`
- `slug`：英文短横线 slug，用于分支与 worktree
- `status`：`todo` / `in_progress` / `blocked` / `done`
- `enabled`：布尔值，是否可被纳入候选
- `focus`：布尔值，是否为当前重点

## 可选字段

- `depends_on`：任务依赖列表
- `summary`：主控 agent 汇报时的简短摘要
- `acceptance`：验收条件数组
- `notes`：多行补充说明

## 分支规则

- `feature` 任务 → `feature/zhouwu/<slug>`
- `test` 任务 → `test/zhouwu/<slug>`
- `fix` 任务 → `fix/zhouwu/<slug>`

不要使用前导斜杠形式，如 `/feature/...`。
