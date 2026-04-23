#include "record_runtime/process_supervisor.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

TEST(ProcessSupervisorTest, StopsWorkerWithExpectedExitState)
{
    ugripper::runtime::ProcessSupervisor supervisor;
    std::string error;
    ASSERT_TRUE(supervisor.Start(
        ugripper::runtime::WorkerName::AudioPlayer,
        {
            .name = "test_worker",
            .argv = {"/bin/sh", "-c", "trap 'exit 0' TERM; while true; do sleep 1; done"},
            .stop_timeout_ms = 500,
            .stop_mode = ugripper::runtime::ProcessStopMode::SigTermThenKill,
            .restart_policy = ugripper::runtime::ProcessRestartPolicy::Never,
        },
        &error))
        << error;

    EXPECT_TRUE(supervisor.IsRunning(ugripper::runtime::WorkerName::AudioPlayer));
    ASSERT_TRUE(supervisor.Stop(ugripper::runtime::WorkerName::AudioPlayer, "unit test", &error))
        << error;

    const auto status = supervisor.GetStatus(ugripper::runtime::WorkerName::AudioPlayer);
    EXPECT_FALSE(status.running);
    EXPECT_TRUE(status.expected_exit);
    EXPECT_EQ(status.state, ugripper::runtime::ProcessState::ExitedExpected);
}

TEST(ProcessSupervisorTest, MarksUnexpectedExit)
{
    ugripper::runtime::ProcessSupervisor supervisor;
    std::string error;
    ASSERT_TRUE(supervisor.Start(
        ugripper::runtime::WorkerName::CameraRecorder,
        {
            .name = "failing_worker",
            .argv = {"/bin/sh", "-c", "exit 7"},
            .stop_timeout_ms = 500,
            .stop_mode = ugripper::runtime::ProcessStopMode::SigTermThenKill,
            .restart_policy = ugripper::runtime::ProcessRestartPolicy::Never,
        },
        &error))
        << error;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(supervisor.IsRunning(ugripper::runtime::WorkerName::CameraRecorder));

    const auto status = supervisor.GetStatus(ugripper::runtime::WorkerName::CameraRecorder);
    EXPECT_EQ(status.state, ugripper::runtime::ProcessState::ExitedUnexpected);
    EXPECT_EQ(status.last_exit_code, 7);
}

TEST(ProcessSupervisorTest, StopSignalsWholeProcessGroup)
{
    namespace fs = std::filesystem;

    const fs::path pid_file = fs::temp_directory_path() /
                              ("ugripper-process-group-" + std::to_string(::getpid()) + ".pid");
    fs::remove(pid_file);

    ugripper::runtime::ProcessSupervisor supervisor;
    std::string error;
    const std::string script = "trap 'exit 0' TERM; "
                               "sleep 30 & child=$!; "
                               "echo $child > \"" + pid_file.string() + "\"; "
                               "wait $child";
    ASSERT_TRUE(supervisor.Start(
        ugripper::runtime::WorkerName::SensorRecorder,
        {
            .name = "group_worker",
            .argv = {"/bin/sh", "-c", script},
            .stop_timeout_ms = 500,
            .stop_mode = ugripper::runtime::ProcessStopMode::SigTermThenKill,
            .restart_policy = ugripper::runtime::ProcessRestartPolicy::Never,
        },
        &error))
        << error;

    pid_t child_pid = -1;
    for (int attempt = 0; attempt < 50 && child_pid <= 0; ++attempt)
    {
        if (!fs::exists(pid_file))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::ifstream input(pid_file);
        input >> child_pid;
        if (child_pid <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ASSERT_GT(child_pid, 0);
    ASSERT_EQ(kill(child_pid, 0), 0);

    ASSERT_TRUE(supervisor.Stop(ugripper::runtime::WorkerName::SensorRecorder, "unit test", &error))
        << error;

    bool child_exited = false;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (kill(child_pid, 0) != 0 && errno == ESRCH)
        {
            child_exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    fs::remove(pid_file);
    EXPECT_TRUE(child_exited);
}

}  // namespace
