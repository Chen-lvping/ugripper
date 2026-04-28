#include "im648_driver.h"
#include "encoder_driver.h"
#include "record_runtime/motion_alert_ipc.h"
#include "sensor_recorder/logging_compat.h"
#include "sensor_recorder/sensor_domain.h"

#include <mcap/writer.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

std::atomic<bool> g_stopFlag(false);

constexpr uint64_t kNanosecondsPerMicrosecond = 1000ULL;
constexpr uint64_t kImuNominalPeriodNs = 5'000'000ULL;
constexpr uint64_t kEncoderNominalPeriodNs = 1'000'000ULL;
constexpr double kGravityMps2 = 9.80665;
constexpr double kMotionAccelThresholdMps2 = 8.0;
constexpr double kMotionGyroThresholdRadps = 1.6;
constexpr uint64_t kMotionDebounceMs = 50;
constexpr uint64_t kMotionCooldownMs = 100;
constexpr uint64_t kMotionMinAlertDurationMs = 500;

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
    std::atomic<uint64_t> writtenMessageCount{0};
    std::atomic<uint64_t> writeFailureCount{0};
    std::atomic<uint32_t> lastWrittenSequence{0};
    std::atomic<uint64_t> lastWrittenLogTimeNs{0};
    std::atomic<uint64_t> lastWriteSystemTimeNs{0};
};

using ugripper::sensor::BatchTimestampSmoothingState;
using ugripper::sensor::CreateBatchTimestampSmoothingState;
using ugripper::sensor::CurrentSystemTimeNs;
using ugripper::sensor::SmoothTimestampedBatch;
using ugripper::sensor::TimestampedPayloadFrame;

struct ImuRuntime {
    SensorSideConfig config;
    std::unique_ptr<dmbot_serial::Im648Driver> driver;
    mcap::Channel channel;
    BatchTimestampSmoothingState timestampSmoothing;
    uint32_t sequence = 0;
    bool firstSampleLogged = false;
    uint64_t receivedSampleCount = 0;
    uint64_t emittedMessageCount = 0;
    uint32_t lastEmittedSequence = 0;
    uint64_t lastEmittedTimestampNs = 0;
};

struct MotionSample {
    uint64_t steadyTimeMs = 0;
    double gyroMagnitude = 0.0;
    double accelExcess = 0.0;
    bool overGyro = false;
    bool overAccel = false;
};

struct MotionAlertState {
    bool alertActive = false;
    uint64_t overThresholdSinceMs = 0;
    uint64_t lastTransitionMs = 0;
};

struct MotionQueuedSample {
    std::string label;
    dmbot_serial::IM648_Data imuData;
};

struct MotionEventQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<MotionQueuedSample> queue;
    bool stopped = false;
    size_t droppedSamples = 0;
    static constexpr size_t kMaxQueueSize = 4096;

    void push(MotionQueuedSample &&sample) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) {
                return;
            }
            if (queue.size() >= kMaxQueueSize) {
                queue.pop_front();
                ++droppedSamples;
                if (droppedSamples == 1 || (droppedSamples % 256) == 0) {
                    DM_LOG_WARN_STREAM() << "[MotionAlertQueue] dropped oldest samples=" << droppedSamples;
                }
            }
            queue.push_back(std::move(sample));
        }
        cv.notify_one();
    }

    bool pop(MotionQueuedSample *sample) {
        if (sample == nullptr) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stopped || !queue.empty(); });
        if (queue.empty()) {
            return false;
        }

        *sample = std::move(queue.front());
        queue.pop_front();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }
        cv.notify_all();
    }
};

struct EncoderRuntime {
    SensorSideConfig config;
    std::unique_ptr<EncoderDriver> driver;
    mcap::Channel channel;
    BatchTimestampSmoothingState timestampSmoothing;
    std::thread readThread;
    std::thread requestThread;
    uint32_t sequence = 0;
    bool firstSampleLogged = false;
    int warmupCounter = 0;
    bool connected = false;
    uint64_t receivedSampleCount = 0;
    uint64_t emittedMessageCount = 0;
    uint32_t lastEmittedSequence = 0;
    uint64_t lastEmittedTimestampNs = 0;
};

