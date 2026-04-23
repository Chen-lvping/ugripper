#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

namespace ugripper::runtime {

enum class ProcessStopMode
{
    SigTermThenKill,
    SigIntThenTermThenKill,
};

class SubprocessHandle
{
public:
    explicit SubprocessHandle(std::string name);
    ~SubprocessHandle();

    bool Start(const std::vector<std::string>& arguments,
               const std::vector<int>& inherited_fds = {},
               std::string* error_message = nullptr);
    bool IsRunning();
    bool Wait(int timeout_ms);
    bool SendSignal(int signal_number);
    bool Stop(ProcessStopMode stop_mode, int timeout_ms, std::string* error_message = nullptr);
    void Reset();
    int LastExitCode() const;
    int Pid() const;

private:
    bool PollExit(bool blocking);
    pid_t SignalTarget() const;

    std::string name_;
    pid_t pid_ = -1;
    pid_t process_group_id_ = -1;
    int last_exit_code_ = 0;
};

}  // namespace ugripper::runtime
