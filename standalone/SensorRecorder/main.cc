#include "encoder_driver.h"
#include "utils/logger.h"
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
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

std::atomic<bool> g_stopFlag(false);

constexpr uint64_t kEncoderNominalPeriodNs = 1'000'000ULL;

struct EncoderSample {
    int32_t raw;
    float rad;
};

struct SensorSideConfig {
    std::string label;
    std::string encoderPort;
};

struct SideWriter {
    SensorSideConfig config;
    fs::path outputFile;
    mcap::McapWriter writer;
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
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "[SensorWriteQueue-" << label
                                     << "] backlog grew to " << queue.size()).str());
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

static_assert(sizeof(EncoderSample) == 8, "EncoderSample layout changed");

void signalHandler(int signum) {
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Interrupt signal (" << signum << ") received. Stopping...").str());
    g_stopFlag = true;
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
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "Failed to write " << pendingQueue->label
                                  << " sensor frame: " << writeStatus.message).str());
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
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Encoder read thread stopped for " << label).str());
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
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Encoder request thread stopped for " << label).str());
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
    sideWriter->outputFile = outputDir / ("sensor_" + config.label + ".mcap");
    sideWriter->encoderSchema = buildEncoderSchema();

    mcap::McapWriterOptions options("sensor_recorder_" + config.label);
    options.noChunking = false;
    options.chunkSize = 256 * 1024;
    options.compression = mcap::Compression::Lz4;
    options.compressionLevel = mcap::CompressionLevel::Default;
    options.forceCompression = false;
    // Keep schema/channel metadata readable by downstream validation so topic-based
    // checks can resolve encoder channels reliably.
    options.noRepeatedSchemas = false;
    options.noRepeatedChannels = false;
    options.noMessageIndex = true;

    auto status = sideWriter->writer.open(sideWriter->outputFile.string(), options);
    if (!status.ok()) {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "Failed to open MCAP file for " << config.label << ": " << status.message).str());
        return false;
    }

    sideWriter->writer.addSchema(sideWriter->encoderSchema);
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Recording " << config.label << " sensor data to " << sideWriter->outputFile).str());
    return true;
}

