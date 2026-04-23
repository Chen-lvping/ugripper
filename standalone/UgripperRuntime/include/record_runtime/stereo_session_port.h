#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ugripper::runtime {

struct StereoSessionStatusSnapshot
{
    bool finalize_pending = false;
    std::string last_finalize_error;
    std::string active_episode_dir;
    std::string last_finalized_episode_dir;
    bool has_last_session = false;
    std::string last_session_episode_dir;
};

class StereoSessionPort
{
public:
    virtual ~StereoSessionPort() = default;

    virtual void ResetSessionState() = 0;
    virtual bool WriteControl(bool recording,
                              const std::string& episode_dir,
                              int64_t start_system_time_us,
                              int64_t stop_system_time_us,
                              uint64_t command_seq,
                              std::string* error_message) const = 0;
    virtual bool LoadStatus(StereoSessionStatusSnapshot* status,
                            std::string* error_message) const = 0;
};

std::unique_ptr<StereoSessionPort> CreateFileStereoSessionPort(std::string control_path,
                                                               std::string status_path);

}  // namespace ugripper::runtime
