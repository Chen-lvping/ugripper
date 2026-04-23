#pragma once

#include <cstdint>

namespace utils {

uint64_t CurrentSteadyMs();
uint64_t CurrentEpochMs();
int64_t CurrentEpochUs();

}  // namespace utils
