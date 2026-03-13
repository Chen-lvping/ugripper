# Runtime Protocol

`todo-hub` 用两个层级维护运行态：

## 1. 机器可读状态

文件：`.codex/todo-hub/runtime.json`

用途：

- 记录任务到分支 / worktree / session 的映射
- 记录最近一次命令发送时间
- 作为 `relay_command.sh` 与 `sync_dashboard.py` 的共享状态

建议字段：

- `owner`
- `updated_at`
- `tasks.<TASK_ID>.branch`
- `tasks.<TASK_ID>.worktree`
- `tasks.<TASK_ID>.session_id`
- `tasks.<TASK_ID>.pid`
- `tasks.<TASK_ID>.unit_name`
- `tasks.<TASK_ID>.progress_file`（子 agent 阶段性进度摘要）
- `tasks.<TASK_ID>.status`
- `tasks.<TASK_ID>.last_event_at`
- `tasks.<TASK_ID>.last_message_file`
- `tasks.<TASK_ID>.log_file`
- `tasks.<TASK_ID>.last_command`

## 2. 人类可读状态

文件：`docs/todo_dashboard.md`

用途：

- 展示所有任务总览
- 突出 `focus = true` 的任务
- 展示每个已启动任务的 worktree / session / 最近摘要 / 阻塞项

## 子会话工作目录

每个任务的运行附件放到：

`.codex/todo-hub/tasks/<TASK_ID>/`

常见文件：

- `prompt.txt`：首次启动提示词
- `last_message.txt`：最近一次 Codex 最终消息
- `codex.jsonl`：`codex exec --json` 输出

## 指令转发协议

主控 agent 给子会话发指令时：

1. 从 `runtime.json` 读取 `session_id` 与 `worktree`
2. 调用 `codex exec resume <session_id> '<message>'`（沿用和子任务相同的无沙箱执行参数）
3. 刷新 `last_message.txt`
4. 更新 `runtime.json`
5. 刷新 `docs/todo_dashboard.md`

若任务尚未登记 `session_id`，则不允许转发指令，必须先启动该任务。

## 启动持久化策略

- 优先使用 `systemd-run --user` 启动每个任务的独立 transient service，避免任务生命周期绑定到主控 agent 的当前 shell / 当前 turn。
- 若环境不支持 `systemd-run`，再回退到 `nohup`。
- 看板展示的 `runtime` 应根据 `unit_name` 或 `pid` 实时判定，避免出现“进程已退出但仍显示 running”的假状态。

## 子 agent 进度上报

- dashboard 的阶段性刷新由子 agent 自行触发：每完成一个阶段，就调用进度上报脚本写入阶段总结并刷新 `docs/todo_dashboard.md`。
- 子任务退出时 runner 仍会立即回写最终状态并刷新 dashboard，保证收尾可见。
- dashboard 读取顺序应优先使用 `progress_file`，其次是 `last_message.txt`，最后才回退到 `codex.jsonl` 最近一条 `agent_message`。

## 会话续接策略

- `launch_agent_session.sh` 默认应优先读取 `runtime.json` 中该 task 已登记的 `session_id`，并通过 `codex exec resume <session_id>` 续接同一 agent。
- 只有用户明确要求“新 agent / 重置上下文 / 重新开一个 agent”时，才允许忽略旧 `session_id` 创建新会话。
- 向运行中的 task 发送补充说明、纠正方向或追问进展时，优先使用同一 `session_id` 续接，而不是重新 `codex exec`。
