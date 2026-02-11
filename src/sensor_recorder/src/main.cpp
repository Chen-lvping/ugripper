#include "im648_driver.h"
#include "encoder_driver.h"

#include <mcap/writer.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

std::atomic<bool> g_stopFlag(false);

struct ImuSample {
    float qx;
    float qy;
    float qz;
    float qw;
    float gx;
    float gy;
    float gz;
    float ax;
    float ay;
    float az;
};

struct EncoderSample {
    int32_t raw;
    float rad;
};

static_assert(sizeof(ImuSample) == 40, "ImuSample layout changed");
static_assert(sizeof(EncoderSample) == 8, "EncoderSample layout changed");

void signalHandler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

ImuSample create_imu_sample(const dmbot_serial::IM648_Data &d) {
    ImuSample s{};
    s.qx = d.quat_x;
    s.qy = d.quat_y;
    s.qz = d.quat_z;
    s.qw = d.quat_w;
    s.gx = d.gyrox;
    s.gy = d.gyroy;
    s.gz = d.gyroz;
    s.ax = d.accx;
    s.ay = d.accy;
    s.az = d.accz;
    return s;
}

EncoderSample create_encoder_sample(const EncoderData &d) {
    EncoderSample s{};
    s.raw = d.currentPosition;
    s.rad = d.currentPositionRad;
    return s;
}

