#include "encoder_driver.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <string>

std::atomic<bool> g_stopFlag(false);

// 信号处理函数
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

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
        // 非阻塞读取
        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0)
        {
            // 解析数据，这对于确认命令执行状态通常是必要的
            auto frames_num = encoder->parseReceivedData(readBuf, bytesRead);
            if (frames_num)
            {
                encoder->updateActiveStatus();
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

int main(int argc, char *argv[])
{
    // 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "--- Encoder Zeroing Tool ---" << std::endl;

    // 创建编码器驱动实例 (默认配置)
    EncoderDriver encoder(1, "/dev/ttyS7", 1000000, "Joint1_Encoder");

    // ---------------------------------------------------------
    // 1. 自动波特率配置逻辑
    // ---------------------------------------------------------
    std::cout << "Connecting to encoder..." << std::endl;

    // 尝试 1Mbps
    auto status = encoder.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (status == ConnectStatus::SERIAL_FAIL)
    {
        std::cerr << "Error: Serial port open failed!" << std::endl;
        return -1;
    }
    else if (status == ConnectStatus::NO_RESPONSE)
    {
        std::cout << "1Mbps not responding, trying 115200..." << std::endl;

        // 切换串口波特率到 115200
        encoder.disconnect();
        encoder.resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); 

        status = encoder.connect();
        if (status == ConnectStatus::SUCCESS)
        {
            std::cout << "Encoder found at 115200. Switching encoder to 1Mbps..." << std::endl;

            encoder.setBaudrate(1000000); // 发送指令让编码器切换到 1Mbps
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); 

            std::cout << "Reconnecting host to 1Mbps..." << std::endl;
            encoder.disconnect();
            encoder.resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            status = encoder.connect();
            if (status == ConnectStatus::SUCCESS)
            {
                std::cout << "Baudrate negotiation successful (1Mbps)." << std::endl;
            }
            else
            {
                std::cerr << "Failed to reconnect at 1Mbps after baudrate reset." << std::endl;
                return -1;
            }
        }
        else
        {
            std::cerr << "Failed to connect at 115200. Encoder not responding." << std::endl;
            return -1;
        }
    }
    else
    {
        std::cout << "Encoder connected ready at 1Mbps." << std::endl;
    }

    // ---------------------------------------------------------
    // 2. 启动读取线程 (连接成功后)
    // ---------------------------------------------------------

    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);

    // ---------------------------------------------------------
    // 3. 执行归零逻辑
    // ---------------------------------------------------------
    std::cout << "Sending Zero Position Command..." << std::endl;
    
    // 发送归零指令
    bool zeroSuccess = encoder.setCurrentAsZero();
    
    // 给一点时间让读取线程处理 ACK 和状态更新
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (zeroSuccess)
    {
        // 验证归零结果
        EncoderData state = encoder.getState();
        std::cout << "--------------------------------" << std::endl;
        std::cout << " [SUCCESS] Encoder Zeroed." << std::endl;
        std::cout << " Current Pos (Raw): " << state.currentPosition << std::endl;
        std::cout << " Current Pos (Rad): " << state.currentPositionRad << std::endl;
        std::cout << "--------------------------------" << std::endl;
    }
    else
    {
        std::cerr << " [FAILED] Failed to set zero position! (Or verify failed)" << std::endl;
    }

    // ---------------------------------------------------------
    // 4. 退出清理
    // ---------------------------------------------------------
    g_stopFlag = true;
    if (encoderReadThread.joinable())
        encoderReadThread.join();

    encoder.disconnect();
    return 0;
}
