#pragma once

#include <string>

namespace ugripper::runtime {

struct GripperRefreshRuntimeView
{
    std::string side;
    bool connected = false;
    bool calibration_valid = false;
    std::string calibration_status;
    std::string calibration_source;
    std::string last_error;
};

enum class GripperRefreshAction
{
    None,
    RequestState,
    RefreshRuntimeState,
};

inline void ClearGripperRefreshRuntimeView(GripperRefreshRuntimeView* state,
                                          const std::string& side,
                                          bool connected,
                                          const std::string& status,
                                          const std::string& error_message)
{
    if (state == nullptr)
    {
        return;
    }

    state->side = side;
    state->connected = connected;
    state->calibration_valid = false;
    state->calibration_status = status;
    state->calibration_source.clear();
    state->last_error = error_message;
}

inline void ApplyGripperConnectionEvent(GripperRefreshRuntimeView* state,
                                        bool* pending_refresh,
                                        const std::string& side,
                                        bool connected)
{
    if (connected)
    {
        if (pending_refresh != nullptr)
        {
            *pending_refresh = true;
        }
        ClearGripperRefreshRuntimeView(state, side, true, "reconnecting", "waiting for gripper reconnect refresh");
        return;
    }

    if (pending_refresh != nullptr)
    {
        *pending_refresh = false;
    }
    ClearGripperRefreshRuntimeView(state, side, false, "disconnected", "gripper disconnected");
}

inline GripperRefreshAction AdvancePendingGripperRefresh(GripperRefreshRuntimeView* state,
                                                         bool* pending_refresh,
                                                         const std::string& side,
                                                         bool side_critical_devices_ready,
                                                         bool gripper_ready)
{
    if (pending_refresh == nullptr || !*pending_refresh)
    {
        return GripperRefreshAction::None;
    }

    if (state != nullptr)
    {
        state->side = side;
        state->connected = true;
    }

    if (!side_critical_devices_ready)
    {
        ClearGripperRefreshRuntimeView(state, side, true, "waiting_side_devices", "waiting for side critical devices");
        return GripperRefreshAction::None;
    }

    if (!gripper_ready)
    {
        ClearGripperRefreshRuntimeView(state, side, true, "waiting_gripper_ready",
                                       "waiting for gripper reconnect to become active");
        return GripperRefreshAction::RequestState;
    }

    *pending_refresh = false;
    return GripperRefreshAction::RefreshRuntimeState;
}

}  // namespace ugripper::runtime
