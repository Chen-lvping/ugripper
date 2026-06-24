#include "gripper_hmi_driver.h"
#include "gripper_hmi_driver_logic.h"
#include "utils/logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
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
constexpr int kCalibrationWriteRetryLimit = 3;
constexpr int kCalibrationReadRetryLimit = 5;
constexpr int kCalibrationRangeReadTimeoutMs = 6500;
constexpr int kCalibrationMissingChunkSweepLimit = 3;
constexpr int kCalibrationBeginSettleMs = 50;
constexpr int kCalibrationAbortDrainMs = 220;
constexpr int kCalibrationAbortSettleMs = 220;
constexpr int kCalibrationAbortRetryLimit = 2;
constexpr int kSerialNumberCommandRetryLimit = 3;
constexpr int kExclusiveCommandDrainIdleMs = 40;
constexpr int kExclusiveCommandDrainMaxMs = 120;
struct CommandDiagStats
{
    uint64_t sends = 0;
    uint64_t acks = 0;
    uint64_t timeouts = 0;
    uint64_t retries = 0;
    uint64_t recoveries = 0;
};

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

std::string sanitizeDiagValue(std::string value)
{
    for (char &ch : value)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            ch = '_';
        }
    }
    return value;
}

void logCommandSummary(const std::string &name,
                       const std::string &port,
                       uint64_t commandId,
                       const char *command,
                       const CommandDiagStats &stats,
                       bool success,
                       const std::string &reason)
{
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[GRIPPER_DIAG] category=command_summary"
        << " port=" << sanitizeDiagValue(port)
        << " name=" << sanitizeDiagValue(name)
        << " command=" << sanitizeDiagValue(command != nullptr ? command : "unknown")
        << " command_id=" << commandId
        << " sends=" << stats.sends
        << " acks=" << stats.acks
        << " timeouts=" << stats.timeouts
        << " retries=" << stats.retries
        << " recoveries=" << stats.recoveries
        << " status=" << (success ? "success" : "failure")
        << " reason=" << sanitizeDiagValue(reason.empty() ? "none" : reason)).str());
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

size_t requiredCalibrationChunkCountFromHeader(const gripper_hmi::GripperCalibrationHeader &header)
{
    const size_t requiredBytes = std::max<size_t>(header.payloadSize, header.headerSize);
    const size_t clampedBytes = std::min(requiredBytes, gripper_hmi::kCalibrationPayloadSize);
    return std::max<size_t>(
        gripper_hmi::driver_logic::kCalibrationHeaderChunkCount,
        (clampedBytes + GripperHmiProtocol::kCalibrationChunkSize - 1) / GripperHmiProtocol::kCalibrationChunkSize);
}

bool tryParseCalibrationHeader(const std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> &raw,
                               const std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> &received,
                               gripper_hmi::GripperCalibrationHeader *header,
                               size_t *requiredChunkCount)
{
    if (header == nullptr || requiredChunkCount == nullptr)
    {
        return false;
    }
    for (size_t packetIndex = 0; packetIndex < gripper_hmi::driver_logic::kCalibrationHeaderChunkCount;
         ++packetIndex)
    {
        if (!received[packetIndex])
        {
            return false;
        }
    }

    gripper_hmi::GripperCalibrationHeader parsed{};
    std::memcpy(&parsed, raw.data(), sizeof(parsed));
    if (std::memcmp(parsed.magic, "UCAL", 4) != 0)
    {
        return false;
    }
    if (parsed.headerSize != sizeof(gripper_hmi::GripperCalibrationHeader))
    {
        return false;
    }
    if (parsed.payloadSize == 0 || parsed.payloadSize > gripper_hmi::kCalibrationPayloadSize)
    {
        return false;
    }

    *header = parsed;
    *requiredChunkCount = requiredCalibrationChunkCountFromHeader(parsed);
    return true;
}

