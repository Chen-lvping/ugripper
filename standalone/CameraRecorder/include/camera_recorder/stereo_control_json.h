#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace ugripper::camera {

struct StereoControlCommand {
    uint64_t command_seq = 0;
    bool recording = false;
    std::string episode_dir;
    int64_t start_system_time_us = 0;
    int64_t stop_system_time_us = 0;
};

struct StereoTrackStatus {
    std::string state;
    std::string device;
    bool ready = false;
    std::optional<int64_t> first_frame_system_time_us;
    std::optional<int64_t> last_frame_system_time_us;
    bool session_recording = false;
    std::string last_error;
};

struct StereoServiceStatus {
    bool recording = false;
    bool finalize_pending = false;
    std::string active_episode_dir;
    std::string last_finalized_episode_dir;
    std::string last_finalize_error;
    nlohmann::json last_session = nlohmann::json::object();
    bool ready = false;
    std::string service_state;
    std::map<std::string, StereoTrackStatus> cameras;
};

std::optional<StereoControlCommand> ParseStereoControlCommand(const nlohmann::json& root);
nlohmann::json BuildStereoTrackStatusJson(const StereoTrackStatus& status);
nlohmann::json BuildStereoServiceStatusJson(const StereoServiceStatus& status);

}  // namespace ugripper::camera
