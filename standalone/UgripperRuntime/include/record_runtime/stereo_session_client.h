#pragma once

#include "record_runtime/process_supervisor.h"
#include "record_runtime/stereo_session_port.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ugripper::runtime {

struct StereoSessionClientOptions
{
    std::vector<std::string> daemon_arguments;
    std::string control_file;
    std::string status_file;
    int daemon_stop_timeout_ms = 2000;
    uint64_t restart_interval_ms = 2000;
    uint64_t finalize_wait_poll_ms = 100;
};

class StereoSessionClient
{
public:
    using NowMsFn = uint64_t (*)();

    StereoSessionClient(ProcessSupervisor* supervisor,
                        StereoSessionClientOptions options,
                        NowMsFn now_ms_fn,
                        std::unique_ptr<StereoSessionPort> session_port = nullptr);

    bool StartDaemon(std::string* error_message = nullptr);
    void StopDaemon();
    void MaintainDaemon();
    bool StartSession(const std::string& episode_dir,
                      int64_t start_system_time_us,
                      std::string* error_message = nullptr);
    bool StopSession(const std::string& episode_dir,
                     int64_t stop_system_time_us,
                     std::string* error_message = nullptr);
    bool WaitForFinalize(const std::string& episode_dir,
                         int timeout_ms,
                         std::string* error_message = nullptr) const;

    bool daemon_started() const;
    uint64_t command_seq() const;

private:
    bool WriteControl(bool recording,
                      const std::string& episode_dir,
                      int64_t start_system_time_us,
                      int64_t stop_system_time_us,
                      std::string* error_message) const;

    ProcessSupervisor* supervisor_ = nullptr;
    StereoSessionClientOptions options_;
    NowMsFn now_ms_fn_ = nullptr;
    std::unique_ptr<StereoSessionPort> session_port_;
    bool daemon_started_ = false;
    uint64_t last_start_attempt_ms_ = 0;
    mutable uint64_t command_seq_ = 0;
};

}  // namespace ugripper::runtime
