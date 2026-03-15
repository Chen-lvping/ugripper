#include "im648_driver.h"
#include "encoder_driver.h"

#include <mcap/writer.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
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

struct SensorSideConfig {
    std::string label;
    std::string imuPort;
    std::string encoderPort;
};

struct SideWriter {
    SensorSideConfig config;
    fs::path outputFile;
    mcap::McapWriter writer;
    mcap::Schema imuSchema;
    mcap::Schema encoderSchema;
};

struct ImuRuntime {
    SensorSideConfig config;
    std::unique_ptr<dmbot_serial::Im648Driver> driver;
    mcap::Channel channel;
    uint32_t sequence = 0;
    bool firstSampleLogged = false;
};

struct EncoderRuntime {
    SensorSideConfig config;
    std::unique_ptr<EncoderDriver> driver;
    mcap::Channel channel;
    std::thread readThread;
    std::thread requestThread;
    uint32_t sequence = 0;
    bool firstSampleLogged = false;
    int warmupCounter = 0;
    bool connected = false;
};

static_assert(sizeof(ImuSample) == 40, "ImuSample layout changed");
static_assert(sizeof(EncoderSample) == 8, "EncoderSample layout changed");

void signalHandler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

ImuSample create_imu_sample(const dmbot_serial::IM648_Data &d) {
    ImuSample sample{};
    sample.qx = d.quat_x;
    sample.qy = d.quat_y;
    sample.qz = d.quat_z;
    sample.qw = d.quat_w;
    sample.gx = d.gyrox;
    sample.gy = d.gyroy;
    sample.gz = d.gyroz;
    sample.ax = d.accx;
    sample.ay = d.accy;
    sample.az = d.accz;
    return sample;
}

EncoderSample create_encoder_sample(const EncoderData &d) {
    EncoderSample sample{};
    sample.raw = d.currentPosition;
    sample.rad = d.currentPositionRad;
    return sample;
}

