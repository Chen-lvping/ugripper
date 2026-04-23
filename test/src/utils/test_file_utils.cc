#include "utils/file_utils.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

class FileUtilsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tempDir_ = std::filesystem::temp_directory_path() / "ugripper_test_file_utils";
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(tempDir_, error);
    }

    std::filesystem::path MakePath(const std::string &name) const
    {
        return tempDir_ / name;
    }

private:
    std::filesystem::path tempDir_;
};

TEST_F(FileUtilsTest, ReturnsFalseForMissingFile)
{
    EXPECT_FALSE(utils::FileExistsAndNotEmpty(MakePath("missing.txt").string()));
}

TEST_F(FileUtilsTest, ReturnsFalseForEmptyFile)
{
    std::ofstream output(MakePath("empty.txt"));
    output.flush();

    EXPECT_FALSE(utils::FileExistsAndNotEmpty(MakePath("empty.txt").string()));
}

TEST_F(FileUtilsTest, ReturnsTrueForNonEmptyFile)
{
    std::ofstream output(MakePath("data.txt"));
    output << "ok";
    output.flush();

    EXPECT_TRUE(utils::FileExistsAndNotEmpty(MakePath("data.txt").string()));
}

}  // namespace
