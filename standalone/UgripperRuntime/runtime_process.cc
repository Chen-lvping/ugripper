#include "record_runtime/audio_command_port.h"
#include "record_runtime/audio_coordinator.h"
#include "record_runtime/process_supervisor.h"
#include "record_runtime/shutdown_request_port.h"
#include "record_runtime/stereo_session_client.h"
#include "record_runtime/stereo_session_port.h"
#include "record_runtime/subprocess_handle.h"

#include "utils/logger.h"
#include "utils/time_utils.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr uint64_t kProcessPollIntervalMs = 50;

template <typename ReadyFn>
bool PollUntilReady(uint64_t timeout_ms, uint64_t poll_interval_ms, uint64_t (*now_ms_fn)(), ReadyFn&& ready_fn)
{
    const uint64_t start_ms = now_ms_fn();
    while ((now_ms_fn() - start_ms) < timeout_ms)
    {
        if (ready_fn())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
    return ready_fn();
}

bool RetryIntervalElapsed(uint64_t now_ms, uint64_t last_attempt_ms, uint64_t retry_interval_ms)
{
    return (now_ms - last_attempt_ms) >= retry_interval_ms;
}

bool WriteTextFileAtomically(const fs::path& path,
                             const std::string& content,
                             std::string* error_message);

class FileAudioCommandPort : public ugripper::runtime::AudioCommandPort
{
public:
    FileAudioCommandPort(std::string command_path, std::string ready_path)
        : command_path_(std::move(command_path)),
          ready_path_(std::move(ready_path))
    {
    }

    void ResetReadyState() override
    {
        std::error_code error;
        fs::remove(ready_path_, error);
    }

    bool HasCommandChannel() const override
    {
        return fs::exists(command_path_);
    }

    bool IsBackendReady() const override
    {
        return fs::exists(ready_path_);
    }

    bool SendCommand(const std::string& command, std::string* error_message) const override
    {
        const int fd = open(command_path_.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd < 0)
        {
            if (error_message != nullptr)
            {
                *error_message = std::strerror(errno);
            }
            return false;
        }

        const std::string payload = command + "\n";
        const ssize_t written = write(fd, payload.data(), payload.size());
        const int saved_errno = errno;
        close(fd);

        if (written < 0)
        {
            if (error_message != nullptr)
            {
                *error_message = std::strerror(saved_errno);
            }
            return false;
        }
        return true;
    }

private:
    std::string command_path_;
    std::string ready_path_;
};

class FifoStereoSessionPort : public ugripper::runtime::StereoSessionPort
{
public:
    FifoStereoSessionPort(std::string control_path, std::string status_path)
        : control_path_(std::move(control_path)),
          status_path_(std::move(status_path))
    {
    }

    void ResetSessionState() override
    {
        std::error_code error;
        fs::remove(status_path_, error);
        fs::remove(control_path_, error);
        fs::path legacy_control_path = control_path_;
        if (legacy_control_path.extension() == ".pipe")
        {
            legacy_control_path.replace_extension(".json");
        }
        else
        {
            legacy_control_path += ".json";
        }
        fs::remove(legacy_control_path, error);
    }

    bool WriteControl(bool recording,
                      const std::string& episode_dir,
                      int64_t start_system_time_us,
                      int64_t stop_system_time_us,
                      uint64_t command_seq,
                      std::string* error_message) const override
    {
        const std::string payload = BuildCommandLine(recording,
                                                     episode_dir,
                                                     start_system_time_us,
                                                     stop_system_time_us,
                                                     command_seq);
        const int fd = open(control_path_.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd < 0)
        {
            if (error_message != nullptr)
            {
                *error_message = "open stereo control pipe failed: " + std::string(std::strerror(errno));
            }
            return false;
        }

        const ssize_t written = write(fd, payload.data(), payload.size());
        const int saved_errno = errno;
        close(fd);

        if (written < 0 || static_cast<size_t>(written) != payload.size())
        {
            if (error_message != nullptr)
            {
                *error_message = written < 0
                                     ? "write stereo control pipe failed: " + std::string(std::strerror(saved_errno))
                                     : "short write to stereo control pipe";
            }
            return false;
        }
        if (error_message != nullptr)
        {
            error_message->clear();
        }
        return true;
    }

    bool LoadStatus(ugripper::runtime::StereoSessionStatusSnapshot* status,
                    std::string* error_message) const override
    {
        std::ifstream input(status_path_);
        if (!input.is_open())
        {
            if (error_message != nullptr)
            {
                error_message->clear();
            }
            return false;
        }

        json root;
        try
        {
            root = json::parse(input);
        }
        catch (const std::exception&)
        {
            if (error_message != nullptr)
            {
                error_message->clear();
            }
            return false;
        }

        status->finalize_pending = root.value("finalize_pending", false);
        status->last_finalize_error = root.value("last_finalize_error", std::string());
        status->active_episode_dir = root.value("active_episode_dir", std::string());
        status->last_finalized_episode_dir =
            root.value("last_finalized_episode_dir", std::string());
        status->has_last_session = false;
        status->last_session_episode_dir.clear();

        if (root.contains("last_session") && root["last_session"].is_object())
        {
            const json& last_session = root["last_session"];
            status->has_last_session = true;
            status->last_session_episode_dir =
                last_session.value("episode_dir", std::string());
        }

        if (error_message != nullptr)
        {
            error_message->clear();
        }
        return true;
    }

private:
    static std::string BuildCommandLine(bool recording,
                                        const std::string& episode_dir,
                                        int64_t start_system_time_us,
                                        int64_t stop_system_time_us,
                                        uint64_t command_seq)
    {
        std::string line = recording ? "START" : "STOP";
        line += '|';
        line += std::to_string(command_seq);
        line += '|';
        line += episode_dir;
        line += '|';
        line += std::to_string(recording ? start_system_time_us : stop_system_time_us);
        line += '\n';
        return line;
    }

    fs::path control_path_;
    fs::path status_path_;
};

class FileShutdownRequestPort : public ugripper::runtime::ShutdownRequestPort
{
public:
    FileShutdownRequestPort(std::string request_path, std::string result_path)
        : request_path_(std::move(request_path)),
          result_path_(std::move(result_path))
    {
    }

    bool RequestAction(const std::string& action, std::string* error_message) const override
    {
        std::error_code error;
        if (!result_path_.empty())
        {
            fs::remove(result_path_, error);
        }
        return WriteTextFileAtomically(request_path_, action + "\n", error_message);
    }

    bool WaitForActionResult(std::string* result,
                             int timeout_ms,
                             std::string* error_message) const override
    {
        if (result != nullptr)
        {
            result->clear();
        }
        if (result_path_.empty())
        {
            if (error_message != nullptr)
            {
                *error_message = "system action result path is empty";
            }
            return false;
        }

        const bool ready = PollUntilReady(
            timeout_ms > 0 ? static_cast<uint64_t>(timeout_ms) : 0,
            kProcessPollIntervalMs,
            &utils::CurrentSteadyMs,
            [this]() {
                std::error_code error;
                return fs::exists(result_path_, error) && !error;
            });
        if (!ready)
        {
            if (error_message != nullptr)
            {
                *error_message = "system action result timed out: " + result_path_.string();
            }
            return false;
        }

        std::ifstream input(result_path_);
        if (!input.is_open())
        {
            if (error_message != nullptr)
            {
                *error_message = "failed to open system action result: " + result_path_.string();
            }
            return false;
        }

        std::string line;
        std::getline(input, line);
        if (result != nullptr)
        {
            *result = line;
        }
        if (error_message != nullptr)
        {
            error_message->clear();
        }
        return !line.empty();
    }

private:
    fs::path request_path_;
    fs::path result_path_;
};

std::string JoinArguments(const std::vector<std::string>& arguments)
{
    std::string joined;
    for (size_t index = 0; index < arguments.size(); ++index)
    {
        if (index > 0)
        {
            joined += ' ';
        }
        joined += arguments[index];
    }
    return joined;
}

bool WriteTextFileAtomically(const fs::path& path,
                             const std::string& content,
                             std::string* error_message)
{
    const fs::path parent = path.parent_path();
    std::error_code error;
    if (!parent.empty())
    {
        fs::create_directories(parent, error);
        if (error)
        {
            if (error_message != nullptr)
            {
                *error_message = "create parent directory failed: " + error.message();
            }
            return false;
        }
    }

    const fs::path temp_path = path.string() + ".tmp";
    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output.is_open())
        {
            if (error_message != nullptr)
            {
                *error_message = "cannot open temp file for write: " + temp_path.string();
            }
            return false;
        }
        output << content;
        output.flush();
        if (!output.good())
        {
            if (error_message != nullptr)
            {
                *error_message = "failed to write temp file: " + temp_path.string();
            }
            return false;
        }
    }

    fs::rename(temp_path, path, error);
    if (error)
    {
        if (error_message != nullptr)
        {
            *error_message = "rename temp file failed: " + error.message();
        }
        fs::remove(temp_path, error);
        return false;
    }
    return true;
}

}  // namespace

