# Camera Record

## 安装依赖

```bash
pip install av

sudo cp 99-fixed-usb-map.rules /etc/udev/rules.d/

sudo udevadm control --reload-rules
sudo udevadm trigger


# 1. 定义变量
DEVICE="/dev/data_disk"
MOUNT_POINT="/mnt/data_disk"

# 关键修改：
# umask=000: 对于 FAT/exFAT/NTFS，允许所有用户读写 (777 权限)
# rw: 显式指定读写
# users: 允许非 root 用户挂载/卸载
# nosuid,nodev: 安全选项，防止U盘上的可执行文件提权
FSTAB_OPTS="defaults,nofail,x-systemd.automount,x-systemd.device-timeout=1,umask=000,rw,users,nosuid,nodev"
FSTAB_LINE="$DEVICE $MOUNT_POINT auto $FSTAB_OPTS 0 0"

# 2. 创建挂载点目录 (如果不存在) 并放宽目录权限
sudo mkdir -p "$MOUNT_POINT"
sudo chmod 777 "$MOUNT_POINT"

# 3. 清理旧配置 (防止重复或保留了旧的错误权限)
# 使用 sed 删除所有包含 /mnt/data_disk 的行，确保干净
if grep -qF "$MOUNT_POINT" /etc/fstab; then
    echo "发现旧配置，正在清理..."
    sudo sed -i "\|${MOUNT_POINT}|d" /etc/fstab
fi

# 4. 写入新配置
echo "正在添加带写权限的配置到 /etc/fstab ..."
echo "$FSTAB_LINE" | sudo tee -a /etc/fstab

# 5. 重载 Systemd
echo "重载系统配置..."
sudo systemctl daemon-reload

# 6. 重启挂载单元
# 注意：如果设备正忙，这一步可能会报错，但不影响下次插入
sudo systemctl restart local-fs.target 2>/dev/null || true

# 7. 强制重置 Automount (解决缓存问题)
MOUNT_UNIT=$(systemd-escape -p --suffix=automount "$MOUNT_POINT")
sudo systemctl restart "$MOUNT_UNIT"

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
