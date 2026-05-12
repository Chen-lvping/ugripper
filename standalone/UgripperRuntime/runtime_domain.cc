#include "record_runtime/health_monitor.h"
#include "record_runtime/hmi_controller.h"
#include "record_runtime/recording_orchestrator.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

std::string JoinStrings(const std::vector<std::string>& items, const char* separator)
{
    std::ostringstream stream;
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (index > 0)
        {
            stream << separator;
        }
        stream << items[index];
    }
    return stream.str();
}

std::string AppendHmiPortActivity(std::string detail, const std::vector<std::string>& port_activity)
{
    if (!port_activity.empty())
    {
        detail += "; port_activity=" + JoinStrings(port_activity, ", ");
    }
    return detail;
}

void CallLog(const ugripper::runtime::RecordingOrchestrator::LogFn& log_fn, const std::string& message)
{
    if (log_fn != nullptr)
    {
        log_fn(message);
    }
}

}  // namespace

namespace ugripper::runtime {

HmiController::HmiController(HmiControllerOptions options, NowMsFn now_ms_fn)
    : options_(std::move(options)), now_ms_fn_(now_ms_fn)
{
}

std::vector<HmiEvent> HmiController::HandleButtons(const ButtonSnapshot& buttons)
{
    std::vector<HmiEvent> events;
    const uint64_t now_ms = now_ms_fn_ != nullptr ? now_ms_fn_() : 0;

    if (buttons.up_pressed && !last_buttons_.up_pressed)
    {
        tracker_.up_pressed_since_ms = now_ms;
        tracker_.up_long_handled = false;
    }
    if (buttons.down_pressed && !last_buttons_.down_pressed)
    {
        tracker_.down_pressed_since_ms = now_ms;
        tracker_.down_long_handled = false;
    }

    if (!buttons.up_pressed)
    {
        tracker_.up_pressed_since_ms = 0;
    }
    if (!buttons.down_pressed)
    {
        tracker_.down_pressed_since_ms = 0;
    }

    if (buttons.up_pressed && buttons.down_pressed)
    {
        if (!tracker_.dual_chord_active)
        {
            tracker_.dual_chord_active = true;
            tracker_.both_pressed_since_ms = now_ms;
            tracker_.dual_long_handled = false;
            tracker_.shutdown_prompt_played = false;
        }

        const uint64_t dual_held_ms = now_ms - tracker_.both_pressed_since_ms;
        if (dual_held_ms >= options_.shutdown_prompt_threshold_ms && !tracker_.shutdown_prompt_played)
        {
            tracker_.shutdown_prompt_played = true;
            events.push_back({HmiEventType::ShutdownPromptRequested});
        }
        if (dual_held_ms >= options_.dual_long_press_threshold_ms && !tracker_.dual_long_handled)
        {
            tracker_.dual_long_handled = true;
            last_button_action_ms_ = now_ms;
            events.push_back({HmiEventType::ShutdownRequested});
        }

        last_buttons_ = buttons;
        return events;
    }

    if (tracker_.dual_chord_active)
    {
        if (!buttons.up_pressed && !buttons.down_pressed)
        {
            tracker_.dual_chord_active = false;
            tracker_.both_pressed_since_ms = 0;
            tracker_.dual_long_handled = false;
            tracker_.shutdown_prompt_played = false;
        }
        last_buttons_ = buttons;
        return events;
    }

    if (buttons.up_pressed && !tracker_.up_long_handled && tracker_.up_pressed_since_ms > 0 &&
        (now_ms - tracker_.up_pressed_since_ms) >= options_.long_press_threshold_ms)
    {
        tracker_.up_long_handled = true;
        last_button_action_ms_ = now_ms;
        events.push_back({HmiEventType::LongUpPressed});
    }

    if (buttons.down_pressed && !tracker_.down_long_handled && tracker_.down_pressed_since_ms > 0 &&
        (now_ms - tracker_.down_pressed_since_ms) >= options_.long_press_threshold_ms)
    {
        tracker_.down_long_handled = true;
        last_button_action_ms_ = now_ms;
        events.push_back({HmiEventType::LongDownPressed});
    }

    const bool up_released = last_buttons_.up_pressed && !buttons.up_pressed;
    const bool down_released = last_buttons_.down_pressed && !buttons.down_pressed;

    if (up_released && !tracker_.up_long_handled &&
        (now_ms - last_button_action_ms_) >= options_.action_debounce_ms)
    {
        last_button_action_ms_ = now_ms;
        events.push_back({HmiEventType::ShortUpPressed});
    }

    if (down_released && !tracker_.down_long_handled &&
        (now_ms - last_button_action_ms_) >= options_.action_debounce_ms)
    {
        last_button_action_ms_ = now_ms;
        events.push_back({HmiEventType::ShortDownPressed});
    }

    if (up_released)
    {
        tracker_.up_long_handled = false;
    }
    if (down_released)
    {
        tracker_.down_long_handled = false;
    }

    last_buttons_ = buttons;
    return events;
}

void HmiController::Reset()
{
    last_buttons_ = {};
    tracker_ = {};
    last_button_action_ms_ = 0;
}

HealthMonitor::HealthMonitor(HealthMonitorOptions options, Dependencies dependencies, NowMsFn now_ms_fn)
    : options_(std::move(options)), dependencies_(std::move(dependencies)), now_ms_fn_(now_ms_fn)
{
}

HealthMonitor::PollResult HealthMonitor::Poll(const HealthState& current_state) const
{
    PollResult result;
    result.state = current_state;

    const uint64_t now_ms = now_ms_fn_ != nullptr ? now_ms_fn_() : current_state.last_check_ms;
    if ((now_ms - current_state.last_check_ms) < options_.poll_interval_ms)
    {
        return result;
    }

    result.checked = true;
    result.state.last_check_ms = now_ms;
    result.fault = EvaluateHealth();
    if (result.fault.has_value())
    {
        const std::string error_key = result.fault->key + "|" + result.fault->detail;
        result.should_notify_fault =
            current_state.status != HealthStatus::Error || current_state.last_error_key != error_key;
        result.state.status = HealthStatus::Error;
        result.state.last_error_key = error_key;
        return result;
    }

    result.recovered = current_state.status == HealthStatus::Error;
    result.state.status = HealthStatus::Ok;
    result.state.last_error_key.clear();
    return result;
}

std::optional<HealthFault> HealthMonitor::EvaluateHealth() const
{
    if (dependencies_.is_disk_writable != nullptr && !dependencies_.is_disk_writable(options_.disk_root))
    {
        return HealthFault{
            RuntimeLedState::Error1,
            "disk_not_writable",
            "Disk not writable or mount lost: " + options_.disk_root,
        };
    }

    if (dependencies_.path_exists != nullptr)
    {
        std::vector<std::string> missing_device_paths;
        for (const std::string& path : options_.critical_device_paths)
        {
            if (!path.empty() && !dependencies_.path_exists(path))
            {
                missing_device_paths.push_back(path);
            }
        }
        if (!missing_device_paths.empty())
        {
            return HealthFault{
                RuntimeLedState::Error2,
                "critical_devices_missing",
                "Critical device nodes missing: " + JoinStrings(missing_device_paths, ", "),
            };
        }
    }

    if (dependencies_.get_process_status != nullptr &&
        dependencies_.get_process_status(WorkerName::StereoDaemon).pid <= 0)
    {
        return HealthFault{
            RuntimeLedState::Error2,
            "stereo_daemon_not_running",
            "Stereo warmup daemon is not running",
        };
    }

    std::ifstream stereo_status_input(options_.stereo_status_file);
    if (!stereo_status_input.is_open())
    {
        return HealthFault{
            RuntimeLedState::Error2,
            "stereo_status_missing",
            "Stereo status file missing: " + options_.stereo_status_file,
        };
    }

    try
    {
        json stereo_status = json::parse(stereo_status_input);
        if (!stereo_status.value("ready", false))
        {
            return HealthFault{
                RuntimeLedState::Error2,
                "stereo_not_ready",
                "Stereo warmup not ready: " + stereo_status.value("service_state", std::string("unknown")),
            };
        }
    }
    catch (const std::exception& ex)
    {
        return HealthFault{
            RuntimeLedState::Error2,
            "stereo_status_invalid",
            std::string("Stereo status invalid: ") + ex.what(),
        };
    }

    if (dependencies_.get_hmi_health != nullptr)
    {
        const HmiHealthSnapshot hmi_health =
            dependencies_.get_hmi_health(options_.hmi_active_timeout_ms);
        if (!hmi_health.has_connected_device)
        {
            return HealthFault{
                RuntimeLedState::Error2,
                "hmi_all_disconnected",
                "All HMI ports are disconnected",
            };
        }
        if (!hmi_health.disconnected_ports.empty())
        {
            return HealthFault{
                RuntimeLedState::Error2,
                "hmi_ports_disconnected",
                "HMI ports disconnected: " + JoinStrings(hmi_health.disconnected_ports, ", "),
            };
        }
        if (!hmi_health.input_connected)
        {
            return HealthFault{
                RuntimeLedState::Error2,
                "hmi_input_disconnected",
                "Input HMI port disconnected",
            };
        }
        if (!hmi_health.input_active)
        {
            std::string detail =
                "Input HMI port inactive for more than " + std::to_string(options_.hmi_active_timeout_ms) + "ms";
            if (hmi_health.input_last_rx_age_ms > 0)
            {
                detail += " (input_age_ms=" + std::to_string(hmi_health.input_last_rx_age_ms) + ")";
            }
            return HealthFault{
                RuntimeLedState::Error2,
                "hmi_input_inactive",
                AppendHmiPortActivity(std::move(detail), hmi_health.port_activity),
            };
        }
        if (!hmi_health.inactive_ports.empty())
        {
            const std::vector<std::string>& inactive_detail_source =
                hmi_health.inactive_port_details.empty() ? hmi_health.inactive_ports : hmi_health.inactive_port_details;
            return HealthFault{
                RuntimeLedState::Error2,
                "hmi_ports_inactive",
                AppendHmiPortActivity("HMI ports inactive: " + JoinStrings(inactive_detail_source, ", "),
                                      hmi_health.port_activity),
            };
        }
    }

    return std::nullopt;
}

RecordingOrchestrator::RecordingOrchestrator(RecordingOrchestratorOptions options,
                                             Dependencies dependencies,
                                             EpochUsFn epoch_us_fn,
                                             SteadyMsFn steady_ms_fn)
    : options_(std::move(options)),
      dependencies_(std::move(dependencies)),
      epoch_us_fn_(epoch_us_fn),
      steady_ms_fn_(steady_ms_fn)
{
}

bool RecordingOrchestrator::StartRecording(bool reset_recording, std::string* error_message)
{
    if (dependencies_.remove_recording_lock != nullptr)
    {
        dependencies_.remove_recording_lock();
    }
    if (dependencies_.write_recording_lock != nullptr &&
        !dependencies_.write_recording_lock(""))
    {
        CallLog(dependencies_.log_error, "failed to write recording lock before episode setup");
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (error_message != nullptr)
        {
            *error_message = "failed to write recording lock";
        }
        return false;
    }

    state_.current_episode_dir =
        dependencies_.create_next_episode_dir != nullptr ? dependencies_.create_next_episode_dir() : std::string();
    if (state_.current_episode_dir.empty())
    {
        CallLog(dependencies_.log_error, "failed to create episode directory");
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (error_message != nullptr)
        {
            *error_message = "failed to create episode directory";
        }
        return false;
    }

    std::string local_error;
    const std::string reset_source = reset_recording ? state_.last_episode_dir : std::string();
    if (dependencies_.prepare_episode == nullptr ||
        !dependencies_.prepare_episode(state_.current_episode_dir, reset_recording, reset_source, &local_error))
    {
        CallLog(dependencies_.log_error, "prepare episode failed: " + local_error);
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (error_message != nullptr)
        {
            *error_message = local_error;
        }
        return false;
    }

    if (dependencies_.attach_pending_pre_audio != nullptr)
    {
        dependencies_.attach_pending_pre_audio(state_.current_episode_dir);
    }

    if (dependencies_.path_exists != nullptr && !dependencies_.path_exists(options_.camera_recorder_bin))
    {
        CallLog(dependencies_.log_error, "camera recorder binary not found: " + options_.camera_recorder_bin);
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (error_message != nullptr)
        {
            *error_message = "camera recorder binary not found";
        }
        return false;
    }

    if (dependencies_.path_exists != nullptr && !dependencies_.path_exists(options_.sensor_recorder_bin))
    {
        CallLog(dependencies_.log_error, "sensor recorder binary not found: " + options_.sensor_recorder_bin);
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (error_message != nullptr)
        {
            *error_message = "sensor recorder binary not found";
        }
        return false;
    }

    if (dependencies_.write_recording_lock != nullptr &&
        !dependencies_.write_recording_lock(state_.current_episode_dir))
    {
        CallLog(dependencies_.log_error,
                "failed to update recording lock for episode " + state_.current_episode_dir);
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (error_message != nullptr)
        {
            *error_message = "failed to update recording lock";
        }
        return false;
    }

    const int64_t session_start_system_time_us = epoch_us_fn_ != nullptr ? epoch_us_fn_() : 0;
    const std::vector<std::string> camera_args = {
        options_.camera_recorder_bin,
        "--codec",
        options_.camera_codec,
        "--output-dir",
        state_.current_episode_dir,
        "--only",
        options_.session_camera_streams_csv,
    };
    const std::vector<std::string> sensor_args = {
        options_.sensor_recorder_bin,
        state_.current_episode_dir,
    };
    std::vector<std::string> sensor_args_with_extras = sensor_args;
    std::vector<int> sensor_inherited_fds;
    if (dependencies_.prepare_sensor_start != nullptr)
    {
        dependencies_.prepare_sensor_start(&sensor_args_with_extras, &sensor_inherited_fds);
    }

    bool camera_started = false;
    bool sensor_started = false;
    std::thread camera_thread([&]() {
        if (dependencies_.start_worker != nullptr)
        {
            camera_started = dependencies_.start_worker(
                WorkerName::CameraRecorder,
                {
                    .name = "camera_recorder",
                    .argv = camera_args,
                    .stop_timeout_ms = options_.worker_stop_timeout_ms,
                    .stop_mode = ProcessStopMode::SigTermThenKill,
                    .restart_policy = ProcessRestartPolicy::Never,
                },
                nullptr);
        }
    });
    std::thread sensor_thread([&]() {
        if (dependencies_.start_worker != nullptr)
        {
            sensor_started = dependencies_.start_worker(
                WorkerName::SensorRecorder,
                {
                    .name = "sensor_recorder",
                    .argv = sensor_args_with_extras,
                    .inherited_fds = sensor_inherited_fds,
                    .stop_timeout_ms = options_.worker_stop_timeout_ms,
                    .stop_mode = ProcessStopMode::SigTermThenKill,
                    .restart_policy = ProcessRestartPolicy::Never,
                },
                nullptr);
        }
    });
    camera_thread.join();
    sensor_thread.join();
    if (dependencies_.finalize_sensor_start != nullptr)
    {
        dependencies_.finalize_sensor_start();
    }

    if (!camera_started || !sensor_started)
    {
        if (!camera_started)
        {
            CallLog(dependencies_.log_error, "failed to launch camera recorder");
        }
        if (!sensor_started)
        {
            CallLog(dependencies_.log_error, "failed to launch sensor recorder");
        }
        if (camera_started && dependencies_.stop_worker != nullptr)
        {
            dependencies_.stop_worker(WorkerName::CameraRecorder, "startup rollback", nullptr);
        }
        if (sensor_started && dependencies_.stop_worker != nullptr)
        {
            dependencies_.stop_worker(WorkerName::SensorRecorder, "startup rollback", nullptr);
        }
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (error_message != nullptr)
        {
            *error_message = "failed to launch recording workers";
        }
        return false;
    }

    if (dependencies_.start_stereo_session == nullptr ||
        !dependencies_.start_stereo_session(state_.current_episode_dir, session_start_system_time_us, &local_error))
    {
        CallLog(dependencies_.log_error,
                "failed to start stereo session for episode " + state_.current_episode_dir);
        if (dependencies_.stop_worker != nullptr)
        {
            dependencies_.stop_worker(WorkerName::CameraRecorder, "stereo session start failed", nullptr);
            dependencies_.stop_worker(WorkerName::SensorRecorder, "stereo session start failed", nullptr);
        }
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        if (error_message != nullptr)
        {
            *error_message = local_error.empty() ? "failed to start stereo session" : local_error;
        }
        return false;
    }

    state_.is_recording = true;
    if (dependencies_.set_led_state != nullptr)
    {
        dependencies_.set_led_state(RuntimeLedState::Recording, 0.0);
    }
    if (dependencies_.set_audio_recovery_command != nullptr)
    {
        dependencies_.set_audio_recovery_command("");
    }
    if (dependencies_.send_audio_command != nullptr)
    {
        dependencies_.send_audio_command(reset_recording ? "reset_recording_start" : "recording_started");
    }
    CallLog(dependencies_.log_info, "recording started: " + state_.current_episode_dir);
    return true;
}

bool RecordingOrchestrator::StopRecording(bool due_to_error,
                                          const std::string& reason,
                                          std::string* error_message)
{
    if (!state_.is_recording)
    {
        if (dependencies_.remove_recording_lock != nullptr)
        {
            dependencies_.remove_recording_lock();
        }
        if (due_to_error)
        {
            if (dependencies_.set_led_state != nullptr)
            {
                dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
            }
            if (dependencies_.set_audio_recovery_command != nullptr)
            {
                dependencies_.set_audio_recovery_command("error");
            }
            if (dependencies_.send_audio_command != nullptr)
            {
                dependencies_.send_audio_command("error");
            }
        }
        return true;
    }

    const int64_t stop_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    const int64_t stop_system_time_us = epoch_us_fn_ != nullptr ? epoch_us_fn_() : 0;
    CallLog(dependencies_.log_info, "stopping recording: " + reason);
    CallLog(dependencies_.log_info,
            "[PERF] stop phase begin: reason=" + reason + " episode_dir=" + state_.current_episode_dir);

    std::string stereo_error;
    bool stereo_stop_ok = dependencies_.stop_stereo_session != nullptr &&
                          dependencies_.stop_stereo_session(state_.current_episode_dir, stop_system_time_us, &stereo_error);
    if (!stereo_stop_ok)
    {
        CallLog(dependencies_.log_error, "failed to send stereo stop command");
    }

    const int64_t camera_stop_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    const bool camera_stop_ok = dependencies_.stop_worker != nullptr &&
                                dependencies_.stop_worker(WorkerName::CameraRecorder, "recording stop", nullptr);
    CallLog(dependencies_.log_info,
            "[PERF] stop camera_recorder done: ok=" + std::string(camera_stop_ok ? "true" : "false") +
                " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : camera_stop_start_ms) -
                               camera_stop_start_ms));

