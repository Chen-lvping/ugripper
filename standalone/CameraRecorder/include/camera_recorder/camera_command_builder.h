#pragma once

#include "camera_recorder/camera_types.h"

#include <filesystem>
#include <optional>
#include <string>

namespace ugripper::camera {

struct CommandBuildOptions {
    std::filesystem::path output_dir;
    std::string codec;
    std::string ffmpeg_bin = "ffmpeg";
    std::string gst_bin = "gst-launch-1.0";
};

std::string CaptureInputFormat(const CameraConfig& config);
int CaptureWidth(const CameraConfig& config);
int CaptureHeight(const CameraConfig& config);
bool UsesFrameDropWithPreservedPts(const CameraConfig& config);
int FrameDropModulo(const CameraConfig& config);
int StereoSessionFps(const CameraConfig& config);
std::optional<std::string> BuildVideoFilter(const CameraConfig& config);
std::optional<std::string> BuildStereoSessionVideoFilter(const CameraConfig& config);
std::string OutputTimingArgs(const CameraConfig& config);

std::string BuildMainCameraCommand(const CameraConfig& config, const CommandBuildOptions& options);
std::string BuildHybridCameraCommand(const CameraConfig& config, const CommandBuildOptions& options);
std::string BuildStereoHybridCameraCommand(const CameraConfig& config, const CommandBuildOptions& options);
std::string BuildStereoSessionCommand(const CameraConfig& config,
                                      const CommandBuildOptions& options,
                                      const std::filesystem::path& output_path);

}  // namespace ugripper::camera
