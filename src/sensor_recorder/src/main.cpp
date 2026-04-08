#include "im648_driver.h"
#include "encoder_driver.h"
#include "motion_alert_ipc.h"

#include <mcap/writer.hpp>

#include <algorithm>
#include <atomic>
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
};

struct TimestampedPayloadFrame {
    uint64_t hostTimestampNs = 0;
    std::vector<std::byte> payload;
};

struct BatchTimestampSmoothingState {
    std::string label;
    uint64_t nominalPeriodNs = 0;
    uint64_t lastAssignedTimestampNs = 0;
    bool hasLastAssignedTimestamp = false;
    size_t smoothedBatchCount = 0;
    size_t smoothedFrameCount = 0;
};

struct ImuRuntime {
    SensorSideConfig config;
    std::unique_ptr<dmbot_serial::Im648Driver> driver;
    mcap::Channel channel;
    BatchTimestampSmoothingState timestampSmoothing;
    uint32_t sequence = 0;
    bool firstSampleLogged = false;
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
                    std::cerr << "[MotionAlertQueue] dropped oldest samples=" << droppedSamples << std::endl;
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
                std::cerr << "[SensorWriteQueue-" << label
                          << "] backlog grew to " << queue.size() << std::endl;
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
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

uint64_t currentSystemTimeNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

uint64_t currentSteadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

ugripper::MotionAlertReasonCode motionAlertReasonCode(bool overAccel, bool overGyro) {
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

ugripper::MotionAlertSide motionAlertSideForLabel(const std::string &label) {
    if (label == "left") {
        return ugripper::MotionAlertSide::Left;
    }
    if (label == "right") {
        return ugripper::MotionAlertSide::Right;
    }
    return ugripper::MotionAlertSide::Unknown;
}

void emitMotionAlertStateChange(int motionAlertFd,
                                const std::string &label,
                                const MotionSample &sample,
                                bool active,
                                ugripper::MotionAlertReasonCode reason) {
    if (motionAlertFd < 0) {
        return;
    }

    ugripper::MotionAlertMessage message;
    message.side = static_cast<uint8_t>(motionAlertSideForLabel(label));
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

BatchTimestampSmoothingState createBatchTimestampSmoothingState(const std::string &label,
                                                                uint64_t nominalPeriodNs) {
    BatchTimestampSmoothingState state;
    state.label = label;
    state.nominalPeriodNs = nominalPeriodNs;
    return state;
}

uint64_t clampMonotonicTimestamp(BatchTimestampSmoothingState *state, uint64_t timestampNs) {
    if (state == nullptr) {
        return timestampNs;
    }
    if (timestampNs == 0) {
        timestampNs = currentSystemTimeNs();
    }
    if (state->hasLastAssignedTimestamp && timestampNs <= state->lastAssignedTimestampNs) {
        timestampNs = state->lastAssignedTimestampNs + 1;
    }
    state->lastAssignedTimestampNs = timestampNs;
    state->hasLastAssignedTimestamp = true;
    return timestampNs;
}

template <typename EmitFn>
void emitTimestampedBatch(BatchTimestampSmoothingState *state,
                          std::vector<TimestampedPayloadFrame> *frames,
                          EmitFn emitFn) {
    if (state == nullptr || frames == nullptr || frames->empty()) {
        return;
    }

    if (frames->size() == 1) {
        auto &frame = frames->front();
        emitFn(clampMonotonicTimestamp(state, frame.hostTimestampNs), std::move(frame.payload));
        frames->clear();
        return;
    }

    const auto &lastFrame = frames->back();
    const uint64_t baseTimestampNs =
        lastFrame.hostTimestampNs == 0 ? currentSystemTimeNs() : lastFrame.hostTimestampNs;
    const uint64_t batchSpanNs = state->nominalPeriodNs * (frames->size() - 1);
    const uint64_t startTimestampNs =
        baseTimestampNs > batchSpanNs ? (baseTimestampNs - batchSpanNs) : 1;

    for (size_t index = 0; index < frames->size(); ++index) {
        auto &frame = (*frames)[index];
        const uint64_t reconstructedTimestampNs =
            clampMonotonicTimestamp(state, startTimestampNs + state->nominalPeriodNs * index);
        emitFn(reconstructedTimestampNs, std::move(frame.payload));
    }

    ++state->smoothedBatchCount;
    state->smoothedFrameCount += frames->size();
    if (state->smoothedBatchCount <= 3 || (state->smoothedBatchCount % 20) == 0) {
        std::cout << "[BatchTimestamp-" << state->label << "] batch_size=" << frames->size()
                  << ", base_ts_ns=" << baseTimestampNs << std::endl;
    }
    frames->clear();
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
            std::cerr << "Failed to write " << pendingQueue->label
                      << " sensor frame: " << writeStatus.message << std::endl;
            g_stopFlag = true;
            pendingQueue->stop();
            return;
        }
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
    std::cout << "Encoder read thread stopped for " << label << std::endl;
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
    // Keep schema/channel metadata readable by downstream validation so topic-based
    // checks can resolve encoder and IMU channels reliably.
    options.noRepeatedSchemas = false;
    options.noRepeatedChannels = false;
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
        std::cerr << "Unknown argument: " << argument << std::endl;
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
    MotionAlertState rightMotionAlert;
    MotionAlertState leftMotionAlert;
    MotionEventQueue motionEventQueue;
    rightImu.timestampSmoothing = createBatchTimestampSmoothingState("imu_right", kImuNominalPeriodNs);
    leftImu.timestampSmoothing = createBatchTimestampSmoothingState("imu_left", kImuNominalPeriodNs);
    rightEncoder.timestampSmoothing = createBatchTimestampSmoothingState("encoder_right", kEncoderNominalPeriodNs);
    leftEncoder.timestampSmoothing = createBatchTimestampSmoothingState("encoder_left", kEncoderNominalPeriodNs);
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
            sample.steadyTimeMs = currentSteadyMs();
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
            const auto reason = motionAlertReasonCode(sample.overAccel, sample.overGyro);

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
                    emitMotionAlertStateChange(motionAlertFd, label, sample, true, reason);
                }
                return;
            }

