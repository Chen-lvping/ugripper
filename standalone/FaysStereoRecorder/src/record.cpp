#include <string>
#include <thread>
#include <memory>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <cstddef>
#include <cmath>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <mcap/writer.hpp>
#include <limits.h>
#include <stdexcept>
#include <opencv2/imgproc.hpp>
#include "fays_atrak/fays_atrak_types.h"
#include "fays_atrak/fays_vikit.h"
#include "common/print_helpers.h"

namespace {
std::string TrimCopy(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string ReadYamlConfigValue(const std::string& configPath, const std::string& key) {
    std::ifstream in(configPath);
    if (!in.is_open()) {
        return "";
    }

    const std::string prefix = key + ":";
    std::string line;
    while (std::getline(in, line)) {
        const size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = TrimCopy(line);
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }

        std::string value = TrimCopy(line.substr(prefix.size()));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (value == "NULL" || value == "null") {
            return "";
        }
        return value;
    }

    return "";
}

std::string ErrnoToString(int err) {
    if (err == 0) {
        return "0";
    }
    std::ostringstream oss;
    oss << err << "(" << std::strerror(err) << ")";
    return oss.str();
}

bool ResolveDevicePath(const std::string& path, std::string* outResolved, int* outErrno = nullptr) {
    char resolvedPath[PATH_MAX];
    errno = 0;
    if (realpath(path.c_str(), resolvedPath) == nullptr) {
        if (outResolved != nullptr) {
            outResolved->clear();
        }
        if (outErrno != nullptr) {
            *outErrno = errno;
        }
        return false;
    }
    if (outResolved != nullptr) {
        *outResolved = std::string(resolvedPath);
    }
    if (outErrno != nullptr) {
        *outErrno = 0;
    }
    return true;
}

struct FaysConfigDevices {
    std::string stereoPath;
    std::string imuPath;
    std::string stereoResolved;
    std::string imuResolved;
};

bool LoadAndValidateConfigDevices(const std::string& configPath,
                                  FaysConfigDevices* outDevices,
                                  std::string* errorMessage) {
    const std::string stereoPath = ReadYamlConfigValue(configPath, "stereo_dev_port");
    const std::string imuPath = ReadYamlConfigValue(configPath, "imu_dev_port");
    if (stereoPath.empty() || imuPath.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "missing stereo_dev_port or imu_dev_port in config: " + configPath;
        }
        return false;
    }

    std::string stereoResolved;
    std::string imuResolved;
    int stereoErrno = 0;
    int imuErrno = 0;
    if (!ResolveDevicePath(stereoPath, &stereoResolved, &stereoErrno)) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot resolve stereo_dev_port=" + stereoPath +
                            " from config=" + configPath +
                            ", errno=" + ErrnoToString(stereoErrno);
        }
        return false;
    }
    if (!ResolveDevicePath(imuPath, &imuResolved, &imuErrno)) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot resolve imu_dev_port=" + imuPath +
                            " from config=" + configPath +
                            ", errno=" + ErrnoToString(imuErrno);
        }
        return false;
    }
    if (stereoResolved == imuResolved) {
        if (errorMessage != nullptr) {
            *errorMessage = "stereo_dev_port and imu_dev_port resolve to the same node: " + stereoResolved;
        }
        return false;
    }

    if (outDevices != nullptr) {
        outDevices->stereoPath = stereoPath;
        outDevices->imuPath = imuPath;
        outDevices->stereoResolved = stereoResolved;
        outDevices->imuResolved = imuResolved;
    }
    return true;
}

bool WriteSdkResolvedConfig(const std::string& configPath,
                            const FaysConfigDevices& devices,
                            std::string* sdkConfigPath,
                            std::string* tempConfigPath,
                            std::string* errorMessage) {
    if (sdkConfigPath == nullptr || tempConfigPath == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "resolved config output pointer is null";
        }
        return false;
    }

    std::map<std::string, std::string> replacements;
    replacements["stereo_dev_port"] = devices.stereoResolved;
    replacements["imu_dev_port"] = devices.imuResolved;

    const std::string rgbPath = ReadYamlConfigValue(configPath, "rgb_dev_port");
    if (!rgbPath.empty()) {
        std::string rgbResolved;
        int rgbErrno = 0;
        if (!ResolveDevicePath(rgbPath, &rgbResolved, &rgbErrno)) {
            if (errorMessage != nullptr) {
                *errorMessage = "cannot resolve rgb_dev_port=" + rgbPath +
                                " from config=" + configPath +
                                ", errno=" + ErrnoToString(rgbErrno);
            }
            return false;
        }
        replacements["rgb_dev_port"] = rgbResolved;
    }

    bool needsTempConfig = devices.stereoPath != devices.stereoResolved ||
                           devices.imuPath != devices.imuResolved ||
                           (!rgbPath.empty() && replacements["rgb_dev_port"] != rgbPath);
    if (!needsTempConfig) {
        *sdkConfigPath = configPath;
        tempConfigPath->clear();
        return true;
    }

    std::ifstream in(configPath);
    if (!in.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot open config for resolved SDK copy: " + configPath;
        }
        return false;
    }

    char tempTemplate[] = "/tmp/ugripper_fays_resolved_XXXXXX";
    const int fd = mkstemp(tempTemplate);
    if (fd < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "mkstemp failed for resolved SDK config: " + ErrnoToString(errno);
        }
        return false;
    }

    FILE* tempFile = fdopen(fd, "w");
    if (tempFile == nullptr) {
        const int savedErrno = errno;
        close(fd);
        unlink(tempTemplate);
        if (errorMessage != nullptr) {
            *errorMessage = "fdopen failed for resolved SDK config: " + ErrnoToString(savedErrno);
        }
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string withoutComment = line;
        const size_t commentPos = withoutComment.find('#');
        if (commentPos != std::string::npos) {
            withoutComment = withoutComment.substr(0, commentPos);
        }
        const std::string trimmed = TrimCopy(withoutComment);

        bool replaced = false;
        for (const auto& entry : replacements) {
            const std::string prefix = entry.first + ":";
            if (trimmed.rfind(prefix, 0) == 0) {
                std::fprintf(tempFile, "%s: %s\n", entry.first.c_str(), entry.second.c_str());
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            std::fprintf(tempFile, "%s\n", line.c_str());
        }
    }

    if (std::fclose(tempFile) != 0) {
        const int savedErrno = errno;
        unlink(tempTemplate);
        if (errorMessage != nullptr) {
            *errorMessage = "failed to close resolved SDK config: " + ErrnoToString(savedErrno);
        }
        return false;
    }

    *sdkConfigPath = tempTemplate;
    *tempConfigPath = tempTemplate;
    std::cout << "[FaysConfig] SDK config resolved from " << configPath
              << " to " << *sdkConfigPath << std::endl;
    std::cout << "[FaysConfig] SDK stereo_dev_port: " << devices.stereoResolved << std::endl;
    std::cout << "[FaysConfig] SDK imu_dev_port: " << devices.imuResolved << std::endl;
    return true;
}

bool SameResolvedDevices(const FaysConfigDevices& lhs, const FaysConfigDevices& rhs) {
    return lhs.stereoResolved == rhs.stereoResolved &&
           lhs.imuResolved == rhs.imuResolved;
}

uint64_t ReadEnvUInt64(const char* name, uint64_t defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return defaultValue;
    }
    return static_cast<uint64_t>(parsed);
}

void CleanupSdkTempConfig(std::string* sdkConfigTempPath) {
    if (sdkConfigTempPath != nullptr && !sdkConfigTempPath->empty()) {
        unlink(sdkConfigTempPath->c_str());
        sdkConfigTempPath->clear();
    }
}

bool CreateStableFaysHandle(const std::string& configPath,
                            void** outHandle,
                            FaysConfigDevices* outDevices,
                            std::string* outSdkConfigPath,
                            std::string* outSdkConfigTempPath,
                            std::string* errorMessage) {
    if (outHandle == nullptr || outDevices == nullptr ||
        outSdkConfigPath == nullptr || outSdkConfigTempPath == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "CreateStableFaysHandle output pointer is null";
        }
        return false;
    }

    *outHandle = nullptr;
    outSdkConfigPath->clear();
    outSdkConfigTempPath->clear();

    std::string lastError;
    FaysConfigDevices devices;
    if (!LoadAndValidateConfigDevices(configPath, &devices, &lastError)) {
        if (errorMessage != nullptr) {
            *errorMessage = lastError;
        }
        return false;
    }

    std::string sdkConfigPath;
    std::string sdkConfigTempPath;
    if (!WriteSdkResolvedConfig(configPath, devices, &sdkConfigPath, &sdkConfigTempPath, &lastError)) {
        if (errorMessage != nullptr) {
            *errorMessage = lastError;
        }
        return false;
    }

    void* handle = nullptr;
    const int createRc = FAYS_VIK_CreateHandleWithConfig(&handle, sdkConfigPath.c_str());
    if (createRc != EXIT_SUCCESS || handle == nullptr) {
        CleanupSdkTempConfig(&sdkConfigTempPath);
        std::ostringstream oss;
        oss << "FAYS_VIK_CreateHandleWithConfig failed for config=" << configPath
            << ", sdk_config=" << sdkConfigPath
            << ", rc=" << createRc << ", handle=" << handle;
        if (errorMessage != nullptr) {
            *errorMessage = oss.str();
        }
        return false;
    }

    FaysConfigDevices currentDevices;
    std::string currentError;
    const bool currentOk = LoadAndValidateConfigDevices(configPath, &currentDevices, &currentError);
    if (!currentOk || !SameResolvedDevices(devices, currentDevices)) {
        std::ostringstream oss;
        oss << "device node remapped during SDK handle creation: config=" << configPath
            << ", stereo_before=" << devices.stereoResolved
            << ", imu_before=" << devices.imuResolved
            << ", stereo_after=" << (currentOk ? currentDevices.stereoResolved : "<unresolved>")
            << ", imu_after=" << (currentOk ? currentDevices.imuResolved : "<unresolved>")
            << ", error_after=" << (currentOk ? "" : currentError)
            << ". Exit this process and let the outer daemon restart with current /dev/videoN.";
        CleanupSdkTempConfig(&sdkConfigTempPath);
        if (errorMessage != nullptr) {
            *errorMessage = oss.str();
        }
        return false;
    }

    *outHandle = handle;
    *outDevices = devices;
    *outSdkConfigPath = sdkConfigPath;
    *outSdkConfigTempPath = sdkConfigTempPath;
    return true;
}

