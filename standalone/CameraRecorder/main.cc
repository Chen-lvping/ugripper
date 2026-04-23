#include "camera_recorder/camera_recorder.h"
#include "camera_recorder/logging_compat.h"

#include <filesystem>
#include <system_error>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path ResolveConfigYamlPath(const camera_recorder::Options& options, const char* argv0) {
    namespace fs = std::filesystem;

    auto exists = [](const fs::path& path) {
        std::error_code error;
        return !path.empty() && fs::exists(path, error);
    };

    if (exists(options.config_yaml)) {
        return options.config_yaml;
    }

    std::vector<fs::path> candidates;
    candidates.push_back(options.config_yaml);

    if (!options.config_yaml_explicit) {
        candidates.push_back(fs::path("bin/CameraRecorder/config") / options.config_yaml.filename());
        if (argv0 != nullptr && *argv0 != '\0') {
            std::error_code error;
            const fs::path executable_path = fs::absolute(fs::path(argv0), error);
            const fs::path executable_dir = error ? fs::path(argv0).parent_path() : executable_path.parent_path();
            if (!executable_dir.empty()) {
                candidates.push_back(executable_dir / "config" / options.config_yaml.filename());
            }
        }
    }

    for (const auto& candidate : candidates) {
        if (candidate != options.config_yaml && exists(candidate)) {
            return candidate;
        }
    }

    std::string message = "camera config yaml not found";
    for (const auto& candidate : candidates) {
        message += ": " + candidate.string();
    }
    throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        using namespace camera_recorder;

        Options options = ParseArgs(argc, argv);
        options.config_yaml = ResolveConfigYamlPath(options, argc > 0 ? argv[0] : nullptr);
        DM_LOG_INFO("[camera_recorder] config_yaml={}", options.config_yaml.string());
        const std::vector<CameraConfig> configs = LoadCameraConfigList(options.config_yaml);
        InstallSignalHandlers();

        if (options.stereo_daemon) {
            return RunStereoDaemon(options, configs) ? 0 : 1;
        }

        if (options.apply_uvc_roll_only) {
            return ApplyUvcRollForSelectedCameras(options, configs) ? 0 : 1;
        }

        std::filesystem::create_directories(options.output_dir);

        CameraRecorderManager manager(options);
        std::vector<std::string> missing_devices;
        manager.Prepare(configs, &missing_devices);

        if (!missing_devices.empty()) {
            DM_LOG_ERROR("[camera_recorder] missing devices:");
            for (const auto& device : missing_devices) {
                DM_LOG_ERROR("  - {}", device);
            }
            if (!options.allow_missing) {
                return 2;
            }
        }

        if (manager.empty()) {
            DM_LOG_ERROR("[camera_recorder] no camera process to start");
            return 2;
        }

        if (options.dry_run) {
            for (const auto& recorder : manager.recorders()) {
                std::cout << recorder->config().name << " ["
                          << ugripper::camera::ModeName(recorder->config().mode) << "]:\n"
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
        DM_LOG_ERROR("[camera_recorder] {}", ex.what());
        return 1;
    }
}
