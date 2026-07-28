#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ugripper::runtime {

enum class PhysicalButton : uint8_t
{
    LeftUp = 1U << 0,
    LeftDown = 1U << 1,
    RightUp = 1U << 2,
    RightDown = 1U << 3,
};

constexpr uint8_t ButtonMask(PhysicalButton button)
{
    return static_cast<uint8_t>(button);
}

enum class ButtonAction
{
    ToggleEgoBinding,
};

struct FourButtonSnapshot
{
    bool left_up = false;
    bool left_down = false;
    bool right_up = false;
    bool right_down = false;
};

struct ButtonChordDefinition
{
    ButtonAction action = ButtonAction::ToggleEgoBinding;
    uint8_t required_mask = 0;
    uint64_t hold_ms = 0;
    bool allowed_while_recording = false;
};

struct ButtonRouteResult
{
    uint8_t consumed_mask = 0;
    std::vector<ButtonAction> actions;
};

class ButtonActionRouter
{
public:
    explicit ButtonActionRouter(std::vector<ButtonChordDefinition> definitions);

    ButtonRouteResult Update(const FourButtonSnapshot& snapshot,
                             bool recording,
                             uint64_t now_ms);
    void Reset();

private:
    struct ChordState
    {
        bool active = false;
        bool fired = false;
        uint64_t pressed_since_ms = 0;
    };

    static uint8_t SnapshotMask(const FourButtonSnapshot& snapshot);

    std::vector<ButtonChordDefinition> definitions_;
    std::vector<ChordState> states_;
    uint8_t release_latch_mask_ = 0;
};

enum class TaskMarkerResult
{
    StartMarked,
    StopMarked,
    Rejected,
};

enum class HmiFeedbackEvent
{
    EgoBound,
    EgoUnbound,
    EgoBindingFailed,
    TaskStartMarked,
    TaskStopMarked,
    TaskMarkerRejected,
};

enum class HmiFeedbackSide
{
    Right,
    Both,
};

struct HmiFeedbackPattern
{
    HmiFeedbackSide led_side = HmiFeedbackSide::Both;
    HmiFeedbackSide beep_side = HmiFeedbackSide::Both;
    int pulse_count = 0;
    int on_ms = 0;
    int off_ms = 0;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    std::array<uint16_t, 3> beep_frequencies_hz{};
};

HmiFeedbackPattern FeedbackPatternFor(HmiFeedbackEvent event);

class TaskMarkerTracker
{
public:
    TaskMarkerResult Mark(double unix_seconds);
    void Reset();

    std::optional<double> task_start_s() const;
    std::optional<double> task_stop_s() const;

private:
    std::optional<double> task_start_s_;
    std::optional<double> task_stop_s_;
};

}  // namespace ugripper::runtime
