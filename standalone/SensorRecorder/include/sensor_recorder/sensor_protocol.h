#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ugripper::sensor {

uint16_t CalculateCrc16Modbus(std::span<const uint8_t> data);
bool VerifyCrc16Modbus(std::span<const uint8_t> data, uint8_t crc_low, uint8_t crc_high);
std::optional<size_t> TryGetEncoderResponseFrameLength(std::span<const uint8_t> frame_prefix);

}  // namespace ugripper::sensor
