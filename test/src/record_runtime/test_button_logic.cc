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
using ugripper::runtime::HmiEventType;

TEST(HmiControllerTest, EmitsShortPressEventOnRelease)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 300;
    const auto events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortUpPressed);
}

TEST(HmiControllerTest, EmitsLongPressInsteadOfShortPress)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.down_pressed = true}).empty());

    g_now_ms = 950;
    auto events = controller.HandleButtons(ButtonSnapshot{.down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::LongDownPressed);

    g_now_ms = 1000;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{}).empty());
}

TEST(HmiControllerTest, EmitsShutdownPromptAndShutdownForDualHold)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true}).empty());

    g_now_ms = 2200;
    auto events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShutdownPromptRequested);

    g_now_ms = 4300;
    events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShutdownRequested);
}

TEST(HmiControllerTest, DebouncesRepeatedShortPresses)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 300;
    auto events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortUpPressed);

    g_now_ms = 320;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.down_pressed = true}).empty());

    g_now_ms = 360;
    events = controller.HandleButtons(ButtonSnapshot{});
    EXPECT_TRUE(events.empty());

    g_now_ms = 620;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.down_pressed = true}).empty());

    g_now_ms = 900;
    events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortDownPressed);
}

TEST(HmiControllerTest, AcceptsFastSecondShortPressAfterDefaultDebounce)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 200;
    auto events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortUpPressed);

    g_now_ms = 220;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 300;
    events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortUpPressed);
}

TEST(HmiControllerTest, DualChordReleaseDoesNotEmitShortPress)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true}).empty());

    g_now_ms = 2200;
    auto events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShutdownPromptRequested);

    g_now_ms = 2300;
    events = controller.HandleButtons(ButtonSnapshot{});
    EXPECT_TRUE(events.empty());
}

TEST(HmiControllerTest, ShutdownPromptOnlyEmitsOncePerHold)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true}).empty());

    g_now_ms = 2200;
    auto events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShutdownPromptRequested);

    g_now_ms = 2600;
    events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    EXPECT_TRUE(events.empty());
}

TEST(HmiControllerTest, ResetClearsPendingChordAndAllowsFreshShortPress)
{
    HmiController controller({}, &FakeNowMs);

    g_now_ms = 100;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true}).empty());

    g_now_ms = 2200;
    auto events = controller.HandleButtons(ButtonSnapshot{.up_pressed = true, .down_pressed = true});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShutdownPromptRequested);

    controller.Reset();

    g_now_ms = 2600;
    EXPECT_TRUE(controller.HandleButtons(ButtonSnapshot{.up_pressed = true}).empty());

    g_now_ms = 2900;
    events = controller.HandleButtons(ButtonSnapshot{});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().type, HmiEventType::ShortUpPressed);
}

}  // namespace
