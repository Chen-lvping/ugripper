#include "record_runtime/stereo_session_client.h"
#include "record_runtime/stereo_session_port.h"

#include "utils/time_utils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class TempDir
{
public:
    TempDir()
        : path_(fs::temp_directory_path() / ("ugripper_stereo_test_" + std::to_string(utils::CurrentEpochMs())))
    {
        fs::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

struct FakeStereoWrite
{
    bool recording = false;
    std::string episode_dir;
    int64_t start_system_time_us = 0;
    int64_t stop_system_time_us = 0;
    uint64_t command_seq = 0;
};

class FakeStereoSessionPort : public ugripper::runtime::StereoSessionPort
{
public:
    void ResetSessionState() override
    {
        ++reset_count;
    }

    bool WriteControl(bool recording,
                      const std::string& episode_dir,
                      int64_t start_system_time_us,
                      int64_t stop_system_time_us,
                      uint64_t command_seq,
                      std::string* error_message) const override
    {
        writes.push_back({
            .recording = recording,
            .episode_dir = episode_dir,
            .start_system_time_us = start_system_time_us,
            .stop_system_time_us = stop_system_time_us,
            .command_seq = command_seq,
        });
        if (error_message != nullptr)
        {
            error_message->clear();
        }
        return true;
    }

    bool LoadStatus(ugripper::runtime::StereoSessionStatusSnapshot* status,
                    std::string* error_message) const override
    {
        if (statuses.empty())
        {
            if (error_message != nullptr)
            {
                error_message->clear();
            }
            return false;
        }

        *status = statuses.front();
        statuses.erase(statuses.begin());
        if (error_message != nullptr)
        {
            error_message->clear();
        }
        return true;
    }

    mutable std::vector<FakeStereoWrite> writes;
    mutable std::vector<ugripper::runtime::StereoSessionStatusSnapshot> statuses;
    int reset_count = 0;
};

TEST(StereoSessionClientTest, WritesControlFifoAndReadsFinalizeStatus)
{
    TempDir temp_dir;
    const fs::path control_pipe = temp_dir.path() / "stereo_control.pipe";
    const fs::path status_file = temp_dir.path() / "stereo_status.json";

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::StereoSessionClient client(
        &supervisor,
        {
            .daemon_arguments = {"/bin/sh", "-c", "sleep 5"},
            .control_pipe = control_pipe.string(),
            .status_file = status_file.string(),
            .daemon_stop_timeout_ms = 500,
            .restart_interval_ms = 50,
            .finalize_wait_poll_ms = 10,
        },
        &utils::CurrentSteadyMs);

    std::string error;
    ASSERT_TRUE(client.StartDaemon(&error)) << error;
    ASSERT_EQ(mkfifo(control_pipe.c_str(), 0600), 0);
    const int pipe_fd = open(control_pipe.c_str(), O_RDWR | O_NONBLOCK);
    ASSERT_GE(pipe_fd, 0);
    ASSERT_TRUE(client.StartSession("/tmp/episode-1", 123, &error)) << error;

    char buffer[256] = {};
    ssize_t bytes = read(pipe_fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(bytes)), "START|1|/tmp/episode-1|123\n");

    ASSERT_TRUE(client.StopSession("/tmp/episode-1", 456, &error)) << error;
    buffer[0] = '\0';
    bytes = read(pipe_fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(bytes)), "STOP|2|/tmp/episode-1|456\n");
    {
        std::ofstream status_output(status_file, std::ios::trunc);
        status_output << json{
            {"finalize_pending", false},
            {"last_finalize_error", ""},
            {"active_episode_dir", ""},
            {"last_finalized_episode_dir", "/tmp/episode-1"},
            {"last_session", json{{"episode_dir", "/tmp/episode-1"}}},
        }
                               .dump(2);
    }

    EXPECT_TRUE(client.WaitForFinalize("/tmp/episode-1", 200, &error)) << error;
    client.StopDaemon();
    close(pipe_fd);
}