struct QueuedMessage {
    uint16_t channelId = 0;
    uint32_t sequence = 0;
    uint64_t logTime = 0;
    uint64_t publishTime = 0;
    std::vector<std::byte> payload;
};

struct PendingWriteQueue {
    std::string label;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedMessage> queue;
    bool stopped = false;
    size_t nextBacklogWarnSize = 4096;
    size_t maxObservedBacklog = 0;

    void push(QueuedMessage &&message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) {
                return;
            }
            queue.push_back(std::move(message));
            if (queue.size() > maxObservedBacklog) {
                maxObservedBacklog = queue.size();
            }
            if (queue.size() >= nextBacklogWarnSize) {
                DM_LOG_WARN_STREAM() << "[SensorWriteQueue-" << label
                                     << "] backlog grew to " << queue.size();
                nextBacklogWarnSize = queue.size() + 4096;
            }
        }
        cv.notify_one();
    }

    bool pop(QueuedMessage *message) {
        if (message == nullptr) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stopped || !queue.empty(); });
        if (queue.empty()) {
            return false;
        }

        *message = std::move(queue.front());
        queue.pop_front();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }
        cv.notify_all();
    }
};

static_assert(sizeof(ImuSample) == 40, "ImuSample layout changed");
static_assert(sizeof(EncoderSample) == 8, "EncoderSample layout changed");

void signalHandler(int signum) {
    DM_LOG_INFO_STREAM() << "Interrupt signal (" << signum << ") received. Stopping...";
    g_stopFlag = true;
}

uint64_t CurrentSteadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

ugripper::MotionAlertReasonCode MotionAlertReasonCode(bool overAccel, bool overGyro) {
    if (overAccel && overGyro) {
        return ugripper::MotionAlertReasonCode::AccelAndGyro;
    }
    if (overAccel) {
        return ugripper::MotionAlertReasonCode::Accel;
    }
    if (overGyro) {
        return ugripper::MotionAlertReasonCode::Gyro;
    }
    return ugripper::MotionAlertReasonCode::None;
}

ugripper::MotionAlertSide MotionAlertSideForLabel(const std::string &label) {
    if (label == "left") {
        return ugripper::MotionAlertSide::Left;
    }
    if (label == "right") {
        return ugripper::MotionAlertSide::Right;
    }
    return ugripper::MotionAlertSide::Unknown;
}

