#include "gripper_hmi_driver.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kStateRequestIntervalMs = 1000;
constexpr uint64_t kLedRenderIntervalMs = GripperLedEffectRenderer::recommendedRenderIntervalMs();
constexpr uint64_t kLedResendIntervalMs = 250;
constexpr uint64_t kBeepResendIntervalMs = 20;
constexpr uint64_t kConnectFailureLogIntervalMs = 30000;
constexpr int kExclusiveCommandTimeoutMs = 500;
constexpr int kCalibrationChunkSendIntervalMs = 20;
constexpr int kCalibrationCommitDelayMs = 1500;
constexpr int kCalibrationBeginRetryLimit = 5;
constexpr int kCalibrationChunkRetryLimit = 8;
constexpr int kCalibrationReadRetryLimit = 5;
constexpr int kCalibrationRangeReadTimeoutMs = 6500;
constexpr int kCalibrationBeginSettleMs = 50;
constexpr int kCalibrationAbortDrainMs = 120;
constexpr int kCalibrationAbortSettleMs = 80;
constexpr int kCalibrationAbortRetryLimit = 2;
constexpr int kSerialNumberCommandRetryLimit = 3;
constexpr int kExclusiveCommandDrainIdleMs = 40;
constexpr int kExclusiveCommandDrainMaxMs = 120;
constexpr size_t kCalibrationChunkCount =
    gripper_hmi::kCalibrationPayloadSize / GripperHmiProtocol::kCalibrationChunkSize;

size_t trimTrailingZeroLength(const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0)
    {
        return 0;
    }

    size_t used = size;
    while (used > 0 && data[used - 1] == 0)
    {
        --used;
    }

    return used;
}

std::string formatHexByte(uint8_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned int>(value);
    return stream.str();
}

std::string describeStatusFrame(uint8_t token, uint8_t statusCode)
{
    return "status token=" + formatHexByte(token) + " code=" + formatHexByte(statusCode) +
           " (" + GripperHmiProtocol::describeStatusCode(statusCode) + ")";
}

bool isValidEightByteRecvFrame(const uint8_t *frame)
{
    if (frame == nullptr)
    {
        return false;
    }

    return GripperHmiProtocol::validateXor(
        frame,
        GripperHmiProtocol::kStatusFrameLength - 1,
        frame[GripperHmiProtocol::kStatusFrameLength - 1]);
}

bool isExclusiveStatusFrame(const uint8_t *frame)
{
    if (!isValidEightByteRecvFrame(frame))
    {
        return false;
    }

    return frame[4] == 0x00 && frame[5] == 0x00 && frame[6] == 0xA5;
}

bool isRetryableCalibrationStatus(uint8_t statusCode)
{
    return statusCode == GripperHmiProtocol::kStatusMissingData ||
           statusCode == GripperHmiProtocol::kStatusZeroDataChecksumError ||
           statusCode == GripperHmiProtocol::kStatusChecksumError ||
           statusCode == GripperHmiProtocol::kStatusAddressOutOfLimit;
}

bool isCalibrationAbortRecoveryStatus(uint8_t statusCode)
{
    return statusCode == GripperHmiProtocol::kStatusMissingData ||
           statusCode == GripperHmiProtocol::kStatusZeroDataChecksumError;
}

bool isCalibrationWriteAckToken(size_t packetIndex, uint8_t token)
{
    return token == static_cast<uint8_t>(packetIndex) ||
           token == static_cast<uint8_t>(packetIndex + 1);
}

bool validateCalibrationReadFrame(const uint8_t *frame)
{
    if (frame == nullptr)
    {
        return false;
    }

    return GripperHmiProtocol::calculateXor(
               frame + 3,
               GripperHmiProtocol::kCalibrationChunkSize) ==
           frame[GripperHmiProtocol::kCalibrationChunkFrameLength - 1];
}

}

static std::string resolveSerialPortPath(const std::string &configuredPort)
{
    std::error_code ec;
    if (fs::exists(configuredPort, ec))
    {
        const fs::path resolved = fs::weakly_canonical(configuredPort, ec);
        if (!ec && !resolved.empty())
        {
            return resolved.string();
        }
    }
    return configuredPort;
}

GripperHmiDriver::GripperHmiDriver(const std::string &port, uint32_t baudrate, const std::string &name)
    : name_(name), port_(port), baudrate_(baudrate), serialPort_(nullptr)
{
}

GripperHmiDriver::~GripperHmiDriver()
{
    disconnect();
}

uint64_t GripperHmiDriver::currentSteadyMs()
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

void GripperHmiDriver::logConnectFailureLocked(const std::string &message)
{
    const uint64_t nowMs = currentSteadyMs();
    const bool sameFailure = (message == lastConnectFailureMessage_);
    const bool shouldLog = !sameFailure ||
                           lastConnectFailureLogAtMs_ == 0 ||
                           (nowMs - lastConnectFailureLogAtMs_) >= kConnectFailureLogIntervalMs;

    if (!shouldLog)
    {
        ++suppressedConnectFailureCount_;
        return;
    }

    if (sameFailure && suppressedConnectFailureCount_ > 0)
    {
        const uint64_t suppressedWindowMs =
            firstConnectFailureAtMs_ == 0 || nowMs < firstConnectFailureAtMs_
                ? 0
                : (nowMs - firstConnectFailureAtMs_);
        std::cerr << name_ << ": " << message
                  << " (suppressed " << suppressedConnectFailureCount_
                  << " repeated attempts over " << (suppressedWindowMs / 1000) << "s)"
                  << std::endl;
    }
    else
    {
        std::cerr << name_ << ": " << message << std::endl;
    }

    lastConnectFailureMessage_ = message;
    lastConnectFailureLogAtMs_ = nowMs;
    firstConnectFailureAtMs_ = nowMs;
    suppressedConnectFailureCount_ = 0;
}

void GripperHmiDriver::resetConnectFailureLogLocked(const std::string &resolvedPort)
{
    if (lastConnectFailureMessage_.empty())
    {
        return;
    }

    const uint64_t nowMs = currentSteadyMs();
    const uint64_t outageMs =
        firstConnectFailureAtMs_ == 0 || nowMs < firstConnectFailureAtMs_
            ? 0
            : (nowMs - firstConnectFailureAtMs_);
    const uint32_t totalAttempts = suppressedConnectFailureCount_ + 1;

    std::cout << name_ << ": reconnected port " << port_
              << " (resolved=" << resolvedPort << ") after " << totalAttempts
              << " failed attempt" << (totalAttempts == 1 ? "" : "s")
              << " over " << (outageMs / 1000) << "s" << std::endl;

    lastConnectFailureMessage_.clear();
    lastConnectFailureLogAtMs_ = 0;
    firstConnectFailureAtMs_ = 0;
    suppressedConnectFailureCount_ = 0;
}

