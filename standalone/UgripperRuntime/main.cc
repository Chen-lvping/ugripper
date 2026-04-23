#include "record_runtime.h"
#include "record_runtime/logging_compat.h"
#include "utils/logger.h"

#include <csignal>
#include <iostream>
#include <string>

namespace {
RecordRuntime *g_runtime = nullptr;

void handleSignal(int signal)
{
    if ((signal == SIGINT || signal == SIGTERM) && g_runtime != nullptr)
    {
        g_runtime->requestStop();
    }
}

void printUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " [--port PATH]... [--camera-bin PATH] [--sensor-bin PATH] [--disk-root PATH] [--poll-ms N]\n"
        << "Defaults:\n"
        << "  --port /dev/right_gripper\n"
        << "  --port /dev/left_gripper\n";
}
}

int main(int argc, char **argv)
{
    const std::string app_name = "UgripperRuntime";
    DM_LOG_INIT(app_name,
                "info",
                "./logs/UgripperRuntime/UgripperRuntime.log",
                1024 * 1024 * 10,
                3,
                false);

    RecordRuntimeOptions options;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            printUsage(argv[0]);
            return 0;
        }
        if (argument == "--port" && index + 1 < argc)
        {
            options.gripperPorts.emplace_back(argv[++index]);
            continue;
        }
        if (argument == "--camera-bin" && index + 1 < argc)
        {
            options.cameraRecorderBin = argv[++index];
            continue;
        }
        if (argument == "--sensor-bin" && index + 1 < argc)
        {
            options.sensorRecorderBin = argv[++index];
            continue;
        }
        if (argument == "--disk-root" && index + 1 < argc)
        {
            options.diskRoot = argv[++index];
            continue;
        }
        if (argument == "--poll-ms" && index + 1 < argc)
        {
            options.pollMs = std::stoi(argv[++index]);
            continue;
        }

        DM_LOG_ERROR_STREAM() << "Unknown argument: " << argument;
        printUsage(argv[0]);
        return 1;
    }

    RecordRuntime runtime(options);
    g_runtime = &runtime;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    if (!runtime.initialize())
    {
        return 1;
    }

    return runtime.run();
}
