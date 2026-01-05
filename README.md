# 数采夹爪（Data Collection Gripper）

本仓库用于数采夹爪项目，当前为占位仓库。

后续将补充：
- 项目代码
- 硬件接口说明
- 使用文档与示例
sensor

# 安装依赖
sudo apt install -y sox libsox-fmt-all

sudo systemctl stop ugripper.service 
sudo systemctl restart ugripper.service 
sudo systemctl status ugripper.service

# 使用说明
触发自动校准脚本：
当网线插入时，自动触发校准脚本 `run_calibration.sh`，按住按键可进入校准模式，无按键按下则会自动恢复采集模式。