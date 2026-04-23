#include "utils/env_utils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

class EnvUtilsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tempDir_ = std::filesystem::temp_directory_path() / "ugripper_test_env_utils";
        std::filesystem::create_directories(tempDir_);
        envFile_ = tempDir_ / "environment";
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(tempDir_, error);
    }

    void WriteEnvFile(const std::string &contents)
    {
        std::ofstream output(envFile_);
        output << contents;
    }

    std::filesystem::path envFile_;

private:
    std::filesystem::path tempDir_;
};

TEST_F(EnvUtilsTest, ReadsAndTrimsQuotedValue)
{
    WriteEnvFile("DEVICE_SN=\"  SN_001  \"\n");

    EXPECT_EQ(utils::ReadEnvValue(envFile_.string(), "DEVICE_SN"), "SN_001");
}

TEST_F(EnvUtilsTest, ReturnsEmptyWhenKeyMissing)
{
    WriteEnvFile("UGRIPPER_LANG=en\n");

    EXPECT_TRUE(utils::ReadEnvValue(envFile_.string(), "DEVICE_SN").empty());
}

TEST_F(EnvUtilsTest, IgnoresPartialPrefixMatches)
{
    WriteEnvFile("DEVICE_SN_BACKUP=wrong\nDEVICE_SN=actual\n");

    EXPECT_EQ(utils::ReadEnvValue(envFile_.string(), "DEVICE_SN"), "actual");
}

}  // namespace
