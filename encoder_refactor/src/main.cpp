#include "encoder_driver.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

std::atomic<bool> g_stopFlag(false);

// 编码器读取线程函数
void encoderReadThreadFunc(EncoderDriver *encoder)
{
    uint8_t readBuf[256];

    while (!g_stopFlag.load())
    {
        if (!encoder->isConnected())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 非阻塞读取串口数据
        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0)
        {
            encoder->parseReceivedData(readBuf, bytesRead);
            encoder->updateActiveStatus();
        }
        else
        {
            // 没有数据时短暂休眠，避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    std::cout << "Encoder read thread stopped" << std::endl;
}

// 编码器请求线程函数（只负责发送请求）
void encoderRequestThreadFunc(EncoderDriver *encoder)
{
    auto period = std::chrono::microseconds(1000); // 1 kHz

    while (!g_stopFlag.load())
    {
        // 只发送请求，不读取数据
        encoder->requestState(false);
        // encoder->updateActiveStatus();
        std::this_thread::sleep_for(period);
    }

    std::cout << "Encoder request thread stopped" << std::endl;
}

int main()
{
    // 创建编码器驱动实例
    EncoderDriver encoder(1, "/dev/ttyS7", 115200, "Joint1_Encoder");

    // 连接编码器
    if (!encoder.connect())
    {
        std::cerr << "Failed to connect to encoder" << std::endl;
        return -1;
    }

    // 设置零位
    // TODO:启动时添加标志位，用按键触发标零
    if (!encoder.setCurrentAsZero())
    {
        std::cerr << "Failed to set zero position" << std::endl;
    }

    // 启动编码器读取线程
    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);

    // 启动编码器请求线程
    std::thread encoderRequestThread(encoderRequestThreadFunc, &encoder);

    // 可调打印频率
    double print_hz = 10.0; // 默认 10Hz
    auto printPeriod = std::chrono::microseconds(static_cast<int>(1e6 / print_hz));

    std::cout << "Starting encoder monitoring... Press Ctrl+C to stop." << std::endl;

    // 主线程打印
    while (true)
    {
        try
        {
            auto start = std::chrono::steady_clock::now();
            EncoderData state = encoder.getState();

            std::cout << "Position: " << state.currentPosition
                      << " rad: " << state.currentPositionRad
                      << std::endl;

            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < printPeriod)
                std::this_thread::sleep_for(printPeriod - elapsed);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error in main loop: " << e.what() << std::endl;
            break;
        }
    }

    // 清理资源
    std::cout << "Shutting down..." << std::endl;
    g_stopFlag = true;

    // 等待线程结束
    if (encoderReadThread.joinable())
        encoderReadThread.join();

    if (encoderRequestThread.joinable())
        encoderRequestThread.join();

    encoder.disconnect();

    std::cout << "Application terminated successfully." << std::endl;
    return 0;
}