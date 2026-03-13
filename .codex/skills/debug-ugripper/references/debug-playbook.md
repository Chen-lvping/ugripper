# Ugripper Debug Playbook

## 目录

1. 服务启动失败
2. 录制启动/停止异常
3. 网络 / 环境异常
4. 相机录制异常
5. 传感器异常
6. Episode 校验失败
7. USB 升级/标定导入失败
8. 打包/恢复异常

## 1. 服务启动失败

- 优先查看 `journalctl -u ugripper.service -n 120 --no-pager`
- 查看单元文件 `pack_script/ugripper.service` 与主脚本 `run_record.sh`
- 核对 `/etc/environment` 中 `DEVICE_ROLE`、`CAMERA_CODEC`、`UGRIPPER_LANG`
- 核对关键路径是否存在：`/opt/ugripper`、`/mnt/data_disk`

## 2. 录制启动/停止异常

- 查看 `journalctl -u ugripper.service -n 200 --no-pager` 中 `start_recording`、`stop_recording`、`READY`、`RECORDING` 相关日志
- 检查 `/tmp/umi_recording.lock`、`/tmp/umi_shutdown_request`
- 确认 episode 根目录是否落在 `/mnt/data_disk/<device_sn>/data/episode_*`
- 查看是否拉起 `camera_recorder`、`sensor_recorder`、`record_runtime` 相关进程
- 若用户提供 episode 目录，检查目录内是否有 `left_cam_main.mkv`、`right_cam_main.mkv`、4 路 tact `mkv`、`sensor_data_left.mcap`、`sensor_data_right.mcap`、`metadata.json`、`calibration.json`

## 3. 网络 / 环境异常

- 核对 `/etc/environment` 中网络相关配置与 `DEVICE_ROLE`，但不要把它们当作当前录制主流程的前置条件
- 结合 `docs/agent/overview.md` 确认当前系统默认是本地录制，网络更偏部署与运维环境约束而非录制前提
- 从服务日志中搜索网络重启、`ugripper-network-monitor.service`、`carrier` 边沿和 `usb_auto_update` 相关痕迹
- 确认静态 IP 是否被安装脚本下发，以及 NetworkManager 连接是否异常

## 4. 相机录制异常

- 查看 `build/src/camera_recorder/camera_recorder` 是否被启动
- 核对 `config/camera_recorder.yaml` 与 `/etc/environment` 中 `CAMERA_CODEC`
- 从服务日志中查找相机启动参数、`--only` 列表和编码器选择
- 若日志指向 UVC/USB，不要只看服务日志，同时看内核日志
- 优先确认当前默认录制集合是否为左右主摄 + 4 路 tact，而不是去找旧三路脚本

## 5. 传感器异常

- 确认 `build/src/sensor_recorder/sensor_recorder` 是否存在
- 从服务日志中搜索 encoder 首条样本打印，确认编码器链路是否正常
- 若已有 episode，检查 `sensor_data_left.mcap`、`sensor_data_right.mcap` 是否存在且非空

## 6. Episode 校验失败

- 先读 episode 下 `validation_error.log`
- 检查基础文件是否齐全：`left_cam_main.mkv`、`right_cam_main.mkv`、`left_tcam_l.mkv`、`left_tcam_r.mkv`、`right_tcam_l.mkv`、`right_tcam_r.mkv`、`sensor_data_left.mcap`、`sensor_data_right.mcap`、`metadata.json`、`calibration.json`
- 若是 tact 时长失败，重点关注双侧 tact 视频跨度与双份 MCAP 是否完整

## 7. USB 升级/标定导入失败

- 查看 `/var/log/ugripper/boot_install.log`
- 查看 `auto_update/usb_auto_update.sh` 与 `auto_calibration/import_camera_calibration.sh`
- 核对 U 盘目录是否符合 `ugripper_calib/<DEVICE_SN>/`
- 核对 `config.txt` 中 `LANGUAGE`、`CAMERA_CODEC`、`DEVICE_ROLE` 的键和值
- 关注失败后是否进入 `ERROR_1` 并在导入后触发统一重启

## 8. 打包/恢复异常

- 查看 `build_deb.sh`、`usb_updater_build.sh`
- 查看 `/var/log/ugripper/boot_install.log` 中 updater 与主包恢复顺序
- 若用户报告“升级后坏了”，先区分是包构建问题、安装问题还是服务恢复问题
