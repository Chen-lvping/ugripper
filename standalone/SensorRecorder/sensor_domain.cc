#include "sensor_recorder/sensor_domain.h"

#include <chrono>

namespace ugripper::sensor {

uint64_t CurrentSystemTimeNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

BatchTimestampSmoothingState CreateBatchTimestampSmoothingState(const std::string &label,
                                                               uint64_t nominal_period_ns)
{
    BatchTimestampSmoothingState state;
    state.label = label;
    state.nominal_period_ns = nominal_period_ns;
    return state;
}

uint64_t ClampMonotonicTimestamp(BatchTimestampSmoothingState *state,
                                 uint64_t timestamp_ns,
                                 NowNsFn now_ns_fn)
{
    if (state == nullptr)
    {
        return timestamp_ns;
    }

    if (timestamp_ns == 0)
    {
        timestamp_ns = now_ns_fn();
    }

    if (state->has_last_assigned_timestamp && timestamp_ns <= state->last_assigned_timestamp_ns)
    {
        timestamp_ns = state->last_assigned_timestamp_ns + 1;
    }

    state->last_assigned_timestamp_ns = timestamp_ns;
    state->has_last_assigned_timestamp = true;
    return timestamp_ns;
}

std::vector<TimestampedPayloadFrame> SmoothTimestampedBatch(BatchTimestampSmoothingState *state,
                                                            std::vector<TimestampedPayloadFrame> frames,
                                                            NowNsFn now_ns_fn)
{
    if (state == nullptr || frames.empty())
    {
        return {};
    }

    if (frames.size() == 1)
    {
        frames.front().host_timestamp_ns =
            ClampMonotonicTimestamp(state, frames.front().host_timestamp_ns, now_ns_fn);
        return frames;
    }

    const auto &last_frame = frames.back();
    const uint64_t base_timestamp_ns =
        last_frame.host_timestamp_ns == 0 ? now_ns_fn() : last_frame.host_timestamp_ns;
    const uint64_t batch_span_ns = state->nominal_period_ns * (frames.size() - 1);
    const uint64_t start_timestamp_ns =
        base_timestamp_ns > batch_span_ns ? (base_timestamp_ns - batch_span_ns) : 1;

    for (size_t index = 0; index < frames.size(); ++index)
    {
        frames[index].host_timestamp_ns = ClampMonotonicTimestamp(
            state, start_timestamp_ns + state->nominal_period_ns * index, now_ns_fn);
    }

    ++state->smoothed_batch_count;
    state->smoothed_frame_count += frames.size();
    return frames;
}

}  // namespace ugripper::sensor
