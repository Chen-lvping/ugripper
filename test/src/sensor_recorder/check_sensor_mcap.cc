#include "sensor_mcap_checker.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using ugripper::sensor::testing::CheckFile;
using ugripper::sensor::testing::FileResult;
using ugripper::sensor::testing::Options;
using ugripper::sensor::testing::ToJson;
using json = ugripper::sensor::testing::json;

namespace {

void PrintUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [--expect-topic TOPIC]... [--json-out PATH] [--no-require-seq] "
        << "[--no-require-monotonic] [--min-message-count N] [--min-span-ns N] "
        << "[--max-gap-ns N] [--max-span-gap-ns N] MCAP...\n"
        << "Checks MCAP topic presence, message counts, sequence continuity, monotonic log times, "
        << "optional minimum topic span, optional inter-topic span alignment, and optional max timestamp gap.\n";
}

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
    if (options == nullptr) {
        if (error != nullptr) {
            *error = "internal error: missing options";
        }
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--expect-topic") {
            if (i + 1 >= argc) {
                if (error != nullptr) {
                    *error = "--expect-topic requires a value";
                }
                return false;
            }
            options->expect_topics.push_back(argv[++i]);
        } else if (arg == "--json-out") {
            if (i + 1 >= argc) {
                if (error != nullptr) {
                    *error = "--json-out requires a value";
                }
                return false;
            }
            options->json_out = fs::path(argv[++i]);
        } else if (arg == "--min-message-count") {
            if (i + 1 >= argc) {
                if (error != nullptr) {
                    *error = "--min-message-count requires a value";
                }
                return false;
            }
            try {
                options->min_message_count = static_cast<uint64_t>(std::stoull(argv[++i]));
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "--min-message-count must be an unsigned integer";
                }
                return false;
            }
        } else if (arg == "--min-span-ns") {
            if (i + 1 >= argc) {
                if (error != nullptr) {
                    *error = "--min-span-ns requires a value";
                }
                return false;
            }
            try {
                options->min_span_ns = static_cast<uint64_t>(std::stoull(argv[++i]));
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "--min-span-ns must be an unsigned integer";
                }
                return false;
            }
        } else if (arg == "--no-require-seq") {
            options->require_seq_continuous = false;
        } else if (arg == "--no-require-monotonic") {
            options->require_monotonic_log_time = false;
        } else if (arg == "--max-gap-ns") {
            if (i + 1 >= argc) {
                if (error != nullptr) {
                    *error = "--max-gap-ns requires a value";
                }
                return false;
            }
            try {
                options->max_allowed_gap_ns = static_cast<uint64_t>(std::stoull(argv[++i]));
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "--max-gap-ns must be an unsigned integer";
                }
                return false;
            }
        } else if (arg == "--max-span-gap-ns") {
            if (i + 1 >= argc) {
                if (error != nullptr) {
                    *error = "--max-span-gap-ns requires a value";
                }
                return false;
            }
            try {
                options->max_span_gap_ns = static_cast<uint64_t>(std::stoull(argv[++i]));
            } catch (const std::exception&) {
                if (error != nullptr) {
                    *error = "--max-span-gap-ns must be an unsigned integer";
                }
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            if (error != nullptr) {
                *error = "unknown argument: " + arg;
            }
            return false;
        } else {
            options->inputs.push_back(fs::path(arg));
        }
    }

    if (options->inputs.empty()) {
        if (error != nullptr) {
            *error = "at least one MCAP input is required";
        }
        return false;
    }
    return true;
}

std::string JoinFailures(const std::vector<std::string>& failures) {
    std::ostringstream oss;
    for (size_t i = 0; i < failures.size(); ++i) {
        if (i > 0) {
            oss << "; ";
        }
        oss << failures[i];
    }
    return oss.str();
}
}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!ParseArgs(argc, argv, &options, &error)) {
        std::cerr << "check_sensor_mcap: " << error << std::endl;
        PrintUsage(argv[0]);
        return 2;
    }

    json summary = json::object();
    summary["ok"] = true;
    summary["files"] = json::array();

    bool all_ok = true;
    for (const auto& input : options.inputs) {
        const auto result = CheckFile(input, options);
        summary["files"].push_back(ToJson(result));
        if (result.ok) {
            std::cout << "[PASS] " << input << std::endl;
            std::cout << "  reference_span_ns=" << result.reference_span_ns << std::endl;
            for (const auto& [topic, stats] : result.topics) {
                std::cout << "  - " << topic
                          << ": messages=" << stats.message_count
                          << " first_seq=" << (stats.has_first ? std::to_string(stats.first_sequence) : "n/a")
                          << " last_seq=" << (stats.has_first ? std::to_string(stats.last_sequence) : "n/a")
                          << " span_ns=" << stats.span_ns
                          << " max_gap_ns=" << stats.max_gap_ns
                          << std::endl;
            }
        } else {
            all_ok = false;
            std::cout << "[FAIL] " << input << std::endl;
            std::cout << "  reference_span_ns=" << result.reference_span_ns << std::endl;
            for (const auto& failure : result.failures) {
                std::cout << "  - " << failure << std::endl;
            }
        }
    }

    summary["ok"] = all_ok;

    if (options.json_out.has_value()) {
        std::ofstream output(*options.json_out, std::ios::trunc);
        if (!output.is_open()) {
            std::cerr << "check_sensor_mcap: failed to write json output to "
                      << options.json_out->string() << std::endl;
            return 2;
        }
        output << summary.dump(2) << '\n';
    }

    return all_ok ? 0 : 1;
}
