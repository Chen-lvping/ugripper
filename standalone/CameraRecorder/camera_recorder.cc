#include "camera_recorder/camera_recorder.h"
#include "camera_recorder/camera_command_builder.h"
#include "camera_recorder/camera_config.h"
#include "camera_recorder/camera_registry.h"
#include "camera_recorder/stereo_control_json.h"
#include "utils/logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdio>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if CAMERA_RECORDER_HAS_GSTREAMER
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#endif
#include <nlohmann/json.hpp>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

namespace camera_recorder {

namespace {

using json = nlohmann::json;

std::atomic<bool> g_stop_requested{false};

constexpr auto kRecorderStopSigintTimeout = std::chrono::milliseconds(3000);
constexpr auto kRecorderStopSigtermTimeout = std::chrono::milliseconds(1000);
constexpr auto kRecorderStopReapTimeout = std::chrono::milliseconds(500);
constexpr auto kRecorderStopPollInterval = std::chrono::milliseconds(50);
constexpr auto kDeviceRebindTimeout = std::chrono::seconds(5);
constexpr auto kDeviceRebindPollInterval = std::chrono::milliseconds(100);
constexpr auto kStereoDaemonPollInterval = std::chrono::milliseconds(200);
constexpr auto kStereoRestartInterval = std::chrono::milliseconds(1000);
constexpr auto kStereoWarmupReadySettle = std::chrono::milliseconds(1200);
constexpr auto kStereoSessionEofGrace = std::chrono::milliseconds(500);
constexpr auto kStereoSessionEofGraceNoOutput = std::chrono::milliseconds(10000);
constexpr auto kStereoSessionSigintTimeout = std::chrono::milliseconds(1500);
constexpr auto kStereoSessionSigintTimeoutNoOutput = std::chrono::milliseconds(3000);
constexpr auto kStereoSessionSigtermTimeout = std::chrono::milliseconds(400);
constexpr int kStereoSessionMaxRestartAttempts = 2;
constexpr int64_t kUsPerSecond = 1'000'000;
constexpr uint64_t kMainCameraLeakyQueueMaxTimeNs = 5ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr uint64_t kMainCameraQueueInfoThresholdNs = 1ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr uint64_t kMainCameraQueueWarnThresholdNs = 4ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr auto kMainCameraQueueDiagInterval = std::chrono::milliseconds(1000);
constexpr auto kMainCameraPushWarnThreshold = std::chrono::milliseconds(200);
constexpr auto kMainCameraStopEosGrace = std::chrono::milliseconds(500);
constexpr auto kMainCameraStopPollInterval = std::chrono::milliseconds(10);
constexpr int64_t kMainCameraShortStreamMinSessionUs = 5 * kUsPerSecond;
constexpr int64_t kMainCameraShortStreamMaxSpanUs = 2 * kUsPerSecond;

#ifndef UGRIPPER_ENABLE_PERF_LOG
#define UGRIPPER_ENABLE_PERF_LOG 0
#endif

constexpr bool kPerfLogEnabled = UGRIPPER_ENABLE_PERF_LOG != 0;

void SignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

bool ConfigureManagedChildProcessGroup() {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
        std::cerr << "[camera_recorder] failed to set parent-death signal: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    if (getppid() == 1) {
        std::cerr << "[camera_recorder] parent exited before child setup completed" << std::endl;
        return false;
    }
    if (setpgid(0, 0) != 0 && errno != EACCES) {
        std::cerr << "[camera_recorder] failed to create child process group: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

#if CAMERA_RECORDER_HAS_GSTREAMER
std::optional<uint64_t> ReadGObjectUintProperty(GObject* object, const char* property_name) {
    if (object == nullptr || property_name == nullptr) {
        return std::nullopt;
    }
    GParamSpec* spec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), property_name);
    if (spec == nullptr) {
        return std::nullopt;
    }

    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(spec));
    g_object_get_property(object, property_name, &value);

    std::optional<uint64_t> result;
    if (G_VALUE_HOLDS_UINT64(&value)) {
        result = g_value_get_uint64(&value);
    } else if (G_VALUE_HOLDS_UINT(&value)) {
        result = g_value_get_uint(&value);
    } else if (G_VALUE_HOLDS_ULONG(&value)) {
        result = g_value_get_ulong(&value);
    } else if (G_VALUE_HOLDS_INT64(&value)) {
        const gint64 raw = g_value_get_int64(&value);
        if (raw >= 0) {
            result = static_cast<uint64_t>(raw);
        }
    } else if (G_VALUE_HOLDS_INT(&value)) {
        const gint raw = g_value_get_int(&value);
        if (raw >= 0) {
            result = static_cast<uint64_t>(raw);
        }
    } else if (G_VALUE_HOLDS_LONG(&value)) {
        const glong raw = g_value_get_long(&value);
        if (raw >= 0) {
            result = static_cast<uint64_t>(raw);
        }
    }

    g_value_unset(&value);
    return result;
}

std::string GstMessageSourceName(GstMessage* message) {
    if (message == nullptr || GST_MESSAGE_SRC(message) == nullptr) {
        return "unknown";
    }
    const gchar* name = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
    return (name != nullptr && *name != '\0') ? std::string(name) : "unknown";
}
#endif

std::string Trim(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const size_t start = input.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
}

std::string SanitizeTimingPathComponent(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            ch == '.' || ch == '_' || ch == '-') {
            result.push_back(ch);
        } else {
            result.push_back('_');
        }
    }
    return result.empty() ? "unknown" : result;
}

fs::path RecordingTimingPathForEpisode(const fs::path& episode_dir) {
    const std::string parent = SanitizeTimingPathComponent(episode_dir.parent_path().filename().string());
    const std::string name = SanitizeTimingPathComponent(episode_dir.filename().string());
    return fs::path("/dev/shm") / ("ugripper_recording_timing_" + parent + "_" + name + ".json");
}

fs::path CameraRecorderFaultPathForEpisode(const fs::path& episode_dir) {
    const std::string parent = SanitizeTimingPathComponent(episode_dir.parent_path().filename().string());
    const std::string name = SanitizeTimingPathComponent(episode_dir.filename().string());
    return fs::path("/dev/shm") / ("ugripper_camera_recorder_fault_" + parent + "_" + name + ".json");
}

std::string ShellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

bool EnsureDirectory(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    return !error;
}

std::vector<std::string> ListSortedFiles(const fs::path& dir, const std::string& extension) {
    std::error_code error;
    std::vector<std::string> files;
    if (!fs::exists(dir, error)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != extension) {
            continue;
        }
        files.push_back(entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string JoinArgumentsForShell(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (size_t index = 0; index < args.size(); ++index) {
        if (index > 0) {
            oss << ' ';
        }
        oss << ShellQuote(args[index]);
    }
    return oss.str();
}

bool RunShellCommandCapture(const std::vector<std::string>& args, std::string* output, int* exit_code) {
    if (output != nullptr) {
        output->clear();
    }
    if (exit_code != nullptr) {
        *exit_code = -1;
    }
    if (args.empty()) {
        return false;
    }

    const std::string command = JoinArgumentsForShell(args) + " 2>&1";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return false;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        if (output != nullptr) {
            *output += buffer;
        }
    }

    const int status = pclose(pipe.release());
    if (exit_code != nullptr) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_code = 128 + WTERMSIG(status);
        } else {
            *exit_code = status;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::optional<json> ReadJsonFile(const fs::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::nullopt;
    }
    try {
        return json::parse(input);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool WriteJsonFile(const fs::path& path, const json& value) {
    const fs::path parent = path.parent_path();
    if (!parent.empty() && !EnsureDirectory(parent)) {
        return false;
    }
    const fs::path temp_path = path.string() + ".tmp";
    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output << value.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            output.close();
            std::error_code cleanup_error;
            fs::remove(temp_path, cleanup_error);
            return false;
        }
    }
    std::error_code rename_error;
    fs::rename(temp_path, path, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        fs::remove(temp_path, cleanup_error);
        return false;
    }
    return true;
}

std::optional<int64_t> ParseSecondsTextUs(const std::string& text) {
    try {
        const long double seconds = std::stold(Trim(text));
        return static_cast<int64_t>(std::llround(seconds * static_cast<long double>(kUsPerSecond)));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

struct VideoWindowUs {
    int64_t start_pts_us = 0;
    int64_t duration_us = 0;
};

std::optional<VideoWindowUs> ProbeVideoWindowUs(const fs::path& path) {
    std::string output;
    int exit_code = -1;
    if (!RunShellCommandCapture({
            "ffprobe",
            "-v", "error",
            "-show_entries", "format=start_time,duration",
            "-of", "default=nokey=1:noprint_wrappers=1",
            path.string(),
        }, &output, &exit_code)) {
        return std::nullopt;
    }

    std::istringstream stream(output);
    std::string start_line;
    std::string duration_line;
    if (!std::getline(stream, start_line) || !std::getline(stream, duration_line)) {
        return std::nullopt;
    }

    const auto start_pts_us = ParseSecondsTextUs(start_line);
    const auto duration_us = ParseSecondsTextUs(duration_line);
    if (!start_pts_us.has_value() || !duration_us.has_value()) {
        return std::nullopt;
    }

    VideoWindowUs window;
    window.start_pts_us = *start_pts_us;
    window.duration_us = *duration_us;
    return window;
}

std::string ReadEnvFileValue(const std::string& key) {
    std::ifstream file("/etc/environment");
    if (!file.is_open()) {
        return "";
    }

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.rfind(key + "=", 0) != 0) {
            continue;
        }
        std::string value = Trim(line.substr(key.size() + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return "";
}

bool PerfLogEnabled() {
    return kPerfLogEnabled;
}

bool ContainsAny(const std::string& text, const std::vector<std::string>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&text](const std::string& needle) {
        return text.find(needle) != std::string::npos;
    });
}

bool IsNoisyRecorderLogLine(const std::string& line) {
    if (line.rfind("frame=", 0) == 0 ||
        line.rfind("fps=", 0) == 0 ||
        line.rfind("out_time_us=", 0) == 0 ||
        line.rfind("out_time_ms=", 0) == 0 ||
        line.rfind("progress=", 0) == 0) {
        return true;
    }
    static const std::vector<std::string> noisyTokens = {
        "MPP", "mpp", "rk_mpp", "rkmpp", "RGA", "rga",
        "Stereo FPS:", "IMU FPS:", "GST_DEBUG", "Redistribute latency",
        "Pipeline is PREROLLING", "Pipeline is PLAYING", "New clock:"
    };
    return ContainsAny(line, noisyTokens);
}

std::string ResolveCodec(const std::string& cli_codec) {
    if (!cli_codec.empty()) {
        return cli_codec;
    }

    const char* env_codec = std::getenv("CAMERA_CODEC");
    if (env_codec != nullptr && std::string(env_codec).size() > 0) {
        return env_codec;
    }

    const std::string file_codec = ReadEnvFileValue("CAMERA_CODEC");
    if (!file_codec.empty()) {
        return file_codec;
    }

    return "h265";
}

bool Exists(const std::string& path) {
    std::error_code error;
    return fs::exists(path, error);
}

bool WaitForDeviceNode(const std::string& device, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (Exists(device)) {
            return true;
        }
        std::this_thread::sleep_for(kDeviceRebindPollInterval);
    }
    return Exists(device);
}

bool IsMainCamera(const CameraConfig& config) {
    return config.name == "left_cam_main" ||
           config.name == "right_cam_main" ||
           config.name == "chest_cam_main";
}

bool IsMainCameraStartupHardwareFailure(const CameraConfig& config, const std::string& error_message) {
    if (!IsMainCamera(config)) {
        return false;
    }
    static const std::vector<std::string> kNeedles = {
        "VIDIOC_",
        "Input/output error",
        "Connection timed out",
        "No such device",
        "device poll error",
        "device lacks capture/streaming capability",
    };
    return ContainsAny(error_message, kNeedles);
}

int64_t FrameSpanUs(const std::optional<int64_t>& first_pts_us,
                    const std::optional<int64_t>& last_pts_us) {
    if (!first_pts_us.has_value() || !last_pts_us.has_value() ||
        *last_pts_us <= *first_pts_us) {
        return 0;
    }
    return *last_pts_us - *first_pts_us;
}

bool IsMainCameraShortStreamFault(int64_t session_us,
                                  uint64_t frame_count,
                                  int64_t span_us,
                                  int fps) {
    if (session_us < kMainCameraShortStreamMinSessionUs) {
        return false;
    }
    const uint64_t min_expected_frames =
        static_cast<uint64_t>(std::max(1, fps)) * 2ULL;
    return span_us <= kMainCameraShortStreamMaxSpanUs ||
           frame_count < min_expected_frames;
}

int64_t CurrentSystemTimeUs();

void RemoveCameraRecorderFaultFile(const fs::path& episode_dir) {
    std::error_code error;
    fs::remove(CameraRecorderFaultPathForEpisode(episode_dir), error);
}

void WriteCameraRecorderFaultJson(const fs::path& episode_dir,
                                  const CameraConfig& config,
                                  const std::string& fault_type,
                                  const std::string& error_message,
                                  const json& extra = json::object()) {
    if (episode_dir.empty() || !IsMainCamera(config)) {
        return;
    }
    const fs::path path = CameraRecorderFaultPathForEpisode(episode_dir);
    const fs::path tmp_path = path.string() + ".tmp";
    json root = json::object();
    root["schema"] = "ugripper_camera_recorder_fault_v1";
    root["fault_type"] = fault_type;
    root["episode_dir"] = episode_dir.string();
    root["camera_name"] = config.name;
    root["device"] = config.device;
    root["error"] = error_message;
    root["system_time_us"] = CurrentSystemTimeUs();
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        root[it.key()] = it.value();
    }

    std::ofstream output(tmp_path, std::ios::trunc);
    if (!output.is_open()) {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
                           << "[camera_recorder] failed to open fault file for write: "
                           << tmp_path).str());
        return;
    }
    output << root.dump(2) << "\n";
    output.close();
    if (!output) {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
                           << "[camera_recorder] failed to close fault file after write: "
                           << tmp_path).str());
        return;
    }
    std::error_code error;
    fs::rename(tmp_path, path, error);
    if (error) {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
                           << "[camera_recorder] failed to publish fault file: "
                           << path << " error=" << error.message()).str());
        fs::remove(tmp_path, error);
        return;
    }
    DM_LOG_ERROR("{}", (::DA::utils::LogString()
                       << "[camera_recorder] main camera hardware fault published: "
                       << path << " camera=" << config.name
                       << " device=" << config.device
                       << " fault_type=" << fault_type
                       << " error=" << error_message).str());
}

void WriteCameraRecorderFault(const fs::path& episode_dir,
                              const CameraConfig& config,
                              const std::string& error_message) {
    if (!IsMainCameraStartupHardwareFailure(config, error_message)) {
        return;
    }
    WriteCameraRecorderFaultJson(
        episode_dir,
        config,
        "main_camera_v4l2_startup_failure",
        error_message);
}

int64_t CurrentSystemTimeUs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

