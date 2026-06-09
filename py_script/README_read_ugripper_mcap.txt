MCAP 读取说明
==============

本目录提供一个 UGripper MCAP 读取示例脚本：

  read_ugripper_mcap.txt

脚本内容是 Python，只是文件后缀改成了 .txt，方便在会拦截、加密或限制发送 .py 文件的环境里传给数据使用者。运行时仍然用 python3 执行。


1. 安装依赖
-----------

脚本需要 python mcap 包：

  /usr/bin/python3 -m pip install --user mcap

在 UGripper 开发机上建议固定使用 /usr/bin/python3。部分环境里的 python3 可能指向交叉编译解释器，宿主机上无法直接运行。


2. 读取单个 MCAP
----------------

读取 sensor MCAP：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/sensor_left.mcap

只读取 encoder topic，并打印前 10 条样本：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/sensor_left.mcap --topic encoder_left --limit 10

读取 Fays MCAP：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/fays_data_left.mcap --limit 10

只读取 Fays IMU topic：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/fays_data_left.mcap --topic i --limit 10

只读取 Fays camera timestamp topic：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/fays_data_left.mcap --topic c --limit 10


3. 读取整个 episode
-------------------

脚本也可以直接输入 episode 目录：

  /usr/bin/python3 read_ugripper_mcap.txt /mnt/data_disk/<device_sn_lower>/data/episode_<date>_<index> --limit 3

输入 episode 目录时，脚本会自动读取存在的这些文件：

  sensor_left.mcap
  sensor_right.mcap
  fays_data_left.mcap
  fays_data_right.mcap
  ego/sensor.mcap


4. 输出 JSON 报告
----------------

生成机器可读的 JSON 报告：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/sensor_left.mcap --json-out /tmp/sensor_left_report.json

只看 topic 统计，不打印样本：

  /usr/bin/python3 read_ugripper_mcap.txt /tmp/sensor_left.mcap --no-samples


5. 当前 payload 格式
--------------------

sensor_left.mcap / sensor_right.mcap：

  topic: encoder_left / encoder_right
  payload: little-endian int32 raw + float32 rad

fays_data_left.mcap / fays_data_right.mcap：

  topic: i
  payload: little-endian float64[6]
  顺序: gx, gy, gz, ax, ay, az

  topic: c
  payload: little-endian uint32 frame_index


6. 为什么 ros2 bag info / Foxglove 可能打不开
----------------------------------------------

当前 UGripper MCAP 是通用 MCAP 容器，里面是自定义紧凑二进制 payload，不是 ROS 2 bag。

ros2 bag info 需要 rosbag2 的 metadata 和 ROS 消息序列化信息。Foxglove 直接可视化也需要它认识的 schema 或 encoding。因此，这些文件能作为 MCAP 被读取，但不能用 ros2 bag info 或 Foxglove ROS bag 入口是否能打开来判断数据是否正常。

如果需要在 ROS 或 Foxglove 中可视化，需要先把脚本解出的数值转换成 ROS 消息类型，或转换成 Foxglove 能识别的 schema。