bool hasAllCalibrationChunks(const std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> &received,
                             size_t requiredChunkCount)
{
    const size_t verifyCount = std::min(requiredChunkCount, received.size());
    return std::all_of(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(verifyCount),
                       [](bool value) { return value; });
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

uint64_t GripperHmiDriver::allocateCommandIdLocked()
{
    return nextCommandId_++;
}

void GripperHmiDriver::logIoSummaryLocked(const char *reason, uint64_t nowMs)
{
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[GRIPPER_DIAG] category=io_summary"
        << " port=" << sanitizeDiagValue(port_)
        << " name=" << sanitizeDiagValue(name_)
        << " reason=" << sanitizeDiagValue(reason != nullptr ? reason : "periodic")
        << " tx_led=" << txLedCount_.load()
        << " tx_beep=" << txBeepCount_.load()
        << " tx_state_req=" << txStateRequestCount_.load()
        << " rx_frames=" << rxFrameCount_.load()
        << " rx_key_reports=" << rxKeyReportCount_.load()
        << " rx_beep_states=" << rxBeepStateCount_.load()
        << " io_failures=" << ioFailureCount_.load()
        << " connect_success=" << connectSuccessCount_.load()
        << " reconnect_success=" << reconnectSuccessCount_.load()
        << " exclusive_commands=" << exclusiveCommandCount_.load()
        << " exclusive_acks=" << exclusiveAckCount_.load()
        << " exclusive_timeouts=" << exclusiveTimeoutCount_.load()
        << " exclusive_retries=" << exclusiveRetryCount_.load()
        << " exclusive_failures=" << exclusiveFailureCount_.load()
        << " exclusive_recoveries=" << exclusiveRecoveryCount_.load()).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << name_ << ": " << message
                             << " (suppressed " << suppressedConnectFailureCount_
                             << " repeated attempts over " << (suppressedWindowMs / 1000) << "s)").str());
    }
    else
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << name_ << ": " << message).str());
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

    DM_LOG_INFO("{}", (::DA::utils::LogString() << name_ << ": reconnected port " << port_
                         << " (resolved=" << resolvedPort << ") after " << totalAttempts
                         << " failed attempt" << (totalAttempts == 1 ? "" : "s")
                         << " over " << (outageMs / 1000) << "s").str());

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

#ifdef TIOCEXCL
    int portHandle = -1;
    if (sp_get_port_handle(serialPort_, &portHandle) != SP_OK || portHandle < 0 || ioctl(portHandle, TIOCEXCL) != 0)
    {
        logConnectFailureLocked("cannot exclusively lock port " + port_ + " (resolved=" + resolvedPort + ")");
        sp_close(serialPort_);
        sp_free_port(serialPort_);
        serialPort_ = nullptr;
        return false;
    }
