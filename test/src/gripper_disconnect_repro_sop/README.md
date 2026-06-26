# UGripper 夹爪掉线复现测试

把整个 `gripper_disconnect_repro_sop` 文件夹发给测试同学。测试时先进入这个文件夹，再执行命令。

## 1. 开始自动测试

在主机上执行：

```bash
./scripts/prepare_240_repro.sh
```

默认连接：

- 板端：`ubuntu@192.168.2.240`
- 密码：`ubuntu`

脚本会自动完成：

- 同步板端时间。
- 上传板端脚本到 `/tmp/gripper_disconnect_repro_sop/`。
- 通过软件触发连续录制。
- 在主机终端实时刷新 `hws` 硬件连接状态。
- 发现错误后自动抓日志，并暂停等待人工确认。
- 使用 `--continue-on-error` 时，发现错误会先抓日志，再等待服务、硬件和数据盘恢复后继续下一轮。

`hws` 前台显示会用颜色标记状态：绿色表示 `OK`，红色表示失败或缺失，灰色表示未启用。

脚本暂停时，先记录现场现象，确认或恢复硬件连接后，按 Enter 继续。

## 2. 常用参数

只跑指定轮数：

```bash
./scripts/prepare_240_repro.sh --record-count 20
```

调整每轮录制时间和间隔：

```bash
./scripts/prepare_240_repro.sh --record-sec 30 --idle-sec 10
```

指定其他板子：

```bash
./scripts/prepare_240_repro.sh ubuntu@192.168.2.240
```

长时间测试建议使用安全入口，默认 10 小时、每小时只拉取 `/tmp` 测试日志，不碰数据盘 episode：

```bash
./scripts/run_240_soak_safe.sh
```

如果必须边测边拉 episode 数据，显式加 `--pull-data`；如果还要清理板端 episode，再加 `--clean-board-data`。脚本会先让板端测试停在两轮之间，并确认无录制、无自动 USB 复位、数据盘稳定后才操作 `/mnt/data_disk`。

## 3. 手动实时看日志

自动脚本只实时显示 `hws`，系统日志和软件日志建议另开终端手动看。

系统/内核日志：

```bash
ssh ubuntu@192.168.2.240 'journalctl -k -f'
```

软件日志：

```bash
ssh ubuntu@192.168.2.240 'journalctl -fu ugripper.service'
```

## 4. 拉回日志

测试结束后，在主机这个文件夹里执行：

```bash
./scripts/pull_240_repro_logs.sh
```

日志会拉到：

```text
./logs/<RUN_ID>/
```

重点看这些文件：

- `auto_record_loop.log`：自动测试每一轮起停、错误原因、episode 结果。
  - `错误摘要` / `相关硬件`：优先看这两行，表示脚本从 `validation_error.log` 中提取出的直接原因和疑似硬件链路。
- `hws_5s.log`：持续记录的硬件连接状态。
- `error_*/capture_meta.log`：报错时间点。
- `error_*/dmesg_full_at_error.log`：报错时完整 `dmesg`。
- `error_*/kernel_pm2min.log`：报错前后 2 分钟内核日志。
- `error_*/ugripper_service_pm2min.log`：报错前后 2 分钟软件日志。
- `error_*/hws_at_error.log`：报错瞬间硬件连接状态。
- `error_*/lsusb_at_error.log`：报错瞬间 USB 状态。

## 5. 停止和清理

前台测试中按 `Ctrl-C` 停止，脚本会尝试自动停止板端采集。

如果需要手动清理板端脚本：

```bash
ssh ubuntu@192.168.2.240 'bash /tmp/gripper_disconnect_repro_sop/scripts/board_repro_log.sh stop'
```

## 6. 只下发脚本不测试

一般不用。需要手动模式时执行：

```bash
./scripts/prepare_240_repro.sh --prepare-only
```

完整 SOP 见 `gripper_disconnect_repro_sop.md`。