namespace ugripper::runtime {

std::unique_ptr<AudioCommandPort> CreateFileAudioCommandPort(std::string command_path, std::string ready_path)
{
    return std::make_unique<FileAudioCommandPort>(std::move(command_path), std::move(ready_path));
}

std::unique_ptr<StereoSessionPort> CreateFifoStereoSessionPort(std::string control_path,
                                                               std::string status_path)
{
    return std::make_unique<FifoStereoSessionPort>(std::move(control_path), std::move(status_path));
}

std::unique_ptr<ShutdownRequestPort> CreateFileShutdownRequestPort(std::string request_path,
                                                                   std::string result_path)
{
    return std::make_unique<FileShutdownRequestPort>(std::move(request_path), std::move(result_path));
}

SubprocessHandle::SubprocessHandle(std::string name)
    : name_(std::move(name))
{
}

SubprocessHandle::~SubprocessHandle()
{
    std::string error_message;
    Stop(ProcessStopMode::SigTermThenKill, 1000, &error_message);
}

bool SubprocessHandle::Start(const std::vector<std::string>& arguments,
                             const std::vector<int>& inherited_fds,
                             std::string* error_message)
{
    if (arguments.empty())
    {
        if (error_message != nullptr)
        {
            *error_message = "empty argv";
        }
        return false;
    }

    Stop(ProcessStopMode::SigTermThenKill, 1000, nullptr);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
    {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "launching " << name_ << ": " << JoinArguments(arguments)).str());

