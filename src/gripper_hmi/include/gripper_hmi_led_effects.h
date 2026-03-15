#ifndef GRIPPER_HMI_LED_EFFECTS_H
#define GRIPPER_HMI_LED_EFFECTS_H

#include "gripper_hmi_driver.h"

#include <cstdint>
#include <string>

enum class GripperLedEffectState
{
    Init,
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

struct GripperLedEffect
{
    GripperLedEffectState state = GripperLedEffectState::Init;
    double progress = 0.0;
};

class GripperLedEffectRenderer
{
public:
    static constexpr uint64_t recommendedRenderIntervalMs() { return 20; }

    static bool parseStateText(const std::string &text, GripperLedEffect *effect);
    static std::string stateText(const GripperLedEffect &effect);

    GripperLedColor render(const GripperLedEffect &effect, uint64_t steadyMs, uint64_t epochMs) const;

private:
    static int errorLevel(GripperLedEffectState state);
    static uint8_t clampToU8(int value);
};

#endif
