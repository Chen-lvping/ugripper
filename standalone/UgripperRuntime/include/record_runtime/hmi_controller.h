#pragma once

#include "record_runtime/runtime_types.h"

#include <cstdint>
#include <vector>

namespace ugripper::runtime {

struct HmiControllerOptions
{
    uint64_t press_debounce_ms = 40;
    uint64_t release_debounce_ms = 40;
    uint64_t action_debounce_ms = 80;
    uint64_t long_press_threshold_ms = 800;
    uint64_t dual_long_press_threshold_ms = 4000;
    uint64_t shutdown_prompt_threshold_ms = 2000;
};

class HmiController
{
public:
    using NowMsFn = uint64_t (*)();

    HmiController(HmiControllerOptions options, NowMsFn now_ms_fn);

    std::vector<HmiEvent> HandleButtons(const ButtonSnapshot& buttons);
    void Reset();

private:
    struct DebouncedButtonState
    {
        bool raw_pressed = false;
        bool stable_pressed = false;
        uint64_t raw_changed_ms = 0;
    };

    bool updateDebouncedButton(bool raw_pressed,
                               uint64_t now_ms,
                               uint64_t debounce_ms,
                               DebouncedButtonState *state);

    HmiControllerOptions options_;
    NowMsFn now_ms_fn_ = nullptr;
    DebouncedButtonState up_button_{};
    DebouncedButtonState down_button_{};
    ButtonSnapshot last_buttons_{};
    ButtonStateTracker tracker_{};
    uint64_t last_button_action_ms_ = 0;
};

}  // namespace ugripper::runtime
