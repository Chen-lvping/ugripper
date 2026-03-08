---
name: debug-ugripper
description: 为 ugripper 项目执行故障定位与只读调试验证。用于用户提出“debug / 排查问题 / 为什么失败 / 看日志 / 录制异常 / Fays 异常 / USB 升级失败 / 主从同步异常 / 校验失败”等请求时：先阅读 docs/agent/overview.md 理解项目链路，再立即执行非修改、低风险、只读的快速检查并输出初步发现；只有涉及代码修改、配置落盘、服务重启、构建打包、升级导入或其他有副作用操作时，才等待用户明确确认。
---

# debug-ugripper

## 固定执行顺序

1. 先阅读 `docs/agent/overview.md`，用其中的主流程、模块边界、错误态和排障入口建立上下文。
2. 将问题归类到最接近的链路：服务启动、录制流程、主从同步、Fays、三路相机、传感器、episode 校验、USB 升级/标定导入、打包/恢复。
3. 无需等待用户确认，立即执行非修改、低风险、只读的快速验证。
4. 输出“当前症状判断 + 已验证证据 + 下一步排查计划 + 是否需要用户确认”的简要结论。
5. 只有进入有副作用阶段时，才等待用户明确确认后继续。

## 只读快速验证规则

收到 debug 类请求后，默认可以直接执行以下动作，不许等待用户确认：

- 读取服务日志、内核 USB/UVC 日志、`validation_error.log`、`boot_install.log`。
- 查看脚本、配置、systemd 单元、构建脚本与关键状态文件。
- 检查设备节点、锁文件、`/dev/shm` 状态文件、环境变量与进程是否存在。
- 运行轻量语法检查、关键路径搜索、只读状态命令。
- 运行 `scripts/collect_debug_context.sh` 收集现场上下文。

默认目标是先建立证据链，而不是立刻修改系统。

## 必须等待确认的动作

以下动作必须等待用户明确确认：

- 修改任何代码、脚本、配置、文档或 skill 文件。
- 启停/重启 `ugripper.service` 或其他服务。
- 执行构建、打包、升级、恢复、导入标定、写入 `/etc/environment`。
- 主动启动录制、停止录制、清理数据、删除文件或其他可能改变设备状态的动作。

若用户只说“先看看”“帮我 debug”，保持在只读调试阶段，不跨入有副作用操作。

## 默认问题收敛方式

优先补齐以下事实，缺失项可在只读检查阶段自行推断：

- 失败现象是什么，首次出现在哪个步骤。
- 是否只影响单臂、双臂、单次 episode 或所有录制。
- 最近是否做过升级、导入配置/标定、改过编码器或主从角色。
- 期望行为与实际行为的差异。
- 是否已有具体 episode 目录、报错时间点或日志关键词。

## 项目专用排查入口

- 服务主入口：`pack_script/ugripper.service` -> `run_record.sh`
- Fays 控制：`faysSense_vi_kit/scripts/run_fays_record.sh`
- USB 升级：`auto_update/usb_auto_update.sh`
- 标定导入：`auto_calibration/import_camera_calibration.sh`
- 打包：`build_deb.sh`、`usb_updater_build.sh`
- 参考清单：读取 `references/debug-playbook.md`

## 输出要求

每次 debug 响应都尽量按以下顺序输出：

1. 问题归类。
2. 已执行的只读验证。
3. 关键证据与当前判断。
4. 下一步计划。
5. 哪些动作仍需用户确认。

若已定位到高概率根因，明确区分“证据支持的结论”和“待确认的推测”。

## 资源使用

- 需要项目专项排查步骤时，读取 `references/debug-playbook.md`。
- 需要快速采集现场状态时，执行 `scripts/collect_debug_context.sh [episode_dir]`。
- 若用户后续要求真正修复代码，不继续扩展本 skill 的职责，转入相应修改流程。
