#include "gripper_hmi_led_effects.h"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(GripperLedEffectRendererTest, ParseStateTextAcceptsKnownStateAndClampsProgress)
{
    GripperLedEffect effect;

    ASSERT_TRUE(GripperLedEffectRenderer::parseStateText("CALIB_RUN:1.5", &effect));
    EXPECT_EQ(effect.state, GripperLedEffectState::CalibRun);
    EXPECT_DOUBLE_EQ(effect.progress, 1.0);
}

TEST(GripperLedEffectRendererTest, ParseStateTextRejectsUnknownState)
{
    GripperLedEffect effect;

    EXPECT_FALSE(GripperLedEffectRenderer::parseStateText("UNKNOWN", &effect));
}

TEST(GripperLedEffectRendererTest, ParseStateTextAcceptsWarning)
{
    GripperLedEffect effect;

    ASSERT_TRUE(GripperLedEffectRenderer::parseStateText("WARNING", &effect));
    EXPECT_EQ(effect.state, GripperLedEffectState::Warning);
}

TEST(GripperLedEffectRendererTest, ParseStateTextAcceptsWriting)
{
    GripperLedEffect effect;

    ASSERT_TRUE(GripperLedEffectRenderer::parseStateText("WRITING", &effect));
    EXPECT_EQ(effect.state, GripperLedEffectState::Writing);
}

TEST(GripperLedEffectRendererTest, RenderProducesStableColorsForSimpleStates)
{
    GripperLedEffectRenderer renderer;

    const auto writingColor = renderer.render(GripperLedEffect{GripperLedEffectState::Writing, 0.0}, 0, 0);
    EXPECT_EQ(writingColor.red, 0);
    EXPECT_EQ(writingColor.green, 122);
    EXPECT_EQ(writingColor.blue, 255);

    const auto recordingOn = renderer.render(GripperLedEffect{GripperLedEffectState::Recording, 0.0}, 100, 0);
    EXPECT_EQ(recordingOn.red, 0);
    EXPECT_EQ(recordingOn.green, 255);
    EXPECT_EQ(recordingOn.blue, 0);

    const auto recordingOff = renderer.render(GripperLedEffect{GripperLedEffectState::Recording, 0.0}, 700, 0);
    EXPECT_EQ(recordingOff.red, 0);
    EXPECT_EQ(recordingOff.green, 0);
    EXPECT_EQ(recordingOff.blue, 0);
}

TEST(GripperLedEffectRendererTest, InitBlinksBlue)
{
    GripperLedEffectRenderer renderer;

    const auto initOn = renderer.render(GripperLedEffect{GripperLedEffectState::Init, 0.0}, 0, 0);
    EXPECT_EQ(initOn.red, 0);
    EXPECT_EQ(initOn.green, 122);
    EXPECT_EQ(initOn.blue, 255);

    const auto initOff = renderer.render(GripperLedEffect{GripperLedEffectState::Init, 0.0}, 700, 0);
    EXPECT_EQ(initOff.red, 0);
    EXPECT_EQ(initOff.green, 0);
    EXPECT_EQ(initOff.blue, 0);
}

TEST(GripperLedEffectRendererTest, ReadyBreathStartsBright)
{
    GripperLedEffectRenderer renderer;

    const auto readyStart = renderer.render(GripperLedEffect{GripperLedEffectState::Ready, 0.0}, 0, 0);
    EXPECT_EQ(readyStart.red, 0);
    EXPECT_EQ(readyStart.green, 120);
    EXPECT_EQ(readyStart.blue, 9);

    const auto readyHalfCycle = renderer.render(GripperLedEffect{GripperLedEffectState::Ready, 0.0}, 2250, 0);
    EXPECT_EQ(readyHalfCycle.red, 0);
    EXPECT_EQ(readyHalfCycle.green, 0);
    EXPECT_EQ(readyHalfCycle.blue, 0);
}

TEST(GripperLedEffectRendererTest, StateTextKeepsCalibRunPrefix)
{
    const auto text = GripperLedEffectRenderer::stateText(
        GripperLedEffect{GripperLedEffectState::CalibRun, 0.25});

    EXPECT_EQ(text.rfind("CALIB_RUN:", 0), 0U);
}

}  // namespace