bool GripperHmiDriver::connect()
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ != nullptr)
    {
        return true;
    }
    if (ioThread_.joinable())
    {
        ioThread_.join();
    }

    const std::string resolvedPort = resolveSerialPortPath(port_);
    if (sp_get_port_by_name(resolvedPort.c_str(), &serialPort_) != SP_OK)
    {
        logConnectFailureLocked("cannot find port " + port_ + " (resolved=" + resolvedPort + ")");
        serialPort_ = nullptr;
        return false;
    }

    if (sp_open(serialPort_, SP_MODE_READ_WRITE) != SP_OK)
    {
        logConnectFailureLocked("cannot open port " + port_ + " (resolved=" + resolvedPort + ")");
        sp_free_port(serialPort_);
        serialPort_ = nullptr;
        return false;
    }

    sp_set_baudrate(serialPort_, static_cast<int>(baudrate_));
    sp_set_bits(serialPort_, 8);
    sp_set_parity(serialPort_, SP_PARITY_NONE);
    sp_set_stopbits(serialPort_, 1);
    sp_set_flowcontrol(serialPort_, SP_FLOWCONTROL_NONE);
    sp_flush(serialPort_, SP_BUF_BOTH);

    rxBuffer_.clear();
    pendingStateRequest_ = true;
    pendingLedUpdate_ = false;
    pendingBeepUpdate_ = false;
    pendingLedColor_ = {};
    pendingBeepState_ = {};
    activeBeepState_ = {};
    ledEffectEnabled_ = false;
    ledEffectDirty_ = false;
    ledEffect_ = {};
    pendingLedColor_ = GripperLedColor{0, 0, 0};
    lastRenderedColor_ = {255, 255, 255};
    lastLedRenderAtMs_ = 0;
    lastStateRequestAtMs_ = 0;
    lastLedWriteAtMs_ = 0;
    lastBeepWriteAtMs_ = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        keyPressed_.fill(false);
        beepState_ = {};
        lastKeyReport_.reset();
        lastReceiveTimeMs_ = 0;
        stateGeneration_ = 0;
    }

    resetConnectFailureLogLocked(resolvedPort);
    ioRunning_ = true;
    ioThread_ = std::thread(&GripperHmiDriver::ioLoop, this);
    return true;
}

void GripperHmiDriver::disconnect()
{
    {
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        ioRunning_ = false;
        pendingStateRequest_ = false;
        pendingLedUpdate_ = false;
        pendingBeepUpdate_ = false;
    }
    ioCv_.notify_all();
    if (ioThread_.joinable())
    {
        ioThread_.join();
    }

    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ == nullptr)
    {
        return;
    }

    const auto offFrame = GripperHmiProtocol::buildSetRgbCommand(GripperLedColor{0, 0, 0});
    writeFrameLocked(offFrame.data(), offFrame.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sp_flush(serialPort_, SP_BUF_BOTH);
    sp_close(serialPort_);
    sp_free_port(serialPort_);
    serialPort_ = nullptr;
}

bool GripperHmiDriver::isConnected() const
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    return serialPort_ != nullptr;
}

bool GripperHmiDriver::writeFrameLocked(const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }

    if (serialPort_ == nullptr)
    {
        return false;
    }

    const auto written = sp_blocking_write(serialPort_, data, size, 50);
    if (written < 0 || static_cast<size_t>(written) != size)
    {
        handleIoFailureLocked("write");
        return false;
    }

    return true;
}

void GripperHmiDriver::handleIoFailureLocked(const char *operation)
{
    std::cerr << name_ << ": " << operation << " failed on " << port_ << std::endl;
    pendingStateRequest_ = false;
    pendingLedUpdate_ = false;
    pendingBeepUpdate_ = false;
    ioRunning_ = false;
    if (serialPort_ != nullptr)
    {
        sp_flush(serialPort_, SP_BUF_BOTH);
        sp_close(serialPort_);
        sp_free_port(serialPort_);
        serialPort_ = nullptr;
    }
}

bool GripperHmiDriver::requestState()
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ == nullptr)
    {
        return false;
    }
    pendingStateRequest_ = true;
    ioCv_.notify_one();
    return true;
}

bool GripperHmiDriver::setLedColor(const GripperLedColor &color)
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ == nullptr)
    {
        return false;
    }
    ledEffectEnabled_ = false;
    pendingLedColor_ = color;
    pendingLedUpdate_ = true;
    ioCv_.notify_one();
    return true;
}

bool GripperHmiDriver::setLedColor(uint8_t red, uint8_t green, uint8_t blue)
{
    return setLedColor(GripperLedColor{red, green, blue});
}

bool GripperHmiDriver::setLedEffect(const GripperLedEffect &effect)
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ == nullptr)
    {
        return false;
    }

    ledEffect_ = effect;
    ledEffectEnabled_ = true;
    ledEffectDirty_ = true;
    ioCv_.notify_one();
    return true;
}

bool GripperHmiDriver::setBeepState(const GripperBeepState &state)
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ == nullptr)
    {
        return false;
    }

    pendingBeepState_ = state;
    activeBeepState_ = state;
    pendingBeepUpdate_ = true;
    ioCv_.notify_one();
    return true;
}

bool GripperHmiDriver::setBeepEnabled(bool enabled)
{
    if (!enabled)
    {
        return silenceBeep();
    }
    return setBeepState(GripperBeepState{kDefaultBeepDuty, kDefaultBeepFrequency});
}

bool GripperHmiDriver::silenceBeep()
{
    return setBeepState(GripperBeepState{0, 0});
}

bool GripperHmiDriver::pauseIoThreadForExclusiveCommand(std::unique_lock<std::mutex> *ioLock)
{
    if (ioLock == nullptr || !ioLock->owns_lock())
    {
        return false;
    }

    if (serialPort_ == nullptr)
    {
        return false;
    }

    if (ioThread_.joinable())
    {
        ioRunning_ = false;
        ioCv_.notify_all();
        ioLock->unlock();
        ioThread_.join();
        ioLock->lock();
    }

    rxBuffer_.clear();
    sp_flush(serialPort_, SP_BUF_BOTH);

    // The background IO loop may have sent a low-frequency probe frame just
    // before we stopped it. Give the firmware a short quiet window to finish
    // that reply and drain any trailing async frame before starting an
    // exclusive SN/calibration transaction.
    auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kExclusiveCommandDrainIdleMs);
    const auto drainMaxDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kExclusiveCommandDrainMaxMs);
    uint8_t temp[256];
    while (std::chrono::steady_clock::now() < drainDeadline &&
           std::chrono::steady_clock::now() < drainMaxDeadline)
    {
        const int bytesRead = sp_nonblocking_read(serialPort_, temp, sizeof(temp));
        if (bytesRead < 0)
        {
            handleIoFailureLocked("read");
            return false;
        }
        if (bytesRead > 0)
        {
            drainDeadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(kExclusiveCommandDrainIdleMs);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return serialPort_ != nullptr;
}

void GripperHmiDriver::resumeIoThreadLocked()
{
    if (serialPort_ == nullptr || ioRunning_)
    {
        return;
    }

    ioRunning_ = true;
    ioThread_ = std::thread(&GripperHmiDriver::ioLoop, this);
}

bool GripperHmiDriver::readBytesLocked(std::vector<uint8_t> *buffer, int timeoutMs)
{
    if (buffer == nullptr || serialPort_ == nullptr)
    {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    uint8_t temp[256];
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int bytesRead = sp_nonblocking_read(serialPort_, temp, sizeof(temp));
        if (bytesRead < 0)
        {
            handleIoFailureLocked("read");
            return false;
        }
        if (bytesRead > 0)
        {
            buffer->insert(buffer->end(), temp, temp + bytesRead);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return true;
}

bool GripperHmiDriver::readSizedFrameLocked(size_t frameSize, std::vector<uint8_t> *frame, int timeoutMs)
{
    if (frame == nullptr || serialPort_ == nullptr || frameSize < 4)
    {
        return false;
    }

    std::vector<uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const std::array<uint8_t, 2> header = {GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2};

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            break;
        }

        if (!readBytesLocked(&buffer, static_cast<int>(remaining.count())))
        {
            return false;
        }

        while (buffer.size() >= header.size())
        {
            auto headerPos = std::search(buffer.begin(), buffer.end(), header.begin(), header.end());
            if (headerPos == buffer.end())
            {
                if (buffer.size() > 1)
                {
                    buffer.erase(buffer.begin(), buffer.end() - 1);
                }
                break;
            }

            if (headerPos != buffer.begin())
            {
                buffer.erase(buffer.begin(), headerPos);
            }

            if (buffer.size() < frameSize)
            {
                break;
            }

            if (!GripperHmiProtocol::validateXor(buffer.data(), frameSize - 1, buffer[frameSize - 1]))
            {
                buffer.erase(buffer.begin());
                continue;
            }

            frame->assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frameSize));
            return true;
        }
    }

    return false;
}