#endif

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
    ledEffectStartedAtMs_ = 0;
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

    ++connectSuccessCount_;
    if (!lastConnectFailureMessage_.empty())
    {
        ++reconnectSuccessCount_;
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

    logIoSummaryLocked("disconnect", currentSteadyMs());

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
    ++ioFailureCount_;
    logIoSummaryLocked(operation != nullptr ? operation : "io_failure", currentSteadyMs());
    DM_LOG_WARN("{}", (::DA::utils::LogString() << name_ << ": " << operation << " failed on " << port_).str());
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

    const bool sameState = ledEffectEnabled_ && ledEffect_.state == effect.state;
    ledEffect_ = effect;
    ledEffectEnabled_ = true;
    if (!sameState || ledEffectStartedAtMs_ == 0)
    {
        ledEffectStartedAtMs_ = currentSteadyMs();
    }
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
    const int64_t drainStartMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    int64_t drainDeadlineMs = drainStartMs + kExclusiveCommandDrainIdleMs;
    const int64_t drainMaxDeadlineMs = drainStartMs + kExclusiveCommandDrainMaxMs;
    uint8_t temp[256];
    while (true)
    {
        const int64_t nowMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(
                nowMs, drainDeadlineMs, drainMaxDeadlineMs, 0, kExclusiveCommandDrainIdleMs)
                .action == gripper_hmi::driver_logic::ExclusiveDrainAction::StopDrained)
        {
            break;
        }

        const int bytesRead = sp_nonblocking_read(serialPort_, temp, sizeof(temp));
        const auto drainStep = gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(
            nowMs, drainDeadlineMs, drainMaxDeadlineMs, bytesRead, kExclusiveCommandDrainIdleMs);
        if (drainStep.action == gripper_hmi::driver_logic::ExclusiveDrainAction::StopIoFailure)
        {
            handleIoFailureLocked("read");
            return false;
        }
        if (drainStep.action == gripper_hmi::driver_logic::ExclusiveDrainAction::ContinueAfterBytes)
        {
            drainDeadlineMs = drainStep.next_idle_deadline_ms;
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
    while (true)
    {
        const bool deadlineReached = std::chrono::steady_clock::now() >= deadline;
        int bytesRead = 0;
        if (!deadlineReached)
        {
            bytesRead = sp_nonblocking_read(serialPort_, temp, sizeof(temp));
        }

        const auto action = gripper_hmi::driver_logic::EvaluateReadBytesStep(deadlineReached, bytesRead);
        if (action == gripper_hmi::driver_logic::ReadBytesAction::ReturnIoFailure)
        {
            handleIoFailureLocked("read");
            return false;
        }
        if (action == gripper_hmi::driver_logic::ReadBytesAction::ReturnSuccessWithData)
        {
            buffer->insert(buffer->end(), temp, temp + bytesRead);
            return true;
        }
        if (action == gripper_hmi::driver_logic::ReadBytesAction::ReturnSuccessNoData)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool GripperHmiDriver::readSizedFrameLocked(size_t frameSize, std::vector<uint8_t> *frame, int timeoutMs)
{
    if (frame == nullptr || serialPort_ == nullptr || frameSize < 4)
    {
        return false;
    }

    std::vector<uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

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

        while (buffer.size() >= 2)
        {
            const auto scanResult = gripper_hmi::driver_logic::ScanSizedRecvFrame(&buffer, frameSize, frame);
            if (scanResult == gripper_hmi::driver_logic::SizedFrameScanResult::MatchedFrame)
            {
                return true;
            }
            if (scanResult == gripper_hmi::driver_logic::SizedFrameScanResult::NeedMoreData)
            {
                break;
            }
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
    DM_LOG_WARN("{}", (::DA::utils::LogString() << name_ << ": abort calibration write state: " << reason).str());
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

        while (true)
        {
            const auto scanResult = gripper_hmi::driver_logic::ScanExpectedExclusiveStatusFrame(
                &buffer, expectedToken, responseToken, statusCode);
            if (scanResult == gripper_hmi::driver_logic::StatusFrameScanResult::MatchedStatus)
            {
                return true;
            }
            if (scanResult == gripper_hmi::driver_logic::StatusFrameScanResult::NeedMoreData)
            {
                break;
            }
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
            const auto scanResult = gripper_hmi::driver_logic::ScanExpectedCalibrationFrame(
                &buffer, expectedToken, payload);
            if (scanResult == gripper_hmi::driver_logic::CalibrationFrameScanResult::MatchedFrame)
            {
                return true;
            }
            if (scanResult == gripper_hmi::driver_logic::CalibrationFrameScanResult::NeedMoreData)
            {
                break;
            }
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

        while (true)
        {
            const auto scanResult = gripper_hmi::driver_logic::ScanRawDataOrStatusFrame(
                &buffer, dataLength, payload, token, statusCode);
            if (scanResult == gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedData)
            {
                return ExclusiveFrameReadResult::Data;
            }
            if (scanResult == gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedStatus)
            {
                return ExclusiveFrameReadResult::Status;
            }
            if (scanResult == gripper_hmi::driver_logic::ExclusiveFrameScanResult::NeedMoreData)
            {
                break;
            }
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

        while (true)
        {
            const auto scanResult = gripper_hmi::driver_logic::ScanCalibrationDataOrStatusFrame(
                &buffer, expectedToken, dataLength, payload, token, statusCode);
            if (scanResult == gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedData)
            {
                return ExclusiveFrameReadResult::Data;
            }
            if (scanResult == gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedStatus)
            {
                return ExclusiveFrameReadResult::Status;
            }
            if (scanResult == gripper_hmi::driver_logic::ExclusiveFrameScanResult::NeedMoreData)
            {
                break;
            }
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
    return runExclusiveCommand([this, serialNumber]() { return readSerialNumberLocked(serialNumber); });
}

bool GripperHmiDriver::readSerialNumberAndCalibration(std::string *serialNumber,
                                                      bool *hasSerialNumber,
                                                      gripper_hmi::GripperCalibrationDataV1 *calibrationData)
{
    if (serialNumber == nullptr || hasSerialNumber == nullptr || calibrationData == nullptr)
    {
        return false;
    }

    return runExclusiveCommand([this, serialNumber, hasSerialNumber, calibrationData]() {
        std::string value;
        if (readSerialNumberLocked(&value))
        {
            *serialNumber = value;
            *hasSerialNumber = true;
        }
        else
        {
            serialNumber->clear();
            *hasSerialNumber = false;
        }
        return readCalibrationDataLocked(calibrationData);
    });
}

bool GripperHmiDriver::writeSerialNumber(const std::string &serialNumber)
{
    gripper_hmi::GripperSerialNumber encoded{};
    if (!gripper_hmi::encodeSerialNumber(serialNumber, &encoded))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << name_ << ": invalid serial number, expected 1.."
                              << gripper_hmi::kSerialNumberFieldLength
                              << " printable ASCII characters (32-byte field, zero-padded)").str());
        return false;
    }

    uint64_t commandId = 0;
    {
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        commandId = allocateCommandIdLocked();
    }
    ++exclusiveCommandCount_;

    return runExclusiveCommand([this, &encoded, commandId]() {
        CommandDiagStats stats;
        auto finish = [this, commandId, &stats](bool success, const std::string &reason) {
            exclusiveAckCount_.fetch_add(stats.acks);
            exclusiveTimeoutCount_.fetch_add(stats.timeouts);
            exclusiveRetryCount_.fetch_add(stats.retries);
            exclusiveRecoveryCount_.fetch_add(stats.recoveries);
            if (!success)
            {
                ++exclusiveFailureCount_;
            }
            logCommandSummary(name_, port_, commandId, "write_serial_number", stats, success, reason);
            return success;
        };

        auto writeChunkWithRetry = [this, &stats](size_t chunkIndex, const uint8_t *chunkData) -> bool {
            const auto frame = GripperHmiProtocol::buildWriteSerialNumberFrame(
                chunkData,
                GripperHmiProtocol::kSerialNumberChunkSize);
            bool chunkSent = false;
            bool sawStatus = false;
            uint8_t lastResponseToken = 0;
            uint8_t lastStatusCode = 0;
            int chunkAbortRecoveries = 0;

            for (int attempt = 0; attempt < kCalibrationChunkRetryLimit; ++attempt)
            {
                ++stats.sends;
                if (!writeFrameLocked(frame.data(), frame.size()))
                {
                    lastCommandError_ = "failed to send serial number payload chunk " + std::to_string(chunkIndex);
                    return false;
                }

                uint8_t responseToken = 0;
                uint8_t statusCode = 0;
                if (!readAnyStatusFrameLocked(&responseToken, &statusCode, kExclusiveCommandTimeoutMs))
                {
                    ++stats.timeouts;
                    if (attempt + 1 < kCalibrationChunkRetryLimit)
                    {
                        ++stats.retries;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                    continue;
                }

                sawStatus = true;
                lastResponseToken = responseToken;
                lastStatusCode = statusCode;

                if (statusCode == GripperHmiProtocol::kStatusOk)
                {
                    ++stats.acks;
                    chunkSent = true;
                    break;
                }

                if (gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(statusCode) &&
                    chunkAbortRecoveries < kCalibrationAbortRetryLimit &&
                    abortCalibrationWriteStateLocked("serial write recovery chunk=" +
                                                     std::to_string(chunkIndex) + " after " +
                                                     describeStatusFrame(responseToken, statusCode)))
                {
                    ++chunkAbortRecoveries;
                    ++stats.retries;
                    ++stats.recoveries;
                    continue;
                }

                if (gripper_hmi::driver_logic::IsRetryableCalibrationStatus(statusCode))
                {
                    if (attempt + 1 < kCalibrationChunkRetryLimit)
                    {
                        ++stats.retries;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                    continue;
                }

                lastCommandError_ = "serial number write chunk " + std::to_string(chunkIndex) +
                                    " failed: " + describeStatusFrame(responseToken, statusCode);
                return false;
            }

            if (!chunkSent)
            {
                lastCommandError_ = "serial number write chunk " + std::to_string(chunkIndex) +
                                    " failed after retry" +
                                    (sawStatus ? (": " + describeStatusFrame(lastResponseToken, lastStatusCode))
                                               : std::string());
                return false;
            }

            return true;
        };

        bool preambleReady = false;
        bool sawPreambleStatus = false;
        uint8_t lastPreambleToken = 0;
        uint8_t lastPreambleStatusCode = 0;
        int preambleAbortRecoveries = 0;

        for (int attempt = 0; attempt < kCalibrationBeginRetryLimit; ++attempt)
        {
            sp_flush(serialPort_, SP_BUF_INPUT);
            const auto request = GripperHmiProtocol::buildWriteSerialNumberCommand();
            ++stats.sends;
            if (!writeFrameLocked(request.data(), request.size()))
            {
                lastCommandError_ = "failed to send write serial number command";
                return finish(false, lastCommandError_);
            }

            uint8_t responseToken = 0;
            uint8_t statusCode = 0;
            if (!readAnyStatusFrameLocked(&responseToken, &statusCode, kExclusiveCommandTimeoutMs))
            {
                ++stats.timeouts;
                if (attempt + 1 < kCalibrationBeginRetryLimit)
                {
                    ++stats.retries;
                }
                continue;
            }

            sawPreambleStatus = true;
            lastPreambleToken = responseToken;
            lastPreambleStatusCode = statusCode;

            if (statusCode == GripperHmiProtocol::kStatusOk ||
                (responseToken == GripperHmiProtocol::kIndexSerialNumber &&
                 statusCode == GripperHmiProtocol::kStatusMissingData))
            {
                ++stats.acks;
                preambleReady = true;
                break;
            }

            if (gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(statusCode) &&
                preambleAbortRecoveries < kCalibrationAbortRetryLimit &&
                abortCalibrationWriteStateLocked("serial write preamble recovery after " +
                                                 describeStatusFrame(responseToken, statusCode)))
            {
                ++preambleAbortRecoveries;
                ++stats.retries;
                ++stats.recoveries;
                continue;
            }

            if (gripper_hmi::driver_logic::IsRetryableCalibrationStatus(statusCode))
            {
                if (attempt + 1 < kCalibrationBeginRetryLimit)
                {
                    ++stats.retries;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationBeginSettleMs));
                continue;
            }

            lastCommandError_ = "serial number write preamble failed: " + describeStatusFrame(responseToken, statusCode);
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << name_ << ": " << lastCommandError_).str());
            return finish(false, lastCommandError_);
        }

        if (!preambleReady)
        {
            lastCommandError_ = "timed out waiting for serial number write preamble acknowledgement" +
                                (sawPreambleStatus ? (": " + describeStatusFrame(lastPreambleToken, lastPreambleStatusCode))
                                                   : std::string());
            return finish(false, lastCommandError_);
        }

        if (!writeChunkWithRetry(0, reinterpret_cast<const uint8_t *>(encoded.data())))
        {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << name_ << ": " << lastCommandError_).str());
            return finish(false, lastCommandError_);
        }

        if (!writeChunkWithRetry(1,
                                 reinterpret_cast<const uint8_t *>(encoded.data()) +
                                     GripperHmiProtocol::kSerialNumberChunkSize))
        {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << name_ << ": " << lastCommandError_).str());
            return finish(false, lastCommandError_);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return finish(true, "ok");
    });
}

