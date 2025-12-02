#include "imu_driver.h"
#include <chrono>
#include <iomanip>
#include <cstring>
#include <iostream>

using namespace dmbot_serial;

// ---------------------- 工具函数 --------------------------
static void msleep(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static uint64_t getCurrentTimestamp()
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

// ---------------------- 构造 / 析构 ------------------------

DmImu::DmImu(const std::string &port_name, int baudrate, ProtocolType protocol)
    : port_name_(port_name), baudrate_(baudrate), protocol_type_(protocol), stop_flag_(false)
{
    initSerial();

    // USB初始化流程
    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        enterSettingMode();
        turnOnAccel();
        turnOnGyro();
        turnOnEuler();
        setOutputHZ(FreqMode::HZ1000);
        setOutputInterface(OutputInterface::USB);
        saveImuPara();
        exitSettingMode();
    }
    else if (protocol_type_ == ProtocolType::RS485)
    {
        setOutputHZ(FreqMode::HZ500);
        setOutputInterface(OutputInterface::RS485);
        setActiveMode();
        saveImuPara();
    }

    // 等待IMU完全初始化
    msleep(200);

    // 清空串口缓冲区
    sp_flush(port_, SP_BUF_BOTH);
    std::cout << "IMU initialization completed" << std::endl;
}

DmImu::~DmImu()
{
    stop();
    if (port_)
        sp_close(port_);
}

// ---------------------- 启动停止 ------------------------

void DmImu::start()
{
    stop_flag_ = false;
    th_ = std::thread(&DmImu::readThread, this);
}

void DmImu::stop()
{
    stop_flag_ = true;
    if (th_.joinable())
        th_.join();
}

// ---------------------- 串口初始化 ------------------------

void DmImu::initSerial()
{
    enum sp_return r = sp_get_port_by_name(port_name_.c_str(), &port_);
    if (r != SP_OK)
    {
        std::cerr << "Cannot find serial port " << port_name_ << std::endl;
        exit(1);
    }

    if (sp_open(port_, SP_MODE_READ_WRITE) != SP_OK)
    {
        std::cerr << "Cannot open port " << port_name_ << std::endl;
        exit(1);
    }

    sp_set_baudrate(port_, baudrate_);
    sp_set_bits(port_, 8);
    sp_set_parity(port_, SP_PARITY_NONE);
    sp_set_stopbits(port_, 1);
    sp_set_flowcontrol(port_, SP_FLOWCONTROL_NONE);

    if (!port_)
    {
        std::cerr << "Serial port not properly opened" << std::endl;
        exit(1);
    }

    std::cout << "IMU serial port " << port_name_ << " opened successfully" << std::endl;
    std::cout << "Protocol: " << (protocol_type_ == ProtocolType::USB_TTY ? "USB-TTY" : "RS485") << std::endl;
}

// ---------------------- 发送指令 ------------------------