bool GripperHmiDriver::readStatusFrameLocked(uint8_t token, uint8_t *statusCode, int timeoutMs)
{
    return readExpectedStatusFrameLocked(token, nullptr, statusCode, timeoutMs);
}

bool GripperHmiDriver::readAnyStatusFrameLocked(uint8_t *token, uint8_t *statusCode, int timeoutMs)
{
    return readExpectedStatusFrameLocked(0xFF, token, statusCode, timeoutMs);
}

bool GripperHmiDriver::abortCalibrationWriteStateLocked(const std::string &reason)
{
    if (serialPort_ == nullptr)
    {
        return false;
    }

    const auto abortCommand = GripperHmiProtocol::buildAbortCalibrationWriteCommand();
    if (!writeFrameLocked(abortCommand.data(), abortCommand.size()))
    {
        lastCommandError_ = "failed to send abort calibration write command";
        return false;
    }

    std::vector<uint8_t> drain;
    if (!readBytesLocked(&drain, kCalibrationAbortDrainMs))
    {
        lastCommandError_ = "failed while draining abort calibration write response";
        return false;
    }

    sp_flush(serialPort_, SP_BUF_INPUT);
    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationAbortSettleMs));
    std::cerr << name_ << ": abort calibration write state: " << reason << std::endl;
    return true;
}

bool GripperHmiDriver::readExpectedStatusFrameLocked(uint8_t expectedToken, uint8_t *responseToken, uint8_t *statusCode, int timeoutMs)
{
    if (serialPort_ == nullptr)
    {
        return false;
    }

    std::vector<uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const std::array<uint8_t, 2> header = {GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2};

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            break;
        }

        if (!readBytesLocked(&buffer, static_cast<int>(remaining.count())))
        {
            return false;
        }

        while (buffer.size() >= header.size())
        {
            auto headerPos = std::search(buffer.begin(), buffer.end(), header.begin(), header.end());
            if (headerPos == buffer.end())
            {
                if (buffer.size() > 1)
                {
                    buffer.erase(buffer.begin(), buffer.end() - 1);
                }
                break;
            }

            if (headerPos != buffer.begin())
            {
                buffer.erase(buffer.begin(), headerPos);
            }

            if (buffer.size() < GripperHmiProtocol::kStatusFrameLength)
            {
                break;
            }

            if (!isValidEightByteRecvFrame(buffer.data()))
            {
                buffer.erase(buffer.begin());
                continue;
            }

            // A full 8-byte frame from another function should be skipped as
            // a whole frame so it does not block the following SN/calibration
            // response in the buffer.
            if (!isExclusiveStatusFrame(buffer.data()))
            {
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<std::ptrdiff_t>(GripperHmiProtocol::kStatusFrameLength));
                continue;
            }

            const uint8_t parsedToken = buffer[2];
            const uint8_t parsedStatusCode = buffer[3];
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(GripperHmiProtocol::kStatusFrameLength));

            if (expectedToken != 0xFF && parsedToken != expectedToken)
            {
                continue;
            }

            if (responseToken != nullptr)
            {
                *responseToken = parsedToken;
            }
            if (statusCode != nullptr)
            {
                *statusCode = parsedStatusCode;
            }
            return true;
        }
    }

    return false;
}

bool GripperHmiDriver::readDataFrameLocked(uint8_t token, size_t dataLength, std::vector<uint8_t> *payload, int timeoutMs)
{
    std::vector<uint8_t> frame;
    const size_t frameSize = 2 + 1 + dataLength + 1;
    if (!readSizedFrameLocked(frameSize, &frame, timeoutMs))
    {
        return false;
    }

    if (frame.size() != frameSize || frame[2] != token)
    {
        return false;
    }

    if (payload != nullptr)
    {
        payload->assign(frame.begin() + 3, frame.begin() + 3 + static_cast<std::ptrdiff_t>(dataLength));
    }
    return true;
}

bool GripperHmiDriver::readRawDataFrameLocked(size_t dataLength, std::vector<uint8_t> *payload, int timeoutMs)
{
    std::vector<uint8_t> frame;
    const size_t frameSize = 2 + dataLength + 1;
    if (!readSizedFrameLocked(frameSize, &frame, timeoutMs))
    {
        return false;
    }

    if (frame.size() != frameSize)
    {
        return false;
    }

    if (payload != nullptr)
    {
        payload->assign(frame.begin() + 2, frame.begin() + 2 + static_cast<std::ptrdiff_t>(dataLength));
    }
    return true;
}

bool GripperHmiDriver::readCalibrationFrameLocked(uint8_t expectedToken, std::vector<uint8_t> *payload, int timeoutMs)
{
    if (serialPort_ == nullptr)
    {
        return false;
    }

    std::vector<uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const size_t frameSize = 2 + 1 + GripperHmiProtocol::kCalibrationChunkSize + 1;
    const std::array<uint8_t, 2> header = {GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2};

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            break;
        }

        if (!readBytesLocked(&buffer, static_cast<int>(remaining.count())))
        {
            return false;
        }

        while (buffer.size() >= header.size())
        {
            auto headerPos = std::search(buffer.begin(), buffer.end(), header.begin(), header.end());
            if (headerPos == buffer.end())
            {
                if (buffer.size() > 1)
                {
                    buffer.erase(buffer.begin(), buffer.end() - 1);
                }
                break;
            }

            if (headerPos != buffer.begin())
            {
                buffer.erase(buffer.begin(), headerPos);
            }

            if (buffer.size() >= frameSize)
            {
                if (validateCalibrationReadFrame(buffer.data()))
                {
                    const uint8_t token = buffer[2];
                    if (token == expectedToken)
                    {
                        if (payload != nullptr)
                        {
                            payload->assign(buffer.begin() + 3,
                                            buffer.begin() + 3 + static_cast<std::ptrdiff_t>(GripperHmiProtocol::kCalibrationChunkSize));
                        }
                        return true;
                    }

                    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frameSize));
                    continue;
                }
            }

            if (buffer.size() < frameSize)
            {
                break;
            }

            buffer.erase(buffer.begin());
        }
    }

    return false;
}