int64_t CurrentSteadyTimeUs() {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

int64_t BootTimeOffsetUs() {
    return CurrentSystemTimeUs() - CurrentSteadyTimeUs();
}

#if CAMERA_RECORDER_HAS_GSTREAMER
bool EnsureGstreamerInitialized(std::string* error_message) {
    static std::once_flag once;
    static bool initialized = false;
    static std::string init_error;
    std::call_once(once, []() {
        GError* error = nullptr;
        initialized = gst_init_check(nullptr, nullptr, &error);
        if (!initialized && error != nullptr) {
            init_error = error->message;
            g_error_free(error);
        }
    });

    if (!initialized && error_message != nullptr) {
        *error_message = init_error.empty() ? "gst_init_check failed" : init_error;
    }
    return initialized;
}

int64_t TimevalToUs(const timeval& value) {
    return static_cast<int64_t>(value.tv_sec) * kUsPerSecond +
           static_cast<int64_t>(value.tv_usec);
}

std::optional<int64_t> V4l2BufferSystemTimeUs(const v4l2_buffer& buffer) {
    const int64_t raw_us = TimevalToUs(buffer.timestamp);
    if (raw_us <= 0) {
        return std::nullopt;
    }
    if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) != 0) {
        return BootTimeOffsetUs() + raw_us;
    }
    return raw_us;
}

#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif
#endif

bool WriteAll(int fd, const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t rc = write(fd, data + written, size - written);
        if (rc > 0) {
            written += static_cast<size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<int64_t> ParseFramePtsUsFromStatsLine(const std::string& line) {
    const size_t separator = line.find(' ');
    if (separator == std::string::npos) {
        return std::nullopt;
    }

    const std::string pts_text = Trim(line.substr(0, separator));
    const std::string tb_text = Trim(line.substr(separator + 1));
    if (pts_text.empty() || tb_text.empty()) {
        return std::nullopt;
    }

    const size_t slash = tb_text.find('/');
    if (slash == std::string::npos) {
        return std::nullopt;
    }

    try {
        const int64_t pts = std::stoll(pts_text);
        const int64_t num = std::stoll(tb_text.substr(0, slash));
        const int64_t den = std::stoll(tb_text.substr(slash + 1));
        if (den == 0) {
            return std::nullopt;
        }
        const long double pts_us =
            (static_cast<long double>(pts) * static_cast<long double>(num) * 1'000'000.0L) /
            static_cast<long double>(den);
        return static_cast<int64_t>(std::llround(pts_us));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> ParseFramePtsUsFromDebugTsLine(const std::string& line) {
    const std::string marker = "muxer <- type:video";
    if (line.find(marker) == std::string::npos) {
        return std::nullopt;
    }

    const std::string key = "pkt_pts_time:";
    const size_t key_pos = line.find(key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }

    size_t value_begin = key_pos + key.size();
    size_t value_end = line.find(' ', value_begin);
    const std::string value_text = Trim(line.substr(value_begin, value_end - value_begin));
    if (value_text.empty() || value_text == "NOPTS") {
        return std::nullopt;
    }

    try {
        const long double pts_sec = std::stold(value_text);
        return static_cast<int64_t>(std::llround(pts_sec * 1'000'000.0L));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> ParseFramePtsUsFromGstIdentityLine(const std::string& line) {
    const std::string marker = "pts: ";
    const size_t marker_pos = line.find(marker);
    if (marker_pos == std::string::npos || line.find("identity") == std::string::npos) {
        return std::nullopt;
    }

    size_t value_begin = marker_pos + marker.size();
    size_t value_end = line.find(',', value_begin);
    const std::string value_text = Trim(line.substr(value_begin, value_end - value_begin));
    if (value_text.empty() || value_text == "none" || value_text == "N/A") {
        return std::nullopt;
    }

    int hours = 0;
    int minutes = 0;
    long double seconds = 0.0;
    if (std::sscanf(value_text.c_str(), "%d:%d:%Lf", &hours, &minutes, &seconds) != 3) {
        return std::nullopt;
    }

    const long double total_seconds =
        static_cast<long double>(hours) * 3600.0L +
        static_cast<long double>(minutes) * 60.0L +
        seconds;
    return static_cast<int64_t>(std::llround(total_seconds * 1'000'000.0L));
}

std::optional<int64_t> ParseFramePtsUsFromProgressLine(const std::string& line) {
    const std::string key = "out_time_us=";
    if (line.rfind(key, 0) != 0) {
        return std::nullopt;
    }
    try {
        return std::stoll(Trim(line.substr(key.size())));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

class ShellCameraRecorder : public CameraRecorder {
public:
    ShellCameraRecorder(CameraConfig config, Options options)
        : config_(std::move(config)), options_(std::move(options)) {}

    ~ShellCameraRecorder() override {
        JoinOutputReaderThread();
    }

    const CameraConfig& config() const override {
        return config_;
    }

    const std::string& command() const override {
        return command_;
    }

    bool Start() override {
        if (started_) {
            return running_;
        }
        if (!BeforeStart()) {
            return false;
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder] starting " << config_.name
                             << " mode=" << ugripper::camera::ModeName(config_.mode)).str());
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder] cmd: " << command_).str());

        int output_pipe[2] = {-1, -1};
        if (pipe(output_pipe) != 0) {
            perror("pipe");
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(output_pipe[0]);
            close(output_pipe[1]);
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        if (pid == 0) {
            if (!ConfigureManagedChildProcessGroup()) {
                _exit(126);
            }
            close(output_pipe[0]);
            dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            close(output_pipe[1]);
            execl("/bin/bash", "bash", "-lc", command_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(output_pipe[1]);
        if (setpgid(pid, pid) != 0 && errno != EACCES) {
            std::cerr << "[camera_recorder] failed to assign recorder process group: "
                      << config_.name << " error=" << std::strerror(errno) << std::endl;
        }
        pid_ = pid;
        started_ = true;
        running_ = true;
        StartOutputReaderThread(output_pipe[0]);

        Poll();
        return running_;
    }

    void Poll() override {
        if (!running_ || pid_ <= 0) {
            return;
        }

        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0 || result == -1) {
            return;
        }

        running_ = false;
        exit_code_ = DecodeExitCode(status);
        const bool unexpected_exit = !stop_requested_;
        if (unexpected_exit) {
            failure_ = true;
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] recorder exited early: " << config_.name
                                  << " exit_code=" << *exit_code_).str());
        } else {
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder] recorder stopped: " << config_.name
                                 << " exit_code=" << *exit_code_).str());
        }
    }

    void Stop() override {
        if (!running_ || pid_ <= 0) {
            JoinOutputReaderThread();
            return;
        }

        stop_requested_ = true;
        kill(-pid_, SIGINT);

        const auto soft_deadline = std::chrono::steady_clock::now() + kRecorderStopSigintTimeout;
        while (std::chrono::steady_clock::now() < soft_deadline) {
            Poll();
            if (!running_) {
                JoinOutputReaderThread();
                return;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }

        DM_LOG_WARN("{}", (::DA::utils::LogString() << "[camera_recorder] stop timeout after SIGINT, escalating to SIGTERM: "
                             << config_.name).str());
        kill(-pid_, SIGTERM);
        const auto hard_deadline = std::chrono::steady_clock::now() + kRecorderStopSigtermTimeout;
        while (std::chrono::steady_clock::now() < hard_deadline) {
            Poll();
            if (!running_) {
                JoinOutputReaderThread();
                return;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }

        DM_LOG_WARN("{}", (::DA::utils::LogString() << "[camera_recorder] stop timeout after SIGTERM, escalating to SIGKILL: "
                             << config_.name).str());
        kill(-pid_, SIGKILL);
        const auto reap_deadline = std::chrono::steady_clock::now() + kRecorderStopReapTimeout;
        while (std::chrono::steady_clock::now() < reap_deadline) {
            Poll();
            if (!running_) {
                break;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }
        if (running_) {
            std::cerr << "[camera_recorder] recorder still not reaped after SIGKILL: "
                      << config_.name << std::endl;
        }
        JoinOutputReaderThread();
    }

    bool IsRunning() const override {
        return running_;
    }

    bool HasStarted() const override {
        return started_;
    }

    bool HasFailure() const override {
        return failure_;
    }

    std::optional<int> ExitCode() const override {
        return exit_code_;
    }

    bool HasWrittenOutput() const override {
        const auto output_path = PrimaryOutputPath();
        if (!output_path.has_value()) {
            return false;
        }
        std::error_code error;
        return fs::exists(*output_path, error) && fs::file_size(*output_path, error) > 0;
    }

    std::optional<int64_t> RecordTimeOffsetUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return record_time_offset_us_;
    }

    std::optional<int64_t> FirstFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return first_frame_pts_us_;
    }

    std::optional<int64_t> FirstFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return first_frame_system_time_us_;
    }

    std::optional<int64_t> LastFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return last_frame_pts_us_;
    }

    std::optional<int64_t> LastFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return last_frame_system_time_us_;
    }

    uint64_t ObservedFrameCount() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return observed_output_frame_count_;
    }

protected:
    virtual bool BeforeStart() {
        return true;
    }

    virtual std::string BuildCommand() const = 0;

    void UpdateFrameTiming(int64_t frame_pts_us) {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        const int64_t system_time_us = CurrentSystemTimeUs();
        ++observed_output_frame_count_;
        if (record_time_offset_us_.has_value()) {
            last_frame_pts_us_ = frame_pts_us;
            last_frame_system_time_us_ = system_time_us;
            return;
        }

        first_frame_pts_us_ = frame_pts_us;
        first_frame_system_time_us_ = system_time_us;
        record_time_offset_us_ = system_time_us - frame_pts_us;
        last_frame_pts_us_ = frame_pts_us;
        last_frame_system_time_us_ = system_time_us;
    }

    void HandleOutputLine(const std::string& raw_line) {
        const std::string line = Trim(raw_line);
        if (line.empty()) {
            return;
        }

        if (const auto pts_us = ParseFramePtsUsFromStatsLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromDebugTsLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromGstIdentityLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromProgressLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us);
            return;
        }

        if (!PerfLogEnabled() && IsNoisyRecorderLogLine(line)) {
            return;
        }
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[" << config_.name << "] " << line).str());
    }

    void StartOutputReaderThread(int read_fd) {
        output_reader_thread_ = std::thread([this, read_fd]() {
            std::unique_ptr<FILE, decltype(&fclose)> stream(fdopen(read_fd, "r"), fclose);
            if (!stream) {
                close(read_fd);
                return;
            }

            char* line = nullptr;
            size_t line_capacity = 0;
            while (true) {
                const ssize_t line_size = getline(&line, &line_capacity, stream.get());
                if (line_size < 0) {
                    break;
                }
                HandleOutputLine(std::string(line, static_cast<size_t>(line_size)));
            }
            free(line);
        });
    }

    void JoinOutputReaderThread() {
        if (output_reader_thread_.joinable()) {
            output_reader_thread_.join();
        }
        LogFinalStatsOnce();
    }

    static int DecodeExitCode(int status) {
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return status;
    }

    std::optional<fs::path> PrimaryOutputPath() const {
        if (config_.output_files.empty()) {
            return std::nullopt;
        }
        return options_.output_dir / config_.output_files.front();
    }

    void LogFinalStatsOnce() {
        if (final_stats_logged_) {
            return;
        }
        final_stats_logged_ = true;

        std::optional<int64_t> first_pts_us;
        std::optional<int64_t> first_system_time_us;
        std::optional<int64_t> last_pts_us;
        std::optional<int64_t> last_system_time_us;
        std::optional<int64_t> offset_us;
        uint64_t observed_output_frames = 0;
        {
            std::lock_guard<std::mutex> lock(first_frame_mutex_);
            first_pts_us = first_frame_pts_us_;
            first_system_time_us = first_frame_system_time_us_;
            last_pts_us = last_frame_pts_us_;
            last_system_time_us = last_frame_system_time_us_;
            offset_us = record_time_offset_us_;
            observed_output_frames = observed_output_frame_count_;
        }

        std::error_code error;
        const auto output_path = PrimaryOutputPath();
        const bool has_output = output_path.has_value() && fs::exists(*output_path, error) &&
                                fs::file_size(*output_path, error) > 0;
        const auto output_size = has_output ? fs::file_size(*output_path, error) : 0;
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder][summary] name=" << config_.name
                             << " mode=" << ugripper::camera::ModeName(config_.mode)
                             << " observed_output_frames=" << observed_output_frames
                             << " first_pts_us=" << first_pts_us.value_or(-1)
                             << " last_pts_us=" << last_pts_us.value_or(-1)
                             << " first_system_time_us=" << first_system_time_us.value_or(-1)
                             << " last_system_time_us=" << last_system_time_us.value_or(-1)
                             << " record_time_offset_us=" << offset_us.value_or(-1)
                             << " has_output=" << (has_output ? "true" : "false")
                             << " output_size=" << output_size
                             << " exit_code=" << exit_code_.value_or(-1)).str());
    }

    CameraConfig config_;
    Options options_;
    std::string command_;
    pid_t pid_ = -1;
    bool started_ = false;
    bool running_ = false;
    bool stop_requested_ = false;
    bool failure_ = false;
    std::optional<int> exit_code_;
    std::thread output_reader_thread_;
    mutable std::mutex first_frame_mutex_;
    std::optional<int64_t> first_frame_pts_us_;
    std::optional<int64_t> first_frame_system_time_us_;
    std::optional<int64_t> last_frame_pts_us_;
    std::optional<int64_t> last_frame_system_time_us_;
    std::optional<int64_t> record_time_offset_us_;
    uint64_t observed_output_frame_count_ = 0;
    bool final_stats_logged_ = false;
};

#if CAMERA_RECORDER_HAS_GSTREAMER
class MainCameraRecorder final : public CameraRecorder {
public:
    MainCameraRecorder(CameraConfig config, Options options)
        : config_(std::move(config)),
          options_(std::move(options)),
          command_description_("internal-v4l2-appsrc-copy") {}

    ~MainCameraRecorder() override {
        Stop();
    }

    const CameraConfig& config() const override {
        return config_;
    }

    const std::string& command() const override {
        return command_description_;
    }

