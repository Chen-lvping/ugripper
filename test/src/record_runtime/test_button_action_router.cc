#include "record_runtime/button_action_router.h"

#include <gtest/gtest.h>

namespace {

using ugripper::runtime::ButtonAction;
using ugripper::runtime::ButtonActionRouter;
using ugripper::runtime::ButtonChordDefinition;
using ugripper::runtime::ButtonMask;
using ugripper::runtime::FourButtonSnapshot;
using ugripper::runtime::FeedbackPatternFor;
using ugripper::runtime::HmiFeedbackEvent;
using ugripper::runtime::HmiFeedbackSide;
using ugripper::runtime::PhysicalButton;
using ugripper::runtime::TaskMarkerResult;
using ugripper::runtime::TaskMarkerTracker;

ButtonActionRouter MakeRouter()
{
    return ButtonActionRouter({
        {
            .action = ButtonAction::ToggleEgoBinding,
            .required_mask = ButtonMask(PhysicalButton::LeftUp) |
                             ButtonMask(PhysicalButton::RightUp),
            .hold_ms = 2000,
            .allowed_while_recording = false,
        },
    });
}

TEST(ButtonActionRouterTest, CrossHandUpperChordFiresOnceAtTwoSeconds)
{
    auto router = MakeRouter();
    const FourButtonSnapshot bothUp{.left_up = true, .right_up = true};

    auto result = router.Update(bothUp, false, 100);
    EXPECT_NE(result.consumed_mask, 0);
    EXPECT_TRUE(result.actions.empty());

    result = router.Update(bothUp, false, 2099);
    EXPECT_TRUE(result.actions.empty());

    result = router.Update(bothUp, false, 2100);
    ASSERT_EQ(result.actions.size(), 1U);
    EXPECT_EQ(result.actions.front(), ButtonAction::ToggleEgoBinding);

    result = router.Update(bothUp, false, 4000);
    EXPECT_TRUE(result.actions.empty());
}

TEST(ButtonActionRouterTest, SecondUpperButtonCanJoinAtNineHundredNinetyNineMilliseconds)
{
    auto router = MakeRouter();

    auto result = router.Update(FourButtonSnapshot{.left_up = true}, false, 100);
    EXPECT_EQ(result.consumed_mask, 0);
    EXPECT_TRUE(result.actions.empty());

    const FourButtonSnapshot bothUp{.left_up = true, .right_up = true};
    result = router.Update(bothUp, false, 1099);
    EXPECT_NE(result.consumed_mask, 0);
    EXPECT_TRUE(result.actions.empty());

    result = router.Update(bothUp, false, 3098);
    EXPECT_TRUE(result.actions.empty());
    result = router.Update(bothUp, false, 3099);
    ASSERT_EQ(result.actions.size(), 1U);
    EXPECT_EQ(result.actions.front(), ButtonAction::ToggleEgoBinding);
}

TEST(ButtonActionRouterTest, ChordConsumesRemainingUpperButtonUntilBothRelease)
{
    auto router = MakeRouter();
    const FourButtonSnapshot bothUp{.left_up = true, .right_up = true};
    router.Update(bothUp, false, 100);
    router.Update(bothUp, false, 2100);

    auto result = router.Update(FourButtonSnapshot{.right_up = true}, false, 2200);
    EXPECT_NE(result.consumed_mask, 0);
    EXPECT_TRUE(result.actions.empty());

    result = router.Update({}, false, 2300);
    EXPECT_EQ(result.consumed_mask, 0);
}

TEST(ButtonActionRouterTest, ChordIsDisabledWhileRecording)
{
    auto router = MakeRouter();
    const FourButtonSnapshot bothUp{.left_up = true, .right_up = true};

    EXPECT_EQ(router.Update(bothUp, true, 100).consumed_mask, 0);
    const auto result = router.Update(bothUp, true, 3000);
    EXPECT_EQ(result.consumed_mask, 0);
    EXPECT_TRUE(result.actions.empty());
}

TEST(ButtonActionRouterTest, ExtraButtonPreventsReservedCombinationFromMatching)
{
    auto router = MakeRouter();
    const FourButtonSnapshot withExtra{
        .left_up = true,
        .left_down = true,
        .right_up = true,
    };

    EXPECT_EQ(router.Update(withExtra, false, 100).consumed_mask, 0);
    EXPECT_TRUE(router.Update(withExtra, false, 3000).actions.empty());
}

TEST(TaskMarkerTrackerTest, RecordsStartThenStopAndRejectsThirdMarker)
{
    TaskMarkerTracker tracker;
    EXPECT_EQ(tracker.Mark(1000.125), TaskMarkerResult::StartMarked);
    EXPECT_EQ(tracker.Mark(1010.875), TaskMarkerResult::StopMarked);
    EXPECT_EQ(tracker.Mark(1020.0), TaskMarkerResult::Rejected);
    ASSERT_TRUE(tracker.task_start_s().has_value());
    ASSERT_TRUE(tracker.task_stop_s().has_value());
    EXPECT_DOUBLE_EQ(*tracker.task_start_s(), 1000.125);
    EXPECT_DOUBLE_EQ(*tracker.task_stop_s(), 1010.875);
}

TEST(TaskMarkerTrackerTest, ResetStartsFreshEpisodeMarkers)
{
    TaskMarkerTracker tracker;
    tracker.Mark(1000.0);
    tracker.Mark(1010.0);
    tracker.Reset();

    EXPECT_FALSE(tracker.task_start_s().has_value());
    EXPECT_FALSE(tracker.task_stop_s().has_value());
    EXPECT_EQ(tracker.Mark(2000.0), TaskMarkerResult::StartMarked);
}

TEST(HmiFeedbackPatternTest, EgoBindingUsesDistinctColorsAndToneDirections)
{
    const auto bound = FeedbackPatternFor(HmiFeedbackEvent::EgoBound);
    EXPECT_EQ(bound.led_side, HmiFeedbackSide::Both);
    EXPECT_EQ(bound.beep_side, HmiFeedbackSide::Both);
    EXPECT_EQ(bound.pulse_count, 3);
    EXPECT_EQ(bound.red, 255);
    EXPECT_EQ(bound.green, 255);
    EXPECT_EQ(bound.blue, 0);
    EXPECT_LT(bound.beep_frequencies_hz[0], bound.beep_frequencies_hz[1]);
    EXPECT_LT(bound.beep_frequencies_hz[1], bound.beep_frequencies_hz[2]);

    const auto unbound = FeedbackPatternFor(HmiFeedbackEvent::EgoUnbound);
    EXPECT_EQ(unbound.led_side, HmiFeedbackSide::Both);
    EXPECT_EQ(unbound.beep_side, HmiFeedbackSide::Both);
    EXPECT_EQ(unbound.pulse_count, 3);
    EXPECT_EQ(unbound.red, 180);
    EXPECT_EQ(unbound.green, 0);
    EXPECT_EQ(unbound.blue, 255);
    EXPECT_GT(unbound.beep_frequencies_hz[0], unbound.beep_frequencies_hz[1]);
    EXPECT_GT(unbound.beep_frequencies_hz[1], unbound.beep_frequencies_hz[2]);
}

TEST(HmiFeedbackPatternTest, TaskMarkersKeepExistingCyanPatterns)
{
    const auto taskStart = FeedbackPatternFor(HmiFeedbackEvent::TaskStartMarked);
    const auto taskStop = FeedbackPatternFor(HmiFeedbackEvent::TaskStopMarked);
    EXPECT_EQ(taskStart.led_side, HmiFeedbackSide::Both);
    EXPECT_EQ(taskStart.beep_side, HmiFeedbackSide::Right);
    EXPECT_EQ(taskStart.pulse_count, 1);
    EXPECT_EQ(taskStop.led_side, HmiFeedbackSide::Both);
    EXPECT_EQ(taskStop.beep_side, HmiFeedbackSide::Right);
    EXPECT_EQ(taskStop.pulse_count, 2);
}

TEST(HmiFeedbackPatternTest, FailuresAndRejectedMarkersUseErrorSemanticColors)
{
    const auto bindingFailed = FeedbackPatternFor(HmiFeedbackEvent::EgoBindingFailed);
    EXPECT_EQ(bindingFailed.led_side, HmiFeedbackSide::Both);
    EXPECT_EQ(bindingFailed.beep_side, HmiFeedbackSide::Both);
    EXPECT_EQ(bindingFailed.pulse_count, 3);
    EXPECT_EQ(bindingFailed.red, 255);
    EXPECT_EQ(bindingFailed.green, 0);
    EXPECT_EQ(bindingFailed.blue, 0);

    const auto rejected = FeedbackPatternFor(HmiFeedbackEvent::TaskMarkerRejected);
    EXPECT_EQ(rejected.led_side, HmiFeedbackSide::Right);
    EXPECT_EQ(rejected.beep_side, HmiFeedbackSide::Right);
    EXPECT_EQ(rejected.pulse_count, 3);
    EXPECT_EQ(rejected.red, 255);
    EXPECT_EQ(rejected.green, 110);
    EXPECT_EQ(rejected.blue, 0);
}

}  // namespace