GripperHmiDriver::ExclusiveFrameReadResult GripperHmiDriver::readRawDataOrStatusFrameLocked(size_t dataLength,
                                                                                             std::vector<uint8_t> *payload,
                                                                                             uint8_t *token,
                                                                                             uint8_t *statusCode,
                                                                                             int timeoutMs)
{
    if (serialPort_ == nullptr)
    {
        return ExclusiveFrameReadResult::Timeout;
    }

    std::vector<uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const size_t dataFrameSize = 2 + dataLength + 1;
    const std::array<uint8_t, 2> header = {GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2};

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            break;
        }

        if (!readBytesLocked(&buffer, static_cast<int>(remaining.count())))
        {
            return ExclusiveFrameReadResult::Timeout;
        }

        while (buffer.size() >= header.size())
        {
            auto headerPos = std::search(buffer.begin(), buffer.end(), header.begin(), header.end());
            if (headerPos == buffer.end())
            {
                if (buffer.size() > 1)
                {
                    buffer.erase(buffer.begin(), buffer.end() - 1);
                }
                break;
            }

            if (headerPos != buffer.begin())
            {
                buffer.erase(buffer.begin(), headerPos);
            }

            if (buffer.size() >= dataFrameSize &&
                GripperHmiProtocol::validateXor(buffer.data(), dataFrameSize - 1, buffer[dataFrameSize - 1]))
            {
                if (payload != nullptr)
                {
                    payload->assign(buffer.begin() + 2, buffer.begin() + 2 + static_cast<std::ptrdiff_t>(dataLength));
                }
                return ExclusiveFrameReadResult::Data;
            }

            if (buffer.size() >= GripperHmiProtocol::kStatusFrameLength &&
                isExclusiveStatusFrame(buffer.data()))
            {
                if (token != nullptr)
                {
                    *token = buffer[2];
                }
                if (statusCode != nullptr)
                {
                    *statusCode = buffer[3];
                }
                return ExclusiveFrameReadResult::Status;
            }

            if (buffer.size() > dataFrameSize)
            {
                buffer.erase(buffer.begin());
                continue;
            }
            break;
        }
    }

    return ExclusiveFrameReadResult::Timeout;
}

GripperHmiDriver::ExclusiveFrameReadResult GripperHmiDriver::readDataOrStatusFrameLocked(uint8_t expectedToken,
                                                                                          size_t dataLength,
                                                                                          std::vector<uint8_t> *payload,
                                                                                          uint8_t *token,
                                                                                          uint8_t *statusCode,
                                                                                          int timeoutMs)
{
    if (serialPort_ == nullptr)
    {
        return ExclusiveFrameReadResult::Timeout;
    }

    std::vector<uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const size_t dataFrameSize = 2 + 1 + dataLength + 1;
    const std::array<uint8_t, 2> header = {GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2};

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            break;
        }

        if (!readBytesLocked(&buffer, static_cast<int>(remaining.count())))
        {
            return ExclusiveFrameReadResult::Timeout;
        }

        while (buffer.size() >= header.size())
        {
            auto headerPos = std::search(buffer.begin(), buffer.end(), header.begin(), header.end());
            if (headerPos == buffer.end())
            {
                if (buffer.size() > 1)
                {
                    buffer.erase(buffer.begin(), buffer.end() - 1);
                }
                break;
            }

            if (headerPos != buffer.begin())
            {
                buffer.erase(buffer.begin(), headerPos);
            }

            if (buffer.size() >= dataFrameSize &&
                buffer[2] == expectedToken &&
                validateCalibrationReadFrame(buffer.data()))
            {
                if (payload != nullptr)
                {
                    payload->assign(buffer.begin() + 3, buffer.begin() + 3 + static_cast<std::ptrdiff_t>(dataLength));
                }
                return ExclusiveFrameReadResult::Data;
            }

            if (buffer.size() >= GripperHmiProtocol::kStatusFrameLength &&
                isExclusiveStatusFrame(buffer.data()))
            {
                const uint8_t parsedToken = buffer[2];
                const uint8_t parsedStatusCode = buffer[3];
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<std::ptrdiff_t>(GripperHmiProtocol::kStatusFrameLength));

                if (parsedToken != expectedToken)
                {
                    continue;
                }

                if (token != nullptr)
                {
                    *token = parsedToken;
                }
                if (statusCode != nullptr)
                {
                    *statusCode = parsedStatusCode;
                }
                return ExclusiveFrameReadResult::Status;
            }

            if (buffer.size() > dataFrameSize)
            {
                buffer.erase(buffer.begin());
                continue;
            }
            break;
        }
    }

    return ExclusiveFrameReadResult::Timeout;
}

bool GripperHmiDriver::runExclusiveCommand(const std::function<bool()> &command)
{
    std::unique_lock<std::mutex> ioLock(ioMutex_);
    lastCommandError_.clear();
    if (serialPort_ == nullptr)
    {
        lastCommandError_ = "serial port is not connected";
        return false;
    }

    if (!pauseIoThreadForExclusiveCommand(&ioLock))
    {
        if (lastCommandError_.empty())
        {
            lastCommandError_ = "failed to pause background io loop";
        }
        return false;
    }

    const bool ok = command();
    if (serialPort_ != nullptr)
    {
        pendingStateRequest_ = true;
        resumeIoThreadLocked();
    }
    return ok;
}

bool GripperHmiDriver::readSerialNumber(std::string *serialNumber)
{
    if (serialNumber == nullptr)
    {
        return false;
    }

    return runExclusiveCommand([this, serialNumber]() {
        for (int attempt = 0; attempt < kSerialNumberCommandRetryLimit; ++attempt)
        {
            sp_flush(serialPort_, SP_BUF_INPUT);
            const auto request = GripperHmiProtocol::buildReadSerialNumberCommand();
            if (!writeFrameLocked(request.data(), request.size()))
            {
                lastCommandError_ = "failed to send read serial number command";
                return false;
            }

            std::vector<uint8_t> payload;
            uint8_t responseToken = 0;
            uint8_t statusCode = 0;
            const auto firstResult = readRawDataOrStatusFrameLocked(GripperHmiProtocol::kSerialNumberChunkSize,
                                                                    &payload,
                                                                    &responseToken,
                                                                    &statusCode,
                                                                    kExclusiveCommandTimeoutMs);
            if (firstResult == ExclusiveFrameReadResult::Data)
            {
                gripper_hmi::GripperSerialNumber encoded{};
                std::memcpy(encoded.data(), payload.data(), std::min(payload.size(), encoded.size()));
                std::vector<uint8_t> tailPayload;
                if (readRawDataOrStatusFrameLocked(GripperHmiProtocol::kSerialNumberChunkSize,
                                                  &tailPayload,
                                                  &responseToken,
                                                  &statusCode,
                                                  120) == ExclusiveFrameReadResult::Data)
                {
                    std::memcpy(encoded.data() + GripperHmiProtocol::kSerialNumberChunkSize,
                                tailPayload.data(),
                                std::min(tailPayload.size(),
                                         encoded.size() - GripperHmiProtocol::kSerialNumberChunkSize));
                }

                *serialNumber = gripper_hmi::decodeSerialNumber(encoded);
                if (serialNumber->size() == (GripperHmiProtocol::kSerialNumberChunkSize * 2) &&
                    serialNumber->substr(0, GripperHmiProtocol::kSerialNumberChunkSize) ==
                        serialNumber->substr(GripperHmiProtocol::kSerialNumberChunkSize))
                {
                    *serialNumber = serialNumber->substr(0, GripperHmiProtocol::kSerialNumberChunkSize);
                }
                return true;
            }

            if (firstResult == ExclusiveFrameReadResult::Status)
            {
                if (attempt + 1 < kSerialNumberCommandRetryLimit &&
                    isCalibrationAbortRecoveryStatus(statusCode) &&
                    abortCalibrationWriteStateLocked("serial read recovery after " +
                                                     describeStatusFrame(responseToken, statusCode)))
                {
                    continue;
                }

                lastCommandError_ = "serial number read failed: " + describeStatusFrame(responseToken, statusCode);
                std::cerr << name_ << ": " << lastCommandError_ << std::endl;
                return false;
            }

            if (!readStatusFrameLocked(GripperHmiProtocol::kIndexSerialNumber, &statusCode, kExclusiveCommandTimeoutMs))
            {
                lastCommandError_ = "timed out waiting for serial number response";
                return false;
            }

            if (attempt + 1 < kSerialNumberCommandRetryLimit &&
                isCalibrationAbortRecoveryStatus(statusCode) &&
                abortCalibrationWriteStateLocked("serial read status recovery after " +
                                                 GripperHmiProtocol::describeStatusCode(statusCode)))
            {
                continue;
            }

            lastCommandError_ = "serial number read failed: " + GripperHmiProtocol::describeStatusCode(statusCode);
            std::cerr << name_ << ": " << lastCommandError_ << std::endl;
            return false;
        }

        return false;
    });
}

