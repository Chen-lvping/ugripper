#include "sensor_recorder/sensor_domain.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

uint64_t FixedNowNs()
{
    return 5'000'000'000ULL;
}

std::vector<std::byte> MakePayload(size_t size)
{
    return std::vector<std::byte>(size, std::byte{0x01});
}

TEST(SensorDomainTest, SingleFrameUsesProvidedTimestamp)
{
    auto state = ugripper::sensor::CreateBatchTimestampSmoothingState("imu", 5'000'000ULL);
    std::vector<ugripper::sensor::TimestampedPayloadFrame> frames;
    frames.push_back({123ULL, MakePayload(4)});

    const auto smoothed =
        ugripper::sensor::SmoothTimestampedBatch(&state, std::move(frames), &FixedNowNs);

    ASSERT_EQ(smoothed.size(), 1U);
    EXPECT_EQ(smoothed.front().host_timestamp_ns, 123ULL);
    EXPECT_EQ(state.last_assigned_timestamp_ns, 123ULL);
    EXPECT_EQ(state.smoothed_batch_count, 0U);
    EXPECT_EQ(state.smoothed_frame_count, 0U);
}

TEST(SensorDomainTest, MultiFrameBackfillsFromLastTimestamp)
{
    auto state = ugripper::sensor::CreateBatchTimestampSmoothingState("imu", 10ULL);
    std::vector<ugripper::sensor::TimestampedPayloadFrame> frames;
    frames.push_back({0ULL, MakePayload(1)});
    frames.push_back({0ULL, MakePayload(1)});
    frames.push_back({130ULL, MakePayload(1)});

    const auto smoothed =
        ugripper::sensor::SmoothTimestampedBatch(&state, std::move(frames), &FixedNowNs);

    ASSERT_EQ(smoothed.size(), 3U);
    EXPECT_EQ(smoothed[0].host_timestamp_ns, 110ULL);
    EXPECT_EQ(smoothed[1].host_timestamp_ns, 120ULL);
    EXPECT_EQ(smoothed[2].host_timestamp_ns, 130ULL);
    EXPECT_EQ(state.smoothed_batch_count, 1U);
    EXPECT_EQ(state.smoothed_frame_count, 3U);
}

TEST(SensorDomainTest, MultiFrameClampsBackwardsTimestampsToMonotonicSequence)
{
    auto state = ugripper::sensor::CreateBatchTimestampSmoothingState("imu", 10ULL);
    state.last_assigned_timestamp_ns = 250ULL;
    state.has_last_assigned_timestamp = true;

    std::vector<ugripper::sensor::TimestampedPayloadFrame> frames;
    frames.push_back({0ULL, MakePayload(1)});
    frames.push_back({240ULL, MakePayload(1)});

    const auto smoothed =
        ugripper::sensor::SmoothTimestampedBatch(&state, std::move(frames), &FixedNowNs);

    ASSERT_EQ(smoothed.size(), 2U);
    EXPECT_EQ(smoothed[0].host_timestamp_ns, 251ULL);
    EXPECT_EQ(smoothed[1].host_timestamp_ns, 252ULL);
    EXPECT_EQ(state.last_assigned_timestamp_ns, 252ULL);
}

}  // namespace
