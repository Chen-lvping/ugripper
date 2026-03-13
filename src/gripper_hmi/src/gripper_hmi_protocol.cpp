#include "gripper_hmi_protocol.h"

#include <algorithm>

uint8_t GripperHmiProtocol::calculateXor(const uint8_t *data, size_t length)
{
    if (data == nullptr || length == 0)
    {
        return 0;
    }

    uint8_t value = data[0];
    for (size_t index = 1; index < length; ++index)
    {
        value ^= data[index];
    }
    return value;
}

std::optional<GripperKeyReport> GripperHmiProtocol::decodeKeyReport(uint8_t rawCode)
{
    GripperKeyReport report;
    report.rawCode = rawCode;

    if (rawCode >= 0x01 && rawCode <= 0x06)
    {
        report.keyIndex = static_cast<int>(rawCode - 0x01);
        report.pressed = true;
        return report;
    }

    if (rawCode >= 0x0A && rawCode <= 0x0F)
    {
        report.keyIndex = static_cast<int>(rawCode - 0x0A);
        report.pressed = false;
        return report;
    }

    return std::nullopt;
}

GripperKeyReport GripperHmiProtocol::buildUnknownKeyReport(uint8_t rawCode)
{
    GripperKeyReport report;
    report.rawCode = rawCode;
    report.keyIndex = -1;
    report.pressed = (rawCode > 0x00 && rawCode < 0x0A);
    return report;
}

std::optional<GripperParsedFrame> GripperHmiProtocol::tryConsumeFrame(std::vector<uint8_t> &buffer)
{
    static constexpr std::array<uint8_t, 2> kHeader = {kRecvHead1, kRecvHead2};

    while (buffer.size() >= kFrameLength)
    {
        auto header = std::search(buffer.begin(), buffer.end(), kHeader.begin(), kHeader.end());
        if (header == buffer.end())
        {
            buffer.clear();
            return std::nullopt;
        }

        if (header != buffer.begin())
        {
            buffer.erase(buffer.begin(), header);
        }

        if (buffer.size() < kFrameLength)
        {
            return std::nullopt;
        }

        const uint8_t checksum = calculateXor(buffer.data(), kFrameLength - 1);
        if (checksum != buffer[kFrameLength - 1])
        {
            buffer.erase(buffer.begin());
            continue;
        }

        GripperParsedFrame frame;
        frame.indexWord = buffer[2];

        if (frame.indexWord == kIndexKeyReport)
        {
            frame.type = GripperFrameType::KeyReport;
            const auto keyReport = decodeKeyReport(buffer[6]);
            frame.keyReport = keyReport.value_or(buildUnknownKeyReport(buffer[6]));
        }
        else if (frame.indexWord == kIndexBeepBase)
        {
            frame.type = GripperFrameType::BeepState;
            frame.beepState.duty = static_cast<uint16_t>(buffer[3] << 8) | buffer[4];
            frame.beepState.frequency = static_cast<uint16_t>(buffer[5] << 8) | buffer[6];
        }

        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(kFrameLength));
        return frame;
    }

    return std::nullopt;
}

std::array<uint8_t, 7> GripperHmiProtocol::buildBeepStateRequest()
{
    std::array<uint8_t, 7> frame = {
        kSendHead1,
        kSendHead2,
        kIndexBeepBase,
        kFuncPwmGet,
        0x00,
        0x00,
        0x00,
    };
    frame[6] = calculateXor(frame.data(), frame.size() - 1);
    return frame;
}

std::array<uint8_t, 8> GripperHmiProtocol::buildSetRgbCommand(const GripperLedColor &color)
{
    std::array<uint8_t, 8> frame = {
        kSendHead1,
        kSendHead2,
        kIndexRgbLight,
        color.red,
        color.green,
        color.blue,
        0x00,
    };
    frame[6] = calculateXor(frame.data(), 6);
    return frame;
}
