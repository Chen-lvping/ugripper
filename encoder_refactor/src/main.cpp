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
#include <cstring>
#include <mcap/writer.hpp>

// 定义调试宏,注释可以取消一些调试信息的打印
#define DEBUG_READ_FREQ

std::atomic<bool> g_stopFlag(false);

// 信号处理函数
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

std::string create_encoder_json(const EncoderData& d) {
    char buffer[256];
    // 使用 snprintf 格式化数据，比 string 拼接更高效且格式可控
    snprintf(buffer, sizeof(buffer), 
        "{"
            "\"position_raw\":%d,"
            "\"position_rad\":%.6f"
        "}",
        d.currentPosition, d.currentPositionRad
    );
    return std::string(buffer);
}

// 编码器读取线程函数
void encoderReadThreadFunc(EncoderDriver *encoder)
{
    uint8_t readBuf[256];
    int dataCount = 0;
#ifdef DEBUG_READ_FREQ
    auto lastTime = std::chrono::steady_clock::now();
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
            if (elapsed >= 0.01)
            {
                std::cout << "[DEBUG] Encoder Read Frequency: " << (dataCount / elapsed) << " Hz" << std::endl;
                std::cout << "[DEBUG] Last frame position (raw): " << encoder->getState().currentPosition 
                          << ", position (rad): " << encoder->getState().currentPositionRad << std::endl;
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
    auto period = std::chrono::microseconds(1000); // 1 kHz = 1ms
    
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
    // 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 处理命令行参数
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
    std::string filename = outputDir + "encoder_data.mcap";

    // --- 1. 初始化 MCAP Writer ---
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("");
    options.compression = mcap::Compression::Lz4; // 参考 imu: 开启压缩

    auto writerStatus = writer.open(filename, options); // 修改变量名防止重定义
    if (!writerStatus.ok())
    {
        std::cerr << "Failed to open MCAP file: " << filename << std::endl;
        return -1;
    }

    // --- 2. 注册 Schema ---
    mcap::Schema schema("EncoderFrame", "json", R"({
        "type": "object",
        "properties": {
            "position_raw": { "type": "integer" },
            "position_rad": { "type": "number" }
        }
    })");
    writer.addSchema(schema);

    // --- 3. 注册 Channel ---
    mcap::Channel channel("/encoder", "json", schema.id);
    writer.addChannel(channel);

    std::cout << "Logging data to " << filename << " (MCAP format)" << std::endl;

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

    // 启动线程
    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);
    std::thread encoderRequestThread(encoderRequestThreadFunc, &encoder);

    std::cout << "Starting encoder monitoring... Waiting for stop signal." << std::endl;

    auto next_time = std::chrono::steady_clock::now();
    auto writePeriod = std::chrono::microseconds(1000); // 1 ms = 1 kHz
    uint32_t sequenceId = 0;

    int wait_counts = 0;
    while (!g_stopFlag.load())//预热
    {
        EncoderData state = encoder.getState();

        // 只要读到的值不是 65535，说明读取线程已经工作并更新了数据
        if (state.currentPosition != 65535) {break;}

        // 超时保护：如果等了 1秒 (100次 * 10ms) 还没数据，也强行开始，避免死锁
        if (wait_counts++ > 100) {
            std::cerr << "Warning: No valid data received after 1s. Starting anyway." << std::endl;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // --- 5. 主循环 ---
    while (!g_stopFlag.load())
    {
        try
        {
            next_time += writePeriod; // 下一帧目标时间

            // 获取编码器状态
            EncoderData state = encoder.getState();
            
            // 获取纳秒级时间戳 (MCAP 标准)
            auto now = std::chrono::system_clock::now();
            auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();

            // 构建 JSON Payload (使用辅助函数)
            std::string payload = create_encoder_json(state);

            // 构建并写入消息
            mcap::Message msg;
            msg.channelId = channel.id;
            msg.sequence = sequenceId++;
            msg.logTime = timestamp_ns;     // 采样时间
            msg.publishTime = timestamp_ns; // 发布时间
            msg.data = reinterpret_cast<const std::byte*>(payload.data());
            msg.dataSize = payload.size();

            auto writeRes = writer.write(msg);
            if (!writeRes.ok()) {
                 std::cerr << "Failed to write frame: " << writeRes.message << std::endl;
            }

            std::this_thread::sleep_until(next_time);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error in main loop: " << e.what() << std::endl;
            break;
        }
    }

    std::cout << "Shutting down..." << std::endl;
    // --- 6. 清理 ---
    // g_stopFlag 已经在信号处理或循环结束时置位，无需再次置位，但为了安全：
    g_stopFlag = true;
    
    writer.close();
    std::cout << "MCAP log saved to " << filename << std::endl;

    if (encoderReadThread.joinable())
        encoderReadThread.join();
    if (encoderRequestThread.joinable())
        encoderRequestThread.join();

    encoder.disconnect();
    return 0;
}
