#include "gripper_hmi_protocol.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

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

bool GripperHmiProtocol::validateXor(const uint8_t *data, size_t length, uint8_t expected)
{
    return calculateXor(data, length) == expected;
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

std::array<uint8_t, 7> GripperHmiProtocol::buildStandardCommand(uint8_t indexWord,
                                                                uint8_t functionWord,
                                                                uint8_t dataHigh,
                                                                uint8_t dataLow)
{
    std::array<uint8_t, 7> frame = {
        kSendHead1,
        kSendHead2,
        indexWord,
        functionWord,
        dataHigh,
        dataLow,
        0x00,
    };
    frame[6] = calculateXor(frame.data(), frame.size() - 1);
    return frame;
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
    return buildStandardCommand(kIndexBeepBase, kFuncPwmGet, 0x00, 0x00);
}

std::array<uint8_t, 7> GripperHmiProtocol::buildSetBeepCommand(const GripperBeepState &state)
{
    // The legacy UGripper driver writes the actual buzzer command through
    // INDEX_WORD_BEEP_FREE (0x0E). INDEX_WORD_BEEP_BASE (0x04) is only used
    // for querying the current PWM state.
    const uint8_t duty = static_cast<uint8_t>(std::min<uint16_t>(state.duty, 0xFF));
    std::array<uint8_t, 7> frame = buildStandardCommand(
        kIndexBeepFree,
        duty,
        static_cast<uint8_t>((state.frequency >> 8) & 0xFF),
        static_cast<uint8_t>(state.frequency & 0xFF));
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

std::array<uint8_t, 7> GripperHmiProtocol::buildReadSerialNumberCommand()
{
    return buildStandardCommand(kIndexReadCalibrationData, kIndexSerialNumber, 0x00, 0x00);
}

std::array<uint8_t, 7> GripperHmiProtocol::buildWriteSerialNumberCommand()
{
    return buildStandardCommand(kIndexWriteCalibrationData, kIndexSerialNumber, 0x4E, 0x43);
}

std::array<uint8_t, GripperHmiProtocol::kSerialNumberFrameLength> GripperHmiProtocol::buildWriteSerialNumberFrame(
    const uint8_t *chunkData,
    size_t chunkSize)
{
    std::array<uint8_t, kSerialNumberFrameLength> frame = {};
    frame[0] = kSendHead1;
    frame[1] = kSendHead2;
    const size_t copySize = std::min(chunkSize, kSerialNumberChunkSize);
    if (chunkData != nullptr && copySize > 0)
    {
        std::memcpy(frame.data() + 2, chunkData, copySize);
    }
    frame[kSerialNumberFrameLength - 1] = calculateXor(frame.data(), kSerialNumberFrameLength - 1);
    return frame;
}

std::array<uint8_t, 7> GripperHmiProtocol::buildBeginCalibrationWriteCommand()
{
    return buildStandardCommand(kIndexWriteCalibrationData, 0x5A, 0x72, 0x6F);
}

std::array<uint8_t, 7> GripperHmiProtocol::buildReadCalibrationChunkCommand(uint8_t packetIndex)
{
    return buildStandardCommand(kIndexReadCalibrationData, kFuncReadCalibrationData, 0x00, packetIndex);
}

std::array<uint8_t, GripperHmiProtocol::kCalibrationChunkFrameLength> GripperHmiProtocol::buildCalibrationChunkFrame(
    uint8_t packetIndex,
    const uint8_t *chunkData,
    size_t chunkSize)
{
    std::array<uint8_t, kCalibrationChunkFrameLength> frame = {};
    frame[0] = kSendHead1;
    frame[1] = kSendHead2;
    frame[2] = packetIndex;
    const size_t copySize = std::min(chunkSize, kCalibrationChunkSize);
    if (chunkData != nullptr && copySize > 0)
    {
        std::memcpy(frame.data() + 3, chunkData, copySize);
    }
    frame[kCalibrationChunkFrameLength - 1] = calculateXor(frame.data(), kCalibrationChunkFrameLength - 1);
    return frame;
}

std::array<uint8_t, GripperHmiProtocol::kStopCalibrationWriteFrameLength> GripperHmiProtocol::buildEndCalibrationWriteCommand()
{
    std::array<uint8_t, kStopCalibrationWriteFrameLength> frame = {};
    static constexpr char kCommandText[] = "Stop_WriteInData";
    frame[0] = kSendHead1;
    frame[1] = kSendHead2;
    std::memcpy(frame.data() + 2, kCommandText, sizeof(kCommandText));
    frame[kStopCalibrationWriteFrameLength - 1] = calculateXor(frame.data(), kStopCalibrationWriteFrameLength - 1);
    return frame;
}

std::array<uint8_t, GripperHmiProtocol::kAbortCalibrationWriteFrameLength> GripperHmiProtocol::buildAbortCalibrationWriteCommand()
{
    std::array<uint8_t, kAbortCalibrationWriteFrameLength> frame = {};
    static constexpr char kCommandText[] = "AbortWriteInData";
    frame[0] = kSendHead1;
    frame[1] = kSendHead2;
    std::memcpy(frame.data() + 2, kCommandText, sizeof(kCommandText));
    frame[kAbortCalibrationWriteFrameLength - 1] = calculateXor(frame.data(), kAbortCalibrationWriteFrameLength - 1);
    return frame;
}

std::string GripperHmiProtocol::describeStatusCode(uint8_t statusCode)
{
    switch (statusCode)
    {
    case kStatusOk:
        return "ok";
    case kStatusAddressOutOfLimit:
        return "address_out_of_limit";
    case kStatusIndexError:
        return "index_error";
    case kStatusFunctionError:
        return "function_error";
    case kStatusMissingData:
        return "missing_data";
    case kStatusInvalidCommand:
        return "invalid_command";
    case kStatusZeroDataChecksumError:
        return "zero_data_checksum_error";
    case kStatusChecksumError:
        return "checksum_error";
    default:
    {
        std::ostringstream stream;
        stream << "unknown_status_0x"
               << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(statusCode);
        return stream.str();
    }
    }
}