const char* CameraModelName(ATRAK_CAM_MODEL model) {
    switch (model) {
        case ACM_PINHOLE: return "pinhole";
        case ACM_UNKNOWN: return "unknown";
    }
    return "unknown";
}

const char* DistortionModelName(ATRAK_DISTORTION_MODEL model) {
    switch (model) {
        case ADM_NONE: return "ADM_NONE";
        case ADM_KB4: return "ADM_KB4";
        case ADM_RADTAN: return "ADM_RADTAN";
        case ADM_BROWN_CONRADY: return "ADM_BROWN_CONRADY";
    }
    return "UNKNOWN";
}

bool CalibrationLooksValid(const AtrakCalibrationParam& calib) {
    if (calib.cameras.num_of_cams < 2 || calib.cameras.num_of_cams > FAYS_ATRAK_MAX_CAMERAS) {
        return false;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        const AtrakCamParam& cam = calib.cameras.cameras[i];
        if (cam.intrinsics.width == 0 || cam.intrinsics.height == 0) {
            return false;
        }
        if (!std::isfinite(cam.intrinsics.fx) || !std::isfinite(cam.intrinsics.fy) ||
            cam.intrinsics.fx <= 0.0f || cam.intrinsics.fy <= 0.0f) {
            return false;
        }
    }
    return true;
}

std::string ExtractJsonStringValue(const std::string& content, const std::string& key) {
    const std::string quotedKey = "\"" + key + "\"";
    const size_t keyPos = content.find(quotedKey);
    if (keyPos == std::string::npos) {
        return "";
    }
    const size_t colonPos = content.find(':', keyPos + quotedKey.size());
    if (colonPos == std::string::npos) {
        return "";
    }

    size_t valuePos = colonPos + 1;
    while (valuePos < content.size() && std::isspace(static_cast<unsigned char>(content[valuePos]))) {
        ++valuePos;
    }
    if (valuePos >= content.size() || content[valuePos] != '"') {
        return "";
    }
    ++valuePos;

    std::string value;
    bool escaped = false;
    for (; valuePos < content.size(); ++valuePos) {
        const char ch = content[valuePos];
        if (escaped) {
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return "";
}

std::string ReadCalibrationJsonSerial(const std::string& path, std::string* errorMessage) {
    std::ifstream in(path);
    if (!in.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot open cached calibration json: " + path;
        }
        return "";
    }
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string serial = TrimCopy(ExtractJsonStringValue(content, "serial_number"));
    if (serial.empty() && errorMessage != nullptr) {
        *errorMessage = "cached calibration json has empty serial_number: " + path;
    }
    return serial;
}

std::string GetFaysDeviceSerial(void* handle, std::string* errorMessage) {
    if (handle == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Fays handle is null";
        }
        return "";
    }

    ViKitDeviceInfo info{};
    const int rc = FAYS_VIK_GetDeviceInfo(handle, &info);
    if (rc != EXIT_SUCCESS) {
        if (errorMessage != nullptr) {
            std::ostringstream oss;
            oss << "FAYS_VIK_GetDeviceInfo failed, rc=" << rc;
            *errorMessage = oss.str();
        }
        return "";
    }

    const std::string serial = TrimCopy(info.serial_number);
    if (serial.empty() && errorMessage != nullptr) {
        *errorMessage = "Fays device serial is empty";
    }
    return serial;
}

void WriteFloatArray(std::ostream& out, const float* values, size_t count) {
    out << "[";
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << values[i];
    }
    out << "]";
}

void WriteTransform4x4(std::ostream& out, const AtrakExtrinsics& transform, int indentSpaces) {
    const std::string indent(indentSpaces, ' ');
    const std::string rowIndent(indentSpaces + 4, ' ');
    out << "[\n";
    for (int row = 0; row < 3; ++row) {
        out << rowIndent << "["
            << transform.rot[row * 3 + 0] << ", "
            << transform.rot[row * 3 + 1] << ", "
            << transform.rot[row * 3 + 2] << ", "
            << transform.trans[row] << "],\n";
    }
    out << rowIndent << "[0, 0, 0, 1]\n";
    out << indent << "]";
}

void WriteCameraJson(std::ostream& out, const AtrakCamParam& cam, const char* name, bool last) {
    out << "      \"" << name << "\": {\n";
    out << "        \"cam_id\": " << static_cast<int>(cam.cam_id) << ",\n";
    out << "        \"available_mask\": " << static_cast<int>(cam.available_mask) << ",\n";
    out << "        \"camera_model\": \"" << CameraModelName(cam.intrinsics.cam_model) << "\",\n";
    out << "        \"camera_model_enum\": " << static_cast<int>(cam.intrinsics.cam_model) << ",\n";
    out << "        \"distortion_model\": \"" << DistortionModelName(cam.intrinsics.distortion_model) << "\",\n";
    out << "        \"resolution\": [" << cam.intrinsics.width << ", " << cam.intrinsics.height << "],\n";
    out << "        \"intrinsics\": {\n";
    out << "          \"" << cam.intrinsics.width << "x" << cam.intrinsics.height << "\": {\n";
    out << "            \"fx\": " << cam.intrinsics.fx << ",\n";
    out << "            \"fy\": " << cam.intrinsics.fy << ",\n";
    out << "            \"ppx\": " << cam.intrinsics.cx << ",\n";
    out << "            \"ppy\": " << cam.intrinsics.cy << "\n";
    out << "          }\n";
    out << "        },\n";
    out << "        \"distortion_coeffs\": ";
    WriteFloatArray(out, cam.intrinsics.distortion, 8);
    out << ",\n";
    out << "        \"T_cn_imu\": ";
    WriteTransform4x4(out, cam.T_cn_imu, 8);
    out << ",\n";
    out << "        \"T_cn_cnm1\": ";
    WriteTransform4x4(out, cam.T_cn_cnm1, 8);
    out << ",\n";
    out << "        \"timeshift_cam_imu\": " << cam.timeshift_cam_imu << "\n";
    out << "      }" << (last ? "\n" : ",\n");
}

