#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace camera_recorder {

namespace fs = std::filesystem;

struct Options {
    fs::path output_dir;
    std::string codec;
    int duration_sec = 0;
    bool dry_run = false;
    bool allow_missing = false;
    std::string ffmpeg_bin = "ffmpeg";
    std::string gst_bin = "gst-launch-1.0";
    fs::path config_yaml = "config/camera_recorder.yaml";
    std::set<std::string> only_names;
};

enum class CameraRecordMode {
    DirectCopyH265,
    HybridDecodeEncode,
    StereoHybridDecodeEncode,
};

struct CameraConfig {
    std::string name;
    std::string device;
    CameraRecordMode mode = CameraRecordMode::HybridDecodeEncode;
    int width = 0;
    int height = 0;
    int fps = 0;
    int output_fps = 0;
    int eye_width = 0;
    int eye_height = 0;
    std::vector<std::string> output_files;
    int input_thread_queue_size = 0;
};

class CameraRecorder {
public:
    virtual ~CameraRecorder() = default;

    virtual const CameraConfig& config() const = 0;
    virtual const std::string& command() const = 0;
    virtual bool Start() = 0;
    virtual void Poll() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual bool HasStarted() const = 0;
    virtual bool HasFailure() const = 0;
    virtual std::optional<int> ExitCode() const = 0;
    virtual std::optional<int64_t> RecordTimeOffsetUs() const = 0;
};

class CameraRecorderManager {
public:
    explicit CameraRecorderManager(Options options);

    bool Prepare(const std::vector<CameraConfig>& configs, std::vector<std::string>* missing_devices);
    bool StartAll();
    bool MonitorUntilStop();
    void StopAll();

    bool empty() const;
    bool had_failure() const;
    const std::vector<std::unique_ptr<CameraRecorder>>& recorders() const;
    bool WriteInfoJson() const;

private:
    Options options_;
    std::vector<std::unique_ptr<CameraRecorder>> recorders_;
    bool had_failure_ = false;
};

Options ParseArgs(int argc, char** argv);
std::vector<CameraConfig> LoadCameraConfigList(const fs::path& yaml_path);
std::string ModeName(CameraRecordMode mode);
void InstallSignalHandlers();

}  // namespace camera_recorder
