#include "record_runtime/recording_orchestrator.h"

#include <gtest/gtest.h>

#include <map>
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

using ugripper::runtime::ProcessRestartPolicy;
using ugripper::runtime::ProcessSpec;
using ugripper::runtime::ProcessState;
using ugripper::runtime::ProcessStatus;
using ugripper::runtime::RecordingOrchestrator;
using ugripper::runtime::RuntimeLedState;
using ugripper::runtime::HardwareFaultSide;
using ugripper::runtime::HealthFault;
using ugripper::runtime::WorkerName;

struct RecordingHarness
{
    std::map<WorkerName, ProcessStatus> worker_status;
    std::map<WorkerName, ProcessSpec> started_specs;
    std::vector<std::string> audio_commands;
    std::vector<std::string> recovery_commands;
    std::vector<RuntimeLedState> led_states;
    std::vector<HealthFault> hardware_fault_led_states;
    std::vector<std::string> flushed_stages;
    std::vector<std::string> stop_reasons;
    std::vector<std::string> logs;
    std::vector<std::string> lock_events;
    std::vector<std::string> validate_error_types;
    std::vector<std::string> metadata_error_types;
    std::vector<std::string> sensor_args;
    std::vector<int> sensor_inherited_fds;
    std::string last_validation_log;
    std::string sync_reason;
    int validate_calls = 0;
    bool prepare_ok = true;
    bool validate_ok = true;
    bool merge_ok = true;
    bool stereo_start_ok = true;
    bool stereo_stop_ok = true;
    bool wait_finalize_ok = true;
    bool attach_ok = true;
    bool camera_bin_exists = true;
    bool sensor_bin_exists = true;
    bool camera_start_ok = true;
    bool sensor_start_ok = true;
    bool write_lock_ok = true;
    std::string prepare_error = "prepare episode failed";
    std::string validate_error = "episode validation failed";
    std::string merge_error = "merge episode info failed";
    std::string stereo_start_error = "stereo session start failed";
    std::string wait_finalize_error = "stereo finalize timeout";

