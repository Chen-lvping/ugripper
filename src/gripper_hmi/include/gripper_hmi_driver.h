#ifndef GRIPPER_HMI_DRIVER_H
#define GRIPPER_HMI_DRIVER_H

#include "gripper_hmi_protocol.h"

#include <libserialport.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct GripperHmiSnapshot
{
    std::array<bool, 6> keyPressed{};
    bool active = false;
    uint64_t lastRxAgeMs = 0;
    GripperBeepState beepState{};
    std::optional<GripperKeyReport> lastKeyReport;
};

class GripperHmiDriver
{
public:
    GripperHmiDriver(const std::string &port, uint32_t baudrate = 115200, const std::string &name = "GripperHmiDriver");
    ~GripperHmiDriver();

    bool connect();
    void disconnect();
    bool isConnected() const;

    bool requestState();
    bool setLedColor(const GripperLedColor &color);
    bool setLedColor(uint8_t red, uint8_t green, uint8_t blue);

    bool pollOnce(int timeoutMs = 20);
    bool waitForKeyChange(int timeoutMs, GripperKeyReport *report);

    bool isKeyPressed(size_t keyIndex) const;
    bool isActive(uint64_t timeoutMs = 500) const;
    GripperHmiSnapshot getSnapshot(uint64_t activeTimeoutMs = 500) const;

    const std::string &getName() const { return name_; }
    const std::string &getPort() const { return port_; }
    uint32_t getBaudrate() const { return baudrate_; }

private:
    bool writeFrame(const uint8_t *data, size_t size);
    bool readAndProcessAvailable();
    void handleParsedFrame(const GripperParsedFrame &frame);
    static uint64_t currentSteadyMs();

    std::string name_;
    std::string port_;
    uint32_t baudrate_;
    struct sp_port *serialPort_;
    std::vector<uint8_t> rxBuffer_;

    mutable std::mutex ioMutex_;
    mutable std::mutex stateMutex_;
    std::array<bool, 6> keyPressed_{};
    GripperBeepState beepState_{};
    std::optional<GripperKeyReport> lastKeyReport_;
    uint64_t lastReceiveTimeMs_ = 0;
};

#endif