    pid_ = fork();
    if (pid_ < 0)
    {
        if (error_message != nullptr)
        {
            *error_message = std::string("fork failed: ") + std::strerror(errno);
        }
        pid_ = -1;
        return false;
    }

    if (pid_ == 0)
    {
        setpgid(0, 0);
        for (const int fd : inherited_fds)
        {
            if (fd < 0)
            {
                continue;
            }
            const int flags = fcntl(fd, F_GETFD);
            if (flags >= 0)
            {
                fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (setpgid(pid_, pid_) != 0 && errno != EACCES)
    {
        if (error_message != nullptr)
        {
            *error_message = std::string("setpgid failed: ") + std::strerror(errno);
        }
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to assign process group for " << name_ << ": " << std::strerror(errno)).str());
    }

    process_group_id_ = pid_;
    last_exit_code_ = 0;
    return true;
}

bool SubprocessHandle::IsRunning()
{
    if (pid_ <= 0)
    {
        return false;
    }
    return !PollExit(false);
}

bool SubprocessHandle::Wait(int timeout_ms)
{
    if (pid_ <= 0)
    {
        return true;
    }

    return PollUntilReady(
        static_cast<uint64_t>(timeout_ms), kProcessPollIntervalMs, &utils::CurrentSteadyMs, [this]() {
            return PollExit(false);
        });
}

bool SubprocessHandle::SendSignal(int signal_number)
{
    const pid_t target = SignalTarget();
    if (target == 0)
    {
        return false;
    }
    return kill(target, signal_number) == 0;
}

bool SubprocessHandle::Stop(ProcessStopMode stop_mode, int timeout_ms, std::string* error_message)
{
    if (pid_ <= 0)
    {
        return true;
    }

    const auto set_timeout_error = [&](const char* stage) {
        if (error_message != nullptr)
        {
            *error_message = std::string(stage) + " timeout for " + name_;
        }
    };

    const int sigint_timeout_ms = std::max(100, timeout_ms / 2);
    const int sigterm_timeout_ms = std::max(100, timeout_ms);

    if (stop_mode == ProcessStopMode::SigIntThenTermThenKill)
    {
        if (!SendSignal(SIGINT) && errno != ESRCH)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to send SIGINT to " << name_ << ": " << std::strerror(errno)).str());
        }
        if (Wait(sigint_timeout_ms))
        {
            return true;
        }
    }

    if (!SendSignal(SIGTERM) && errno != ESRCH)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to send SIGTERM to " << name_ << ": " << std::strerror(errno)).str());
    }
    if (Wait(sigterm_timeout_ms))
    {
        return true;
    }

    set_timeout_error(stop_mode == ProcessStopMode::SigIntThenTermThenKill ? "sigterm" : "stop");
    DM_LOG_WARN("{}", (::DA::utils::LogString() << name_ << " did not exit after stop signal, escalating to SIGKILL").str());
    if (!SendSignal(SIGKILL) && errno != ESRCH)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to send SIGKILL to " << name_ << ": " << std::strerror(errno)).str());
    }
    PollExit(true);
    return pid_ <= 0;
}