    bool Start() override {
        if (started_) {
            return running_;
        }
        std::string error_message;
        if (!OpenDevice(&error_message) || !CreatePipeline(&error_message)) {
            failure_ = true;
            exit_code_ = -1;
            last_error_ = error_message;
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] failed to start " << config_.name
                                  << ": " << error_message).str());
            WriteCameraRecorderFault(options_.output_dir, config_, error_message);
            CleanupPipeline();
            CleanupDevice();
            return false;
        }

        started_ = true;
        running_ = true;
        started_system_time_us_ = CurrentSystemTimeUs();
        stop_requested_.store(false, std::memory_order_release);
        capture_thread_done_.store(false, std::memory_order_release);
        capture_thread_ = std::thread([this]() { CaptureLoop(); });
        return true;
    }

    void Poll() override {
        if (!started_) {
            return;
        }
        HandleBusMessages();
    }

    void Stop() override {
        const auto stop_start = std::chrono::steady_clock::now();
        stop_requested_.store(true, std::memory_order_release);
        if (!short_stream_fault_checked_) {
            short_stream_fault_checked_ = true;
            MaybePublishShortStreamFault("stop_begin");
        }
        DM_LOG_INFO("{}", (::DA::utils::LogString()
                           << "[camera_recorder][main_stop_diag] category=stop_begin"
                           << " name=" << config_.name
                           << " capture_joinable=" << (capture_thread_.joinable() ? "true" : "false")
                           << " capture_done=" << (capture_thread_done_.load(std::memory_order_acquire) ? "true" : "false")
                           << " appsrc=" << (appsrc_ != nullptr ? "true" : "false")
                           << " pipeline=" << (pipeline_ != nullptr ? "true" : "false")).str());
        if (capture_thread_.joinable()) {
            RequestAppsrcEosForStop();
            if (!WaitForCaptureThreadDone(kMainCameraStopEosGrace)) {
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                                   << "[camera_recorder][main_stop_diag] category=eos_grace_timeout"
                                   << " name=" << config_.name
                                   << " grace_ms=" << kMainCameraStopEosGrace.count()
                                   << " capture_done=false").str());
                RequestPipelineFlushForStop();
            }
            capture_thread_.join();
            const auto join_elapsed = std::chrono::steady_clock::now() - stop_start;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                               << "[camera_recorder][main_stop_diag] category=capture_joined"
                               << " name=" << config_.name
                               << " stop_elapsed_ms="
                               << std::chrono::duration_cast<std::chrono::milliseconds>(join_elapsed).count()
                               << " capture_done=" << (capture_thread_done_.load(std::memory_order_acquire) ? "true" : "false")).str());
        } else if (appsrc_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        }
        if (bus_ != nullptr) {
            const auto bus_wait_start = std::chrono::steady_clock::now();
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus_,
                static_cast<GstClockTime>(kRecorderStopSigintTimeout.count()) * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (message != nullptr) {
                HandleBusMessage(message);
                gst_message_unref(message);
            } else {
                const auto bus_wait_elapsed = std::chrono::steady_clock::now() - bus_wait_start;
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                                   << "[camera_recorder][main_stop_diag] category=bus_wait_timeout"
                                   << " name=" << config_.name
                                   << " wait_elapsed_ms="
                                   << std::chrono::duration_cast<std::chrono::milliseconds>(bus_wait_elapsed).count()).str());
            }
        }

        CleanupPipeline();
        CleanupDevice();
        running_ = false;
        const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
        DM_LOG_INFO("{}", (::DA::utils::LogString()
                           << "[camera_recorder][main_stop_diag] category=stop_end"
                           << " name=" << config_.name
                           << " stop_elapsed_ms="
                           << std::chrono::duration_cast<std::chrono::milliseconds>(stop_elapsed).count()
                           << " failure=" << (failure_ ? "true" : "false")
                           << " exit_code=" << exit_code_.value_or(-1)).str());
    }

    bool IsRunning() const override {
        return running_;
    }

    bool HasStarted() const override {
        return started_;
    }

    bool HasFailure() const override {
        return failure_;
    }

    std::optional<int> ExitCode() const override {
        return exit_code_;
    }

    bool HasWrittenOutput() const override {
        std::error_code error;
        return fs::exists(output_path_, error) &&
               fs::file_size(output_path_, error) > 0;
    }

    std::optional<int64_t> RecordTimeOffsetUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return record_time_offset_us_;
    }

    std::optional<int64_t> FirstFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return first_frame_pts_us_;
    }

    std::optional<int64_t> FirstFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return first_frame_system_time_us_;
    }

    std::optional<int64_t> LastFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return last_frame_pts_us_;
    }

    std::optional<int64_t> LastFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return last_frame_system_time_us_;
    }

    uint64_t ObservedFrameCount() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return frame_count_;
    }

private:
    struct MmapBuffer {
        void* data = nullptr;
        size_t length = 0;
    };

    static bool RetryIoctl(int fd, unsigned long request, void* arg) {
        while (true) {
            const int rc = ioctl(fd, request, arg);
            if (rc == 0) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
    }

    uint32_t CapturePixelFormat() const {
        return options_.codec == "h264" ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
    }

    std::string GstCapsName() const {
        return options_.codec == "h264" ? "video/x-h264" : "video/x-h265";
    }

    std::string GstParserName() const {
        return options_.codec == "h264" ? "h264parse" : "h265parse";
    }

    bool OpenDevice(std::string* error_message) {
        output_path_ = options_.output_dir / config_.output_files.at(0);
        if (!WaitForDeviceNode(config_.device, kDeviceRebindTimeout)) {
            if (error_message != nullptr) {
                *error_message = "device node did not appear before open: " + config_.device;
            }
            return false;
        }
        fd_ = open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to open device " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }

        v4l2_capability capability{};
        if (!RetryIoctl(fd_, VIDIOC_QUERYCAP, &capability)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_QUERYCAP failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
            (capability.capabilities & V4L2_CAP_STREAMING) == 0) {
            if (error_message != nullptr) {
                *error_message = "device lacks capture/streaming capability: " + config_.device;
            }
            return false;
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<uint32_t>(config_.width);
        format.fmt.pix.height = static_cast<uint32_t>(config_.height);
        format.fmt.pix.pixelformat = CapturePixelFormat();
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (!RetryIoctl(fd_, VIDIOC_S_FMT, &format)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_S_FMT failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        if (format.fmt.pix.pixelformat != CapturePixelFormat()) {
            if (error_message != nullptr) {
                *error_message = "device did not accept requested compressed format: " + config_.device;
            }
            return false;
        }

        v4l2_streamparm streamparm{};
        streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(config_.fps);
        RetryIoctl(fd_, VIDIOC_S_PARM, &streamparm);

        v4l2_requestbuffers request{};
        // Keep this modest: each MMAP buffer can consume contiguous DMA/CMA
        // memory on RK3588, and repeated multi-camera start/stop can fragment it.
        request.count = static_cast<uint32_t>(std::max(2, config_.v4l2_buffer_count));
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (!RetryIoctl(fd_, VIDIOC_REQBUFS, &request) || request.count < 2) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_REQBUFS failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }

        buffers_.assign(request.count, {});
        for (uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (!RetryIoctl(fd_, VIDIOC_QUERYBUF, &buffer)) {
                if (error_message != nullptr) {
                    *error_message = "VIDIOC_QUERYBUF failed for " + config_.device + ": " + std::strerror(errno);
                }
                return false;
            }

            void* mapped = mmap(nullptr,
                                buffer.length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED,
                                fd_,
                                static_cast<off_t>(buffer.m.offset));
            if (mapped == MAP_FAILED) {
                if (error_message != nullptr) {
                    *error_message = "mmap failed for " + config_.device + ": " + std::strerror(errno);
                }
                return false;
            }
            buffers_[index].data = mapped;
            buffers_[index].length = buffer.length;

            if (!RetryIoctl(fd_, VIDIOC_QBUF, &buffer)) {
                if (error_message != nullptr) {
                    *error_message = "VIDIOC_QBUF failed for " + config_.device + ": " + std::strerror(errno);
                }
                return false;
            }
        }
        return true;
    }

    bool CreatePipeline(std::string* error_message) {
        if (!EnsureGstreamerInitialized(error_message)) {
            return false;
        }

        pipeline_ = gst_pipeline_new((config_.name + "_pipeline").c_str());
        GstElement* queue = gst_element_factory_make("queue", (config_.name + "_queue").c_str());
        GstElement* parser = gst_element_factory_make(GstParserName().c_str(), (config_.name + "_parser").c_str());
        GstElement* mux = gst_element_factory_make("matroskamux", (config_.name + "_mux").c_str());
        GstElement* sink = gst_element_factory_make("filesink", (config_.name + "_sink").c_str());
        appsrc_ = gst_element_factory_make("appsrc", (config_.name + "_src").c_str());

        if (pipeline_ == nullptr || appsrc_ == nullptr || queue == nullptr || parser == nullptr ||
            mux == nullptr || sink == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to create one or more main camera gstreamer elements";
            }
            return false;
        }

        g_object_set(G_OBJECT(appsrc_),
                     "is-live", TRUE,
                     "format", GST_FORMAT_TIME,
                     "do-timestamp", FALSE,
                     "block", TRUE,
                     "stream-type", GST_APP_STREAM_TYPE_STREAM,
                     nullptr);
        g_object_set(G_OBJECT(queue),
                     "leaky", 0,
                     "max-size-buffers", 0,
                     "max-size-bytes", 0,
                     "max-size-time", kMainCameraLeakyQueueMaxTimeNs,
                     nullptr);
        g_object_set(G_OBJECT(parser),
                     "config-interval", -1,
                     "disable-passthrough", TRUE,
                     nullptr);
        g_object_set(G_OBJECT(mux), "timecodescale", 1000LL, nullptr);
        g_object_set(G_OBJECT(sink), "location", output_path_.c_str(), nullptr);
        queue_ = queue;

        gst_bin_add_many(GST_BIN(pipeline_), appsrc_, queue, parser, mux, sink, nullptr);
        if (!gst_element_link_many(appsrc_, queue, parser, mux, sink, nullptr)) {
            if (error_message != nullptr) {
                *error_message = "failed to link main camera gstreamer elements";
            }
            return false;
        }

        GstCaps* caps = gst_caps_new_simple(
            GstCapsName().c_str(),
            "width", G_TYPE_INT, config_.width,
            "height", G_TYPE_INT, config_.height,
            "framerate", GST_TYPE_FRACTION, config_.fps, 1,
            "stream-format", G_TYPE_STRING, "byte-stream",
            nullptr);
        gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
        gst_caps_unref(caps);
        gst_app_src_set_stream_type(GST_APP_SRC(appsrc_), GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_max_bytes(GST_APP_SRC(appsrc_), 0);

        bus_ = gst_element_get_bus(pipeline_);
        const GstStateChangeReturn state_result = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (state_result == GST_STATE_CHANGE_FAILURE) {
            HandleBusMessages();
            if (error_message != nullptr) {
                *error_message = last_error_.empty() ? "failed to set main camera pipeline to PLAYING" : last_error_;
            }
            return false;
        }

        v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (!RetryIoctl(fd_, VIDIOC_STREAMON, &buffer_type)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_STREAMON failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        streaming_ = true;
        return true;
    }

    void CaptureLoop() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            pollfd poll_fd{};
            poll_fd.fd = fd_;
            poll_fd.events = POLLIN | POLLPRI;
            const int poll_rc = poll(&poll_fd, 1, 200);
            if (poll_rc == 0) {
                HandleBusMessages();
                continue;
            }
            if (poll_rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                SetFailure("main camera poll failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
            if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                SetFailure("main camera device poll error for " + config_.name);
                break;
            }

            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (!RetryIoctl(fd_, VIDIOC_DQBUF, &buffer)) {
                if (errno == EAGAIN) {
                    continue;
                }
                SetFailure("VIDIOC_DQBUF failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
            if (buffer.index >= buffers_.size()) {
                SetFailure("VIDIOC_DQBUF returned invalid index for " + config_.name);
                break;
            }

            const int64_t system_time_us = V4l2BufferSystemTimeUs(buffer).value_or(CurrentSystemTimeUs());
            const int64_t pts_us = UpdateFrameTimingFromCapture(system_time_us);

            GstBuffer* gst_buffer = gst_buffer_new_allocate(nullptr, buffer.bytesused, nullptr);
            if (gst_buffer == nullptr) {
                SetFailure("gst_buffer_new_allocate failed for " + config_.name);
                RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
                break;
            }

            GstMapInfo map_info{};
            if (!gst_buffer_map(gst_buffer, &map_info, GST_MAP_WRITE)) {
                gst_buffer_unref(gst_buffer);
                SetFailure("gst_buffer_map failed for " + config_.name);
                RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
                break;
            }
            std::memcpy(map_info.data, buffers_[buffer.index].data, buffer.bytesused);
            gst_buffer_unmap(gst_buffer, &map_info);

            GST_BUFFER_PTS(gst_buffer) = static_cast<GstClockTime>(pts_us) * 1000ULL;
            GST_BUFFER_DTS(gst_buffer) = static_cast<GstClockTime>(pts_us) * 1000ULL;
            GST_BUFFER_DURATION(gst_buffer) = static_cast<GstClockTime>(NominalFrameDurationUs()) * 1000ULL;

            MaybeLogQueueBeforePush(pts_us, buffer.bytesused);
            const auto push_start = std::chrono::steady_clock::now();
            const GstFlowReturn push_result = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), gst_buffer);
            const auto push_elapsed = std::chrono::steady_clock::now() - push_start;
            if (push_elapsed >= kMainCameraPushWarnThreshold) {
                LogQueueDiag("push_slow", pts_us, buffer.bytesused, push_elapsed, true);
            }
            if (push_result != GST_FLOW_OK) {
                LogQueueDiag("push_failed", pts_us, buffer.bytesused, push_elapsed, true);
                RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
                if (stop_requested_.load(std::memory_order_acquire)) {
                    DM_LOG_INFO("{}", (::DA::utils::LogString()
                                       << "[camera_recorder][main_stop_diag] category=push_stopped"
                                       << " name=" << config_.name
                                       << " flow=" << gst_flow_get_name(push_result)
                                       << " flow_code=" << static_cast<int>(push_result)).str());
                    break;
                }
                SetFailure("gst_app_src_push_buffer failed for " + config_.name +
                           " flow=" + std::to_string(push_result));
                break;
            }

            if (!RetryIoctl(fd_, VIDIOC_QBUF, &buffer)) {
                SetFailure("VIDIOC_QBUF failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
        }

        running_ = false;
        exit_code_ = failure_ ? -1 : 0;
        capture_thread_done_.store(true, std::memory_order_release);
        DM_LOG_INFO("{}", (::DA::utils::LogString()
                           << "[camera_recorder][main_stop_diag] category=capture_loop_exit"
                           << " name=" << config_.name
                           << " stop_requested=" << (stop_requested_.load(std::memory_order_acquire) ? "true" : "false")
                           << " failure=" << (failure_ ? "true" : "false")
                           << " exit_code=" << exit_code_.value_or(-1)
                           << " frame_count=" << frame_count_
                           << " last_pts_us=" << LastFramePtsUs().value_or(-1)
                           << " last_system_time_us=" << LastFrameSystemTimeUs().value_or(-1)).str());
    }

    int64_t NominalFrameDurationUs() const {
        return std::max<int64_t>(1, static_cast<int64_t>(std::llround(
            static_cast<long double>(kUsPerSecond) /
            static_cast<long double>(std::max(1, config_.fps)))));
    }

    int64_t UpdateFrameTimingFromCapture(int64_t system_time_us) {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        if (!first_frame_system_time_us_.has_value()) {
            first_frame_system_time_us_ = system_time_us;
            first_frame_pts_us_ = 0;
            last_frame_system_time_us_ = system_time_us;
            last_frame_pts_us_ = 0;
            record_time_offset_us_ = system_time_us;
            frame_count_ = 1;
            return 0;
        }

        const int64_t next_pts_us = static_cast<int64_t>(frame_count_) * NominalFrameDurationUs();
        last_frame_system_time_us_ = system_time_us;
        last_frame_pts_us_ = next_pts_us;
        ++frame_count_;
        return next_pts_us;
    }

    void HandleBusMessages() {
        if (bus_ == nullptr) {
            return;
        }
        while (true) {
            GstMessage* message = gst_bus_pop_filtered(
                bus_,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
            if (message == nullptr) {
                break;
            }
            HandleBusMessage(message);
            gst_message_unref(message);
        }
    }

    void HandleBusMessage(GstMessage* message) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            std::string text = error != nullptr ? error->message : "unknown gstreamer error";
            if (debug != nullptr && *debug != '\0') {
                text += " debug=" + std::string(debug);
            }
            if (error != nullptr) {
                g_error_free(error);
            }
            if (debug != nullptr) {
                g_free(debug);
            }
            DM_LOG_ERROR("{}", (::DA::utils::LogString()
                                << "[camera_recorder][main_bus_diag] category=bus_error"
                                << " name=" << config_.name
                                << " source=" << GstMessageSourceName(message)
                                << " message=\"" << text << "\"").str());
            LogQueueDiag("bus_error", LastFramePtsUs().value_or(-1), 0, std::chrono::steady_clock::duration::zero(), true);
            SetFailure("main camera pipeline error for " + config_.name + ": " + text);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* warning = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(message, &warning, &debug);
            std::string text = warning != nullptr ? warning->message : "unknown";
            if (debug != nullptr && *debug != '\0') {
                text += " debug=" + std::string(debug);
            }
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                                << "[camera_recorder][main_bus_diag] category=bus_warning"
                                << " name=" << config_.name
                                << " source=" << GstMessageSourceName(message)
                                << " message=\"" << text << "\"").str());
            LogQueueDiag("bus_warning", LastFramePtsUs().value_or(-1), 0, std::chrono::steady_clock::duration::zero(), false);
            if (warning != nullptr) {
                g_error_free(warning);
            }
            if (debug != nullptr) {
                g_free(debug);
            }
            break;
        }
        case GST_MESSAGE_EOS:
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                               << "[camera_recorder][main_bus_diag] category=bus_eos"
                               << " name=" << config_.name
                               << " source=" << GstMessageSourceName(message)).str());
            break;
        default:
            break;
        }
    }

    struct QueueLevels {
        uint64_t buffers = 0;
        uint64_t bytes = 0;
        uint64_t time_ns = 0;
    };

    QueueLevels ReadQueueLevels() const {
        QueueLevels levels;
        if (queue_ == nullptr) {
            return levels;
        }
        levels.buffers = ReadGObjectUintProperty(G_OBJECT(queue_), "current-level-buffers").value_or(0);
        levels.bytes = ReadGObjectUintProperty(G_OBJECT(queue_), "current-level-bytes").value_or(0);
        levels.time_ns = ReadGObjectUintProperty(G_OBJECT(queue_), "current-level-time").value_or(0);
        return levels;
    }

    void MaybeLogQueueBeforePush(int64_t pts_us, uint32_t bytesused) {
        const QueueLevels levels = ReadQueueLevels();
        if (levels.time_ns < kMainCameraQueueInfoThresholdNs) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_queue_diag_log_ < kMainCameraQueueDiagInterval) {
            return;
        }
        last_queue_diag_log_ = now;
        LogQueueDiag("pre_push_queue_high",
                     pts_us,
                     bytesused,
                     std::chrono::steady_clock::duration::zero(),
                     levels.time_ns >= kMainCameraQueueWarnThresholdNs);
    }

    void LogQueueDiag(const char* reason,
                      int64_t pts_us,
                      uint32_t bytesused,
                      std::chrono::steady_clock::duration push_elapsed,
                      bool warn) const {
        const QueueLevels levels = ReadQueueLevels();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(push_elapsed).count();
        auto stream = ::DA::utils::LogString();
        stream << "[camera_recorder][main_queue_diag]"
               << " category=" << reason
               << " name=" << config_.name
               << " pts_us=" << pts_us
               << " frame_bytes=" << bytesused
               << " push_elapsed_ms=" << elapsed_ms
               << " queue_buffers=" << levels.buffers
               << " queue_bytes=" << levels.bytes
               << " queue_time_ns=" << levels.time_ns
               << " queue_time_ms=" << (levels.time_ns / 1000000ULL)
               << " queue_limit_time_ns=" << kMainCameraLeakyQueueMaxTimeNs;
        if (warn) {
            DM_LOG_WARN("{}", stream.str());
        } else {
            DM_LOG_INFO("{}", stream.str());
        }
    }

    bool WaitForCaptureThreadDone(std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!capture_thread_done_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(kMainCameraStopPollInterval);
        }
        return true;
    }

    void MaybePublishShortStreamFault(const char* phase) const {
        if (started_system_time_us_ <= 0) {
            return;
        }

        std::optional<int64_t> first_pts_us;
        std::optional<int64_t> last_pts_us;
        std::optional<int64_t> first_system_time_us;
        std::optional<int64_t> last_system_time_us;
        uint64_t frame_count = 0;
        {
            std::lock_guard<std::mutex> lock(timing_mutex_);
            first_pts_us = first_frame_pts_us_;
            last_pts_us = last_frame_pts_us_;
            first_system_time_us = first_frame_system_time_us_;
            last_system_time_us = last_frame_system_time_us_;
            frame_count = frame_count_;
        }

        const int64_t stop_system_time_us = CurrentSystemTimeUs();
        const int64_t session_us =
            std::max<int64_t>(0, stop_system_time_us - started_system_time_us_);
        const int64_t span_us = FrameSpanUs(first_pts_us, last_pts_us);
        if (!IsMainCameraShortStreamFault(session_us, frame_count, span_us, config_.fps)) {
            if (session_us < kMainCameraShortStreamMinSessionUs) {
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                                   << "[camera_recorder] skip main camera short stream check for short session"
                                   << " camera=" << config_.name
                                   << " session_us=" << session_us
                                   << " min_session_us=" << kMainCameraShortStreamMinSessionUs).str());
            }
            return;
        }

        std::ostringstream error;
        error << "main camera short stream"
              << " camera=" << config_.name
              << " device=" << config_.device
              << " frame_count=" << frame_count
              << " span_us=" << span_us
              << " session_us=" << session_us
              << " first_pts_us=" << first_pts_us.value_or(-1)
              << " last_pts_us=" << last_pts_us.value_or(-1);

        json extra = json::object();
        extra["phase"] = phase == nullptr ? "unknown" : phase;
        extra["frame_count"] = frame_count;
        extra["span_us"] = span_us;
        extra["session_us"] = session_us;
        extra["started_system_time_us"] = started_system_time_us_;
        extra["stop_system_time_us"] = stop_system_time_us;
        extra["first_pts_us"] = first_pts_us.value_or(-1);
        extra["last_pts_us"] = last_pts_us.value_or(-1);
        extra["first_system_time_us"] = first_system_time_us.value_or(-1);
        extra["last_system_time_us"] = last_system_time_us.value_or(-1);
        WriteCameraRecorderFaultJson(options_.output_dir,
                                     config_,
                                     "main_camera_short_stream",
                                     error.str(),
                                     extra);
    }

    void RequestAppsrcEosForStop() {
        if (appsrc_ == nullptr) {
            return;
        }
        const GstFlowReturn eos_result = gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        DM_LOG_INFO("{}", (::DA::utils::LogString()
                           << "[camera_recorder][main_stop_diag] category=pre_join_eos"
                           << " name=" << config_.name
                           << " flow=" << gst_flow_get_name(eos_result)
                           << " flow_code=" << static_cast<int>(eos_result)
                           << " capture_done=" << (capture_thread_done_.load(std::memory_order_acquire) ? "true" : "false")).str());
        LogQueueDiag("pre_join_eos",
                     LastFramePtsUs().value_or(-1),
                     0,
                     std::chrono::steady_clock::duration::zero(),
                     false);
    }

    void RequestPipelineFlushForStop() {
        if (pipeline_ == nullptr) {
            return;
        }
        const gboolean flush_start_sent = gst_element_send_event(pipeline_, gst_event_new_flush_start());
        const gboolean flush_stop_sent = gst_element_send_event(pipeline_, gst_event_new_flush_stop(FALSE));
        DM_LOG_WARN("{}", (::DA::utils::LogString()
                           << "[camera_recorder][main_stop_diag] category=pre_join_flush"
                           << " name=" << config_.name
                           << " flush_start_sent=" << (flush_start_sent ? "true" : "false")
                           << " flush_stop_sent=" << (flush_stop_sent ? "true" : "false")
                           << " capture_done=" << (capture_thread_done_.load(std::memory_order_acquire) ? "true" : "false")).str());
        LogQueueDiag("pre_join_flush",
                     LastFramePtsUs().value_or(-1),
                     0,
                     std::chrono::steady_clock::duration::zero(),
                     true);
    }

    void CleanupPipeline() {
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (bus_ != nullptr) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        appsrc_ = nullptr;
        queue_ = nullptr;
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    void CleanupDevice() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            RetryIoctl(fd_, VIDIOC_STREAMOFF, &buffer_type);
            streaming_ = false;
        }
        for (auto& buffer : buffers_) {
            if (buffer.data != nullptr && buffer.length > 0) {
                munmap(buffer.data, buffer.length);
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void SetFailure(const std::string& error_message) {
        if (!failure_) {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] " << error_message).str());
        }
        failure_ = true;
        last_error_ = error_message;
        running_ = false;
        exit_code_ = -1;
    }

    CameraConfig config_;
    Options options_;
    std::string command_description_;
    fs::path output_path_;
    int fd_ = -1;
    bool streaming_ = false;
    bool started_ = false;
    bool running_ = false;
    bool failure_ = false;
    bool short_stream_fault_checked_ = false;
    std::optional<int> exit_code_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> capture_thread_done_{true};
    std::thread capture_thread_;
    std::vector<MmapBuffer> buffers_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstElement* queue_ = nullptr;
    GstBus* bus_ = nullptr;
    std::string last_error_;
    std::chrono::steady_clock::time_point last_queue_diag_log_{};
    int64_t started_system_time_us_ = 0;
    mutable std::mutex timing_mutex_;
    std::optional<int64_t> first_frame_pts_us_;
    std::optional<int64_t> first_frame_system_time_us_;
    std::optional<int64_t> last_frame_pts_us_;
    std::optional<int64_t> last_frame_system_time_us_;
    std::optional<int64_t> record_time_offset_us_;
    uint64_t frame_count_ = 0;
};
#else
class MainCameraRecorder final : public ShellCameraRecorder {
public:
    MainCameraRecorder(CameraConfig config, Options options)
        : config_(std::move(config)),
          options_(std::move(options)),
          command_description_("internal-v4l2-appsrc-copy") {}

