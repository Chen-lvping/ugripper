#include "camera_recorder/camera_recorder.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        using namespace camera_recorder;

        const Options options = ParseArgs(argc, argv);
        const std::vector<CameraConfig> configs = LoadCameraConfigList(options.config_yaml);
        InstallSignalHandlers();

        if (options.stereo_daemon) {
            return RunStereoDaemon(options, configs) ? 0 : 1;
        }

        std::filesystem::create_directories(options.output_dir);

        CameraRecorderManager manager(options);
        std::vector<std::string> missing_devices;
        manager.Prepare(configs, &missing_devices);

        if (!missing_devices.empty()) {
            std::cerr << "[camera_recorder] missing devices:" << std::endl;
            for (const auto& device : missing_devices) {
                std::cerr << "  - " << device << std::endl;
            }
            if (!options.allow_missing) {
                return 2;
            }
        }

        if (manager.empty()) {
            std::cerr << "[camera_recorder] no camera process to start" << std::endl;
            return 2;
        }

        if (options.dry_run) {
            for (const auto& recorder : manager.recorders()) {
                std::cout << recorder->config().name << " [" << ModeName(recorder->config().mode) << "]:\n"
                          << recorder->command() << std::endl;
            }
            return 0;
        }

        if (!manager.StartAll()) {
            manager.StopAll();
            return 1;
        }

        const bool monitor_ok = manager.MonitorUntilStop();
        manager.StopAll();
        const bool info_ok = manager.WriteInfoJson();
        return (monitor_ok && !manager.had_failure() && info_ok) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "[camera_recorder] " << ex.what() << std::endl;
        return 1;
    }
}