void SubprocessHandle::Reset()
{
    pid_ = -1;
    process_group_id_ = -1;
    last_exit_code_ = 0;
}

int SubprocessHandle::LastExitCode() const
{
    return last_exit_code_;
}

int SubprocessHandle::Pid() const
{
    return pid_;
}

pid_t SubprocessHandle::SignalTarget() const
{
    if (process_group_id_ > 0)
    {
        return -process_group_id_;
    }
    if (pid_ > 0)
    {
        return pid_;
    }
    return 0;
}

bool SubprocessHandle::PollExit(bool blocking)
{
    if (pid_ <= 0)
    {
        return true;
    }

    int status = 0;
    const int flags = blocking ? 0 : WNOHANG;
    const pid_t wait_result = waitpid(pid_, &status, flags);
    if (wait_result == 0)
    {
        return false;
    }
    if (wait_result < 0)
    {
        return false;
    }

    if (WIFEXITED(status))
    {
        last_exit_code_ = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        last_exit_code_ = 128 + WTERMSIG(status);
    }
    else
    {
        last_exit_code_ = status;
    }

    pid_ = -1;
    process_group_id_ = -1;
    return true;
}

ProcessSupervisor::ProcessSupervisor() = default;
ProcessSupervisor::~ProcessSupervisor() = default;

bool ProcessSupervisor::Start(WorkerName worker, const ProcessSpec& spec, std::string* error_message)
{
    auto& slot = Slot(worker);
    RefreshStatus(worker);
    if (slot.status.running)
    {
        if (error_message != nullptr)
        {
            *error_message = spec.name + " already running";
        }
        return true;
    }

    slot.spec = spec;
    slot.status = {};
    slot.status.state = ProcessState::Starting;
    slot.status.expected_exit = false;
    slot.handle.emplace(spec.name);
    if (!slot.handle->Start(spec.argv, spec.inherited_fds, error_message))
    {
        slot.status.state = ProcessState::FailedToStart;
        slot.status.running = false;
        slot.status.ready = false;
        slot.status.pid = -1;
        return false;
    }

    slot.status.state = ProcessState::Running;
    slot.status.running = true;
    slot.status.ready = true;
    slot.status.pid = slot.handle->Pid();
    slot.status.last_exit_code = slot.handle->LastExitCode();
    slot.status.last_stop_reason.clear();
    return true;
}

bool ProcessSupervisor::Stop(WorkerName worker, const std::string& reason, std::string* error_message)
{
    auto& slot = Slot(worker);
    RefreshStatus(worker);
    if (!slot.handle.has_value())
    {
        slot.status.state = ProcessState::Stopped;
        slot.status.running = false;
        slot.status.ready = false;
        slot.status.expected_exit = true;
        slot.status.last_stop_reason = reason;
        return true;
    }

    slot.status.state = ProcessState::Stopping;
    slot.status.expected_exit = true;
    slot.status.last_stop_reason = reason;
    const bool stopped =
        slot.handle->Stop(slot.spec.stop_mode, slot.spec.stop_timeout_ms, error_message);
    RefreshStatus(worker);
    if (stopped)
    {
        slot.status.expected_exit = true;
        if (slot.status.state == ProcessState::ExitedUnexpected)
        {
            slot.status.state = ProcessState::ExitedExpected;
        }
    }
    return stopped;
}