void encoderReadThreadFunc(EncoderDriver *encoder, const std::string &label) {
    uint8_t readBuf[256];
    while (!g_stopFlag.load()) {
        if (!encoder->isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int bytesRead = encoder->readDataNonBlocking(readBuf, sizeof(readBuf));
        if (bytesRead > 0) {
            const auto frames = encoder->parseReceivedData(readBuf, bytesRead);
            if (frames > 0) {
                encoder->updateActiveStatus();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    std::cout << "Encoder read thread stopped for " << label << std::endl;
}

void encoderRequestThreadFunc(EncoderDriver *encoder, const std::string &label) {
    auto nextTime = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds(1000);

    while (!g_stopFlag.load()) {
        nextTime += period;
        encoder->requestState(false);
        std::this_thread::sleep_until(nextTime);
    }
    std::cout << "Encoder request thread stopped for " << label << std::endl;
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

bool openSideWriter(const fs::path &outputDir, const SensorSideConfig &config, SideWriter *sideWriter) {
    if (sideWriter == nullptr) {
        return false;
    }

    sideWriter->config = config;
    sideWriter->outputFile = outputDir / ("sensor_data_" + config.label + ".mcap");
    sideWriter->imuSchema = buildImuSchema();
    sideWriter->encoderSchema = buildEncoderSchema();

    mcap::McapWriterOptions options("sensor_recorder_" + config.label);
    options.noChunking = false;
    options.chunkSize = 256 * 1024;
    options.compression = mcap::Compression::Lz4;
    options.compressionLevel = mcap::CompressionLevel::Default;
    options.forceCompression = false;
    options.noRepeatedSchemas = true;
    options.noRepeatedChannels = true;
    options.noMessageIndex = true;

    auto status = sideWriter->writer.open(sideWriter->outputFile.string(), options);
    if (!status.ok()) {
        std::cerr << "Failed to open MCAP file for " << config.label << ": " << status.message << std::endl;
        return false;
    }

    sideWriter->writer.addSchema(sideWriter->imuSchema);
    sideWriter->writer.addSchema(sideWriter->encoderSchema);
    std::cout << "Recording " << config.label << " sensor data to " << sideWriter->outputFile << std::endl;
    return true;
}

bool connectEncoderWithFallback(EncoderRuntime &encoderRuntime) {
    auto connectStatus = encoderRuntime.driver->connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (connectStatus == ConnectStatus::SERIAL_FAIL) {
        std::cerr << "Failed to connect encoder serial port: " << encoderRuntime.config.encoderPort << std::endl;
        return false;
    }

    if (connectStatus == ConnectStatus::NO_RESPONSE) {
        std::cout << encoderRuntime.config.label
                  << " encoder not responding at 1Mbps, falling back to 115200..." << std::endl;
        encoderRuntime.driver->disconnect();
        encoderRuntime.driver->resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        connectStatus = encoderRuntime.driver->connect();
        if (connectStatus == ConnectStatus::SUCCESS) {
            std::cout << encoderRuntime.config.label
                      << " encoder ready at 115200, switching back to 1Mbps." << std::endl;
            encoderRuntime.driver->setBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            encoderRuntime.driver->disconnect();
            encoderRuntime.driver->resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            connectStatus = encoderRuntime.driver->connect();
        }
        if (connectStatus != ConnectStatus::SUCCESS) {
            std::cerr << encoderRuntime.config.label
                      << " encoder failed to respond after baudrate adjustments." << std::endl;
            return false;
        }
    } else {
        std::cout << encoderRuntime.config.label << " encoder ready at 1Mbps." << std::endl;
    }

    return true;
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

    const SensorSideConfig rightConfig{"right", "/dev/right_imu", "/dev/right_encoder"};
    const SensorSideConfig leftConfig{"left", "/dev/left_imu", "/dev/left_encoder"};

    SideWriter rightWriter;
    SideWriter leftWriter;
    if (!openSideWriter(outputDir, rightConfig, &rightWriter) || !openSideWriter(outputDir, leftConfig, &leftWriter)) {
        return -1;
    }

    ImuRuntime rightImu{rightConfig, nullptr, mcap::Channel("imu_right", "binary", rightWriter.imuSchema.id)};
    ImuRuntime leftImu{leftConfig, nullptr, mcap::Channel("imu_left", "binary", leftWriter.imuSchema.id)};
    EncoderRuntime rightEncoder{rightConfig, nullptr, mcap::Channel("encoder_right", "binary", rightWriter.encoderSchema.id)};
    EncoderRuntime leftEncoder{leftConfig, nullptr, mcap::Channel("encoder_left", "binary", leftWriter.encoderSchema.id)};

    rightWriter.writer.addChannel(rightImu.channel);
    rightWriter.writer.addChannel(rightEncoder.channel);
    leftWriter.writer.addChannel(leftImu.channel);
    leftWriter.writer.addChannel(leftEncoder.channel);

    std::cout << "Opening dual-arm sensors: "
              << "right(imu=" << rightConfig.imuPort << ", encoder=" << rightConfig.encoderPort << ") "
              << "left(imu=" << leftConfig.imuPort << ", encoder=" << leftConfig.encoderPort << ")"
              << std::endl;

    rightImu.driver = std::make_unique<dmbot_serial::Im648Driver>(rightConfig.imuPort, 115200);
    leftImu.driver = std::make_unique<dmbot_serial::Im648Driver>(leftConfig.imuPort, 115200);
    rightImu.driver->start();
    leftImu.driver->start();
    std::cout << "Both IM648 devices initialized." << std::endl;

    rightEncoder.driver = std::make_unique<EncoderDriver>(1, rightConfig.encoderPort, 1000000, "Right_Encoder");
    leftEncoder.driver = std::make_unique<EncoderDriver>(1, leftConfig.encoderPort, 1000000, "Left_Encoder");
    rightEncoder.connected = connectEncoderWithFallback(rightEncoder);
    leftEncoder.connected = connectEncoderWithFallback(leftEncoder);

    if (!rightEncoder.connected && !leftEncoder.connected) {
        std::cerr << "Both encoders failed to initialize." << std::endl;
        return -1;
    }

    if (rightEncoder.connected) {
        rightEncoder.readThread = std::thread(encoderReadThreadFunc, rightEncoder.driver.get(), rightConfig.label);
        rightEncoder.requestThread = std::thread(encoderRequestThreadFunc, rightEncoder.driver.get(), rightConfig.label);
    }
    if (leftEncoder.connected) {
        leftEncoder.readThread = std::thread(encoderReadThreadFunc, leftEncoder.driver.get(), leftConfig.label);
        leftEncoder.requestThread = std::thread(encoderRequestThreadFunc, leftEncoder.driver.get(), leftConfig.label);
    }

    auto nextEncoderWrite = std::chrono::steady_clock::now();
    const auto encoderPeriod = std::chrono::microseconds(1000);

    while (!g_stopFlag.load()) {
        bool wroteData = false;

        const auto handleImu = [&](ImuRuntime &imuRuntime, mcap::McapWriter &writer) {
            dmbot_serial::IM648_Data imuData;
            if (!imuRuntime.driver->tryConsumeData(&imuData)) {
                return false;
            }

            const auto timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
            const auto sample = create_imu_sample(imuData);

            mcap::Message msg;
            msg.channelId = imuRuntime.channel.id;
            msg.sequence = imuRuntime.sequence++;
            msg.logTime = timestampNs;
            msg.publishTime = timestampNs;
            msg.data = reinterpret_cast<const std::byte *>(&sample);
            msg.dataSize = sizeof(sample);

            const auto writeStatus = writer.write(msg);
            if (!writeStatus.ok()) {
                std::cerr << "Failed to write " << imuRuntime.config.label
                          << " IMU frame: " << writeStatus.message << std::endl;
                g_stopFlag = true;
                return false;
            }

            if (!imuRuntime.firstSampleLogged) {
                std::cout << "[IMU-" << imuRuntime.config.label << "] First sample: "
                          << "q=(" << sample.qx << "," << sample.qy << "," << sample.qz << "," << sample.qw << ") "
                          << "g=(" << sample.gx << "," << sample.gy << "," << sample.gz << ") "
                          << "a=(" << sample.ax << "," << sample.ay << "," << sample.az << ") "
                          << "ts_ns=" << timestampNs
                          << std::endl;
                imuRuntime.firstSampleLogged = true;
            }
            return true;
        };

        wroteData = handleImu(rightImu, rightWriter.writer) || wroteData;
        wroteData = handleImu(leftImu, leftWriter.writer) || wroteData;

        auto nowSteady = std::chrono::steady_clock::now();
        if (nowSteady >= nextEncoderWrite) {
            nextEncoderWrite += encoderPeriod;

            const auto handleEncoder = [&](EncoderRuntime &encoderRuntime, mcap::McapWriter &writer) {
                if (!encoderRuntime.connected) {
                    return false;
                }

                EncoderData state = encoderRuntime.driver->getState();
                if (state.currentPosition == 65535) {
                    if (encoderRuntime.warmupCounter++ < 100) {
                        return false;
                    }
                }

                const auto timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                const auto sample = create_encoder_sample(state);

                mcap::Message msg;
                msg.channelId = encoderRuntime.channel.id;
                msg.sequence = encoderRuntime.sequence++;
                msg.logTime = timestampNs;
                msg.publishTime = timestampNs;
                msg.data = reinterpret_cast<const std::byte *>(&sample);
                msg.dataSize = sizeof(sample);

                const auto writeStatus = writer.write(msg);
                if (!writeStatus.ok()) {
                    std::cerr << "Failed to write " << encoderRuntime.config.label
                              << " encoder frame: " << writeStatus.message << std::endl;
                    g_stopFlag = true;
                    return false;
                }

                if (!encoderRuntime.firstSampleLogged) {
                    std::cout << "[Encoder-" << encoderRuntime.config.label << "] First sample: "
                              << "raw=" << state.currentPosition
                              << ", rad=" << state.currentPositionRad
                              << ", speed_raw=" << state.currentSpeed
                              << ", speed_rad=" << state.currentSpeedRad
                              << ", ts_ns=" << timestampNs
                              << std::endl;
                    encoderRuntime.firstSampleLogged = true;
                }
                return true;
            };

            wroteData = handleEncoder(rightEncoder, rightWriter.writer) || wroteData;
            wroteData = handleEncoder(leftEncoder, leftWriter.writer) || wroteData;
        }

        if (!wroteData) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    g_stopFlag = true;
    std::cout << "Stopping sensors..." << std::endl;

    rightImu.driver->stop();
    leftImu.driver->stop();
    if (rightEncoder.driver) {
        rightEncoder.driver->disconnect();
    }
    if (leftEncoder.driver) {
        leftEncoder.driver->disconnect();
    }

    if (rightEncoder.readThread.joinable()) {
        rightEncoder.readThread.join();
    }
    if (rightEncoder.requestThread.joinable()) {
        rightEncoder.requestThread.join();
    }
    if (leftEncoder.readThread.joinable()) {
        leftEncoder.readThread.join();
    }
    if (leftEncoder.requestThread.joinable()) {
        leftEncoder.requestThread.join();
    }

    rightWriter.writer.close();
    leftWriter.writer.close();
    std::cout << "Right MCAP log saved to " << rightWriter.outputFile << std::endl;
    std::cout << "Left MCAP log saved to " << leftWriter.outputFile << std::endl;
    return 0;
}