bool GripperHmiDriver::writeSerialNumber(const std::string &serialNumber)
{
    gripper_hmi::GripperSerialNumber encoded{};
    if (!gripper_hmi::encodeSerialNumber(serialNumber, &encoded))
    {
        std::cerr << name_ << ": invalid serial number, expected 1.."
                  << gripper_hmi::kSerialNumberFieldLength
                  << " printable ASCII characters (32-byte field, zero-padded)" << std::endl;
        return false;
    }

    return runExclusiveCommand([this, &encoded]() {
        sp_flush(serialPort_, SP_BUF_INPUT);
        const auto request = GripperHmiProtocol::buildWriteSerialNumberCommand();
        if (!writeFrameLocked(request.data(), request.size()))
        {
            lastCommandError_ = "failed to send write serial number command";
            return false;
        }

        uint8_t responseToken = 0;
        uint8_t statusCode = 0;
        if (!readAnyStatusFrameLocked(&responseToken, &statusCode, kExclusiveCommandTimeoutMs))
        {
            lastCommandError_ = "timed out waiting for serial number write preamble acknowledgement";
            return false;
        }
        if (statusCode != GripperHmiProtocol::kStatusOk &&
            !(responseToken == GripperHmiProtocol::kIndexSerialNumber &&
              statusCode == GripperHmiProtocol::kStatusMissingData))
        {
            lastCommandError_ = "serial number write preamble failed: " +
                                GripperHmiProtocol::describeStatusCode(statusCode);
            std::cerr << name_ << ": " << lastCommandError_ << std::endl;
            return false;
        }

        const auto firstFrame = GripperHmiProtocol::buildWriteSerialNumberFrame(
            reinterpret_cast<const uint8_t *>(encoded.data()),
            GripperHmiProtocol::kSerialNumberChunkSize);
        if (!writeFrameLocked(firstFrame.data(), firstFrame.size()))
        {
            lastCommandError_ = "failed to send serial number payload chunk 0";
            return false;
        }

        if (!readAnyStatusFrameLocked(&responseToken, &statusCode, kExclusiveCommandTimeoutMs))
        {
            lastCommandError_ = "timed out waiting for serial number write acknowledgement chunk 0";
            return false;
        }

        if (statusCode != GripperHmiProtocol::kStatusOk)
        {
            lastCommandError_ = "serial number write chunk 0 failed: " + GripperHmiProtocol::describeStatusCode(statusCode);
            std::cerr << name_ << ": " << lastCommandError_ << std::endl;
            return false;
        }

        const auto secondFrame = GripperHmiProtocol::buildWriteSerialNumberFrame(
            reinterpret_cast<const uint8_t *>(encoded.data()) + GripperHmiProtocol::kSerialNumberChunkSize,
            GripperHmiProtocol::kSerialNumberChunkSize);
        if (!writeFrameLocked(secondFrame.data(), secondFrame.size()))
        {
            lastCommandError_ = "failed to send serial number payload chunk 1";
            return false;
        }

        if (!readAnyStatusFrameLocked(&responseToken, &statusCode, kExclusiveCommandTimeoutMs))
        {
            lastCommandError_ = "timed out waiting for serial number write acknowledgement chunk 1";
            return false;
        }

        if (statusCode != GripperHmiProtocol::kStatusOk)
        {
            lastCommandError_ = "serial number write chunk 1 failed: " + GripperHmiProtocol::describeStatusCode(statusCode);
            std::cerr << name_ << ": " << lastCommandError_ << std::endl;
            return false;
        }

        // The current V1.1 firmware needs a short settle window after the
        // final SN chunk before it reliably answers the next command.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return true;
    });
}

