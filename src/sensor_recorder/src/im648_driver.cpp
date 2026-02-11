#include "im648_driver.h"
#include "im648_CMD.h"
#include <chrono>
#include <iomanip>
#include <cstring>
#include <iostream>

using namespace dmbot_serial;

// Forward declaration - will be set by Im648Driver
static struct sp_port *g_im648_port = nullptr;

// Global data structure pointer for im648_CMD.cpp to update
// This is declared as extern in im648_CMD.h
dmbot_serial::IM648_Data *g_im648_data_ptr = nullptr;

// UART write function for im648_CMD.cpp
int im648_UART_Write(const U8 *buf, int Len)
{
    if (!g_im648_port)
        return 0;
    return sp_blocking_write(g_im648_port, buf, Len, 50);
}

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

Im648Driver::Im648Driver(const std::string &port_name, int baudrate)
    : port_name_(port_name), baudrate_(baudrate), stop_flag_(false)
{
    initSerial();
    
    // Set global port pointer for im648_UART_Write
    g_im648_port = port_;
    
    // Set global data pointer
    g_im648_data_ptr = &data_;
    
    // Set device address (broadcast)
    im648_targetDeviceAddress = 255;
    
    // Set device parameters (参考imuDo.cpp)
    // Cmd_12(5, 255, 0,  0, 2, 200, 1, 3, 5, 0x0026);
    im648_Cmd_12(5, 255, 0, 0, 2, 200, 1, 3, 5, 0x0026); // 设置参数，200Hz频率
    msleep(100);
    im648_Cmd_03(); // 唤醒传感器
    msleep(100);
    im648_Cmd_19(); // 开启数据主动上报
    msleep(100);
    
    // Wait for IMU to initialize
    msleep(200);
    
    // Clear serial buffer
    sp_flush(port_, SP_BUF_BOTH);
    std::cout << "IM648 initialization completed" << std::endl;
}

Im648Driver::~Im648Driver()
{
    stop();
    if (port_)
        sp_close(port_);
    g_im648_port = nullptr;
    g_im648_data_ptr = nullptr;
}

// ---------------------- 启动停止 ------------------------

void Im648Driver::start()
{
    stop_flag_ = false;
    th_ = std::thread(&Im648Driver::readThread, this);
}

void Im648Driver::stop()
{
    stop_flag_ = true;
    if (th_.joinable())
        th_.join();
}

// ---------------------- 串口初始化 ------------------------

void Im648Driver::initSerial()
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

    std::cout << "IM648 serial port " << port_name_ << " opened successfully" << std::endl;
}

// ---------------------- 读取线程 ------------------------

void Im648Driver::readThread()
{
    unsigned char tmpdata[4096];
    
    std::cout << "IM648 read thread started" << std::endl;

    while (!stop_flag_.load())
    {
        // Non-blocking read from serial port
        int n = sp_nonblocking_read(port_, tmpdata, sizeof(tmpdata));
        
        if (n > 0)
        {
            // Process data byte by byte (参考imuDo.cpp)
            for(int i = 0; i < n; i++)
            {
                im648_Cmd_GetPkt(tmpdata[i]); // Callback will update data_
            }
        }
        else if (n < 0)
        {
            // Error reading
            std::cerr << "Error reading from IM648 serial port" << std::endl;
        }
        
        // Brief sleep to avoid high CPU usage
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// ---------------------- 获取数据 ------------------------

const IM648_Data& Im648Driver::getData() const
{
    return data_;
}

void Im648Driver::clearDataUpdated()
{
    data_.data_updated.store(false);
}