            alertState->overThresholdSinceMs = 0;
            if (alertState->alertActive &&
                (sample.steadyTimeMs - alertState->lastTransitionMs) >= kMotionMinAlertDurationMs) {
                alertState->alertActive = false;
                alertState->lastTransitionMs = sample.steadyTimeMs;
                emitMotionAlertStateChange(
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

    std::cout << "Opening dual-arm sensors: "
              << "right(imu=" << rightConfig.imuPort << ", encoder=" << rightConfig.encoderPort << ") "
              << "left(imu=" << leftConfig.imuPort << ", encoder=" << leftConfig.encoderPort << ")"
              << std::endl;

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
    std::cout << "Both IM648 devices initialized." << std::endl;

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

    while (!g_stopFlag.load()) {
        bool wroteData = false;

        const auto handleImu = [&](ImuRuntime &imuRuntime, PendingWriteQueue &pendingWrites) {
            bool wroteFrame = false;
            const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
                auto message = buildQueuedMessage(
                    imuRuntime.channel.id,
                    imuRuntime.sequence++,
                    timestampNs,
                    payload.data(),
                    payload.size());
                pendingWrites.push(std::move(message));
            };

            std::vector<TimestampedPayloadFrame> batchFrames;
            dmbot_serial::IM648_Data imuData;
            while (imuRuntime.driver->tryConsumeData(&imuData)) {
                const auto sample = create_imu_sample(imuData);
                motionEventQueue.push(MotionQueuedSample{imuRuntime.config.label, imuData});
                TimestampedPayloadFrame frame;
                frame.hostTimestampNs = imuData.timestamp * kNanosecondsPerMicrosecond;
                if (frame.hostTimestampNs == 0) {
                    frame.hostTimestampNs = currentSystemTimeNs();
                }
                const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
                frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
                batchFrames.push_back(std::move(frame));
                wroteFrame = true;

                if (!imuRuntime.firstSampleLogged) {
                    std::cout << "[IMU-" << imuRuntime.config.label << "] First sample: "
                              << "q=(" << sample.qx << "," << sample.qy << "," << sample.qz << "," << sample.qw << ") "
                              << "g=(" << sample.gx << "," << sample.gy << "," << sample.gz << ") "
                              << "a=(" << sample.ax << "," << sample.ay << "," << sample.az << ") "
                              << "ts_ns=" << frame.hostTimestampNs
                              << std::endl;
                    imuRuntime.firstSampleLogged = true;
                }
            }
            emitTimestampedBatch(&imuRuntime.timestampSmoothing, &batchFrames, emitFrame);
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
                std::cerr << encoderRuntime.config.label
                          << " encoder disconnected during recording; stopping encoder samples for this side."
                          << std::endl;
                return false;
            }

            bool wroteFrame = false;
            const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
                auto message = buildQueuedMessage(
                    encoderRuntime.channel.id,
                    encoderRuntime.sequence++,
                    timestampNs,
                    payload.data(),
                    payload.size());
                pendingWrites.push(std::move(message));
            };

            std::vector<TimestampedPayloadFrame> batchFrames;
            EncoderQueuedSample queuedSample;
            while (encoderRuntime.driver->tryConsumeSample(&queuedSample)) {
                const EncoderData &state = queuedSample.state;
                if (state.currentPosition == 65535) {
                    if (encoderRuntime.warmupCounter++ < 100) {
                        continue;
                    }
                }

                const auto sample = create_encoder_sample(state);
                TimestampedPayloadFrame frame;
                frame.hostTimestampNs =
                    queuedSample.hostTimestampNs == 0 ? currentSystemTimeNs() : queuedSample.hostTimestampNs;
                const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
                frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
                batchFrames.push_back(std::move(frame));
                wroteFrame = true;

                if (!encoderRuntime.firstSampleLogged) {
                    std::cout << "[Encoder-" << encoderRuntime.config.label << "] First sample: "
                              << "raw=" << state.currentPosition
                              << ", rad=" << state.currentPositionRad
                              << ", speed_raw=" << state.currentSpeed
                              << ", speed_rad=" << state.currentSpeedRad
                              << ", ts_ns=" << queuedSample.hostTimestampNs
                              << std::endl;
                    encoderRuntime.firstSampleLogged = true;
                }
            }
            emitTimestampedBatch(&encoderRuntime.timestampSmoothing, &batchFrames, emitFrame);
            return wroteFrame;
        };

