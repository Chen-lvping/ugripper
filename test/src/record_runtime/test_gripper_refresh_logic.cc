#include "record_runtime/gripper_refresh_logic.h"

#include <gtest/gtest.h>

namespace {

using ugripper::runtime::AdvancePendingGripperRefresh;
using ugripper::runtime::ApplyGripperConnectionEvent;
using ugripper::runtime::GripperRefreshAction;
using ugripper::runtime::GripperRefreshRuntimeView;

TEST(GripperRefreshLogicTest, ConnectedEventMarksStateAsReconnectingAndPending)
{
    GripperRefreshRuntimeView state{
        .side = "right",
        .connected = false,
        .last_error = "",
    };
    bool pending_refresh = false;

    ApplyGripperConnectionEvent(&state, &pending_refresh, "right", true);

    EXPECT_TRUE(pending_refresh);
    EXPECT_EQ(state.side, "right");
    EXPECT_TRUE(state.connected);
    EXPECT_EQ(state.last_error, "waiting for gripper reconnect refresh");
}

TEST(GripperRefreshLogicTest, DisconnectedEventClearsPendingAndMarksDisconnected)
{
    GripperRefreshRuntimeView state{
        .side = "left",
        .connected = true,
        .last_error = "",
    };
    bool pending_refresh = true;

    ApplyGripperConnectionEvent(&state, &pending_refresh, "left", false);

    EXPECT_FALSE(pending_refresh);
    EXPECT_EQ(state.side, "left");
    EXPECT_FALSE(state.connected);
    EXPECT_EQ(state.last_error, "gripper disconnected");
}

TEST(GripperRefreshLogicTest, PendingRefreshWaitsForSideDevicesBeforeRequestingState)
{
    GripperRefreshRuntimeView state{
        .side = "right",
        .connected = true,
        .last_error = "waiting for gripper reconnect refresh",
    };
    bool pending_refresh = true;

    const auto action = AdvancePendingGripperRefresh(&state, &pending_refresh, "right", false, false);

    EXPECT_EQ(action, GripperRefreshAction::None);
    EXPECT_TRUE(pending_refresh);
    EXPECT_TRUE(state.connected);
    EXPECT_EQ(state.last_error, "waiting for side critical devices");
}

TEST(GripperRefreshLogicTest, PendingRefreshRequestsStateUntilGripperBecomesActive)
{
    GripperRefreshRuntimeView state{
        .side = "right",
        .connected = true,
        .last_error = "waiting for gripper reconnect refresh",
    };
    bool pending_refresh = true;

    const auto action = AdvancePendingGripperRefresh(&state, &pending_refresh, "right", true, false);

    EXPECT_EQ(action, GripperRefreshAction::RequestState);
    EXPECT_TRUE(pending_refresh);
    EXPECT_TRUE(state.connected);
    EXPECT_EQ(state.last_error, "waiting for gripper reconnect to become active");
}

TEST(GripperRefreshLogicTest, PendingRefreshTransitionsToRefreshActionWhenReady)
{
    GripperRefreshRuntimeView state{
        .side = "left",
        .connected = true,
        .last_error = "waiting for gripper reconnect to become active",
    };
    bool pending_refresh = true;

    const auto action = AdvancePendingGripperRefresh(&state, &pending_refresh, "left", true, true);

    EXPECT_EQ(action, GripperRefreshAction::RefreshRuntimeState);
    EXPECT_FALSE(pending_refresh);
    EXPECT_TRUE(state.connected);
    EXPECT_EQ(state.side, "left");
}

TEST(GripperRefreshLogicTest, NoPendingRefreshProducesNoAction)
{
    GripperRefreshRuntimeView state{
        .side = "left",
        .connected = true,
        .last_error = "",
    };
    bool pending_refresh = false;

    const auto action = AdvancePendingGripperRefresh(&state, &pending_refresh, "left", true, true);

    EXPECT_EQ(action, GripperRefreshAction::None);
    EXPECT_FALSE(pending_refresh);
    EXPECT_TRUE(state.connected);
}

}  // namespace