    ~MainCameraRecorder() override {
        Stop();
    }

    const CameraConfig& config() const override {
        return config_;
    }

    const std::string& command() const override {
        return command_description_;
    }

    bool Start() override {
        if (started_) {
            return running_;
        }
        std::string error_message;
        if (!BeforeStart(&error_message) ||
            !OpenDevice(&error_message) ||
            !CreatePipeline(&error_message)) {
            failure_ = true;
            exit_code_ = -1;
            last_error_ = error_message;
            std::cerr << "[camera_recorder] failed to start " << config_.name
                      << ": " << error_message << std::endl;
            WriteCameraRecorderFault(options_.output_dir, config_, error_message);
            CleanupPipeline();
            CleanupDevice();
            return false;
        }

        started_ = true;
        running_ = true;
        started_system_time_us_ = CurrentSystemTimeUs();
        stop_requested_.store(false, std::memory_order_release);
        capture_thread_ = std::thread([this]() { CaptureLoop(); });
        return true;
    }

    void Poll() override {
        if (!started_) {
            return;
        }
        HandleBusMessages();
    }

    void Stop() override {
        stop_requested_.store(true, std::memory_order_release);
        if (!short_stream_fault_checked_) {
            short_stream_fault_checked_ = true;
            MaybePublishShortStreamFault("stop_begin");
        }
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }

        if (appsrc_ != nullptr) {
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        }
        if (bus_ != nullptr) {
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus_,
                static_cast<GstClockTime>(kRecorderStopSigintTimeout.count()) * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (message != nullptr) {
                HandleBusMessage(message);
                gst_message_unref(message);
            }
        }

        CleanupPipeline();
        CleanupDevice();
        running_ = false;
    }

    bool IsRunning() const override {
        return running_;
    }

    bool HasStarted() const override {
        return started_;
    }

    bool HasFailure() const override {
        return failure_;
    }

    std::optional<int> ExitCode() const override {
        return exit_code_;
    }

    bool HasWrittenOutput() const override {
        std::error_code error;
        return fs::exists(output_path_, error) &&
               fs::file_size(output_path_, error) > 0;
    }

    std::optional<int64_t> RecordTimeOffsetUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return record_time_offset_us_;
    }

    std::optional<int64_t> FirstFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return first_frame_pts_us_;
    }

    std::optional<int64_t> FirstFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return first_frame_system_time_us_;
    }

    std::optional<int64_t> LastFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return last_frame_pts_us_;
    }

    std::optional<int64_t> LastFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return last_frame_system_time_us_;
    }

    uint64_t ObservedFrameCount() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return frame_count_;
    }

