---
name: todo-hub
description: 管理 `docs/todo_list.md`、按任务创建 `git worktree` 与 `feature/zhouwu/slug-name` / `test/zhouwu/slug-name` / `fix/zhouwu/slug-name` 分支、启动或续接 Codex 子会话、聚合任务进度看板、以及通过主控 agent 向指定子 session 下达指令。用于用户提出“管理 TODO / 规划任务 / 启动指定任务 / 并行开 worktree / 查看某个功能进度 / 给子 agent 发指令”等场景。默认只处理用户明确指定、`focus = true` 或筛选命中的任务；不要默认启动全部 TODO。
---

# todo-hub

## 默认原则

1. 先执行 `python3 .codex/skills/todo-hub/scripts/ensure_todo_template.py docs/todo_list.md --owner zhouwu`；若 `docs/todo_list.md` 不存在，立即生成空模板供用户手改。
2. 默认进入“计划 / 状态”模式，不要自动启动全部任务。
3. 当用户要求“同时推进 / 并行启动”多个任务，但没有明确说“直接开始实现/直接改代码”时，默认子 agent 只进入**规划阶段**：先给任务规划、影响文件和文档/测试计划，不做代码修改。
4. 主控 agent 需要等所有已启动任务都返回规划后，统一向用户汇总，再等待用户修改意见或明确确认执行。
5. 只有用户明确确认后，主控 agent 才通过续接子会话下达“开始执行”指令。
6. 只有在以下条件之一成立时，才创建 worktree 并拉起子会话：
   - 用户明确点名任务 ID；
   - 用户要求启动 `focus = true` 的任务；
   - 用户给出明确筛选条件（例如 `type = feature`、`status = todo`）。
7. 若任务选择条件不充分且执行会造成明显副作用，只问一个简短澄清问题，不要连续追问。
8. 每次任务状态变化后，都同步刷新 `docs/todo_dashboard.md`。

## 固定工作流

### 1. 建立或读取 TODO

1. 确保 `docs/todo_list.md` 存在。
2. 使用 `references/todo_schema.md` 约定的 Markdown + TOML 任务块。
3. 使用 `python3 .codex/skills/todo-hub/scripts/parse_todo.py docs/todo_list.md` 读取任务。

### 2. 规划候选任务

1. 当用户说“先规划”“看 TODO”“列可执行任务”时，只解析并汇总任务，不启动任何子会话。
2. 汇报时优先展示：`enabled = true`、`status != done`、`focus = true` 的任务。
3. 若用户要求“只看某类任务”，对 `type`、`status`、`focus` 做筛选。

### 3. 启动指定任务

1. 对每个选中的任务执行：
   - `bash .codex/skills/todo-hub/scripts/allocate_worktree.sh --task-id <TASK_ID> --owner zhouwu`
   - `bash .codex/skills/todo-hub/scripts/launch_agent_session.sh --task-id <TASK_ID> --owner zhouwu`
2. 分支格式固定为：
   - `feature/zhouwu/<slug>`
   - `test/zhouwu/<slug>`
   - `fix/zhouwu/<slug>`
3. 不要使用前导 `/feature/...` 形式。
4. 默认 worktree 目录为仓库根目录下的 `.worktrees/<task-id>-<slug>/`。
5. 若任务分支首次创建，会把当前仓库未提交的 tracked + untracked 改动同步到新 worktree（排除 `.worktrees/` 与 `todo-hub` 运行态附件），避免子任务看不到主仓当前修改。
6. 启动子会话时，优先使用用户级 `systemd-run --user` 托管；不要依赖当前主控 shell 的后台子进程长期存活。
7. dashboard 的阶段性刷新应由子 agent 自行完成：每完成一段落就调用进度上报脚本刷新，不依赖主控轮询。
8. 子任务退出时仍需立即回写最终状态。
9. 默认重拉任务时应复用该 task 已登记的 `session_id` 继续同一 agent 对话；只有用户明确要求“新 agent/重置上下文”时，才使用新的子 session。
10. 默认启动后的第一轮子会话只做规划并退出；主控 agent 需等待所有任务都进入“已返回规划”的状态后，再统一给用户看。
11. 当用户确认某个或全部规划后，主控 agent 通过 `relay_command.sh` 或重新 `launch_agent_session.sh` 续接会话，明确下达“开始执行”指令。

### 4. 查看进度

1. 优先读取 `docs/todo_dashboard.md`。
2. 若用户要求看单个任务的实时进度，再补读：
   - `.codex/todo-hub/runtime.json`
   - `.codex/todo-hub/tasks/<TASK_ID>/last_message.txt`
   - `.codex/todo-hub/tasks/<TASK_ID>/codex.jsonl`
3. 汇报时重点说明：当前状态、分支、worktree、session ID、最近摘要、阻塞项。
4. 若 runtime 记录为 `running`，仍需结合 `unit_name` / `pid` 判断是否真实存活，避免把僵尸状态当成进行中。
5. 若子 agent 已写入阶段进度文件，优先显示该阶段总结；其次再看 `last_message.txt` 与 `codex.jsonl`。

### 5. 向子会话下达指令

1. 当用户说“让 TASK-001 先补测试”“通知某个子 agent 暂停”时，执行：
   - `bash .codex/skills/todo-hub/scripts/relay_command.sh --task-id TASK-001 --message '...'`
2. 指令发送后，刷新 `docs/todo_dashboard.md` 并回报目标任务的最新摘要。
3. 当用户说“确认方案，开始执行”时，消息里必须明确包含执行授权语义，例如：
   - `方案已确认，开始执行`
   - `按当前计划实现`
   - `先做代码，不用再等`

## TODO 选择规则

1. `enabled = true` 表示该任务可被纳入候选。
2. `focus = true` 表示该任务是当前重点；只有在用户要求“启动重点任务”时才批量启动。
3. `status` 只允许：`todo`、`in_progress`、`blocked`、`done`。
4. `type` 只允许：`feature`、`test`、`fix`。
5. `slug` 必须使用短横线英文，用于分支名和 worktree 目录。

## 仓库上下文要求

1. 子会话执行真实代码修改前，优先阅读 `docs/agent/overview.md`。
2. 如需系统链路、部署或排障细节，继续在 `docs/agent/overview.md` 内按需查找对应章节。
3. 不要为了启动子任务而预先读取大量无关文档。

## 资源

### scripts/

- `ensure_todo_template.py`：初始化 `docs/todo_list.md` 空模板。
- `parse_todo.py`：解析 TODO，支持按任务 ID / 类型 / 状态 / focus 过滤。
- `allocate_worktree.sh`：为任务创建或复用分支与 worktree。
- `launch_agent_session.sh`：在对应 worktree 中启动 Codex 子会话，并登记 runtime。
- `relay_command.sh`：向已登记的子会话发送续接指令。
- `sync_dashboard.py`：聚合 TODO 与 runtime，刷新 `docs/todo_dashboard.md`。
- `runtime_ctl.py`：维护 `.codex/todo-hub/runtime.json`。

### references/

- `todo_schema.md`：`docs/todo_list.md` 的手改格式说明。
- `runtime_protocol.md`：runtime 文件、dashboard 字段和会话控制约定。
