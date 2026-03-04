/**
 * @file fays_vikit.h
 * @brief Used to get sensor datas and set sensor properties.
 */

#pragma once


#include "fays_atrak_types.h"
#include <functional>


#if (defined (_WIN32) || defined(WIN64))
	#define FAYS_VIK_API __declspec(dllexport)
#else
    #define FAYS_VIK_API __attribute__((visibility("default")))
#endif



/****************************************************************************
 *  standard Fays ViKit SDK
 ****************************************************************************/

/*
* @brief Image callback function definition.
*/
using FAYS_VIK_ImageCallback = std::function<void(AtrakImage* pImg)>;

/*
* @brief IMU callback function definition.
*/
using FAYS_VIK_ImuCallback = std::function<void(const AtrakIMU&)>;

/**
 * @brief Open sensor .
 * 
 * @param[in] configuration The configuration file path.
 * @param[out] handle
 * @return Creation success or failure.
 */
FAYS_VIK_API      int     FAYS_VIK_CreateHandleWithConfig           (void** handle, const char* configPath);

/**
 * @brief Destroy the handle and turn off the sensors.
 * 
 * @param[out] handle
 * @return Destruction success or failure.
 */
FAYS_VIK_API      int     FAYS_VIK_DestroyHandle                    (void* handle);

/**
 * @brief Get device information.
 * 
 * @param[in] handle
 * @param[out] pInfo Device information.
 * @return Whether getting device information is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_GetDeviceInfo                  (void* handle, AtrakDeviceInfo* pInfo);

/**
 * @brief Get calibration parameters.
 * 
 * @param[in] handle
 * @param[out] pCalib Calibration parameters.
 * @return Whether getting calibration parameters is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_GetCalibrationParam            (void* handle, AtrakCalibrationParam* pCalib);

/**
 * @brief Get grayscale camera image (The image is stitched together).
 * 
 * @param[in] handle
 * @param[out] image Stereo camera grayscale image.
 * @return Whether the image acquisition is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_GetStereoFrames                  (void* handle, AtrakImage* pImage);

/**
 * @brief Set the Gain of the stereo camera.
 *
 * @param[in] handle
 * @param[in] value Target gain value.
 * @note Only supports setting gain value through root.
 * @note Gain range: 1.0 ~ 15.0.
 * @return Set whether gain is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetStereoGain                    (void* handle, float gainValue);

/**
 * @brief Set the exposure time of the stereo camera.
 * 
 * @param[in] handle
 * @param[in] value Target exposure value. 
 * @note Only supports setting exposure value through root.
 * @note Exposure range: 1.0 ~ 507.0.
 * @return Set whether exposure is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetStereoExposure                (void* handle, double exposureValue);

/**
 * @attention  Feature not open yet.
 * @brief Set the camera frame rate.
 * 
 * @param[in] handle
 * @note Only supports setting FPS through root.
 * @return Set whether FPS is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetStereoFPS                     (void* handle, int fps);

/**
    * @brief Register stereo image callback function.
    * 
    * @param[in] handle
    * @param[in] imgCallback Callback function for new images.
    * @return Registration success or failure.
*/
FAYS_VIK_API  int FAYS_VIK_RegisterStereoImageCallback          (void* handle, FAYS_VIK_ImageCallback imgCallback);

/**
    * @brief Get the RGB image.
    * 
    * @param[in] handle
    * @param[out] rgb RGB image data.
    * @return Get whether RGB image data is successful or not.
*/
FAYS_VIK_API      int     FAYS_VIK_GetRgbFrames                      (void* handle, AtrakImage* pImg);

/**
 * @brief Set the exposure time of the RGB camera.
 * 
 * @param[in] handle
 * @param[in] value Target exposure value. 
 * @note Only supports setting exposure value through root.
 * @note Exposure range: 8.0 ~ 2146.0.
 * @return Set whether exposure is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetRgbExposure                  (void* handle, double exposureValue);

/**
 * @brief Set the gain of the RGB camera.
 * 
 * @param[in] handle
 * @param[in] value Target gain value.
 * @note Only supports setting gain value through root.
 * @note Gain range: 1.0 ~ 72.0.
 * @return Set whether gain is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetRgbGain                      (void* handle, float gainValue);

/**
    * @brief Get the RGB image.
    * 
    * @param[in] handle
    * @param[out] rgb RGB image data.
    * @return Get whether RGB image data is successful or not.
*/
FAYS_VIK_API      int     FAYS_VIK_GetRgbFrames                      (void* handle, AtrakImage* pImg);

/**
 * @brief Set the exposure time of the RGB camera.
 * 
 * @param[in] handle
 * @param[in] value Target exposure value. 
 * @note Only supports setting exposure value through root.
 * @note Exposure range: 8.0 ~ 2146.0.
 * @return Set whether exposure is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetRgbExposure                  (void* handle, double exposureValue);

/**
 * @brief Set the gain of the RGB camera.
 * 
 * @param[in] handle
 * @param[in] value Target gain value.
 * @note Only supports setting gain value through root.
 * @note Gain range: 1.0 ~ 72.0.
 * @return Set whether gain is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_SetRgbGain                      (void* handle, float gainValue);

/**
 * @brief Register RGB image callback function.
 * 
 * @param[in] handle
 * @param[in] imgCallback Callback function for new images.
 * @return Registration success or failure.
 */
FAYS_VIK_API  int FAYS_VIK_RegisterRgbImageCallback             (void* handle, FAYS_VIK_ImageCallback imgCallback);

/**
 * @brief Get imu data.
 * 
 * @param[in] handle
 * @param[out] imu 6-axis imu data.
 * @return Get whether IMU data is successful or not.
 */
FAYS_VIK_API      int     FAYS_VIK_GetImuData                       (void* handle, AtrakIMU* pImu);

/**
 * @brief Get the version of the SDK.
 * 
 * @param[in] handle
 * @return The version of the SDK.
 */
FAYS_VIK_API const char* FAYS_VIK_GetVersion                        (void* handle);

/*
 * @brief Register IMU callback function.
 *
 * @param[in] handle
 * @param[in] imuCallback Callback function for new IMU data.
 * @return Registration success or failure.
 */
FAYS_VIK_API  int FAYS_VIK_RegisterImuCallback            (void* handle, FAYS_VIK_ImuCallback imuCallback);


/**
 * @brief Dump calibration parameters to a yaml file.
 * 
 * @param[in] handle The handle to the ViKit instance.
 * @param[in] outdir The output directory path.
 * @return Dump success or failure.
 */
FAYS_VIK_API  int FAYS_VIK_DumpCalib           (void* handle, const std::string& outdir = "./");