private:
    std::string BuildCommand() const override {
        return ugripper::camera::BuildMainCameraCommand(
            config_,
            {.output_dir = options_.output_dir,
             .codec = options_.codec,
             .ffmpeg_bin = options_.ffmpeg_bin,
             .gst_bin = options_.gst_bin});
    }

    void MaybePublishShortStreamFault(const char* phase) const {
        if (started_system_time_us_ <= 0) {
            return;
        }

        std::optional<int64_t> first_pts_us;
        std::optional<int64_t> last_pts_us;
        std::optional<int64_t> first_system_time_us;
        std::optional<int64_t> last_system_time_us;
        uint64_t frame_count = 0;
        {
            std::lock_guard<std::mutex> lock(timing_mutex_);
            first_pts_us = first_frame_pts_us_;
            last_pts_us = last_frame_pts_us_;
            first_system_time_us = first_frame_system_time_us_;
            last_system_time_us = last_frame_system_time_us_;
            frame_count = frame_count_;
        }

        const int64_t stop_system_time_us = CurrentSystemTimeUs();
        const int64_t session_us =
            std::max<int64_t>(0, stop_system_time_us - started_system_time_us_);
        const int64_t span_us = FrameSpanUs(first_pts_us, last_pts_us);
        if (!IsMainCameraShortStreamFault(session_us, frame_count, span_us, config_.fps)) {
            if (session_us < kMainCameraShortStreamMinSessionUs) {
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                                   << "[camera_recorder] skip main camera short stream check for short session"
                                   << " camera=" << config_.name
                                   << " session_us=" << session_us
                                   << " min_session_us=" << kMainCameraShortStreamMinSessionUs).str());
            }
            return;
        }

        std::ostringstream error;
        error << "main camera short stream"
              << " camera=" << config_.name
              << " device=" << config_.device
              << " frame_count=" << frame_count
              << " span_us=" << span_us
              << " session_us=" << session_us
              << " first_pts_us=" << first_pts_us.value_or(-1)
              << " last_pts_us=" << last_pts_us.value_or(-1);

        json extra = json::object();
        extra["phase"] = phase == nullptr ? "unknown" : phase;
        extra["frame_count"] = frame_count;
        extra["span_us"] = span_us;
        extra["session_us"] = session_us;
        extra["started_system_time_us"] = started_system_time_us_;
        extra["stop_system_time_us"] = stop_system_time_us;
        extra["first_pts_us"] = first_pts_us.value_or(-1);
        extra["last_pts_us"] = last_pts_us.value_or(-1);
        extra["first_system_time_us"] = first_system_time_us.value_or(-1);
        extra["last_system_time_us"] = last_system_time_us.value_or(-1);
        WriteCameraRecorderFaultJson(options_.output_dir,
                                     config_,
                                     "main_camera_short_stream",
                                     error.str(),
                                     extra);
    }

    bool CreatePipeline(std::string* error_message) {
        if (!EnsureGstreamerInitialized(error_message)) {
            return false;
        }

        pipeline_ = gst_pipeline_new((config_.name + "_pipeline").c_str());
        GstElement* queue = gst_element_factory_make("queue", (config_.name + "_queue").c_str());
        GstElement* parser = gst_element_factory_make(GstParserName().c_str(), (config_.name + "_parser").c_str());
        GstElement* mux = gst_element_factory_make("matroskamux", (config_.name + "_mux").c_str());
        GstElement* sink = gst_element_factory_make("filesink", (config_.name + "_sink").c_str());
        appsrc_ = gst_element_factory_make("appsrc", (config_.name + "_src").c_str());

        if (pipeline_ == nullptr || appsrc_ == nullptr || queue == nullptr || parser == nullptr || mux == nullptr || sink == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to create one or more main camera gstreamer elements";
            }
            return false;
        }

        g_object_set(G_OBJECT(appsrc_),
                     "is-live", TRUE,
                     "format", GST_FORMAT_TIME,
                     "do-timestamp", FALSE,
                     "block", FALSE,
                     "stream-type", GST_APP_STREAM_TYPE_STREAM,
                     nullptr);
        g_object_set(G_OBJECT(queue),
                     "leaky", 2,
                     "max-size-buffers", 0,
                     "max-size-bytes", 0,
                     "max-size-time", kMainCameraLeakyQueueMaxTimeNs,
                     nullptr);
        g_object_set(G_OBJECT(parser),
                     "config-interval", -1,
                     "disable-passthrough", TRUE,
                     nullptr);
        g_object_set(G_OBJECT(mux), "timecodescale", 1000LL, nullptr);
        g_object_set(G_OBJECT(sink), "location", output_path_.c_str(), nullptr);

        gst_bin_add_many(GST_BIN(pipeline_), appsrc_, queue, parser, mux, sink, nullptr);
        if (!gst_element_link_many(appsrc_, queue, parser, mux, sink, nullptr)) {
            if (error_message != nullptr) {
                *error_message = "failed to link main camera gstreamer elements";
            }
            return false;
        }

        GstCaps* caps = gst_caps_new_simple(
            GstCapsName().c_str(),
            "width", G_TYPE_INT, config_.width,
            "height", G_TYPE_INT, config_.height,
            "framerate", GST_TYPE_FRACTION, config_.fps, 1,
            "stream-format", G_TYPE_STRING, "byte-stream",
            nullptr);
        gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
        gst_caps_unref(caps);
        gst_app_src_set_stream_type(GST_APP_SRC(appsrc_), GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_max_bytes(GST_APP_SRC(appsrc_), 0);

        bus_ = gst_element_get_bus(pipeline_);
        const GstStateChangeReturn state_result = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (state_result == GST_STATE_CHANGE_FAILURE) {
            HandleBusMessages();
            if (error_message != nullptr) {
                *error_message = last_error_.empty() ? "failed to set main camera pipeline to PLAYING" : last_error_;
            }
            return false;
        }

        v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (!RetryIoctl(fd_, VIDIOC_STREAMON, &buffer_type)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_STREAMON failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        streaming_ = true;
        return true;
    }

    void CaptureLoop() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            pollfd poll_fd{};
            poll_fd.fd = fd_;
            poll_fd.events = POLLIN | POLLPRI;
            const int poll_rc = poll(&poll_fd, 1, 200);
            if (poll_rc == 0) {
                HandleBusMessages();
                continue;
            }
            if (poll_rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                SetFailure("main camera poll failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
            if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                SetFailure("main camera device poll error for " + config_.name);
                break;
            }

            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (!RetryIoctl(fd_, VIDIOC_DQBUF, &buffer)) {
                if (errno == EAGAIN) {
                    continue;
                }
                SetFailure("VIDIOC_DQBUF failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
            if (buffer.index >= buffers_.size()) {
                SetFailure("VIDIOC_DQBUF returned invalid index for " + config_.name);
                break;
            }

            const int64_t system_time_us =
                V4l2BufferSystemTimeUs(buffer).value_or(CurrentSystemTimeUs());
            const int64_t pts_us = UpdateFrameTimingFromCapture(system_time_us);

            GstBuffer* gst_buffer = gst_buffer_new_allocate(nullptr, buffer.bytesused, nullptr);
            if (gst_buffer == nullptr) {
                SetFailure("gst_buffer_new_allocate failed for " + config_.name);
                RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
                break;
            }

            GstMapInfo map_info{};
            if (!gst_buffer_map(gst_buffer, &map_info, GST_MAP_WRITE)) {
                gst_buffer_unref(gst_buffer);
                SetFailure("gst_buffer_map failed for " + config_.name);
                RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
                break;
            }
            std::memcpy(map_info.data, buffers_[buffer.index].data, buffer.bytesused);
            gst_buffer_unmap(gst_buffer, &map_info);

            GST_BUFFER_PTS(gst_buffer) = static_cast<GstClockTime>(pts_us) * 1000ULL;
            GST_BUFFER_DTS(gst_buffer) = static_cast<GstClockTime>(pts_us) * 1000ULL;
            GST_BUFFER_DURATION(gst_buffer) = static_cast<GstClockTime>(NominalFrameDurationUs()) * 1000ULL;

            const GstFlowReturn push_result = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), gst_buffer);
            if (push_result != GST_FLOW_OK) {
                SetFailure("gst_app_src_push_buffer failed for " + config_.name +
                           " flow=" + std::to_string(push_result));
                RetryIoctl(fd_, VIDIOC_QBUF, &buffer);
                break;
            }

            if (!RetryIoctl(fd_, VIDIOC_QBUF, &buffer)) {
                SetFailure("VIDIOC_QBUF failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
        }

        running_ = false;
        exit_code_ = failure_ ? -1 : 0;
    }

    int64_t NominalFrameDurationUs() const {
        return std::max<int64_t>(1, static_cast<int64_t>(std::llround(
            static_cast<long double>(kUsPerSecond) / static_cast<long double>(std::max(1, config_.fps)))));
    }

    int64_t UpdateFrameTimingFromCapture(int64_t system_time_us) {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        if (!first_frame_system_time_us_.has_value()) {
            first_frame_system_time_us_ = system_time_us;
            first_frame_pts_us_ = 0;
            last_frame_system_time_us_ = system_time_us;
            last_frame_pts_us_ = 0;
            record_time_offset_us_ = system_time_us;
            frame_count_ = 1;
            return 0;
        }

        const int64_t next_pts_us = frame_count_ * NominalFrameDurationUs();
        last_frame_system_time_us_ = system_time_us;
        last_frame_pts_us_ = next_pts_us;
        ++frame_count_;
        return next_pts_us;
    }

    void HandleBusMessages() {
        if (bus_ == nullptr) {
            return;
        }
        while (true) {
            GstMessage* message = gst_bus_pop_filtered(
                bus_,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
            if (message == nullptr) {
                break;
            }
            HandleBusMessage(message);
            gst_message_unref(message);
        }
    }

    void HandleBusMessage(GstMessage* message) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            std::string text = error != nullptr ? error->message : "unknown gstreamer error";
            if (debug != nullptr && *debug != '\0') {
                text += " debug=" + std::string(debug);
            }
            if (error != nullptr) {
                g_error_free(error);
            }
            if (debug != nullptr) {
                g_free(debug);
            }
            SetFailure("main camera pipeline error for " + config_.name + ": " + text);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* warning = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(message, &warning, &debug);
            std::cerr << "[camera_recorder] warning from main camera pipeline "
                      << config_.name << ": "
                      << (warning != nullptr ? warning->message : "unknown") << std::endl;
            if (warning != nullptr) {
                g_error_free(warning);
            }
            if (debug != nullptr) {
                g_free(debug);
            }
            break;
        }
        case GST_MESSAGE_EOS:
            break;
        default:
            break;
        }
    }

    void CleanupPipeline() {
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (bus_ != nullptr) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        appsrc_ = nullptr;
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    void CleanupDevice() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            RetryIoctl(fd_, VIDIOC_STREAMOFF, &buffer_type);
            streaming_ = false;
        }
        for (auto& buffer : buffers_) {
            if (buffer.data != nullptr && buffer.length > 0) {
                munmap(buffer.data, buffer.length);
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void SetFailure(const std::string& error_message) {
        if (!failure_) {
            std::cerr << "[camera_recorder] " << error_message << std::endl;
        }
        failure_ = true;
        last_error_ = error_message;
        running_ = false;
        exit_code_ = -1;
    }

    CameraConfig config_;
    Options options_;
    std::string command_description_;
    fs::path output_path_;
    int fd_ = -1;
    bool streaming_ = false;
    bool started_ = false;
    bool running_ = false;
    bool failure_ = false;
    bool short_stream_fault_checked_ = false;
    std::optional<int> exit_code_;
    std::atomic<bool> stop_requested_{false};
    std::thread capture_thread_;
    std::vector<MmapBuffer> buffers_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstBus* bus_ = nullptr;
    std::string last_error_;
    int64_t started_system_time_us_ = 0;
    mutable std::mutex timing_mutex_;
    std::optional<int64_t> first_frame_pts_us_;
    std::optional<int64_t> first_frame_system_time_us_;
    std::optional<int64_t> last_frame_pts_us_;
    std::optional<int64_t> last_frame_system_time_us_;
    std::optional<int64_t> record_time_offset_us_;
    uint64_t frame_count_ = 0;
};
#endif

class HybridCameraRecorder final : public ShellCameraRecorder {
public:
    HybridCameraRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        return ugripper::camera::BuildHybridCameraCommand(
            config_,
            {.output_dir = options_.output_dir,
             .codec = options_.codec,
             .ffmpeg_bin = options_.ffmpeg_bin,
             .gst_bin = options_.gst_bin});
    }
};

class StereoHybridRecorder final : public ShellCameraRecorder {
public:
    StereoHybridRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        return ugripper::camera::BuildStereoHybridCameraCommand(
            config_,
            {.output_dir = options_.output_dir,
             .codec = options_.codec,
             .ffmpeg_bin = options_.ffmpeg_bin,
             .gst_bin = options_.gst_bin});
    }
};

std::unique_ptr<CameraRecorder> CreateRecorder(const CameraConfig& config, const Options& options) {
    switch (config.mode) {
    case CameraRecordMode::DirectCopyH265:
        return std::make_unique<MainCameraRecorder>(config, options);
    case CameraRecordMode::HybridDecodeEncode:
        return std::make_unique<HybridCameraRecorder>(config, options);
    case CameraRecordMode::StereoHybridDecodeEncode:
        return std::make_unique<StereoHybridRecorder>(config, options);
    }
    throw std::runtime_error("unknown camera record mode");
}

struct StereoCapturedFrame {
    std::vector<uint8_t> frame_bytes;
    int64_t system_time_us = 0;
    uint64_t sequence = 0;
};

class StereoWarmupCapture {
public:
    using FrameHandler = std::function<void(StereoCapturedFrame&& frame)>;

    StereoWarmupCapture(CameraConfig config, FrameHandler frame_handler)
        : config_(std::move(config)),
          frame_handler_(std::move(frame_handler)) {}

    ~StereoWarmupCapture() {
        Stop();
    }

    bool Start(std::string* error_message) {
        Stop();

        stop_requested_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        forward_frames_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            first_frame_system_time_us_.reset();
            last_frame_system_time_us_.reset();
            last_error_.clear();
        }
        frame_sequence_ = 0;

        if (!OpenCaptureSource(error_message)) {
            CleanupDevice();
            return false;
        }

        started_system_time_us_ = CurrentSystemTimeUs();
        running_.store(true, std::memory_order_release);
        capture_thread_ = std::thread([this]() { CaptureLoop(); });
        return true;
    }

    void Stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        running_.store(false, std::memory_order_release);
        CleanupDevice();
        forward_frames_.store(false, std::memory_order_relaxed);
    }

    bool IsRunning() const {
        return running_.load(std::memory_order_acquire);
    }

    void SetForwardFrames(bool enabled) {
        forward_frames_.store(enabled, std::memory_order_release);
    }

    std::optional<int64_t> FirstFrameSystemTimeUs() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return first_frame_system_time_us_;
    }

    std::optional<int64_t> LastFrameSystemTimeUs() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_frame_system_time_us_;
    }

    int64_t started_system_time_us() const {
        return started_system_time_us_;
    }

    std::string last_error() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_error_;
    }