void EmitMotionAlertStateChange(int motionAlertFd,
                                const std::string &label,
                                const MotionSample &sample,
                                bool active,
                                ugripper::MotionAlertReasonCode reason) {
    if (motionAlertFd < 0) {
        return;
    }

    ugripper::MotionAlertMessage message;
    message.side = static_cast<uint8_t>(MotionAlertSideForLabel(label));
    message.reason = static_cast<uint8_t>(reason);
    message.active = active ? 1 : 0;
    message.gyroMagnitude = static_cast<float>(sample.gyroMagnitude);
    message.accelExcess = static_cast<float>(sample.accelExcess);
    message.steadyTimeMs = sample.steadyTimeMs;

    const ssize_t bytesWritten = write(motionAlertFd, &message, sizeof(message));
    if (bytesWritten < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(motionAlertFd);
    }
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

QueuedMessage buildQueuedMessage(uint16_t channelId,
                                 uint32_t sequence,
                                 uint64_t timestampNs,
                                 const std::byte *data,
                                 size_t dataSize) {
    QueuedMessage message;
    message.channelId = channelId;
    message.sequence = sequence;
    message.logTime = timestampNs;
    message.publishTime = timestampNs;
    message.payload.assign(data, data + dataSize);
    return message;
}

void sideWriterThreadFunc(SideWriter *sideWriter, PendingWriteQueue *pendingQueue) {
    if (sideWriter == nullptr || pendingQueue == nullptr) {
        return;
    }

    QueuedMessage queuedMessage;
    while (pendingQueue->pop(&queuedMessage)) {
        mcap::Message message;
        message.channelId = queuedMessage.channelId;
        message.sequence = queuedMessage.sequence;
        message.logTime = queuedMessage.logTime;
        message.publishTime = queuedMessage.publishTime;
        message.data = queuedMessage.payload.data();
        message.dataSize = queuedMessage.payload.size();

        const auto writeStatus = sideWriter->writer.write(message);
        if (!writeStatus.ok()) {
            sideWriter->writeFailureCount.fetch_add(1, std::memory_order_relaxed);
            DM_LOG_ERROR_STREAM() << "Failed to write " << pendingQueue->label
                                  << " sensor frame: " << writeStatus.message;
            g_stopFlag = true;
            pendingQueue->stop();
            return;
        }
        sideWriter->writtenMessageCount.fetch_add(1, std::memory_order_relaxed);
        sideWriter->lastWrittenSequence.store(message.sequence, std::memory_order_relaxed);
        sideWriter->lastWrittenLogTimeNs.store(message.logTime, std::memory_order_relaxed);
        sideWriter->lastWriteSystemTimeNs.store(CurrentSystemTimeNs(), std::memory_order_relaxed);
    }
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
    DM_LOG_INFO_STREAM() << "Encoder read thread stopped for " << label;
}

void encoderRequestThreadFunc(EncoderDriver *encoder, const std::string &label) {
    auto nextTime = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds(1000);

    while (!g_stopFlag.load()) {
        nextTime += period;
        if (!encoder->isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            nextTime = std::chrono::steady_clock::now();
            continue;
        }
        encoder->requestState(false);
        std::this_thread::sleep_until(nextTime);
    }
    DM_LOG_INFO_STREAM() << "Encoder request thread stopped for " << label;
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
    // Keep schema/channel metadata readable by downstream validation so topic-based
    // checks can resolve encoder and IMU channels reliably.
    options.noRepeatedSchemas = false;
    options.noRepeatedChannels = false;
    options.noMessageIndex = true;

    auto status = sideWriter->writer.open(sideWriter->outputFile.string(), options);
    if (!status.ok()) {
        DM_LOG_ERROR_STREAM() << "Failed to open MCAP file for " << config.label << ": " << status.message;
        return false;
    }

    sideWriter->writer.addSchema(sideWriter->imuSchema);
    sideWriter->writer.addSchema(sideWriter->encoderSchema);
    DM_LOG_INFO_STREAM() << "Recording " << config.label << " sensor data to " << sideWriter->outputFile;
    return true;
}

bool connectEncoderWithFallback(EncoderRuntime &encoderRuntime) {
    auto connectStatus = encoderRuntime.driver->connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (connectStatus == ConnectStatus::SERIAL_FAIL) {
        DM_LOG_ERROR_STREAM() << "Failed to connect encoder serial port: " << encoderRuntime.config.encoderPort;
        return false;
    }

    if (connectStatus == ConnectStatus::NO_RESPONSE) {
        DM_LOG_WARN_STREAM() << encoderRuntime.config.label
                             << " encoder not responding at 1Mbps, falling back to 115200...";
        encoderRuntime.driver->disconnect();
        encoderRuntime.driver->resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        connectStatus = encoderRuntime.driver->connect();
        if (connectStatus == ConnectStatus::SUCCESS) {
            DM_LOG_INFO_STREAM() << encoderRuntime.config.label
                                 << " encoder ready at 115200, switching back to 1Mbps.";
            encoderRuntime.driver->setBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            encoderRuntime.driver->disconnect();
            encoderRuntime.driver->resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            connectStatus = encoderRuntime.driver->connect();
        }
        if (connectStatus != ConnectStatus::SUCCESS) {
            DM_LOG_ERROR_STREAM() << encoderRuntime.config.label
                                  << " encoder failed to respond after baudrate adjustments.";
            return false;
        }
    } else {
        DM_LOG_INFO_STREAM() << encoderRuntime.config.label << " encoder ready at 1Mbps.";
    }

    return true;
}

int main(int argc, char *argv[]) {
    DM_LOG_INIT("SensorRecorder",
                "info",
                "./logs/SensorRecorder/SensorRecorder.log",
                1024 * 1024 * 10,
                3,
                false);

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    fs::path outputDir = ".";
    int motionAlertFd = -1;
    bool outputDirSet = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--motion-alert-fd" && index + 1 < argc) {
            motionAlertFd = std::stoi(argv[++index]);
            continue;
        }
        if (!outputDirSet && !argument.empty() && argument[0] != '-') {
            outputDir = fs::path(argument);
            outputDirSet = true;
            continue;
        }
        if (argument == "-h" || argument == "--help") {
            std::cout
                << "Usage: SensorRecorder [OUTPUT_DIR]\n"
                << "Records left/right IMU and encoder streams into MCAP files.\n";
            return 0;
        }
        DM_LOG_ERROR_STREAM() << "Unknown argument: " << argument;
        return -1;
    }
    if (motionAlertFd >= 0) {
        const int flags = fcntl(motionAlertFd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(motionAlertFd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    std::error_code ec;
    if (!fs::exists(outputDir)) {
        fs::create_directories(outputDir, ec);
        if (ec) {
            DM_LOG_ERROR_STREAM() << "Failed to create output directory: " << ec.message();
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
    MotionAlertState rightMotionAlert;
    MotionAlertState leftMotionAlert;
    MotionEventQueue motionEventQueue;
    rightImu.timestampSmoothing = CreateBatchTimestampSmoothingState("imu_right", kImuNominalPeriodNs);
    leftImu.timestampSmoothing = CreateBatchTimestampSmoothingState("imu_left", kImuNominalPeriodNs);
    rightEncoder.timestampSmoothing = CreateBatchTimestampSmoothingState("encoder_right", kEncoderNominalPeriodNs);
    leftEncoder.timestampSmoothing = CreateBatchTimestampSmoothingState("encoder_left", kEncoderNominalPeriodNs);
    PendingWriteQueue rightPendingWrites{rightConfig.label};
    PendingWriteQueue leftPendingWrites{leftConfig.label};

    rightWriter.writer.addChannel(rightImu.channel);
    rightWriter.writer.addChannel(rightEncoder.channel);
    leftWriter.writer.addChannel(leftImu.channel);
    leftWriter.writer.addChannel(leftEncoder.channel);

    std::thread rightWriterThread(sideWriterThreadFunc, &rightWriter, &rightPendingWrites);
    std::thread leftWriterThread(sideWriterThreadFunc, &leftWriter, &leftPendingWrites);
    std::thread motionWorkerThread([&]() {
        const auto processMotionSample = [&](const std::string &label,
                                             const dmbot_serial::IM648_Data &imuData,
                                             MotionAlertState *alertState) {
            if (alertState == nullptr) {
                return;
            }

            MotionSample sample;
            sample.steadyTimeMs = CurrentSteadyMs();
            sample.gyroMagnitude = std::sqrt(
                static_cast<double>(imuData.gyrox) * imuData.gyrox +
                static_cast<double>(imuData.gyroy) * imuData.gyroy +
                static_cast<double>(imuData.gyroz) * imuData.gyroz);
            const double accelMagnitude = std::sqrt(
                static_cast<double>(imuData.accx) * imuData.accx +
                static_cast<double>(imuData.accy) * imuData.accy +
                static_cast<double>(imuData.accz) * imuData.accz);
            sample.accelExcess = std::fabs(accelMagnitude - kGravityMps2);
            sample.overGyro = sample.gyroMagnitude >= kMotionGyroThresholdRadps;
            sample.overAccel = sample.accelExcess >= kMotionAccelThresholdMps2;

            const bool overThreshold = sample.overGyro || sample.overAccel;
            const auto reason = MotionAlertReasonCode(sample.overAccel, sample.overGyro);

            if (overThreshold) {
                if (alertState->overThresholdSinceMs == 0) {
                    alertState->overThresholdSinceMs = sample.steadyTimeMs;
                }
                if (!alertState->alertActive &&
                    (sample.steadyTimeMs - alertState->overThresholdSinceMs) >= kMotionDebounceMs &&
                    (alertState->lastTransitionMs == 0 ||
                     (sample.steadyTimeMs - alertState->lastTransitionMs) >= kMotionCooldownMs)) {
                    alertState->alertActive = true;
                    alertState->lastTransitionMs = sample.steadyTimeMs;
                    EmitMotionAlertStateChange(motionAlertFd, label, sample, true, reason);
                }
                return;
            }

            alertState->overThresholdSinceMs = 0;
            if (alertState->alertActive &&
                (sample.steadyTimeMs - alertState->lastTransitionMs) >= kMotionMinAlertDurationMs) {
                alertState->alertActive = false;
                alertState->lastTransitionMs = sample.steadyTimeMs;
                EmitMotionAlertStateChange(
                    motionAlertFd,
                    label,
                    sample,
                    false,
                    ugripper::MotionAlertReasonCode::Recovered);
            }
        };

        MotionQueuedSample queuedSample;
        while (motionEventQueue.pop(&queuedSample)) {
            processMotionSample(
                queuedSample.label,
                queuedSample.imuData,
                queuedSample.label == "left" ? &leftMotionAlert : &rightMotionAlert);
        }
    });

    DM_LOG_INFO_STREAM() << "Opening dual-arm sensors: "
                         << "right(imu=" << rightConfig.imuPort << ", encoder=" << rightConfig.encoderPort << ") "
                         << "left(imu=" << leftConfig.imuPort << ", encoder=" << leftConfig.encoderPort << ")";

    std::thread rightImuInitThread([&rightImu, &rightConfig]() {
        rightImu.driver = std::make_unique<dmbot_serial::Im648Driver>(rightConfig.imuPort, 115200);
        rightImu.driver->start();
    });
    std::thread leftImuInitThread([&leftImu, &leftConfig]() {
        leftImu.driver = std::make_unique<dmbot_serial::Im648Driver>(leftConfig.imuPort, 115200);
        leftImu.driver->start();
    });
    rightImuInitThread.join();
    leftImuInitThread.join();
    DM_LOG_INFO("Both IM648 devices initialized.");

    std::thread rightEncoderInitThread([&rightEncoder, &rightConfig]() {
        rightEncoder.driver = std::make_unique<EncoderDriver>(1, rightConfig.encoderPort, 1000000, "Right_Encoder");
        rightEncoder.connected = connectEncoderWithFallback(rightEncoder);
    });
    std::thread leftEncoderInitThread([&leftEncoder, &leftConfig]() {
        leftEncoder.driver = std::make_unique<EncoderDriver>(1, leftConfig.encoderPort, 1000000, "Left_Encoder");
        leftEncoder.connected = connectEncoderWithFallback(leftEncoder);
    });
    rightEncoderInitThread.join();
    leftEncoderInitThread.join();

    if (!rightEncoder.connected && !leftEncoder.connected) {
        DM_LOG_ERROR("Both encoders failed to initialize.");
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

    while (!g_stopFlag.load()) {
        bool wroteData = false;

        const auto handleImu = [&](ImuRuntime &imuRuntime, PendingWriteQueue &pendingWrites) {
            bool wroteFrame = false;
            const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
                const uint32_t sequence = imuRuntime.sequence++;
                auto message = buildQueuedMessage(
                    imuRuntime.channel.id,
                    sequence,
                    timestampNs,
                    payload.data(),
                    payload.size());
                pendingWrites.push(std::move(message));
                imuRuntime.lastEmittedSequence = sequence;
                imuRuntime.lastEmittedTimestampNs = timestampNs;
                ++imuRuntime.emittedMessageCount;
            };

            std::vector<TimestampedPayloadFrame> batchFrames;
            dmbot_serial::IM648_Data imuData;
            while (imuRuntime.driver->tryConsumeData(&imuData)) {
                ++imuRuntime.receivedSampleCount;
                const auto sample = create_imu_sample(imuData);
                motionEventQueue.push(MotionQueuedSample{imuRuntime.config.label, imuData});
                TimestampedPayloadFrame frame;
                frame.host_timestamp_ns = imuData.timestamp * kNanosecondsPerMicrosecond;
                if (frame.host_timestamp_ns == 0) {
                    frame.host_timestamp_ns = CurrentSystemTimeNs();
                }
                const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
                frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
                batchFrames.push_back(std::move(frame));
                wroteFrame = true;

                if (!imuRuntime.firstSampleLogged) {
                    DM_LOG_INFO_STREAM() << "[IMU-" << imuRuntime.config.label << "] First sample: "
                                         << "q=(" << sample.qx << "," << sample.qy << "," << sample.qz << "," << sample.qw << ") "
                                         << "g=(" << sample.gx << "," << sample.gy << "," << sample.gz << ") "
                                         << "a=(" << sample.ax << "," << sample.ay << "," << sample.az << ") "
                                         << "ts_ns=" << frame.host_timestamp_ns;
                    imuRuntime.firstSampleLogged = true;
                }
            }
            auto smoothedFrames =
                SmoothTimestampedBatch(&imuRuntime.timestampSmoothing, std::move(batchFrames));
            for (auto &frame : smoothedFrames) {
                emitFrame(frame.host_timestamp_ns, std::move(frame.payload));
            }
            return wroteFrame;
        };

        wroteData = handleImu(rightImu, rightPendingWrites) || wroteData;
        wroteData = handleImu(leftImu, leftPendingWrites) || wroteData;

        const auto handleEncoder = [&](EncoderRuntime &encoderRuntime, PendingWriteQueue &pendingWrites) {
            if (!encoderRuntime.connected) {
                return false;
            }
            if (!encoderRuntime.driver->isConnected()) {
                encoderRuntime.connected = false;
                DM_LOG_ERROR_STREAM() << encoderRuntime.config.label
                                      << " encoder disconnected during recording; stopping encoder samples for this side.";
                return false;
            }

            bool wroteFrame = false;
            const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
                const uint32_t sequence = encoderRuntime.sequence++;
                auto message = buildQueuedMessage(
                    encoderRuntime.channel.id,
                    sequence,
                    timestampNs,
                    payload.data(),
                    payload.size());
                pendingWrites.push(std::move(message));
                encoderRuntime.lastEmittedSequence = sequence;
                encoderRuntime.lastEmittedTimestampNs = timestampNs;
                ++encoderRuntime.emittedMessageCount;
            };

            std::vector<TimestampedPayloadFrame> batchFrames;
            EncoderQueuedSample queuedSample;
            while (encoderRuntime.driver->tryConsumeSample(&queuedSample)) {
                ++encoderRuntime.receivedSampleCount;
                const EncoderData &state = queuedSample.state;
                if (state.currentPosition == 65535) {
                    if (encoderRuntime.warmupCounter++ < 100) {
                        continue;
                    }
                }

                const auto sample = create_encoder_sample(state);
                TimestampedPayloadFrame frame;
                frame.host_timestamp_ns =
                    queuedSample.hostTimestampNs == 0 ? CurrentSystemTimeNs() : queuedSample.hostTimestampNs;
                const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
                frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
                batchFrames.push_back(std::move(frame));
                wroteFrame = true;

                if (!encoderRuntime.firstSampleLogged) {
                    DM_LOG_INFO_STREAM() << "[Encoder-" << encoderRuntime.config.label << "] First sample: "
                                         << "raw=" << state.currentPosition
                                         << ", rad=" << state.currentPositionRad
                                         << ", speed_raw=" << state.currentSpeed
                                         << ", speed_rad=" << state.currentSpeedRad
                                         << ", ts_ns=" << queuedSample.hostTimestampNs;
                    encoderRuntime.firstSampleLogged = true;
                }
            }
            auto smoothedFrames =
                SmoothTimestampedBatch(&encoderRuntime.timestampSmoothing, std::move(batchFrames));
            for (auto &frame : smoothedFrames) {
                emitFrame(frame.host_timestamp_ns, std::move(frame.payload));
            }
            return wroteFrame;
        };

        wroteData = handleEncoder(rightEncoder, rightPendingWrites) || wroteData;
        wroteData = handleEncoder(leftEncoder, leftPendingWrites) || wroteData;

        if (!wroteData) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    g_stopFlag = true;
    DM_LOG_INFO("Stopping sensors...");
    motionEventQueue.stop();

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
    if (motionWorkerThread.joinable()) {
        motionWorkerThread.join();
    }

    const auto drainImuRuntime = [](ImuRuntime &imuRuntime, PendingWriteQueue &pendingWrites) {
        const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
            const uint32_t sequence = imuRuntime.sequence++;
            auto message = buildQueuedMessage(
                imuRuntime.channel.id,
                sequence,
                timestampNs,
                payload.data(),
                payload.size());
            pendingWrites.push(std::move(message));
            imuRuntime.lastEmittedSequence = sequence;
            imuRuntime.lastEmittedTimestampNs = timestampNs;
            ++imuRuntime.emittedMessageCount;
        };
        std::vector<TimestampedPayloadFrame> batchFrames;
        dmbot_serial::IM648_Data imuData;
        while (imuRuntime.driver && imuRuntime.driver->tryConsumeData(&imuData)) {
            ++imuRuntime.receivedSampleCount;
            const auto sample = create_imu_sample(imuData);
            TimestampedPayloadFrame frame;
            frame.host_timestamp_ns = imuData.timestamp * kNanosecondsPerMicrosecond;
            if (frame.host_timestamp_ns == 0) {
                frame.host_timestamp_ns = CurrentSystemTimeNs();
            }
            const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
            frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
            batchFrames.push_back(std::move(frame));
        }
        auto smoothedFrames =
            SmoothTimestampedBatch(&imuRuntime.timestampSmoothing, std::move(batchFrames));
        for (auto &frame : smoothedFrames) {
            emitFrame(frame.host_timestamp_ns, std::move(frame.payload));
        }
    };

    const auto drainEncoderRuntime = [](EncoderRuntime &encoderRuntime, PendingWriteQueue &pendingWrites) {
        const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
            const uint32_t sequence = encoderRuntime.sequence++;
            auto message = buildQueuedMessage(
                encoderRuntime.channel.id,
                sequence,
                timestampNs,
                payload.data(),
                payload.size());
            pendingWrites.push(std::move(message));
            encoderRuntime.lastEmittedSequence = sequence;
            encoderRuntime.lastEmittedTimestampNs = timestampNs;
            ++encoderRuntime.emittedMessageCount;
        };
        std::vector<TimestampedPayloadFrame> batchFrames;
        EncoderQueuedSample queuedSample;
        while (encoderRuntime.driver && encoderRuntime.driver->tryConsumeSample(&queuedSample)) {
            ++encoderRuntime.receivedSampleCount;
            const auto &state = queuedSample.state;
            if (state.currentPosition == 65535) {
                if (encoderRuntime.warmupCounter++ < 100) {
                    continue;
                }
            }

            const auto sample = create_encoder_sample(state);
            TimestampedPayloadFrame frame;
            frame.host_timestamp_ns =
                queuedSample.hostTimestampNs == 0 ? CurrentSystemTimeNs() : queuedSample.hostTimestampNs;
            const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
            frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
            batchFrames.push_back(std::move(frame));
        }
        auto smoothedFrames =
            SmoothTimestampedBatch(&encoderRuntime.timestampSmoothing, std::move(batchFrames));
        for (auto &frame : smoothedFrames) {
            emitFrame(frame.host_timestamp_ns, std::move(frame.payload));
        }
    };

    drainImuRuntime(rightImu, rightPendingWrites);
    drainImuRuntime(leftImu, leftPendingWrites);
    drainEncoderRuntime(rightEncoder, rightPendingWrites);
    drainEncoderRuntime(leftEncoder, leftPendingWrites);

    rightPendingWrites.stop();
    leftPendingWrites.stop();
    if (rightWriterThread.joinable()) {
        rightWriterThread.join();
    }
    if (leftWriterThread.joinable()) {
        leftWriterThread.join();
    }

    rightWriter.writer.close();
    leftWriter.writer.close();
    DM_LOG_INFO_STREAM() << "[BatchTimestamp-imu_right] batches=" << rightImu.timestampSmoothing.smoothed_batch_count
                         << ", frames=" << rightImu.timestampSmoothing.smoothed_frame_count;
    DM_LOG_INFO_STREAM() << "[BatchTimestamp-imu_left] batches=" << leftImu.timestampSmoothing.smoothed_batch_count
                         << ", frames=" << leftImu.timestampSmoothing.smoothed_frame_count;
    DM_LOG_INFO_STREAM() << "[BatchTimestamp-encoder_right] batches=" << rightEncoder.timestampSmoothing.smoothed_batch_count
                         << ", frames=" << rightEncoder.timestampSmoothing.smoothed_frame_count;
    DM_LOG_INFO_STREAM() << "[BatchTimestamp-encoder_left] batches=" << leftEncoder.timestampSmoothing.smoothed_batch_count
                         << ", frames=" << leftEncoder.timestampSmoothing.smoothed_frame_count;
    DM_LOG_INFO_STREAM() << "[SensorStats-imu_right] received_samples=" << rightImu.receivedSampleCount
                         << ", emitted_messages=" << rightImu.emittedMessageCount
                         << ", last_sequence=" << rightImu.lastEmittedSequence
                         << ", last_timestamp_ns=" << rightImu.lastEmittedTimestampNs;
    DM_LOG_INFO_STREAM() << "[SensorStats-imu_left] received_samples=" << leftImu.receivedSampleCount
                         << ", emitted_messages=" << leftImu.emittedMessageCount
                         << ", last_sequence=" << leftImu.lastEmittedSequence
                         << ", last_timestamp_ns=" << leftImu.lastEmittedTimestampNs;
    DM_LOG_INFO_STREAM() << "[SensorStats-encoder_right] received_samples=" << rightEncoder.receivedSampleCount
                         << ", emitted_messages=" << rightEncoder.emittedMessageCount
                         << ", last_sequence=" << rightEncoder.lastEmittedSequence
                         << ", last_timestamp_ns=" << rightEncoder.lastEmittedTimestampNs;
    DM_LOG_INFO_STREAM() << "[SensorStats-encoder_left] received_samples=" << leftEncoder.receivedSampleCount
                         << ", emitted_messages=" << leftEncoder.emittedMessageCount
                         << ", last_sequence=" << leftEncoder.lastEmittedSequence
                         << ", last_timestamp_ns=" << leftEncoder.lastEmittedTimestampNs;
    DM_LOG_INFO_STREAM() << "[SensorWriteQueue-right] max_backlog=" << rightPendingWrites.maxObservedBacklog;
    DM_LOG_INFO_STREAM() << "[SensorWriteQueue-left] max_backlog=" << leftPendingWrites.maxObservedBacklog;
    DM_LOG_INFO_STREAM() << "[SensorWriter-right] written_messages="
                         << rightWriter.writtenMessageCount.load(std::memory_order_relaxed)
                         << ", write_failures=" << rightWriter.writeFailureCount.load(std::memory_order_relaxed)
                         << ", last_written_sequence=" << rightWriter.lastWrittenSequence.load(std::memory_order_relaxed)
                         << ", last_written_log_time_ns=" << rightWriter.lastWrittenLogTimeNs.load(std::memory_order_relaxed)
                         << ", last_write_system_time_ns=" << rightWriter.lastWriteSystemTimeNs.load(std::memory_order_relaxed);
    DM_LOG_INFO_STREAM() << "[SensorWriter-left] written_messages="
                         << leftWriter.writtenMessageCount.load(std::memory_order_relaxed)
                         << ", write_failures=" << leftWriter.writeFailureCount.load(std::memory_order_relaxed)
                         << ", last_written_sequence=" << leftWriter.lastWrittenSequence.load(std::memory_order_relaxed)
                         << ", last_written_log_time_ns=" << leftWriter.lastWrittenLogTimeNs.load(std::memory_order_relaxed)
                         << ", last_write_system_time_ns=" << leftWriter.lastWriteSystemTimeNs.load(std::memory_order_relaxed);
    DM_LOG_INFO_STREAM() << "Right MCAP log saved to " << rightWriter.outputFile;
    DM_LOG_INFO_STREAM() << "Left MCAP log saved to " << leftWriter.outputFile;
    return 0;
}
