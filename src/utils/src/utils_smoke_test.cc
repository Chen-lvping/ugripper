#include "utils/env_utils.h"
#include "utils/file_utils.hpp"
#include "utils/time_utils.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main()
{
    const fs::path tempDir = fs::temp_directory_path() / "ugripper_utils_smoke";
    std::error_code error;
    fs::create_directories(tempDir, error);
    if (error)
    {
        std::cerr << "create temp dir failed: " << error.message() << std::endl;
        return 1;
    }

    const fs::path envFile = tempDir / "env.txt";
    {
        std::ofstream output(envFile);
        output << "DEVICE_SN=test_sn\n";
        output << "UGRIPPER_LANG=\"en\"\n";
    }

    if (utils::ReadEnvValue(envFile.string(), "DEVICE_SN") != "test_sn")
    {
        std::cerr << "ReadEnvValue DEVICE_SN failed" << std::endl;
        return 1;
    }

    if (utils::ReadEnvValue(envFile.string(), "UGRIPPER_LANG") != "en")
    {
        std::cerr << "ReadEnvValue UGRIPPER_LANG failed" << std::endl;
        return 1;
    }

    if (!utils::FileExistsAndNotEmpty(envFile.string()))
    {
        std::cerr << "FileExistsAndNotEmpty failed" << std::endl;
        return 1;
    }

    const auto steadyMs = utils::CurrentSteadyMs();
    const auto epochMs = utils::CurrentEpochMs();
    const auto epochUs = utils::CurrentEpochUs();
    if (steadyMs == 0 || epochMs == 0 || epochUs == 0)
    {
        std::cerr << "time utils returned zero" << std::endl;
        return 1;
    }

    fs::remove_all(tempDir, error);
    return 0;
}
