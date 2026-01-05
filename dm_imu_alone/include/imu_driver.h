#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <libserialport.h>
#include "bsp_crc.h"

namespace dmbot_serial
{

    // ---------------- IMU 数据 ----------------
    struct IMU_Data
    {
        // 加速度
        float accx = 0, accy = 0, accz = 0;
        // 角速度
        float gyrox = 0, gyroy = 0, gyroz = 0;
        // 欧拉角
        float roll = 0, pitch = 0, yaw = 0;
        // 四元数 (仅485模式支持)
        float quat_x = 1.0, quat_y = 0.0, quat_z = 0.0, quat_w = 0.0;
        // 时间戳
        uint64_t timestamp = 0;
    };

// ---------------- 数据帧 (USB-TTY没有quat数据帧） ----------------
#pragma pack(push, 1)
    struct IMU_Frame // 应该用不到，只做数据结构示意
    {
        uint8_t FrameHeader1;
        uint8_t flag1;
        uint8_t slave_id1;
        uint8_t reg_acc;

        uint32_t accx_u32;
        uint32_t accy_u32;
        uint32_t accz_u32;
        uint16_t crc1;
        uint8_t FrameEnd1;

        uint8_t FrameHeader2;
        uint8_t flag2;
        uint8_t slave_id2;
        uint8_t reg_gyro;

        uint32_t gyrox_u32;
        uint32_t gyroy_u32;
        uint32_t gyroz_u32;
        uint16_t crc2;
        uint8_t FrameEnd2;

        uint8_t FrameHeader3;
        uint8_t flag3;
        uint8_t slave_id3;
        uint8_t reg_euler;

        uint32_t roll_u32;
        uint32_t pitch_u32;
        uint32_t yaw_u32;
        uint16_t crc3;
        uint8_t FrameEnd3;

        uint8_t FrameHeader4;
        uint8_t flag4;
        uint8_t slave_id4;
        uint8_t reg_quat;

        uint32_t quat_x;
        uint32_t quat_y;
        uint32_t quat_z;
        uint32_t quat_w;
        uint16_t crc4;
        uint8_t FrameEnd4;
    };
#pragma pack(pop)

// ---------------- 485 命令帧 ----------------
#pragma pack(push, 1)
    struct IMU_485_Cmd_Frame
    {
        uint8_t header;        // 0xA5
        uint8_t data_type;     // 数据类型 0x0C:指令
        uint8_t slave_id;      // 从机ID 0x01
        uint8_t register_addr; // 寄存器地址
        uint8_t rw_flag;       // 读/写标志
        uint32_t data0;        // 32位数据0
        uint32_t data1;        // 32位数据1
        uint32_t data2;        // 32位数据2
        uint32_t data3;        // 32位数据3
        uint8_t ack_flag;      // 应答位
        uint8_t reserved;      // 保留字节
        uint8_t end;           // 帧尾
    };
#pragma pack(pop)

    class DmImu
    {
    public:
        enum class ProtocolType
        {
            USB_TTY,
            RS485
        };

        enum class FreqMode : uint8_t
        {
            HZ100 = 0x01,
            HZ125 = 0x02,
            HZ200 = 0x03,
            HZ250 = 0x04,
            HZ500 = 0x05,
            HZ1000 = 0x06
        };

        enum class OutputInterface : uint8_t
        {
            USB = 0x00,
            RS485 = 0x01,
            CAN = 0x02
        };

        DmImu(const std::string &port_name, int baudrate = 921600, ProtocolType protocol = ProtocolType::USB_TTY);
        ~DmImu();

        void start();
        void stop();

        void enterGyroCalib();
        void enterAccelCalib();
        void restoreFactorySettings();

        ProtocolType getProtocolType();

        IMU_Data getData();

        // 获取四元数数据 (仅485模式有效)
        void getQuaternion(float &q0, float &q1, float &q2, float &q3)
        {
            q0 = data_.quat_x;
            q1 = data_.quat_y;
            q2 = data_.quat_z;
            q3 = data_.quat_w;
        }

    private:
        void initSerial();
        void readThread();
        void sendCmd(const uint8_t *data, size_t len);

        // 485协议专用发送
        void send485Cmd(uint8_t register_addr, uint8_t rw_flag, uint32_t data0 = 0,
                        uint32_t data1 = 0, uint32_t data2 = 0, uint32_t data3 = 0);

        // ---- 通用指令接口 ----
        void enterSettingMode();
        void turnOnAccel();
        void turnOnGyro();
        void turnOnEuler();
        void turnOnQuat();
        void turnOffQuat();
        void setOutputHZ(FreqMode freq_mode);
        void setOutputInterface(OutputInterface iface);
        void saveImuPara();
        void exitSettingMode();
        void restartImu();

        // ---- 485指令 ----
        void setEulerZero();
        void setActiveMode();
        void setPassiveMode();

        // ---- 数据解析 ----
        bool parseDataFrame(uint8_t *buf, uint8_t len);

        // ---- 工具函数 ----
        void updateTimestamp();

        // ---- Debug ----
        void dumpHex(const uint8_t *data, int len);

    private:
        std::string port_name_;
        int baudrate_;
        ProtocolType protocol_type_;
        std::thread th_;
        std::atomic<bool> stop_flag_{false};

        struct sp_port *port_ = nullptr;

        IMU_Frame rx_frame_{};
        IMU_Data data_;
    };

} // namespace dmbot_serial