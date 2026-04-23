#include "utils/time_utils.h"

#include <gtest/gtest.h>

namespace {

TEST(TimeUtilsTest, SteadyClockDoesNotGoBackwards)
{
    const auto first = utils::CurrentSteadyMs();
    const auto second = utils::CurrentSteadyMs();

    EXPECT_LE(first, second);
}

TEST(TimeUtilsTest, EpochValuesArePositiveAndConsistent)
{
    const auto epochMs = utils::CurrentEpochMs();
    const auto epochUs = utils::CurrentEpochUs();

    EXPECT_GT(epochMs, 0U);
    EXPECT_GT(epochUs, 0);
    EXPECT_GE(epochUs, static_cast<int64_t>(epochMs) * 1000);
    EXPECT_LT(epochUs - static_cast<int64_t>(epochMs) * 1000, 1000);
}

}  // namespace
