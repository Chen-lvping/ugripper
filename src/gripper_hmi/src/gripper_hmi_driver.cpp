#include "gripper_hmi_driver.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

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
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        keyPressed_.fill(false);
        beepState_ = {};
        lastKeyReport_.reset();
        lastReceiveTimeMs_ = 0;
    }

    return true;
}

void GripperHmiDriver::disconnect()
{
    if (serialPort_ == nullptr)
    {
        return;
    }

    const auto offFrame = GripperHmiProtocol::buildSetRgbCommand(GripperLedColor{0, 0, 0});
    writeFrame(offFrame.data(), offFrame.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sp_flush(serialPort_, SP_BUF_BOTH);
    sp_close(serialPort_);
    sp_free_port(serialPort_);
    serialPort_ = nullptr;
}

bool GripperHmiDriver::isConnected() const
{
    return serialPort_ != nullptr;
}

bool GripperHmiDriver::writeFrame(const uint8_t *data, size_t size)
{
    if (serialPort_ == nullptr || data == nullptr || size == 0)
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
    const auto frame = GripperHmiProtocol::buildBeepStateRequest();
    return writeFrame(frame.data(), frame.size());
}

bool GripperHmiDriver::setLedColor(const GripperLedColor &color)
{
    const auto frame = GripperHmiProtocol::buildSetRgbCommand(color);
    return writeFrame(frame.data(), frame.size());
}

bool GripperHmiDriver::setLedColor(uint8_t red, uint8_t green, uint8_t blue)
{
    return setLedColor(GripperLedColor{red, green, blue});
}

void GripperHmiDriver::handleParsedFrame(const GripperParsedFrame &frame)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastReceiveTimeMs_ = currentSteadyMs();

    if (frame.type == GripperFrameType::KeyReport)
    {
        lastKeyReport_ = frame.keyReport;
        if (frame.keyReport.keyIndex >= 0 && frame.keyReport.keyIndex < static_cast<int>(keyPressed_.size()))
        {
            keyPressed_[static_cast<size_t>(frame.keyReport.keyIndex)] = frame.keyReport.pressed;
        }
        return;
    }

    if (frame.type == GripperFrameType::BeepState)
    {
        beepState_ = frame.beepState;
    }
}

bool GripperHmiDriver::readAndProcessAvailable()
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

bool GripperHmiDriver::pollOnce(int timeoutMs)
{
    if (serialPort_ == nullptr)
    {
        return false;
    }

    const uint64_t startMs = currentSteadyMs();
    bool parsedAnyFrame = false;

    do
    {
        if (readAndProcessAvailable())
        {
            parsedAnyFrame = true;
        }

        if (parsedAnyFrame || timeoutMs <= 0)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while ((currentSteadyMs() - startMs) < static_cast<uint64_t>(timeoutMs));

    return parsedAnyFrame;
}

bool GripperHmiDriver::waitForKeyChange(int timeoutMs, GripperKeyReport *report)
{
    std::optional<GripperKeyReport> before;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        before = lastKeyReport_;
    }

    const uint64_t startMs = currentSteadyMs();
    do
    {
        pollOnce(10);

        std::optional<GripperKeyReport> current;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            current = lastKeyReport_;
        }

        if (current.has_value() && (!before.has_value() ||
            current->rawCode != before->rawCode ||
            current->pressed != before->pressed ||
            current->keyIndex != before->keyIndex))
        {
            if (report != nullptr)
            {
                *report = current.value();
            }
            return true;
        }
    } while ((currentSteadyMs() - startMs) < static_cast<uint64_t>(timeoutMs));

    return false;
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
