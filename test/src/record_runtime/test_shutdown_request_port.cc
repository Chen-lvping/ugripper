#include "record_runtime/shutdown_request_port.h"

#include "utils/time_utils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace {

class TempDir
{
public:
    TempDir()
        : path_(fs::temp_directory_path() /
                ("ugripper_shutdown_port_test_" + std::to_string(utils::CurrentEpochMs())))
    {
        fs::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

TEST(ShutdownRequestPortTest, WritesShutdownRequestFile)
{
    TempDir temp_dir;
    const fs::path request_file = temp_dir.path() / "umi_system_action_request";
    const fs::path result_file = temp_dir.path() / "umi_system_action_result";

    auto port = ugripper::runtime::CreateFileShutdownRequestPort(request_file.string(), result_file.string());

    std::string error;
    ASSERT_TRUE(port->RequestShutdown(&error)) << error;
    ASSERT_TRUE(fs::exists(request_file));

    std::ifstream input(request_file);
    ASSERT_TRUE(input.is_open());
    std::string content;
    std::getline(input, content);
    EXPECT_EQ(content, "shutdown");
}

TEST(ShutdownRequestPortTest, OverwritesExistingRequestFile)
{
    TempDir temp_dir;
    const fs::path request_file = temp_dir.path() / "umi_system_action_request";
    const fs::path result_file = temp_dir.path() / "umi_system_action_result";
    {
        std::ofstream output(request_file, std::ios::trunc);
        output << "stale-content\n";
    }

    auto port = ugripper::runtime::CreateFileShutdownRequestPort(request_file.string(), result_file.string());

    std::string error;
    ASSERT_TRUE(port->RequestShutdown(&error)) << error;

    std::ifstream input(request_file);
    ASSERT_TRUE(input.is_open());
    std::string content;
    std::getline(input, content);
    EXPECT_EQ(content, "shutdown");
}

TEST(ShutdownRequestPortTest, WaitsForSystemActionResult)
{
    TempDir temp_dir;
    const fs::path request_file = temp_dir.path() / "umi_system_action_request";
    const fs::path result_file = temp_dir.path() / "umi_system_action_result";

    auto port = ugripper::runtime::CreateFileShutdownRequestPort(request_file.string(), result_file.string());

    std::thread writer([result_file]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ofstream output(result_file, std::ios::trunc);
        output << "ok\n";
    });

    std::string result;
    std::string error;
    EXPECT_TRUE(port->WaitForActionResult(&result, 500, &error)) << error;
    EXPECT_EQ(result, "ok");

    writer.join();
}

}  // namespace