TEST(StereoSessionClientTest, ReportsFinalizeErrorFromStatusFile)
{
    TempDir temp_dir;
    const fs::path control_pipe = temp_dir.path() / "stereo_control.pipe";
    const fs::path status_file = temp_dir.path() / "stereo_status.json";

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::StereoSessionClient client(
        &supervisor,
        {
            .daemon_arguments = {"/bin/sh", "-c", "sleep 5"},
            .control_pipe = control_pipe.string(),
            .status_file = status_file.string(),
            .daemon_stop_timeout_ms = 500,
            .restart_interval_ms = 50,
            .finalize_wait_poll_ms = 10,
        },
        &utils::CurrentSteadyMs);

    std::ofstream status_output(status_file, std::ios::trunc);
    status_output << json{
        {"finalize_pending", false},
        {"last_finalize_error", "stereo finalize failed"},
        {"active_episode_dir", ""},
        {"last_finalized_episode_dir", ""},
        {"last_session", json::object()},
    }
                           .dump(2);
    status_output.close();

    std::string error;
    EXPECT_FALSE(client.WaitForFinalize("/tmp/episode-2", 100, &error));
    EXPECT_EQ(error, "stereo finalize failed");
}

TEST(StereoSessionClientTest, DirectSideFifosStopAndFinalizeWithoutTopControlPipe)
{
    TempDir temp_dir;
    const fs::path control_pipe = temp_dir.path() / "unused_top_control.pipe";
    const fs::path left_fifo = temp_dir.path() / "left_fays_cmd";
    const fs::path right_fifo = temp_dir.path() / "right_fays_cmd";
    const fs::path status_file = temp_dir.path() / "stereo_status.json";
    const fs::path episode_dir = temp_dir.path() / "episode_1";
    fs::create_directories(episode_dir);

    ASSERT_EQ(mkfifo(left_fifo.c_str(), 0600), 0);
    ASSERT_EQ(mkfifo(right_fifo.c_str(), 0600), 0);
    const int left_fd = open(left_fifo.c_str(), O_RDWR | O_NONBLOCK);
    const int right_fd = open(right_fifo.c_str(), O_RDWR | O_NONBLOCK);
    ASSERT_GE(left_fd, 0);
    ASSERT_GE(right_fd, 0);

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::StereoSessionClient client(
        &supervisor,
        {
            .daemon_arguments = {"/bin/sh", "-c", "sleep 5"},
            .control_pipe = control_pipe.string(),
            .left_control_fifo = left_fifo.string(),
            .right_control_fifo = right_fifo.string(),
            .status_file = status_file.string(),
            .daemon_stop_timeout_ms = 500,
            .restart_interval_ms = 50,
            .finalize_wait_poll_ms = 10,
        },
        &utils::CurrentSteadyMs);

    std::string error;
    ASSERT_TRUE(client.StartSession(episode_dir.string(), 1000, &error)) << error;

    char buffer[256] = {};
    ssize_t bytes = read(left_fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(bytes)), "START|" + episode_dir.string() + "\n");
    buffer[0] = '\0';
    bytes = read(right_fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(bytes)), "START|" + episode_dir.string() + "\n");

    ASSERT_TRUE(client.StopSession(episode_dir.string(), 2000, &error)) << error;
    buffer[0] = '\0';
    bytes = read(left_fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(bytes)), "STOP\n");
    buffer[0] = '\0';
    bytes = read(right_fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(bytes)), "STOP\n");

    {
        std::ofstream(episode_dir / "stereo_left.mkv") << "video";
        std::ofstream(episode_dir / "stereo_right.mkv") << "video";
        const std::string magic = "\x89MCAP0\r\n";
        std::ofstream left_mcap(episode_dir / "fays_data_left.mcap", std::ios::binary);
        left_mcap << magic << "payload" << magic;
        std::ofstream right_mcap(episode_dir / "fays_data_right.mcap", std::ios::binary);
        right_mcap << magic << "payload" << magic;
    }

    EXPECT_TRUE(client.WaitForFinalize(episode_dir.string(), 200, &error)) << error;
    std::string session_json;
    ASSERT_TRUE(client.LastSessionJson(episode_dir.string(), &session_json, &error)) << error;
    const json session = json::parse(session_json);
    EXPECT_EQ(session.value("episode_dir", std::string()), episode_dir.string());
    EXPECT_EQ(session["cameras"]["left_stereo"].value("record_time_offset_us", 0), 1000);
    EXPECT_EQ(session["cameras"]["right_stereo"].value("record_time_offset_us", 0), 1000);

    close(left_fd);
    close(right_fd);
}

