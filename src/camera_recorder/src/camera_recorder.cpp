#include "camera_recorder/camera_recorder.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <yaml-cpp/yaml.h>

namespace camera_recorder {

namespace {

std::atomic<bool> g_stop_requested{false};

void SignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::string Trim(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const size_t start = input.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
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

bool IsMainCamera(const CameraConfig& config) {
    return config.name == "left_cam_main" || config.name == "right_cam_main";
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

std::string GetRkmppEncoder(const std::string& codec) {
    return codec == "h264" ? "h264_rkmpp" : "hevc_rkmpp";
}

std::string CommonEncodeArgs(const std::string& codec) {
    std::ostringstream oss;
    oss << "-c:v " << GetRkmppEncoder(codec) << ' '
        << "-rc_mode CQP "
        << "-qp_init 30 "
        << "-qp_max 38 "
        << "-qp_min 24 "
        << "-qp_max_i 38 "
        << "-qp_min_i 20 ";

    if (codec == "h264") {
        oss << "-profile:v main -level 5.1 ";
    } else {
        oss << "-profile:v main ";
    }
    return oss.str();
}

CameraRecordMode ParseMode(const std::string& mode_text) {
    if (mode_text == "direct-copy-h265") {
        return CameraRecordMode::DirectCopyH265;
    }
    if (mode_text == "hybrid-decode-encode") {
        return CameraRecordMode::HybridDecodeEncode;
    }
    if (mode_text == "stereo-hybrid-decode-encode") {
        return CameraRecordMode::StereoHybridDecodeEncode;
    }
    throw std::runtime_error("invalid camera mode: " + mode_text);
}

const YAML::Node RequireNode(const YAML::Node& parent, const std::string& key, const std::string& context) {
    const YAML::Node node = parent[key];
    if (!node) {
        throw std::runtime_error("missing field '" + key + "' in " + context);
    }
    return node;
}

std::string RequireString(const YAML::Node& parent, const std::string& key, const std::string& context) {
    const YAML::Node node = RequireNode(parent, key, context);
    if (!node.IsScalar()) {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    const std::string value = Trim(node.as<std::string>());
    if (value.empty()) {
        throw std::runtime_error("field '" + key + "' must not be empty in " + context);
    }
    return value;
}

int OptionalInt(const YAML::Node& parent, const std::string& key, int default_value, const std::string& context) {
    const YAML::Node node = parent[key];
    if (!node) {
        return default_value;
    }
    if (!node.IsScalar()) {
        throw std::runtime_error("field '" + key + "' must be a scalar in " + context);
    }
    return node.as<int>();
}

int RequirePositiveInt(const YAML::Node& parent, const std::string& key, const std::string& context) {
    const int value = RequireNode(parent, key, context).as<int>();
    if (value <= 0) {
        throw std::runtime_error("field '" + key + "' must be > 0 in " + context);
    }
    return value;
}

std::vector<std::string> RequireOutputFiles(const YAML::Node& parent, const std::string& context) {
    const YAML::Node output_files = RequireNode(parent, "output_files", context);
    if (!output_files.IsSequence() || output_files.size() == 0) {
        throw std::runtime_error("field 'output_files' must be a non-empty sequence in " + context);
    }

    std::vector<std::string> result;
    result.reserve(output_files.size());
    for (size_t index = 0; index < output_files.size(); ++index) {
        const YAML::Node item = output_files[index];
        if (!item.IsScalar()) {
            throw std::runtime_error("output_files[" + std::to_string(index) + "] must be a scalar in " + context);
        }
        const std::string file_name = Trim(item.as<std::string>());
        if (file_name.empty()) {
            throw std::runtime_error("output_files[" + std::to_string(index) + "] must not be empty in " + context);
        }
        result.push_back(file_name);
    }
    return result;
}

CameraConfig ParseCameraConfig(const YAML::Node& camera_node, size_t index) {
    const std::string context = "cameras[" + std::to_string(index) + "]";
    CameraConfig config;
    config.name = RequireString(camera_node, "name", context);
    config.device = RequireString(camera_node, "device", context);
    config.mode = ParseMode(RequireString(camera_node, "mode", context));
    config.width = RequirePositiveInt(camera_node, "width", context);
    config.height = RequirePositiveInt(camera_node, "height", context);
    config.fps = RequirePositiveInt(camera_node, "fps", context);
    config.output_fps = OptionalInt(camera_node, "output_fps", 0, context);
    config.eye_width = OptionalInt(camera_node, "eye_width", 0, context);
    config.eye_height = OptionalInt(camera_node, "eye_height", 0, context);
    config.output_files = RequireOutputFiles(camera_node, context);
    config.input_thread_queue_size = OptionalInt(camera_node, "input_thread_queue_size", 0, context);
    if (config.input_thread_queue_size < 0) {
        throw std::runtime_error("field 'input_thread_queue_size' must be >= 0 in " + context);
    }
    return config;
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

        std::cout << "[camera_recorder] starting " << config_.name
                  << " mode=" << ModeName(config_.mode) << std::endl;
        std::cout << "[camera_recorder] cmd: " << command_ << std::endl;

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
            setsid();
            close(output_pipe[0]);
            dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            close(output_pipe[1]);
            execl("/bin/bash", "bash", "-lc", command_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(output_pipe[1]);
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
            std::cerr << "[camera_recorder] recorder exited early: " << config_.name
                      << " exit_code=" << *exit_code_ << std::endl;
        } else {
            std::cout << "[camera_recorder] recorder stopped: " << config_.name
                      << " exit_code=" << *exit_code_ << std::endl;
        }
    }

    void Stop() override {
        if (!running_ || pid_ <= 0) {
            JoinOutputReaderThread();
            return;
        }

        stop_requested_ = true;
        kill(-pid_, SIGINT);

        const auto soft_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < soft_deadline) {
            Poll();
            if (!running_) {
                JoinOutputReaderThread();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        kill(-pid_, SIGTERM);
        const auto hard_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < hard_deadline) {
            Poll();
            if (!running_) {
                JoinOutputReaderThread();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        kill(-pid_, SIGKILL);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Poll();
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

    std::optional<int64_t> RecordTimeOffsetUs() const override {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        return record_time_offset_us_;
    }

protected:
    virtual std::string BuildCommand() const = 0;

    fs::path OutputPath(const std::string& file_name) const {
        return options_.output_dir / file_name;
    }

    void MaybeLockFirstFrame(int64_t frame_pts_us) {
        std::lock_guard<std::mutex> lock(first_frame_mutex_);
        if (record_time_offset_us_.has_value()) {
            return;
        }

        const int64_t system_time_us = CurrentSystemTimeUs();
        first_frame_pts_us_ = frame_pts_us;
        first_frame_system_time_us_ = system_time_us;
        record_time_offset_us_ = system_time_us - frame_pts_us;

        std::cout << "[camera_recorder] first frame locked: " << config_.name
                  << " frame_pts_us=" << frame_pts_us
                  << " system_time_us=" << system_time_us
                  << " record_time_offset_us=" << *record_time_offset_us_
                  << std::endl;
    }

    void HandleOutputLine(const std::string& raw_line) {
        const std::string line = Trim(raw_line);
        if (line.empty()) {
            return;
        }

        if (const auto pts_us = ParseFramePtsUsFromStatsLine(line); pts_us.has_value()) {
            MaybeLockFirstFrame(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromDebugTsLine(line); pts_us.has_value()) {
            MaybeLockFirstFrame(*pts_us);
            return;
        }
        if (const auto pts_us = ParseFramePtsUsFromGstIdentityLine(line); pts_us.has_value()) {
            MaybeLockFirstFrame(*pts_us);
            return;
        }

        std::cout << "[" << config_.name << "] " << line << std::endl;
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
    std::optional<int64_t> record_time_offset_us_;
};

class MainCameraRecorder final : public ShellCameraRecorder {
public:
    MainCameraRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        const std::string device = ShellQuote(config_.device);
        const std::string output = ShellQuote(OutputPath(config_.output_files.at(0)).string());

        if (options_.codec == "h264") {
            std::ostringstream oss;
            oss << options_.ffmpeg_bin
                << " -hide_banner -loglevel info -nostats -debug_ts -y "
                << "-f v4l2 -input_format h264 "
                << "-framerate " << config_.fps << ' '
                << "-video_size " << config_.width << 'x' << config_.height << ' '
                << "-copyts "
                << "-i " << device << ' '
                << "-c:v copy "
                << output;
            return oss.str();
        }

        std::ostringstream oss;
        oss << "GST_DEBUG=identity:7 " << options_.gst_bin << " -e "
            << "v4l2src device=" << device << " do-timestamp=true ! "
            << ShellQuote(
                   "video/x-h265,width=" + std::to_string(config_.width) +
                   ",height=" + std::to_string(config_.height) +
                   ",framerate=" + std::to_string(config_.fps) + "/1")
            << " ! "
            << "identity silent=false ! "
            << "queue leaky=downstream max-size-buffers=4 ! "
            << "h265parse config-interval=-1 ! "
            << "matroskamux ! "
            << "filesink location="
            << output;
        return oss.str();
    }
};

class HybridCameraRecorder final : public ShellCameraRecorder {
public:
    HybridCameraRecorder(CameraConfig config, Options options)
        : ShellCameraRecorder(std::move(config), std::move(options)) {
        command_ = BuildCommand();
    }

private:
    std::string BuildCommand() const override {
        const std::string device = ShellQuote(config_.device);
        const std::string output = ShellQuote(OutputPath(config_.output_files.at(0)).string());

        std::ostringstream oss;
        oss << options_.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y ";
        if (config_.input_thread_queue_size > 0) {
            oss << "-thread_queue_size " << config_.input_thread_queue_size << ' ';
        }
        oss << "-f v4l2 -input_format mjpeg "
            << "-framerate " << config_.fps << ' '
            << "-video_size " << config_.width << 'x' << config_.height << ' '
            << "-i " << device << ' '
            << CommonEncodeArgs(options_.codec)
            << "-stats_mux_pre pipe:1 "
            << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
            << output;
        return oss.str();
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
        const std::string device = ShellQuote(config_.device);
        const std::string output = ShellQuote(OutputPath(config_.output_files.at(0)).string());

        std::ostringstream oss;
        oss << options_.ffmpeg_bin
            << " -hide_banner -loglevel warning -nostats -y ";
        if (config_.input_thread_queue_size > 0) {
            oss << "-thread_queue_size " << config_.input_thread_queue_size << ' ';
        }
        oss << "-f v4l2 -input_format mjpeg "
            << "-framerate " << config_.fps << ' '
            << "-video_size " << config_.width << 'x' << config_.height << ' '
            << "-i " << device << ' '
            << CommonEncodeArgs(options_.codec)
            << "-stats_mux_pre pipe:1 "
            << "-stats_mux_pre_fmt " << ShellQuote("{pts} {tb}") << ' '
            << output;
        return oss.str();
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

}  // namespace

CameraRecorderManager::CameraRecorderManager(Options options)
    : options_(std::move(options)) {}

bool CameraRecorderManager::Prepare(const std::vector<CameraConfig>& configs, std::vector<std::string>* missing_devices) {
    recorders_.clear();
    had_failure_ = false;

    auto selected = [&](const std::string& name) {
        return options_.only_names.empty() || options_.only_names.count(name) > 0;
    };

    for (const auto& config : configs) {
        if (!selected(config.name)) {
            continue;
        }
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
    for (auto& recorder : recorders_) {
        if (recorder->Start()) {
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

        if (!any_running) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return !had_failure_;
}

void CameraRecorderManager::StopAll() {
    for (auto& recorder : recorders_) {
        recorder->Stop();
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

bool CameraRecorderManager::WriteInfoJson() const {
    const int64_t boot_time_offset_us = BootTimeOffsetUs();
    const fs::path info_json_path = options_.output_dir / "info.json";
    std::ofstream output(info_json_path, std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "[camera_recorder] failed to open info.json for write: "
                  << info_json_path << std::endl;
        return false;
    }

    output << "{\n";
    output << "  \"boot_time_offset\": " << std::fixed << std::setprecision(6)
           << (static_cast<double>(boot_time_offset_us) / 1'000'000.0) << ",\n";
    output << "  \"boot_time_offset_us\": " << boot_time_offset_us;

    bool missing_offset = false;
    for (const auto& recorder : recorders_) {
        auto offset_us = recorder->RecordTimeOffsetUs();
        if (!offset_us.has_value() && options_.codec == "h265" && IsMainCamera(recorder->config())) {
            offset_us = boot_time_offset_us;
            std::cerr << "[camera_recorder] fallback to boot_time_offset_us for main camera "
                      << recorder->config().name << " in h265 direct-stream mode" << std::endl;
        }
        if (!offset_us.has_value()) {
            std::cerr << "[camera_recorder] missing record time offset for "
                      << recorder->config().name << std::endl;
            missing_offset = true;
            continue;
        }
        output << ",\n"
               << "  \"" << recorder->config().name << "_record_time_offset_us\": "
               << *offset_us;
    }
    output << "\n}\n";
    return !missing_offset;
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
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout
                << "Usage: camera_recorder --output-dir DIR [--codec h264|h265] [--duration SEC] [--config-yaml PATH] [--allow-missing] [--only a,b] [--dry-run]\n"
                << "Records main/tactile/stereo streams with YAML-driven recorder classes.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (options.output_dir.empty()) {
        throw std::runtime_error("--output-dir is required");
    }

    options.codec = ResolveCodec(options.codec);
    if (options.codec != "h264" && options.codec != "h265") {
        throw std::runtime_error("--codec must be h264 or h265");
    }

    return options;
}

std::vector<CameraConfig> LoadCameraConfigList(const fs::path& yaml_path) {
    std::error_code error;
    if (!fs::exists(yaml_path, error)) {
        throw std::runtime_error("camera config yaml not found: " + yaml_path.string());
    }

    const YAML::Node root = YAML::LoadFile(yaml_path.string());
    const YAML::Node cameras = root["cameras"];
    if (!cameras || !cameras.IsSequence() || cameras.size() == 0) {
        throw std::runtime_error("camera config yaml must contain non-empty 'cameras' sequence: " + yaml_path.string());
    }

    std::vector<CameraConfig> configs;
    configs.reserve(cameras.size());
    std::set<std::string> names;
    for (size_t index = 0; index < cameras.size(); ++index) {
        const YAML::Node camera_node = cameras[index];
        if (!camera_node.IsMap()) {
            throw std::runtime_error("cameras[" + std::to_string(index) + "] must be a map in " + yaml_path.string());
        }

        CameraConfig config = ParseCameraConfig(camera_node, index);
        if (!names.insert(config.name).second) {
            throw std::runtime_error("duplicate camera name in yaml: " + config.name);
        }
        configs.push_back(std::move(config));
    }

    return configs;
}

std::string ModeName(CameraRecordMode mode) {
    switch (mode) {
    case CameraRecordMode::DirectCopyH265:
        return "direct-copy-h265";
    case CameraRecordMode::HybridDecodeEncode:
        return "hybrid-decode-encode";
    case CameraRecordMode::StereoHybridDecodeEncode:
        return "stereo-hybrid-decode-encode";
    }
    return "unknown";
}

void InstallSignalHandlers() {
    g_stop_requested.store(false, std::memory_order_relaxed);
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
}

}  // namespace camera_recorder