bool connectEncoderWithFallback(EncoderRuntime &encoderRuntime) {
    auto connectStatus = encoderRuntime.driver->connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (connectStatus == ConnectStatus::SERIAL_FAIL) {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "Failed to connect encoder serial port: " << encoderRuntime.config.encoderPort).str());
        return false;
    }

    if (connectStatus == ConnectStatus::NO_RESPONSE) {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << encoderRuntime.config.label
                             << " encoder not responding at 1Mbps, falling back to 115200...").str());
        encoderRuntime.driver->disconnect();
        encoderRuntime.driver->resetBaudrate(115200);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        connectStatus = encoderRuntime.driver->connect();
        if (connectStatus == ConnectStatus::SUCCESS) {
            DM_LOG_INFO("{}", (::DA::utils::LogString() << encoderRuntime.config.label
                                 << " encoder ready at 115200, switching back to 1Mbps.").str());
            encoderRuntime.driver->setBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            encoderRuntime.driver->disconnect();
            encoderRuntime.driver->resetBaudrate(1000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            connectStatus = encoderRuntime.driver->connect();
        }
        if (connectStatus != ConnectStatus::SUCCESS) {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << encoderRuntime.config.label
                                  << " encoder failed to respond after baudrate adjustments.").str());
            return false;
        }
    } else {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << encoderRuntime.config.label << " encoder ready at 1Mbps.").str());
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
    bool outputDirSet = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (!outputDirSet && !argument.empty() && argument[0] != '-') {
            outputDir = fs::path(argument);
            outputDirSet = true;
            continue;
        }
        if (argument == "-h" || argument == "--help") {
            std::cout
                << "Usage: SensorRecorder [OUTPUT_DIR]\n"
                << "Records left/right encoder streams into MCAP files.\n";
            return 0;
        }
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "Unknown argument: " << argument).str());
        return -1;
    }
    std::error_code ec;
    if (!fs::exists(outputDir)) {
        fs::create_directories(outputDir, ec);
        if (ec) {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "Failed to create output directory: " << ec.message()).str());
            return -1;
        }
    }

    const SensorSideConfig rightConfig{"right", "/dev/right_encoder"};
    const SensorSideConfig leftConfig{"left", "/dev/left_encoder"};

    SideWriter rightWriter;
    SideWriter leftWriter;
    if (!openSideWriter(outputDir, rightConfig, &rightWriter) || !openSideWriter(outputDir, leftConfig, &leftWriter)) {
        return -1;
    }

    EncoderRuntime rightEncoder{rightConfig, nullptr, mcap::Channel("encoder_right", "binary", rightWriter.encoderSchema.id)};
    EncoderRuntime leftEncoder{leftConfig, nullptr, mcap::Channel("encoder_left", "binary", leftWriter.encoderSchema.id)};
    rightEncoder.timestampSmoothing = CreateBatchTimestampSmoothingState("encoder_right", kEncoderNominalPeriodNs);
    leftEncoder.timestampSmoothing = CreateBatchTimestampSmoothingState("encoder_left", kEncoderNominalPeriodNs);
    PendingWriteQueue rightPendingWrites{rightConfig.label};
    PendingWriteQueue leftPendingWrites{leftConfig.label};

    rightWriter.writer.addChannel(rightEncoder.channel);
    leftWriter.writer.addChannel(leftEncoder.channel);

    std::thread rightWriterThread(sideWriterThreadFunc, &rightWriter, &rightPendingWrites);
    std::thread leftWriterThread(sideWriterThreadFunc, &leftWriter, &leftPendingWrites);

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Opening dual-arm sensors: "
                         << "right(encoder=" << rightConfig.encoderPort << ") "
                         << "left(encoder=" << leftConfig.encoderPort << ")").str());

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

        const auto handleEncoder = [&](EncoderRuntime &encoderRuntime, PendingWriteQueue &pendingWrites) {
            if (!encoderRuntime.connected) {
                return false;
            }
            if (!encoderRuntime.driver->isConnected()) {
                encoderRuntime.connected = false;
                DM_LOG_ERROR("{}", (::DA::utils::LogString() << encoderRuntime.config.label
                                      << " encoder disconnected during recording; stopping encoder samples for this side.").str());
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
                    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[Encoder-" << encoderRuntime.config.label << "] First sample: "
                                         << "raw=" << state.currentPosition
                                         << ", rad=" << state.currentPositionRad
                                         << ", speed_raw=" << state.currentSpeed
                                         << ", speed_rad=" << state.currentSpeedRad
                                         << ", ts_ns=" << queuedSample.hostTimestampNs).str());
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
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[BatchTimestamp-encoder_right] batches=" << rightEncoder.timestampSmoothing.smoothed_batch_count
                         << ", frames=" << rightEncoder.timestampSmoothing.smoothed_frame_count).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[BatchTimestamp-encoder_left] batches=" << leftEncoder.timestampSmoothing.smoothed_batch_count
                         << ", frames=" << leftEncoder.timestampSmoothing.smoothed_frame_count).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[SensorStats-encoder_right] received_samples=" << rightEncoder.receivedSampleCount
                         << ", emitted_messages=" << rightEncoder.emittedMessageCount
                         << ", last_sequence=" << rightEncoder.lastEmittedSequence
                         << ", last_timestamp_ns=" << rightEncoder.lastEmittedTimestampNs).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[SensorStats-encoder_left] received_samples=" << leftEncoder.receivedSampleCount
                         << ", emitted_messages=" << leftEncoder.emittedMessageCount
                         << ", last_sequence=" << leftEncoder.lastEmittedSequence
                         << ", last_timestamp_ns=" << leftEncoder.lastEmittedTimestampNs).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[SensorWriteQueue-right] max_backlog=" << rightPendingWrites.maxObservedBacklog).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[SensorWriteQueue-left] max_backlog=" << leftPendingWrites.maxObservedBacklog).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[SensorWriter-right] written_messages="
                         << rightWriter.writtenMessageCount.load(std::memory_order_relaxed)
                         << ", write_failures=" << rightWriter.writeFailureCount.load(std::memory_order_relaxed)
                         << ", last_written_sequence=" << rightWriter.lastWrittenSequence.load(std::memory_order_relaxed)
                         << ", last_written_log_time_ns=" << rightWriter.lastWrittenLogTimeNs.load(std::memory_order_relaxed)
                         << ", last_write_system_time_ns=" << rightWriter.lastWriteSystemTimeNs.load(std::memory_order_relaxed)).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[SensorWriter-left] written_messages="
                         << leftWriter.writtenMessageCount.load(std::memory_order_relaxed)
                         << ", write_failures=" << leftWriter.writeFailureCount.load(std::memory_order_relaxed)
                         << ", last_written_sequence=" << leftWriter.lastWrittenSequence.load(std::memory_order_relaxed)
                         << ", last_written_log_time_ns=" << leftWriter.lastWrittenLogTimeNs.load(std::memory_order_relaxed)
                         << ", last_write_system_time_ns=" << leftWriter.lastWriteSystemTimeNs.load(std::memory_order_relaxed)).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Right MCAP log saved to " << rightWriter.outputFile).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "Left MCAP log saved to " << leftWriter.outputFile).str());
    return 0;
}