bool ProcessSupervisor::Wait(WorkerName worker, int timeout_ms)
{
    auto& slot = Slot(worker);
    if (!slot.handle.has_value())
    {
        return true;
    }
    const bool exited = slot.handle->Wait(timeout_ms);
    RefreshStatus(worker);
    return exited;
}

bool ProcessSupervisor::SendSignal(WorkerName worker, int signal_number)
{
    auto& slot = Slot(worker);
    if (!slot.handle.has_value())
    {
        return false;
    }
    return slot.handle->SendSignal(signal_number);
}

bool ProcessSupervisor::IsRunning(WorkerName worker)
{
    RefreshStatus(worker);
    return Slot(worker).status.running;
}

ProcessStatus ProcessSupervisor::GetStatus(WorkerName worker)
{
    RefreshStatus(worker);
    return Slot(worker).status;
}

ProcessSupervisor::WorkerSlot& ProcessSupervisor::Slot(WorkerName worker)
{
    return workers_[ToIndex(worker)];
}

const ProcessSupervisor::WorkerSlot& ProcessSupervisor::Slot(WorkerName worker) const
{
    return workers_[ToIndex(worker)];
}

size_t ProcessSupervisor::ToIndex(WorkerName worker)
{
    return static_cast<size_t>(worker);
}

void ProcessSupervisor::RefreshStatus(WorkerName worker)
{
    auto& slot = Slot(worker);
    if (!slot.handle.has_value())
    {
        return;
    }

    const bool running = slot.handle->IsRunning();
    slot.status.pid = slot.handle->Pid();
    slot.status.last_exit_code = slot.handle->LastExitCode();
    slot.status.running = running;
    slot.status.ready = running;

    if (running)
    {
        slot.status.state = ProcessState::Running;
        return;
    }

    if (slot.status.state == ProcessState::FailedToStart)
    {
        return;
    }

    if (slot.status.expected_exit)
    {
        slot.status.state = ProcessState::ExitedExpected;
    }
    else if (slot.status.pid == -1)
    {
        slot.status.state = ProcessState::ExitedUnexpected;
    }
}

AudioCoordinator::AudioCoordinator(ProcessSupervisor* supervisor,
                                   AudioCoordinatorOptions options,
                                   NowMsFn now_ms_fn,
                                   std::unique_ptr<AudioCommandPort> command_port)
    : supervisor_(supervisor),
      options_(std::move(options)),
      now_ms_fn_(now_ms_fn),
      command_port_(std::move(command_port))
{
    if (command_port_ == nullptr)
    {
        command_port_ = CreateFileAudioCommandPort(options_.audio_pipe, options_.audio_ready_file);
    }
}

bool AudioCoordinator::StartAudioPlayer(std::string* error_message)
{
    if (options_.player_arguments.empty())
    {
        if (error_message != nullptr)
        {
            *error_message = "audio player argv is empty";
        }
        return false;
    }

    command_port_->ResetReadyState();
    last_start_attempt_ms_ = now_ms_fn_();

    ProcessSpec spec;
    spec.name = "audio_player";
    spec.argv = options_.player_arguments;
    spec.stop_timeout_ms = options_.player_stop_timeout_ms;
    spec.stop_mode = ProcessStopMode::SigTermThenKill;

    if (!supervisor_->Start(WorkerName::AudioPlayer, spec, error_message))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to launch audio player, sound prompts disabled").str());
        audio_player_started_ = false;
        return false;
    }

    const bool ready = PollUntilReady(options_.startup_timeout_ms, kProcessPollIntervalMs, now_ms_fn_, [this, error_message]() {
        if (!supervisor_->IsRunning(WorkerName::AudioPlayer))
        {
            const auto status = supervisor_->GetStatus(WorkerName::AudioPlayer);
            if (error_message != nullptr)
            {
                *error_message = "audio player exited before ready, last_exit=" +
                                 std::to_string(status.last_exit_code);
            }
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio player exited before ready, last_exit=" << status.last_exit_code).str());
            audio_player_started_ = false;
            return true;
        }
        if (!command_port_->HasCommandChannel())
        {
            return false;
        }

        const bool playback_ready = command_port_->IsBackendReady();
        audio_player_started_ = playback_ready;
        if (!playback_ready)
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "audio player launched; waiting for USB headset sink").str());
        }
        return true;
    });

    if (ready)
    {
        return audio_player_started_ || command_port_->HasCommandChannel();
    }

    if (error_message != nullptr)
    {
        *error_message = "audio player pipe timeout";
    }
    DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio player pipe timeout, audio daemon did not finish bootstrap").str());
    supervisor_->Stop(WorkerName::AudioPlayer, "audio bootstrap timeout", nullptr);
    audio_player_started_ = false;
    return false;
}

