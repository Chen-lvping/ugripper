#pragma once

#include "camera_recorder/camera_types.h"

#include <set>
#include <string>

namespace ugripper::camera {

bool IsStereoCamera(const CameraConfig& config);
bool HasGroup(const CameraConfig& config, const std::string& group_name);
bool IsSelectedCamera(const CameraConfig& config, const std::set<std::string>& only_names);
bool ShouldManageInStereoDaemon(const CameraConfig& config, const std::set<std::string>& only_names);
const std::string& PrimaryOutputFileName(const CameraConfig& config);

}  // namespace ugripper::camera
