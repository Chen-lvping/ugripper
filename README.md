# Ugripper V2

当前仓库对应 **Ugripper V2**，默认部署形态为单机双手、本地控制的数据采集系统。

## 文档入口
- 唯一总览文档：`docs/agent/overview.md`
- 历史变更记录：`docs/CHANGELOG.md`

推荐先读 `docs/agent/overview.md`；`docs/CHANGELOG.md` 只用于追溯阶段性变更，不作为当前功能口径。

## 系统摘要
- systemd 入口：`pack_script/ugripper.service`
- 主运行时：`build/src/record_runtime/record_runtime`
- 默认录制集合：`left_cam_main`、`right_cam_main`、4 路 tactile、左右传感器 MCAP
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data/episode_YYYYMMDD_NNNN`
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- U 盘入口：`auto_update/usb_auto_update.sh`

## 常用命令
```bash
sudo systemctl status ugripper.service
sudo systemctl restart ugripper.service
sudo journalctl -u ugripper.service -f
```