bool GripperHmiDriver::readCalibrationData(gripper_hmi::GripperCalibrationDataV1 *calibrationData)
{
    if (calibrationData == nullptr)
    {
        return false;
    }
    return runExclusiveCommand([this, calibrationData]() { return readCalibrationDataLocked(calibrationData); });
}

bool GripperHmiDriver::readSerialNumberLocked(std::string *serialNumber)
{
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
            if (serialNumber->empty())
            {
                lastCommandError_ = "serial number read returned empty payload";
                return false;
            }
            const bool all_same = std::all_of(encoded.begin(), encoded.end(), [](char ch) {
                return static_cast<unsigned char>(ch) == 0xFF || ch == '\0';
            });
            if (all_same)
            {
                lastCommandError_ = "serial number read returned invalid payload";
                return false;
            }
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
                gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(statusCode) &&
                abortCalibrationWriteStateLocked("serial read recovery after " +
                                                 describeStatusFrame(responseToken, statusCode)))
            {
                continue;
            }

            lastCommandError_ = "serial number read failed: " + describeStatusFrame(responseToken, statusCode);
            return false;
        }

        if (!readStatusFrameLocked(GripperHmiProtocol::kIndexSerialNumber, &statusCode, kExclusiveCommandTimeoutMs))
        {
            lastCommandError_ = "timed out waiting for serial number response";
            return false;
        }

        if (attempt + 1 < kSerialNumberCommandRetryLimit &&
            gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(statusCode) &&
            abortCalibrationWriteStateLocked("serial read status recovery after " +
                                             GripperHmiProtocol::describeStatusCode(statusCode)))
        {
            continue;
        }

        lastCommandError_ = "serial number read failed: " + GripperHmiProtocol::describeStatusCode(statusCode);
        return false;
    }

    return false;
}

