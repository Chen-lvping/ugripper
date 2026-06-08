#include "record_runtime/health_monitor.h"
#include "record_runtime/hmi_controller.h"
#include "record_runtime/recording_orchestrator.h"

#include <cstdint>
#include <future>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <thread>
#include <algorithm>
#include <cctype>
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

ugripper::runtime::HardwareFaultSide SideForText(const std::string& text)
{
    const bool has_left = text.find("left") != std::string::npos ||
                          text.find("/dev/cam_left") != std::string::npos ||
                          text.find("/dev/stereo_left") != std::string::npos ||
                          text.find("/dev/left_") != std::string::npos ||
                          text.find("/dev/tcam_left") != std::string::npos;
    const bool has_right = text.find("right") != std::string::npos ||
                           text.find("/dev/cam_right") != std::string::npos ||
                           text.find("/dev/stereo_right") != std::string::npos ||
                           text.find("/dev/right_") != std::string::npos ||
                           text.find("/dev/tcam_right") != std::string::npos;
    if (has_left && has_right)
    {
        return ugripper::runtime::HardwareFaultSide::Both;
    }
    if (has_left)
    {
        return ugripper::runtime::HardwareFaultSide::Left;
    }
    if (has_right)
    {
        return ugripper::runtime::HardwareFaultSide::Right;
    }
    return ugripper::runtime::HardwareFaultSide::Unknown;
}

ugripper::runtime::HardwareFaultSide MergeSides(ugripper::runtime::HardwareFaultSide lhs,
                                                ugripper::runtime::HardwareFaultSide rhs)
{
    using ugripper::runtime::HardwareFaultSide;
    if (lhs == rhs)
    {
        return lhs;
    }
    if (lhs == HardwareFaultSide::Unknown)
    {
        return rhs;
    }
    if (rhs == HardwareFaultSide::Unknown)
    {
        return lhs;
    }
    return HardwareFaultSide::Both;
}

void CallLog(const ugripper::runtime::RecordingOrchestrator::LogFn& log_fn, const std::string& message)
{
    if (log_fn != nullptr)
    {
        log_fn(message);
    }
}

void CallPerfLog(const ugripper::runtime::RecordingOrchestrator::Dependencies& dependencies,
                 const std::string& message)
{
    CallLog(dependencies.log_perf, message);
}

bool HasErrorType(const std::vector<std::string>& error_types, const std::string& target)
{
    return std::find(error_types.begin(), error_types.end(), target) != error_types.end();
}

void AddErrorType(std::vector<std::string>* error_types, const std::string& error_type)
{
    if (error_types == nullptr || error_type.empty())
    {
        return;
    }
    if (!HasErrorType(*error_types, error_type))
    {
        error_types->push_back(error_type);
    }
}

void AddErrorTypes(std::vector<std::string>* error_types, const std::vector<std::string>& new_error_types)
{
    for (const std::string& error_type : new_error_types)
    {
        AddErrorType(error_types, error_type);
    }
}

bool IsDiskErrorType(const std::string& error_type)
{
    return error_type == "disk_mount_lost" ||
           error_type == "disk_not_writable" ||
           error_type == "disk_full" ||
           error_type == "disk_space_unavailable";
}

bool IsHardwareErrorType(const std::string& error_type)
{
    return error_type == "critical_devices_missing" ||
           error_type == "stereo_daemon_not_running" ||
           error_type == "stereo_status_missing" ||
           error_type == "stereo_not_ready" ||
           error_type == "stereo_status_invalid" ||
           error_type == "hmi_all_disconnected" ||
           error_type == "hmi_ports_disconnected" ||
           error_type == "hmi_input_disconnected" ||
           error_type == "hmi_input_inactive" ||
           error_type == "hmi_ports_inactive" ||
           error_type == ugripper::runtime::kErrorTypeDeviceDisconnected;
}

