# Camera Record

## 安装依赖

```bash
pip install av
sudo cp 99-fixed-usb-map.rules /etc/udev/rules.d/
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
