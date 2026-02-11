#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <libserialport.h>
#include <chrono>

namespace dmbot_serial
{

    // ---------------- IM648 数据 ----------------
    struct IM648_Data
    {
        // 加速度 (m/s²)
        float accx = 0, accy = 0, accz = 0;
        // 角速度 (rad/s)
        float gyrox = 0, gyroy = 0, gyroz = 0;
        // 欧拉角 (度)
        float roll = 0, pitch = 0, yaw = 0;
        // 四元数
        float quat_x = 0.0, quat_y = 0.0, quat_z = 0.0, quat_w = 1.0;
        // 时间戳
        uint64_t timestamp = 0;
        // 数据更新标志（线程安全）
        std::atomic<bool> data_updated{false};
    };

    class Im648Driver
    {
    public:
        Im648Driver(const std::string &port_name, int baudrate = 115200);
        ~Im648Driver();

        void start();  // 启动读取线程
        void stop();   // 停止读取线程
        const IM648_Data& getData() const; // 获取最新数据（返回引用，避免复制atomic成员）
        void clearDataUpdated(); // 清除数据更新标志

    private:
        void initSerial();      // 初始化串口
        void readThread();      // 后台读取线程（类似DmImu::readThread）

        std::string port_name_;
        int baudrate_;
        std::thread th_;
        std::atomic<bool> stop_flag_{false};
        struct sp_port *port_ = nullptr;
        IM648_Data data_;
    };

} // namespace dmbot_serial