    RecordingOrchestrator Make()
    {
        return RecordingOrchestrator(
            {.camera_recorder_bin = "/tmp/camera_recorder",
             .sensor_recorder_bin = "/tmp/sensor_recorder",
             .camera_codec = "h264",
             .session_camera_streams_csv = "left,right",
             .worker_stop_timeout_ms = 5000,
             .stereo_finalize_timeout_ms = 10000},
            {.create_next_episode_dir =
                 []() {
                     return std::string("/tmp/episode_0001");
                 },
             .prepare_episode =
                 [this](const std::string&, bool, const std::string&, std::string* error) {
                     if (!prepare_ok)
                     {
                         if (error != nullptr)
                         {
                             *error = prepare_error;
                         }
                         return false;
                     }
                     return prepare_ok;
                 },
             .validate_episode =
                 [this](const std::string&, std::string* error, std::vector<std::string>* error_types) {
                     ++validate_calls;
                     if (!validate_ok && error != nullptr)
                     {
                         *error = validate_error;
                     }
                     if (!validate_ok && error_types != nullptr)
                     {
                         *error_types = validate_error_types;
                     }
                     return validate_ok;
                 },
             .prepare_sensor_start =
                 [this](std::vector<std::string>* extra_args, std::vector<int>* inherited_fds) {
                    if (extra_args != nullptr)
                    {
                        extra_args->push_back("--extra-test-fd");
                        extra_args->push_back("99");
                    }
                     if (inherited_fds != nullptr)
                     {
                         inherited_fds->push_back(99);
                     }
                 },
             .finalize_sensor_start =
                 []() {},
             .attach_pending_pre_audio =
                 [this](const std::string&) {
                     return attach_ok;
                 },
             .merge_episode_info =
                 [this](const std::string&, std::string* error) {
                     if (!merge_ok && error != nullptr)
                     {
                         *error = merge_error;
                     }
                     return merge_ok;
                 },
             .path_exists =
                 [this](const std::string& path) {
                     if (path == "/tmp/camera_recorder")
                     {
                         return camera_bin_exists;
                     }
                     if (path == "/tmp/sensor_recorder")
                     {
                         return sensor_bin_exists;
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
             .set_hardware_fault_led_state =
                 [this](const HealthFault& fault) {
                     hardware_fault_led_states.push_back(fault);
                     led_states.push_back(fault.led_state);
                 },
             .start_stereo_session =
                 [this](const std::string&, int64_t, std::string* error) {
                     if (!stereo_start_ok && error != nullptr)
                     {
                         *error = stereo_start_error;
                     }
                     return stereo_start_ok;
                 },
             .stop_stereo_session =
                 [this](const std::string&, int64_t, std::string*) {
                     return stereo_stop_ok;
                 },
             .wait_for_stereo_finalize =
                 [this](const std::string&, int, std::string* error) {
                     if (!wait_finalize_ok && error != nullptr)
                     {
                         *error = wait_finalize_error;
                     }
                     return wait_finalize_ok;
                 },
             .flush_episode_artifacts =
                 [this](const std::string&, const char* stage) {
                     flushed_stages.emplace_back(stage);
                 },
             .write_episode_metadata =
                 [this](const std::string&, bool, const std::string&, const std::string& error_type) {
                     metadata_error_types.push_back(error_type);
                 },
             .write_validation_error_log =
                 [this](const std::string&, const std::string& error_message) {
                     last_validation_log = error_message;
                 },
             .write_recording_lock =
                 [this](const std::string& episode_dir) {
                     lock_events.push_back("write:" + episode_dir);
                     return write_lock_ok;
                 },
             .remove_recording_lock =
                 [this]() {
                     lock_events.push_back("remove");
                 },
             .start_worker =
                 [this](WorkerName worker, const ProcessSpec& spec, std::string*) {
                     started_specs[worker] = spec;
                     const bool should_start = worker == WorkerName::CameraRecorder ? camera_start_ok : sensor_start_ok;
                     if (worker == WorkerName::SensorRecorder)
                     {
                         sensor_args = spec.argv;
                         sensor_inherited_fds = spec.inherited_fds;
                     }
                     if (!should_start)
                     {
                         worker_status[worker] = ProcessStatus{
                             .state = ProcessState::ExitedUnexpected,
                             .running = false,
                             .pid = 0,
                         };
                         return false;
                     }
                     worker_status[worker] = ProcessStatus{
                         .state = ProcessState::Running,
                         .running = true,
                         .pid = spec.name == "camera_recorder" ? 101 : 202,
                     };
                     return true;
                 },
             .stop_worker =
                 [this](WorkerName worker, const std::string& reason, std::string*) {
                     const char* worker_name =
                         worker == WorkerName::CameraRecorder ? "camera_recorder" : "sensor_recorder";
                     stop_reasons.push_back(std::string(worker_name) + ":" + reason);
                     worker_status[worker].running = false;
                     worker_status[worker].state = ProcessState::ExitedExpected;
                     return true;
                 },
             .get_worker_status =
                 [this](WorkerName worker) {
                     const auto it = worker_status.find(worker);
                     return it == worker_status.end() ? ProcessStatus{} : it->second;
                 },
             .log_info =
                 [this](const std::string& message) {
                     logs.push_back("I:" + message);
                 },
             .log_warn =
                 [this](const std::string& message) {
                     logs.push_back("W:" + message);
                 },
             .log_error =
                 [this](const std::string& message) {
                     logs.push_back("E:" + message);
                 },
             .sync_runtime_log =
                 [this](const std::string& reason) {
                     sync_reason = reason;
                 }},
            &FakeEpochUs,
            &FakeSteadyMs);
    }
};

TEST(RecordingOrchestratorTest, StartRecordingUpdatesStateAndSignalsReadyFlow)
{
    RecordingHarness harness;
    auto orchestrator = harness.Make();

    ASSERT_TRUE(orchestrator.StartRecording(false));
    EXPECT_TRUE(orchestrator.state().is_recording);
    EXPECT_EQ(orchestrator.state().current_episode_dir, "/tmp/episode_0001");
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Recording);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "recording_started");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "");
    EXPECT_EQ(harness.sensor_args,
              (std::vector<std::string>{
                  "/tmp/sensor_recorder",
                  "/tmp/episode_0001",
                  "--extra-test-fd",
                  "99",
              }));
    EXPECT_EQ(harness.sensor_inherited_fds, (std::vector<int>{99}));
    EXPECT_EQ(harness.lock_events,
              (std::vector<std::string>{"remove", "write:", "write:/tmp/episode_0001"}));
    ASSERT_TRUE(harness.started_specs.count(WorkerName::CameraRecorder) > 0);
    EXPECT_EQ(harness.started_specs[WorkerName::CameraRecorder].stop_mode,
              ugripper::runtime::ProcessStopMode::SigTermThenKill);
    ASSERT_TRUE(harness.started_specs.count(WorkerName::SensorRecorder) > 0);
    EXPECT_EQ(harness.started_specs[WorkerName::SensorRecorder].stop_mode,
              ugripper::runtime::ProcessStopMode::SigTermThenKill);
}

