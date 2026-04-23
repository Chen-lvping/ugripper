#include "sensor_recorder/sensor_protocol.h"

#include "bsp_crc.h"

namespace {

constexpr uint8_t kEncoderFuncRead = 0x03;
constexpr uint8_t kEncoderFuncWrite = 0x06;

}  // namespace

namespace ugripper::sensor {

uint16_t CalculateCrc16Modbus(std::span<const uint8_t> data)
{
    if (data.empty())
    {
        return 0;
    }

    return Get_CRC16(const_cast<uint8_t *>(data.data()),
                     static_cast<uint16_t>(data.size()),
                     CRC16Type::MODBUS);
}

bool VerifyCrc16Modbus(std::span<const uint8_t> data, uint8_t crc_low, uint8_t crc_high)
{
    const uint16_t crc = CalculateCrc16Modbus(data);
    return static_cast<uint8_t>(crc & 0xFF) == crc_low &&
           static_cast<uint8_t>((crc >> 8) & 0xFF) == crc_high;
}

std::optional<size_t> TryGetEncoderResponseFrameLength(std::span<const uint8_t> frame_prefix)
{
    if (frame_prefix.size() < 3)
    {
        return std::nullopt;
    }

    const uint8_t function_code = frame_prefix[1];
    const uint8_t payload_length = frame_prefix[2];

    if (function_code == kEncoderFuncRead)
    {
        if (payload_length == 0x02 || payload_length == 0x04)
        {
            return static_cast<size_t>(payload_length) + 5;
        }
        return std::nullopt;
    }

    if (function_code == kEncoderFuncWrite)
    {
        return static_cast<size_t>(8);
    }

    return std::nullopt;
}

}  // namespace ugripper::sensor