bool GripperHmiDriver::readCalibrationData(gripper_hmi::GripperCalibrationDataV1 *calibrationData)
{
    if (calibrationData == nullptr)
    {
        return false;
    }

    return runExclusiveCommand([this, calibrationData]() {
        sp_flush(serialPort_, SP_BUF_INPUT);

        std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> raw = {};
        std::array<bool, kCalibrationChunkCount> received = {};
        auto collectFrames = [this, &raw, &received](int timeoutMs, uint8_t *statusToken, uint8_t *statusCode) -> bool {
            if (serialPort_ == nullptr)
            {
                return false;
            }

            std::vector<uint8_t> buffer;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            const size_t dataFrameSize = 2 + 1 + GripperHmiProtocol::kCalibrationChunkSize + 1;
            const std::array<uint8_t, 2> header = {GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2};

            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0)
                {
                    break;
                }

                if (!readBytesLocked(&buffer, static_cast<int>(remaining.count())))
                {
                    return false;
                }

                while (buffer.size() >= header.size())
                {
                    auto headerPos = std::search(buffer.begin(), buffer.end(), header.begin(), header.end());
                    if (headerPos == buffer.end())
                    {
                        if (buffer.size() > 1)
                        {
                            buffer.erase(buffer.begin(), buffer.end() - 1);
                        }
                        break;
                    }

                    if (headerPos != buffer.begin())
                    {
                        buffer.erase(buffer.begin(), headerPos);
                    }

                    if (buffer.size() >= dataFrameSize &&
                        validateCalibrationReadFrame(buffer.data()))
                    {
                        const uint8_t token = buffer[2];

                        // Current firmware replies to the 0x00..0x3F range read
                        // with zero-based packet tokens 0x00..0x3F.
                        if (token < received.size())
                        {
                            const size_t packetIndex = static_cast<size_t>(token);
                            std::memcpy(raw.data() + packetIndex * GripperHmiProtocol::kCalibrationChunkSize,
                                        buffer.data() + 3,
                                        GripperHmiProtocol::kCalibrationChunkSize);
                            received[packetIndex] = true;
                        }

                        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(dataFrameSize));
                        continue;
                    }

                    if (buffer.size() >= GripperHmiProtocol::kStatusFrameLength &&
                        isExclusiveStatusFrame(buffer.data()))
                    {
                        if (statusToken != nullptr)
                        {
                            *statusToken = buffer[2];
                        }
                        if (statusCode != nullptr)
                        {
                            *statusCode = buffer[3];
                        }
                        buffer.erase(buffer.begin(),
                                     buffer.begin() + static_cast<std::ptrdiff_t>(GripperHmiProtocol::kStatusFrameLength));
                        continue;
                    }

                    if (buffer.size() < dataFrameSize)
                    {
                        break;
                    }

                    buffer.erase(buffer.begin());
                }
            }

            return true;
        };

        uint8_t responseToken = 0;
        uint8_t statusCode = 0;
        auto requestRangeRead = [this, &collectFrames, &responseToken, &statusCode]() -> bool {
            const auto request = GripperHmiProtocol::buildReadCalibrationChunkCommand(0x3F);
            if (!writeFrameLocked(request.data(), request.size()))
            {
                lastCommandError_ = "failed to send calibration read range command";
                return false;
            }
            return collectFrames(kCalibrationRangeReadTimeoutMs, &responseToken, &statusCode);
        };

        auto requestSingleChunkRead = [this](uint8_t packetIndex,
                                             std::vector<uint8_t> *payload,
                                             uint8_t *responseToken,
                                             uint8_t *responseStatusCode) -> ExclusiveFrameReadResult {
            if (payload != nullptr)
            {
                payload->clear();
            }

            const auto request = GripperHmiProtocol::buildReadCalibrationChunkCommand(packetIndex);
            if (!writeFrameLocked(request.data(), request.size()))
            {
                lastCommandError_ = "failed to send calibration read command for chunk " +
                                    std::to_string(static_cast<unsigned int>(packetIndex));
                return ExclusiveFrameReadResult::Timeout;
            }

            return readDataOrStatusFrameLocked(packetIndex,
                                               GripperHmiProtocol::kCalibrationChunkSize,
                                               payload,
                                               responseToken,
                                               responseStatusCode,
                                               kExclusiveCommandTimeoutMs);
        };

        bool rangeReadCollected = false;
        for (int attempt = 0; attempt < kCalibrationReadRetryLimit; ++attempt)
        {
            raw.fill(0);
            received.fill(false);
            responseToken = 0;
            statusCode = 0;

            if (!requestRangeRead())
            {
                lastCommandError_ = "failed while collecting calibration read frames";
                return false;
            }

            if (std::all_of(received.begin(), received.end(), [](bool value) { return value; }))
            {
                rangeReadCollected = true;
                break;
            }

            if (isCalibrationAbortRecoveryStatus(statusCode) &&
                attempt + 1 < kCalibrationReadRetryLimit &&
                abortCalibrationWriteStateLocked("range read recovery after " +
                                                 describeStatusFrame(responseToken, statusCode)))
            {
                continue;
            }

            if (!isRetryableCalibrationStatus(statusCode))
            {
                break;
            }

            sp_flush(serialPort_, SP_BUF_INPUT);
            std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
        }

        const bool needSequentialFallback =
            !rangeReadCollected &&
            !std::all_of(received.begin(), received.end(), [](bool value) { return value; }) &&
            isRetryableCalibrationStatus(statusCode);
        if (needSequentialFallback)
        {
            sp_flush(serialPort_, SP_BUF_INPUT);
            raw.fill(0);
            received.fill(false);
            responseToken = 0;
            statusCode = 0;

            for (size_t packetIndex = 0; packetIndex < received.size(); ++packetIndex)
            {
                std::vector<uint8_t> payload;
                ExclusiveFrameReadResult result = ExclusiveFrameReadResult::Timeout;
                bool chunkReceived = false;

                for (int attempt = 0; attempt < kCalibrationChunkRetryLimit; ++attempt)
                {
                    result = requestSingleChunkRead(static_cast<uint8_t>(packetIndex),
                                                    &payload,
                                                    &responseToken,
                                                    &statusCode);
                    if (result == ExclusiveFrameReadResult::Data &&
                        payload.size() == GripperHmiProtocol::kCalibrationChunkSize)
                    {
                        std::memcpy(raw.data() + packetIndex * GripperHmiProtocol::kCalibrationChunkSize,
                                    payload.data(),
                                    GripperHmiProtocol::kCalibrationChunkSize);
                        received[packetIndex] = true;
                        chunkReceived = true;
                        break;
                    }

                    if (result == ExclusiveFrameReadResult::Status &&
                        isCalibrationAbortRecoveryStatus(statusCode) &&
                        attempt + 1 < kCalibrationChunkRetryLimit &&
                        abortCalibrationWriteStateLocked("chunk read recovery packet=" +
                                                         std::to_string(packetIndex) + " after " +
                                                         describeStatusFrame(responseToken, statusCode)))
                    {
                        continue;
                    }

                    if (result == ExclusiveFrameReadResult::Status &&
                        isRetryableCalibrationStatus(statusCode))
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                        continue;
                    }

                    break;
                }

                if (!chunkReceived)
                {
                    if (result == ExclusiveFrameReadResult::Status)
                    {
                        lastCommandError_ = "calibration read failed at chunk " + std::to_string(packetIndex) +
                                            ": " + describeStatusFrame(responseToken, statusCode);
                    }
                    else
                    {
                        lastCommandError_ = "timed out waiting for calibration chunk " + std::to_string(packetIndex);
                    }
                    return false;
                }
            }
        }

        for (size_t packetIndex = 0; packetIndex < received.size(); ++packetIndex)
        {
            if (!received[packetIndex])
            {
                if (statusCode != 0)
                {
                    lastCommandError_ = "calibration read failed at chunk " + std::to_string(packetIndex) +
                                        ": " + describeStatusFrame(responseToken, statusCode);
                    return false;
                }
                lastCommandError_ = "timed out waiting for calibration chunk " + std::to_string(packetIndex);
                return false;
            }
        }

        std::memcpy(calibrationData, raw.data(), sizeof(*calibrationData));
        if (std::memcmp(calibrationData->header.magic, "UCAL", 4) != 0)
        {
            lastCommandError_ = "calibration read returned invalid header magic";
            return false;
        }
        if (calibrationData->header.headerSize != sizeof(gripper_hmi::GripperCalibrationHeader))
        {
            lastCommandError_ = "calibration read returned invalid header size";
            return false;
        }
        if (calibrationData->header.payloadSize == 0 ||
            calibrationData->header.payloadSize > gripper_hmi::kCalibrationPayloadSize)
        {
            lastCommandError_ = "calibration read returned invalid payload size";
            return false;
        }
        return true;
    });
}

