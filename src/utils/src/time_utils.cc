#include "utils/time_utils.h"

#include <chrono>

namespace utils {

uint64_t CurrentSteadyMs()
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

uint64_t CurrentEpochMs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

int64_t CurrentEpochUs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

}  // namespace utils
