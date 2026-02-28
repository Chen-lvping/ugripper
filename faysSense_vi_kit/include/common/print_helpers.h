#ifndef __PRINT_HELPERS_H__
#define __PRINT_HELPERS_H__

#include "fays_atrak/fays_vikit.h"

bool PrintDeviceInfo(void *handle) {
    AtrakDeviceInfo info;
    if (FAYS_VIK_GetDeviceInfo(handle, &info) == EXIT_SUCCESS) {
        std::cout << "Device Model: " << info.device_model << std::endl;
        std::cout << "Serial Number: " << info.serial_number << std::endl;
        std::cout << "Firmware Version: " << info.firmware_version << std::endl;
        std::cout << "Number of Cameras: " << info.camera_nums << std::endl;
        std::cout << "Number of IMUs: " << info.imu_nums << std::endl;
        return true;
    }
    return false;
}

bool PrintCalibrationInfo(void *handle) {
    AtrakCalibrationParam calibParam;
    if (EXIT_SUCCESS != FAYS_VIK_GetCalibrationParam(handle, &calibParam)) {
        return false;
    }

    // 设置输出流精度为小数点后6位
    std::cout << std::fixed << std::setprecision(6);

    std::cout << "Calibration camera number: " << calibParam.cameras.num_of_cams << std::endl;
    for (uint32_t i = 0; i < calibParam.cameras.num_of_cams; ++i) {
        const AtrakCamParam& cam = calibParam.cameras.cameras[i];
        std::cout << "Camera " << static_cast<int>(cam.cam_id) << " Intrinsics:" << std::endl;
        std::cout << "  Focal Length: (" << cam.intrinsic.fx << ", " << cam.intrinsic.fy << ")" << std::endl;
        std::cout << "  Principal Point: (" << cam.intrinsic.cx << ", " << cam.intrinsic.cy << ")" << std::endl;
        std::cout << "  Distortion Coefficients: ";
        for (int j = 0; j < 8; ++j) {
            std::cout << cam.distortion.dis[j] << " ";
        }
        std::cout << std::endl;
    }

    std::cout << "IMU Calibration Parameters:" << std::endl;
    std::cout << "  Accelerometer Noise Density: " << calibParam.imu.accelerometer_noise_density << std::endl;
    std::cout << "  Accelerometer Random Walk: " << calibParam.imu.accelerometer_random_walk << std::endl;
    std::cout << "  Gyroscope Noise Density: " << calibParam.imu.gyroscope_noise_density << std::endl;
    std::cout << "  Gyroscope Random Walk: " << calibParam.imu.gyroscope_random_walk << std::endl;
    std::cout << "  Update Rate: " << calibParam.imu.update_rate << std::endl;

    return true;
}

#endif // __PRINT_HELPERS_H__