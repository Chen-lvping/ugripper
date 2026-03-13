# TODO Dashboard

- Updated at: `2026-03-13T11:27:55+00:00`
- Owner: `zhouwu`
- Total tasks: `2`
- Active tasks: `0`
- Focus tasks: `2`

## Focus

- `TASK-001` | `done` | runtime `-` | session `-` | 确认并落地通过 pactl unload-module module-suspend-on-idle 修复音频吞音
- `TASK-002` | `done` | runtime `-` | session `-` | 以当前 V2 功能为基准重整 V1/V2 变更并重建 V2 文档

## Tasks

### TASK-001 · 确认并落地通过 pactl unload-module module-suspend-on-idle 修复音频吞音

- status `done` | runtime `-` | enabled `True` | focus `True` | branch `-` | session `-`
- Worktree: `-`
- Summary: 确认 `pactl unload-module module-suspend-on-idle` 是否是修复 USB 音频吞音的必要动作，并落地可验证方案。
- Acceptance: 定位吞音出现的具体链路、触发条件与受影响模块；确认 `pactl unload-module module-suspend-on-idle` 是否应作为修复方案的一部分，并明确对应时序与作用边界；完成至少一轮定向验证，记录修复前后差异与剩余风险

### TASK-002 · 以当前 V2 功能为基准重整 V1/V2 变更并重建 V2 文档

- status `done` | runtime `-` | enabled `True` | focus `True` | branch `-` | session `-`
- Worktree: `-`
- Summary: 以 `overview` 作为唯一 V2 主文档收口当前功能说明，整理完成后删除旧 V1 文档，并列出遗留清理项。
- Acceptance: 梳理 V2 相对 V1 的真实功能差异，仅保留当前版本仍成立的信息；按当前 V2 实现把主说明全部收敛到 `docs/agent/overview.md`，不再保留独立 `ARCHITECTURE` 作为 V2 主文档；整理完成后删除旧 V1 文档，并输出一份仍需清理的遗留点清单，区分文档问题与代码/配置问题
