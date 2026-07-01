#include "gripper_hmi_led_effects.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr uint64_t kReadyBreathPeriodMs = 4500;
constexpr uint64_t kRecordingBlinkPeriodMs = 1000;
constexpr uint64_t kCalibBlinkPeriodMs = 1000;
constexpr uint64_t kErrorLongOnMs = 700;
constexpr uint64_t kErrorShortOnMs = 220;
constexpr uint64_t kErrorPulseGapMs = 300;
constexpr uint64_t kErrorSequenceGapMs = 1200;
constexpr double kPi = 3.14159265358979323846;

enum class TactileWarningPattern
{
    OneLongOneShort,
    OneLongTwoShort,
    TwoLong,
};

double clampProgress(double progress)
{
    if (progress < 0.0)
    {
        return 0.0;
    }
    if (progress > 1.0)
    {
        return 1.0;
    }
    return progress;
}

GripperLedColor renderPulseSequence(uint64_t steadyMs,
                                    const std::vector<uint64_t> &onDurationsMs,
                                    const GripperLedColor &onColor)
{
    uint64_t cycleMs = 0;
    for (size_t index = 0; index < onDurationsMs.size(); ++index)
    {
        cycleMs += onDurationsMs[index];
        cycleMs += (index + 1 == onDurationsMs.size()) ? kErrorSequenceGapMs : kErrorPulseGapMs;
    }

    if (cycleMs == 0)
    {
        return GripperLedColor{0, 0, 0};
    }

    uint64_t phaseMs = steadyMs % cycleMs;
    for (size_t index = 0; index < onDurationsMs.size(); ++index)
    {
        const uint64_t onMs = onDurationsMs[index];
        if (phaseMs < onMs)
        {
            return onColor;
        }
        phaseMs -= onMs;

        const uint64_t offMs = (index + 1 == onDurationsMs.size()) ? kErrorSequenceGapMs : kErrorPulseGapMs;
        if (phaseMs < offMs)
        {
            return GripperLedColor{0, 0, 0};
        }
        phaseMs -= offMs;
    }

    return GripperLedColor{0, 0, 0};
}

GripperLedColor renderTactileWarningPattern(uint64_t steadyMs, TactileWarningPattern pattern)
{
    constexpr GripperLedColor kWarningYellow{255, 110, 0};
    switch (pattern)
    {
    case TactileWarningPattern::OneLongOneShort:
        return renderPulseSequence(steadyMs, {kErrorLongOnMs, kErrorShortOnMs}, kWarningYellow);
    case TactileWarningPattern::OneLongTwoShort:
        return renderPulseSequence(steadyMs, {kErrorLongOnMs, kErrorShortOnMs, kErrorShortOnMs}, kWarningYellow);
    case TactileWarningPattern::TwoLong:
        return renderPulseSequence(steadyMs, {kErrorLongOnMs, kErrorLongOnMs}, kWarningYellow);
    }
    return GripperLedColor{0, 0, 0};
}
}

bool GripperLedEffectRenderer::parseStateText(const std::string &text, GripperLedEffect *effect)
{
    if (effect == nullptr)
    {
        return false;
    }

    const auto colon = text.find(':');
    const std::string stateText = text.substr(0, colon);
    const std::string progressText = (colon == std::string::npos) ? std::string() : text.substr(colon + 1);

    GripperLedEffect parsed;
    if (stateText == "BOOT_INIT")
    {
        parsed.state = GripperLedEffectState::BootInit;
    }
    else if (stateText == "INIT")
    {
        parsed.state = GripperLedEffectState::Init;
    }
    else if (stateText == "WRITING")
    {
        parsed.state = GripperLedEffectState::Writing;
    }
    else if (stateText == "WAIT_STORAGE")
    {
        parsed.state = GripperLedEffectState::WaitStorage;
    }
    else if (stateText == "READY")
    {
        parsed.state = GripperLedEffectState::Ready;
    }
    else if (stateText == "WARNING")
    {
        parsed.state = GripperLedEffectState::Warning;
    }
    else if (stateText == "TACTILE_WARNING_L")
    {
        parsed.state = GripperLedEffectState::TactileWarningLeftSensor;
    }
    else if (stateText == "TACTILE_WARNING_R")
    {
        parsed.state = GripperLedEffectState::TactileWarningRightSensor;
    }
    else if (stateText == "TACTILE_WARNING_BOTH")
    {
        parsed.state = GripperLedEffectState::TactileWarningBothSensors;
    }
    else if (stateText == "RECORDING")
    {
        parsed.state = GripperLedEffectState::Recording;
    }
    else if (stateText == "ERROR" || stateText == "ERROR_5")
    {
        parsed.state = GripperLedEffectState::Error5;
    }
    else if (stateText == "ERROR_1")
    {
        parsed.state = GripperLedEffectState::Error1;
    }
    else if (stateText == "ERROR_2")
    {
        parsed.state = GripperLedEffectState::Error2;
    }
    else if (stateText == "ERROR_3")
    {
        parsed.state = GripperLedEffectState::Error3;
    }
    else if (stateText == "ERROR_4")
    {
        parsed.state = GripperLedEffectState::Error4;
    }
    else if (stateText == "ERROR_2_UNKNOWN")
    {
        parsed.state = GripperLedEffectState::Error2Unknown;
    }
    else if (stateText == "CALIB_PRE")
    {
        parsed.state = GripperLedEffectState::CalibPre;
    }
    else if (stateText == "CALIB_RUN")
    {
        parsed.state = GripperLedEffectState::CalibRun;
    }
    else if (stateText == "CALIB_DONE")
    {
        parsed.state = GripperLedEffectState::CalibDone;
    }
    else if (stateText == "EXIT")
    {
        parsed.state = GripperLedEffectState::Exit;
    }
    else
    {
        return false;
    }

    if (!progressText.empty())
    {
        try
        {
            parsed.progress = clampProgress(std::stod(progressText));
        }
        catch (...)
        {
            return false;
        }
    }

    *effect = parsed;
    return true;
}

