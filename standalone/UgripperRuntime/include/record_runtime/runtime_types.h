#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ugripper::runtime {

inline constexpr const char* kErrorTypeUnknown = "unknown";
inline constexpr const char* kErrorTypeRuntimeError = "runtime_error";
inline constexpr const char* kErrorTypeMissingFile = "missing_file";
inline constexpr const char* kErrorTypeFinalizeError = "finalize_error";
inline constexpr const char* kErrorTypeStereoControlFailed = "stereo_control_failed";
inline constexpr const char* kErrorTypeCalibrationError = "calibration_error";
inline constexpr const char* kErrorTypeDeviceDisconnected = "device_disconnected";
inline constexpr const char* kErrorTypeCollectionDurationTooShort = "collection_duration_too_short";
inline constexpr const char* kErrorTypeFrameLoss = "frame_loss";
inline constexpr const char* kErrorTypeOperatorMarkedFailed = "operator_marked_failed";

enum class RuntimeLedState
{
    Init,
    Writing,
    Ready,
    Recording,
    Error1,
    Error2,
    Error3,
    Error4,
    Error5,
    CalibPre,
    CalibRun,
    CalibDone,
    Exit,
};

struct ButtonSnapshot
{
    bool up_pressed = false;
    bool down_pressed = false;
};

struct ButtonStateTracker
{
    uint64_t up_pressed_since_ms = 0;
    uint64_t down_pressed_since_ms = 0;
    uint64_t both_pressed_since_ms = 0;
    bool up_long_handled = false;
    bool down_long_handled = false;
    bool dual_chord_active = false;
    bool dual_long_handled = false;
    bool shutdown_prompt_played = false;
};

enum class HmiEventType
{
    ShortUpPressed,
    ShortDownPressed,
    LongUpPressed,
    LongDownPressed,
    ShutdownPromptRequested,
    ShutdownRequested,
};

struct HmiEvent
{
    HmiEventType type = HmiEventType::ShortUpPressed;
};

enum class HealthStatus
{
    Unknown,
    Ok,
    Error,
};

enum class HardwareFaultSide
{
    Unknown,
    Left,
    Right,
    Both,
};

struct HealthState
{
    HealthStatus status = HealthStatus::Unknown;
    std::string last_error_key;
    uint64_t last_check_ms = 0;
    uint64_t first_seen_ms = 0;
};

struct HealthFault
{
    RuntimeLedState led_state = RuntimeLedState::Error5;
    HardwareFaultSide side = HardwareFaultSide::Unknown;
    std::string key;
    std::string detail;
};

struct HmiHealthSnapshot
{
    bool has_connected_device = false;
    bool input_connected = false;
    bool input_active = false;
    uint64_t input_last_rx_age_ms = 0;
    std::vector<std::string> disconnected_ports;
    std::vector<std::string> inactive_ports;
    std::vector<std::string> inactive_port_details;
    std::vector<std::string> port_activity;
};

struct RecordingState
{
    bool is_recording = false;
    std::string current_episode_dir;
    std::string last_episode_dir;
};

}  // namespace ugripper::runtime