std::string SelectPrimaryErrorType(const std::vector<std::string>& error_types)
{
    for (const std::string& error_type : error_types)
    {
        if (IsDiskErrorType(error_type))
        {
            return error_type;
        }
    }
    for (const std::string& error_type : error_types)
    {
        if (IsHardwareErrorType(error_type))
        {
            return error_type;
        }
    }
    for (const char* preferred : {
             ugripper::runtime::kErrorTypeRuntimeError,
             ugripper::runtime::kErrorTypeFinalizeError,
             ugripper::runtime::kErrorTypeCalibrationError,
             ugripper::runtime::kErrorTypeMissingFile,
             ugripper::runtime::kErrorTypeCollectionDurationTooShort,
             ugripper::runtime::kErrorTypeFrameLoss,
             ugripper::runtime::kErrorTypeUnknown,
         })
    {
        if (HasErrorType(error_types, preferred))
        {
            return preferred;
        }
    }
    return error_types.empty() ? std::string(ugripper::runtime::kErrorTypeUnknown) : error_types.front();
}

ugripper::runtime::RuntimeLedState SelectFailureLedState(const std::vector<std::string>& error_types,
                                                         bool due_to_error)
{
    for (const std::string& error_type : error_types)
    {
        if (IsDiskErrorType(error_type))
        {
            return ugripper::runtime::RuntimeLedState::Error3;
        }
    }
    for (const std::string& error_type : error_types)
    {
        if (IsHardwareErrorType(error_type))
        {
            return ugripper::runtime::RuntimeLedState::Error2;
        }
    }
    if (HasErrorType(error_types, ugripper::runtime::kErrorTypeRuntimeError))
    {
        return ugripper::runtime::RuntimeLedState::Error5;
    }
    return due_to_error ? ugripper::runtime::RuntimeLedState::Error5
                        : ugripper::runtime::RuntimeLedState::Error1;
}