bool GripperHmiDriver::readCalibrationDataLocked(gripper_hmi::GripperCalibrationDataV1 *calibrationData)
{
    if (calibrationData == nullptr)
    {
        return false;
    }
    sp_flush(serialPort_, SP_BUF_INPUT);

    std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> raw = {};
    std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> received = {};
    size_t requiredChunkCount = gripper_hmi::driver_logic::kCalibrationChunkCount;
    bool requiredChunkCountKnown = false;
    auto collectFrames = [this, &raw, &received, &requiredChunkCount, &requiredChunkCountKnown](
                             int timeoutMs, uint8_t *statusToken, uint8_t *statusCode) -> bool {
        if (serialPort_ == nullptr)
        {
            return false;
        }

        std::vector<uint8_t> buffer;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
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
                const auto scanResult = gripper_hmi::driver_logic::CollectCalibrationRangeFrame(
                    &buffer,
                    gripper_hmi::driver_logic::CalibrationRangeCollectState{
                        .raw = &raw,
                        .received = &received,
                        .required_chunk_count_known = &requiredChunkCountKnown,
                        .required_chunk_count = &requiredChunkCount,
                        .status_token = statusToken,
                        .status_code = statusCode,
                    });
                if (scanResult == gripper_hmi::driver_logic::CalibrationRangeCollectResult::CollectionComplete)
                {
                    return true;
                }
                if (scanResult == gripper_hmi::driver_logic::CalibrationRangeCollectResult::NeedMoreData)
                {
                    break;
                }
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
        requiredChunkCount = gripper_hmi::driver_logic::kCalibrationChunkCount;
        requiredChunkCountKnown = false;
        responseToken = 0;
        statusCode = 0;

        if (!requestRangeRead())
        {
            lastCommandError_ = "failed while collecting calibration read frames";
            return false;
        }

        if (requiredChunkCountKnown &&
            gripper_hmi::driver_logic::HasAllCalibrationChunks(received, requiredChunkCount))
        {
            rangeReadCollected = true;
            break;
        }

        const auto retryAction = gripper_hmi::driver_logic::DecideCalibrationRangeRetryAction(
            statusCode, attempt, kCalibrationReadRetryLimit);
        if (retryAction == gripper_hmi::driver_logic::CalibrationReadRetryAction::AbortRecoverRetry &&
            abortCalibrationWriteStateLocked("range read recovery after " +
                                             describeStatusFrame(responseToken, statusCode)))
        {
            continue;
        }

        if (retryAction == gripper_hmi::driver_logic::CalibrationReadRetryAction::StopFailure)
        {
            break;
        }

        sp_flush(serialPort_, SP_BUF_INPUT);
        std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
    }

    const bool needSequentialFallback =
        gripper_hmi::driver_logic::ShouldAttemptSequentialCalibrationFallback(
            rangeReadCollected, requiredChunkCountKnown, requiredChunkCount, received, statusCode);
    if (needSequentialFallback)
    {
        for (int sweep = 0; sweep < kCalibrationMissingChunkSweepLimit; ++sweep)
        {
            bool fetchedAnyMissingChunk = false;
            for (size_t packetIndex = 0; packetIndex < received.size(); ++packetIndex)
            {
                if (received[packetIndex])
                {
                    continue;
                }

                std::vector<uint8_t> payload;
                ExclusiveFrameReadResult result = ExclusiveFrameReadResult::Timeout;
                bool chunkReceived = false;

                for (int attempt = 0; attempt < kCalibrationChunkRetryLimit; ++attempt)
                {
                    result = requestSingleChunkRead(static_cast<uint8_t>(packetIndex),
                                                    &payload,
                                                    &responseToken,
                                                    &statusCode);
                    const auto readAction = gripper_hmi::driver_logic::DecideCalibrationChunkReadAction(
                        result == ExclusiveFrameReadResult::Data ? gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedData
                                                                 : (result == ExclusiveFrameReadResult::Status
                                                                        ? gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedStatus
                                                                        : gripper_hmi::driver_logic::ExclusiveFrameScanResult::NeedMoreData),
                        payload.size(),
                        GripperHmiProtocol::kCalibrationChunkSize,
                        statusCode,
                        attempt,
                        kCalibrationChunkRetryLimit);
                    if (readAction == gripper_hmi::driver_logic::CalibrationChunkReadAction::AcceptData)
                    {
                        std::memcpy(raw.data() + packetIndex * GripperHmiProtocol::kCalibrationChunkSize,
                                    payload.data(),
                                    GripperHmiProtocol::kCalibrationChunkSize);
                        received[packetIndex] = true;
                        chunkReceived = true;
                        fetchedAnyMissingChunk = true;
                        gripper_hmi::GripperCalibrationHeader header{};
                        size_t parsedRequiredChunkCount = gripper_hmi::driver_logic::kCalibrationChunkCount;
                        if (!requiredChunkCountKnown &&
                            gripper_hmi::driver_logic::TryParseCalibrationHeader(
                                raw, received, &header, &parsedRequiredChunkCount))
                        {
                            requiredChunkCount = parsedRequiredChunkCount;
                            requiredChunkCountKnown = true;
                        }
                        break;
                    }

                    if (readAction == gripper_hmi::driver_logic::CalibrationChunkReadAction::AbortRecoverRetry)
                    {
                        if (abortCalibrationWriteStateLocked("chunk read recovery packet=" +
                                                             std::to_string(packetIndex) + " after " +
                                                             describeStatusFrame(responseToken, statusCode)))
                        {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                            continue;
                        }
                        break;
                    }

                    if (readAction == gripper_hmi::driver_logic::CalibrationChunkReadAction::RetryAfterDelay)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                        continue;
                    }

                    break;
                }
                if (!chunkReceived)
                {
                    break;
                }
                if (requiredChunkCountKnown && hasAllCalibrationChunks(received, requiredChunkCount))
                {
                    break;
                }
            }

            if (requiredChunkCountKnown &&
                gripper_hmi::driver_logic::HasAllCalibrationChunks(received, requiredChunkCount))
            {
                break;
            }
            if (!fetchedAnyMissingChunk)
            {
                break;
            }
        }
    }

    const size_t verifyChunkCount = requiredChunkCountKnown ? requiredChunkCount : received.size();
    for (size_t packetIndex = 0; packetIndex < verifyChunkCount; ++packetIndex)
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
}

