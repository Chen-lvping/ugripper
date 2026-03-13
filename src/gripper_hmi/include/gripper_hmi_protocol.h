#ifndef GRIPPER_HMI_PROTOCOL_H
#define GRIPPER_HMI_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    static constexpr uint8_t kSendHead1 = 0x5A;
    static constexpr uint8_t kSendHead2 = 0x5A;
    static constexpr uint8_t kRecvHead1 = 0xA5;
    static constexpr uint8_t kRecvHead2 = 0xA5;
    static constexpr uint8_t kIndexKeyReport = 0x01;
    static constexpr uint8_t kIndexBeepBase = 0x04;
    static constexpr uint8_t kIndexRgbLight = 0x0F;
    static constexpr uint8_t kFuncPwmGet = 0x02;

    static uint8_t calculateXor(const uint8_t *data, size_t length);
    static std::optional<GripperParsedFrame> tryConsumeFrame(std::vector<uint8_t> &buffer);
    static std::array<uint8_t, 7> buildBeepStateRequest();
    static std::array<uint8_t, 8> buildSetRgbCommand(const GripperLedColor &color);

private:
    static std::optional<GripperKeyReport> decodeKeyReport(uint8_t rawCode);
    static GripperKeyReport buildUnknownKeyReport(uint8_t rawCode);
};

#endif
