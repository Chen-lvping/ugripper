#pragma once

#include "record_runtime/subprocess_handle.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace ugripper::runtime {

enum class WorkerName
{
    AudioPlayer = 0,
    StereoDaemon,
    CameraRecorder,
    SensorRecorder,
    AudioRecorder,
    Count,
};

enum class ProcessRestartPolicy
{
    Never,
    OnUnexpectedExit,
};

enum class ProcessState
{
    Stopped,
    Starting,
    Running,
    Stopping,
    ExitedExpected,
    ExitedUnexpected,
    FailedToStart,
};

struct ProcessSpec
{
    std::string name;
    std::vector<std::string> argv;
    std::vector<int> inherited_fds;
    int stop_timeout_ms = 5000;
    ProcessStopMode stop_mode = ProcessStopMode::SigTermThenKill;
    ProcessRestartPolicy restart_policy = ProcessRestartPolicy::Never;
};

struct ProcessStatus
{
    ProcessState state = ProcessState::Stopped;
    bool running = false;
    bool ready = false;
    bool expected_exit = false;
    int pid = -1;
    int last_exit_code = 0;
    std::string last_stop_reason;
};

class ProcessSupervisor
{
public:
    ProcessSupervisor();
    ~ProcessSupervisor();

    bool Start(WorkerName worker, const ProcessSpec& spec, std::string* error_message = nullptr);
    bool Stop(WorkerName worker, const std::string& reason, std::string* error_message = nullptr);
    bool Wait(WorkerName worker, int timeout_ms);
    bool SendSignal(WorkerName worker, int signal_number);
    bool IsRunning(WorkerName worker);
    ProcessStatus GetStatus(WorkerName worker);

private:
    struct WorkerSlot
    {
        ProcessSpec spec;
        ProcessStatus status;
        std::optional<SubprocessHandle> handle;
    };

    WorkerSlot& Slot(WorkerName worker);
    const WorkerSlot& Slot(WorkerName worker) const;
    static size_t ToIndex(WorkerName worker);
    void RefreshStatus(WorkerName worker);

    std::array<WorkerSlot, static_cast<size_t>(WorkerName::Count)> workers_{};
};

}  // namespace ugripper::runtime
