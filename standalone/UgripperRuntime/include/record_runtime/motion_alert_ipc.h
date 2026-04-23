#pragma once

#include <cstddef>
#include <cstdint>

namespace ugripper {

constexpr uint32_t kMotionAlertMessageMagic = 0x4D414C54;
constexpr uint16_t kMotionAlertMessageVersion = 1;

enum class MotionAlertSide : uint8_t
{
    Unknown = 0,
    Left = 1,
    Right = 2,
};

enum class MotionAlertReasonCode : uint8_t
{
    None = 0,
    Gyro = 1,
    Accel = 2,
    AccelAndGyro = 3,
    Recovered = 4,
};

struct MotionAlertMessage
{
    uint32_t magic = kMotionAlertMessageMagic;
    uint16_t version = kMotionAlertMessageVersion;
    uint8_t side = static_cast<uint8_t>(MotionAlertSide::Unknown);
    uint8_t reason = static_cast<uint8_t>(MotionAlertReasonCode::None);
    uint8_t active = 0;
    uint8_t reserved0 = 0;
    uint16_t reserved1 = 0;
    float gyroMagnitude = 0.0f;
    float accelExcess = 0.0f;
    uint64_t steadyTimeMs = 0;
};

static_assert(std::is_standard_layout<MotionAlertMessage>::value,
              "MotionAlertMessage must be standard layout");

}  // namespace ugripper