std::string SelectStopFailureAudioCommand(ugripper::runtime::RuntimeLedState led_state)
{
    return led_state == ugripper::runtime::RuntimeLedState::Error1 ? "validation_failed" : "error";
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
    if (result.state.first_seen_ms == 0)
    {
        result.state.first_seen_ms = now_ms;
    }
    if ((now_ms - current_state.last_check_ms) < options_.poll_interval_ms)
    {
        return result;
    }

    result.checked = true;
    result.state.last_check_ms = now_ms;
    result.fault = EvaluateHealth();
    if (result.fault.has_value())
    {
        const bool startup_grace_active =
            current_state.status == HealthStatus::Unknown &&
            result.state.first_seen_ms != 0 &&
            (now_ms - result.state.first_seen_ms) < options_.stereo_startup_grace_ms &&
            (result.fault->key == "stereo_daemon_not_running" ||
             result.fault->key == "stereo_status_missing" ||
             result.fault->key == "stereo_status_invalid" ||
             result.fault->key == "stereo_not_ready");
        if (startup_grace_active)
        {
            result.fault.reset();
            result.state.status = current_state.status;
            result.state.last_error_key = current_state.last_error_key;
            return result;
        }
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
    if (dependencies_.get_disk_fault != nullptr)
    {
        if (const std::optional<HealthFault> disk_fault = dependencies_.get_disk_fault(options_.disk_root))
        {
            return disk_fault;
        }
    }
    else if (dependencies_.is_disk_writable != nullptr && !dependencies_.is_disk_writable(options_.disk_root))
    {
        return HealthFault{
            RuntimeLedState::Error3,
            HardwareFaultSide::Unknown,
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
            HardwareFaultSide side = HardwareFaultSide::Unknown;
            for (const std::string& path : missing_device_paths)
            {
                side = MergeSides(side, SideForText(path));
            }
            return HealthFault{
                RuntimeLedState::Error2,
                side,
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
            HardwareFaultSide::Unknown,
            "stereo_daemon_not_running",
            "Stereo warmup daemon is not running",
        };
    }

    std::ifstream stereo_status_input(options_.stereo_status_file);
    if (!stereo_status_input.is_open())
    {
        return HealthFault{
            RuntimeLedState::Error2,
            HardwareFaultSide::Unknown,
            "stereo_status_missing",
            "Stereo status file missing: " + options_.stereo_status_file,
        };
    }

    try
    {
        json stereo_status = json::parse(stereo_status_input);
        if (!stereo_status.value("ready", false))
        {
            HardwareFaultSide side = HardwareFaultSide::Unknown;
            if (stereo_status.contains("cameras") && stereo_status["cameras"].is_object())
            {
                const json& cameras = stereo_status["cameras"];
                if (cameras.contains("left_stereo") && cameras["left_stereo"].is_object() &&
                    !cameras["left_stereo"].value("ready", false))
                {
                    side = MergeSides(side, HardwareFaultSide::Left);
                }
                if (cameras.contains("right_stereo") && cameras["right_stereo"].is_object() &&
                    !cameras["right_stereo"].value("ready", false))
                {
                    side = MergeSides(side, HardwareFaultSide::Right);
                }
            }
            return HealthFault{
                RuntimeLedState::Error2,
                side,
                "stereo_not_ready",
                "Stereo warmup not ready: " + stereo_status.value("service_state", std::string("unknown")),
            };
        }
    }
    catch (const std::exception& ex)
    {
        return HealthFault{
            RuntimeLedState::Error2,
            HardwareFaultSide::Unknown,
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
                HardwareFaultSide::Both,
                "hmi_all_disconnected",
                "All HMI ports are disconnected",
            };
        }
        if (!hmi_health.disconnected_ports.empty())
        {
            return HealthFault{
                RuntimeLedState::Error2,
                SideForText(JoinStrings(hmi_health.disconnected_ports, ", ")),
                "hmi_ports_disconnected",
                "HMI ports disconnected: " + JoinStrings(hmi_health.disconnected_ports, ", "),
            };
        }
        if (!hmi_health.input_connected)
        {
            return HealthFault{
                RuntimeLedState::Error2,
                SideForText(JoinStrings(hmi_health.disconnected_ports, ", ")),
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
                SideForText(AppendHmiPortActivity(detail, hmi_health.port_activity)),
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
                SideForText(JoinStrings(inactive_detail_source, ", ")),
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

    const int64_t session_start_system_time_us = epoch_us_fn_ != nullptr ? epoch_us_fn_() : 0;
    bool ego_started_for_episode = false;
    auto stop_ego_startup_rollback =
        [&]() {
            if (!ego_started_for_episode || dependencies_.stop_ego_recording == nullptr)
            {
                return;
            }
            std::string ego_stop_error;
            if (!dependencies_.stop_ego_recording(
                    state_.current_episode_dir,
                    epoch_us_fn_ != nullptr ? epoch_us_fn_() : session_start_system_time_us,
                    &ego_stop_error))
            {
                CallLog(dependencies_.log_warn,
                        "ego recording startup rollback failed: " +
                            (ego_stop_error.empty() ? std::string("unknown error") : ego_stop_error));
            }
            ego_started_for_episode = false;
        };

    if (dependencies_.start_ego_recording != nullptr)
    {
        std::string ego_error;
        if (dependencies_.start_ego_recording(
                state_.current_episode_dir, session_start_system_time_us, &ego_error))
        {
            ego_started_for_episode = true;
        }
        else
        {
            CallLog(dependencies_.log_warn,
                    "ego recording sidecar did not start early: " +
                        (ego_error.empty() ? std::string("unknown error") : ego_error));
        }
    }

    std::string local_error;
    const std::string reset_source = reset_recording ? state_.last_episode_dir : std::string();
    if (dependencies_.prepare_episode == nullptr ||
        !dependencies_.prepare_episode(state_.current_episode_dir, reset_recording, reset_source, &local_error))
    {
        CallLog(dependencies_.log_error, "prepare episode failed: " + local_error);
        stop_ego_startup_rollback();
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
        stop_ego_startup_rollback();
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
        stop_ego_startup_rollback();
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
        stop_ego_startup_rollback();
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
        stop_ego_startup_rollback();
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
        stop_ego_startup_rollback();
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
                                          const std::string& error_type,
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
    std::vector<std::string> error_types;
    if (due_to_error)
    {
        AddErrorType(&error_types, error_type.empty() ? kErrorTypeRuntimeError : error_type);
    }
    CallLog(dependencies_.log_info, "stopping recording: " + reason);
    CallPerfLog(dependencies_,
            "[PERF] stop phase begin: reason=" + reason + " episode_dir=" + state_.current_episode_dir);

    std::string stereo_error;
    bool stereo_stop_ok = dependencies_.stop_stereo_session != nullptr &&
                          dependencies_.stop_stereo_session(state_.current_episode_dir, stop_system_time_us, &stereo_error);
    if (!stereo_stop_ok)
    {
        CallLog(dependencies_.log_error, "failed to send stereo stop command");
    }

    const int64_t workers_stop_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    auto stop_worker_task =
        [this](WorkerName worker, const std::string& name) {
            const int64_t worker_stop_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
            const bool ok = dependencies_.stop_worker != nullptr &&
                            dependencies_.stop_worker(worker, "recording stop", nullptr);
            CallPerfLog(dependencies_,
                    "[PERF] stop " + name + " done: ok=" + std::string(ok ? "true" : "false") +
                        " elapsed_ms=" +
                        std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : worker_stop_start_ms) -
                                       worker_stop_start_ms));
            return ok;
        };
    auto camera_stop_future = std::async(
        std::launch::async,
        stop_worker_task,
        WorkerName::CameraRecorder,
        std::string("camera_recorder"));
    auto sensor_stop_future = std::async(
        std::launch::async,
        stop_worker_task,
        WorkerName::SensorRecorder,
        std::string("sensor_recorder"));
    const bool camera_stop_ok = camera_stop_future.get();
    const bool sensor_stop_ok = sensor_stop_future.get();
    CallPerfLog(dependencies_,
            "[PERF] stop workers done: camera_ok=" + std::string(camera_stop_ok ? "true" : "false") +
                " sensor_ok=" + std::string(sensor_stop_ok ? "true" : "false") +
                " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : workers_stop_start_ms) -
                               workers_stop_start_ms));

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

    bool ego_stop_ok = true;
    if (dependencies_.stop_ego_recording != nullptr)
    {
        std::string ego_stop_error;
        ego_stop_ok = dependencies_.stop_ego_recording(
            state_.current_episode_dir, stop_system_time_us, &ego_stop_error);
        if (!ego_stop_ok)
        {
            CallLog(dependencies_.log_warn,
                    "ego recording stop failed: " +
                        (ego_stop_error.empty() ? std::string("unknown error") : ego_stop_error));
        }
    }

    const int64_t write_phase_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    CallPerfLog(dependencies_, "[PERF] writing phase begin: episode_dir=" + state_.current_episode_dir);
    if (dependencies_.flush_episode_artifacts != nullptr)
    {
        dependencies_.flush_episode_artifacts(state_.current_episode_dir, "pre_stereo_finalize");
    }

    std::string stereo_finalize_error;
    std::string merge_error;
    if (dependencies_.wait_for_ego_finalize != nullptr)
    {
        const int64_t ego_finalize_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
        std::string ego_finalize_error;
        if (!dependencies_.wait_for_ego_finalize(
                state_.current_episode_dir, options_.stereo_finalize_timeout_ms, &ego_finalize_error))
        {
            ego_stop_ok = false;
            CallLog(dependencies_.log_warn,
                    "ego recording finalize wait failed: " +
                        (ego_finalize_error.empty() ? std::string("unknown error") : ego_finalize_error));
        }
        CallPerfLog(dependencies_,
                "[PERF] ego finalize wait done: ok=" + std::string(ego_stop_ok ? "true" : "false") +
                    " elapsed_ms=" +
                    std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : ego_finalize_start_ms) -
                                   ego_finalize_start_ms));
    }
    if (stereo_stop_ok && dependencies_.wait_for_stereo_finalize != nullptr)
    {
        const int64_t stereo_finalize_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
        if (!dependencies_.wait_for_stereo_finalize(
                state_.current_episode_dir, options_.stereo_finalize_timeout_ms, &stereo_finalize_error))
        {
            CallLog(dependencies_.log_error, "stereo finalize failed: " + stereo_finalize_error);
            stereo_stop_ok = false;
        }
        CallPerfLog(dependencies_,
                "[PERF] stereo finalize wait done: ok=" + std::string(stereo_stop_ok ? "true" : "false") +
                    " elapsed_ms=" +
                    std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : stereo_finalize_start_ms) -
                                   stereo_finalize_start_ms));
    }
    if (stereo_stop_ok && dependencies_.merge_episode_info != nullptr)
    {
        const int64_t stereo_merge_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
        if (!dependencies_.merge_episode_info(state_.current_episode_dir, &merge_error))
        {
            CallLog(dependencies_.log_error, "failed to merge episode info: " + merge_error);
            stereo_stop_ok = false;
        }
        CallPerfLog(dependencies_,
                "[PERF] stereo merge done: ok=" + std::string(stereo_stop_ok ? "true" : "false") +
                    " elapsed_ms=" +
                    std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : stereo_merge_start_ms) -
                                   stereo_merge_start_ms));
    }
    if (dependencies_.flush_episode_artifacts != nullptr)
    {
        dependencies_.flush_episode_artifacts(state_.current_episode_dir, "final");
    }
    if (ego_stop_ok && dependencies_.cleanup_ego_remote != nullptr)
    {
        std::string cleanup_error;
        if (!dependencies_.cleanup_ego_remote(state_.current_episode_dir, &cleanup_error))
        {
            CallLog(dependencies_.log_warn,
                    "ego remote cleanup failed: " +
                        (cleanup_error.empty() ? std::string("unknown error") : cleanup_error));
        }
        if (dependencies_.flush_episode_artifacts != nullptr)
        {
            dependencies_.flush_episode_artifacts(state_.current_episode_dir, "ego_cleanup");
        }
    }
    CallPerfLog(dependencies_,
            "[PERF] writing phase end: episode_dir=" + state_.current_episode_dir +
                " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : write_phase_start_ms) -
                               write_phase_start_ms));

    std::string episode_validation_error;
    std::string final_error_message = due_to_error ? reason : std::string();
    if (!stereo_stop_ok)
    {
        AddErrorType(&error_types, kErrorTypeFinalizeError);
        std::string stereo_failure;
        if (!stereo_finalize_error.empty())
        {
            stereo_failure = stereo_finalize_error;
        }
        else if (!merge_error.empty())
        {
            stereo_failure = merge_error;
        }
        else
        {
            stereo_failure = "stereo session finalize failed";
        }

        if (final_error_message.empty())
        {
            final_error_message = stereo_failure;
        }
        else if (!stereo_failure.empty())
        {
            final_error_message += "; " + stereo_failure;
        }
    }
    const int64_t validate_phase_start_ms = steady_ms_fn_ != nullptr ? steady_ms_fn_() : 0;
    CallPerfLog(dependencies_, "[PERF] validation phase begin: episode_dir=" + state_.current_episode_dir);
    const bool skip_episode_validation = !stereo_stop_ok;
    if (skip_episode_validation)
    {
        CallLog(dependencies_.log_warn,
                "skip episode validation because stereo finalize already failed: " + final_error_message);
    }
    if (dependencies_.write_episode_metadata != nullptr)
    {
        dependencies_.write_episode_metadata(
            state_.current_episode_dir,
            !due_to_error && stereo_stop_ok,
            final_error_message,
            SelectPrimaryErrorType(error_types));
    }
    std::vector<std::string> episode_validation_error_types;
    const bool episode_valid =
        skip_episode_validation ||
        (dependencies_.validate_episode != nullptr &&
         dependencies_.validate_episode(
             state_.current_episode_dir,
             &episode_validation_error,
             &episode_validation_error_types));
    if (!episode_valid)
    {
        AddErrorTypes(&error_types, episode_validation_error_types);
        if (episode_validation_error_types.empty())
        {
            AddErrorType(&error_types, kErrorTypeUnknown);
        }
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
    const bool valid = !due_to_error && stereo_stop_ok && episode_valid;
    CallPerfLog(dependencies_,
            "[PERF] validation phase end: episode_dir=" + state_.current_episode_dir +
                " valid=" + std::string(valid ? "true" : "false") + " elapsed_ms=" +
                std::to_string((steady_ms_fn_ != nullptr ? steady_ms_fn_() : validate_phase_start_ms) -
                               validate_phase_start_ms));
    if (!valid && dependencies_.write_episode_metadata != nullptr)
    {
        dependencies_.write_episode_metadata(
            state_.current_episode_dir,
            false,
            final_error_message,
            SelectPrimaryErrorType(error_types));
    }
    if (!valid)
    {
        CallLog(dependencies_.log_error, "episode validation failed: " + final_error_message);
        if (dependencies_.write_validation_error_log != nullptr)
        {
            dependencies_.write_validation_error_log(state_.current_episode_dir, final_error_message);
        }
    }

    bool episode_dir_finalized = true;
    if (dependencies_.finalize_episode_dir != nullptr && !state_.current_episode_dir.empty())
    {
        std::string finalize_dir_error;
        const std::string renamed_episode_dir =
            dependencies_.finalize_episode_dir(state_.current_episode_dir, &finalize_dir_error);
        if (renamed_episode_dir.empty())
        {
            AddErrorType(&error_types, kErrorTypeFinalizeError);
            episode_dir_finalized = false;
            CallLog(dependencies_.log_error, "failed to finalize episode directory: " + finalize_dir_error);
            if (final_error_message.empty())
            {
                final_error_message = finalize_dir_error;
            }
            else if (!finalize_dir_error.empty())
            {
                final_error_message += "; " + finalize_dir_error;
            }
            if (dependencies_.write_episode_metadata != nullptr)
            {
                dependencies_.write_episode_metadata(
                    state_.current_episode_dir,
                    false,
                    final_error_message,
                    SelectPrimaryErrorType(error_types));
            }
            if (dependencies_.write_validation_error_log != nullptr)
            {
                dependencies_.write_validation_error_log(state_.current_episode_dir, final_error_message);
            }
        }
        else
        {
            state_.last_episode_dir = renamed_episode_dir;
        }
    }

    state_.current_episode_dir.clear();
    if (dependencies_.remove_recording_lock != nullptr)
    {
        dependencies_.remove_recording_lock();
    }

    const RuntimeLedState failure_led_state = SelectFailureLedState(error_types, due_to_error);
    const std::string failure_audio_command = SelectStopFailureAudioCommand(failure_led_state);

    if (due_to_error)
    {
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(failure_led_state, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command(failure_audio_command);
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command(failure_audio_command);
        }
        CallPerfLog(dependencies_,
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

    const bool final_valid = valid && episode_dir_finalized;
    if (!final_valid)
    {
        if (dependencies_.set_led_state != nullptr)
        {
            dependencies_.set_led_state(failure_led_state, 0.0);
        }
        if (dependencies_.set_audio_recovery_command != nullptr)
        {
            dependencies_.set_audio_recovery_command(failure_audio_command);
        }
        if (dependencies_.send_audio_command != nullptr)
        {
            dependencies_.send_audio_command(failure_audio_command);
        }
        CallPerfLog(dependencies_,
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
    CallPerfLog(dependencies_,
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
