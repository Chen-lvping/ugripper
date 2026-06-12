#include "utils/env_utils.h"
#include "utils/fifo_utils.h"
#include "utils/file_utils.hpp"
#include "utils/time_utils.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

bool writeAll(int fd, const std::string& data)
{
    const char* cursor = data.data();
    size_t remaining = data.size();
    while (remaining > 0)
    {
        const ssize_t written = write(fd, cursor, remaining);
        if (written <= 0)
        {
            return false;
        }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

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

    const fs::path fifoPath = tempDir / "control.pipe";
    std::string fifoError;
    utils::BufferedFifoLineReader reader;
    if (!reader.Open(fifoPath.string(), 0600, &fifoError))
    {
        std::cerr << "BufferedFifoLineReader open failed: " << fifoError << std::endl;
        return 1;
    }

    const int writerFd = open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (writerFd < 0)
    {
        std::cerr << "open fifo writer failed" << std::endl;
        return 1;
    }
    const std::string firstHalf = "ST";
    const std::string secondHalf = "OP\nSTART|episode\n";
    if (!writeAll(writerFd, firstHalf))
    {
        std::cerr << "write first FIFO fragment failed" << std::endl;
        close(writerFd);
        return 1;
    }
    std::vector<std::string> lines;
    if (!reader.ReadAvailable(&lines, &fifoError) || !lines.empty())
    {
        std::cerr << "partial FIFO line should be buffered" << std::endl;
        close(writerFd);
        return 1;
    }
    if (!writeAll(writerFd, secondHalf))
    {
        std::cerr << "write second FIFO fragment failed" << std::endl;
        close(writerFd);
        return 1;
    }
    close(writerFd);
    if (!reader.ReadAvailable(&lines, &fifoError) || lines.size() != 2 ||
        lines[0] != "STOP" || lines[1] != "START|episode")
    {
        std::cerr << "buffered FIFO line assembly failed" << std::endl;
        return 1;
    }
    if (!utils::WriteFifoLine(fifoPath.string(), "PING", true, &fifoError))
    {
        std::cerr << "WriteFifoLine failed: " << fifoError << std::endl;
        return 1;
    }
    if (!reader.ReadAvailable(&lines, &fifoError) || lines.size() != 1 || lines[0] != "PING")
    {
        std::cerr << "WriteFifoLine readback failed" << std::endl;
        return 1;
    }
    reader.Close();

    fs::remove_all(tempDir, error);
    return 0;
}
