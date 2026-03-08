# Ugripper Debug Playbook

## 目录

1. 服务启动失败
2. 录制启动/停止异常
3. 主从同步异常
4. Fays 异常
5. 三路相机异常
6. 传感器异常
7. Episode 校验失败
8. USB 升级/标定导入失败
9. 打包/恢复异常

## 1. 服务启动失败

- 优先查看 `journalctl -u ugripper.service -n 120 --no-pager`
- 查看单元文件 `pack_script/ugripper.service` 与主脚本 `run_record.sh`
- 核对 `/etc/environment` 中 `DEVICE_ROLE`、`CAMERA_CODEC`、`UGRIPPER_LANG`
- 核对关键路径是否存在：`/opt/ugripper`、`/mnt/data_disk`
- 检查 `/dev/shm/umi_fays_present`、`/dev/shm/umi_fays_usb_speed_mbps` 是否已被初始化

## 2. 录制启动/停止异常

- 查看 `journalctl -u ugripper.service -n 200 --no-pager` 中 `start_recording`、`stop_recording` 相关日志
- 检查 `/tmp/umi_recording.lock`
- 确认 episode 根目录是否落在 `/mnt/data_disk/<device_sn>/data/episode_*`
- 查看是否拉起三路相机、`sensor_recorder`、Fays 录制进程
- 若用户提供 episode 目录，检查目录内是否有 `cam.mkv`、`tact_left.mkv`、`tact_right.mkv`、`sensor_data.mcap`

## 3. 主从同步异常

- 核对 `/etc/environment` 中 `DEVICE_ROLE`
- 结合 `docs/agent/overview.md` 确认 `DEVICE_SIDE` 与默认旧逻辑是否混淆
- 从服务日志中搜索 `START|`、`STOP|0`、`paired_master_sn`
- 确认 `nc` 使用端口 `12345`
- 若已有 episode，查看 Slave 侧 `info.json` 是否回写 `paired_master_sn`

## 4. Fays 异常

- 核对设备节点 `/dev/fays_stereo`、`/dev/fays_imu`
- 查看 `journalctl -u ugripper.service -n 200 --no-pager` 中 Fays daemon/start/stop 日志
- 查看 `/dev/shm/umi_fays_present` 与 `/dev/shm/umi_fays_usb_speed_mbps`
- 若怀疑 USB 问题，查看 `journalctl -k -n 200 --no-pager | egrep 'usb|uvcvideo|xhci|reset|disconnect|error -71'`
- 录制后若 Fays 校验失败，检查 episode 下 `fays_stereo_output.mkv`、`fays_data.mcap`、`validation_error.log`

## 5. 三路相机异常

- 查看 `camera_record/triple_camera_record.py` 是否被启动
- 核对 `/etc/environment` 中 `CAMERA_CODEC`
- 从服务日志中查找相机启动参数与编码器选择
- 若日志指向 UVC/USB，不要只看服务日志，同时看内核日志

## 6. 传感器异常

- 确认 `build/src/sensor_recorder/sensor_recorder` 是否存在
- 从服务日志中搜索 encoder 首条样本打印，确认编码器链路是否正常
- 若已有 episode，检查 `sensor_data.mcap` 是否存在且非空

## 7. Episode 校验失败

- 先读 episode 下 `validation_error.log`
- 检查基础文件是否齐全：`cam.mkv`、`tact_left.mkv`、`tact_right.mkv`、`sensor_data.mcap`、`fays_stereo_output.mkv`、`fays_data.mcap`
- 若是 Fays 时长失败，重点比对 `cam.mkv` 与 `fays_stereo_output.mkv`
- 若是 Fays MCAP 末尾失败，重点关注 `fays_data.mcap` 尾段 IMU 覆盖
- 若是 tact 时长失败，重点关注 `sensor_data.mcap` 与双 tact 视频跨度

## 8. USB 升级/标定导入失败

- 查看 `/var/log/ugripper/boot_install.log`
- 查看 `auto_update/usb_auto_update.sh` 与 `auto_calibration/import_camera_calibration.sh`
- 核对 U 盘目录是否符合 `ugripper_calib/<DEVICE_SN>/`
- 核对 `config.txt` 中 `LANGUAGE`、`CAMERA_CODEC`、`DEVICE_ROLE` 的键和值
- 关注失败后是否进入 `ERROR_1` 并在导入后触发统一重启

## 9. 打包/恢复异常

- 查看 `build_deb.sh`、`usb_updater_build.sh`
- 查看 `/var/log/ugripper/boot_install.log` 中 updater 与主包恢复顺序
- 若用户报告“升级后坏了”，先区分是包构建问题、安装问题还是服务恢复问题
