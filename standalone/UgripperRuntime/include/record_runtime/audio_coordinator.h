#pragma once

#include "record_runtime/audio_command_port.h"
#include "record_runtime/process_supervisor.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ugripper::runtime {

struct AudioCoordinatorOptions
{
    std::vector<std::string> player_arguments;
    std::string audio_pipe;
    std::string audio_ready_file;
    int player_stop_timeout_ms = 1000;
    uint64_t restart_interval_ms = 2000;
    uint64_t ready_grace_ms = 3000;
    uint64_t startup_timeout_ms = 5000;
};

class AudioCoordinator
{
public:
    using NowMsFn = uint64_t (*)();

    AudioCoordinator(ProcessSupervisor* supervisor,
                     AudioCoordinatorOptions options,
                     NowMsFn now_ms_fn,
                     std::unique_ptr<AudioCommandPort> command_port = nullptr);

    bool StartAudioPlayer(std::string* error_message = nullptr);
    void StopAudioPlayer();
    void MaintainAudioPlayer();
    void SendCommand(const std::string& command) const;
    void SetRecoveryCommand(std::string command);

    bool audio_player_started() const;
    const std::string& recovery_command() const;

private:
    ProcessSupervisor* supervisor_ = nullptr;
    AudioCoordinatorOptions options_;
    NowMsFn now_ms_fn_ = nullptr;
    std::unique_ptr<AudioCommandPort> command_port_;
    bool audio_player_started_ = false;
    uint64_t last_start_attempt_ms_ = 0;
    std::string recovery_command_;
};

}  // namespace ugripper::runtime
