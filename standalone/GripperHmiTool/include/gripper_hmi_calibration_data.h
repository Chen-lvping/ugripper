#ifndef GRIPPER_HMI_CALIBRATION_DATA_H
#define GRIPPER_HMI_CALIBRATION_DATA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gripper_hmi
{

constexpr size_t kSerialNumberLength = 16;
constexpr size_t kSerialNumberFieldLength = 32;
constexpr size_t kCalibrationPayloadSize = 1024;
constexpr uint32_t kCalibrationDataFormatVersion1 = 0x00010000u;

enum GripperCalibrationValidField : uint32_t
{
    kCalibrationValidRgbCamera = 1u << 0,
    kCalibrationValidStereoCam0 = 1u << 1,
    kCalibrationValidStereoCam1 = 1u << 2,
    kCalibrationValidExtrinsics = 1u << 3,
    kCalibrationValidImu = 1u << 4,
    kCalibrationValidResiduals = 1u << 5,
};

#pragma pack(push, 1)

struct GripperCalibrationHeader
{
    char magic[4] = {'U', 'C', 'A', 'L'};
    uint32_t dataFormatVersion = kCalibrationDataFormatVersion1;
    uint16_t payloadSize = static_cast<uint16_t>(sizeof(GripperCalibrationHeader));
    uint16_t headerSize = static_cast<uint16_t>(sizeof(GripperCalibrationHeader));
    uint32_t validFields = 0;
    uint8_t reserved[16] = {};
};

struct GripperRgbCameraBlock
{
    uint32_t cameraModelEnum = 0;
    float distortionCoefficients[4] = {};
    float intrinsics[4] = {};
    float resolution[2] = {};
    uint8_t reserved[4] = {};
};

struct GripperStereoCameraBlock
{
    uint32_t cameraModelEnum = 0;
    float focalLength[2] = {};
    float principalPoint[2] = {};
    float distortionCoefficients[4] = {};
    float reserved[3] = {};
};

struct GripperExtrinsicsBlock
{
    float tIcCam0ToImu0[16] = {};
    float timeshiftCam0ToImu0 = 0.0f;
    float reserved0[3] = {};
    float tIcCam1ToImu0[16] = {};
    float timeshiftCam1ToImu0 = 0.0f;
    float baselineNorm = 0.0f;
    float reserved1[2] = {};
};

struct GripperImuBlock
{
    float updateRate = 0.0f;
    float accelerometerNoiseDensityDiscrete = 0.0f;
    float accelerometerRandomWalk = 0.0f;
    float gyroscopeNoiseDensityDiscrete = 0.0f;
    float gyroscopeRandomWalk = 0.0f;
    float reserved[3] = {};
};

struct GripperStatisticsBlock
{
    float mean = 0.0f;
    float median = 0.0f;
    float stddev = 0.0f;
    float reserved = 0.0f;
};

struct GripperResidualsBlock
{
    GripperStatisticsBlock reprojectionErrorCam0Px{};
    GripperStatisticsBlock reprojectionErrorCam1Px{};
    GripperStatisticsBlock gyroscopeErrorImu0RadS{};
    GripperStatisticsBlock accelerometerErrorImu0MS2{};
};

struct GripperCalibrationDataV1
{
    // RGB 主相机：model + 畸变 + [fx, fy, cx, cy] + [width, height]
    GripperCalibrationHeader header{};
    GripperRgbCameraBlock rgbCamera{};

    // 双目 cam0 / cam1：model + [fx, fy] + [cx, cy] + 4 项畸变
    GripperStereoCameraBlock stereoCam0{};
    GripperStereoCameraBlock stereoCam1{};

    // cam0/cam1 -> imu0 外参矩阵、时间偏移与 baseline norm
    GripperExtrinsicsBlock extrinsics{};

    // IMU0：update_rate、acc/gyro discrete noise density、random walk
    GripperImuBlock imu0{};

    // 残差统计：cam0/cam1 reprojection 与 imu gyro/acc 的 mean/median/stddev
    GripperResidualsBlock residuals{};

    // 预留扩展：保持 1024-byte 固定对齐，不改变现有块偏移
    uint8_t reserved[592] = {};
};

#pragma pack(pop)

using GripperSerialNumber = std::array<char, kSerialNumberFieldLength>;

static_assert(sizeof(GripperCalibrationHeader) == 32, "GripperCalibrationHeader must be 32 bytes");
static_assert(sizeof(GripperRgbCameraBlock) == 48, "GripperRgbCameraBlock must be 48 bytes");
static_assert(sizeof(GripperStereoCameraBlock) == 48, "GripperStereoCameraBlock must be 48 bytes");
static_assert(sizeof(GripperExtrinsicsBlock) == 160, "GripperExtrinsicsBlock must be 160 bytes");
static_assert(sizeof(GripperImuBlock) == 32, "GripperImuBlock must be 32 bytes");
static_assert(sizeof(GripperStatisticsBlock) == 16, "GripperStatisticsBlock must be 16 bytes");
static_assert(sizeof(GripperResidualsBlock) == 64, "GripperResidualsBlock must be 64 bytes");
static_assert(sizeof(GripperCalibrationDataV1) == kCalibrationPayloadSize,
              "GripperCalibrationDataV1 must be exactly 1024 bytes");

constexpr size_t kCalibrationOffsetHeader = 0x000;
constexpr size_t kCalibrationOffsetRgbCamera = 0x020;
constexpr size_t kCalibrationOffsetStereoCam0 = 0x050;
constexpr size_t kCalibrationOffsetStereoCam1 = 0x080;
constexpr size_t kCalibrationOffsetExtrinsics = 0x0B0;
constexpr size_t kCalibrationOffsetImu0 = 0x150;
constexpr size_t kCalibrationOffsetResiduals = 0x170;
constexpr size_t kCalibrationOffsetReserved = 0x1B0;

static_assert(offsetof(GripperCalibrationDataV1, header) == 0, "header offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, rgbCamera) == 32, "rgbCamera offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, stereoCam0) == 80, "stereoCam0 offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, stereoCam1) == 128, "stereoCam1 offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, extrinsics) == 176, "extrinsics offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, imu0) == 336, "imu0 offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, residuals) == 368, "residuals offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, reserved) == 432, "reserved offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, header) == kCalibrationOffsetHeader, "header const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, rgbCamera) == kCalibrationOffsetRgbCamera, "rgbCamera const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, stereoCam0) == kCalibrationOffsetStereoCam0, "stereoCam0 const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, stereoCam1) == kCalibrationOffsetStereoCam1, "stereoCam1 const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, extrinsics) == kCalibrationOffsetExtrinsics, "extrinsics const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, imu0) == kCalibrationOffsetImu0, "imu0 const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, residuals) == kCalibrationOffsetResiduals, "residuals const offset mismatch");
static_assert(offsetof(GripperCalibrationDataV1, reserved) == kCalibrationOffsetReserved, "reserved const offset mismatch");

