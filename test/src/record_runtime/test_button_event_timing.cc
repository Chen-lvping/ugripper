#include "record_runtime/hmi_controller.h"

#include <gtest/gtest.h>

namespace {

uint64_t g_now_ms = 0;

uint64_t FakeNowMs()
{
    return g_now_ms;
}

using ugripper::runtime::ButtonSnapshot;
using ugripper::runtime::HmiController;
using ugripper::runtime::HmiControllerOptions;
using ugripper::runtime::HmiEventType;

TEST(HmiControllerTimingTest, UsesConfiguredDebounceWindowForShortPress)
{
    HmiController controller(
        HmiControllerOptions{
            .action_debounce_ms = 500,
            .long_press_threshold_ms = 1000,
            .dual_long_press_threshold_ms = 4000,
            .shutdown_prompt_threshold_ms = 2000,
        },
        &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 300;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{}).empty());

    g_now_ms = 900;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.down_pressed = true}).empty());

    g_now_ms = 1500;
    const auto events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortDownPressed);
}

TEST(HmiControllerTimingTest, LongPressFiresOnceAtConfiguredThreshold)
{
    HmiController controller(
        HmiControllerOptions{
            .action_debounce_ms = 250,
            .long_press_threshold_ms = 1200,
            .dual_long_press_threshold_ms = 4000,
            .shutdown_prompt_threshold_ms = 2000,
        },
        &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 1100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 1300;
    auto events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::LongUpPressed);

    g_now_ms = 1700;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 1800;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{}).empty());
}

TEST(HmiControllerTimingTest, UsesConfiguredShutdownPromptThreshold)
{
    HmiController controller(
        HmiControllerOptions{
            .action_debounce_ms = 250,
            .long_press_threshold_ms = 800,
            .dual_long_press_threshold_ms = 5000,
            .shutdown_prompt_threshold_ms = 1500,
        },
        &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true}).empty());

    g_now_ms = 1400;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true}).empty());

    g_now_ms = 1600;
    const auto events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShutdownPromptRequested);
}

}  // namespace