TEST(RecordingOrchestratorTest, StopRecordingHandlesValidationFailure)
{
    RecordingHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    harness.validate_ok = false;
    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_TRUE(orchestrator.state().current_episode_dir.empty());
    EXPECT_EQ(orchestrator.state().last_episode_dir, "/tmp/episode_0001");
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error1);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "validation_failed");
    EXPECT_EQ(harness.sync_reason, "video stop");
    EXPECT_FALSE(harness.last_validation_log.empty());
}

TEST(RecordingOrchestratorTest, CheckRecorderProcessesUsesWorkerStatus)
{
    RecordingHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    harness.worker_status[WorkerName::SensorRecorder].running = false;
    harness.worker_status[WorkerName::SensorRecorder].last_exit_code = 9;

    EXPECT_FALSE(orchestrator.CheckRecorderProcesses());
}

TEST(RecordingOrchestratorTest, StopRecordingSuccessSignalsWritingThenReady)
{
    RecordingHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_TRUE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_TRUE(orchestrator.state().current_episode_dir.empty());
    EXPECT_EQ(orchestrator.state().last_episode_dir, "/tmp/episode_0001");

    EXPECT_EQ(harness.flushed_stages,
              (std::vector<std::string>{"pre_stereo_finalize", "final"}));
    EXPECT_EQ(harness.audio_commands,
              (std::vector<std::string>{"recording_started", "recording_stop", "writing", "ready"}));
    EXPECT_EQ(harness.recovery_commands,
              (std::vector<std::string>{"", "writing", "ready"}));
    EXPECT_EQ(harness.led_states,
              (std::vector<RuntimeLedState>{
                  RuntimeLedState::Recording,
                  RuntimeLedState::Ready,
                  RuntimeLedState::Writing,
                  RuntimeLedState::Ready,
              }));
    EXPECT_EQ(harness.sync_reason, "video stop");
    EXPECT_EQ(harness.lock_events,
              (std::vector<std::string>{"remove", "write:", "write:/tmp/episode_0001", "remove"}));
}

TEST(RecordingOrchestratorTest, StartRecordingLockFailureDoesNotLaunchWorkers)
{
    RecordingHarness harness;
    harness.write_lock_ok = false;
    auto orchestrator = harness.Make();

    ASSERT_FALSE(orchestrator.StartRecording(false));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_TRUE(harness.started_specs.empty());
    EXPECT_EQ(harness.lock_events,
              (std::vector<std::string>{"remove", "write:"}));
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
}