std::string GripperLedEffectRenderer::stateText(const GripperLedEffect &effect)
{
    switch (effect.state)
    {
    case GripperLedEffectState::BootInit:
        return "BOOT_INIT";
    case GripperLedEffectState::Init:
        return "INIT";
    case GripperLedEffectState::Writing:
        return "WRITING";
    case GripperLedEffectState::WaitStorage:
        return "WAIT_STORAGE";
    case GripperLedEffectState::Ready:
        return "READY";
    case GripperLedEffectState::Warning:
        return "WARNING";
    case GripperLedEffectState::TactileWarningLeftSensor:
        return "TACTILE_WARNING_L";
    case GripperLedEffectState::TactileWarningRightSensor:
        return "TACTILE_WARNING_R";
    case GripperLedEffectState::TactileWarningBothSensors:
        return "TACTILE_WARNING_BOTH";
    case GripperLedEffectState::Recording:
        return "RECORDING";
    case GripperLedEffectState::Error1:
        return "ERROR_1";
    case GripperLedEffectState::Error2:
        return "ERROR_2";
    case GripperLedEffectState::Error3:
        return "ERROR_3";
    case GripperLedEffectState::Error4:
        return "ERROR_4";
    case GripperLedEffectState::Error5:
        return "ERROR_5";
    case GripperLedEffectState::Error2Unknown:
        return "ERROR_2_UNKNOWN";
    case GripperLedEffectState::CalibPre:
        return "CALIB_PRE";
    case GripperLedEffectState::CalibRun:
        return "CALIB_RUN:" + std::to_string(effect.progress);
    case GripperLedEffectState::CalibDone:
        return "CALIB_DONE";
    case GripperLedEffectState::Exit:
    default:
        return "EXIT";
    }
}

