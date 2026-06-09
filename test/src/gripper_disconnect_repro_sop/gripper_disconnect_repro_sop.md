# UGripper 夹爪传感器掉线复现与日志采集 SOP

## 1. 目标

只做两件事：

- 复现连续数采或插拔过程中的掉线/报错。
- 报错后第一时间保住日志，尤其是 `dmesg`。

目标板：

- IP：`192.168.2.240`
- 用户：`ubuntu`
- 密码：`ubuntu`

## 2. 用到的脚本

这个文件夹会单独打包给测试同学使用。所有主机侧命令都从本文件夹根目录执行，路径只依赖本文件夹内部结构。

主机侧：

- `./scripts/prepare_240_repro.sh`
- `./scripts/pull_240_repro_logs.sh`

板端：

- `/tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh`

## 3. 推荐：主机一键自动测试

测试同学拿到文件夹后，先进入这个测试文件夹根目录再执行下面命令。

连续软件触发录制、持续采集和本地 `hws` 实时刷新可以用一条命令启动：

```bash
./scripts/prepare_240_repro.sh
```

如果主机没有配置 SSH key，脚本会走 SSH 密码登录；脚本会复用一个 SSH 控制连接，正常只需要输入一次登录密码。也可以用下面方式做完全无交互测试：

```bash
BOARD_SSH_PASSWORD=ubuntu BOARD_SUDO_PASSWORD=ubuntu ./scripts/prepare_240_repro.sh
```

这一步会自动：

- 把主机当前时间同步到 `240` 设备。
- 把板端脚本下发到 `/tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh`。
- 通过 `/tmp/umi_record_control.pipe` 触发连续软件录制。
- 在主机终端实时刷新 `/usr/local/bin/hws`。
- 检测到录制错误后自动执行 `capture-now` 抓取错误窗口日志和快照，然后暂停等待测试同学记录现象、恢复或确认硬件连接，按 Enter 后继续。

常用参数：

```bash
./scripts/prepare_240_repro.sh --record-count 20
./scripts/prepare_240_repro.sh --record-sec 30 --idle-sec 10
```

默认目标是 `ubuntu@192.168.2.240`。如果目标不同，把目标写在命令后面：

```bash
./scripts/prepare_240_repro.sh ubuntu@192.168.2.240
```

注意：

- 持续日志不会在重启后自动恢复。
- 板端脚本只放在 `/tmp/gripper_disconnect_repro_sop`，执行 `stop` 后会自动删除；如果设备重启，它也会一起消失。
- 下一轮测试前，需要重新执行一次 `prepare_240_repro.sh`。
- 自动脚本只负责连续录制、`hws` 状态刷新和报错时 `capture-now` 快照；系统日志和软件日志的实时观察仍建议测试同学另开终端手动执行，避免自动脚本代管造成误判。

## 4. 手动实时看日志

需要实时观察系统日志时，另开终端执行：

```bash
ssh ubuntu@192.168.2.240 'journalctl -k -f'
```

需要实时观察 ugripper 软件日志时，另开终端执行：

```bash
ssh ubuntu@192.168.2.240 'journalctl -fu ugripper.service'
```

## 5. 手动模式：先准备

如果不使用一键自动测试，也可以先只准备和下发脚本：

```bash
./scripts/prepare_240_repro.sh --prepare-only
```

然后登录板子：

```bash
ssh ubuntu@192.168.2.240
```

## 6. 手动模式：开始采集

在板子上执行：

```bash
bash /tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh start
```

它会开始持续保存这些日志：

- `dmesg_live.log`
- `ugripper_live.log`
- `hws_5s.log`

日志默认写到：

- `/tmp/gripper_disconnect_repro_sop/logs/<RUN_ID>/`

如果 `/mnt/data_disk` 已挂载，也会同步写到：

- `/mnt/data_disk/gripper_disconnect_repro_sop/logs/<RUN_ID>/`

## 7. 怎么测

只保留最简单的执行原则：

- 插拔测试：做左插右插、左拔右拔这几种动作，动作前后看 `hws`。
- 连续数采：开始录制，停止录制，反复多条。

一旦出现下面任一现象，立刻转第 8 节：

- 结束后出现红灯
- 停录后设备没反应
- 一侧设备掉线
- 录制失败

## 8. 报错后立刻抓日志

报错后不要重启服务，不要继续插拔，直接在板子上执行：

```bash
bash /tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh capture-now
```

这个命令会自动用“当前板端时间”当作报错时间，并保存：

- 前后 `2 min` 的 `journalctl -k`
- 前后 `2 min` 的 `ugripper.service`
- 当前完整 `dmesg -T`
- 当前 `hws`
- 当前 `lsusb`

生成的关键文件包括：

- `kernel_pm2min.log`
- `ugripper_service_pm2min.log`
- `dmesg_full_at_error.log`
- `hws_at_error.log`
- `lsusb_at_error.log`

其中最优先保住的是所有 `dmesg` 相关日志。

## 9. 结束采集

测试结束后，在板子上执行：

```bash
bash /tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh stop
```

这一步会：

- 停掉持续采集进程
- 清理当前轮次标记
- 自动删除 `/tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh`

## 10. 拉回主机

回到主机后执行：

```bash
./scripts/pull_240_repro_logs.sh
```

它会把板子上最新一轮日志拉回到主机的：

```text
./logs/<RUN_ID>/
```

## 11. 最少只记什么

只记报错相关信息，不记正常步骤。

- 报错现象
- 当时是否在录制中
- 这轮日志目录位置

日志文件里最重要的是：

- `auto_record_loop.log`
- `dmesg_live.log`
- `dmesg_full_at_error.log`
- `kernel_pm2min.log`
- `ugripper_live.log`
- `ugripper_service_pm2min.log`