TEST(RecordingOrchestratorTest, StopRecordingStereoFinalizeFailureMapsToValidationFailed)
{
    RecordingHarness harness;
    harness.wait_finalize_ok = false;
    harness.wait_finalize_error = "stereo finalize timeout";
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_TRUE(orchestrator.state().current_episode_dir.empty());
    EXPECT_EQ(orchestrator.state().last_episode_dir, "/tmp/episode_0001");
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error1);
    EXPECT_EQ(harness.audio_commands.back(), "validation_failed");
    EXPECT_EQ(harness.recovery_commands.back(), "validation_failed");
    EXPECT_EQ(harness.last_validation_log, "stereo finalize timeout");
    EXPECT_EQ(harness.validate_calls, 0);
    EXPECT_EQ(harness.sync_reason, "video stop");
}

TEST(RecordingOrchestratorTest, StopRecordingRightStereoControlFailureMapsToError4)
{
    RecordingHarness harness;
    harness.wait_finalize_ok = false;
    harness.wait_finalize_error = "right Fays recorder unhealthy during session";
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error4);
    ASSERT_FALSE(harness.hardware_fault_led_states.empty());
    EXPECT_EQ(harness.hardware_fault_led_states.back().side, HardwareFaultSide::Right);
    EXPECT_EQ(harness.hardware_fault_led_states.back().key,
              ugripper::runtime::kErrorTypeStereoControlFailed);
    EXPECT_EQ(harness.audio_commands.back(), "error");
    EXPECT_EQ(harness.recovery_commands.back(), "error");
    EXPECT_NE(harness.last_validation_log.find("unplug/replug the right gripper"), std::string::npos);
    ASSERT_FALSE(harness.metadata_error_types.empty());
    EXPECT_EQ(harness.metadata_error_types.back(),
              ugripper::runtime::kErrorTypeStereoControlFailed);
    EXPECT_EQ(harness.validate_calls, 0);
}

TEST(RecordingOrchestratorTest, StartRecordingMissingCameraBinaryMapsToError)
{
    RecordingHarness harness;
    harness.camera_bin_exists = false;
    auto orchestrator = harness.Make();

    ASSERT_FALSE(orchestrator.StartRecording(false));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_EQ(orchestrator.state().current_episode_dir, "/tmp/episode_0001");
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error5);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
}

TEST(RecordingOrchestratorTest, StartRecordingMissingSensorBinaryMapsToError)
{
    RecordingHarness harness;
    harness.sensor_bin_exists = false;
    auto orchestrator = harness.Make();

    ASSERT_FALSE(orchestrator.StartRecording(false));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_EQ(orchestrator.state().current_episode_dir, "/tmp/episode_0001");
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error5);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
}

TEST(RecordingOrchestratorTest, StartRecordingWorkerStartupFailureRollsBackStartedWorker)
{
    RecordingHarness harness;
    harness.sensor_start_ok = false;
    auto orchestrator = harness.Make();

    ASSERT_FALSE(orchestrator.StartRecording(false));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_EQ(orchestrator.state().current_episode_dir, "/tmp/episode_0001");
    EXPECT_EQ(harness.stop_reasons,
              (std::vector<std::string>{"camera_recorder:startup rollback"}));
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error5);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
}

TEST(RecordingOrchestratorTest, StartRecordingStereoStartFailureStopsWorkersAndSignalsError)
{
    RecordingHarness harness;
    harness.stereo_start_ok = false;
    auto orchestrator = harness.Make();

    ASSERT_FALSE(orchestrator.StartRecording(false));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_EQ(orchestrator.state().current_episode_dir, "/tmp/episode_0001");
    EXPECT_EQ(harness.stop_reasons,
              (std::vector<std::string>{
                  "camera_recorder:stereo session start failed",
                  "sensor_recorder:stereo session start failed",
              }));
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error5);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
}

TEST(RecordingOrchestratorTest, StopRecordingDueToErrorMapsToErrorOutput)
{
    RecordingHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(true, "recorder crashed"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_TRUE(orchestrator.state().current_episode_dir.empty());
    EXPECT_EQ(orchestrator.state().last_episode_dir, "/tmp/episode_0001");
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error5);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
    EXPECT_EQ(harness.sync_reason, "video stop");
}

