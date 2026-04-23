#pragma once

#include "camera_recorder/camera_types.h"

#include <filesystem>
#include <vector>

namespace ugripper::camera {

std::vector<CameraConfig> LoadCameraConfigList(const std::filesystem::path& yaml_path);

}  // namespace ugripper::camera