bool WriteCalibrationJsonFromHandle(void* handle,
                                    const std::string& configPath,
                                    const std::string& outputPath,
                                    std::string* errorMessage) {
    FaysConfigDevices devices;
    if (!LoadAndValidateConfigDevices(configPath, &devices, errorMessage)) {
        return false;
    }

    std::cout << "[FaysConfig] Config: " << configPath << std::endl;
    std::cout << "[FaysConfig] stereo_dev_port: " << devices.stereoPath
              << " -> " << devices.stereoResolved << std::endl;
    std::cout << "[FaysConfig] imu_dev_port: " << devices.imuPath
              << " -> " << devices.imuResolved << std::endl;

    if (handle == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Fays handle is null for config=" + configPath;
        }
        return false;
    }

    ViKitDeviceInfo info{};
    AtrakCalibrationParam calib{};
    const char* versionPtr = FAYS_VIK_GetVersion(handle);
    const std::string sdkVersion = versionPtr != nullptr ? versionPtr : "";
    const int infoRc = FAYS_VIK_GetDeviceInfo(handle, &info);
    const int calibRc = FAYS_VIK_GetCalibrationParam(handle, &calib);

    if (infoRc != EXIT_SUCCESS) {
        if (errorMessage != nullptr) {
            *errorMessage = "FAYS_VIK_GetDeviceInfo failed for config=" + configPath;
        }
        return false;
    }
    if (calibRc != EXIT_SUCCESS || !CalibrationLooksValid(calib)) {
        if (errorMessage != nullptr) {
            std::ostringstream oss;
            oss << "FAYS_VIK_GetCalibrationParam failed or invalid for config=" << configPath
                << ", rc=" << calibRc
                << ", num_of_cams=" << calib.cameras.num_of_cams;
            *errorMessage = oss.str();
        }
        return false;
    }

    std::ofstream out(outputPath);
    if (!out.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot open calibration output for write: " + outputPath;
        }
        return false;
    }

    out << std::setprecision(9);
    out << "{\n";
    out << "  \"valid\": true,\n";
    out << "  \"schema\": \"ugripper_fays_calibration_v1\",\n";
    out << "  \"source_config\": \"" << JsonEscape(configPath) << "\",\n";
    out << "  \"devices\": {\n";
    out << "    \"stereo_dev_port\": \"" << JsonEscape(devices.stereoPath) << "\",\n";
    out << "    \"stereo_resolved\": \"" << JsonEscape(devices.stereoResolved) << "\",\n";
    out << "    \"imu_dev_port\": \"" << JsonEscape(devices.imuPath) << "\",\n";
    out << "    \"imu_resolved\": \"" << JsonEscape(devices.imuResolved) << "\"\n";
    out << "  },\n";
    out << "  \"device_info\": {\n";
    out << "    \"device_model\": \"" << JsonEscape(info.device_model) << "\",\n";
    out << "    \"serial_number\": \"" << JsonEscape(info.serial_number) << "\",\n";
    out << "    \"firmware_version\": \"" << JsonEscape(info.firmware_version) << "\",\n";
    out << "    \"camera_nums\": " << info.camera_nums << ",\n";
    out << "    \"imu_nums\": " << info.imu_nums << ",\n";
    out << "    \"sdk_version\": \"" << JsonEscape(sdkVersion) << "\"\n";
    out << "  },\n";
    out << "  \"shape\": [400, 1280, 3],\n";
    out << "  \"names\": [\"height\", \"width\", \"channels\"],\n";
    out << "  \"info\": null,\n";
    out << "  \"camera_model\": \"pinhole\",\n";
    out << "  \"distortion_model\": \"" << DistortionModelName(calib.cameras.cameras[0].intrinsics.distortion_model) << "\",\n";
    out << "  \"downsize_ratio\": " << calib.cameras.downsize_ratio << ",\n";
    out << "  \"cam0\": {\n";
    out << "    \"camera_model_enum\": " << static_cast<int>(calib.cameras.cameras[0].intrinsics.cam_model) << ",\n";
    out << "    \"intrinsics\": {\n";
    out << "      \"" << calib.cameras.cameras[0].intrinsics.width << "x" << calib.cameras.cameras[0].intrinsics.height << "\": {\n";
    out << "        \"fx\": " << calib.cameras.cameras[0].intrinsics.fx << ",\n";
    out << "        \"fy\": " << calib.cameras.cameras[0].intrinsics.fy << ",\n";
    out << "        \"ppx\": " << calib.cameras.cameras[0].intrinsics.cx << ",\n";
    out << "        \"ppy\": " << calib.cameras.cameras[0].intrinsics.cy << "\n";
    out << "      }\n";
    out << "    },\n";
    out << "    \"distortion_model\": \"" << DistortionModelName(calib.cameras.cameras[0].intrinsics.distortion_model) << "\",\n";
    out << "    \"distortion_coeffs\": ";
    WriteFloatArray(out, calib.cameras.cameras[0].intrinsics.distortion, 8);
    out << "\n";
    out << "  },\n";
    out << "  \"cam1\": {\n";
    out << "    \"camera_model_enum\": " << static_cast<int>(calib.cameras.cameras[1].intrinsics.cam_model) << ",\n";
    out << "    \"intrinsics\": {\n";
    out << "      \"" << calib.cameras.cameras[1].intrinsics.width << "x" << calib.cameras.cameras[1].intrinsics.height << "\": {\n";
    out << "        \"fx\": " << calib.cameras.cameras[1].intrinsics.fx << ",\n";
    out << "        \"fy\": " << calib.cameras.cameras[1].intrinsics.fy << ",\n";
    out << "        \"ppx\": " << calib.cameras.cameras[1].intrinsics.cx << ",\n";
    out << "        \"ppy\": " << calib.cameras.cameras[1].intrinsics.cy << "\n";
    out << "      }\n";
    out << "    },\n";
    out << "    \"distortion_model\": \"" << DistortionModelName(calib.cameras.cameras[1].intrinsics.distortion_model) << "\",\n";
    out << "    \"distortion_coeffs\": ";
    WriteFloatArray(out, calib.cameras.cameras[1].intrinsics.distortion, 8);
    out << "\n";
    out << "  },\n";
    out << "  \"extrinsics\": {\n";
    out << "    \"T_ic_cam0_to_imu0\": ";
    WriteTransform4x4(out, calib.cameras.cameras[0].T_cn_imu, 4);
    out << ",\n";
    out << "    \"timeshift_cam0_to_imu0\": " << calib.cameras.cameras[0].timeshift_cam_imu << ",\n";
    out << "    \"T_ic_cam1_to_imu0\": ";
    WriteTransform4x4(out, calib.cameras.cameras[1].T_cn_imu, 4);
    out << ",\n";
    out << "    \"timeshift_cam1_to_imu0\": " << calib.cameras.cameras[1].timeshift_cam_imu << "\n";
    out << "  },\n";
    out << "  \"cameras\": {\n";
    WriteCameraJson(out, calib.cameras.cameras[0], "camera0", false);
    WriteCameraJson(out, calib.cameras.cameras[1], "camera1", true);
    out << "  },\n";
    out << "  \"imu\": {\n";
    out << "    \"dtype\": \"imu\",\n";
    out << "    \"model\": \"fays_vikit\",\n";
    out << "    \"accelerometer_noise_density\": " << calib.imu.accelerometer_noise_density << ",\n";
    out << "    \"accelerometer_random_walk\": " << calib.imu.accelerometer_random_walk << ",\n";
    out << "    \"gyroscope_noise_density\": " << calib.imu.gyroscope_noise_density << ",\n";
    out << "    \"gyroscope_random_walk\": " << calib.imu.gyroscope_random_walk << ",\n";
    out << "    \"update_rate\": " << calib.imu.update_rate << "\n";
    out << "  },\n";
    out << "  \"dtype\": \"video\",\n";
    out << "  \"fps\": 25\n";
    out << "}\n";

    if (!out.good()) {
        if (errorMessage != nullptr) {
            *errorMessage = "failed while writing calibration output: " + outputPath;
        }
        return false;
    }

    std::cout << "[FaysCalibration] Wrote " << outputPath
              << " serial=" << info.serial_number
              << " firmware=" << info.firmware_version
              << " sdk=" << sdkVersion << std::endl;
    return true;
}

bool DumpCalibrationJson(const std::string& configPath,
                         const std::string& outputPath,
                         std::string* errorMessage) {
    FaysConfigDevices devices;
    std::string sdkConfigPath;
    std::string tempConfigPath;
    void* handle = nullptr;
    if (!CreateStableFaysHandle(configPath, &handle, &devices, &sdkConfigPath, &tempConfigPath, errorMessage)) {
        return false;
    }

    const bool ok = WriteCalibrationJsonFromHandle(handle, configPath, outputPath, errorMessage);
    CleanupSdkTempConfig(&tempConfigPath);
    // The vendor SDK can block in DestroyHandle when used as a short-lived
    // calibration probe without starting the streaming threads. Keep this dump
    // path one-shot and let process teardown release the device after JSON is
    // flushed.
    return ok;
}

std::string ReadCameraCodecFromEnvironmentFile() {
    const std::string defaultCodec = "h264";
    std::ifstream envFile("/etc/environment");
    if (!envFile.is_open()) {
        return defaultCodec;
    }

    std::string line;
    while (std::getline(envFile, line)) {
        line = TrimCopy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = ToLowerCopy(TrimCopy(line.substr(0, pos)));
        if (key != "camera_codec") {
            continue;
        }

        std::string value = TrimCopy(line.substr(pos + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        value = ToLowerCopy(TrimCopy(value));

        if (value == "h264" || value == "h265") {
            return value;
        }

        std::cerr << "[FFmpeg] WARNING: invalid CAMERA_CODEC='" << value
                  << "' in /etc/environment, fallback to h264" << std::endl;
        return defaultCodec;
    }

    return defaultCodec;
}
}  // namespace

struct FaysImuSample {
    double gx;
    double gy;
    double gz;
    double ax;
    double ay;
    double az;
};

struct FaysCamTsSample {
    uint32_t frameIndex;
};

static_assert(sizeof(FaysImuSample) == 48, "FaysImuSample layout changed");
static_assert(sizeof(FaysCamTsSample) == 4, "FaysCamTsSample layout changed");

struct ImuQueuedSample {
    AtrakIMU imu;
    uint64_t publishTimeNs;
    uint64_t sessionId;
};

struct CamTsQueuedSample {
    uint64_t faysTsNs;
    uint64_t publishTimeNs;
    uint32_t frameIndex;
    uint64_t sessionId;
};

struct McapControlCommand {
    enum class Type {
        Start,
        Stop,
    };

    Type type;
    uint64_t sessionId;
    std::string mcapPath;
};

class ImuQueue {
public:
    ImuQueue() : stopped_(false), nextBacklogWarnSize_(8192) {}

    bool TryPushBatch(std::deque<ImuQueuedSample>& pending) {
        if (pending.empty()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(mtx_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }

        queue_.insert(
            queue_.end(),
            std::make_move_iterator(pending.begin()),
            std::make_move_iterator(pending.end()));
        pending.clear();

        if (queue_.size() >= nextBacklogWarnSize_) {
            std::cerr << "[ImuQueue] WARNING: backlog grew to " << queue_.size() << std::endl;
            nextBacklogWarnSize_ = queue_.size() + 4096;
        }
        cv_.notify_one();
        return true;
    }

    size_t DrainTo(std::vector<ImuQueuedSample>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        return DrainToLocked(out, maxCount);
    }

    size_t TryDrainTo(std::vector<ImuQueuedSample>& out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mtx_);
        return DrainToLocked(out, maxCount);
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    size_t DrainToLocked(std::vector<ImuQueuedSample>& out, size_t maxCount) {
        size_t count = queue_.size();
        if (count > maxCount) {
            count = maxCount;
        }
        if (count == 0) {
            return 0;
        }

        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(queue_.front());
            queue_.pop_front();
        }
        return count;
    }

    std::deque<ImuQueuedSample> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    size_t nextBacklogWarnSize_;
};

class CamTsQueue {
public:
    CamTsQueue() : stopped_(false), nextBacklogWarnSize_(2048) {}

    bool TryPushBatch(std::deque<CamTsQueuedSample>& pending) {
        if (pending.empty()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(mtx_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }

        queue_.insert(
            queue_.end(),
            std::make_move_iterator(pending.begin()),
            std::make_move_iterator(pending.end()));
        pending.clear();

        if (queue_.size() >= nextBacklogWarnSize_) {
            std::cerr << "[CamTsQueue] WARNING: backlog grew to " << queue_.size() << std::endl;
            nextBacklogWarnSize_ = queue_.size() + 1024;
        }
        cv_.notify_one();
        return true;
    }

    size_t DrainTo(std::vector<CamTsQueuedSample>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        return DrainToLocked(out, maxCount);
    }

    size_t TryDrainTo(std::vector<CamTsQueuedSample>& out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mtx_);
        return DrainToLocked(out, maxCount);
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    size_t DrainToLocked(std::vector<CamTsQueuedSample>& out, size_t maxCount) {
        size_t count = queue_.size();
        if (count > maxCount) {
            count = maxCount;
        }
        if (count == 0) {
            return 0;
        }

        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(queue_.front());
            queue_.pop_front();
        }
        return count;
    }

    std::deque<CamTsQueuedSample> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    size_t nextBacklogWarnSize_;
};

class McapControlQueue {
public:
    McapControlQueue() : stopped_(false) {}

    void PushStart(uint64_t sessionId, const std::string& mcapPath) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            McapControlCommand cmd{};
            cmd.type = McapControlCommand::Type::Start;
            cmd.sessionId = sessionId;
            cmd.mcapPath = mcapPath;
            queue_.push_back(std::move(cmd));
        }
        cv_.notify_one();
    }

    void PushStop(uint64_t sessionId) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            McapControlCommand cmd{};
            cmd.type = McapControlCommand::Type::Stop;
            cmd.sessionId = sessionId;
            queue_.push_back(std::move(cmd));
        }
        cv_.notify_one();
    }

    size_t DrainTo(std::vector<McapControlCommand>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        return DrainToLocked(out, maxCount);
    }

    size_t TryDrainTo(std::vector<McapControlCommand>& out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mtx_);
        return DrainToLocked(out, maxCount);
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    size_t DrainToLocked(std::vector<McapControlCommand>& out, size_t maxCount) {
        size_t count = queue_.size();
        if (count > maxCount) {
            count = maxCount;
        }
        if (count == 0) {
            return 0;
        }

        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return count;
    }

    std::deque<McapControlCommand> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
};