void encoderReadThreadFunc(EncoderDriver *encoder) {
    uint8_t readBuf[256];
    while (!g_stopFlag.load()) {
        if (!encoder->isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0) {
            auto frames = encoder->parseReceivedData(readBuf, bytesRead);
            if (frames > 0) {
                encoder->updateActiveStatus();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    std::cout << "Encoder read thread stopped" << std::endl;
}

void encoderRequestThreadFunc(EncoderDriver *encoder) {
    auto next_time = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds(1000); // 1 kHz

    while (!g_stopFlag.load()) {
        next_time += period;
        encoder->requestState(false);
        std::this_thread::sleep_until(next_time);
    }
    std::cout << "Encoder request thread stopped" << std::endl;
}

mcap::Schema buildImuSchema() {
    return mcap::Schema("foxglove.Imu", "jsonschema", R"({
        "type": "object",
        "title": "ImuSampleBinary",
        "description": "little-endian float32[10]: qx,qy,qz,qw,gx,gy,gz,ax,ay,az"
    })");
}

mcap::Schema buildEncoderSchema() {
    return mcap::Schema("EncoderFrame", "json", R"({
        "type": "object",
        "title": "EncoderSampleBinary",
        "description": "little-endian: int32 raw, float32 rad"
    })");
}

int main(int argc, char *argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    fs::path outputDir = (argc > 1) ? fs::path(argv[1]) : fs::path(".");
    std::error_code ec;
    if (!fs::exists(outputDir)) {
        fs::create_directories(outputDir, ec);
        if (ec) {
            std::cerr << "Failed to create output directory: " << ec.message() << std::endl;
            return -1;
        }
    }

    const fs::path outputFile = outputDir / "sensor_data.mcap";
    std::cout << "Recording combined sensor data to " << outputFile << std::endl;

    mcap::McapWriter writer;
    mcap::McapWriterOptions options("sensor_recorder");
    options.noChunking = false;
    options.chunkSize = 256 * 1024; // 256 KiB
    options.compression = mcap::Compression::Lz4;
    options.compressionLevel = mcap::CompressionLevel::Default;
    // Let writer skip compression when a tiny chunk would otherwise grow.
    options.forceCompression = false;
    options.noRepeatedSchemas = true;
    options.noRepeatedChannels = true;
    options.noMessageIndex = true;

    auto status = writer.open(outputFile.string(), options);
    if (!status.ok()) {
        std::cerr << "Failed to open MCAP file: " << status.message << std::endl;
        return -1;
    }

    auto imuSchema = buildImuSchema();
    auto encoderSchema = buildEncoderSchema();
    writer.addSchema(imuSchema);
    writer.addSchema(encoderSchema);

    mcap::Channel imuChannel("imu_raw", "binary", imuSchema.id);
    mcap::Channel encoderChannel("encoder", "binary", encoderSchema.id);
    writer.addChannel(imuChannel);
    writer.addChannel(encoderChannel);

    dmbot_serial::Im648Driver imu("/dev/ttyS2", 115200);
    imu.start();
    std::cout << "IM648 initialized." << std::endl;

    EncoderDriver encoder(1, "/dev/ttyS7", 1000000, "Joint1_Encoder");
    auto connectStatus = encoder.connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (connectStatus == ConnectStatus::SERIAL_FAIL) {
        std::cerr << "Failed to connect encoder serial port." << std::endl;
        return -1;
    }

    if (connectStatus == ConnectStatus::NO_RESPONSE) {
        std::cout << "Encoder not responding at 1Mbps, falling back to 115200..." << std::endl;
        encoder.disconnect();
        encoder.resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        connectStatus = encoder.connect();
        if (connectStatus == ConnectStatus::SUCCESS) {
            std::cout << "Encoder ready at 115200, switching back to 1Mbps." << std::endl;
            encoder.setBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            encoder.disconnect();
            encoder.resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            connectStatus = encoder.connect();
        }
        if (connectStatus != ConnectStatus::SUCCESS) {
            std::cerr << "Encoder failed to respond after baudrate adjustments." << std::endl;
            return -1;
        }
    } else {
        std::cout << "Encoder ready at 1Mbps." << std::endl;
    }

    std::thread encoderReadThread(encoderReadThreadFunc, &encoder);
    std::thread encoderRequestThread(encoderRequestThreadFunc, &encoder);

    auto nextEncoderWrite = std::chrono::steady_clock::now();
    const auto encoderPeriod = std::chrono::microseconds(1000); // 1 kHz

    uint32_t imuSequence = 0;
    uint32_t encoderSequence = 0;

    int warmupCounter = 0;
    while (!g_stopFlag.load()) {
        bool wroteData = false;

        const auto &imuData = imu.getData();
        if (imuData.data_updated.load()) {
            const auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
            const auto sample = create_imu_sample(imuData);

            mcap::Message msg;
            msg.channelId = imuChannel.id;
            msg.sequence = imuSequence++;
            msg.logTime = timestamp_ns;
            msg.publishTime = timestamp_ns;
            msg.data = reinterpret_cast<const std::byte *>(&sample);
            msg.dataSize = sizeof(sample);

            auto writeStatus = writer.write(msg);
            if (!writeStatus.ok()) {
                std::cerr << "Failed to write IMU frame: " << writeStatus.message << std::endl;
                break;
            }

            wroteData = true;

            imu.clearDataUpdated();
        }

        auto nowSteady = std::chrono::steady_clock::now();
        if (nowSteady >= nextEncoderWrite) {
            nextEncoderWrite += encoderPeriod;

            EncoderData state = encoder.getState();
            if (state.currentPosition == 65535) {
                if (warmupCounter++ < 100) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }

            const auto timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
            const auto sample = create_encoder_sample(state);

            mcap::Message msg;
            msg.channelId = encoderChannel.id;
            msg.sequence = encoderSequence++;
            msg.logTime = timestamp_ns;
            msg.publishTime = timestamp_ns;
            msg.data = reinterpret_cast<const std::byte *>(&sample);
            msg.dataSize = sizeof(sample);

            auto writeStatus = writer.write(msg);
            if (!writeStatus.ok()) {
                std::cerr << "Failed to write encoder frame: " << writeStatus.message << std::endl;
                break;
            }

            wroteData = true;
        }

        if (!wroteData) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    g_stopFlag = true;
    std::cout << "Stopping sensors..." << std::endl;

    imu.stop();
    encoder.disconnect();

    if (encoderReadThread.joinable()) {
        encoderReadThread.join();
    }
    if (encoderRequestThread.joinable()) {
        encoderRequestThread.join();
    }

    writer.close();
    std::cout << "Combined MCAP log saved to " << outputFile << std::endl;

    return 0;
}
