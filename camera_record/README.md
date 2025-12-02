# Camera Record

## 安装依赖

```bash
pip install av

sudo cp 99-fixed-usb-map.rules /etc/udev/rules.d/

sudo udevadm control --reload-rules
sudo udevadm trigger


DEVICE="/dev/data_disk"
MOUNT_POINT="/mnt/data_disk"

# 挂载选项
# umask=000: FAT/exFAT/NTFS 允许所有用户读写
# rw: 显式读写
# users: 普通用户可以挂载/卸载
# nosuid,nodev: 防止 U 盘上可执行文件提权
# x-systemd.automount: 使用 systemd 自动挂载
# x-systemd.device-timeout=5s: 等待设备就绪
# x-systemd.idle-timeout=10s: 空闲 10 秒自动卸载
FSTAB_OPTS="defaults,nofail,x-systemd.automount,x-systemd.device-timeout=5s,x-systemd.idle-timeout=5s,umask=000,rw,users,nosuid,nodev"

# 拼接 fstab 行
FSTAB_LINE="$DEVICE $MOUNT_POINT auto $FSTAB_OPTS 0 0"

echo "=== 配置挂载点 ==="
# 创建挂载点目录并设置权限
sudo mkdir -p "$MOUNT_POINT"
sudo chmod 777 "$MOUNT_POINT"

echo "=== 清理旧配置 ==="
# 删除旧的 /mnt/data_disk 配置行
if grep -qF "$MOUNT_POINT" /etc/fstab; then
    sudo sed -i "\|${MOUNT_POINT}|d" /etc/fstab
fi

echo "=== 添加新配置到 /etc/fstab ==="
echo "$FSTAB_LINE" | sudo tee -a /etc/fstab

echo "=== 重载 systemd 配置 ==="
sudo systemctl daemon-reload

echo "=== 重启挂载单元 ==="
# 先重启 local-fs.target 以加载新的 fstab（安全起见）
sudo systemctl restart local-fs.target 2>/dev/null || true

# 重启 automount 单元，确保 idle-timeout 生效
MOUNT_UNIT=$(systemd-escape -p --suffix=automount "$MOUNT_POINT")
sudo systemctl restart "$MOUNT_UNIT"

echo "=== 检查自动卸载设置 ==="
systemctl show "$MOUNT_UNIT" | grep -E 'Idle|Timeout'

echo "挂载配置完成。请插入设备并测试自动挂载与自动卸载功能。"

```

## 使用方法

```bash
python triple_camera_record.py -d 60                    # 录制60秒
python triple_camera_record.py -d 60 -o raw_data        # 指定输出目录
python triple_camera_record.py -d 60 --device-id dev01  # 指定设备ID
```

## 输出格式

```
raw_data/
└── episode_20251201_ugripper_001_0000/
    ├── cam.mkv / cam.csv
    ├── tact_left.mkv / tact_left.csv
    └── tact_right.mkv / tact_right.csv
```
