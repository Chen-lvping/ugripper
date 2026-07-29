#pragma once

#include <cstdint>
#include <optional>

namespace ugripper::camera {

struct MainCameraPtsResult {
    int64_t pts_us = 0;
    int64_t raw_pts_us = 0;
    int64_t adjustment_us = 0;
    bool clamped = false;
    bool timestamp_rollback = false;
};

MainCameraPtsResult MainCameraPtsFromV4l2TimeUs(
    int64_t first_frame_system_time_us,
    int64_t current_frame_system_time_us,
    std::optional<int64_t> last_frame_pts_us);

}  // namespace ugripper::camera
