#include "imu_driver.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <cmath>
#include <unistd.h>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <csignal>
#include <mcap/writer.hpp>

// --- 全局控制变量 ---
std::atomic<bool> g_stopFlag(false);
//#define VIEW_IMU_LOG // 定义此宏可在终端查看实时数据（注意：高频IO可能影响控制循环抖动）

// --- 信号处理函数 ---
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

// --- 可视化辅助函数：生成进度条 (保持不变) ---
std::string getVisualBar(float value, float range, int width)
{
    std::string bar = "[";
    float ratio = value / range;
    if (ratio > 1.0f)
        ratio = 1.0f;
    if (ratio < -1.0f)
        ratio = -1.0f;

    int fill_len = static_cast<int>(std::abs(ratio) * width);
    std::string left_space(width, ' ');
    std::string right_space(width, ' ');

    if (ratio < 0)
    {
        for (int i = 0; i < fill_len; ++i)
            left_space[width - 1 - i] = '#';
    }
    else
    {
        for (int i = 0; i < fill_len; ++i)
            right_space[i] = '#';
    }
    bar += left_space + "|" + right_space + "]";
    return bar;
}

// --- UDP 数据包结构 ---
struct __attribute__((packed)) ImuPacket
{
    float w, x, y, z;
};

// --- JSON 序列化 ---
std::string create_imu_json(long timestamp_ns, const auto& d) {
    char buffer[512];
    // 使用 snprintf 保证格式化速度和安全性
    snprintf(buffer, sizeof(buffer), 
        "{"
            "\"frame_id\":\"imu_link\","
            "\"orientation\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"w\":%.6f},"
            "\"angular_velocity\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
            "\"linear_acceleration\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}"
        "}",
        d.quat_x, d.quat_y, d.quat_z, d.quat_w,
        d.gyrox, d.gyroy, d.gyroz,
        d.accx, d.accy, d.accz
    );

    return std::string(buffer);
}

