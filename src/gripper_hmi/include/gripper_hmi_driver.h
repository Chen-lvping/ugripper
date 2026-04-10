#ifndef GRIPPER_HMI_DRIVER_H
#define GRIPPER_HMI_DRIVER_H

#include "gripper_hmi_calibration_data.h"
#include "gripper_hmi_led_effects.h"
#include "gripper_hmi_protocol.h"

#include <libserialport.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <optional>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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
    static constexpr uint16_t kDefaultBeepDuty = 50;
    static constexpr uint16_t kDefaultBeepFrequency = 4000;

    GripperHmiDriver(const std::string &port, uint32_t baudrate = 115200, const std::string &name = "GripperHmiDriver");
    ~GripperHmiDriver();

    bool connect();
    void disconnect();
    bool isConnected() const;

    bool requestState();
    bool setLedColor(const GripperLedColor &color);
    bool setLedColor(uint8_t red, uint8_t green, uint8_t blue);
    bool setLedEffect(const GripperLedEffect &effect);
    bool setBeepState(const GripperBeepState &state);
    bool setBeepEnabled(bool enabled);
    bool silenceBeep();
    bool readSerialNumber(std::string *serialNumber);
    bool writeSerialNumber(const std::string &serialNumber);
    bool readCalibrationData(gripper_hmi::GripperCalibrationDataV1 *calibrationData);
    bool readSerialNumberAndCalibration(std::string *serialNumber,
                                        bool *hasSerialNumber,
                                        gripper_hmi::GripperCalibrationDataV1 *calibrationData);
    bool writeCalibrationData(const gripper_hmi::GripperCalibrationDataV1 &calibrationData);

    bool pollOnce(int timeoutMs = 20);
    bool waitForKeyChange(int timeoutMs, GripperKeyReport *report);

    bool isKeyPressed(size_t keyIndex) const;
    bool isActive(uint64_t timeoutMs = 2000) const;
    GripperHmiSnapshot getSnapshot(uint64_t activeTimeoutMs = 2000) const;

    const std::string &getName() const { return name_; }
    const std::string &getPort() const { return port_; }
    uint32_t getBaudrate() const { return baudrate_; }
    const std::string &getLastCommandError() const { return lastCommandError_; }

private:
    enum class ExclusiveFrameReadResult
    {
        Timeout,
        Data,
        Status,
    };

    void ioLoop();
    bool writeFrameLocked(const uint8_t *data, size_t size);
    bool readAndProcessAvailableLocked();
    bool pauseIoThreadForExclusiveCommand(std::unique_lock<std::mutex> *ioLock);
    void resumeIoThreadLocked();
    bool readBytesLocked(std::vector<uint8_t> *buffer, int timeoutMs);
    bool readSizedFrameLocked(size_t frameSize, std::vector<uint8_t> *frame, int timeoutMs);
    bool readAnyStatusFrameLocked(uint8_t *token, uint8_t *statusCode, int timeoutMs);
    bool readStatusFrameLocked(uint8_t token, uint8_t *statusCode, int timeoutMs);
    bool readExpectedStatusFrameLocked(uint8_t expectedToken, uint8_t *token, uint8_t *statusCode, int timeoutMs);
    bool readDataFrameLocked(uint8_t token, size_t dataLength, std::vector<uint8_t> *payload, int timeoutMs);
    bool readRawDataFrameLocked(size_t dataLength, std::vector<uint8_t> *payload, int timeoutMs);
    bool readCalibrationFrameLocked(uint8_t expectedToken, std::vector<uint8_t> *payload, int timeoutMs);
    bool abortCalibrationWriteStateLocked(const std::string &reason);
    bool readSerialNumberLocked(std::string *serialNumber);
    bool readCalibrationDataLocked(gripper_hmi::GripperCalibrationDataV1 *calibrationData);
    ExclusiveFrameReadResult readRawDataOrStatusFrameLocked(size_t dataLength,
                                                            std::vector<uint8_t> *payload,
                                                            uint8_t *token,
                                                            uint8_t *statusCode,
                                                            int timeoutMs);
    ExclusiveFrameReadResult readDataOrStatusFrameLocked(uint8_t expectedToken,
                                                         size_t dataLength,
                                                         std::vector<uint8_t> *payload,
                                                         uint8_t *token,
                                                         uint8_t *statusCode,
                                                         int timeoutMs);
    bool runExclusiveCommand(const std::function<bool()> &command);
    void handleIoFailureLocked(const char *operation);
    void handleParsedFrame(const GripperParsedFrame &frame);
    void logConnectFailureLocked(const std::string &message);
    void resetConnectFailureLogLocked(const std::string &resolvedPort);
    static uint64_t currentSteadyMs();

    std::string name_;
    std::string port_;
    uint32_t baudrate_;
    struct sp_port *serialPort_;
    std::vector<uint8_t> rxBuffer_;

    mutable std::mutex ioMutex_;
    std::condition_variable ioCv_;
    std::thread ioThread_;
    bool ioRunning_ = false;
    bool pendingStateRequest_ = false;
    bool pendingLedUpdate_ = false;
    GripperLedColor pendingLedColor_{};
    bool pendingBeepUpdate_ = false;
    GripperBeepState pendingBeepState_{};
    GripperBeepState activeBeepState_{};
    uint64_t lastBeepWriteAtMs_ = 0;
    bool ledEffectEnabled_ = false;
    bool ledEffectDirty_ = false;
    GripperLedEffect ledEffect_{};
    GripperLedEffectRenderer ledRenderer_{};
    std::array<uint8_t, 3> lastRenderedColor_{255, 255, 255};
    uint64_t lastLedRenderAtMs_ = 0;
    uint64_t lastStateRequestAtMs_ = 0;
    uint64_t lastLedWriteAtMs_ = 0;

    mutable std::mutex stateMutex_;
    std::condition_variable stateCv_;
    std::array<bool, 6> keyPressed_{};
    GripperBeepState beepState_{};
    std::optional<GripperKeyReport> lastKeyReport_;
    uint64_t lastReceiveTimeMs_ = 0;
    uint64_t stateGeneration_ = 0;
    std::string lastCommandError_;
    std::string lastConnectFailureMessage_;
    uint64_t lastConnectFailureLogAtMs_ = 0;
    uint64_t firstConnectFailureAtMs_ = 0;
    uint32_t suppressedConnectFailureCount_ = 0;
};

#endif
