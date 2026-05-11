#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ugripper::camera {

enum class CameraRecordMode {
    DirectCopyH265,
    HybridDecodeEncode,
    StereoHybridDecodeEncode,
};

enum class CameraRole {
    Unknown,
    Main,
    Stereo,
    Tactile,
};

enum class CameraSide {
    Unknown,
    Mono,
    Left,
    Right,
};

enum class CameraKind {
    Unknown,
    DirectH265,
    Hybrid,
    StereoMjpeg,
};

struct CameraConfig {
    std::string name;
    std::string device;
    CameraRecordMode mode = CameraRecordMode::HybridDecodeEncode;
    std::string input_format;
    int capture_width = 0;
    int capture_height = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    int output_fps = 0;
    int eye_width = 0;
    int eye_height = 0;
    std::string video_filter;
    std::vector<std::string> output_files;
    int input_thread_queue_size = 0;
    int qp_init = 30;
    int qp_max = 38;
    int qp_min = 24;
    int qp_max_i = 38;
    int qp_min_i = 20;

    CameraRole role = CameraRole::Unknown;
    CameraSide side = CameraSide::Unknown;
    CameraKind kind = CameraKind::Unknown;
    std::vector<std::string> groups;
};

std::string ModeName(CameraRecordMode mode);
CameraRecordMode ParseModeText(const std::string& mode_text);

CameraRole ParseRoleText(const std::string& role_text);
CameraSide ParseSideText(const std::string& side_text);
CameraKind ParseKindText(const std::string& kind_text);

}  // namespace ugripper::camera