class FaysDataLogger {
public:
    FaysDataLogger() : writer_(std::make_unique<mcap::McapWriter>()), isOpen_(false), imuSeq_(0), camSeq_(0) {}

    ~FaysDataLogger() {
        Close();
    }

    bool Open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx_);

        if (isOpen_) {
            writer_->close();
            isOpen_ = false;
        }
        writer_ = std::make_unique<mcap::McapWriter>();

        mcap::McapWriterOptions options("fays_recorder");
        options.noChunking = false;
        options.chunkSize = 256 * 1024;
        options.compression = mcap::Compression::Lz4;
        options.compressionLevel = mcap::CompressionLevel::Default;
        options.forceCompression = false;
        options.noRepeatedSchemas = true;
        options.noRepeatedChannels = true;
        options.noMessageIndex = true;

        auto openStatus = writer_->open(path, options);
        if (!openStatus.ok()) {
            std::cerr << "[MCAP] Failed to open " << path << ": " << openStatus.message << std::endl;
            return false;
        }

        auto imuSchema = mcap::Schema(
            "fays.Imu", "jsonschema", R"({
                "type": "object",
                "title": "FaysImuBinary",
                "description": "little-endian float64[6]: gx,gy,gz,ax,ay,az"
            })");
        auto camSchema = mcap::Schema(
            "fays.CamTs", "jsonschema", R"({
                "type": "object",
                "title": "FaysCamTsBinary",
                "description": "little-endian uint32 frameIndex"
            })");

        writer_->addSchema(imuSchema);
        writer_->addSchema(camSchema);

        auto imuChannel = mcap::Channel("i", "binary", imuSchema.id);
        auto camChannel = mcap::Channel("c", "binary", camSchema.id);

        writer_->addChannel(imuChannel);
        writer_->addChannel(camChannel);

        imuChannelId_ = imuChannel.id;
        camChannelId_ = camChannel.id;
        imuSeq_ = 0;
        camSeq_ = 0;
        isOpen_ = true;
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (isOpen_) {
            writer_->close();
            isOpen_ = false;
        }
    }

    void LogImu(const AtrakIMU& imuData, uint64_t publishTimeNs) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!isOpen_) {
            return;
        }

        FaysImuSample sample{};
        sample.gx = imuData.gyro[0];
        sample.gy = imuData.gyro[1];
        sample.gz = imuData.gyro[2];
        sample.ax = imuData.acc[0];
        sample.ay = imuData.acc[1];
        sample.az = imuData.acc[2];

        mcap::Message msg;
        msg.channelId = imuChannelId_;
        msg.sequence = imuSeq_++;
        msg.logTime = imuData.timestamp;
        msg.publishTime = publishTimeNs;
        msg.data = reinterpret_cast<const std::byte*>(&sample);
        msg.dataSize = sizeof(sample);

        auto writeStatus = writer_->write(msg);
        if (!writeStatus.ok()) {
            std::cerr << "[MCAP] Failed to write IMU frame: " << writeStatus.message << std::endl;
        }
    }

    void LogCamTs(uint64_t faysTsNs, uint64_t publishTimeNs, uint32_t frameIndex) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!isOpen_) {
            return;
        }

        FaysCamTsSample sample{};
        sample.frameIndex = frameIndex;

        mcap::Message msg;
        msg.channelId = camChannelId_;
        msg.sequence = camSeq_++;
        msg.logTime = faysTsNs;
        msg.publishTime = publishTimeNs;
        msg.data = reinterpret_cast<const std::byte*>(&sample);
        msg.dataSize = sizeof(sample);

        auto writeStatus = writer_->write(msg);
        if (!writeStatus.ok()) {
            std::cerr << "[MCAP] Failed to write camera timestamp: " << writeStatus.message << std::endl;
        }
    }

private:
    std::unique_ptr<mcap::McapWriter> writer_;
    bool isOpen_;
    mcap::ChannelId imuChannelId_;
    mcap::ChannelId camChannelId_;
    uint32_t imuSeq_;
    uint32_t camSeq_;
    std::mutex mtx_;
};

struct VideoFrame {
    std::vector<uint8_t> bytes;
    int width = 0;
    int height = 0;
    int channels = 0;
    uint64_t faysTsNs;
    uint64_t publishTimeNs;
};

class VideoFrameQueue {
public:
    static constexpr size_t kDefaultCapacity = 128;

    explicit VideoFrameQueue(size_t capacity = kDefaultCapacity)
        : capacity_(capacity), stopped_(false), dropCount_(0) {}

    bool Push(VideoFrame&& frame) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stopped_) return false;
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            const uint64_t count = dropCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((count & (count - 1)) == 0 || count % 50 == 0) {
                std::cerr << "[Video] Encode queue full, dropped oldest frame (total drops: "
                          << count << ")" << std::endl;
            }
        }
        queue_.push_back(std::move(frame));
        cv_.notify_one();
        return true;
    }

    bool Pop(VideoFrame& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mtx_);
        stopped_ = true;
        cv_.notify_all();
    }

    uint64_t GetDropCount() const { return dropCount_.load(std::memory_order_relaxed); }

private:
    const size_t capacity_;
    std::deque<VideoFrame> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    std::atomic<uint64_t> dropCount_;
};

class GstRecorder {
public:
    GstRecorder() : pipeFd_(-1), childPid_(-1) {}

    ~GstRecorder() { Stop(); }

