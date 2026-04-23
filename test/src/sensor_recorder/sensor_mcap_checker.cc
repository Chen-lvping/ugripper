#include "sensor_mcap_checker.h"

#include <mcap/reader.hpp>

#include <unordered_set>

namespace ugripper::sensor::testing {

FileResult CheckFile(const fs::path& path, const Options& options)
{
    FileResult result;
    result.path = path;

    if (!fs::exists(path))
    {
        result.failures.push_back("file does not exist");
        return result;
    }
    if (fs::file_size(path) == 0)
    {
        result.failures.push_back("file is empty");
        return result;
    }

    mcap::McapReader reader;
    const auto open_status = reader.open(path.string());
    if (!open_status.ok())
    {
        result.failures.push_back("failed to open mcap: " + open_status.message);
        return result;
    }

    std::string summary_problem;
    const auto summary_status = reader.readSummary(
        mcap::ReadSummaryMethod::AllowFallbackScan,
        [&summary_problem](const mcap::Status& status) {
            if (summary_problem.empty())
            {
                summary_problem = status.message;
            }
        });
    if (!summary_status.ok())
    {
        result.failures.push_back("failed to read mcap summary: " + summary_status.message);
        if (!summary_problem.empty())
        {
            result.failures.push_back("summary problem: " + summary_problem);
        }
        return result;
    }

    std::unordered_set<std::string> expected_topics(options.expect_topics.begin(), options.expect_topics.end());
    if (expected_topics.empty())
    {
        for (const auto& [channel_id, channel] : reader.channels())
        {
            (void)channel_id;
            if (channel != nullptr)
            {
                expected_topics.insert(channel->topic);
            }
        }
    }

    std::string read_problem;
    for (const auto& message_view : reader.readMessages(
             [&read_problem](const mcap::Status& status) {
                 if (read_problem.empty())
                 {
                     read_problem = status.message;
                 }
             }))
    {
        if (message_view.channel == nullptr)
        {
            result.failures.push_back("message without channel metadata");
            continue;
        }

        const std::string topic = message_view.channel->topic;
        auto& stats = result.topics[topic];
        const auto& message = message_view.message;
        if (!stats.has_first)
        {
            stats.has_first = true;
            stats.first_sequence = message.sequence;
            stats.first_log_time_ns = message.logTime;
        }
        else
        {
            if (options.require_seq_continuous)
            {
                const uint32_t expected = stats.last_sequence + 1;
                if (message.sequence != expected)
                {
                    ++stats.sequence_gap_count;
                    if (stats.failures.size() < 8)
                    {
                        stats.failures.push_back(
                            "sequence gap: expected " + std::to_string(expected) +
                            ", got " + std::to_string(message.sequence));
                    }
                }
            }

            if (options.require_monotonic_log_time && message.logTime < stats.last_log_time_ns)
            {
                ++stats.timestamp_regression_count;
                if (stats.failures.size() < 8)
                {
                    stats.failures.push_back(
                        "timestamp regression: prev=" + std::to_string(stats.last_log_time_ns) +
                        ", cur=" + std::to_string(message.logTime));
                }
            }

            if (message.logTime >= stats.last_log_time_ns)
            {
                const uint64_t gap_ns = message.logTime - stats.last_log_time_ns;
                if (gap_ns > stats.max_gap_ns)
                {
                    stats.max_gap_ns = gap_ns;
                }
                if (options.max_allowed_gap_ns.has_value() && gap_ns > *options.max_allowed_gap_ns)
                {
                    if (stats.failures.size() < 8)
                    {
                        stats.failures.push_back(
                            "gap too large: " + std::to_string(gap_ns) + "ns > " +
                            std::to_string(*options.max_allowed_gap_ns) + "ns");
                    }
                }
            }
        }

        ++stats.message_count;
        stats.last_sequence = message.sequence;
        stats.last_log_time_ns = message.logTime;
        stats.span_ns = stats.last_log_time_ns - stats.first_log_time_ns;
    }

    if (!read_problem.empty())
    {
        result.failures.push_back("message scan problem: " + read_problem);
    }

    for (const auto& topic : expected_topics)
    {
        auto it = result.topics.find(topic);
        if (it == result.topics.end() || it->second.message_count == 0)
        {
            result.failures.push_back("missing topic or zero messages: " + topic);
            continue;
        }
        if (it->second.span_ns > result.reference_span_ns)
        {
            result.reference_span_ns = it->second.span_ns;
        }
        if (options.min_message_count.has_value() && it->second.message_count < *options.min_message_count)
        {
            result.failures.push_back(
                topic + ": message count too low: " + std::to_string(it->second.message_count) + " < " +
                std::to_string(*options.min_message_count));
        }
        if (options.min_span_ns.has_value() && it->second.span_ns < *options.min_span_ns)
        {
            result.failures.push_back(
                topic + ": span too short: " + std::to_string(it->second.span_ns) + "ns < " +
                std::to_string(*options.min_span_ns) + "ns");
        }
        for (const auto& failure : it->second.failures)
        {
            result.failures.push_back(topic + ": " + failure);
        }
    }

    if (options.max_span_gap_ns.has_value())
    {
        for (const auto& topic : expected_topics)
        {
            const auto it = result.topics.find(topic);
            if (it == result.topics.end() || it->second.message_count == 0)
            {
                continue;
            }
            const uint64_t gap_ns = result.reference_span_ns > it->second.span_ns
                                        ? result.reference_span_ns - it->second.span_ns
                                        : 0;
            if (gap_ns > *options.max_span_gap_ns)
            {
                result.failures.push_back(
                    topic + ": span gap too large: span=" + std::to_string(it->second.span_ns) +
                    "ns, reference=" + std::to_string(result.reference_span_ns) +
                    "ns, gap=" + std::to_string(gap_ns) + "ns > " +
                    std::to_string(*options.max_span_gap_ns) + "ns");
            }
        }
    }

    result.ok = result.failures.empty();
    return result;
}

json ToJson(const FileResult& result)
{
    json value = json::object();
    value["path"] = result.path.string();
    value["ok"] = result.ok;
    value["reference_span_ns"] = static_cast<int64_t>(result.reference_span_ns);
    value["failures"] = result.failures;
    json topics = json::object();
    for (const auto& [topic, stats] : result.topics)
    {
        topics[topic] = {
            {"message_count", stats.message_count},
            {"first_sequence", stats.has_first ? static_cast<int64_t>(stats.first_sequence) : -1},
            {"last_sequence", stats.has_first ? static_cast<int64_t>(stats.last_sequence) : -1},
            {"first_log_time_ns", stats.has_first ? static_cast<int64_t>(stats.first_log_time_ns) : -1},
            {"last_log_time_ns", stats.has_first ? static_cast<int64_t>(stats.last_log_time_ns) : -1},
            {"span_ns", static_cast<int64_t>(stats.span_ns)},
            {"max_gap_ns", static_cast<int64_t>(stats.max_gap_ns)},
            {"sequence_gap_count", stats.sequence_gap_count},
            {"timestamp_regression_count", stats.timestamp_regression_count},
            {"failures", stats.failures},
        };
    }
    value["topics"] = std::move(topics);
    return value;
}

}  // namespace ugripper::sensor::testing
