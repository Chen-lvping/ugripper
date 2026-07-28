#include "record_runtime/button_action_router.h"

#include <utility>

namespace ugripper::runtime {

HmiFeedbackPattern FeedbackPatternFor(HmiFeedbackEvent event)
{
    constexpr uint8_t kActionCyanRed = 0;
    constexpr uint8_t kActionCyanGreen = 200;
    constexpr uint8_t kActionCyanBlue = 255;
    constexpr std::array<uint16_t, 3> kRisingToneHz{1000, 2000, 4000};
    constexpr std::array<uint16_t, 3> kFallingToneHz{4000, 2000, 1000};
    switch (event)
    {
    case HmiFeedbackEvent::EgoBound:
        return {HmiFeedbackSide::Both, HmiFeedbackSide::Both, 3, 120, 70,
                255, 255, 0, kRisingToneHz};
    case HmiFeedbackEvent::EgoUnbound:
        return {HmiFeedbackSide::Both, HmiFeedbackSide::Both, 3, 120, 70,
                180, 0, 255, kFallingToneHz};
    case HmiFeedbackEvent::EgoBindingFailed:
        return {HmiFeedbackSide::Both, HmiFeedbackSide::Both, 3, 220, 100, 255, 0, 0};
    case HmiFeedbackEvent::TaskStartMarked:
        return {HmiFeedbackSide::Both, HmiFeedbackSide::Right, 1, 120, 0,
                kActionCyanRed, kActionCyanGreen, kActionCyanBlue};
    case HmiFeedbackEvent::TaskStopMarked:
        return {HmiFeedbackSide::Both, HmiFeedbackSide::Right, 2, 120, 100,
                kActionCyanRed, kActionCyanGreen, kActionCyanBlue};
    case HmiFeedbackEvent::TaskMarkerRejected:
        return {HmiFeedbackSide::Right, HmiFeedbackSide::Right, 3, 220, 100, 255, 110, 0};
    }
    return {};
}

ButtonActionRouter::ButtonActionRouter(std::vector<ButtonChordDefinition> definitions)
    : definitions_(std::move(definitions)), states_(definitions_.size())
{
}

uint8_t ButtonActionRouter::SnapshotMask(const FourButtonSnapshot& snapshot)
{
    uint8_t mask = 0;
    if (snapshot.left_up)
    {
        mask |= ButtonMask(PhysicalButton::LeftUp);
    }
    if (snapshot.left_down)
    {
        mask |= ButtonMask(PhysicalButton::LeftDown);
    }
    if (snapshot.right_up)
    {
        mask |= ButtonMask(PhysicalButton::RightUp);
    }
    if (snapshot.right_down)
    {
        mask |= ButtonMask(PhysicalButton::RightDown);
    }
    return mask;
}

ButtonRouteResult ButtonActionRouter::Update(const FourButtonSnapshot& snapshot,
                                             bool recording,
                                             uint64_t now_ms)
{
    ButtonRouteResult result;
    const uint8_t pressed_mask = SnapshotMask(snapshot);

    if (release_latch_mask_ != 0)
    {
        result.consumed_mask = release_latch_mask_;
        if ((pressed_mask & release_latch_mask_) == 0)
        {
            release_latch_mask_ = 0;
            result.consumed_mask = 0;
        }
        return result;
    }

    for (size_t index = 0; index < definitions_.size(); ++index)
    {
        const ButtonChordDefinition& definition = definitions_[index];
        ChordState& state = states_[index];
        const bool enabled = !recording || definition.allowed_while_recording;
        const bool exact_match = enabled && pressed_mask == definition.required_mask;
        if (!exact_match)
        {
            state = {};
            continue;
        }

        result.consumed_mask = definition.required_mask;
        if (!state.active)
        {
            state.active = true;
            state.pressed_since_ms = now_ms;
        }
        const uint64_t held_ms = now_ms >= state.pressed_since_ms
                                     ? now_ms - state.pressed_since_ms
                                     : 0;
        if (!state.fired && held_ms >= definition.hold_ms)
        {
            state.fired = true;
            result.actions.push_back(definition.action);
            release_latch_mask_ = definition.required_mask;
        }

        for (size_t other = 0; other < states_.size(); ++other)
        {
            if (other != index)
            {
                states_[other] = {};
            }
        }
        return result;
    }
    return result;
}

void ButtonActionRouter::Reset()
{
    for (ChordState& state : states_)
    {
        state = {};
    }
    release_latch_mask_ = 0;
}

TaskMarkerResult TaskMarkerTracker::Mark(double unix_seconds)
{
    if (!task_start_s_.has_value())
    {
        task_start_s_ = unix_seconds;
        return TaskMarkerResult::StartMarked;
    }
    if (!task_stop_s_.has_value())
    {
        task_stop_s_ = unix_seconds;
        return TaskMarkerResult::StopMarked;
    }
    return TaskMarkerResult::Rejected;
}

void TaskMarkerTracker::Reset()
{
    task_start_s_.reset();
    task_stop_s_.reset();
}

std::optional<double> TaskMarkerTracker::task_start_s() const
{
    return task_start_s_;
}

std::optional<double> TaskMarkerTracker::task_stop_s() const
{
    return task_stop_s_;
}

}  // namespace ugripper::runtime
