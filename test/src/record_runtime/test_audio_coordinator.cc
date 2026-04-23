#include "record_runtime/audio_coordinator.h"

#include "utils/time_utils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class FakeAudioCommandPort : public ugripper::runtime::AudioCommandPort
{
public:
    void ResetReadyState() override
    {
        reset_ready_called = true;
    }

    bool HasCommandChannel() const override
    {
        return has_command_channel;
    }

    bool IsBackendReady() const override
    {
        return backend_ready;
    }

    bool SendCommand(const std::string& command, std::string* error_message) const override
    {
        (void)error_message;
        commands.push_back(command);
        return send_succeeds;
    }

    mutable std::vector<std::string> commands;
    bool has_command_channel = true;
    bool backend_ready = true;
    bool send_succeeds = true;
    bool reset_ready_called = false;
};

class TempDir
{
public:
    TempDir()
        : path_(fs::temp_directory_path() / ("ugripper_audio_test_" + std::to_string(utils::CurrentEpochMs())))
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

TEST(AudioCoordinatorTest, StartsPlayerAndSendsCommand)
{
    TempDir temp_dir;
    const fs::path audio_pipe = temp_dir.path() / "audio.pipe";
    const fs::path ready_file = temp_dir.path() / "audio.ready";

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::AudioCoordinator coordinator(
        &supervisor,
        {
            .player_arguments =
                {
                    "/bin/sh",
                    "-c",
                    "touch '" + audio_pipe.string() + "' '" + ready_file.string() + "'; sleep 5",
                },
            .audio_pipe = audio_pipe.string(),
            .audio_ready_file = ready_file.string(),
            .player_stop_timeout_ms = 500,
            .restart_interval_ms = 50,
            .ready_grace_ms = 50,
            .startup_timeout_ms = 500,
        },
        &utils::CurrentSteadyMs);

    std::string error;
    ASSERT_TRUE(coordinator.StartAudioPlayer(&error)) << error;
    EXPECT_TRUE(coordinator.audio_player_started());

    coordinator.SendCommand("ready");

    std::ifstream input(audio_pipe);
    ASSERT_TRUE(input.is_open());
    std::string line;
    std::getline(input, line);
    EXPECT_EQ(line, "ready");

    coordinator.StopAudioPlayer();
    EXPECT_FALSE(coordinator.audio_player_started());
}

TEST(AudioCoordinatorTest, TimesOutWhenPipeNeverAppears)
{
    TempDir temp_dir;
    const fs::path audio_pipe = temp_dir.path() / "missing.pipe";
    const fs::path ready_file = temp_dir.path() / "missing.ready";

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::AudioCoordinator coordinator(
        &supervisor,
        {
            .player_arguments = {"/bin/sh", "-c", "sleep 1"},
            .audio_pipe = audio_pipe.string(),
            .audio_ready_file = ready_file.string(),
            .player_stop_timeout_ms = 200,
            .restart_interval_ms = 50,
            .ready_grace_ms = 50,
            .startup_timeout_ms = 150,
        },
        &utils::CurrentSteadyMs);

    std::string error;
    EXPECT_FALSE(coordinator.StartAudioPlayer(&error));
    EXPECT_FALSE(error.empty());
}

TEST(AudioCoordinatorTest, SupportsInjectedAudioCommandPort)
{
    auto port = std::make_unique<FakeAudioCommandPort>();
    FakeAudioCommandPort* port_ptr = port.get();

    ugripper::runtime::ProcessSupervisor supervisor;
    ugripper::runtime::AudioCoordinator coordinator(
        &supervisor,
        {
            .player_arguments = {"/bin/sh", "-c", "sleep 5"},
            .audio_pipe = "/tmp/unused-audio.pipe",
            .audio_ready_file = "/tmp/unused-audio.ready",
            .player_stop_timeout_ms = 200,
            .restart_interval_ms = 50,
            .ready_grace_ms = 50,
            .startup_timeout_ms = 100,
        },
        &utils::CurrentSteadyMs,
        std::move(port));

    std::string error;
    ASSERT_TRUE(coordinator.StartAudioPlayer(&error)) << error;
    EXPECT_TRUE(port_ptr->reset_ready_called);
    EXPECT_TRUE(coordinator.audio_player_started());

    coordinator.SendCommand("ready");
    ASSERT_EQ(port_ptr->commands.size(), 1u);
    EXPECT_EQ(port_ptr->commands.front(), "ready");

    coordinator.StopAudioPlayer();
}

}  // namespace