TEST(StereoSessionClientTest, ReportsFinalizeErrorWhileDaemonIsStillFinalizing)
{
    auto fake_port = std::make_unique<FakeStereoSessionPort>();
    FakeStereoSessionPort* fake_port_ptr = fake_port.get();
    fake_port->statuses.push_back({
        .finalize_pending = true,
        .last_finalize_error = "left Fays warmup frame stale during session",
        .active_episode_dir = "/tmp/episode-pending-error",
        .last_finalized_episode_dir = "",
        .has_last_session = false,
        .last_session_episode_dir = "",
    });

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::StereoSessionClient client(
        &supervisor,
        {
            .daemon_arguments = {"/bin/sh", "-c", "sleep 5"},
            .control_pipe = "/tmp/unused-control.pipe",
            .status_file = "/tmp/unused-status.json",
            .daemon_stop_timeout_ms = 500,
            .restart_interval_ms = 50,
            .finalize_wait_poll_ms = 10,
        },
        &utils::CurrentSteadyMs,
        std::move(fake_port));

    std::string error;
    EXPECT_FALSE(client.WaitForFinalize("/tmp/episode-pending-error", 1000, &error));
    EXPECT_EQ(error, "left Fays warmup frame stale during session");
    EXPECT_TRUE(fake_port_ptr->statuses.empty());
}

TEST(StereoSessionClientTest, UsesInjectedSessionPortForCommandsAndStatus)
{
    auto fake_port = std::make_unique<FakeStereoSessionPort>();
    FakeStereoSessionPort* fake_port_ptr = fake_port.get();
    fake_port->statuses.push_back({
        .finalize_pending = false,
        .last_finalize_error = "",
        .active_episode_dir = "",
        .last_finalized_episode_dir = "",
        .has_last_session = true,
        .last_session_episode_dir = "/tmp/episode-3",
    });

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::StereoSessionClient client(
        &supervisor,
        {
            .daemon_arguments = {"/bin/sh", "-c", "sleep 5"},
            .control_pipe = "/tmp/unused-control.pipe",
            .status_file = "/tmp/unused-status.json",
            .daemon_stop_timeout_ms = 500,
            .restart_interval_ms = 50,
            .finalize_wait_poll_ms = 10,
        },
        &utils::CurrentSteadyMs,
        std::move(fake_port));

    std::string error;
    ASSERT_TRUE(client.StartDaemon(&error)) << error;
    ASSERT_EQ(fake_port_ptr->reset_count, 1);
    ASSERT_TRUE(fake_port_ptr->writes.empty());

    ASSERT_TRUE(client.StartSession("/tmp/episode-3", 789, &error)) << error;
    ASSERT_TRUE(client.StopSession("/tmp/episode-3", 987, &error)) << error;
    EXPECT_EQ(fake_port_ptr->writes.size(), 2u);
    EXPECT_TRUE(fake_port_ptr->writes[0].recording);
    EXPECT_EQ(fake_port_ptr->writes[0].episode_dir, "/tmp/episode-3");
    EXPECT_EQ(fake_port_ptr->writes[0].start_system_time_us, 789);
    EXPECT_EQ(fake_port_ptr->writes[0].command_seq, 1u);
    EXPECT_FALSE(fake_port_ptr->writes[1].recording);
    EXPECT_EQ(fake_port_ptr->writes[1].stop_system_time_us, 987);
    EXPECT_EQ(fake_port_ptr->writes[1].command_seq, 2u);

    EXPECT_TRUE(client.WaitForFinalize("/tmp/episode-3", 100, &error)) << error;
    client.StopDaemon();
    ASSERT_EQ(fake_port_ptr->writes.size(), 3u);
    EXPECT_EQ(fake_port_ptr->writes[2].command_seq, 3u);
}

}  // namespace