int main(int argc, char *argv[])
{
    // 1. 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 2. 参数解析
    std::string outputDir = ".";
    bool enable_vis = false;

    if (argc > 1)
    {
        if (std::string(argv[1]) != "-view") outputDir = argv[1];
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-view") == 0) enable_vis = true;
        }
    }
    else
    {
        std::cout << "Warning: No output directory provided, using current directory." << std::endl;
    }

    if (outputDir.back() != '/') outputDir += "/";
    std::string filename = outputDir + "imu_data.mcap";

    // 3. 初始化 MCAP Writer
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("");
    options.compression = mcap::Compression::Lz4; // 开启压缩

    auto status = writer.open(filename, options);
    if (!status.ok())
    {
        std::cerr << "Failed to open MCAP file: " << filename << std::endl;
        return -1;
    }

    // 注册 Foxglove IMU Schema
    mcap::Schema schema("foxglove.Imu", "jsonschema", R"({
        "type": "object",
        "properties": {
            "frame_id": { "type": "string" },
            "orientation": {
                "type": "object",
                "properties": { "x": {"type":"number"}, "y": {"type":"number"}, "z": {"type":"number"}, "w": {"type":"number"} }
            },
            "angular_velocity": {
                "type": "object",
                "properties": { "x": {"type":"number"}, "y": {"type":"number"}, "z": {"type":"number"} }
            },
            "linear_acceleration": {
                "type": "object",
                "properties": { "x": {"type":"number"}, "y": {"type":"number"}, "z": {"type":"number"} }
            }
        }
    })");
    writer.addSchema(schema);

    // 注册 Channel
    mcap::Channel channel("imu_raw", "json", schema.id);
    writer.addChannel(channel);

    std::cout << "Logging IMU data to " << filename << " (MCAP format)" << std::endl;

    // 4. 初始化 UDP
    int sock = -1;
    struct sockaddr_in addr;
    if (enable_vis)
    {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0)
        {
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(9999);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            std::cout << "[Info] UDP Visualization Enabled (127.0.0.1:9999)\n";
        }
    }

    // 5. 初始化 IMU
    dmbot_serial::DmImu imu("/dev/ttyS2", 921600, dmbot_serial::DmImu::ProtocolType::RS485);
    imu.start();

    std::cout << "Starting IMU monitoring (1kHz logging)... Press Ctrl+C to stop." << std::endl;
    std::cout << std::fixed << std::setprecision(3);

    // 6. 主循环控制变量
    auto next_time = std::chrono::steady_clock::now();
    auto loopPeriod = std::chrono::microseconds(1000); // 1 kHz = 1000us

    // 用于降低打印频率的计数器 (1000Hz loop / 50 = 20Hz print)
    int printDivisor = 0;
    const int printThreshold = 50;
    uint32_t seq = 0; // 序列号

    while (!g_stopFlag.load())
    {
        next_time += loopPeriod; // 设定下一次唤醒时间点

        // --- 获取数据 ---
        auto d = imu.getData();

        // 获取时间戳
        auto now = std::chrono::system_clock::now();
        auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now.time_since_epoch())
                                .count();

        // --- 写入 MCAP (1kHz) ---
        {
            // 构建 Payload
            std::string payload = create_imu_json(timestamp_ns, d);

            // 构建消息
            mcap::Message msg;
            msg.channelId = channel.id;
            msg.sequence = seq++;
            msg.logTime = timestamp_ns;
            msg.publishTime = timestamp_ns;
            msg.data = reinterpret_cast<const std::byte*>(payload.data());
            msg.dataSize = payload.size();

            // 写入
            auto writeStatus = writer.write(msg);
            if (!writeStatus.ok()) {
                std::cerr << "Error writing MCAP: " << writeStatus.message << std::endl;
            }
        }


        // --- 终端打印与UDP (降频处理: 20Hz) ---
        // 频繁打印 IO 会阻塞线程，导致无法维持 1kHz 记录频率，所以必须降频
        printDivisor++;
        if (printDivisor >= printThreshold)
        {
            printDivisor = 0;

#ifdef VIEW_IMU_LOG
            // 清屏并回位 (注意：这在 log 模式下可能会干扰查看，建议仅在调试时开启)
            long timestamp_us = timestamp_ns / 1000;
            
            std::cout << "\033[2J\033[H";
            std::cout << "================= IMU Data Dashboard =================\n";
            std::cout << std::setfill(' ') << std::right;

            std::cout << "Time: " << timestamp_us << " us\n";
            std::cout << "ACC  (g): " << "X:" << std::setw(8) << d.accx << " Y:" << std::setw(8) << d.accy << " Z:" << std::setw(8) << d.accz << "\n";
            std::cout << "GYRO (d/s): " << "X:" << std::setw(8) << d.gyrox << " Y:" << std::setw(8) << d.gyroy << " Z:" << std::setw(8) << d.gyroz << "\n";
            std::cout << "QUAT    : " << "X:" << std::setw(7) << d.quat_x << " Y:" << std::setw(7) << d.quat_y << " Z:" << std::setw(7) << d.quat_z << " W:" << std::setw(7) << d.quat_w << "\n";
            std::cout << "------------------------------------------------------\n";
            std::cout << "ROLL : " << std::setw(8) << d.roll << " deg " << getVisualBar(d.roll, 90.0f, 15) << "\n";
            std::cout << "PITCH: " << std::setw(8) << d.pitch << " deg " << getVisualBar(d.pitch, 90.0f, 15) << "\n";
            std::cout << "YAW  : " << std::setw(8) << d.yaw << " deg " << getVisualBar(d.yaw, 180.0f, 15) << "\n";
            std::cout << "======================================================\n";
#endif

            // UDP 发送也放在低频循环里
            if (enable_vis && sock >= 0)
            {
                ImuPacket packet;
                packet.w = d.quat_w;
                packet.x = d.quat_x;
                packet.y = d.quat_y;
                packet.z = d.quat_z;
                sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
            }
        }

        // --- 周期控制 ---
        std::this_thread::sleep_until(next_time);
    }

    // --- 清理资源 ---
    std::cout << "Shutting down IMU..." << std::endl;

    writer.close();
    std::cout << "MCAP log saved." << std::endl;

    if (sock >= 0) close(sock);

    return 0;
}