bool GripperHmiDriver::writeCalibrationData(const gripper_hmi::GripperCalibrationDataV1 &calibrationData)
{
    gripper_hmi::GripperCalibrationDataV1 normalized = calibrationData;
    std::memcpy(normalized.header.magic, "UCAL", 4);
    normalized.header.headerSize = static_cast<uint16_t>(sizeof(gripper_hmi::GripperCalibrationHeader));
    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&normalized);
    normalized.header.payloadSize = static_cast<uint16_t>(
        trimTrailingZeroLength(raw, gripper_hmi::kCalibrationPayloadSize));

    uint64_t commandId = 0;
    {
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        commandId = allocateCommandIdLocked();
    }
    ++exclusiveCommandCount_;

    return runExclusiveCommand([this, &normalized, commandId]() {
        CommandDiagStats stats;
        auto finish = [this, commandId, &stats](bool success, const std::string &reason) {
            exclusiveAckCount_.fetch_add(stats.acks);
            exclusiveTimeoutCount_.fetch_add(stats.timeouts);
            exclusiveRetryCount_.fetch_add(stats.retries);
            exclusiveRecoveryCount_.fetch_add(stats.recoveries);
            if (!success)
            {
                ++exclusiveFailureCount_;
            }
            logCommandSummary(name_, port_, commandId, "write_calibration", stats, success, reason);
            return success;
        };

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

                if (!gripper_hmi::driver_logic::IsCalibrationWriteAckToken(packetIndex, token))
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
            ++stats.sends;
            if (!writeFrameLocked(beginCommand.data(), beginCommand.size()))
            {
                lastCommandError_ = "failed to send begin calibration write command";
                return finish(false, lastCommandError_);
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
                ++stats.acks;
                std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationBeginSettleMs));
                break;
            }

            if (gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(statusCode) &&
                beginAbortRecoveries < kCalibrationAbortRetryLimit &&
                abortCalibrationWriteStateLocked("begin write recovery after " +
                                                 describeStatusFrame(responseToken, statusCode)))
            {
                ++beginAbortRecoveries;
                ++stats.retries;
                ++stats.recoveries;
                continue;
            }

            if (gripper_hmi::driver_logic::IsRetryableCalibrationStatus(statusCode))
            {
                ++stats.retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationBeginSettleMs));
                continue;
            }

            lastCommandError_ = "begin calibration write failed: " + describeStatusFrame(responseToken, statusCode);
            return finish(false, lastCommandError_);
        }

        if (!beginReady)
        {
            lastCommandError_ = "timed out waiting for begin calibration write acknowledgement" +
                                (sawBeginStatus ? (": " + describeStatusFrame(lastBeginToken, lastBeginStatusCode))
                                                : std::string());
            ++stats.timeouts;
            return finish(false, lastCommandError_);
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
                ++stats.sends;
                if (!writeFrameLocked(frame.data(), frame.size()))
                {
                    lastCommandError_ = "failed to send calibration chunk " + std::to_string(packetIndex);
                    return finish(false, lastCommandError_);
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
                    ++stats.acks;
                    break;
                }

                const auto chunkAction =
                    gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(statusCode, attempt);
                if (chunkAction == gripper_hmi::driver_logic::CalibrationWriteChunkStatusAction::Ack)
                {
                    chunkSent = true;
                    ++stats.acks;
                    break;
                }

                if (chunkAction == gripper_hmi::driver_logic::CalibrationWriteChunkStatusAction::AbortAndRecover)
                {
                    if (
                        chunkAbortRecoveries < kCalibrationAbortRetryLimit &&
                        abortCalibrationWriteStateLocked("chunk write recovery packet=" +
                                                         std::to_string(packetIndex) + " after " +
                                                         describeStatusFrame(responseToken, statusCode)))
                    {
                        ++chunkAbortRecoveries;
                        ++stats.retries;
                        ++stats.recoveries;
                        continue;
                    }
                }

                if (chunkAction == gripper_hmi::driver_logic::CalibrationWriteChunkStatusAction::Retry)
                {
                    ++stats.retries;
                    std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationChunkSendIntervalMs));
                    continue;
                }

                lastCommandError_ = "calibration chunk " + std::to_string(packetIndex) +
                                    " failed: " + describeStatusFrame(responseToken, statusCode);
                return finish(false, lastCommandError_);
            }

            if (!chunkSent)
            {
                lastCommandError_ = "calibration chunk " + std::to_string(packetIndex) +
                                    " failed after retry" +
                                    (sawStatus ? (": " + describeStatusFrame(lastResponseToken, lastStatusCode))
                                               : std::string());
                if (!sawStatus)
                {
                    ++stats.timeouts;
                }
                return finish(false, lastCommandError_);
            }
        }

        const auto endCommand = GripperHmiProtocol::buildEndCalibrationWriteCommand();
        ++stats.sends;
        if (!writeFrameLocked(endCommand.data(), endCommand.size()))
        {
            lastCommandError_ = "failed to send end calibration write command";
            return finish(false, lastCommandError_);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kCalibrationCommitDelayMs));
        std::vector<uint8_t> endDrain;
        if (!readBytesLocked(&endDrain, 80))
        {
            lastCommandError_ = "failed while draining end calibration write response";
            ++stats.timeouts;
            return finish(false, lastCommandError_);
        }

        return finish(true, "ok");
    });
}

void GripperHmiDriver::handleParsedFrame(const GripperParsedFrame &frame)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastReceiveTimeMs_ = currentSteadyMs();
    ++stateGeneration_;
    ++rxFrameCount_;

    if (frame.type == GripperFrameType::KeyReport)
    {
        ++rxKeyReportCount_;
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
        ++rxBeepStateCount_;
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
                const uint64_t effectMs =
                    nowMs >= ledEffectStartedAtMs_ ? (nowMs - ledEffectStartedAtMs_) : 0;
                desiredColor = ledRenderer_.render(ledEffect_, effectMs, epochMs);
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
                ++txLedCount_;
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
                ++txBeepCount_;
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
                ++txStateRequestCount_;
                lastStateRequestAtMs_ = nowMs;
            }
        }

        if (!ledPrioritySend && ledShouldSend)
        {
            const auto frame = GripperHmiProtocol::buildSetRgbCommand(desiredColor);
            if (writeFrameLocked(frame.data(), frame.size()))
            {
                ++txLedCount_;
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
