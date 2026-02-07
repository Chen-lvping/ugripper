/**
 * IM648 IMU Data Logger (MCAP Version)
 * Usage: ./im648_imu [output_directory]
 *   - If no directory is provided, uses current directory
 *   - Data is saved to im648_data.csv
 *   - Press Ctrl+C to stop
 */

#include "im648_driver.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>
#include <cmath>
#include <mcap/writer.hpp>

#define IM648_DEBUG 0
//#define IM648_DEBUG 1// 定义后会在终端打印原始数据，便于调试

// Global stop flag for signal handling
std::atomic<bool> g_stopFlag(false);

// Signal handler for graceful shutdown
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

std::string create_imu_json(long timestamp_ns, const auto& d) {
    char buffer[512];
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
    // 1. Register signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 2. Parse output directory argument
    std::string outputDir = ".";
    if (argc > 1) outputDir = argv[1];
    else std::cout << "Warning: No output directory provided, using current directory." << std::endl;

    if (outputDir.back() != '/') outputDir += "/";

    // 3. Initialize MCAP Writer
    std::string filename = outputDir + "im648_data.mcap";
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

    std::cout << "IM648 logging to " << filename << " (MCAP format)" << std::endl;

    // 4. Initialize IM648 IMU
    dmbot_serial::Im648Driver im648("/dev/ttyS2", 115200);
    im648.start();
    std::cout << "IM648 initialized and started. Press Ctrl+C to stop." << std::endl;

    uint32_t seq = 0; // 序列号

    // 5. Main loop
    while (!g_stopFlag.load())
    {
        const auto& data = im648.getData();
        
        // Only write when new data is available
        if (data.data_updated.load())
        {

            //debug
            if (IM648_DEBUG)
            {
                std::cout << "--- IM648 Data ---" << std::endl;
                std::cout << data.accx << " " << data.accy << " " << data.accz << " | "
                        << data.gyrox << " " << data.gyroy << " " << data.gyroz << " | "
                        << data.quat_x << " " << data.quat_y << " "
                        << data.quat_z << " " << data.quat_w
                        << std::endl;
            }

            // 获取纳秒级时间戳 (MCAP 标准)
            auto now = std::chrono::system_clock::now();
            auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();
            
            // 构建 JSON Payload
            std::string payload = create_imu_json(timestamp_ns, data);

            // 构建并写入消息
            mcap::Message msg;
            msg.channelId = channel.id;
            msg.sequence = seq++;
            msg.logTime = timestamp_ns;
            msg.publishTime = timestamp_ns;
            msg.data = reinterpret_cast<const std::byte*>(payload.data());
            msg.dataSize = payload.size();

            auto writeStatus = writer.write(msg);
            if (!writeStatus.ok()) {
                std::cerr << "Error writing frame: " << writeStatus.message << std::endl;
            }
            
            // Clear the update flag
            im648.clearDataUpdated();
        }
        
        // Brief sleep to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // 6. Cleanup
    std::cout << "Shutting down IM648..." << std::endl;
    im648.stop();
    
    writer.close();
    std::cout << "MCAP log saved to " << filename << std::endl;

    return 0;
}