        wroteData = handleEncoder(rightEncoder, rightPendingWrites) || wroteData;
        wroteData = handleEncoder(leftEncoder, leftPendingWrites) || wroteData;

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

    const auto drainImuRuntime = [](ImuRuntime &imuRuntime, PendingWriteQueue &pendingWrites) {
        const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
            auto message = buildQueuedMessage(
                imuRuntime.channel.id,
                imuRuntime.sequence++,
                timestampNs,
                payload.data(),
                payload.size());
            pendingWrites.push(std::move(message));
        };
        std::vector<TimestampedPayloadFrame> batchFrames;
        dmbot_serial::IM648_Data imuData;
        while (imuRuntime.driver && imuRuntime.driver->tryConsumeData(&imuData)) {
            const auto sample = create_imu_sample(imuData);
            TimestampedPayloadFrame frame;
            frame.hostTimestampNs = imuData.timestamp * kNanosecondsPerMicrosecond;
            if (frame.hostTimestampNs == 0) {
                frame.hostTimestampNs = currentSystemTimeNs();
            }
            const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
            frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
            batchFrames.push_back(std::move(frame));
        }
        emitTimestampedBatch(&imuRuntime.timestampSmoothing, &batchFrames, emitFrame);
    };

    const auto drainEncoderRuntime = [](EncoderRuntime &encoderRuntime, PendingWriteQueue &pendingWrites) {
        const auto emitFrame = [&](uint64_t timestampNs, std::vector<std::byte> &&payload) {
            auto message = buildQueuedMessage(
                encoderRuntime.channel.id,
                encoderRuntime.sequence++,
                timestampNs,
                payload.data(),
                payload.size());
            pendingWrites.push(std::move(message));
        };
        std::vector<TimestampedPayloadFrame> batchFrames;
        EncoderQueuedSample queuedSample;
        while (encoderRuntime.driver && encoderRuntime.driver->tryConsumeSample(&queuedSample)) {
            const auto &state = queuedSample.state;
            if (state.currentPosition == 65535) {
                if (encoderRuntime.warmupCounter++ < 100) {
                    continue;
                }
            }

            const auto sample = create_encoder_sample(state);
            TimestampedPayloadFrame frame;
            frame.hostTimestampNs =
                queuedSample.hostTimestampNs == 0 ? currentSystemTimeNs() : queuedSample.hostTimestampNs;
            const auto *sampleBytes = reinterpret_cast<const std::byte *>(&sample);
            frame.payload.assign(sampleBytes, sampleBytes + sizeof(sample));
            batchFrames.push_back(std::move(frame));
        }
        emitTimestampedBatch(&encoderRuntime.timestampSmoothing, &batchFrames, emitFrame);
    };

    drainImuRuntime(rightImu, rightPendingWrites);
    drainImuRuntime(leftImu, leftPendingWrites);
    drainEncoderRuntime(rightEncoder, rightPendingWrites);
    drainEncoderRuntime(leftEncoder, leftPendingWrites);

    motionEventQueue.stop();
    if (motionWorkerThread.joinable()) {
        motionWorkerThread.join();
    }

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
    std::cout << "[BatchTimestamp-imu_right] batches=" << rightImu.timestampSmoothing.smoothedBatchCount
              << ", frames=" << rightImu.timestampSmoothing.smoothedFrameCount << std::endl;
    std::cout << "[BatchTimestamp-imu_left] batches=" << leftImu.timestampSmoothing.smoothedBatchCount
              << ", frames=" << leftImu.timestampSmoothing.smoothedFrameCount << std::endl;
    std::cout << "[BatchTimestamp-encoder_right] batches=" << rightEncoder.timestampSmoothing.smoothedBatchCount
              << ", frames=" << rightEncoder.timestampSmoothing.smoothedFrameCount << std::endl;
    std::cout << "[BatchTimestamp-encoder_left] batches=" << leftEncoder.timestampSmoothing.smoothedBatchCount
              << ", frames=" << leftEncoder.timestampSmoothing.smoothedFrameCount << std::endl;
    std::cout << "[SensorWriteQueue-right] max_backlog=" << rightPendingWrites.maxObservedBacklog << std::endl;
    std::cout << "[SensorWriteQueue-left] max_backlog=" << leftPendingWrites.maxObservedBacklog << std::endl;
    std::cout << "Right MCAP log saved to " << rightWriter.outputFile << std::endl;
    std::cout << "Left MCAP log saved to " << leftWriter.outputFile << std::endl;
    if (motionAlertFd >= 0) {
        close(motionAlertFd);
    }
    return 0;
}
