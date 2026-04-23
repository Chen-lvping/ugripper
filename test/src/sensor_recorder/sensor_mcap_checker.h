#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ugripper::sensor::testing {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct TopicStats {
    uint64_t message_count = 0;
    uint32_t first_sequence = 0;
    uint32_t last_sequence = 0;
    uint64_t first_log_time_ns = 0;
    uint64_t last_log_time_ns = 0;
    uint64_t span_ns = 0;
    uint64_t max_gap_ns = 0;
    bool has_first = false;
    uint64_t sequence_gap_count = 0;
    uint64_t timestamp_regression_count = 0;
    std::vector<std::string> failures;
};

struct Options {
    std::vector<fs::path> inputs;
    std::vector<std::string> expect_topics;
    std::optional<fs::path> json_out;
    std::optional<uint64_t> min_message_count;
    std::optional<uint64_t> min_span_ns;
    std::optional<uint64_t> max_allowed_gap_ns;
    std::optional<uint64_t> max_span_gap_ns;
    bool require_seq_continuous = true;
    bool require_monotonic_log_time = true;
};

struct FileResult {
    fs::path path;
    bool ok = false;
    uint64_t reference_span_ns = 0;
    std::vector<std::string> failures;
    std::unordered_map<std::string, TopicStats> topics;
};

FileResult CheckFile(const fs::path& path, const Options& options);
json ToJson(const FileResult& result);

}  // namespace ugripper::sensor::testing