private:
    struct MmapBuffer {
        void* data = nullptr;
        size_t length = 0;
    };

    bool OpenCaptureSource(std::string* error_message) {
        return OpenDevice(error_message);
    }

    static bool RetryIoctl(int fd, unsigned long request, void* arg) {
        while (true) {
            const int rc = ioctl(fd, request, arg);
            if (rc == 0) {
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
    }

    bool OpenDevice(std::string* error_message) {
        fd_ = open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to open stereo device " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }

        v4l2_capability capability{};
        if (!RetryIoctl(fd_, VIDIOC_QUERYCAP, &capability)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_QUERYCAP failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
            (capability.capabilities & V4L2_CAP_STREAMING) == 0) {
            if (error_message != nullptr) {
                *error_message = "stereo device lacks capture/streaming capability: " + config_.device;
            }
            return false;
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<uint32_t>(ugripper::camera::CaptureWidth(config_));
        format.fmt.pix.height = static_cast<uint32_t>(ugripper::camera::CaptureHeight(config_));
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (!RetryIoctl(fd_, VIDIOC_S_FMT, &format)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_S_FMT failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
            if (error_message != nullptr) {
                *error_message = "stereo device did not accept MJPEG format: " + config_.device;
            }
            return false;
        }

        v4l2_streamparm streamparm{};
        streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(config_.fps);
        RetryIoctl(fd_, VIDIOC_S_PARM, &streamparm);

        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (!RetryIoctl(fd_, VIDIOC_REQBUFS, &request) || request.count < 2) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_REQBUFS failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }

        buffers_.assign(request.count, {});
        for (uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (!RetryIoctl(fd_, VIDIOC_QUERYBUF, &buffer)) {
                if (error_message != nullptr) {
                    *error_message = "VIDIOC_QUERYBUF failed for " + config_.device + ": " + std::strerror(errno);
                }
                return false;
            }

            void* mapped = mmap(nullptr,
                                buffer.length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED,
                                fd_,
                                static_cast<off_t>(buffer.m.offset));
            if (mapped == MAP_FAILED) {
                if (error_message != nullptr) {
                    *error_message = "mmap failed for " + config_.device + ": " + std::strerror(errno);
                }
                return false;
            }
            buffers_[index].data = mapped;
            buffers_[index].length = buffer.length;

            if (!RetryIoctl(fd_, VIDIOC_QBUF, &buffer)) {
                if (error_message != nullptr) {
                    *error_message = "VIDIOC_QBUF failed for " + config_.device + ": " + std::strerror(errno);
                }
                return false;
            }
        }

        v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (!RetryIoctl(fd_, VIDIOC_STREAMON, &buffer_type)) {
            if (error_message != nullptr) {
                *error_message = "VIDIOC_STREAMON failed for " + config_.device + ": " + std::strerror(errno);
            }
            return false;
        }
        streaming_ = true;
        return true;
    }

    void CaptureLoop() {
        V4l2CaptureLoop();
    }

    void V4l2CaptureLoop() {
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            pollfd poll_fd{};
            poll_fd.fd = fd_;
            poll_fd.events = POLLIN | POLLPRI;
            const int poll_rc = poll(&poll_fd, 1, 200);
            if (poll_rc == 0) {
                continue;
            }
            if (poll_rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                SetFailure("stereo poll failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
            if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                SetFailure("stereo device poll error for " + config_.name);
                break;
            }

            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (!RetryIoctl(fd_, VIDIOC_DQBUF, &buffer)) {
                if (errno == EAGAIN) {
                    continue;
                }
                SetFailure("VIDIOC_DQBUF failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
            if (buffer.index >= buffers_.size()) {
                SetFailure("VIDIOC_DQBUF returned invalid index for " + config_.name);
                break;
            }

            const int64_t now_us = CurrentSystemTimeUs();
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!first_frame_system_time_us_.has_value()) {
                    first_frame_system_time_us_ = now_us;
                }
                last_frame_system_time_us_ = now_us;
            }

            if (forward_frames_.load(std::memory_order_acquire) &&
                frame_handler_ &&
                buffer.bytesused > 0) {
                StereoCapturedFrame frame;
                frame.system_time_us = now_us;
                frame.sequence = frame_sequence_++;
                frame.frame_bytes.resize(buffer.bytesused);
                std::memcpy(frame.frame_bytes.data(), buffers_[buffer.index].data, buffer.bytesused);
                frame_handler_(std::move(frame));
            }

            if (!RetryIoctl(fd_, VIDIOC_QBUF, &buffer)) {
                SetFailure("VIDIOC_QBUF failed for " + config_.name + ": " + std::strerror(errno));
                break;
            }
        }

        running_.store(false, std::memory_order_release);
    }

    void CleanupDevice() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            RetryIoctl(fd_, VIDIOC_STREAMOFF, &buffer_type);
            streaming_ = false;
        }
        for (auto& buffer : buffers_) {
            if (buffer.data != nullptr && buffer.length > 0) {
                munmap(buffer.data, buffer.length);
            }
        }
        buffers_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void SetFailure(const std::string& error_message) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = error_message;
        }
        running_.store(false, std::memory_order_release);
    }

    CameraConfig config_;
    FrameHandler frame_handler_;
    int fd_ = -1;
    bool streaming_ = false;
    std::vector<MmapBuffer> buffers_;
    std::thread capture_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> forward_frames_{false};
    mutable std::mutex state_mutex_;
    std::optional<int64_t> first_frame_system_time_us_;
    std::optional<int64_t> last_frame_system_time_us_;
    std::string last_error_;
    int64_t started_system_time_us_ = 0;
    uint64_t frame_sequence_ = 0;
};

class StereoSessionRecorder final : public CameraRecorder {
public:
    StereoSessionRecorder(CameraConfig config, Options options, fs::path output_path)
        : config_(std::move(config)),
          options_(std::move(options)),
          output_path_(std::move(output_path)),
          command_(BuildCommand()) {}

    ~StereoSessionRecorder() override {
        Stop();
    }

    const CameraConfig& config() const override {
        return config_;
    }

    const std::string& command() const override {
        return command_;
    }

    bool Start() override {
        if (started_) {
            return running_;
        }

        const fs::path parent = output_path_.parent_path();
        if (!parent.empty() && !EnsureDirectory(parent)) {
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder] starting stereo session " << config_.name).str());
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder] cmd: " << command_).str());

        int input_pipe[2] = {-1, -1};
        int output_pipe[2] = {-1, -1};
        if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
            if (input_pipe[0] >= 0) {
                close(input_pipe[0]);
                close(input_pipe[1]);
            }
            if (output_pipe[0] >= 0) {
                close(output_pipe[0]);
                close(output_pipe[1]);
            }
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(input_pipe[0]);
            close(input_pipe[1]);
            close(output_pipe[0]);
            close(output_pipe[1]);
            failure_ = true;
            exit_code_ = -1;
            return false;
        }

        if (pid == 0) {
            if (!ConfigureManagedChildProcessGroup()) {
                _exit(126);
            }
            dup2(input_pipe[0], STDIN_FILENO);
            dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            close(input_pipe[0]);
            close(input_pipe[1]);
            close(output_pipe[0]);
            close(output_pipe[1]);
            execl("/bin/bash", "bash", "-lc", command_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(input_pipe[0]);
        close(output_pipe[1]);
        if (setpgid(pid, pid) != 0 && errno != EACCES) {
            std::cerr << "[camera_recorder] failed to assign stereo session process group: "
                      << config_.name << " error=" << std::strerror(errno) << std::endl;
        }
        pid_ = pid;
        write_fd_ = input_pipe[1];
        started_ = true;
        running_ = true;
        StartOutputReaderThread(output_pipe[0]);
        writer_thread_ = std::thread([this]() { WriterLoop(); });
        Poll();
        return running_;
    }

    void Poll() override {
        if (!running_ || pid_ <= 0) {
            return;
        }

        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0 || result == -1) {
            return;
        }

        running_ = false;
        exit_code_ = DecodeExitCode(status);
        if (!stop_requested_) {
            failure_ = true;
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] stereo session exited early: " << config_.name
                                  << " exit_code=" << *exit_code_).str());
        } else {
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder] stereo session stopped: " << config_.name
                                 << " exit_code=" << *exit_code_).str());
        }
    }

    void Stop() override {
        if (!started_) {
            JoinThreads();
            return;
        }

        RequestStop();

        const auto eof_timeout =
            HasWrittenOutput() ? kStereoSessionEofGrace : kStereoSessionEofGraceNoOutput;
        const auto eof_deadline = std::chrono::steady_clock::now() + eof_timeout;
        while (running_ && std::chrono::steady_clock::now() < eof_deadline) {
            Poll();
            if (!running_) {
                break;
            }
            std::this_thread::sleep_for(kRecorderStopPollInterval);
        }

        if (running_ && pid_ > 0) {
            kill(-pid_, SIGINT);
            const auto sigint_timeout =
                HasWrittenOutput() ? kStereoSessionSigintTimeout : kStereoSessionSigintTimeoutNoOutput;
            const auto sigint_deadline = std::chrono::steady_clock::now() + sigint_timeout;
            while (running_ && std::chrono::steady_clock::now() < sigint_deadline) {
                Poll();
                if (!running_) {
                    break;
                }
                std::this_thread::sleep_for(kRecorderStopPollInterval);
            }
        }

        if (running_ && pid_ > 0) {
            kill(-pid_, SIGTERM);
            const auto sigterm_deadline = std::chrono::steady_clock::now() + kStereoSessionSigtermTimeout;
            while (running_ && std::chrono::steady_clock::now() < sigterm_deadline) {
                Poll();
                if (!running_) {
                    break;
                }
                std::this_thread::sleep_for(kRecorderStopPollInterval);
            }
        }

        if (running_ && pid_ > 0) {
            kill(-pid_, SIGKILL);
            Poll();
        }

        CloseWriteFd();
        JoinThreads();
    }

    bool IsRunning() const override {
        return running_;
    }

    bool HasStarted() const override {
        return started_;
    }

    bool HasFailure() const override {
        return failure_;
    }

    std::optional<int> ExitCode() const override {
        return exit_code_;
    }

    bool HasWrittenOutput() const override {
        std::error_code error;
        return fs::exists(output_path_, error) && fs::file_size(output_path_, error) > 0;
    }

    std::optional<int64_t> RecordTimeOffsetUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return record_time_offset_us_;
    }

    std::optional<int64_t> FirstFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return first_frame_pts_us_;
    }

    std::optional<int64_t> FirstFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return first_frame_system_time_us_;
    }

    std::optional<int64_t> LastFramePtsUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return last_frame_pts_us_;
    }

    std::optional<int64_t> LastFrameSystemTimeUs() const override {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        return last_frame_system_time_us_;
    }

    uint64_t ObservedFrameCount() const override {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        return submitted_frame_count_;
    }

    bool PushFrame(std::vector<uint8_t> frame_bytes, int64_t system_time_us) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (!running_ || stop_requested_ || failure_) {
            return false;
        }
        queue_cv_.wait(lock, [this]() {
            return frame_queue_.size() < 8 || writer_stop_requested_ || !running_;
        });
        if (!running_ || writer_stop_requested_ || failure_) {
            return false;
        }
        frame_queue_.push_back({std::move(frame_bytes), system_time_us});
        if (frame_queue_.size() > max_queue_backlog_) {
            max_queue_backlog_ = frame_queue_.size();
        }
        lock.unlock();
        queue_cv_.notify_all();
        return true;
    }

    std::optional<int64_t> FirstSubmittedFrameSystemTimeUs() const {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        return first_submitted_frame_system_time_us_;
    }

    std::optional<int64_t> LastSubmittedFrameSystemTimeUs() const {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        return last_submitted_frame_system_time_us_;
    }

    uint64_t SubmittedFrameCount() const {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        return submitted_frame_count_;
    }

    void RequestStop() {
        if (!started_ || stop_initiated_) {
            return;
        }

        stop_requested_ = true;
        stop_initiated_ = true;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            writer_stop_requested_ = true;
        }
        queue_cv_.notify_all();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }

private:
    struct QueuedFrame {
        std::vector<uint8_t> frame_bytes;
        int64_t system_time_us = 0;
    };

    std::string BuildCommand() const {
        return ugripper::camera::BuildStereoSessionCommand(
            config_,
            {.output_dir = options_.output_dir,
             .codec = options_.codec,
             .ffmpeg_bin = options_.ffmpeg_bin,
             .gst_bin = options_.gst_bin},
            output_path_);
    }

    void WriterLoop() {
        while (true) {
            QueuedFrame frame;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() {
                    return writer_stop_requested_ || !frame_queue_.empty();
                });
                if (frame_queue_.empty()) {
                    if (writer_stop_requested_) {
                        break;
                    }
                    continue;
                }
                frame = std::move(frame_queue_.front());
                frame_queue_.pop_front();
                queue_cv_.notify_all();
            }

            if (write_fd_ < 0) {
                failure_ = true;
                break;
            }
            if (!WriteAll(write_fd_,
                          reinterpret_cast<const uint8_t*>(frame.frame_bytes.data()),
                          frame.frame_bytes.size())) {
                if (!stop_requested_) {
                    failure_ = true;
                    DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] failed to write stereo frame to ffmpeg stdin: "
                                          << config_.name).str());
                }
                break;
            }

            std::lock_guard<std::mutex> lock(submitted_mutex_);
            if (!first_submitted_frame_system_time_us_.has_value()) {
                first_submitted_frame_system_time_us_ = frame.system_time_us;
            }
            last_submitted_frame_system_time_us_ = frame.system_time_us;
            ++submitted_frame_count_;
            submitted_frame_system_times_.push_back(frame.system_time_us);
        }

        CloseWriteFd();
        queue_cv_.notify_all();
    }

    void HandleOutputLine(const std::string& raw_line) {
        const std::string line = Trim(raw_line);
        if (line.empty()) {
            return;
        }

        if (const auto pts_us = ParseFramePtsUsFromStatsLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us, TakeSubmittedFrameSystemTimeUs());
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromProgressLine(line); pts_us.has_value()) {
            UpdateFrameTiming(*pts_us, TakeSubmittedFrameSystemTimeUs());
            return;
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[" << config_.name << "] " << line).str());
    }

    void StartOutputReaderThread(int read_fd) {
        output_reader_thread_ = std::thread([this, read_fd]() {
            std::unique_ptr<FILE, decltype(&fclose)> stream(fdopen(read_fd, "r"), fclose);
            if (!stream) {
                close(read_fd);
                return;
            }

            char* line = nullptr;
            size_t line_capacity = 0;
            while (true) {
                const ssize_t line_size = getline(&line, &line_capacity, stream.get());
                if (line_size < 0) {
                    break;
                }
                HandleOutputLine(std::string(line, static_cast<size_t>(line_size)));
            }
            free(line);
        });
    }

    void JoinThreads() {
        if (output_reader_thread_.joinable()) {
            output_reader_thread_.join();
        }
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
        LogFinalStatsOnce();
    }

    void CloseWriteFd() {
        if (write_fd_ >= 0) {
            close(write_fd_);
            write_fd_ = -1;
        }
    }

    std::optional<int64_t> TakeSubmittedFrameSystemTimeUs() {
        std::lock_guard<std::mutex> lock(submitted_mutex_);
        if (submitted_frame_system_times_.empty()) {
            return std::nullopt;
        }
        const int64_t system_time_us = submitted_frame_system_times_.front();
        submitted_frame_system_times_.pop_front();
        return system_time_us;
    }

    void UpdateFrameTiming(int64_t frame_pts_us, std::optional<int64_t> system_time_us) {
        const int64_t effective_system_time_us = system_time_us.value_or(CurrentSystemTimeUs());
        std::lock_guard<std::mutex> lock(timing_mutex_);
        if (!record_time_offset_us_.has_value()) {
            first_frame_pts_us_ = frame_pts_us;
            first_frame_system_time_us_ = effective_system_time_us;
            record_time_offset_us_ = effective_system_time_us - frame_pts_us;
        }
        last_frame_pts_us_ = frame_pts_us;
        last_frame_system_time_us_ = effective_system_time_us;
    }

    static int DecodeExitCode(int status) {
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return status;
    }

    void LogFinalStatsOnce() {
        if (final_stats_logged_) {
            return;
        }
        final_stats_logged_ = true;

        std::optional<int64_t> first_pts_us;
        std::optional<int64_t> first_system_time_us;
        std::optional<int64_t> last_pts_us;
        std::optional<int64_t> last_system_time_us;
        std::optional<int64_t> offset_us;
        {
            std::lock_guard<std::mutex> lock(timing_mutex_);
            first_pts_us = first_frame_pts_us_;
            first_system_time_us = first_frame_system_time_us_;
            last_pts_us = last_frame_pts_us_;
            last_system_time_us = last_frame_system_time_us_;
            offset_us = record_time_offset_us_;
        }

        uint64_t submitted_frame_count = 0;
        std::optional<int64_t> first_submitted_system_time_us;
        std::optional<int64_t> last_submitted_system_time_us;
        {
            std::lock_guard<std::mutex> lock(submitted_mutex_);
            submitted_frame_count = submitted_frame_count_;
            first_submitted_system_time_us = first_submitted_frame_system_time_us_;
            last_submitted_system_time_us = last_submitted_frame_system_time_us_;
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[camera_recorder][stereo_session_summary] name=" << config_.name
                             << " submitted_frames=" << submitted_frame_count
                             << " max_queue_backlog=" << max_queue_backlog_
                             << " first_submitted_system_time_us=" << first_submitted_system_time_us.value_or(-1)
                             << " last_submitted_system_time_us=" << last_submitted_system_time_us.value_or(-1)
                             << " first_pts_us=" << first_pts_us.value_or(-1)
                             << " last_pts_us=" << last_pts_us.value_or(-1)
                             << " first_system_time_us=" << first_system_time_us.value_or(-1)
                             << " last_system_time_us=" << last_system_time_us.value_or(-1)
                             << " record_time_offset_us=" << offset_us.value_or(-1)
                             << " has_output=" << (HasWrittenOutput() ? "true" : "false")
                             << " exit_code=" << exit_code_.value_or(-1)).str());
    }

    CameraConfig config_;
    Options options_;
    fs::path output_path_;
    std::string command_;
    pid_t pid_ = -1;
    int write_fd_ = -1;
    bool started_ = false;
    bool running_ = false;
    bool stop_requested_ = false;
    bool stop_initiated_ = false;
    bool writer_stop_requested_ = false;
    bool failure_ = false;
    std::optional<int> exit_code_;
    std::thread output_reader_thread_;
    std::thread writer_thread_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedFrame> frame_queue_;
    size_t max_queue_backlog_ = 0;
    mutable std::mutex submitted_mutex_;
    std::deque<int64_t> submitted_frame_system_times_;
    std::optional<int64_t> first_submitted_frame_system_time_us_;
    std::optional<int64_t> last_submitted_frame_system_time_us_;
    uint64_t submitted_frame_count_ = 0;
    mutable std::mutex timing_mutex_;
    std::optional<int64_t> first_frame_pts_us_;
    std::optional<int64_t> first_frame_system_time_us_;
    std::optional<int64_t> last_frame_pts_us_;
    std::optional<int64_t> last_frame_system_time_us_;
    std::optional<int64_t> record_time_offset_us_;
    bool final_stats_logged_ = false;
};

