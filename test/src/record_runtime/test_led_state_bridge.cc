#include "record_runtime/recording_orchestrator.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

int64_t g_epoch_us = 1000;
int64_t g_steady_ms = 2000;

int64_t FakeEpochUs()
{
    return g_epoch_us;
}

int64_t FakeSteadyMs()
{
    return g_steady_ms;
}

using ugripper::runtime::ProcessSpec;
using ugripper::runtime::ProcessState;
using ugripper::runtime::ProcessStatus;
using ugripper::runtime::RecordingOrchestrator;
using ugripper::runtime::RuntimeLedState;
using ugripper::runtime::WorkerName;

struct LedBridgeHarness
{
    bool validate_ok = true;
    bool camera_bin_exists = true;
    std::vector<RuntimeLedState> led_states;
    std::vector<std::string> audio_commands;
    std::vector<std::string> recovery_commands;

    RecordingOrchestrator Make()
    {
        return RecordingOrchestrator(
            {.camera_recorder_bin = "/tmp/camera_recorder",
             .sensor_recorder_bin = "/tmp/sensor_recorder",
             .camera_codec = "h264",
             .session_camera_streams_csv = "left,right"},
            {.create_next_episode_dir =
                 []() {
                     return std::string("/tmp/episode_0001");
                 },
             .prepare_episode =
                 [](const std::string&, bool, const std::string&, std::string*) {
                     return true;
                 },
             .validate_episode =
                 [this](const std::string&, std::string*) {
                     return validate_ok;
                 },
             .prepare_sensor_start =
                 [](std::vector<std::string>*, std::vector<int>*) {},
             .finalize_sensor_start =
                 []() {},
             .attach_pending_pre_audio =
                 [](const std::string&) {
                     return true;
                 },
             .merge_episode_info =
                 [](const std::string&, std::string*) {
                     return true;
                 },
             .path_exists =
                 [this](const std::string& path) {
                     if (path == "/tmp/camera_recorder")
                     {
                         return camera_bin_exists;
                     }
                     return true;
                 },
             .set_audio_recovery_command =
                 [this](const std::string& command) {
                     recovery_commands.push_back(command);
                 },
             .send_audio_command =
                 [this](const std::string& command) {
                     audio_commands.push_back(command);
                 },
             .set_led_state =
                 [this](RuntimeLedState state, double) {
                     led_states.push_back(state);
                 },
             .start_stereo_session =
                 [](const std::string&, int64_t, std::string*) {
                     return true;
                 },
             .stop_stereo_session =
                 [](const std::string&, int64_t, std::string*) {
                     return true;
                 },
             .wait_for_stereo_finalize =
                 [](const std::string&, int, std::string*) {
                     return true;
                 },
             .flush_episode_artifacts =
                 [](const std::string&, const char*) {},
             .write_validation_error_log =
                 [](const std::string&, const std::string&) {},
             .write_recording_lock =
                 [](const std::string&) {
                     return true;
                 },
             .remove_recording_lock =
                 []() {},
             .start_worker =
                 [](WorkerName worker, const ProcessSpec& spec, std::string*) {
                     (void)worker;
                     (void)spec;
                     return true;
                 },
             .stop_worker =
                 [](WorkerName, const std::string&, std::string*) {
                     return true;
                 },
             .get_worker_status =
                 [](WorkerName worker) {
                     return ProcessStatus{
                         .state = ProcessState::Running,
                         .running = true,
                         .pid = worker == WorkerName::CameraRecorder ? 101 : 202,
                     };
                 },
             .log_info = [](const std::string&) {},
             .log_warn = [](const std::string&) {},
             .log_error = [](const std::string&) {},
             .sync_runtime_log = [](const std::string&) {}},
            &FakeEpochUs,
            &FakeSteadyMs);
    }
};

TEST(LedStateBridgeTest, StartRecordingMapsReadyFlowToRecordingLed)
{
    LedBridgeHarness harness;
    auto orchestrator = harness.Make();

    ASSERT_TRUE(orchestrator.StartRecording(false));
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Recording);
}

TEST(LedStateBridgeTest, StopRecordingSuccessReturnsToReadyLed)
{
    LedBridgeHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_TRUE(orchestrator.StopRecording(false, "stop"));

    ASSERT_GE(harness.led_states.size(), 4U);
    EXPECT_EQ(harness.led_states[0], RuntimeLedState::Recording);
    EXPECT_EQ(harness.led_states[harness.led_states.size() - 3], RuntimeLedState::Ready);
    EXPECT_EQ(harness.led_states[harness.led_states.size() - 2], RuntimeLedState::Init);
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Ready);
}

TEST(LedStateBridgeTest, ValidationFailureMapsToError1Led)
{
    LedBridgeHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    harness.validate_ok = false;
    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));

    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error1);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "validation_failed");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "validation_failed");
}

TEST(LedStateBridgeTest, MissingCameraBinaryMapsToError5Led)
{
    LedBridgeHarness harness;
    harness.camera_bin_exists = false;
    auto orchestrator = harness.Make();

    ASSERT_FALSE(orchestrator.StartRecording(false));
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error5);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
}

}  // namespace
