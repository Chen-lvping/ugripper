#include "sensor_mcap_checker.h"

#include <gtest/gtest.h>
#include <mcap/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct TestMessage {
    std::string topic;
    uint32_t sequence = 0;
    uint64_t log_time_ns = 0;
};

fs::path MakeTempDir()
{
    char dir_template[] = "/tmp/check_sensor_mcap_testXXXXXX";
    const char* created = mkdtemp(dir_template);
    EXPECT_NE(created, nullptr);
    return fs::path(created);
}

bool WriteSyntheticMcap(const fs::path& path, const std::vector<TestMessage>& messages, std::string* error)
{
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("sensor_checker_test");
    options.noChunking = true;
    options.noRepeatedSchemas = false;
    options.noRepeatedChannels = false;
    options.noMessageIndex = true;
    const auto status = writer.open(path.string(), options);
    if (!status.ok())
    {
        if (error != nullptr)
        {
            *error = status.message;
        }
        return false;
    }

    mcap::Schema schema("SyntheticSensorFrame", "jsonschema", "{}");
    writer.addSchema(schema);

    std::map<std::string, mcap::Channel> channels;
    for (const auto& message : messages)
    {
        if (channels.find(message.topic) != channels.end())
        {
            continue;
        }
        auto [it, inserted] =
            channels.emplace(message.topic, mcap::Channel(message.topic, "binary", schema.id));
        (void)inserted;
        writer.addChannel(it->second);
    }

    const std::vector<std::byte> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    for (const auto& message : messages)
    {
        mcap::Message mcap_message;
        mcap_message.channelId = channels.at(message.topic).id;
        mcap_message.sequence = message.sequence;
        mcap_message.logTime = message.log_time_ns;
        mcap_message.publishTime = message.log_time_ns;
        mcap_message.data = payload.data();
        mcap_message.dataSize = payload.size();
        const auto write_status = writer.write(mcap_message);
        if (!write_status.ok())
        {
            writer.terminate();
            if (error != nullptr)
            {
                *error = write_status.message;
            }
            return false;
        }
    }

    writer.close();
    return true;
}

TEST(SensorMcapCheckerTest, PassesForContiguousExpectedTopics)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "ok.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 10, 100},
            {"encoder_left", 11, 200},
            {"encoder_right", 20, 120},
            {"encoder_right", 21, 220},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left", "encoder_right"};
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.failures.empty());
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").message_count, 2U);
    EXPECT_EQ(result.topics.at("encoder_left").sequence_gap_count, 0U);

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, DetectsSequenceGap)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "gap.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_left", 3, 200},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left"};
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").sequence_gap_count, 1U);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(result.failures.front(), "encoder_left: sequence gap: expected 2, got 3");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, DetectsTimestampRegression)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "regression.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 7, 300},
            {"encoder_left", 8, 250},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left"};
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").timestamp_regression_count, 1U);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(result.failures.front(), "encoder_left: timestamp regression: prev=300, cur=250");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, DetectsGapLargerThanConfiguredThreshold)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "large_gap.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_left", 2, 500},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left"};
    options.max_allowed_gap_ns = 200;
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").max_gap_ns, 400U);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(result.failures.front(), "encoder_left: gap too large: 400ns > 200ns");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, FailsWhenExpectedTopicMissing)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "missing_topic.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_left", 2, 200},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left", "encoder_right"};
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(result.failures.front(), "missing topic or zero messages: encoder_right");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, AllowsLargeGapWhenThresholdNotConfigured)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "large_gap_allowed.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_left", 2, 1000},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left"};
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_TRUE(result.ok);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").max_gap_ns, 900U);
    EXPECT_TRUE(result.failures.empty());

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, DetectsMessageCountBelowConfiguredThreshold)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "low_message_count.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left"};
    options.min_message_count = 2;
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").message_count, 1U);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(result.failures.front(), "encoder_left: message count too low: 1 < 2");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, DetectsSpanShorterThanConfiguredThreshold)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "short_span.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_left", 2, 150},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left"};
    options.min_span_ns = 100;
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    EXPECT_EQ(result.topics.at("encoder_left").span_ns, 50U);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(result.failures.front(), "encoder_left: span too short: 50ns < 100ns");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, DetectsSpanGapBetweenExpectedTopics)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "span_gap.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_right", 10, 100},
            {"encoder_left", 2, 300},
            {"encoder_right", 11, 250},
            {"encoder_left", 3, 500},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left", "encoder_right"};
    options.max_span_gap_ns = 100;
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.reference_span_ns, 400U);
    ASSERT_TRUE(result.topics.contains("encoder_left"));
    ASSERT_TRUE(result.topics.contains("encoder_right"));
    EXPECT_EQ(result.topics.at("encoder_left").span_ns, 400U);
    EXPECT_EQ(result.topics.at("encoder_right").span_ns, 150U);
    ASSERT_EQ(result.failures.size(), 1U);
    EXPECT_EQ(
        result.failures.front(),
        "encoder_right: span gap too large: span=150ns, reference=400ns, gap=250ns > 100ns");

    fs::remove_all(temp_dir);
}

TEST(SensorMcapCheckerTest, AllowsExpectedTopicsWhenSpanGapWithinThreshold)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path input = temp_dir / "span_gap_ok.mcap";
    std::string error;
    ASSERT_TRUE(WriteSyntheticMcap(
        input,
        {
            {"encoder_left", 1, 100},
            {"encoder_right", 10, 100},
            {"encoder_left", 2, 300},
            {"encoder_right", 11, 260},
            {"encoder_left", 3, 500},
            {"encoder_right", 12, 460},
        },
        &error))
        << error;

    ugripper::sensor::testing::Options options;
    options.expect_topics = {"encoder_left", "encoder_right"};
    options.max_span_gap_ns = 100;
    const auto result = ugripper::sensor::testing::CheckFile(input, options);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.reference_span_ns, 400U);
    EXPECT_TRUE(result.failures.empty());

    fs::remove_all(temp_dir);
}

}  // namespace