    const int64_t sensor_stop_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    const bool sensor_stop_ok = dependencies_.stop_worker != nullptr &&
                                dependencies_.stop_worker(WorkerName::SensorRecorder, "recording stop", nullptr);
    CallLog(dependencies_.log_info,
            "[PERF] stop sensor_recorder done: ok=" + std::string(sensor_stop_ok ? "true" : "false") +
                " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : sensor_stop_start_ms) -
                               sensor_stop_start_ms));

    if (dependencies_.set_led_state != nullptr)
    {
        dependencies_.set_led_state(RuntimeLedState::Ready, 0.0);
    }
    if (dependencies_.send_audio_command != nullptr)
    {
        dependencies_.send_audio_command("recording_stop");
    }

    state_.is_recording = false;
    if (!state_.current_episode_dir.empty())
    {
        state_.last_episode_dir = state_.current_episode_dir;
    }

    if (dependencies_.set_audio_recovery_command != nullptr)
    {
        dependencies_.set_audio_recovery_command("writing");
    }
    if (dependencies_.send_audio_command != nullptr)
    {
        dependencies_.send_audio_command("writing");
    }
    if (dependencies_.set_led_state != nullptr)
    {
        dependencies_.set_led_state(RuntimeLedState::Init, 0.0);
    }

    const int64_t write_phase_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    CallLog(dependencies_.log_info, "[PERF] writing phase begin: episode_dir=" + state_.current_episode_dir);
    if (dependencies_.flush_episode_artifacts != nullptr)
    {
        dependencies_.flush_episode_artifacts(state_.current_episode_dir, "pre_stereo_finalize");
    }

    std::string stereo_finalize_error;
    std::string merge_error;
    if (stereo_stop_ok && dependencies_.wait_for_stereo_finalize != nullptr &&
        !dependencies_.wait_for_stereo_finalize(
            state_.current_episode_dir, options_.stereo_finalize_timeout_ms, &stereo_finalize_error))
    {
        CallLog(dependencies_.log_error, "stereo finalize failed: " + stereo_finalize_error);
        stereo_stop_ok = false;
    }
    if (stereo_stop_ok && dependencies_.merge_episode_info != nullptr &&
        !dependencies_.merge_episode_info(state_.current_episode_dir, &merge_error))
    {
        CallLog(dependencies_.log_error, "failed to merge episode info: " + merge_error);
        stereo_stop_ok = false;
    }
    if (dependencies_.flush_episode_artifacts != nullptr)
    {
        dependencies_.flush_episode_artifacts(state_.current_episode_dir, "final");
    }
    CallLog(dependencies_.log_info,
            "[PERF] writing phase end: episode_dir=" + state_.current_episode_dir +
                " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : write_phase_start_ms) -
                               write_phase_start_ms));

    std::string episode_validation_error;
    std::string final_error_message;
    const int64_t validate_phase_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    CallLog(dependencies_.log_info, "[PERF] validation phase begin: episode_dir=" + state_.current_episode_dir);
    const bool episode_valid = dependencies_.validate_episode != nullptr &&
                               dependencies_.validate_episode(state_.current_episode_dir, &episode_validation_error);
    if (!stereo_stop_ok)
    {
        if (!stereo_finalize_error.empty())
        {
            final_error_message = stereo_finalize_error;
        }
        else if (!merge_error.empty())
        {
            final_error_message = merge_error;
        }
        else
        {
            final_error_message = "stereo session finalize failed";
        }
    }
    if (!episode_valid)
    {
        if (final_error_message.empty())
        {
            final_error_message =
                episode_validation_error.empty() ? "episode validation failed" : episode_validation_error;
        }
        else if (!episode_validation_error.empty())
        {
            final_error_message += "; " + episode_validation_error;
        }
    }
    const bool valid = stereo_stop_ok && episode_valid;
    CallLog(dependencies_.log_info,
            "[PERF] validation phase end: episode_dir=" + state_.current_episode_dir +
                " valid=" + std::string(valid ? "true" : "false") + " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : validate_phase_start_ms) -
                               validate_phase_start_ms));
    if (!valid)
    {
        CallLog(dependencies_.log_error, "episode validation failed: " + final_error_message);
        if (dependencies_.write_validation_error_log != nullptr)
        {
            dependencies_.write_validation_error_log(state_.current_episode_dir, final_error_message);
        }
    }

    state_.current_episode_dir.clear();
    if (dependencies_.remove_recording_lock != nullptr)
    {
        dependencies_.remove_recording_lock();
    }

    if (due_to_error)
    {
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error5, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("error");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("error");
        }
        CallLog(dependencies_.log_info,
                "[PERF] stop phase end: due_to_error=true total_elapsed_ms=" +
                    std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : stop_start_ms) - stop_start_ms));
        if (dependencies_.sync_runtime_log != nullptr)
        {
            dependencies_.sync_runtime_log("video stop");
        }
        if (error_message != nullptr)
        {
            *error_message = final_error_message;
        }
        return false;
    }

    if (!valid)
    {
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(RuntimeLedState::Error1, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command("validation_failed");
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command("validation_failed");
        }
        CallLog(dependencies_.log_info,
                "[PERF] stop phase end: valid=false total_elapsed_ms=" +
                    std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : stop_start_ms) - stop_start_ms));
        if (dependencies_.sync_runtime_log != nullptr)
        {
            dependencies_.sync_runtime_log("video stop");
        }
        if (error_message != nullptr)
        {
            *error_message = final_error_message;
        }
        return false;
    }

    if (dependencies_.set_led_state != nullptr)
    {
        dependencies_.set_led_state(RuntimeLedState::Ready, 0.0);
    }
    if (dependencies_.set_audio_recovery_command != nullptr)
    {
        dependencies_.set_audio_recovery_command("ready");
    }
    if (dependencies_.send_audio_command != nullptr)
    {
        dependencies_.send_audio_command("ready");
    }
    CallLog(dependencies_.log_info,
            "[PERF] stop phase end: valid=true total_elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : stop_start_ms) - stop_start_ms));
    if (dependencies_.sync_runtime_log != nullptr)
    {
        dependencies_.sync_runtime_log("video stop");
    }
    return true;
}

bool RecordingOrchestrator::CheckRecorderProcesses() const
{
    const ProcessStatus camera_status = dependencies_.get_worker_status != nullptr
                                            ? dependencies_.get_worker_status(WorkerName::CameraRecorder)
                                            : ProcessStatus{};
    const ProcessStatus sensor_status = dependencies_.get_worker_status != nullptr
                                            ? dependencies_.get_worker_status(WorkerName::SensorRecorder)
                                            : ProcessStatus{};
    const bool camera_ok = camera_status.running;
    const bool sensor_ok = sensor_status.running;
    if (!camera_ok)
    {
        CallLog(dependencies_.log_error,
                "camera recorder exited, last_exit=" + std::to_string(camera_status.last_exit_code));
    }
    if (!sensor_ok)
    {
        CallLog(dependencies_.log_error,
                "sensor recorder exited, last_exit=" + std::to_string(sensor_status.last_exit_code));
    }
    return camera_ok && sensor_ok;
}

const RecordingState& RecordingOrchestrator::state() const
{
    return state_;
}

}  // namespace ugripper::runtime
