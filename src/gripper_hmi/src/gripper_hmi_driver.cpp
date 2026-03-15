#include "gripper_hmi_driver.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kStateRequestIntervalMs = 20;
constexpr uint64_t kLedRenderIntervalMs = GripperLedEffectRenderer::recommendedRenderIntervalMs();
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

bool GripperHmiDriver::connect()
{
    std::lock_guard<std::mutex> ioLock(ioMutex_);
    if (serialPort_ != nullptr)
    {
        return true;
    }

    const std::string resolvedPort = resolveSerialPortPath(port_);
    if (sp_get_port_by_name(resolvedPort.c_str(), &serialPort_) != SP_OK)
    {
        std::cerr << name_ << ": cannot find port " << port_ << " (resolved=" << resolvedPort << ")" << std::endl;
        serialPort_ = nullptr;
        return false;
    }

    if (sp_open(serialPort_, SP_MODE_READ_WRITE) != SP_OK)
    {
        std::cerr << name_ << ": cannot open port " << port_ << " (resolved=" << resolvedPort << ")" << std::endl;
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
    pendingStateRequest_ = false;
    pendingLedUpdate_ = false;
    pendingLedColor_ = {};
    ledEffectEnabled_ = false;
    ledEffectDirty_ = false;
    ledEffect_ = {};
    pendingLedColor_ = GripperLedColor{0, 0, 0};
    lastRenderedColor_ = {255, 255, 255};
    lastStateRequestAtMs_ = 0;
    lastLedWriteAtMs_ = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        keyPressed_.fill(false);
        beepState_ = {};
        lastKeyReport_.reset();
        lastReceiveTimeMs_ = 0;
        stateGeneration_ = 0;
    }

    ioRunning_ = true;
    ioThread_ = std::thread(&GripperHmiDriver::ioLoop, this);
    return true;
}

void GripperHmiDriver::disconnect()
{
    {
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        if (serialPort_ == nullptr)
        {
            return;
        }
        ioRunning_ = false;
        pendingStateRequest_ = false;
        pendingLedUpdate_ = false;
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
        std::cerr << name_ << ": write failed on " << port_ << std::endl;
        return false;
    }

    return true;
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
        if (bytesRead <= 0)
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
                              lastStateRequestAtMs_ == 0 ||
                              (nowMs - lastStateRequestAtMs_) >= kStateRequestIntervalMs;
        const bool ledDue = lastLedWriteAtMs_ == 0 ||
                            (nowMs - lastLedWriteAtMs_) >= kLedRenderIntervalMs;

        GripperLedColor desiredColor = pendingLedColor_;
        if (ledEffectEnabled_)
        {
            desiredColor = ledRenderer_.render(ledEffect_, nowMs, epochMs);
        }

        const std::array<uint8_t, 3> desiredColorArray{desiredColor.red, desiredColor.green, desiredColor.blue};
        const bool ledShouldSend = pendingLedUpdate_ ||
                                   ledEffectDirty_ ||
                                   desiredColorArray != lastRenderedColor_ ||
                                   ledDue;

        if (stateDue)
        {
            const auto frame = GripperHmiProtocol::buildBeepStateRequest();
            pendingStateRequest_ = false;
            if (writeFrameLocked(frame.data(), frame.size()))
            {
                lastStateRequestAtMs_ = nowMs;
            }
        }

        if (ledShouldSend)
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

        ioCv_.wait_for(ioLock, std::chrono::milliseconds(2), [this]() {
            return !ioRunning_ || pendingStateRequest_ || pendingLedUpdate_;
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