void DmImu::sendCmd(const uint8_t *data, size_t len)
{
    if (!port_)
        return;

    for (int i = 0; i < 5; ++i)
    {
        sp_blocking_write(port_, data, len, 50);

        // 打印指令内容
        // std::cout << "[IMU] Sent command: ";
        // for (size_t j = 0; j < len; ++j)
        // {
        //     std::cout << std::hex << std::uppercase << std::setw(2)
        //               << std::setfill('0') << static_cast<int>(data[j]) << " ";
        // }
        // std::cout << std::dec << std::endl; // 恢复十进制输出
        // 打印指令内容 end

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 485协议专用命令发送
void DmImu::send485Cmd(uint8_t register_addr, uint8_t rw_flag, uint32_t data0,
                       uint32_t data1, uint32_t data2, uint32_t data3)
{
    IMU_485_Cmd_Frame frame;
    memset(&frame, 0, sizeof(frame));

    frame.header = 0xA5;
    frame.data_type = 0x0C; // 指令类型
    frame.slave_id = 0x01;  // 从机ID
    frame.register_addr = register_addr;
    frame.rw_flag = rw_flag;
    frame.data0 = data0;
    frame.data1 = data1;
    frame.data2 = data2;
    frame.data3 = data3;
    frame.ack_flag = 0;
    frame.reserved = 0;
    frame.end = 0x5A;

    sendCmd((uint8_t *)&frame, sizeof(frame));
}

// USB-TTY 指令
void DmImu::enterSettingMode()
{
    uint8_t c[4] = {0xAA, 0x06, 0x01, 0x0D};
    sendCmd(c, 4);
}

void DmImu::turnOnAccel()
{
    uint8_t c[4] = {0xAA, 0x01, 0x14, 0x0D};
    sendCmd(c, 4);
}

void DmImu::turnOnGyro()
{
    uint8_t c[4] = {0xAA, 0x01, 0x15, 0x0D};
    sendCmd(c, 4);
}

void DmImu::turnOnEuler()
{
    uint8_t c[4] = {0xAA, 0x01, 0x16, 0x0D};
    sendCmd(c, 4);
}

void DmImu::turnOnQuat()
{
    uint8_t c[4] = {0xAA, 0x01, 0x17, 0x0D};
    sendCmd(c, 4);
}

void DmImu::turnOffQuat()
{
    uint8_t c[4] = {0xAA, 0x01, 0x07, 0x0D};
    sendCmd(c, 4);
}

void DmImu::setOutputHZ(FreqMode freq_mode)
{
    uint8_t mode_val = static_cast<uint8_t>(freq_mode);

    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        uint8_t c[5] = {0xAA, 0x02, mode_val, 0x0D};
        sendCmd(c, 4);
    }
    else if (protocol_type_ == ProtocolType::RS485)
    {
        int freq = 0;
        switch (freq_mode)
        {
        case FreqMode::HZ100:
            freq = 100;
            break;
        case FreqMode::HZ125:
            freq = 125;
            break;
        case FreqMode::HZ200:
            freq = 200;
            break;
        case FreqMode::HZ250:
            freq = 250;
            break;
        case FreqMode::HZ500:
            freq = 500;
            break;
        case FreqMode::HZ1000:
            freq = 1000;
            break;
        }

        int period_ms = 1000 / freq;

        send485Cmd(8, 1, period_ms);
    }
}

void DmImu::setOutputInterface(OutputInterface iface)
{
    uint8_t iface_val = static_cast<uint8_t>(iface);

    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        uint8_t c[4] = {0xAA, 0x03, iface_val, 0x0D};
        sendCmd(c, 4);
    }
    else if (protocol_type_ == ProtocolType::RS485)
    {
        send485Cmd(9, 1, iface_val);
    }
}

void DmImu::enterGyroCalib()
{
    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        uint8_t c[4] = {0xAA, 0x03, 0x02, 0x0D};
        sendCmd(c, 4);
    }
    else if (protocol_type_ == ProtocolType::RS485)
    {
        send485Cmd(3, 1);
    }
}

void DmImu::enterAccelCalib()
{
    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        uint8_t c[4] = {0xAA, 0x03, 0x03, 0x0D};
        sendCmd(c, 4);
    }
    else if (protocol_type_ == ProtocolType::RS485)
    {
        send485Cmd(4, 1);
    }
}

void DmImu::saveImuPara()
{
    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        uint8_t c[4] = {0xAA, 0x03, 0x01, 0x0D};
        sendCmd(c, 4);
    }
    else
    {
        send485Cmd(1, 1); // 485模式保存参数
    }
}

void DmImu::exitSettingMode()
{
    uint8_t c[4] = {0xAA, 0x06, 0x00, 0x0D};
    sendCmd(c, 4);
}

void DmImu::restartImu()
{
    if (protocol_type_ == ProtocolType::USB_TTY)
    {
        uint8_t c[4] = {0xAA, 0x00, 0x00, 0x0D};
        sendCmd(c, 4);
    }
    else
    {
        send485Cmd(0, 1); // 485模式重启
    }
}

void DmImu::setEulerZero()
{
    send485Cmd(2, 1);
}

void DmImu::setActiveMode()
{
    send485Cmd(6, 1, 1); // 设置为主动模式
}

void DmImu::setPassiveMode()
{
    send485Cmd(6, 1, 0); // 设置为被动模式
}

// ---------------------- 数据解析 ------------------------