GripperLedColor GripperLedEffectRenderer::render(const GripperLedEffect &effect, uint64_t steadyMs, uint64_t epochMs) const
{
    switch (effect.state)
    {
    case GripperLedEffectState::BootInit:
    {
        const bool on = (steadyMs % kCalibBlinkPeriodMs) < (kCalibBlinkPeriodMs / 2);
        return on ? GripperLedColor{0, 122, 255} : GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::Init:
    {
        const bool on = (steadyMs % kCalibBlinkPeriodMs) < (kCalibBlinkPeriodMs / 2);
        return on ? GripperLedColor{0, 122, 255} : GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::Writing:
        return GripperLedColor{0, 122, 255};
    case GripperLedEffectState::WaitStorage:
        return GripperLedColor{255, 255, 255};
    case GripperLedEffectState::Ready:
    {
        const double phase = static_cast<double>(steadyMs % kReadyBreathPeriodMs) /
                             static_cast<double>(kReadyBreathPeriodMs);
        const double pulse = 0.5 + 0.5 * std::cos(2.0 * kPi * phase);
        const double shaped = pulse * pulse * pulse;
        const int level = static_cast<int>(std::lround(120.0 * shaped));
        return GripperLedColor{0, clampToU8(level), clampToU8((12 * level) / 150)};
    }
    case GripperLedEffectState::Warning:
    {
        const bool on = (steadyMs % kRecordingBlinkPeriodMs) < (kRecordingBlinkPeriodMs / 2);
        return on ? GripperLedColor{255, 110, 0} : GripperLedColor{30, 10, 0};
    }
    case GripperLedEffectState::TactileWarningLeftSensor:
        return renderTactileWarningPattern(steadyMs, TactileWarningPattern::OneLongOneShort);
    case GripperLedEffectState::TactileWarningRightSensor:
        return renderTactileWarningPattern(steadyMs, TactileWarningPattern::OneLongTwoShort);
    case GripperLedEffectState::TactileWarningBothSensors:
        return renderTactileWarningPattern(steadyMs, TactileWarningPattern::TwoLong);
    case GripperLedEffectState::Recording:
    {
        const bool on = (steadyMs % kRecordingBlinkPeriodMs) < (kRecordingBlinkPeriodMs / 2);
        return on ? GripperLedColor{0, 255, 0} : GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::CalibDone:
    {
        const bool on = (steadyMs % kCalibBlinkPeriodMs) < (kCalibBlinkPeriodMs / 2);
        return on ? GripperLedColor{0, 255, 20} : GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::CalibPre:
    {
        const bool on = (steadyMs % kCalibBlinkPeriodMs) < (kCalibBlinkPeriodMs / 2);
        return on ? GripperLedColor{255, 80, 0} : GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::CalibRun:
    {
        const double progress = clampProgress(effect.progress);
        const double frequencyHz = 1.0 + (progress * 7.0);
        const uint64_t periodMs = std::max<uint64_t>(125, static_cast<uint64_t>(std::llround(1000.0 / frequencyHz)));
        const bool on = (steadyMs % periodMs) < std::max<uint64_t>(1, periodMs / 2);
        return on ? GripperLedColor{255, 80, 0} : GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::Error1:
    case GripperLedEffectState::Error2:
    case GripperLedEffectState::Error3:
    case GripperLedEffectState::Error4:
    case GripperLedEffectState::Error5:
    {
        const int level = errorLevel(effect.state);
        uint64_t cycleMs = 0;
        for (int index = 0; index <= level; ++index)
        {
            cycleMs += (index == 0) ? kErrorLongOnMs : kErrorShortOnMs;
            cycleMs += (index == level) ? kErrorSequenceGapMs : kErrorPulseGapMs;
        }

        uint64_t phaseMs = (cycleMs == 0) ? 0 : (steadyMs % cycleMs);
        for (int index = 0; index <= level; ++index)
        {
            const uint64_t onMs = (index == 0) ? kErrorLongOnMs : kErrorShortOnMs;
            if (phaseMs < onMs)
            {
                return GripperLedColor{255, 0, 0};
            }
            phaseMs -= onMs;

            const uint64_t offMs = (index == level) ? kErrorSequenceGapMs : kErrorPulseGapMs;
            if (phaseMs < offMs)
            {
                return GripperLedColor{0, 0, 0};
            }
            phaseMs -= offMs;
        }

        return GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::Error2Unknown:
    {
        constexpr int kError2Level = 1;
        uint64_t errorCycleMs = 0;
        for (int index = 0; index <= kError2Level; ++index)
        {
            errorCycleMs += (index == 0) ? kErrorLongOnMs : kErrorShortOnMs;
            errorCycleMs += (index == kError2Level) ? kErrorSequenceGapMs : kErrorPulseGapMs;
        }

        const uint64_t fullCycleMs = errorCycleMs * 2;
        uint64_t phaseMs = (fullCycleMs == 0) ? 0 : (steadyMs % fullCycleMs);
        if (phaseMs >= errorCycleMs)
        {
            return GripperLedColor{255, 0, 0};
        }

        for (int index = 0; index <= kError2Level; ++index)
        {
            const uint64_t onMs = (index == 0) ? kErrorLongOnMs : kErrorShortOnMs;
            if (phaseMs < onMs)
            {
                return GripperLedColor{255, 0, 0};
            }
            phaseMs -= onMs;

            const uint64_t offMs = (index == kError2Level) ? kErrorSequenceGapMs : kErrorPulseGapMs;
            if (phaseMs < offMs)
            {
                return GripperLedColor{0, 0, 0};
            }
            phaseMs -= offMs;
        }

        return GripperLedColor{0, 0, 0};
    }
    case GripperLedEffectState::Exit:
    default:
        return GripperLedColor{0, 0, 0};
    }
}

int GripperLedEffectRenderer::errorLevel(GripperLedEffectState state)
{
    switch (state)
    {
    case GripperLedEffectState::Error1:
        return 1;
    case GripperLedEffectState::Error2:
        return 2;
    case GripperLedEffectState::Error3:
        return 3;
    case GripperLedEffectState::Error4:
        return 4;
    case GripperLedEffectState::Error5:
    default:
        return 5;
    }
}

uint8_t GripperLedEffectRenderer::clampToU8(int value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return static_cast<uint8_t>(value);
}
