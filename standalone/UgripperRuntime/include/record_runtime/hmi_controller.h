#pragma once

#include "record_runtime/runtime_types.h"

#include <cstdint>
#include <vector>

namespace ugripper::runtime {

struct HmiControllerOptions
{
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
    HmiControllerOptions options_;
    NowMsFn now_ms_fn_ = nullptr;
    ButtonSnapshot last_buttons_{};
    ButtonStateTracker tracker_{};
    uint64_t last_button_action_ms_ = 0;
};

}  // namespace ugripper::runtime
