#pragma once

#include <string>

namespace ugripper::runtime {

struct GripperRefreshRuntimeView
{
    std::string side;
    bool connected = false;
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
                                          const std::string& error_message)
{
    if (state == nullptr)
    {
        return;
    }

    state->side = side;
    state->connected = connected;
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
        ClearGripperRefreshRuntimeView(state, side, true, "waiting for gripper reconnect refresh");
        return;
    }

    if (pending_refresh != nullptr)
    {
        *pending_refresh = false;
    }
    ClearGripperRefreshRuntimeView(state, side, false, "gripper disconnected");
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
        ClearGripperRefreshRuntimeView(state, side, true, "waiting for side critical devices");
        return GripperRefreshAction::None;
    }

    if (!gripper_ready)
    {
        ClearGripperRefreshRuntimeView(state, side, true, "waiting for gripper reconnect to become active");
        return GripperRefreshAction::RequestState;
    }

    *pending_refresh = false;
    return GripperRefreshAction::RefreshRuntimeState;
}

}  // namespace ugripper::runtime
