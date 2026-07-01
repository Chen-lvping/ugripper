#pragma once

#include "record_runtime/process_supervisor.h"
#include "record_runtime/runtime_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ugripper::runtime {

struct RecordingOrchestratorOptions
{
    std::string camera_recorder_bin;
    std::string sensor_recorder_bin;
    std::string camera_codec;
    std::string session_camera_streams_csv;
    int worker_stop_timeout_ms = 5000;
    int stereo_finalize_timeout_ms = 10000;
};

class RecordingOrchestrator
{
public:
    using EpochUsFn = int64_t (*)();
    using SteadyMsFn = int64_t (*)();
    using EpisodeDirFn = std::function<std::string()>;
    using PrepareEpisodeFn = std::function<bool(const std::string&, bool, const std::string&, std::string*)>;
    using ValidateEpisodeFn = std::function<bool(const std::string&, std::string*, std::vector<std::string>*)>;
    using PrepareSensorStartFn = std::function<void(std::vector<std::string>*, std::vector<int>*)>;
    using FinalizeSensorStartFn = std::function<void()>;
    using AttachPendingPreAudioFn = std::function<bool(const std::string&)>;
    using MergeEpisodeInfoFn = std::function<bool(const std::string&, std::string*)>;
    using PathExistsFn = std::function<bool(const std::string&)>;
    using RecoveryCommandFn = std::function<void(const std::string&)>;
    using AudioCommandFn = std::function<void(const std::string&)>;
    using LedStateFn = std::function<void(RuntimeLedState, double)>;
    using HardwareFaultLedFn = std::function<void(const HealthFault&)>;
    using FailureStateFn = std::function<void(RuntimeLedState, const std::vector<std::string>&, const std::string&)>;
    using StereoSessionFn = std::function<bool(const std::string&, int64_t, std::string*)>;
    using StartEgoRecordingFn = std::function<bool(const std::string&, int64_t, std::string*)>;
    using StopEgoRecordingFn = std::function<bool(const std::string&, int64_t, std::string*)>;
    using WaitForFinalizeFn = std::function<bool(const std::string&, int, std::string*)>;
    using CleanupEgoRemoteFn = std::function<bool(const std::string&, std::string*)>;
    using FlushEpisodeArtifactsFn = std::function<void(const std::string&, const char*)>;
    using WriteEpisodeMetadataFn = std::function<void(const std::string&, bool, const std::string&, const std::string&)>;
    using WriteValidationErrorLogFn = std::function<void(const std::string&, const std::string&)>;
    using FinalizeEpisodeDirFn = std::function<std::string(const std::string&, std::string*)>;
    using DetectRecordingHardwareFaultFn = std::function<std::optional<HealthFault>(const std::string&)>;
    using WriteRecordingLockFn = std::function<bool(const std::string&)>;
    using RemoveRecordingLockFn = std::function<void()>;
    using StartWorkerFn = std::function<bool(WorkerName, const ProcessSpec&, std::string*)>;
    using StopWorkerFn = std::function<bool(WorkerName, const std::string&, std::string*)>;
    using GetWorkerStatusFn = std::function<ProcessStatus(WorkerName)>;
    using LogFn = std::function<void(const std::string&)>;
    using SyncRuntimeLogFn = std::function<void(const std::string&)>;

    struct Dependencies
    {
        EpisodeDirFn create_next_episode_dir;
        PrepareEpisodeFn prepare_episode;
        ValidateEpisodeFn validate_episode;
        PrepareSensorStartFn prepare_sensor_start;
        FinalizeSensorStartFn finalize_sensor_start;
        AttachPendingPreAudioFn attach_pending_pre_audio;
        MergeEpisodeInfoFn merge_episode_info;
        PathExistsFn path_exists;
        RecoveryCommandFn set_audio_recovery_command;
        AudioCommandFn send_audio_command;
        LedStateFn set_led_state;
        HardwareFaultLedFn set_hardware_fault_led_state;
        FailureStateFn handle_failure_state;
        StereoSessionFn start_stereo_session;
        StereoSessionFn stop_stereo_session;
        StartEgoRecordingFn start_ego_recording;
        StopEgoRecordingFn stop_ego_recording;
        WaitForFinalizeFn wait_for_ego_finalize;
        WaitForFinalizeFn wait_for_stereo_finalize;
        CleanupEgoRemoteFn cleanup_ego_remote;
        FlushEpisodeArtifactsFn flush_episode_artifacts;
        WriteEpisodeMetadataFn write_episode_metadata;
        WriteValidationErrorLogFn write_validation_error_log;
        FinalizeEpisodeDirFn finalize_episode_dir;
        DetectRecordingHardwareFaultFn detect_recording_hardware_fault;
        WriteRecordingLockFn write_recording_lock;
        RemoveRecordingLockFn remove_recording_lock;
        StartWorkerFn start_worker;
        StopWorkerFn stop_worker;
        GetWorkerStatusFn get_worker_status;
        LogFn log_info;
        LogFn log_perf;
        LogFn log_warn;
        LogFn log_error;
        SyncRuntimeLogFn sync_runtime_log;
    };

    RecordingOrchestrator(RecordingOrchestratorOptions options,
                          Dependencies dependencies,
                          EpochUsFn epoch_us_fn,
                          SteadyMsFn steady_ms_fn);

    bool StartRecording(bool reset_recording, std::string* error_message = nullptr);
    bool StopRecording(bool due_to_error,
                       const std::string& reason,
                       const std::string& error_type = "",
                       std::string* error_message = nullptr);
    bool CheckRecorderProcesses() const;

    const RecordingState& state() const;

private:
    RecordingOrchestratorOptions options_;
    Dependencies dependencies_;
    EpochUsFn epoch_us_fn_ = nullptr;
    SteadyMsFn steady_ms_fn_ = nullptr;
    RecordingState state_{};
};

}  // namespace ugripper::runtime
