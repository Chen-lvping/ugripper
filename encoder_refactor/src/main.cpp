#include "encoder_driver.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <fstream>
#include <iomanip>
#include <csignal>
#include <string>

// 定义调试宏,注释可以取消一些调试信息的打印
// #define DEBUG_READ_FREQ

std::atomic<bool> g_stopFlag(false);

// 新增：信号处理函数，用于接收 Shell 脚本的停止指令
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

// 编码器读取线程函数
void encoderReadThreadFunc(EncoderDriver *encoder)
{
    uint8_t readBuf[256];
#ifdef DEBUG_READ_FREQ
    auto lastTime = std::chrono::steady_clock::now();
    int dataCount = 0;
#endif
    while (!g_stopFlag.load())
    {
        if (!encoder->isConnected())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0)
        {
            auto frames_num = encoder->parseReceivedData(readBuf, bytesRead);
            if (frames_num)
            {
                encoder->updateActiveStatus();
                dataCount += frames_num;
            }

#ifdef DEBUG_READ_FREQ
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastTime).count();
            if (elapsed >= 1.0)
            {
                std::cout << "[DEBUG] Encoder Read Frequency: " << (dataCount / elapsed) << " Hz" << std::endl;
                dataCount = 0;
                lastTime = now;
            }
#endif
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    std::cout << "Encoder read thread stopped" << std::endl;
}

// 编码器请求线程函数
void encoderRequestThreadFunc(EncoderDriver *encoder)
{
    auto next_time = std::chrono::steady_clock::now();
    auto period = std::chrono::microseconds(980); // 1 kHz = 1ms
    // TODO:不确定什么原因，虽然发送频率1k，但是读取到的数据频率会稍低20hz，通过稍微抬高发送频率缓解，频率不对会导致写数据的时候升采样。当然也可以把传感器频率拉到2k之类的缓解
    size_t count = 0;

    while (!g_stopFlag.load())
    {
        next_time += period; // 下一次请求时间

        encoder->requestState(false); // 发送请求

        // 控制周期
        std::this_thread::sleep_until(next_time);
    }

    std::cout << "Encoder request thread stopped" << std::endl;
}

int main(int argc, char *argv[])
{
    // 新增：注册信号处理，捕获 Ctrl+C 或 kill 信号
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 新增：处理命令行参数，获取输出目录
    std::string outputDir = "."; // 默认为当前目录
    if (argc > 1)
    {
        outputDir = argv[1];
    }
    else
    {
        std::cout << "need output dir" << std::endl;
    }

    // 拼接完整文件路径 (确保 outputDir 后没有多余的斜杠，这里简单拼接)
    // 假设传入路径不带末尾斜杠，或者在这里判断
    if (outputDir.back() != '/')
    {
        outputDir += "/";
    }
    std::string filename = outputDir + "encoder_data.csv";

    // 创建编码器驱动实例
    EncoderDriver encoder(1, "/dev/ttyS7", 1000000, "Joint1_Encoder");

    // 尝试 1Mbps 是否能正常通信
    auto status = encoder.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (status == ConnectStatus::SERIAL_FAIL)
    {
        return -1; // 串口问题
    }
    else if (status == ConnectStatus::NO_RESPONSE)
    {
        std::cout << "1Mbps not responding, trying 115200..." << std::endl;

        // 切换串口波特率到 115200
        encoder.disconnect();
        encoder.resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        status = encoder.connect();
        if (status == ConnectStatus::SUCCESS)
        {
            std::cout << "Encoder ready at 115200." << std::endl;

            encoder.setBaudrate(1000000); // 发送指令切换到 1Mbps
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            encoder.disconnect();
            encoder.resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            status = encoder.connect();
            if (status == ConnectStatus::SUCCESS)
            {
                std::cout << "Reset baudrate to 1Mbps OK." << std::endl;
            }
            else
            {
                std::cerr << "Failed to reconnect at 1Mbps after baudrate reset." << std::endl;
            }
        }
        else
        {
            std::cerr << "Failed to connect at 115200, unknown encoder status." << std::endl;
        }
    }
    else
    {
        std::cout << "Encoder ready at 1Mbps." << std::endl;
    }

    if (!encoder.setCurrentAsZero())
    {
        // TODO: 归零逻辑还没写
        std::cerr << "Failed to set zero position" << std::endl;
    }

    // 打开文件
    std::ofstream csvFile(filename);
    if (!csvFile.is_open())
    {
        std::cerr << "Failed to open file for logging: " << filename << std::endl;
        return -1; // 如果文件无法创建，应该退出
    }
    else
    {
        csvFile << "Timestamp_us,Position_Raw,Position_Rad" << std::endl;
        std::cout << "Logging data to " << filename << std::endl;
    }

    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);
    std::thread encoderRequestThread(encoderRequestThreadFunc, &encoder);

    double print_hz = 10.0;
    auto printPeriod = std::chrono::microseconds(static_cast<int>(1e6 / print_hz));

    std::cout << "Starting encoder monitoring... Waiting for stop signal." << std::endl;

    // 主循环：固定 1 kHz 写入 CSV
    auto next_time = std::chrono::steady_clock::now();
    auto writePeriod = std::chrono::microseconds(1000); // 1 ms = 1 kHz
    while (!g_stopFlag.load())
    {
        try
        {
            next_time += writePeriod; // 下一帧目标时间

            // 获取编码器状态
            EncoderData state = encoder.getState();

            // 获取系统时间戳（微秒）
            auto now = std::chrono::system_clock::now();
            auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    now.time_since_epoch())
                                    .count();

            // 写入 CSV
            if (csvFile.is_open())
            {
                csvFile << timestamp_us << ","
                        << state.currentPosition << ","
                        << state.currentPositionRad
                        << std::endl;
            }

            // 可选：终端打印，注意频繁打印会降低性能
            // std::cout << "[" << timestamp_us << "] Pos: " << state.currentPositionRad << std::endl;

            // 控制 1 kHz
            // 控制周期
            std::this_thread::sleep_until(next_time);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error in main loop: " << e.what() << std::endl;
            break;
        }
    }

    std::cout << "Shutting down..." << std::endl;
    // g_stopFlag 已经在信号处理或循环结束时置位，无需再次置位，但为了安全：
    g_stopFlag = true;

    if (csvFile.is_open())
    {
        csvFile.close();
        std::cout << "CSV log saved to " << filename << std::endl;
    }

    if (encoderReadThread.joinable())
        encoderReadThread.join();
    if (encoderRequestThread.joinable())
        encoderRequestThread.join();

    encoder.disconnect();
    return 0;
}