bool GripperHmiDriver::writeCalibrationData(const gripper_hmi::GripperCalibrationDataV1 &calibrationData)
{
    gripper_hmi::GripperCalibrationDataV1 normalized = calibrationData;
    std::memcpy(normalized.header.magic, "UCAL", 4);
    normalized.header.headerSize = static_cast<uint16_t>(sizeof(gripper_hmi::GripperCalibrationHeader));
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&normalized);
    normalized.header.payloadSize = static_cast<uint16_t>(
        trimTrailingZeroLength(raw, gripper_hmi::kCalibrationPayloadSize));

    return runExclusiveCommand([this, &normalized]() {
        sp_flush(serialPort_, SP_BUF_INPUT);

        auto waitForAnyCalibrationStatus = [this](uint8_t *responseToken,
                                                  uint8_t *statusCode,
                                                  int timeoutMs) -> bool {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0)
                {
                    break;
                }

                uint8_t token = 0;
                uint8_t parsedStatusCode = 0;
                if (!readAnyStatusFrameLocked(&token, &parsedStatusCode, static_cast<int>(remaining.count())))
                {
                    break;
                }

                if (responseToken != nullptr)
                {
                    *responseToken = token;
                }
                if (statusCode != nullptr)
                {
                    *statusCode = parsedStatusCode;
                }
                return true;
            }
            return false;
        };

        auto waitForCalibrationChunkStatus = [this](size_t packetIndex,
                                                    uint8_t *responseToken,
                                                    uint8_t *statusCode,
                                                    int timeoutMs) -> bool {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0)
                {
                    break;
                }

                uint8_t token = 0;
                uint8_t parsedStatusCode = 0;
                if (!readAnyStatusFrameLocked(&token, &parsedStatusCode, static_cast<int>(remaining.count())))
                {
                    break;
                }

                if (!isCalibrationWriteAckToken(packetIndex, token))
                {
                    continue;
                }

                if (responseToken != nullptr)
                {
                    *responseToken = token;
                }
                if (statusCode != nullptr)
                {
                    *statusCode = parsedStatusCode;
                }
                return true;
            }
            return false;
        };

        bool beginReady = false;
        bool sawBeginStatus = false;
        uint8_t lastBeginToken = 0;
        uint8_t lastBeginStatusCode = 0;
        int beginAbortRecoveries = 0;
        for (int attempt = 0; attempt < kCalibrationBeginRetryLimit; ++attempt)
        {
            const auto beginCommand = GripperHmiProtocol::buildBeginCalibrationWriteCommand();
            if (!writeFrameLocked(beginCommand.data(), beginCommand.size()))
            {
                lastCommandError_ = "failed to send begin calibration write command";
                return false;
            }

            uint8_t responseToken = 0;
            uint8_t statusCode = 0;
            if (!waitForAnyCalibrationStatus(&responseToken,
                                             &statusCode,
                                             kExclusiveCommandTimeoutMs))
            {
                continue;
            }
            sawBeginStatus = true;
            lastBeginToken = responseToken;
            lastBeginStatusCode = statusCode;

            if (statusCode == GripperHmiProtocol::kStatusOk)
            {
                beginReady = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationBeginSettleMs));
                break;
            }

            if (isCalibrationAbortRecoveryStatus(statusCode) &&
                beginAbortRecoveries < kCalibrationAbortRetryLimit &&
                abortCalibrationWriteStateLocked("begin write recovery after " +
                                                 describeStatusFrame(responseToken, statusCode)))
            {
                ++beginAbortRecoveries;
                continue;
            }

            if (isRetryableCalibrationStatus(statusCode))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationBeginSettleMs));
                continue;
            }

            lastCommandError_ = "begin calibration write failed: " + describeStatusFrame(responseToken, statusCode);
            return false;
        }

        if (!beginReady)
        {
            lastCommandError_ = "timed out waiting for begin calibration write acknowledgement" +
                                (sawBeginStatus ? (": " + describeStatusFrame(lastBeginToken, lastBeginStatusCode))
                                                : std::string());
            return false;
        }

        const uint8_t *raw = reinterpret_cast<const uint8_t *>(&normalized);
        const size_t chunkCount = gripper_hmi::kCalibrationPayloadSize / GripperHmiProtocol::kCalibrationChunkSize;

        for (size_t packetIndex = 0; packetIndex < chunkCount; ++packetIndex)
        {
            const size_t chunkOffset = packetIndex * GripperHmiProtocol::kCalibrationChunkSize;
            const auto frame = GripperHmiProtocol::buildCalibrationChunkFrame(
                static_cast<uint8_t>(packetIndex),
                raw + chunkOffset,
                GripperHmiProtocol::kCalibrationChunkSize);
            bool chunkSent = false;
            bool sawStatus = false;
            uint8_t lastResponseToken = 0;
            uint8_t lastStatusCode = 0;
            int chunkAbortRecoveries = 0;
            for (int attempt = 0; attempt < kCalibrationChunkRetryLimit; ++attempt)
            {
                if (!writeFrameLocked(frame.data(), frame.size()))
                {
                    lastCommandError_ = "failed to send calibration chunk " + std::to_string(packetIndex);
                    return false;
                }

                uint8_t responseToken = 0;
                uint8_t statusCode = 0;
                if (!waitForCalibrationChunkStatus(packetIndex,
                                                   &responseToken,
                                                   &statusCode,
                                                   kExclusiveCommandTimeoutMs))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                    continue;
                }
                sawStatus = true;
                lastResponseToken = responseToken;
                lastStatusCode = statusCode;

                if (statusCode == GripperHmiProtocol::kStatusOk)
                {
                    chunkSent = true;
                    break;
                }

                if (isCalibrationAbortRecoveryStatus(statusCode))
                {
                    const bool shouldAbortAndRecover =
                        statusCode == GripperHmiProtocol::kStatusMissingData ||
                        (statusCode == GripperHmiProtocol::kStatusZeroDataChecksumError && attempt > 0);
                    if (shouldAbortAndRecover &&
                        chunkAbortRecoveries < kCalibrationAbortRetryLimit &&
                        abortCalibrationWriteStateLocked("chunk write recovery packet=" +
                                                         std::to_string(packetIndex) + " after " +
                                                         describeStatusFrame(responseToken, statusCode)))
                    {
                        ++chunkAbortRecoveries;
                        continue;
                    }
                }

                if (isRetryableCalibrationStatus(statusCode))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                    continue;
                }

                lastCommandError_ = "calibration chunk " + std::to_string(packetIndex) +
                                    " failed: " + describeStatusFrame(responseToken, statusCode);
                return false;
            }

            if (!chunkSent)
            {
                lastCommandError_ = "calibration chunk " + std::to_string(packetIndex) +
                                    " failed after retry" +
                                    (sawStatus ? (": " + describeStatusFrame(lastResponseToken, lastStatusCode))
                                               : std::string());
                return false;
            }
        }

        const auto endCommand = GripperHmiProtocol::buildEndCalibrationWriteCommand();
        if (!writeFrameLocked(endCommand.data(), endCommand.size()))
        {
            lastCommandError_ = "failed to send end calibration write command";
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationCommitDelayMs));
        std::vector<uint8_t> endDrain;
        if (!readBytesLocked(&endDrain, 80))
        {
            lastCommandError_ = "failed while draining end calibration write response";
            return false;
        }

        return true;
    });
}

void GripperHmiDriver::handleParsedFrame(const GripperParsedFrame &frame)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastReceiveTimeMs_ = currentSteadyMs();
    ++stateGeneration_;

    if (frame.type == GripperFrameType::KeyReport)
    {
        lastKeyReport_ = frame.keyReport;
        if (frame.keyReport.keyIndex >= 0 && frame.keyReport.keyIndex < static_cast<int>(keyPressed_.size()))
        {
            keyPressed_[static_cast<size_t>(frame.keyReport.keyIndex)] = frame.keyReport.pressed;
        }
        stateCv_.notify_all();
        return;
    }

    if (frame.type == GripperFrameType::BeepState)
    {
        beepState_ = frame.beepState;
    }

    stateCv_.notify_all();
}

bool GripperHmiDriver::readAndProcessAvailableLocked()
{
    if (serialPort_ == nullptr)
    {
        return false;
    }

    bool parsedAnyFrame = false;
    uint8_t buffer[256];

    while (true)
    {
        const int bytesRead = sp_nonblocking_read(serialPort_, buffer, sizeof(buffer));
        if (bytesRead < 0)
        {
            handleIoFailureLocked("read");
            break;
        }
        if (bytesRead == 0)
        {
            break;
        }

        rxBuffer_.insert(rxBuffer_.end(), buffer, buffer + bytesRead);

        while (true)
        {
            const auto frame = GripperHmiProtocol::tryConsumeFrame(rxBuffer_);
            if (!frame.has_value())
            {
                break;
            }
            parsedAnyFrame = true;
            handleParsedFrame(frame.value());
        }
    }

    return parsedAnyFrame;
}