inline bool isPrintableSerialChar(char value)
{
    return value >= 0x20 && value <= 0x7E;
}

inline bool encodeSerialNumber(const std::string &text, GripperSerialNumber *serial)
{
    if (serial == nullptr || text.empty() || text.size() > kSerialNumberFieldLength)
    {
        return false;
    }

    GripperSerialNumber encoded{};
    for (size_t index = 0; index < text.size(); ++index)
    {
        if (!isPrintableSerialChar(text[index]))
        {
            return false;
        }
        encoded[index] = text[index];
    }

    *serial = encoded;
    return true;
}

inline std::string decodeSerialNumber(const GripperSerialNumber &serial)
{
    size_t length = serial.size();
    while (length > 0 && serial[length - 1] == '\0')
    {
        --length;
    }
    return std::string(serial.data(), length);
}

inline GripperCalibrationDataV1 makeDefaultCalibrationDataV1()
{
    GripperCalibrationDataV1 data;
    data.header.validFields = kCalibrationValidRgbCamera |
                              kCalibrationValidStereoCam0 |
                              kCalibrationValidStereoCam1 |
                              kCalibrationValidExtrinsics |
                              kCalibrationValidImu |
                              kCalibrationValidResiduals;
    return data;
}

}  // namespace gripper_hmi

#endif