class StereoCameraTrack {
public:
    StereoCameraTrack(CameraConfig config, Options options)
        : config_(std::move(config)),
          options_(std::move(options)),
          frame_drop_modulo_(std::max(1, ugripper::camera::FrameDropModulo(config_))) {}

    void Stop() {
        if (warmup_capture_) {
            warmup_capture_->SetForwardFrames(false);
        }
        auto recorder = TakeSessionRecorder();
        if (recorder) {
            recorder->Stop();
        }
        if (warmup_capture_) {
            warmup_capture_->Stop();
            warmup_capture_.reset();
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_episode_dir_.clear();
        session_start_system_time_us_ = 0;
        stop_requested_by_control_ = false;
        session_failed_ = false;
        session_restart_attempts_ = 0;
        last_error_.clear();
    }

    void Poll(bool /*session_active*/) {
        if (warmup_capture_ && !warmup_capture_->IsRunning()) {
            warmup_capture_->SetForwardFrames(false);
            auto recorder = TakeSessionRecorder();
            if (recorder) {
                recorder->Stop();
                std::lock_guard<std::mutex> lock(state_mutex_);
                session_failed_ = true;
                last_error_ = warmup_capture_->last_error().empty()
                                  ? ("stereo warmup exited during active session for " + config_.name)
                                  : warmup_capture_->last_error();
            }
            warmup_capture_.reset();
        }

        std::string restart_episode_dir;
        bool should_restart = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (session_recorder_) {
                session_recorder_->Poll();
                const bool session_running = session_recorder_->IsRunning();
                const bool session_has_progress =
                    session_recorder_->FirstFrameSystemTimeUs().has_value() || session_recorder_->HasWrittenOutput();
                if (!session_running && !stop_requested_by_control_ && !session_failed_) {
                    if (!session_has_progress &&
                        session_restart_attempts_ <= kStereoSessionMaxRestartAttempts &&
                        ready()) {
                        DM_LOG_WARN("{}", (::DA::utils::LogString() << "[camera_recorder] stereo session recorder exited before first frame, restarting: "
                                             << config_.name
                                             << " attempt=" << (session_restart_attempts_ + 1)).str());
                        restart_episode_dir = session_episode_dir_;
                        should_restart = true;
                    } else {
                        session_failed_ = true;
                        last_error_ = "stereo session recorder exited early for " + config_.name;
                    }
                }
            }
        }
        if (should_restart) {
            TakeSessionRecorder();
            std::string restart_error;
            if (!StartSessionRecorder(restart_episode_dir, &restart_error)) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                session_failed_ = true;
                last_error_ = restart_error;
            }
        }

        if (!warmup_capture_ || !warmup_capture_->IsRunning()) {
            TryStartWarmupCapture();
        }
    }

    bool ready() const {
        if (!warmup_capture_ || !warmup_capture_->IsRunning()) {
            return false;
        }
        const auto first_frame_system_time_us = warmup_capture_->FirstFrameSystemTimeUs();
        const auto last_frame_system_time_us = warmup_capture_->LastFrameSystemTimeUs();
        if (first_frame_system_time_us.has_value() && last_frame_system_time_us.has_value()) {
            return (CurrentSystemTimeUs() - *last_frame_system_time_us) <= (2 * kUsPerSecond);
        }
        return (CurrentSystemTimeUs() - warmup_capture_->started_system_time_us()) >=
               (kStereoWarmupReadySettle.count() * 1000);
    }

    std::string state() const {
        if (SessionRecorderRunning()) {
            return ready() ? "recording" : "recording_recovering";
        }
        if (ready()) {
            return "ready";
        }
        if (warmup_capture_ && warmup_capture_->IsRunning()) {
            return "warming";
        }
        if (!LastError().empty()) {
            return "recovering";
        }
        return "not_ready";
    }

    const std::string& name() const {
        return config_.name;
    }

    ugripper::camera::StereoTrackStatus BuildStatus() const {
        ugripper::camera::StereoTrackStatus status;
        status.state = state();
        status.device = config_.device;
        status.ready = ready();
        if (warmup_capture_) {
            if (const auto first = warmup_capture_->FirstFrameSystemTimeUs(); first.has_value()) {
                status.first_frame_system_time_us = *first;
            }
            if (const auto last = warmup_capture_->LastFrameSystemTimeUs(); last.has_value()) {
                status.last_frame_system_time_us = *last;
            }
        }
        status.session_recording = SessionRecorderRunning();
        const std::string last_error = LastError();
        if (!last_error.empty()) {
            status.last_error = last_error;
        }
        return status;
    }

    bool StartSession(const std::string& episode_dir,
                      int64_t start_system_time_us,
                      std::string* error_message) {
        if (!ready()) {
            if (error_message != nullptr) {
                *error_message = "stereo warmup not ready for " + config_.name;
            }
            return false;
        }
        if (SessionRecorderRunning()) {
            if (error_message != nullptr) {
                *error_message = "stereo session already active for " + config_.name;
            }
            return false;
        }

        if (!StartSessionRecorder(episode_dir, error_message)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        session_episode_dir_ = episode_dir;
        session_start_system_time_us_ = start_system_time_us;
        stop_requested_by_control_ = false;
        session_failed_ = false;
        last_error_.clear();
        return true;
    }

    void AbortSessionStart() {
        if (warmup_capture_) {
            warmup_capture_->SetForwardFrames(false);
        }
        auto recorder = TakeSessionRecorder();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stop_requested_by_control_ = true;
            session_failed_ = false;
            session_start_system_time_us_ = 0;
            session_episode_dir_.clear();
            session_restart_attempts_ = 0;
        }
        if (recorder) {
            recorder->Stop();
        }
    }

    void CancelSession() {
        AbortSessionStart();
    }

    bool BeginFinalize(const std::string& episode_dir, std::string* error_message) {
        if (warmup_capture_) {
            warmup_capture_->SetForwardFrames(false);
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        stop_requested_by_control_ = true;
        if (session_episode_dir_ != episode_dir) {
            if (error_message != nullptr) {
                *error_message = "stereo session episode mismatch for " + config_.name;
            }
            return false;
        }
        if (!session_recorder_) {
            if (error_message != nullptr) {
                *error_message = "stereo session recorder missing for " + config_.name;
            }
            return false;
        }
        session_recorder_->RequestStop();
        return true;
    }

    bool FinalizeSession(const std::string& episode_dir,
                         int64_t start_system_time_us,
                         int64_t stop_system_time_us,
                         json* info_json,
                         std::string* error_message) {
        if (warmup_capture_) {
            warmup_capture_->SetForwardFrames(false);
        }

        std::unique_ptr<StereoSessionRecorder> recorder;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stop_requested_by_control_ = true;
            if (session_episode_dir_ != episode_dir) {
                if (error_message != nullptr) {
                    *error_message = "stereo session episode mismatch for " + config_.name;
                }
                return false;
            }
            recorder = std::move(session_recorder_);
            session_episode_dir_.clear();
        }
        if (!recorder) {
            if (error_message != nullptr) {
                *error_message = "stereo session recorder missing for " + config_.name;
            }
            return false;
        }

        recorder->Stop();
        recorder->Poll();

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (session_failed_) {
                if (error_message != nullptr) {
                    *error_message = last_error_.empty() ? ("stereo session failed for " + config_.name) : last_error_;
                }
                session_failed_ = false;
                return false;
            }
        }
        if (recorder->HasFailure() && !recorder->HasWrittenOutput()) {
            if (error_message != nullptr) {
                *error_message = "stereo session encoder failed for " + config_.name;
            }
            return false;
        }

        const fs::path final_output =
            fs::path(episode_dir) / ugripper::camera::PrimaryOutputFileName(config_);
        std::error_code fs_error;
        if (!fs::exists(final_output, fs_error) || fs::file_size(final_output, fs_error) == 0) {
            if (error_message != nullptr) {
                *error_message = "stereo output missing or empty for " + config_.name + ": " + final_output.string();
            }
            return false;
        }

        auto record_time_offset_us = recorder->RecordTimeOffsetUs();
        auto first_frame_pts_us = recorder->FirstFramePtsUs();
        auto first_frame_system_time_us = recorder->FirstFrameSystemTimeUs();
        auto last_frame_pts_us = recorder->LastFramePtsUs();
        auto last_frame_system_time_us = recorder->LastFrameSystemTimeUs();
        if (!record_time_offset_us.has_value() ||
            !first_frame_pts_us.has_value() ||
            !first_frame_system_time_us.has_value() ||
            !last_frame_pts_us.has_value() ||
            !last_frame_system_time_us.has_value()) {
            const auto first_submitted_system_time_us = recorder->FirstSubmittedFrameSystemTimeUs();
            const auto last_submitted_system_time_us = recorder->LastSubmittedFrameSystemTimeUs();
            const uint64_t submitted_frame_count = recorder->SubmittedFrameCount();
            const int session_fps = std::max(1, ugripper::camera::StereoSessionFps(config_));
            if (first_submitted_system_time_us.has_value() &&
                last_submitted_system_time_us.has_value() &&
                submitted_frame_count > 0) {
                first_frame_pts_us = 0;
                if (submitted_frame_count > 1) {
                    const long double last_pts_us_ld =
                        (static_cast<long double>(submitted_frame_count - 1) * 1'000'000.0L) /
                        static_cast<long double>(session_fps);
                    last_frame_pts_us = static_cast<int64_t>(std::llround(last_pts_us_ld));
                } else {
                    last_frame_pts_us = 0;
                }
                first_frame_system_time_us = *first_submitted_system_time_us;
                last_frame_system_time_us = *last_submitted_system_time_us;
                record_time_offset_us = *first_frame_system_time_us;
            } else {
                const auto probed_window = ProbeVideoWindowUs(final_output);
                if (first_submitted_system_time_us.has_value() &&
                    last_submitted_system_time_us.has_value() &&
                    probed_window.has_value()) {
                    first_frame_pts_us = probed_window->start_pts_us;
                    last_frame_pts_us = probed_window->start_pts_us + probed_window->duration_us;
                    first_frame_system_time_us = *first_submitted_system_time_us;
                    last_frame_system_time_us = *last_submitted_system_time_us;
                    record_time_offset_us = *first_frame_system_time_us - *first_frame_pts_us;
                } else {
                    if (error_message != nullptr) {
                        *error_message = "stereo timing metadata incomplete for " + config_.name;
                    }
                    return false;
                }
            }
        }

        json camera_info = json::object();
        camera_info["record_time_offset_us"] = *record_time_offset_us;
        camera_info["first_written_frame_pts_us"] = *first_frame_pts_us;
        camera_info["first_written_frame_system_time_us"] = *first_frame_system_time_us;
        camera_info["last_written_frame_pts_us"] = *last_frame_pts_us;
        camera_info["last_written_frame_system_time_us"] = *last_frame_system_time_us;

        if (info_json != nullptr) {
            (*info_json)[config_.name] = camera_info;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_failed_ = false;
            session_start_system_time_us_ = 0;
            session_restart_attempts_ = 0;
        }
        return true;
    }

private:
    bool StartSessionRecorder(const std::string& episode_dir, std::string* error_message) {
        auto recorder = std::make_unique<StereoSessionRecorder>(
            config_,
            options_,
            fs::path(episode_dir) / ugripper::camera::PrimaryOutputFileName(config_));
        if (!recorder->Start()) {
            if (error_message != nullptr) {
                *error_message = "failed to start stereo session recorder for " + config_.name;
            }
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_recorder_ = std::move(recorder);
            ++session_restart_attempts_;
        }
        if (warmup_capture_) {
            warmup_capture_->SetForwardFrames(true);
        }
        return true;
    }

    void TryStartWarmupCapture() {
        const int64_t now_us = CurrentSystemTimeUs();
        if ((now_us - last_start_attempt_system_time_us_) < (kStereoRestartInterval.count() * 1000)) {
            return;
        }
        last_start_attempt_system_time_us_ = now_us;

        if (!Exists(config_.device)) {
            last_error_ = "device node missing: " + config_.device;
            return;
        }

        auto capture = std::make_unique<StereoWarmupCapture>(
            config_,
            [this](StereoCapturedFrame&& frame) { HandleCapturedFrame(std::move(frame)); });
        std::string error_message;
        if (!capture->Start(&error_message)) {
            last_error_ = error_message.empty() ? "failed to start stereo warmup capture" : error_message;
            return;
        }

        warmup_capture_ = std::move(capture);
        last_error_.clear();
    }

    void HandleCapturedFrame(StereoCapturedFrame&& frame) {
        if (frame_drop_modulo_ > 1 && (frame.sequence % static_cast<uint64_t>(frame_drop_modulo_)) != 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!session_recorder_ || !session_recorder_->IsRunning()) {
            return;
        }
        if (!session_recorder_->PushFrame(std::move(frame.frame_bytes), frame.system_time_us) &&
            !stop_requested_by_control_) {
            session_failed_ = true;
            last_error_ = "failed to queue stereo frame for " + config_.name;
        }
    }

    bool SessionRecorderRunning() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return session_recorder_ && session_recorder_->IsRunning();
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return last_error_;
    }

    std::unique_ptr<StereoSessionRecorder> TakeSessionRecorder() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return std::move(session_recorder_);
    }

    CameraConfig config_;
    Options options_;
    int frame_drop_modulo_ = 1;
    std::unique_ptr<StereoWarmupCapture> warmup_capture_;
    mutable std::mutex state_mutex_;
    std::unique_ptr<StereoSessionRecorder> session_recorder_;
    std::string session_episode_dir_;
    int64_t session_start_system_time_us_ = 0;
    bool stop_requested_by_control_ = false;
    bool session_failed_ = false;
    int session_restart_attempts_ = 0;
    int64_t last_start_attempt_system_time_us_ = 0;
    std::string last_error_;
};

class StereoDaemonRunner {
public:
    StereoDaemonRunner(Options options, std::vector<CameraConfig> configs)
        : options_(std::move(options)) {
        for (const auto& config : configs) {
            if (!ugripper::camera::ShouldManageInStereoDaemon(config, options_.only_names)) {
                continue;
            }
            tracks_.push_back(std::make_unique<StereoCameraTrack>(config, options_));
        }
    }