TEST(RecordingOrchestratorTest, StopRecordingDueToDiskFaultMapsToError3Output)
{
    RecordingHarness harness;
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(
        true,
        "recording hardware fault (disk_full): Disk root has no available space: /mnt/data_disk",
        "disk_full"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error3);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
}

TEST(RecordingOrchestratorTest, StopRecordingDiskValidationFailureMapsToError3Output)
{
    RecordingHarness harness;
    harness.validate_ok = false;
    harness.validate_error_types = {"disk_full"};
    harness.validate_error =
        "failed to write temp file: /mnt/data_disk/test.tmp error=No space left on device";
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error3);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
    EXPECT_EQ(harness.last_validation_log,
              "failed to write temp file: /mnt/data_disk/test.tmp error=No space left on device");
    ASSERT_FALSE(harness.metadata_error_types.empty());
    EXPECT_EQ(harness.metadata_error_types.back(), "disk_full");
}

TEST(RecordingOrchestratorTest, StopRecordingMissingFileUnderDataDiskMapsToValidationFailed)
{
    RecordingHarness harness;
    harness.validate_ok = false;
    harness.validate_error_types = {ugripper::runtime::kErrorTypeMissingFile};
    harness.validate_error =
        "missing or empty video file: /mnt/data_disk/device/data/episode_0001-temp/cam_left.mkv";
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error1);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "validation_failed");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "validation_failed");
    EXPECT_EQ(harness.last_validation_log,
              "missing or empty video file: /mnt/data_disk/device/data/episode_0001-temp/cam_left.mkv");
    ASSERT_FALSE(harness.metadata_error_types.empty());
    EXPECT_EQ(harness.metadata_error_types.back(), ugripper::runtime::kErrorTypeMissingFile);
}

TEST(RecordingOrchestratorTest, StopRecordingMultipleErrorTypesSelectsMostImportantOutput)
{
    RecordingHarness harness;
    harness.validate_ok = false;
    harness.validate_error_types = {ugripper::runtime::kErrorTypeMissingFile};
    harness.validate_error =
        "missing or empty video file: /mnt/data_disk/device/data/episode_0001-temp/cam_left.mkv";
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(
        true,
        "recording hardware fault (disk_mount_lost): Disk root is not mounted",
        "disk_mount_lost"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error3);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "error");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "error");
    ASSERT_FALSE(harness.metadata_error_types.empty());
    EXPECT_EQ(harness.metadata_error_types.back(), "disk_mount_lost");
    EXPECT_EQ(harness.last_validation_log,
              "recording hardware fault (disk_mount_lost): Disk root is not mounted; "
              "missing or empty video file: /mnt/data_disk/device/data/episode_0001-temp/cam_left.mkv");
}

TEST(RecordingOrchestratorTest, StopRecordingMergeFailureMapsToValidationFailed)
{
    RecordingHarness harness;
    harness.merge_ok = false;
    harness.merge_error = "episode info merge failed";
    auto orchestrator = harness.Make();
    ASSERT_TRUE(orchestrator.StartRecording(false));

    g_steady_ms += 100;
    ASSERT_FALSE(orchestrator.StopRecording(false, "stop"));
    EXPECT_FALSE(orchestrator.state().is_recording);
    EXPECT_TRUE(orchestrator.state().current_episode_dir.empty());
    EXPECT_EQ(orchestrator.state().last_episode_dir, "/tmp/episode_0001");
    ASSERT_FALSE(harness.led_states.empty());
    EXPECT_EQ(harness.led_states.back(), RuntimeLedState::Error1);
    ASSERT_FALSE(harness.audio_commands.empty());
    EXPECT_EQ(harness.audio_commands.back(), "validation_failed");
    ASSERT_FALSE(harness.recovery_commands.empty());
    EXPECT_EQ(harness.recovery_commands.back(), "validation_failed");
    EXPECT_EQ(harness.last_validation_log, "episode info merge failed");
    EXPECT_EQ(harness.sync_reason, "video stop");
}

}  // namespace
