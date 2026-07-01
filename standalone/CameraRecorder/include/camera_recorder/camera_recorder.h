#pragma once

#include "camera_recorder/camera_types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace camera_recorder {

namespace fs = std::filesystem;

using CameraConfig = ::ugripper::camera::CameraConfig;
using CameraRecordMode = ::ugripper::camera::CameraRecordMode;

struct Options {
    fs::path output_dir;
    std::string codec;
    int duration_sec = 0;
    bool dry_run = false;
    bool allow_missing = false;
    bool stereo_daemon = false;
    std::string ffmpeg_bin = "ffmpeg";
    std::string gst_bin = "gst-launch-1.0";
    fs::path config_yaml = "config/camera_recorder.yaml";
    bool config_yaml_explicit = false;
    fs::path control_file = "/tmp/umi_stereo_camera_control.json";
    fs::path status_file = "/tmp/umi_stereo_camera_status.json";
    std::set<std::string> only_names;
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
    virtual bool HasWrittenOutput() const = 0;
    virtual std::optional<int64_t> RecordTimeOffsetUs() const = 0;
    virtual std::optional<int64_t> FirstFramePtsUs() const = 0;
    virtual std::optional<int64_t> FirstFrameSystemTimeUs() const = 0;
    virtual std::optional<int64_t> LastFramePtsUs() const = 0;
    virtual std::optional<int64_t> LastFrameSystemTimeUs() const = 0;
    virtual uint64_t ObservedFrameCount() const = 0;
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
    bool WriteInfoJson(bool allow_missing_offsets = false) const;

private:
    bool AllExpectedTimingOffsetsAvailable() const;

    Options options_;
    std::vector<CameraConfig> expected_configs_;
    std::vector<std::unique_ptr<CameraRecorder>> recorders_;
    bool had_failure_ = false;
    bool partial_timing_written_ = false;
};

Options ParseArgs(int argc, char** argv);
std::vector<CameraConfig> LoadCameraConfigList(const fs::path& yaml_path);
std::string ModeName(CameraRecordMode mode);
void InstallSignalHandlers();
void ClearCameraRecorderFault(const fs::path& episode_dir);
bool RunStereoDaemon(const Options& options, const std::vector<CameraConfig>& configs);

}  // namespace camera_recorder