bool DmImu::parseDataFrame(uint8_t *buf, uint8_t len)
{
    if (len < 4 || buf[len - 1] != 0x0A)
        return false;

    uint16_t crc;
    uint8_t reg = buf[3];

    switch (reg)
    {
    case 0x01: // Acc
        // reinterpret_cast 读取 float 而不是 int32
        data_.accx = *reinterpret_cast<float *>(buf + 4);
        data_.accy = *reinterpret_cast<float *>(buf + 8);
        data_.accz = *reinterpret_cast<float *>(buf + 12);
        crc = *reinterpret_cast<uint16_t *>(buf + 16);
        break;

    case 0x02: // Gyro
        data_.gyrox = *reinterpret_cast<float *>(buf + 4);
        data_.gyroy = *reinterpret_cast<float *>(buf + 8);
        data_.gyroz = *reinterpret_cast<float *>(buf + 12);
        crc = *reinterpret_cast<uint16_t *>(buf + 16);
        break;

    case 0x03: // Euler
        data_.roll = *reinterpret_cast<float *>(buf + 4);
        data_.pitch = *reinterpret_cast<float *>(buf + 8);
        data_.yaw = *reinterpret_cast<float *>(buf + 12);
        crc = *reinterpret_cast<uint16_t *>(buf + 16);
        break;

    case 0x04: // Quat
        data_.quat_x = *reinterpret_cast<float *>(buf + 4);
        data_.quat_y = *reinterpret_cast<float *>(buf + 8);
        data_.quat_z = *reinterpret_cast<float *>(buf + 12);
        data_.quat_w = *reinterpret_cast<float *>(buf + 16);
        crc = *reinterpret_cast<uint16_t *>(buf + 20); // Quat 后面 CRC 偏移要 +20 而不是 +30
        break;

    default:
        return false;
    }

    if (Get_CRC16(buf, len - 3, CRC16Type::ISO13239) != crc)
    {
        std::cout << "[IMU] ERROR CRC failed " << std::hex << Get_CRC16(buf, len - 3, CRC16Type::ISO13239) << "   " << crc << std::dec << std::endl;
        return false;
    }

    updateTimestamp();
    return true;
}

void DmImu::updateTimestamp()
{
    data_.timestamp = getCurrentTimestamp();
}

// ---------------------- 报文 dump ------------------------

void DmImu::dumpHex(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
    {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

// ---------------------- 数据线程 ------------------------

void DmImu::readThread()
{
    uint8_t buf[128];
    int error_count = 0;

    bool debug_print_frame = false; // 调试用开关

    std::cout << "IMU read thread started" << std::endl;

    while (!stop_flag_)
    {
        // 读取最小帧头（4 bytes）
        int n = sp_blocking_read(port_, buf, 4, 50);
        if (n != 4)
            continue;
        if (buf[0] == 0x55 && buf[1] == 0xAA && buf[2] == 0x01)
        {

            // 判断帧类型，通过 reg 字段决定剩余长度
            uint8_t frame_len = 0;
            uint8_t reg = buf[3]; // 第四个字节是寄存器/类型

            switch (reg)
            {
            case 0x01:          // acc
                frame_len = 19; // 包含前4字节
                break;
            case 0x02:          // gyro
                frame_len = 19; // 包含前4字节
                break;
            case 0x03:          // euler
                frame_len = 19; // 包含前4字节
                break;
            case 0x04:          // quat
                frame_len = 23; // 包含前4字节
                break;
            default:
                error_count++;
                if (error_count < 5)
                    std::cout << "[IMU] Unknown reg: " << std::hex << (int)reg << std::dec << std::endl;
                continue;
            }
            // 读取剩余数据
            n = sp_blocking_read(port_, buf + 4, frame_len - 4, 50); // 5ms 超时
            if (n != (int)(frame_len - 4))
                continue;

            // 调试打印完整帧
            if (debug_print_frame)
            {
                std::cout << "[IMU] Full frame (" << static_cast<int>(frame_len) << " bytes): ";
                for (size_t i = 0; i < frame_len; ++i)
                    printf("%02X ", buf[i]);
                printf("\n");
            }

            // USB/RS485 通用解析
            bool parsed = parseDataFrame(buf, frame_len);

            if (parsed)
            {
                error_count = 0;
            }
            else
            {
                error_count++;
                if (error_count > 500)
                {
                    std::cerr << "IMU sync lost, attempting reinit..." << std::endl;
                    for (int i = 0; i < 5; i++)
                    {
                        restartImu();
                        msleep(10);
                    }
                    msleep(200);
                    sp_flush(port_, SP_BUF_BOTH);
                    error_count = 0;
                }
            }
        }
    }
}

IMU_Data DmImu::getData()
{
    return data_;
}