#ifndef GRIPPER_HMI_PROTOCOL_H
#define GRIPPER_HMI_PROTOCOL_H

#include "gripper_hmi_calibration_data.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct GripperLedColor
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

struct GripperKeyReport
{
    uint8_t rawCode = 0;
    int keyIndex = -1;
    bool pressed = false;
};

struct GripperBeepState
{
    uint16_t duty = 0;
    uint16_t frequency = 0;
};

enum class GripperFrameType
{
    KeyReport,
    BeepState,
    Unknown,
};

struct GripperParsedFrame
{
    GripperFrameType type = GripperFrameType::Unknown;
    uint8_t indexWord = 0;
    GripperKeyReport keyReport{};
    GripperBeepState beepState{};
};

class GripperHmiProtocol
{
public:
    static constexpr size_t kFrameLength = 8;
    static constexpr size_t kCalibrationChunkSize = 16;
    static constexpr size_t kCalibrationChunkFrameLength = 20;
    static constexpr size_t kSerialNumberChunkSize = 16;
    static constexpr size_t kSerialNumberFrameLength = 19;
    static constexpr size_t kStatusFrameLength = 8;
    static constexpr size_t kStopCalibrationWriteFrameLength = 20;
    static constexpr size_t kSerialNumberResponseFrameLength = 19;
    static constexpr uint8_t kSendHead1 = 0x5A;
    static constexpr uint8_t kSendHead2 = 0x5A;
    static constexpr uint8_t kRecvHead1 = 0xA5;
    static constexpr uint8_t kRecvHead2 = 0xA5;
    static constexpr uint8_t kIndexKeyReport = 0x01;
    static constexpr uint8_t kIndexBeepBase = 0x04;
    static constexpr uint8_t kIndexBeepFree = 0x0E;
    static constexpr uint8_t kIndexRgbLight = 0x0F;
    static constexpr uint8_t kIndexReadCalibrationData = 0x52;
    static constexpr uint8_t kIndexSerialNumber = 0x53;
    static constexpr uint8_t kIndexWriteCalibrationData = 0x57;
    static constexpr uint8_t kFuncPwmGet = 0x02;
    static constexpr uint8_t kFuncReadCalibrationData = 0x5A;
    static constexpr uint8_t kStatusOk = 0x54;
    static constexpr uint8_t kStatusAddressOutOfLimit = 0x88;
    static constexpr uint8_t kStatusIndexError = 0xF1;
    static constexpr uint8_t kStatusFunctionError = 0xF2;
    static constexpr uint8_t kStatusMissingData = 0xF3;
    static constexpr uint8_t kStatusInvalidCommand = 0xF4;
    static constexpr uint8_t kStatusChecksumError = 0xFF;

    static uint8_t calculateXor(const uint8_t *data, size_t length);
    static bool validateXor(const uint8_t *data, size_t length, uint8_t expected);
    static std::optional<GripperParsedFrame> tryConsumeFrame(std::vector<uint8_t> &buffer);
    static std::array<uint8_t, 7> buildBeepStateRequest();
    static std::array<uint8_t, 7> buildSetBeepCommand(const GripperBeepState &state);
    static std::array<uint8_t, 8> buildSetRgbCommand(const GripperLedColor &color);
    static std::array<uint8_t, 7> buildReadSerialNumberCommand();
    static std::array<uint8_t, 7> buildWriteSerialNumberCommand();
    static std::array<uint8_t, kSerialNumberFrameLength> buildWriteSerialNumberFrame(
        const uint8_t *chunkData,
        size_t chunkSize);
    static std::array<uint8_t, 7> buildBeginCalibrationWriteCommand();
    static std::array<uint8_t, 7> buildReadCalibrationChunkCommand(uint8_t packetIndex);
    static std::array<uint8_t, kCalibrationChunkFrameLength> buildCalibrationChunkFrame(
        uint8_t packetIndex,
        const uint8_t *chunkData,
        size_t chunkSize);
    static std::array<uint8_t, kStopCalibrationWriteFrameLength> buildEndCalibrationWriteCommand();
    static std::string describeStatusCode(uint8_t statusCode);

private:
    static std::array<uint8_t, 7> buildStandardCommand(uint8_t indexWord,
                                                       uint8_t functionWord,
                                                       uint8_t dataHigh,
                                                       uint8_t dataLow);
    static std::optional<GripperKeyReport> decodeKeyReport(uint8_t rawCode);
    static GripperKeyReport buildUnknownKeyReport(uint8_t rawCode);
};

#endif
