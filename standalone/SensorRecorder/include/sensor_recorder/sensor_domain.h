#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ugripper::sensor {

struct TimestampedPayloadFrame {
    uint64_t host_timestamp_ns = 0;
    std::vector<std::byte> payload;
};

struct BatchTimestampSmoothingState {
    std::string label;
    uint64_t nominal_period_ns = 0;
    uint64_t last_assigned_timestamp_ns = 0;
    bool has_last_assigned_timestamp = false;
    size_t smoothed_batch_count = 0;
    size_t smoothed_frame_count = 0;
};

using NowNsFn = uint64_t (*)();

uint64_t CurrentSystemTimeNs();
BatchTimestampSmoothingState CreateBatchTimestampSmoothingState(const std::string &label,
                                                               uint64_t nominal_period_ns);
uint64_t ClampMonotonicTimestamp(BatchTimestampSmoothingState *state,
                                 uint64_t timestamp_ns,
                                 NowNsFn now_ns_fn = &CurrentSystemTimeNs);
std::vector<TimestampedPayloadFrame> SmoothTimestampedBatch(
    BatchTimestampSmoothingState *state,
    std::vector<TimestampedPayloadFrame> frames,
    NowNsFn now_ns_fn = &CurrentSystemTimeNs);

}  // namespace ugripper::sensor