void GripperHmiDriver::ioLoop()
{
    std::unique_lock<std::mutex> ioLock(ioMutex_);
    while (ioRunning_ && serialPort_ != nullptr)
    {
        readAndProcessAvailableLocked();

        const uint64_t nowMs = currentSteadyMs();
        const uint64_t epochMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        const bool stateDue = pendingStateRequest_ ||
                              (nowMs - lastStateRequestAtMs_) >= kStateRequestIntervalMs;
        const bool renderDue = !ledEffectEnabled_ ||
                               lastLedRenderAtMs_ == 0 ||
                               (nowMs - lastLedRenderAtMs_) >= kLedRenderIntervalMs;

        GripperLedColor desiredColor = pendingLedColor_;
        if (ledEffectEnabled_)
        {
            if (renderDue || ledEffectDirty_)
            {
                desiredColor = ledRenderer_.render(ledEffect_, nowMs, epochMs);
                lastLedRenderAtMs_ = nowMs;
            }
            else
            {
                desiredColor = GripperLedColor{
                    lastRenderedColor_[0],
                    lastRenderedColor_[1],
                    lastRenderedColor_[2],
                };
            }
        }

        const std::array<uint8_t, 3> desiredColorArray{desiredColor.red, desiredColor.green, desiredColor.blue};
        const bool colorChanged = desiredColorArray != lastRenderedColor_;
        const bool ledResendDue = lastLedWriteAtMs_ == 0 ||
                                  (nowMs - lastLedWriteAtMs_) >= kLedResendIntervalMs;
        const bool ledPrioritySend = pendingLedUpdate_ || ledEffectDirty_ || colorChanged;
        const bool ledShouldSend = ledPrioritySend || ledResendDue;
        const bool beepKeepaliveEnabled = activeBeepState_.duty > 0;
        const bool beepResendDue = beepKeepaliveEnabled &&
                                   (lastBeepWriteAtMs_ == 0 ||
                                    (nowMs - lastBeepWriteAtMs_) >= kBeepResendIntervalMs);

        // Mirror the old driver_origin split: preserve LED edges first, then use
        // low-frequency keepalive traffic so serial writes do not mask blink transitions.
        if (ledPrioritySend)
        {
            const auto frame = GripperHmiProtocol::buildSetRgbCommand(desiredColor);
            if (writeFrameLocked(frame.data(), frame.size()))
            {
                lastLedWriteAtMs_ = nowMs;
                lastRenderedColor_ = desiredColorArray;
            }
            pendingLedUpdate_ = false;
            ledEffectDirty_ = false;
        }

        bool sentBeepUpdate = false;
        if (pendingBeepUpdate_ || beepResendDue)
        {
            const auto frame = GripperHmiProtocol::buildSetBeepCommand(activeBeepState_);
            if (writeFrameLocked(frame.data(), frame.size()))
            {
                lastBeepWriteAtMs_ = nowMs;
                if (pendingBeepUpdate_)
                {
                    pendingBeepUpdate_ = false;
                    pendingStateRequest_ = true;
                    sentBeepUpdate = true;
                }
            }
        }

        if (stateDue && !pendingBeepUpdate_ && !sentBeepUpdate)
        {
            const auto frame = GripperHmiProtocol::buildBeepStateRequest();
            pendingStateRequest_ = false;
            if (writeFrameLocked(frame.data(), frame.size()))
            {
                lastStateRequestAtMs_ = nowMs;
            }
        }

        if (!ledPrioritySend && ledShouldSend)
        {
            const auto frame = GripperHmiProtocol::buildSetRgbCommand(desiredColor);
            if (writeFrameLocked(frame.data(), frame.size()))
            {
                lastLedWriteAtMs_ = nowMs;
                lastRenderedColor_ = desiredColorArray;
            }
            pendingLedUpdate_ = false;
            ledEffectDirty_ = false;
        }

        uint64_t waitMs = kStateRequestIntervalMs;
        if (ledEffectEnabled_)
        {
            const uint64_t renderWaitMs =
                (lastLedRenderAtMs_ == 0 || (nowMs - lastLedRenderAtMs_) >= kLedRenderIntervalMs)
                    ? 1
                    : (kLedRenderIntervalMs - (nowMs - lastLedRenderAtMs_));
            waitMs = std::min(waitMs, renderWaitMs);
        }
        if (lastStateRequestAtMs_ == 0 || (nowMs - lastStateRequestAtMs_) >= kStateRequestIntervalMs)
        {
            waitMs = 1;
        }
        else
        {
            waitMs = std::min(waitMs, kStateRequestIntervalMs - (nowMs - lastStateRequestAtMs_));
        }
        if (beepKeepaliveEnabled)
        {
            const uint64_t beepWaitMs =
                (lastBeepWriteAtMs_ == 0 || (nowMs - lastBeepWriteAtMs_) >= kBeepResendIntervalMs)
                    ? 1
                    : (kBeepResendIntervalMs - (nowMs - lastBeepWriteAtMs_));
            waitMs = std::min(waitMs, beepWaitMs);
        }
        waitMs = std::max<uint64_t>(1, waitMs);

        ioCv_.wait_for(ioLock, std::chrono::milliseconds(waitMs), [this]() {
            return !ioRunning_ || pendingStateRequest_ || pendingLedUpdate_ || pendingBeepUpdate_;
        });
    }
}

bool GripperHmiDriver::pollOnce(int timeoutMs)
{
    std::unique_lock<std::mutex> stateLock(stateMutex_);
    const uint64_t beforeGeneration = stateGeneration_;
    if (timeoutMs <= 0)
    {
        return false;
    }

    return stateCv_.wait_for(stateLock, std::chrono::milliseconds(timeoutMs), [this, beforeGeneration]() {
        return stateGeneration_ != beforeGeneration;
    });
}

bool GripperHmiDriver::waitForKeyChange(int timeoutMs, GripperKeyReport *report)
{
    std::unique_lock<std::mutex> stateLock(stateMutex_);
    const std::optional<GripperKeyReport> before = lastKeyReport_;
    const auto changed = [this, &before]() {
        return lastKeyReport_.has_value() && (!before.has_value() ||
            lastKeyReport_->rawCode != before->rawCode ||
            lastKeyReport_->pressed != before->pressed ||
            lastKeyReport_->keyIndex != before->keyIndex);
    };

    if (!stateCv_.wait_for(stateLock, std::chrono::milliseconds(timeoutMs), changed))
    {
        return false;
    }

    if (report != nullptr)
    {
        *report = *lastKeyReport_;
    }
    return true;
}

bool GripperHmiDriver::isKeyPressed(size_t keyIndex) const
{
    if (keyIndex >= keyPressed_.size())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    return keyPressed_[keyIndex];
}

bool GripperHmiDriver::isActive(uint64_t timeoutMs) const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (lastReceiveTimeMs_ == 0)
    {
        return false;
    }
    return (currentSteadyMs() - lastReceiveTimeMs_) <= timeoutMs;
}

GripperHmiSnapshot GripperHmiDriver::getSnapshot(uint64_t activeTimeoutMs) const
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    GripperHmiSnapshot snapshot;
    snapshot.keyPressed = keyPressed_;
    snapshot.beepState = beepState_;
    snapshot.lastKeyReport = lastKeyReport_;
    if (lastReceiveTimeMs_ == 0)
    {
        snapshot.active = false;
        snapshot.lastRxAgeMs = 0;
        return snapshot;
    }

    snapshot.lastRxAgeMs = currentSteadyMs() - lastReceiveTimeMs_;
    snapshot.active = snapshot.lastRxAgeMs <= activeTimeoutMs;
    return snapshot;
}