    bool Start(const std::string& savePath, int width, int height, int fps) {
        if (pipeFd_ >= 0 || childPid_ > 0) {
            return true;
        }

        const std::string cameraCodec = ReadCameraCodecFromEnvironmentFile();
        const std::string ffmpegEncoder = (cameraCodec == "h265") ? "hevc_rkmpp" : "h264_rkmpp";

        std::stringstream cmd;
        cmd << "ffmpeg -hide_banner -loglevel error -nostats -y "
            << "-thread_queue_size 512 "
            << "-f rawvideo -vcodec rawvideo "
            << "-pix_fmt bgr24 "
            << "-s " << width << "x" << height << " "
            << "-r " << fps << " "
            << "-i - "
            << "-c:v " << ffmpegEncoder << " "
            << "-rc_mode CQP "
            << "-qp_init 30 "
            << "-qp_max 38 "
            << "-qp_min 24 "
            << "-qp_max_i 38 "
            << "-qp_min_i 20 "
            << "\"" << savePath << "\"";

        std::cout << "[FFmpeg] CAMERA_CODEC=" << cameraCodec
                  << " -> " << ffmpegEncoder << std::endl;
        std::cout << "[FFmpeg] Command: " << cmd.str() << std::endl;

        int pipeFds[2] = {-1, -1};
        if (pipe(pipeFds) != 0) {
            std::cerr << "[FFmpeg] Failed to create pipe: " << std::strerror(errno) << std::endl;
            return false;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[FFmpeg] Failed to fork: " << std::strerror(errno) << std::endl;
            close(pipeFds[0]);
            close(pipeFds[1]);
            return false;
        }

        if (pid == 0) {
            setpgid(0, 0);
            close(pipeFds[1]);
            if (dup2(pipeFds[0], STDIN_FILENO) < 0) {
                _exit(127);
            }
            close(pipeFds[0]);
            execl("/bin/sh", "sh", "-c", cmd.str().c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        setpgid(pid, pid);
        close(pipeFds[0]);
        pipeFd_ = pipeFds[1];
        childPid_ = pid;

        constexpr int kTargetPipeSz = 1048576;
        const int actual = fcntl(pipeFd_, F_SETPIPE_SZ, kTargetPipeSz);
        if (actual > 0) {
            std::cout << "[FFmpeg] Pipe buffer expanded to " << actual << " bytes" << std::endl;
        }
        return true;
    }

    void Write(const uint8_t* data, size_t size) {
        if (pipeFd_ < 0 || data == nullptr || size == 0) {
            return;
        }
        const uint8_t* cursor = data;
        size_t remaining = size;
        while (remaining > 0) {
            const ssize_t written = write(pipeFd_, cursor, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EPIPE) {
                    std::cerr << "[FFmpeg] Encoder pipe closed." << std::endl;
                    return;
                }
                std::cerr << "[FFmpeg] Pipe write failed: " << std::strerror(errno) << std::endl;
                return;
            }
            if (written == 0) {
                return;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
    }

    void Stop() {
        if (pipeFd_ >= 0) {
            close(pipeFd_);
            pipeFd_ = -1;
        }
        if (childPid_ > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            int status = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t result = waitpid(childPid_, &status, WNOHANG);
                if (result == childPid_) {
                    childPid_ = -1;
                    std::cout << "[FFmpeg] Recording stopped." << std::endl;
                    return;
                }
                if (result < 0 && errno == ECHILD) {
                    childPid_ = -1;
                    std::cout << "[FFmpeg] Recording stopped." << std::endl;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            std::cerr << "[FFmpeg] Encoder did not exit after stdin close; sending SIGTERM." << std::endl;
            kill(-childPid_, SIGTERM);
            const auto termDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < termDeadline) {
                const pid_t result = waitpid(childPid_, &status, WNOHANG);
                if (result == childPid_ || (result < 0 && errno == ECHILD)) {
                    childPid_ = -1;
                    std::cout << "[FFmpeg] Recording stopped." << std::endl;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            std::cerr << "[FFmpeg] Encoder did not exit after SIGTERM; sending SIGKILL." << std::endl;
            kill(-childPid_, SIGKILL);
            waitpid(childPid_, &status, 0);
            childPid_ = -1;
            std::cout << "[FFmpeg] Recording stopped." << std::endl;
        }
    }

private:
    int pipeFd_;
    pid_t childPid_;
};

class FaysRecorder;
static FaysRecorder* g_recorder = nullptr;
static volatile std::sig_atomic_t g_stopRequested = 0;
void signalHandler(int signal);

class FaysRecorder {
public:
    FaysRecorder(const char* configPath,
                 std::string videoFileName,
                 std::string mcapFileName,
                 std::string calibrationJsonPath,
                 std::string statusJsonPath)
        : mptrHandle_(nullptr),
          mbIsRunning_(true),
          recordingEnabled_(false),
          recordingSessionId_(0),
          recordingSessionSeed_(0),
          configPath_(configPath),
          videoFileName_(std::move(videoFileName)),
          mcapFileName_(std::move(mcapFileName)),
          calibrationJsonPath_(std::move(calibrationJsonPath)),
          statusJsonPath_(std::move(statusJsonPath)),
          calibrationDumped_(false),
          lastImuTimestamp_(0),
          lastImgTimestamp_(0),
          lastFrameSystemNs_(0),
          lastFrameFaysNs_(0),
          lastEncodedFrameSystemNs_(0),
          lastEncodedFrameFaysNs_(0),
          lastRuntimeStatusWriteNs_(0),
          imuGapCount_(0),
          imuRollbackCount_(0),
          imuToSysOffsetNs_(0),
          hasImuTimeOffset_(false) {
        statusIntervalNs_ = ReadEnvUInt64("FAYS_RUNTIME_STATUS_INTERVAL_MS", 1000) * 1000000ULL;
        mImgData_.data = new uchar[FAYS_ATRAK_MONO_MAX_BYTES * 3];

        FaysConfigDevices configDevices;
        std::string configError;
        if (!CreateStableFaysHandle(
                configPath_, &mptrHandle_, &configDevices, &sdkConfigPath_, &sdkConfigTempPath_, &configError)) {
            delete[] mImgData_.data;
            mImgData_.data = nullptr;
            throw std::runtime_error(configError);
        }
        std::cout << "[FaysConfig] Config: " << configPath << std::endl;
        std::cout << "[FaysConfig] stereo_dev_port: " << configDevices.stereoPath
                  << " -> " << configDevices.stereoResolved << std::endl;
        std::cout << "[FaysConfig] imu_dev_port: " << configDevices.imuPath
                  << " -> " << configDevices.imuResolved << std::endl;
        PrintDeviceInfo(mptrHandle_);
        PrintCalibrationInfo(mptrHandle_);

        LoadMonitoredDevicePaths(configPath);
        const std::string fpsStr = ReadConfigValue(configPath, "stereo_fps");
        recordFps_ = fpsStr.empty() ? 25 : std::stoi(fpsStr);
        struct stat calibStat {};
        if (!calibrationJsonPath_.empty() &&
            stat(calibrationJsonPath_.c_str(), &calibStat) == 0 &&
            calibStat.st_size > 0) {
            std::string currentSerialError;
            const std::string currentSerial = GetFaysDeviceSerial(mptrHandle_, &currentSerialError);
            std::string cachedSerialError;
            const std::string cachedSerial = ReadCalibrationJsonSerial(calibrationJsonPath_, &cachedSerialError);
            if (!currentSerial.empty() && !cachedSerial.empty() && currentSerial == cachedSerial) {
                calibrationDumped_.store(true, std::memory_order_release);
                std::cout << "[FaysCalibration] Using pre-dumped calibration json: "
                          << calibrationJsonPath_
                          << " serial=" << cachedSerial << std::endl;
            } else {
                std::cout << "[FaysCalibration] Discarding cached calibration json: "
                          << calibrationJsonPath_
                          << " cached_serial=" << (cachedSerial.empty() ? "<empty>" : cachedSerial)
                          << " current_serial=" << (currentSerial.empty() ? "<empty>" : currentSerial)
                          << " cached_error=" << cachedSerialError
                          << " current_error=" << currentSerialError
                          << std::endl;
                unlink(calibrationJsonPath_.c_str());
            }
        }
        if (!calibrationDumped_.load(std::memory_order_acquire)) {
            TryDumpCalibrationJson("startup");
        }
        std::cout << "[FaysRecorder] Created handle with config: " << configPath << std::endl;
        std::cout << "[FaysRecorder] Standby mode ready. Waiting for START command." << std::endl;

        mptrImuThr_ = std::thread(&FaysRecorder::ImuOnlineCapture, this);
        mptrMcapWriterThr_ = std::thread(&FaysRecorder::McapWriteThread, this);
        mptrImgThr_ = std::thread(&FaysRecorder::ImgOnlineCapture, this);
        mptrEncThr_ = std::thread(&FaysRecorder::VideoEncodeThread, this);
        mptrUsbWatchThr_ = std::thread(&FaysRecorder::UsbConnectionWatchdog, this);
    }

    ~FaysRecorder() {
        StopRecordingSession();
        mbIsRunning_ = false;

        if (mptrImuThr_.joinable()) {
            mptrImuThr_.join();
        }
        if (mptrImgThr_.joinable()) {
            mptrImgThr_.join();
        }

        videoFrameQueue_.Stop();
        if (mptrEncThr_.joinable()) {
            mptrEncThr_.join();
        }

        if (mptrUsbWatchThr_.joinable()) {
            mptrUsbWatchThr_.join();
        }

        imuQueue_.NotifyStop();
        camTsQueue_.NotifyStop();
        mcapControlQueue_.NotifyStop();

        if (mptrMcapWriterThr_.joinable()) {
            mptrMcapWriterThr_.join();
        }

        mRecorder_.Stop();

        std::cout << "[IMU] Final stats - Gaps: " << imuGapCount_
                  << ", Rollbacks: " << imuRollbackCount_ << std::endl;
        std::cout << "[Video] Final stats - Encode queue drops: "
                  << videoFrameQueue_.GetDropCount() << std::endl;

        FAYS_VIK_DestroyHandle(mptrHandle_);
        std::cout << "[FaysRecorder] Destroyed handle" << std::endl;
        CleanupSdkTempConfig(&sdkConfigTempPath_);
        delete[] mImgData_.data;
        std::cout << "[FaysRecorder] Deleted image data" << std::endl;
    }

    bool IsRunning() const { return mbIsRunning_; }

    void Stop() {
        mbIsRunning_.store(false, std::memory_order_release);
        StopRecordingSession();
        videoFrameQueue_.Stop();
        imuQueue_.NotifyStop();
        camTsQueue_.NotifyStop();
        mcapControlQueue_.NotifyStop();
    }

    void StartRecording(const std::string& outputDir) {
        std::lock_guard<std::mutex> lock(recordingMtx_);
        const std::string normalizedOutputDir = NormalizeOutputDir(outputDir);
        const std::string mcapPath = normalizedOutputDir + mcapFileName_;

        recordingOutputDir_ = normalizedOutputDir;
        const uint64_t sessionId = recordingSessionSeed_.fetch_add(1, std::memory_order_relaxed) + 1;
        recordingSessionId_.store(sessionId, std::memory_order_release);
        recordingEnabled_.store(true, std::memory_order_release);
        hasImuTimeOffset_.store(false, std::memory_order_release);
        mcapControlQueue_.PushStart(sessionId, mcapPath);

        std::cout << "[Control] START recording. Output directory: " << recordingOutputDir_ << std::endl;
        std::cout << "[Control] MCAP output: " << mcapPath << std::endl;
        WriteRuntimeStatus(true);
    }

    void StopRecordingSession() {
        bool wasRecording = recordingEnabled_.exchange(false, std::memory_order_acq_rel);
        const uint64_t stoppedSessionId = recordingSessionId_.exchange(0, std::memory_order_acq_rel);
        if (wasRecording) {
            std::cout << "[Control] STOP recording." << std::endl;
        }
        mcapControlQueue_.PushStop(stoppedSessionId);
        WriteRuntimeStatus(true);
    }

private:
    struct UsbWatchdogStatus {
        size_t consecutiveFailures = 0;
        std::chrono::steady_clock::time_point firstFailureTs;
        std::string lastFailureSignature;
    };

    static constexpr int kUsbWatchdogPollIntervalMs = 100;
    static constexpr int kUsbDisconnectDebounceMs = 500;

    static std::string TrimCopy(const std::string& input) {
        const std::string whitespace = " \t\r\n";
        const size_t start = input.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const size_t end = input.find_last_not_of(whitespace);
        return input.substr(start, end - start + 1);
    }

    static std::string ReadConfigValue(const std::string& configPath, const std::string& key) {
        std::ifstream in(configPath);
        if (!in.is_open()) {
            std::cerr << "[USB] Failed to open config file for USB watch: " << configPath << std::endl;
            return "";
        }

        const std::string prefix = key + ":";
        std::string line;
        while (std::getline(in, line)) {
            const size_t commentPos = line.find('#');
            if (commentPos != std::string::npos) {
                line = line.substr(0, commentPos);
            }

            line = TrimCopy(line);
            if (line.rfind(prefix, 0) != 0) {
                continue;
            }

            std::string value = TrimCopy(line.substr(prefix.size()));
            if (value == "NULL" || value == "null") {
                return "";
            }
            return value;
        }

        return "";
    }

    static std::string ErrnoToString(int err) {
        if (err == 0) {
            return "0";
        }
        std::ostringstream oss;
        oss << err << "(" << std::strerror(err) << ")";
        return oss.str();
    }

    static std::string ReadSymlinkTarget(const std::string& path, int* outErrno = nullptr) {
        char linkTarget[PATH_MAX];
        errno = 0;
        const ssize_t len = readlink(path.c_str(), linkTarget, sizeof(linkTarget) - 1);
        if (len < 0) {
            if (outErrno != nullptr) {
                *outErrno = errno;
            }
            return "";
        }

        linkTarget[len] = '\0';
        if (outErrno != nullptr) {
            *outErrno = 0;
        }
        return std::string(linkTarget);
    }

    static bool ResolveDevicePath(const std::string& path, std::string* outResolved, int* outErrno = nullptr) {
        char resolvedPath[PATH_MAX];
        errno = 0;
        if (realpath(path.c_str(), resolvedPath) == nullptr) {
            if (outResolved != nullptr) {
                outResolved->clear();
            }
            if (outErrno != nullptr) {
                *outErrno = errno;
            }
            return false;
        }
        if (outResolved != nullptr) {
            *outResolved = std::string(resolvedPath);
        }
        if (outErrno != nullptr) {
            *outErrno = 0;
        }
        return true;
    }

    void LoadMonitoredDevicePaths(const std::string& configPath) {
        monitoredDevicePaths_.clear();
        monitoredResolvedPaths_.clear();
        monitoredWatchdogStatus_.clear();

        const std::string stereoPort = ReadConfigValue(configPath, "stereo_dev_port");
        const std::string imuPort = ReadConfigValue(configPath, "imu_dev_port");

        if (!stereoPort.empty()) {
            monitoredDevicePaths_.push_back(stereoPort);
        }
        if (!imuPort.empty() && imuPort != stereoPort) {
            monitoredDevicePaths_.push_back(imuPort);
        }

        if (monitoredDevicePaths_.empty()) {
            std::cerr << "[USB] No Fays device nodes parsed from config. USB disconnection watch disabled." << std::endl;
            return;
        }

        for (const auto& devPath : monitoredDevicePaths_) {
            int resolveErrno = 0;
            int linkErrno = 0;
            std::string resolved;
            const bool resolvedOk = ResolveDevicePath(devPath, &resolved, &resolveErrno);
            const std::string symlinkTarget = ReadSymlinkTarget(devPath, &linkErrno);
            monitoredResolvedPaths_.push_back(resolvedOk ? resolved : "");
            monitoredWatchdogStatus_.emplace_back();

            if (!resolvedOk) {
                std::cerr << "[USB] Watching device node: " << devPath
                          << " (target unresolved at startup, realpath_errno=" << ErrnoToString(resolveErrno)
                          << ", readlink_target="
                          << (symlinkTarget.empty() ? "<unavailable>" : symlinkTarget)
                          << ", readlink_errno=" << ErrnoToString(linkErrno) << ")"
                          << std::endl;
                continue;
            }

            std::cout << "[USB] Watching device node: " << devPath
                      << " -> " << resolved << std::endl;
        }
    }

    void UsbConnectionWatchdog() {
        while (mbIsRunning_) {
            for (size_t idx = 0; idx < monitoredDevicePaths_.size(); ++idx) {
                const std::string& devPath = monitoredDevicePaths_[idx];
                if (devPath.empty()) {
                    continue;
                }

                if (idx >= monitoredResolvedPaths_.size()) {
                    monitoredResolvedPaths_.resize(idx + 1);
                }
                if (idx >= monitoredWatchdogStatus_.size()) {
                    monitoredWatchdogStatus_.resize(idx + 1);
                }
                std::string& baseline = monitoredResolvedPaths_[idx];
                UsbWatchdogStatus& status = monitoredWatchdogStatus_[idx];

                int accessErrno = 0;
                int resolveErrno = 0;
                int linkErrno = 0;
                std::string resolved;
                std::string failureType;
                std::string failureSignature;

                const std::string symlinkTarget = ReadSymlinkTarget(devPath, &linkErrno);

                errno = 0;
                if (access(devPath.c_str(), F_OK) != 0) {
                    accessErrno = errno;
                    failureType = "missing_node";
                } else if (!ResolveDevicePath(devPath, &resolved, &resolveErrno)) {
                    failureType = "unresolved_target";
                } else if (!baseline.empty() && resolved != baseline) {
                    failureType = "target_remapped";
                }

                if (failureType.empty()) {
                    if (baseline.empty()) {
                        baseline = resolved;
                    }

                    if (status.consecutiveFailures > 0) {
                        const auto unstableMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - status.firstFailureTs).count();
                        std::cout << "[USB] Watchdog recovered: path=" << devPath
                                  << ", resolved=" << resolved
                                  << ", unstable_ms=" << unstableMs
                                  << ", previous_consecutive_failures=" << status.consecutiveFailures
                                  << std::endl;
                    }

                    status.consecutiveFailures = 0;
                    status.lastFailureSignature.clear();
                    continue;
                }

                failureSignature = failureType + "|" + baseline + "|" + resolved + "|" +
                                   ErrnoToString(accessErrno) + "|" + ErrnoToString(resolveErrno) +
                                   "|" + ErrnoToString(linkErrno) + "|" + symlinkTarget;

                const auto now = std::chrono::steady_clock::now();
                if (status.consecutiveFailures == 0) {
                    status.firstFailureTs = now;
                }
                status.consecutiveFailures++;

                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - status.firstFailureTs).count();
                const bool signatureChanged = (failureSignature != status.lastFailureSignature);
                if (signatureChanged || status.consecutiveFailures == 1) {
                    uint64_t sessionId = 0;
                    const bool isRecording = GetRecordingState(nullptr, &sessionId);
                    std::cerr << "[USB] Watchdog anomaly: type=" << failureType
                              << ", path=" << devPath
                              << ", baseline_target=" << (baseline.empty() ? "<unset>" : baseline)
                              << ", current_target=" << (resolved.empty() ? "<unresolved>" : resolved)
                              << ", symlink_target=" << (symlinkTarget.empty() ? "<unavailable>" : symlinkTarget)
                              << ", access_errno=" << ErrnoToString(accessErrno)
                              << ", realpath_errno=" << ErrnoToString(resolveErrno)
                              << ", readlink_errno=" << ErrnoToString(linkErrno)
                              << ", recording=" << (isRecording ? "1" : "0")
                              << ", session_id=" << sessionId
                              << ", consecutive_failures=" << status.consecutiveFailures
                              << ", elapsed_ms=" << elapsedMs
                              << ", debounce_ms=" << kUsbDisconnectDebounceMs
                              << std::endl;
                    status.lastFailureSignature = failureSignature;
                }

                if (elapsedMs >= kUsbDisconnectDebounceMs) {
                    std::cerr << "[USB] Fays USB disconnect confirmed after debounce: path=" << devPath
                              << ", failure_type=" << failureType
                              << ", elapsed_ms=" << elapsedMs
                              << ", consecutive_failures=" << status.consecutiveFailures
                              << ". Exiting fays_record_example." << std::endl;
                    Stop();
                    std::fflush(nullptr);
                    std::exit(2);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kUsbWatchdogPollIntervalMs));
        }
    }

    static std::string NormalizeOutputDir(const std::string& outputDir) {
        std::string normalized = outputDir;
        if (normalized.empty()) {
            normalized = ".";
        }
        if (!normalized.empty() && normalized.back() != '/') {
            normalized += '/';
        }
        return normalized;
    }

    static uint64_t SystemNowNs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
    }

    void UpdateImuTimeOffset(uint64_t imuTsNs, uint64_t systemTsNs) {
        const int64_t offsetNs = static_cast<int64_t>(systemTsNs) - static_cast<int64_t>(imuTsNs);
        imuToSysOffsetNs_.store(offsetNs, std::memory_order_relaxed);
        hasImuTimeOffset_.store(true, std::memory_order_release);
    }

    uint64_t AlignFaysTsToSystem(uint64_t faysTsNs) const {
        if (!hasImuTimeOffset_.load(std::memory_order_acquire)) {
            return SystemNowNs();
        }
        const int64_t offsetNs = imuToSysOffsetNs_.load(std::memory_order_relaxed);
        const int64_t aligned = static_cast<int64_t>(faysTsNs) + offsetNs;
        return aligned > 0 ? static_cast<uint64_t>(aligned) : 0ULL;
    }

    bool GetRecordingState(std::string* outDir = nullptr, uint64_t* outSessionId = nullptr) const {
        const bool enabled = recordingEnabled_.load(std::memory_order_acquire);
        if (outSessionId != nullptr) {
            *outSessionId = recordingSessionId_.load(std::memory_order_relaxed);
        }
        if (enabled && outDir != nullptr) {
            std::lock_guard<std::mutex> lock(recordingMtx_);
            *outDir = recordingOutputDir_;
        }
        return enabled;
    }

    void ImuOnlineCapture() {
        AtrakIMU imuData;
        const uint64_t IMU_THRESHOLD_NS = 10000000;
        std::deque<ImuQueuedSample> pendingSamples;

        while (mbIsRunning_) {
            bool gotData = false;
            while (FAYS_VIK_GetImuData(mptrHandle_, &imuData) == EXIT_SUCCESS) {
                gotData = true;

                if (lastImuTimestamp_ != 0) {
                    if (imuData.timestamp < lastImuTimestamp_) {
                        imuRollbackCount_++;
                    } else {
                        const uint64_t timeDiff = imuData.timestamp - lastImuTimestamp_;
                        if (timeDiff > IMU_THRESHOLD_NS) {
                            imuGapCount_++;
                        }
                    }
                }
                lastImuTimestamp_ = imuData.timestamp;

                const uint64_t systemNowNs = SystemNowNs();
                UpdateImuTimeOffset(imuData.timestamp, systemNowNs);

                uint64_t sessionId = 0;
                if (GetRecordingState(nullptr, &sessionId) && sessionId != 0) {
                    ImuQueuedSample sample{};
                    sample.imu = imuData;
                    sample.publishTimeNs = systemNowNs;
                    sample.sessionId = sessionId;
                    pendingSamples.push_back(sample);
                    imuQueue_.TryPushBatch(pendingSamples);
                }
            }

            if (!pendingSamples.empty()) {
                imuQueue_.TryPushBatch(pendingSamples);
            }
            if (!gotData) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        while (!pendingSamples.empty()) {
            if (!imuQueue_.TryPushBatch(pendingSamples)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        imuQueue_.NotifyStop();
    }

    void McapWriteThread() {
        std::vector<ImuQueuedSample> imuBatch;
        std::vector<CamTsQueuedSample> camBatch;
        std::vector<McapControlCommand> controlBatch;
        imuBatch.reserve(512);
        camBatch.reserve(256);
        controlBatch.reserve(16);
        uint64_t lastBacklogReportNs = 0;
        uint64_t activeLoggerSessionId = 0;
        bool loggerOpen = false;

        auto processControlCommand = [&](const McapControlCommand& cmd) {
            if (cmd.type == McapControlCommand::Type::Start) {
                if (loggerOpen) {
                    mDataLogger_.Close();
                    loggerOpen = false;
                    activeLoggerSessionId = 0;
                }
                if (!mDataLogger_.Open(cmd.mcapPath)) {
                    std::cerr << "[Control] START aborted in MCAP thread: failed to open "
                              << cmd.mcapPath << std::endl;
                    recordingSessionId_.store(0, std::memory_order_release);
                    recordingEnabled_.store(false, std::memory_order_release);
                    return;
                }
                loggerOpen = true;
                activeLoggerSessionId = cmd.sessionId;
                return;
            }

            if (cmd.type == McapControlCommand::Type::Stop) {
                if (!loggerOpen) {
                    return;
                }
                if (cmd.sessionId != 0 && cmd.sessionId != activeLoggerSessionId) {
                    return;
                }
                mDataLogger_.Close();
                loggerOpen = false;
                activeLoggerSessionId = 0;
            }
        };

        while (mbIsRunning_.load(std::memory_order_acquire) || !imuQueue_.Empty() || !camTsQueue_.Empty() || !mcapControlQueue_.Empty()) {
            size_t controlCount = mcapControlQueue_.TryDrainTo(controlBatch, 16);
            if (controlCount > 0) {
                for (const auto& cmd : controlBatch) {
                    processControlCommand(cmd);
                }
                controlBatch.clear();
            }

            const size_t imuCount = imuQueue_.DrainTo(imuBatch, 512);
            size_t camCount = camTsQueue_.TryDrainTo(camBatch, 256);
            if (imuCount == 0 && camCount == 0 && controlCount == 0) {
                controlCount = mcapControlQueue_.DrainTo(controlBatch, 16);
                if (controlCount > 0) {
                    for (const auto& cmd : controlBatch) {
                        processControlCommand(cmd);
                    }
                    controlBatch.clear();
                }
            }
            if (imuCount == 0 && camCount == 0 && controlCount == 0) {
                camCount = camTsQueue_.DrainTo(camBatch, 64);
            }
            if (imuCount == 0 && camCount == 0 && controlCount == 0) {
                continue;
            }

            for (const auto& sample : imuBatch) {
                if (sample.sessionId == 0) {
                    continue;
                }
                if (!loggerOpen) {
                    continue;
                }
                if (sample.sessionId != activeLoggerSessionId) {
                    continue;
                }
                mDataLogger_.LogImu(sample.imu, sample.publishTimeNs);
            }
            for (const auto& sample : camBatch) {
                if (sample.sessionId == 0) {
                    continue;
                }
                if (!loggerOpen) {
                    continue;
                }
                if (sample.sessionId != activeLoggerSessionId) {
                    continue;
                }
                mDataLogger_.LogCamTs(sample.faysTsNs, sample.publishTimeNs, sample.frameIndex);
            }

            imuBatch.clear();
            camBatch.clear();

            const size_t imuBacklog = imuQueue_.Size();
            const size_t camBacklog = camTsQueue_.Size();
            if (imuBacklog > 4096 || camBacklog > 512) {
                const uint64_t nowNs = SystemNowNs();
                if (lastBacklogReportNs == 0 || nowNs - lastBacklogReportNs > 2000000000ULL) {
                    std::cerr << "[McapWriter] WARNING: queue backlog imu=" << imuBacklog
                              << ", cam=" << camBacklog << std::endl;
                    lastBacklogReportNs = nowNs;
                }
            }
        }

        if (loggerOpen) {
            mDataLogger_.Close();
        }
    }

    void ImgOnlineCapture() {
        const std::string version = FAYS_VIK_GetVersion(mptrHandle_);
        std::cout << "[SDK] Version: " << version << std::endl;

        const uint64_t VIDEO_THRESHOLD_NS = 60000000;

        std::cout << "[Record] Waiting for Stereo frames..." << std::endl;

        while (mbIsRunning_) {
            bool gotFrame = false;
            if (EXIT_SUCCESS == FAYS_VIK_GetStereoFrames(mptrHandle_, &mImgData_)) {
                gotFrame = true;
                if (lastImgTimestamp_ != 0) {
                    if (mImgData_.timestamp < lastImgTimestamp_) {
                        std::cout << "[Video] Timestamp rollback detected: prev=" << lastImgTimestamp_
                                  << ", curr=" << mImgData_.timestamp
                                  << " diff=" << (static_cast<int64_t>(mImgData_.timestamp) - static_cast<int64_t>(lastImgTimestamp_))
                                  << " ns" << std::endl;
                    } else {
                        const uint64_t timeDiff = mImgData_.timestamp - lastImgTimestamp_;
                        if (timeDiff > VIDEO_THRESHOLD_NS) {
                            const double timeDiffMs = timeDiff / 1e6;
                            std::cout << "[Video] Time gap detected: " << std::fixed << std::setprecision(3)
                                      << timeDiffMs << " ms"
                                      << " (prev=" << lastImgTimestamp_ << ", curr=" << mImgData_.timestamp << ")"
                                      << std::endl;
                        }
                    }
                }
                lastImgTimestamp_ = mImgData_.timestamp;
                const uint64_t systemNowNs = SystemNowNs();
                lastFrameSystemNs_.store(systemNowNs, std::memory_order_release);
                lastFrameFaysNs_.store(mImgData_.timestamp, std::memory_order_release);

                VideoFrame vf;
                vf.width = mImgData_.width;
                vf.height = mImgData_.height;
                vf.channels = mImgData_.channel;
                const size_t frameBytes = mImgData_.bytes > 0
                    ? static_cast<size_t>(mImgData_.bytes)
                    : static_cast<size_t>(std::max(0, vf.width)) *
                          static_cast<size_t>(std::max(0, vf.height)) *
                          static_cast<size_t>(std::max(0, vf.channels));
                vf.bytes.assign(mImgData_.data, mImgData_.data + frameBytes);
                vf.faysTsNs = mImgData_.timestamp;
                vf.publishTimeNs = AlignFaysTsToSystem(mImgData_.timestamp);
                videoFrameQueue_.Push(std::move(vf));
                TryDumpCalibrationJson("stereo_frame");
                WriteRuntimeStatus(false);
            }

            if (!gotFrame) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }

        videoFrameQueue_.Stop();
    }

    void VideoEncodeThread() {
        bool videoSessionOpen = false;
        uint32_t frameIndex = 0;
        uint64_t activeVideoSessionId = 0;
        std::string sessionOutputDir;
        std::deque<CamTsQueuedSample> pendingCamSamples;
        std::vector<uint8_t> bgrBuffer;

        VideoFrame vf;
        while (videoFrameQueue_.Pop(vf)) {
            std::string outputDir;
            uint64_t sessionId = 0;
            const bool shouldRecord = GetRecordingState(&outputDir, &sessionId);

            if (shouldRecord && sessionId != 0) {
                if (videoSessionOpen && sessionId != activeVideoSessionId) {
                    mRecorder_.Stop();
                    videoSessionOpen = false;
                    sessionOutputDir.clear();
                    activeVideoSessionId = 0;
                }
                if (!videoSessionOpen && vf.width > 0 && vf.height > 0) {
                    std::cout << "[Record] Input Info: " << vf.width << "x" << vf.height
                              << " Channels: " << vf.channels << std::endl;
                    if (mRecorder_.Start(outputDir + videoFileName_,
                                         vf.width, vf.height, recordFps_)) {
                        sessionOutputDir = outputDir;
                        videoSessionOpen = true;
                        activeVideoSessionId = sessionId;
                        frameIndex = 0;
                    }
                }

                if (videoSessionOpen) {
                    if (vf.channels == 1) {
                        const size_t pixelCount =
                            static_cast<size_t>(vf.width) * static_cast<size_t>(vf.height);
                        bgrBuffer.resize(pixelCount * 3);
                        if (vf.bytes.size() < pixelCount) {
                            std::cerr << "[Video] WARNING: gray frame is smaller than expected, bytes="
                                      << vf.bytes.size() << ", expected=" << pixelCount << std::endl;
                            continue;
                        }
                        cv::Mat gray(vf.height, vf.width, CV_8UC1, vf.bytes.data());
                        cv::Mat bgr(vf.height, vf.width, CV_8UC3, bgrBuffer.data());
                        cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
                        mRecorder_.Write(bgrBuffer.data(), bgrBuffer.size());
                    } else {
                        mRecorder_.Write(vf.bytes.data(), vf.bytes.size());
                    }

                    CamTsQueuedSample camSample{};
                    camSample.faysTsNs = vf.faysTsNs;
                    camSample.publishTimeNs = vf.publishTimeNs;
                    camSample.frameIndex = frameIndex;
                    camSample.sessionId = activeVideoSessionId;
                    pendingCamSamples.push_back(camSample);
                    camTsQueue_.TryPushBatch(pendingCamSamples);

                    lastEncodedFrameSystemNs_.store(SystemNowNs(), std::memory_order_release);
                    lastEncodedFrameFaysNs_.store(vf.faysTsNs, std::memory_order_release);
                    WriteRuntimeStatus(false);
                    frameIndex++;
                }
            } else if (videoSessionOpen) {
                mRecorder_.Stop();
                videoSessionOpen = false;
                activeVideoSessionId = 0;
                sessionOutputDir.clear();
            }
        }

        if (videoSessionOpen) {
            mRecorder_.Stop();
        }
        while (!pendingCamSamples.empty()) {
            if (!camTsQueue_.TryPushBatch(pendingCamSamples)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        camTsQueue_.NotifyStop();
    }

private:
    void WriteRuntimeStatus(bool force) {
        if (statusJsonPath_.empty()) {
            return;
        }

        const uint64_t nowNs = SystemNowNs();
        uint64_t lastWriteNs = lastRuntimeStatusWriteNs_.load(std::memory_order_acquire);
        if (!force && lastWriteNs != 0 && nowNs - lastWriteNs < statusIntervalNs_) {
            return;
        }
        if (!lastRuntimeStatusWriteNs_.compare_exchange_strong(
                lastWriteNs, nowNs, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }

        const std::string tmpPath =
            statusJsonPath_ + ".tmp." + std::to_string(static_cast<long long>(getpid()));
        std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            return;
        }
        const bool recordingEnabled = recordingEnabled_.load(std::memory_order_acquire);
        const uint64_t sessionId = recordingSessionId_.load(std::memory_order_acquire);
        out << "{\n";
        out << "  \"pid\": " << static_cast<long long>(getpid()) << ",\n";
        out << "  \"config_path\": \"" << JsonEscape(configPath_) << "\",\n";
        out << "  \"video_name\": \"" << JsonEscape(videoFileName_) << "\",\n";
        out << "  \"mcap_name\": \"" << JsonEscape(mcapFileName_) << "\",\n";
        out << "  \"running\": " << (mbIsRunning_.load(std::memory_order_acquire) ? "true" : "false") << ",\n";
        out << "  \"recording_enabled\": " << (recordingEnabled ? "true" : "false") << ",\n";
        out << "  \"recording_session_id\": " << sessionId << ",\n";
        out << "  \"last_frame_system_ns\": " << lastFrameSystemNs_.load(std::memory_order_acquire) << ",\n";
        out << "  \"last_frame_fays_ns\": " << lastFrameFaysNs_.load(std::memory_order_acquire) << ",\n";
        out << "  \"last_encoded_frame_system_ns\": " << lastEncodedFrameSystemNs_.load(std::memory_order_acquire) << ",\n";
        out << "  \"last_encoded_frame_fays_ns\": " << lastEncodedFrameFaysNs_.load(std::memory_order_acquire) << ",\n";
        out << "  \"status_write_system_ns\": " << nowNs << "\n";
        out << "}\n";
        out.close();
        if (!out) {
            unlink(tmpPath.c_str());
            return;
        }
        rename(tmpPath.c_str(), statusJsonPath_.c_str());
    }

    void TryDumpCalibrationJson(const char* reason) {
        if (calibrationDumped_.load(std::memory_order_acquire) || calibrationJsonPath_.empty()) {
            return;
        }

        std::string errorMessage;
        if (WriteCalibrationJsonFromHandle(mptrHandle_, configPath_, calibrationJsonPath_, &errorMessage)) {
            calibrationDumped_.store(true, std::memory_order_release);
            std::cout << "[FaysCalibration] Cached calibration json: " << calibrationJsonPath_
                      << " reason=" << reason << std::endl;
            return;
        }

        const uint64_t nowNs = SystemNowNs();
        if (lastCalibrationWarnNs_ == 0 || nowNs - lastCalibrationWarnNs_ > 2000000000ULL) {
            std::cerr << "[FaysCalibration] Not ready: " << errorMessage
                      << " reason=" << reason << std::endl;
            lastCalibrationWarnNs_ = nowNs;
        }
    }

    void* mptrHandle_;
    std::thread mptrImgThr_;
    std::thread mptrEncThr_;
    std::thread mptrImuThr_;
    std::thread mptrMcapWriterThr_;
    std::thread mptrUsbWatchThr_;

    VideoFrameQueue videoFrameQueue_;
    AtrakImage mImgData_;
    std::atomic<bool> mbIsRunning_;
    std::atomic<bool> recordingEnabled_;
    std::atomic<uint64_t> recordingSessionId_;
    std::atomic<uint64_t> recordingSessionSeed_;
    mutable std::mutex recordingMtx_;
    std::string recordingOutputDir_;
    std::string configPath_;
    std::string sdkConfigPath_;
    std::string sdkConfigTempPath_;
    std::string videoFileName_;
    std::string mcapFileName_;
    std::string calibrationJsonPath_;
    std::string statusJsonPath_;
    std::atomic<bool> calibrationDumped_;
    uint64_t lastCalibrationWarnNs_ = 0;
    uint64_t statusIntervalNs_;

    int recordFps_;
    GstRecorder mRecorder_;
    FaysDataLogger mDataLogger_;
    ImuQueue imuQueue_;
    CamTsQueue camTsQueue_;
    McapControlQueue mcapControlQueue_;

    uint64_t lastImuTimestamp_;
    std::atomic<uint64_t> imuGapCount_;
    std::atomic<uint64_t> imuRollbackCount_;

    uint64_t lastImgTimestamp_;
    std::atomic<uint64_t> lastFrameSystemNs_;
    std::atomic<uint64_t> lastFrameFaysNs_;
    std::atomic<uint64_t> lastEncodedFrameSystemNs_;
    std::atomic<uint64_t> lastEncodedFrameFaysNs_;
    std::atomic<uint64_t> lastRuntimeStatusWriteNs_;
    std::atomic<int64_t> imuToSysOffsetNs_;
    std::atomic<bool> hasImuTimeOffset_;
    std::vector<std::string> monitoredDevicePaths_;
    std::vector<std::string> monitoredResolvedPaths_;
    std::vector<UsbWatchdogStatus> monitoredWatchdogStatus_;
};

void signalHandler(int signal) {
    (void)signal;
    g_stopRequested = 1;
}

static bool ConsumeStopRequested() {
    if (g_stopRequested == 0) {
        return false;
    }
    g_stopRequested = 0;
    return true;
}

static std::string Trim(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const size_t start = input.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
}

static bool EnsureControlFifo(const std::string& fifoPath) {
    struct stat st {};
    if (stat(fifoPath.c_str(), &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            std::cerr << "[Control] Path exists but is not FIFO: " << fifoPath << std::endl;
            return false;
        }
        return true;
    }

    if (mkfifo(fifoPath.c_str(), 0666) != 0) {
        if (errno == EEXIST) {
            return true;
        }
        std::cerr << "[Control] Failed to create FIFO " << fifoPath
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

static void RunControlLoop(FaysRecorder& recorder, const std::string& fifoPath) {
    std::cout << "[Control] Entering command loop. FIFO: " << fifoPath << std::endl;
    std::cout << "[Control] Supported commands: START|<output_dir>, STOP, EXIT" << std::endl;

    while (recorder.IsRunning()) {
        // Keep FIFO opened in RDWR mode so open() won't block waiting for an external writer.
        // This makes the control endpoint visible immediately after daemon startup.
        int fd = open(fifoPath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "[Control] Failed to open FIFO: " << std::strerror(errno) << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        char buffer[1024];
        std::string pending;
        while (recorder.IsRunning()) {
            if (ConsumeStopRequested()) {
                std::cout << "\n[Signal] Stop requested, stopping recorder..." << std::endl;
                recorder.Stop();
                break;
            }

            const ssize_t bytesRead = read(fd, buffer, sizeof(buffer));
            if (bytesRead < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                std::cerr << "[Control] FIFO read failed: " << std::strerror(errno) << std::endl;
                break;
            }
            if (bytesRead == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            pending.append(buffer, buffer + bytesRead);
            size_t newlinePos = std::string::npos;
            while ((newlinePos = pending.find('\n')) != std::string::npos) {
                std::string cmd = Trim(pending.substr(0, newlinePos));
                pending.erase(0, newlinePos + 1);

                if (cmd.empty()) {
                    continue;
                }

                if (cmd.rfind("START|", 0) == 0) {
                    std::string outputDir = Trim(cmd.substr(6));
                    if (outputDir.empty()) {
                        std::cerr << "[Control] START command missing output directory." << std::endl;
                        continue;
                    }
                    recorder.StartRecording(outputDir);
                } else if (cmd == "STOP") {
                    recorder.StopRecordingSession();
                } else if (cmd == "EXIT") {
                    recorder.Stop();
                    break;
                } else {
                    std::cerr << "[Control] Unknown command: " << cmd << std::endl;
                }
            }
        }

        close(fd);
        if (ConsumeStopRequested()) {
            std::cout << "\n[Signal] Stop requested, stopping recorder..." << std::endl;
            recorder.Stop();
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  ./fays_record_example <config_path> [output_directory] [--video-name name] [--mcap-name name]" << std::endl;
        std::cerr << "  ./fays_record_example <config_path> --control-fifo <fifo_path> [--video-name name] [--mcap-name name] [--calib-json path] [--status-json path]" << std::endl;
        std::cerr << "  ./fays_record_example <config_path> --dump-calib-json <output_path>" << std::endl;
        return 1;
    }

    const std::string configPath = argv[1];
    bool controlMode = false;
    bool dumpCalibJsonMode = false;
    std::string outputDir = ".";
    std::string controlFifoPath;
    std::string dumpCalibJsonPath;
    std::string videoFileName = "fays_stereo_output.mkv";
    std::string mcapFileName = "fays_data.mcap";
    std::string calibrationJsonPath;
    std::string statusJsonPath;

    for (int index = 2; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--control-fifo" && index + 1 < argc) {
            controlMode = true;
            controlFifoPath = argv[++index];
            continue;
        }
        if (arg == "--dump-calib-json" && index + 1 < argc) {
            dumpCalibJsonMode = true;
            dumpCalibJsonPath = argv[++index];
            continue;
        }
        if (arg == "--video-name" && index + 1 < argc) {
            videoFileName = argv[++index];
            continue;
        }
        if (arg == "--mcap-name" && index + 1 < argc) {
            mcapFileName = argv[++index];
            continue;
        }
        if (arg == "--calib-json" && index + 1 < argc) {
            calibrationJsonPath = argv[++index];
            continue;
        }
        if (arg == "--status-json" && index + 1 < argc) {
            statusJsonPath = argv[++index];
            continue;
        }
        if (!controlMode && outputDir == ".") {
            outputDir = arg;
            continue;
        }
        std::cerr << "Unknown argument: " << arg << std::endl;
        return 1;
    }

    if (dumpCalibJsonMode) {
        if (controlMode) {
            std::cerr << "--dump-calib-json cannot be combined with --control-fifo" << std::endl;
            return 1;
        }
        std::string errorMessage;
        if (!DumpCalibrationJson(configPath, dumpCalibJsonPath, &errorMessage)) {
            std::cerr << "[FaysCalibration] ERROR: " << errorMessage << std::endl;
            return 1;
        }
        return 0;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    FaysRecorder recorder(configPath.c_str(), videoFileName, mcapFileName, calibrationJsonPath, statusJsonPath);
    g_recorder = &recorder;

    if (controlMode) {
        if (!EnsureControlFifo(controlFifoPath)) {
            g_recorder = nullptr;
            return 1;
        }
        RunControlLoop(recorder, controlFifoPath);
    } else {
        recorder.StartRecording(outputDir);
        std::string normalizedOutputDir = outputDir;
        if (!normalizedOutputDir.empty() && normalizedOutputDir.back() != '/') {
            normalizedOutputDir += '/';
        }

        std::cout << "========================================" << std::endl;
        std::cout << "   Stereo Recorder (Headless) Started" << std::endl;
        std::cout << "   Video: " << normalizedOutputDir << videoFileName << std::endl;
        std::cout << "   Data : " << normalizedOutputDir << mcapFileName << std::endl;
        std::cout << "   Press Ctrl+C to stop recording" << std::endl;
        std::cout << "========================================" << std::endl;

        while (recorder.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    g_recorder = nullptr;
    return 0;
}
