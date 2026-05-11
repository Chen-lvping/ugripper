#ifndef MAIN_CAMERA_CALIBRATION_DATA_H
#define MAIN_CAMERA_CALIBRATION_DATA_H

#include <cstddef>
#include <cstdint>

namespace main_camera
{

constexpr size_t kCalibrationPayloadSize = 1024;
constexpr uint32_t kCalibrationDataFormatVersion1 = 0x00010000u;

enum MainCameraCalibrationValidField : uint32_t
{
    kCalibrationValidCameraModel = 1u << 0,
    kCalibrationValidDistortion = 1u << 1,
    kCalibrationValidIntrinsics = 1u << 2,
    kCalibrationValidResolution = 1u << 3,
};

#pragma pack(push, 1)

struct MainCameraCalibrationHeader
{
    char magic[4] = {'M', 'C', 'A', 'L'};
    uint32_t dataFormatVersion = kCalibrationDataFormatVersion1;
    uint16_t payloadSize = 0;
    uint16_t headerSize = static_cast<uint16_t>(sizeof(MainCameraCalibrationHeader));
    uint32_t validFields = 0;
    uint8_t reserved[16] = {};
};

struct MainCameraIntrinsicsBlock
{
    uint32_t cameraModelEnum = 0;
    float distortionCoefficients[4] = {};
    float intrinsics[4] = {};
    float resolution[2] = {};
    uint8_t reserved[4] = {};
};

struct MainCameraCalibrationDataV1
{
    MainCameraCalibrationHeader header{};
    MainCameraIntrinsicsBlock mainCamera{};
    uint8_t reserved[kCalibrationPayloadSize - sizeof(MainCameraCalibrationHeader) - sizeof(MainCameraIntrinsicsBlock)] = {};
};

#pragma pack(pop)

static_assert(sizeof(MainCameraCalibrationHeader) == 32, "MainCameraCalibrationHeader must be 32 bytes");
static_assert(sizeof(MainCameraIntrinsicsBlock) == 48, "MainCameraIntrinsicsBlock must be 48 bytes");
static_assert(sizeof(MainCameraCalibrationDataV1) == kCalibrationPayloadSize,
              "MainCameraCalibrationDataV1 must be exactly 1024 bytes");

inline MainCameraCalibrationDataV1 makeDefaultCalibrationDataV1()
{
    MainCameraCalibrationDataV1 data{};
    data.header.payloadSize = static_cast<uint16_t>(
        sizeof(MainCameraCalibrationHeader) + sizeof(MainCameraIntrinsicsBlock));
    data.header.validFields = kCalibrationValidCameraModel |
                              kCalibrationValidDistortion |
                              kCalibrationValidIntrinsics |
                              kCalibrationValidResolution;
    return data;
}

}  // namespace main_camera

#endif