void AudioCoordinator::StopAudioPlayer()
{
    if (audio_player_started_ && supervisor_->IsRunning(WorkerName::AudioPlayer))
    {
        SendCommand("exit");
        supervisor_->Wait(WorkerName::AudioPlayer, 1000);
    }
    supervisor_->Stop(WorkerName::AudioPlayer, "audio stop requested", nullptr);
    audio_player_started_ = false;
}

void AudioCoordinator::MaintainAudioPlayer()
{
    const uint64_t now_ms = now_ms_fn_();
    const bool pipe_ready = command_port_->HasCommandChannel();
    const bool ready_marker = command_port_->IsBackendReady();

    if (supervisor_->IsRunning(WorkerName::AudioPlayer))
    {
        if (!pipe_ready)
        {
            if (!RetryIntervalElapsed(now_ms, last_start_attempt_ms_, options_.ready_grace_ms))
            {
                return;
            }

            DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio player running without pipe, restarting").str());
            supervisor_->Stop(WorkerName::AudioPlayer, "missing audio pipe", nullptr);
        }

        if (ready_marker)
        {
            const bool recovered = !audio_player_started_;
            audio_player_started_ = true;
            if (recovered)
            {
                DM_LOG_INFO("{}", (::DA::utils::LogString() << "audio player recovered and is ready").str());
                if (!recovery_command_.empty())
                {
                    SendCommand(recovery_command_);
                }
            }
            return;
        }

        if (audio_player_started_)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio player lost ready marker, waiting for USB headset recovery").str());
        }
        audio_player_started_ = false;
        return;
    }

    if (audio_player_started_)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio player exited, will retry").str());
        audio_player_started_ = false;
    }

    if (!RetryIntervalElapsed(now_ms, last_start_attempt_ms_, options_.restart_interval_ms))
    {
        return;
    }

    StartAudioPlayer(nullptr);
}

void AudioCoordinator::SendCommand(const std::string& command) const
{
    if (command.empty() || !audio_player_started_)
    {
        return;
    }

    std::string error_message;
    if (!command_port_->SendCommand(command, &error_message))
    {
        if (command != "exit")
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to send audio command '" << command << "': " << error_message).str());
        }
        return;
    }
}

void AudioCoordinator::SetRecoveryCommand(std::string command)
{
    recovery_command_ = std::move(command);
}

bool AudioCoordinator::audio_player_started() const
{
    return audio_player_started_;
}

const std::string& AudioCoordinator::recovery_command() const
{
    return recovery_command_;
}

StereoSessionClient::StereoSessionClient(ProcessSupervisor* supervisor,
                                         StereoSessionClientOptions options,
                                         NowMsFn now_ms_fn,
                                         std::unique_ptr<StereoSessionPort> session_port)
    : supervisor_(supervisor),
      options_(std::move(options)),
      now_ms_fn_(now_ms_fn),
      session_port_(std::move(session_port))
{
    if (session_port_ == nullptr)
    {
        session_port_ =
            CreateFifoStereoSessionPort(options_.control_pipe, options_.status_file);
    }
}

