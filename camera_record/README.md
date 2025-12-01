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
# defaults: 默认权限
# nofail: 启动时如果没有插U盘，不要报错卡死系统
# x-systemd.automount: 关键参数，实现按需自动挂载
# x-systemd.device-timeout=1: 查找设备超时时间设短一点，避免拖慢系统
FSTAB_OPTS="defaults,nofail,x-systemd.automount,x-systemd.device-timeout=1"
FSTAB_LINE="$DEVICE $MOUNT_POINT auto $FSTAB_OPTS 0 0"

# 2. 创建挂载点目录 (如果不存在)
sudo mkdir -p "$MOUNT_POINT"

# 3. 修改 fstab (带防重复检查)
# grep -qF: 查找固定字符串，-q表示静默模式
# 如果 /etc/fstab 中找不到 DEVICE 字符串，则执行写入
if ! grep -qF "$DEVICE" /etc/fstab; then
    echo "正在添加配置到 /etc/fstab ..."
    echo "$FSTAB_LINE" | sudo tee -a /etc/fstab
else
    echo "检测到 /etc/fstab 已存在该配置，跳过修改。"
fi

# 4. 通知 systemd 重载配置
sudo systemctl daemon-reload

# 5. 重启相关挂载单元 (确保立即生效)
sudo systemctl restart local-fs.target

echo "配置完成！"

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