    bool Run() {
        if (tracks_.empty()) {
            DM_LOG_ERROR("[camera_recorder] stereo daemon found no stereo cameras to manage");
            return false;
        }

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            ApplyControlCommand();
            const bool session_active = command_state_.recording || finalize_pending_;
            for (auto& track : tracks_) {
                track->Poll(session_active);
            }
            if (finalize_pending_) {
                MaybeFinalizeStoppedSession();
            }
            WriteStatus();
            std::this_thread::sleep_for(kStereoDaemonPollInterval);
        }
        ShutdownTracks();
        WriteStatus();
        return true;
    }

private:
    void ShutdownTracks() {
        for (auto& track : tracks_) {
            track->Stop();
        }
    }

    void ApplyControlCommand() {
        const auto parsed = ReadJsonFile(options_.control_file);
        if (!parsed.has_value()) {
            return;
        }

        const auto command = ugripper::camera::ParseStereoControlCommand(*parsed);
        if (!command.has_value()) {
            return;
        }
        if (command->command_seq == command_state_.command_seq) {
            return;
        }

        command_state_ = *command;

        if (command_state_.recording) {
            finalize_pending_ = false;
            last_finalize_error_.clear();
            last_session_result_ = json::object();
            active_episode_dir_ = command_state_.episode_dir;
            active_start_system_time_us_ = command_state_.start_system_time_us;
            active_stop_system_time_us_ = 0;
            for (auto& track : tracks_) {
                std::string error_message;
                if (!track->StartSession(active_episode_dir_, active_start_system_time_us_, &error_message)) {
                    last_finalize_error_ = error_message;
                    for (auto& rollback_track : tracks_) {
                        rollback_track->AbortSessionStart();
                    }
                    active_episode_dir_.clear();
                    active_start_system_time_us_ = 0;
                    active_stop_system_time_us_ = 0;
                    command_state_.recording = false;
                    return;
                }
            }
            return;
        }

        if (!active_episode_dir_.empty() &&
            command_state_.episode_dir == active_episode_dir_ &&
            command_state_.stop_system_time_us > 0) {
            finalize_pending_ = true;
            active_stop_system_time_us_ = command_state_.stop_system_time_us;
        }
    }

    void MaybeFinalizeStoppedSession() {
        json stereo_info = json::object();
        stereo_info["cameras"] = json::object();
        stereo_info["episode_dir"] = active_episode_dir_;
        stereo_info["start_system_time_us"] = active_start_system_time_us_;
        stereo_info["stop_system_time_us"] = active_stop_system_time_us_;

        for (auto& track : tracks_) {
            std::string error_message;
            if (!track->BeginFinalize(active_episode_dir_, &error_message)) {
                for (auto& rollback_track : tracks_) {
                    rollback_track->CancelSession();
                }
                last_finalize_error_ = error_message;
                finalize_pending_ = false;
                active_episode_dir_.clear();
                active_start_system_time_us_ = 0;
                active_stop_system_time_us_ = 0;
                return;
            }
        }

        for (auto& track : tracks_) {
            std::string error_message;
            if (!track->FinalizeSession(active_episode_dir_,
                                        active_start_system_time_us_,
                                        active_stop_system_time_us_,
                                        &stereo_info["cameras"],
                                        &error_message)) {
                for (auto& rollback_track : tracks_) {
                    rollback_track->CancelSession();
                }
                last_finalize_error_ = error_message;
                finalize_pending_ = false;
                active_episode_dir_.clear();
                active_start_system_time_us_ = 0;
                active_stop_system_time_us_ = 0;
                return;
            }
        }

        last_session_result_ = stereo_info;
        last_finalize_error_.clear();

        finalize_pending_ = false;
        last_finalized_episode_dir_ = active_episode_dir_;
        active_episode_dir_.clear();
        active_start_system_time_us_ = 0;
        active_stop_system_time_us_ = 0;
    }

    void WriteStatus() const {
        ugripper::camera::StereoServiceStatus status;
        status.recording = command_state_.recording;
        status.finalize_pending = finalize_pending_;
        status.active_episode_dir = active_episode_dir_;
        status.last_finalized_episode_dir = last_finalized_episode_dir_;
        status.last_finalize_error = last_finalize_error_;
        status.last_session = last_session_result_;
        status.ready = true;
        status.service_state = finalize_pending_ ? "finalizing" : "warming";
        bool all_ready = true;
        for (const auto& track : tracks_) {
            const auto camera_status = track->BuildStatus();
            const bool camera_ready = camera_status.ready;
            all_ready = all_ready && camera_ready;
            status.cameras[track->name()] = camera_status;
        }
        status.ready = all_ready;
        if (finalize_pending_) {
            status.service_state = "finalizing";
        } else if (command_state_.recording) {
            status.service_state = all_ready ? "recording" : "recording_recovering";
        } else {
            status.service_state = all_ready ? "ready" : "warming";
        }
        WriteJsonFile(options_.status_file, ugripper::camera::BuildStereoServiceStatusJson(status));
    }

    Options options_;
    std::vector<std::unique_ptr<StereoCameraTrack>> tracks_;
    ugripper::camera::StereoControlCommand command_state_;
    bool finalize_pending_ = false;
    std::string active_episode_dir_;
    std::string last_finalized_episode_dir_;
    std::string last_finalize_error_;
    json last_session_result_ = json::object();
    int64_t active_start_system_time_us_ = 0;
    int64_t active_stop_system_time_us_ = 0;
};

}  // namespace

CameraRecorderManager::CameraRecorderManager(Options options)
    : options_(std::move(options)) {}

void ClearCameraRecorderFault(const fs::path& episode_dir) {
    RemoveCameraRecorderFaultFile(episode_dir);
}

bool CameraRecorderManager::Prepare(const std::vector<CameraConfig>& configs, std::vector<std::string>* missing_devices) {
    recorders_.clear();
    expected_configs_.clear();
    had_failure_ = false;
    partial_timing_written_ = false;

    auto selected = [&](const std::string& name) {
        return options_.only_names.empty() || options_.only_names.count(name) > 0;
    };

    for (const auto& config : configs) {
        if (!selected(config.name)) {
            continue;
        }
        expected_configs_.push_back(config);
        if (!Exists(config.device)) {
            if (missing_devices != nullptr) {
                missing_devices->push_back(config.device);
            }
            continue;
        }
        recorders_.push_back(CreateRecorder(config, options_));
    }
    return true;
}

bool CameraRecorderManager::StartAll() {
    size_t started_count = 0;
    for (size_t index = 0; index < recorders_.size(); ++index) {
        if (recorders_[index]->Start()) {
            started_count++;
        } else {
            had_failure_ = true;
        }
    }

    return started_count > 0;
}

bool CameraRecorderManager::MonitorUntilStop() {
    const auto deadline = (options_.duration_sec > 0)
        ? std::optional<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() + std::chrono::seconds(options_.duration_sec))
        : std::nullopt;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            break;
        }

        bool any_running = false;
        for (auto& recorder : recorders_) {
            recorder->Poll();
            any_running = any_running || recorder->IsRunning();
            if (recorder->HasFailure()) {
                had_failure_ = true;
            }
        }

        if (!partial_timing_written_ && AllExpectedTimingOffsetsAvailable()) {
            partial_timing_written_ = WriteInfoJson(true);
        }

        if (!any_running) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return !had_failure_;
}

void CameraRecorderManager::StopAll() {
    std::vector<std::thread> stop_threads;
    stop_threads.reserve(recorders_.size());
    for (size_t index = 0; index < recorders_.size(); ++index) {
        stop_threads.emplace_back([this, index]() {
            recorders_[index]->Stop();
        });
    }

    for (auto& thread : stop_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    for (auto& recorder : recorders_) {
        recorder->Poll();
        if (recorder->HasFailure()) {
            had_failure_ = true;
        }
    }
}

bool CameraRecorderManager::empty() const {
    return recorders_.empty();
}

bool CameraRecorderManager::had_failure() const {
    return had_failure_;
}

const std::vector<std::unique_ptr<CameraRecorder>>& CameraRecorderManager::recorders() const {
    return recorders_;
}

bool CameraRecorderManager::AllExpectedTimingOffsetsAvailable() const {
    for (const auto& config : expected_configs_) {
        const CameraRecorder* matching_recorder = nullptr;
        for (const auto& recorder : recorders_) {
            if (recorder->config().name == config.name) {
                matching_recorder = recorder.get();
                break;
            }
        }
        if (matching_recorder == nullptr ||
            !matching_recorder->RecordTimeOffsetUs().has_value()) {
            return false;
        }
    }
    return !expected_configs_.empty();
}

bool CameraRecorderManager::WriteInfoJson(bool allow_missing_offsets) const {
    const int64_t boot_time_offset_us = BootTimeOffsetUs();
    const fs::path info_json_path = RecordingTimingPathForEpisode(options_.output_dir);
    std::ofstream output(info_json_path, std::ios::trunc);
    if (!output.is_open()) {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] failed to open recording timing file for write: "
                              << info_json_path).str());
        return false;
    }

    output << "{\n";
    output << "  \"boot_time_offset\": " << std::fixed << std::setprecision(6)
           << (static_cast<double>(boot_time_offset_us) / 1'000'000.0) << ",\n";
    output << "  \"boot_time_offset_us\": " << boot_time_offset_us;

    bool missing_offset = false;
    std::vector<std::string> missing_cameras;
    std::vector<std::string> missing_devices;
    for (const auto& config : expected_configs_) {
        const CameraRecorder* matching_recorder = nullptr;
        for (const auto& recorder : recorders_) {
            if (recorder->config().name == config.name) {
                matching_recorder = recorder.get();
                break;
            }
        }

        std::optional<int64_t> offset_us;
        if (matching_recorder != nullptr) {
            offset_us = matching_recorder->RecordTimeOffsetUs();
        }
        if (matching_recorder != nullptr &&
            !offset_us.has_value() &&
            options_.codec == "h265" &&
            IsMainCamera(config)) {
            offset_us = boot_time_offset_us;
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "[camera_recorder] fallback to boot_time_offset_us for main camera "
                                 << config.name << " in h265 direct-stream mode").str());
        }
        output << ",\n"
               << "  \"" << config.name << "_record_time_offset_us\": ";
        if (!offset_us.has_value()) {
            missing_offset = true;
            if (matching_recorder == nullptr) {
                missing_devices.push_back(config.name);
                DM_LOG_ERROR("{}", (::DA::utils::LogString()
                                    << "[camera_recorder] missing camera device for timing: "
                                    << config.name << " device=" << config.device).str());
            } else {
                missing_cameras.push_back(config.name);
                DM_LOG_ERROR("{}", (::DA::utils::LogString() << "[camera_recorder] missing record time offset for "
                                      << config.name).str());
            }
            output << "null";
            continue;
        }
        output << *offset_us;
    }
    if (!missing_devices.empty() || !missing_cameras.empty()) {
        output << ",\n  \"timing_errors\": [";
        bool first_error = true;
        auto write_error = [&](const std::string& camera_name, const std::string& error_type) {
            if (!first_error) {
                output << ",";
            }
            first_error = false;
            output << "\n    {\"camera_name\": \"" << camera_name
                   << "\", \"error\": \"" << error_type << "\"}";
        };
        for (const auto& camera_name : missing_devices) {
            write_error(camera_name, "device_missing");
        }
        for (const auto& camera_name : missing_cameras) {
            write_error(camera_name, "missing_record_time_offset_us");
        }
        output << "\n  ]";
    }
    output << "\n}\n";
    output.flush();
    if (!output.good()) {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
                            << "[camera_recorder] failed to flush recording timing file: "
                            << info_json_path).str());
        return false;
    }
    output.close();
    if (!output) {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
                            << "[camera_recorder] failed to close recording timing file after write: "
                            << info_json_path).str());
        return false;
    }
    if (missing_offset) {
        std::ostringstream oss;
        bool wrote_any = false;
        for (const auto& camera : missing_devices) {
            if (wrote_any) {
                oss << ",";
            }
            wrote_any = true;
            oss << camera << ":device_missing";
        }
        for (size_t i = 0; i < missing_cameras.size(); ++i) {
            if (wrote_any || i != 0) {
                oss << ",";
            }
            wrote_any = true;
            oss << missing_cameras[i];
        }
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
                            << "[camera_recorder] recording timing file is incomplete: path="
                            << info_json_path
                            << " missing_record_time_offset_us="
                            << oss.str()).str());
        if (!allow_missing_offsets) {
            return false;
        }
    }
    DM_LOG_INFO("{}", (::DA::utils::LogString()
                       << "[camera_recorder] wrote recording timing file: "
                       << info_json_path).str());
    return true;
}

Options ParseArgs(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&](const std::string& flag) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + flag);
            }
            return argv[++index];
        };

        if (arg == "--output-dir") {
            options.output_dir = require_value(arg);
        } else if (arg == "--codec") {
            options.codec = require_value(arg);
        } else if (arg == "-d" || arg == "--duration") {
            options.duration_sec = std::stoi(require_value(arg));
        } else if (arg == "--ffmpeg-bin") {
            options.ffmpeg_bin = require_value(arg);
        } else if (arg == "--gst-bin") {
            options.gst_bin = require_value(arg);
        } else if (arg == "--config-yaml") {
            options.config_yaml = require_value(arg);
            options.config_yaml_explicit = true;
        } else if (arg == "--only") {
            std::stringstream ss(require_value(arg));
            std::string item;
            while (std::getline(ss, item, ',')) {
                item = Trim(item);
                if (!item.empty()) {
                    options.only_names.insert(item);
                }
            }
        } else if (arg == "--allow-missing") {
            options.allow_missing = true;
        } else if (arg == "--stereo-daemon") {
            options.stereo_daemon = true;
        } else if (arg == "--control-file") {
            options.control_file = require_value(arg);
        } else if (arg == "--status-file") {
            options.status_file = require_value(arg);
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout
                << "Usage: CameraRecorder --output-dir DIR [--codec h264|h265] [--duration SEC] [--config-yaml PATH] [--allow-missing] [--only a,b] [--dry-run]\n"
                << "   or: CameraRecorder --stereo-daemon [--config-yaml PATH] [--control-file PATH] [--status-file PATH]\n"
                << "Records main/tactile/stereo streams with YAML-driven recorder classes.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (!options.stereo_daemon && options.output_dir.empty()) {
        throw std::runtime_error("--output-dir is required");
    }

    options.codec = ResolveCodec(options.codec);
    if (options.codec != "h264" && options.codec != "h265") {
        throw std::runtime_error("--codec must be h264 or h265");
    }

    return options;
}

std::vector<CameraConfig> LoadCameraConfigList(const fs::path& yaml_path) {
    return ugripper::camera::LoadCameraConfigList(yaml_path);
}

std::string ModeName(CameraRecordMode mode) {
    return ugripper::camera::ModeName(mode);
}

bool RunStereoDaemon(const Options& options, const std::vector<CameraConfig>& configs) {
    StereoDaemonRunner runner(options, configs);
    return runner.Run();
}

void InstallSignalHandlers() {
    g_stop_requested.store(false, std::memory_order_relaxed);
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace camera_recorder