bool StereoSessionClient::StartDaemon(std::string* error_message)
{
    session_port_->ResetSessionState();
    last_start_attempt_ms_ = now_ms_fn_();

    ProcessSpec spec;
    spec.name = "stereo_daemon";
    spec.argv = options_.daemon_arguments;
    spec.stop_timeout_ms = options_.daemon_stop_timeout_ms;
    spec.stop_mode = ProcessStopMode::SigTermThenKill;
    spec.restart_policy = ProcessRestartPolicy::OnUnexpectedExit;

    if (!supervisor_->Start(WorkerName::StereoDaemon, spec, error_message))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to launch stereo daemon").str());
        daemon_started_ = false;
        return false;
    }

    daemon_started_ = true;
    return true;
}

void StereoSessionClient::StopDaemon()
{
    if (daemon_started_)
    {
        WriteControl(false, "", 0, 0, nullptr);
    }
    supervisor_->Stop(WorkerName::StereoDaemon, "stereo daemon stop requested", nullptr);
    daemon_started_ = false;
}

void StereoSessionClient::MaintainDaemon()
{
    const uint64_t now_ms = now_ms_fn_();
    if (supervisor_->IsRunning(WorkerName::StereoDaemon))
    {
        daemon_started_ = true;
        return;
    }

    if (daemon_started_)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "stereo daemon exited, will retry").str());
        daemon_started_ = false;
    }

    if (!RetryIntervalElapsed(now_ms, last_start_attempt_ms_, options_.restart_interval_ms))
    {
        return;
    }
    StartDaemon(nullptr);
}

bool StereoSessionClient::StartSession(const std::string& episode_dir,
                                       int64_t start_system_time_us,
                                       std::string* error_message)
{
    return WriteControl(true, episode_dir, start_system_time_us, 0, error_message);
}

bool StereoSessionClient::StopSession(const std::string& episode_dir,
                                      int64_t stop_system_time_us,
                                      std::string* error_message)
{
    return WriteControl(false, episode_dir, 0, stop_system_time_us, error_message);
}

bool StereoSessionClient::WaitForFinalize(const std::string& episode_dir,
                                          int timeout_ms,
                                          std::string* error_message) const
{
    const bool finalized = PollUntilReady(
        static_cast<uint64_t>(timeout_ms), options_.finalize_wait_poll_ms, now_ms_fn_, [&]() {
            StereoSessionStatusSnapshot status;
            std::string load_error;
            if (!session_port_->LoadStatus(&status, &load_error))
            {
                return false;
            }

            if (!status.last_finalize_error.empty() &&
                (status.active_episode_dir.empty() || status.active_episode_dir == episode_dir))
            {
                if (error_message != nullptr)
                {
                    *error_message = status.last_finalize_error;
                }
                return true;
            }

            if (status.has_last_session && status.last_session_episode_dir == episode_dir)
            {
                return true;
            }

            if (!status.finalize_pending && status.last_finalized_episode_dir == episode_dir)
            {
                if (error_message != nullptr && error_message->empty())
                {
                    *error_message = "stereo daemon finalized the session without session metadata";
                }
                return true;
            }
            return false;
        });

    if (finalized)
    {
        return error_message == nullptr || error_message->empty();
    }

    if (error_message != nullptr && error_message->empty())
    {
        StereoSessionStatusSnapshot status;
        std::string load_error;
        if (session_port_->LoadStatus(&status, &load_error) &&
            !status.last_finalize_error.empty() &&
            (status.active_episode_dir.empty() || status.active_episode_dir == episode_dir))
        {
            *error_message = status.last_finalize_error;
        }
        else
        {
            *error_message = "timed out waiting for stereo session metadata";
        }
    }
    return false;
}

bool StereoSessionClient::daemon_started() const
{
    return daemon_started_;
}

uint64_t StereoSessionClient::command_seq() const
{
    return command_seq_;
}

bool StereoSessionClient::WriteControl(bool recording,
                                       const std::string& episode_dir,
                                       int64_t start_system_time_us,
                                       int64_t stop_system_time_us,
                                       std::string* error_message) const
{
    const uint64_t next_command_seq = ++command_seq_;
    if (!session_port_->WriteControl(recording,
                                     episode_dir,
                                     start_system_time_us,
                                     stop_system_time_us,
                                     next_command_seq,
                                     error_message))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write stereo control pipe: "
                             << (error_message != nullptr ? *error_message : std::string())).str());
        return false;
    }
    return true;
}

}  // namespace ugripper::runtime
