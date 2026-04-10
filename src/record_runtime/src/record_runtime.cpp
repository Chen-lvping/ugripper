#include "record_runtime.h"
#include "motion_alert_ipc.h"

#include <mcap/reader.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr const char *kSessionCameraStreamsCsv =
    "left_cam_main,right_cam_main,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r";
constexpr const char *kWarmupCameraStreamsCsv = "left_stereo,right_stereo";
constexpr uint64_t kActionDebounceMs = 250;
constexpr uint64_t kLongPressThresholdMs = 800;
constexpr uint64_t kDualLongPressThresholdMs = 4000;
constexpr uint64_t kShutdownPromptThresholdMs = 2000;
constexpr uint64_t kAudioPlayerRestartIntervalMs = 2000;
constexpr uint64_t kAudioPlayerReadyGraceMs = 3000;
constexpr uint64_t kStereoDaemonRestartIntervalMs = 2000;
constexpr uint64_t kStereoFinalizeWaitPollMs = 100;
constexpr uint64_t kHealthCheckIntervalMs = 1000;
constexpr uint64_t kHmiActiveTimeoutMs = 2500;
constexpr double kMinReasonableVideoSpanSec = 0.2;
constexpr double kMaxVideoSpanGapSec = 5.0;
constexpr int64_t kEncoderTailWindowNs = 1000LL * 1000LL * 1000LL;
constexpr int64_t kEncoderTailMaxLagNs = 1000LL * 1000LL * 1000LL;
constexpr size_t kEncoderTailChunkScanLimit = 4;
constexpr int kVideoProbeTimeoutMs = 1500;
constexpr int kUdevadmProbeTimeoutMs = 2000;

struct EpisodeVideoArtifact
{
    const char *cameraName;
    const char *fileName;
};

constexpr std::array<EpisodeVideoArtifact, 8> kEpisodeVideoArtifacts = {{
    {"left_cam_main", "left_cam_main.mkv"},
    {"right_cam_main", "right_cam_main.mkv"},
    {"left_stereo", "left_stereo.mkv"},
    {"right_stereo", "right_stereo.mkv"},
    {"left_tcam_l", "left_tcam_l.mkv"},
    {"left_tcam_r", "left_tcam_r.mkv"},
    {"right_tcam_l", "right_tcam_l.mkv"},
    {"right_tcam_r", "right_tcam_r.mkv"},
}};

constexpr std::array<const char *, 12> kCriticalDevicePaths = {{
    "/dev/right_cam_main",
    "/dev/left_cam_main",
    "/dev/right_stereo",
    "/dev/left_stereo",
    "/dev/right_tcam_l",
    "/dev/right_tcam_r",
    "/dev/left_tcam_l",
    "/dev/left_tcam_r",
    "/dev/right_encoder",
    "/dev/left_encoder",
    "/dev/right_imu",
    "/dev/left_imu",
}};

struct VideoProbeResult
{
    std::string cameraName;
    std::string fileName;
    double startTimeSec = 0.0;
    double durationSec = 0.0;
    double spanSec = 0.0;
};

struct EncoderTailCheckTarget
{
    const char *side;
    const char *mcapFileName;
    const char *encoderTopic;
    std::array<const char *, 4> referenceCameraNames;
};

constexpr std::array<EncoderTailCheckTarget, 2> kEncoderTailCheckTargets = {{
    {"left",
     "sensor_data_left.mcap",
     "encoder_left",
     {"left_cam_main", "left_stereo", "left_tcam_l", "left_tcam_r"}},
    {"right",
     "sensor_data_right.mcap",
     "encoder_right",
     {"right_cam_main", "right_stereo", "right_tcam_l", "right_tcam_r"}},
}};

struct CommandCaptureResult
{
    bool success = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output;
};

struct TactileCalibrationTarget
{
    const char *cameraName;
    const char *side;
    const char *devicePath;
    const char *jsonPath;
    const char *serialPlaceholder;
};

constexpr std::array<TactileCalibrationTarget, 4> kTactileCalibrationTargets = {{
    {"left_tcam_l", "left", "/dev/left_tcam_l", "observation.images.left_tcam_l", "{{LEFT_TCAM_L_SERIAL}}"},
    {"left_tcam_r", "left", "/dev/left_tcam_r", "observation.images.left_tcam_r", "{{LEFT_TCAM_R_SERIAL}}"},
    {"right_tcam_l", "right", "/dev/right_tcam_l", "observation.images.right_tcam_l", "{{RIGHT_TCAM_L_SERIAL}}"},
    {"right_tcam_r", "right", "/dev/right_tcam_r", "observation.images.right_tcam_r", "{{RIGHT_TCAM_R_SERIAL}}"},
}};

bool writeTextFileAtomically(const fs::path &path, const std::string &content, std::string *errorMessage)
{
    const fs::path parent = path.parent_path();
    std::error_code error;
    if (!parent.empty())
    {
        fs::create_directories(parent, error);
        if (error)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "create parent directory failed: " + error.message();
            }
            return false;
        }
    }

    const fs::path tempPath = path.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::trunc);
        if (!output.is_open())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "cannot open temp file for write: " + tempPath.string();
            }
            return false;
        }
        output << content;
        output.flush();
        if (!output.good())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "failed to write temp file: " + tempPath.string();
            }
            output.close();
            fs::remove(tempPath, error);
            return false;
        }
    }

    fs::rename(tempPath, path, error);
    if (error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "rename temp file failed: " + error.message();
        }
        fs::remove(tempPath, error);
        return false;
    }

    return true;
}

json makeStereoPlaceholderFromTemplate(const json *templateEntry)
{
    json placeholder = json::object();
    if (templateEntry != nullptr && templateEntry->is_object())
    {
        placeholder = *templateEntry;
    }

    if (!placeholder.contains("shape") || !placeholder["shape"].is_array())
    {
        placeholder["shape"] = json::array({400, 1280, 3});
    }
    if (!placeholder.contains("names") || !placeholder["names"].is_array())
    {
        placeholder["names"] = json::array({"height", "width", "channels"});
    }
    if (!placeholder.contains("info"))
    {
        placeholder["info"] = nullptr;
    }
    if (!placeholder.contains("dtype"))
    {
        placeholder["dtype"] = "video";
    }
    if (!placeholder.contains("fps"))
    {
        placeholder["fps"] = 60;
    }

    if (!placeholder.contains("camera_model"))
    {
        placeholder["camera_model"] = "pinhole";
    }
    if (!placeholder.contains("distortion_model"))
    {
        placeholder["distortion_model"] = "equidistant";
    }
    if (!placeholder.contains("distortion_coeffs"))
    {
        placeholder["distortion_coeffs"] = json::array({0.0, 0.0, 0.0, 0.0});
    }

    if (!placeholder.contains("intrinsics") || !placeholder["intrinsics"].is_object() || placeholder["intrinsics"].empty())
    {
        placeholder["intrinsics"] = json::object({
            {"1280x400", {
                {"fx", 1280.0},
                {"fy", 400.0},
                {"ppx", 640.0},
                {"ppy", 200.0},
            }},
        });
    }

    return placeholder;
}

json makeTactileCalibrationEntry(const json *templateEntry, const std::string &serialPlaceholder)
{
    json entry = json::object();
    if (templateEntry != nullptr && templateEntry->is_object())
    {
        entry = *templateEntry;
    }

    if (!entry.contains("shape") || !entry["shape"].is_array())
    {
        entry["shape"] = json::array({480, 640, 3});
    }
    if (!entry.contains("names") || !entry["names"].is_array())
    {
        entry["names"] = json::array({"height", "width", "channels"});
    }
    if (!entry.contains("info"))
    {
        entry["info"] = nullptr;
    }
    if (!entry.contains("dtype"))
    {
        entry["dtype"] = "video";
    }

    entry["serial"] = serialPlaceholder;
    return entry;
}

void appendCalibrationNote(json *calibrationInfo, const std::string &message)
{
    if (calibrationInfo == nullptr || !calibrationInfo->is_object())
    {
        return;
    }

    const std::string existing = calibrationInfo->value("notes", std::string());
    if (existing.empty())
    {
        (*calibrationInfo)["notes"] = message;
        return;
    }

    if (existing.find(message) != std::string::npos)
    {
        return;
    }

    (*calibrationInfo)["notes"] = existing + " " + message;
}

void removeIfPresent(json *root, const std::string &key)
{
    if (root != nullptr && root->is_object())
    {
        root->erase(key);
    }
}

void replaceJsonStringValues(json *node, const std::string &placeholder, const std::string &replacement)
{
    if (node == nullptr || placeholder.empty())
    {
        return;
    }

    if (node->is_string())
    {
        std::string value = node->get<std::string>();
        size_t position = value.find(placeholder);
        while (position != std::string::npos)
        {
            value.replace(position, placeholder.size(), replacement);
            position = value.find(placeholder, position + replacement.size());
        }
        *node = value;
        return;
    }

    if (node->is_array())
    {
        for (json &item : *node)
        {
            replaceJsonStringValues(&item, placeholder, replacement);
        }
        return;
    }

    if (node->is_object())
    {
        for (auto &item : node->items())
        {
            replaceJsonStringValues(&item.value(), placeholder, replacement);
        }
    }
}

std::string legacyTactileJsonPathForSide(const std::string &side)
{
    return side == "left" ? "observation.images.gripper_left_tactile"
                          : "observation.images.gripper_right_tactile";
}

std::string legacyTactileSerialPlaceholderForSide(const std::string &side)
{
    return side == "left" ? "{{TACTILE_LEFT_SERIAL}}"
                          : "{{TACTILE_RIGHT_SERIAL}}";
}

std::vector<std::string> tactileTemplateCandidateKeys(const TactileCalibrationTarget &target)
{
    std::vector<std::string> keys;
    keys.push_back(target.jsonPath);
    for (const auto &candidate : kTactileCalibrationTargets)
    {
        if (std::string(candidate.side) == target.side && std::string(candidate.jsonPath) != target.jsonPath)
        {
            keys.push_back(candidate.jsonPath);
        }
    }
    keys.push_back(legacyTactileJsonPathForSide(target.side));
    return keys;
}

const json *findSourceCalibrationEntry(const std::map<std::string, json> &entries, const std::vector<std::string> &keys)
{
    for (const std::string &key : keys)
    {
        const auto it = entries.find(key);
        if (it != entries.end() && it->second.is_object())
        {
            return &it->second;
        }
    }
    return nullptr;
}

bool loadJsonFile(const std::string &path, json *output, std::string *errorMessage)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open calibration source: " + path;
        }
        return false;
    }

    try
    {
        input >> *output;
        return true;
    }
    catch (const std::exception &error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "invalid calibration json in " + path + ": " + error.what();
        }
        return false;
    }
}

bool loadCalibrationJsonFile(const std::string &path, json *output, std::string *errorMessage)
{
    if (!loadJsonFile(path, output, errorMessage))
    {
        return false;
    }

    if (!output->is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "invalid calibration json in " + path + ": top-level JSON must be an object";
        }
        return false;
    }

    return true;
}

std::string joinArguments(const std::vector<std::string> &arguments)
{
    std::ostringstream stream;
    for (size_t index = 0; index < arguments.size(); ++index)
    {
        if (index > 0)
        {
            stream << ' ';
        }
        stream << arguments[index];
    }
    return stream.str();
}

std::string joinStrings(const std::vector<std::string> &items, const char *separator)
{
    std::ostringstream stream;
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (index > 0)
        {
            stream << separator;
        }
        stream << items[index];
    }
    return stream.str();
}

bool moveFileWithCrossDeviceFallback(const fs::path &source,
                                     const fs::path &target,
                                     std::error_code *error)
{
    std::error_code localError;
    fs::rename(source, target, localError);
    if (!localError)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        return true;
    }

    if (localError != std::make_error_code(std::errc::cross_device_link))
    {
        if (error != nullptr)
        {
            *error = localError;
        }
        return false;
    }

    localError.clear();
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, localError);
    if (localError)
    {
        if (error != nullptr)
        {
            *error = localError;
        }
        return false;
    }

    fs::remove(source, localError);
    if (localError)
    {
        std::error_code cleanupError;
        fs::remove(target, cleanupError);
        if (error != nullptr)
        {
            *error = localError;
        }
        return false;
    }

    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

int64_t steadyNowMs()
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

bool flushFileToDisk(const fs::path &path, std::error_code *error)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = std::error_code(errno, std::generic_category());
        }
        return false;
    }

    if (::fsync(fd) != 0)
    {
        const std::error_code syncError(errno, std::generic_category());
        close(fd);
        if (error != nullptr)
        {
            *error = syncError;
        }
        return false;
    }

    close(fd);

    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

bool flushDirectoryToDisk(const fs::path &path, std::error_code *error)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0)
    {
        if (error != nullptr)
        {
            *error = std::error_code(errno, std::generic_category());
        }
        return false;
    }

    if (::fsync(fd) != 0)
    {
        const std::error_code syncError(errno, std::generic_category());
        close(fd);
        if (error != nullptr)
        {
            *error = syncError;
        }
        return false;
    }

    close(fd);

    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

void flushEpisodeDirectoriesToDisk(const fs::path &episodeDir, const char *phaseLabel)
{
    if (episodeDir.empty())
    {
        return;
    }

    std::vector<fs::path> directories;
    directories.push_back(episodeDir);

    const fs::path parentDir = episodeDir.parent_path();
    if (!parentDir.empty() && parentDir != episodeDir)
    {
        directories.push_back(parentDir);
    }

    for (const auto &directory : directories)
    {
        if (!fs::exists(directory))
        {
            std::cout << "[PERF] flush dir skip missing: phase=" << phaseLabel
                      << " path=" << directory << std::endl;
            continue;
        }

        const int64_t dirFlushStartMs = steadyNowMs();
        std::error_code error;
        if (!flushDirectoryToDisk(directory, &error))
        {
            std::cerr << "[WARN] failed to flush episode directory: phase=" << phaseLabel
                      << " path=" << directory
                      << " error=" << error.message() << std::endl;
            continue;
        }

        std::cout << "[PERF] flush dir done: phase=" << phaseLabel
                  << " path=" << directory
                  << " elapsed_ms=" << (steadyNowMs() - dirFlushStartMs) << std::endl;
    }
}

void flushEpisodeArtifactsToDisk(const fs::path &episodeDir, const char *phaseLabel)
{
    if (episodeDir.empty())
    {
        return;
    }

    std::vector<fs::path> paths;
    paths.reserve(kEpisodeVideoArtifacts.size() + 6);

    for (const auto &artifact : kEpisodeVideoArtifacts)
    {
        paths.push_back(episodeDir / artifact.fileName);
    }

    paths.push_back(episodeDir / "sensor_data_left.mcap");
    paths.push_back(episodeDir / "sensor_data_right.mcap");
    paths.push_back(episodeDir / "metadata.json");
    paths.push_back(episodeDir / "calibration.json");
    paths.push_back(episodeDir / "info.json");

    const fs::path audioPre = episodeDir / "audio_pre.wav";
    if (fs::exists(audioPre))
    {
        paths.push_back(audioPre);
    }

    const fs::path audioPost = episodeDir / "audio_post.wav";
    if (fs::exists(audioPost))
    {
        paths.push_back(audioPost);
    }

    const int64_t flushStartMs = steadyNowMs();
    std::cout << "[PERF] flush begin: phase=" << phaseLabel
              << " episode_dir=" << episodeDir
              << " candidate_paths=" << paths.size() << std::endl;

    for (const auto &path : paths)
    {
        if (!fs::exists(path))
        {
            std::cout << "[PERF] flush skip missing: path=" << path << std::endl;
            continue;
        }

        const int64_t artifactFlushStartMs = steadyNowMs();
        std::error_code error;
        if (!flushFileToDisk(path, &error))
        {
            std::cerr << "[WARN] failed to flush episode artifact: "
                      << path << " error=" << error.message() << std::endl;
            continue;
        }

        std::cout << "[PERF] flush done: path=" << path
                  << " elapsed_ms=" << (steadyNowMs() - artifactFlushStartMs) << std::endl;
    }

    flushEpisodeDirectoriesToDisk(episodeDir, phaseLabel);

    std::cout << "[PERF] flush end: phase=" << phaseLabel
              << " episode_dir=" << episodeDir
              << " elapsed_ms=" << (steadyNowMs() - flushStartMs) << std::endl;
}

std::string makeTimestampString()
{
    const auto now = std::chrono::system_clock::now();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(nowMs);
}

std::string makeLocalDateTimeString()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&nowTime, &localTime);

    char timeBuffer[32] = {0};
    std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return std::string(timeBuffer);
}

void writeValidationErrorLog(const fs::path &episodeDir, const std::string &errorMessage)
{
    if (episodeDir.empty())
    {
        return;
    }

    const fs::path errorLogPath = episodeDir / "validation_error.log";
    const std::string details = errorMessage.empty() ? "unknown validation error" : errorMessage;
    const std::string content = "Validation failed at " + makeLocalDateTimeString() + ": " + details + "\n";

    std::string writeError;
    if (!writeTextFileAtomically(errorLogPath, content, &writeError))
    {
        std::cerr << "[WARN] failed to write validation_error.log: " << writeError << std::endl;
        return;
    }

    std::error_code flushError;
    if (!flushFileToDisk(errorLogPath, &flushError))
    {
        std::cerr << "[WARN] failed to flush validation_error.log: " << flushError.message() << std::endl;
    }
    flushEpisodeDirectoriesToDisk(episodeDir, "validation_error");
}

bool commandExists(const std::string &command)
{
    if (command.empty())
    {
        return false;
    }
    if (command.find('/') != std::string::npos)
    {
        return access(command.c_str(), X_OK) == 0;
    }

    const char *pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr)
    {
        return false;
    }

    std::stringstream stream(pathEnv);
    std::string directory;
    while (std::getline(stream, directory, ':'))
    {
        if (directory.empty())
        {
            directory = ".";
        }
        const fs::path candidate = fs::path(directory) / command;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> resolvePythonCommand()
{
    if (commandExists("./.venv/bin/python3"))
    {
        return {"./.venv/bin/python3"};
    }
    if (commandExists("uv"))
    {
        return {"uv", "run", "python3"};
    }
    return {"python3"};
}

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string formatSeconds(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

bool parseDoubleStrict(const std::string &text, double *value)
{
    if (value == nullptr)
    {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || errno != 0 || !std::isfinite(parsed))
    {
        return false;
    }

    *value = parsed;
    return true;
}

bool extractJsonNumberField(const std::string &jsonText, const std::string &key, double *value)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?(?:\\d+(?:\\.\\d+)?|\\.\\d+)(?:[eE][+-]?\\d+)?)");
    std::smatch match;
    if (!std::regex_search(jsonText, match, pattern) || match.size() < 2)
    {
        return false;
    }
    return parseDoubleStrict(match[1].str(), value);
}

bool extractJsonIntegerField(const std::string &jsonText, const std::string &key, int64_t *value)
{
    if (value == nullptr)
    {
        return false;
    }

    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch match;
    if (!std::regex_search(jsonText, match, pattern) || match.size() < 2)
    {
        return false;
    }

    const std::string matched = match[1].str();
    char *end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(matched.c_str(), &end, 10);
    if (end == matched.c_str() || *end != '\0' || errno != 0)
    {
        return false;
    }

    *value = static_cast<int64_t>(parsed);
    return true;
}

CommandCaptureResult runCommandCapture(const std::vector<std::string> &arguments, int timeoutMs)
{
    CommandCaptureResult result;
    if (arguments.empty())
    {
        return result;
    }

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
    {
        return result;
    }

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }

    if (pid == 0)
    {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true)
    {
        const pid_t waitResult = waitpid(pid, &status, WNOHANG);
        if (waitResult == pid)
        {
            break;
        }
        if (waitResult < 0)
        {
            close(pipefd[0]);
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            result.timedOut = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::array<char, 512> buffer{};
    ssize_t count = 0;
    while ((count = read(pipefd[0], buffer.data(), buffer.size())) > 0)
    {
        result.output.append(buffer.data(), static_cast<size_t>(count));
    }
    close(pipefd[0]);

    if (result.timedOut)
    {
        return result;
    }

    if (WIFEXITED(status))
    {
        result.exitCode = WEXITSTATUS(status);
        result.success = (result.exitCode == 0);
    }
    return result;
}

std::optional<std::string> extractUsbSerialFromUdevadmOutput(const std::string &output)
{
    const std::regex serialPattern(R"SER(ATTRS\{serial\}=="([^"]+)")SER");
    std::vector<std::string> blockLines;

    const auto inspectBlock = [&blockLines, &serialPattern]() -> std::optional<std::string>
    {
        bool hasUsbSubsystem = false;
        bool hasUsbDriver = false;
        std::string serial;

        for (const std::string &line : blockLines)
        {
            const std::string trimmedLine = trim(line);
            if (trimmedLine.find(R"(SUBSYSTEMS=="usb")") != std::string::npos)
            {
                hasUsbSubsystem = true;
            }
            if (trimmedLine.find(R"(DRIVERS=="usb")") != std::string::npos)
            {
                hasUsbDriver = true;
            }

            std::smatch match;
            if (serial.empty() && std::regex_search(trimmedLine, match, serialPattern) && match.size() >= 2)
            {
                const std::string candidate = match[1].str();
                if (candidate.rfind("xhci-", 0) != 0)
                {
                    serial = candidate;
                }
            }
        }

        if (hasUsbSubsystem && hasUsbDriver && !serial.empty())
        {
            return serial;
        }
        return std::nullopt;
    };

    std::stringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        if (trim(line).empty())
        {
            if (const auto serial = inspectBlock())
            {
                return serial;
            }
            blockLines.clear();
            continue;
        }
        blockLines.push_back(line);
    }

    return inspectBlock();
}

std::optional<std::string> probeUsbSerialForDeviceNode(const std::string &devicePath, std::string *detail)
{
    if (!fs::exists(devicePath))
    {
        if (detail != nullptr)
        {
            *detail = "device node missing";
        }
        return std::nullopt;
    }

    const CommandCaptureResult probe = runCommandCapture(
        {"udevadm", "info", "--attribute-walk", "--name=" + devicePath},
        kUdevadmProbeTimeoutMs);

    if (probe.timedOut)
    {
        if (detail != nullptr)
        {
            *detail = "udevadm timed out after " + std::to_string(kUdevadmProbeTimeoutMs) + "ms";
        }
        return std::nullopt;
    }
    if (!probe.success)
    {
        if (detail != nullptr)
        {
            std::string commandError = trim(probe.output);
            if (commandError.empty())
            {
                commandError = "udevadm exited with code " + std::to_string(probe.exitCode);
            }
            *detail = commandError;
        }
        return std::nullopt;
    }

    const auto serial = extractUsbSerialFromUdevadmOutput(probe.output);
    if (!serial.has_value() && detail != nullptr)
    {
        *detail = "no parent usb ATTRS{serial} found";
    }
    return serial;
}

void injectRuntimeTactileSerial(json *calibrationJson,
                                const TactileCalibrationTarget &target,
                                const std::string &runtimeSerial,
                                const std::string &probeDetail,
                                const std::string &sourceFile,
                                const std::string &persistCalibrationFile)
{
    if (calibrationJson == nullptr || !calibrationJson->is_object())
    {
        return;
    }

    json &entry = (*calibrationJson)[target.jsonPath];
    if (!entry.is_object())
    {
        entry = json::object();
    }

    const std::string previous = entry.value("serial", std::string());
    if (previous.empty())
    {
        std::cout << "[INFO] tactile serial missing in source calibration, inject runtime value:"
                  << " camera=" << target.cameraName
                  << " device=" << target.devicePath
                  << " serial=" << runtimeSerial << std::endl;
    }
    else if (previous == target.serialPlaceholder)
    {
        std::cout << "[INFO] tactile serial placeholder resolved at runtime:"
                  << " camera=" << target.cameraName
                  << " placeholder=" << target.serialPlaceholder
                  << " device=" << target.devicePath
                  << " serial=" << runtimeSerial << std::endl;
    }
    else if (previous != runtimeSerial)
    {
        std::cerr << "[WARN] tactile serial mismatch, correcting episode calibration only:"
                  << " camera=" << target.cameraName
                  << " source_serial=" << previous
                  << " runtime_serial=" << runtimeSerial
                  << " source_file=" << sourceFile
                  << " persist_rewritten=false" << std::endl;
    }
    else
    {
        std::cout << "[INFO] tactile serial matches runtime hardware:"
                  << " camera=" << target.cameraName
                  << " device=" << target.devicePath
                  << " serial=" << runtimeSerial << std::endl;
    }

    if (!probeDetail.empty())
    {
        std::cout << "[INFO] tactile serial probe detail:"
                  << " camera=" << target.cameraName
                  << " detail=" << probeDetail << std::endl;
    }

    replaceJsonStringValues(calibrationJson, target.serialPlaceholder, runtimeSerial);
    entry["serial"] = runtimeSerial;

    if (sourceFile == persistCalibrationFile &&
        previous != runtimeSerial &&
        !previous.empty() &&
        previous != target.serialPlaceholder)
    {
        std::cout << "[INFO] persist calibration remains unchanged;"
                  << " episode calibration now uses runtime tactile serial for camera=" << target.cameraName
                  << std::endl;
    }
}

bool probeVideoFile(const std::string &filePath, VideoProbeResult *result, std::string *errorMessage)
{
    if (result == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "internal error: missing probe output slot";
        }
        return false;
    }

    const int64_t probeStartMs = steadyNowMs();
    std::cout << "[PERF] ffprobe begin: file=" << filePath << std::endl;
    const CommandCaptureResult probe = runCommandCapture(
        {
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_type:format=start_time,duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            filePath,
        },
        kVideoProbeTimeoutMs);

    if (probe.timedOut)
    {
        std::cerr << "[PERF] ffprobe timeout: file=" << filePath
                  << " elapsed_ms=" << (steadyNowMs() - probeStartMs) << std::endl;
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe timeout after " + std::to_string(kVideoProbeTimeoutMs) + "ms";
        }
        return false;
    }
    if (!probe.success)
    {
        std::cerr << "[PERF] ffprobe failed: file=" << filePath
                  << " elapsed_ms=" << (steadyNowMs() - probeStartMs)
                  << " exit_code=" << probe.exitCode << std::endl;
        if (errorMessage != nullptr)
        {
            std::string detail = trim(probe.output);
            if (detail.empty())
            {
                detail = "ffprobe exited with code " + std::to_string(probe.exitCode);
            }
            *errorMessage = detail;
        }
        return false;
    }

    std::vector<std::string> lines;
    std::stringstream stream(probe.output);
    std::string line;
    while (std::getline(stream, line))
    {
        line = trim(line);
        if (!line.empty())
        {
            lines.push_back(line);
        }
    }

    if (lines.size() < 3 || lines[0] != "video")
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe did not report a readable video stream";
        }
        return false;
    }

    if (lines[1] == "N/A" || lines[2] == "N/A" ||
        !parseDoubleStrict(lines[1], &result->startTimeSec) ||
        !parseDoubleStrict(lines[2], &result->durationSec))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe returned invalid start_time/duration";
        }
        return false;
    }

    result->spanSec = result->durationSec;
    if (result->startTimeSec > 0.0 && result->durationSec > result->startTimeSec)
    {
        result->spanSec = result->durationSec - result->startTimeSec;
    }

    if (!std::isfinite(result->spanSec) || result->spanSec <= 0.0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe returned non-positive video span";
        }
        return false;
    }

    std::cout << "[PERF] ffprobe done: file=" << filePath
              << " elapsed_ms=" << (steadyNowMs() - probeStartMs)
              << " start_sec=" << formatSeconds(result->startTimeSec)
              << " duration_sec=" << formatSeconds(result->durationSec)
              << " span_sec=" << formatSeconds(result->spanSec) << std::endl;
    return true;
}

bool loadLastTopicLogTimeFromTailChunks(const std::string &mcapPath,
                                        const std::string &topic,
                                        uint64_t *lastLogTimeNs,
                                        uint64_t *messageCount,
                                        std::string *errorMessage)
{
    if (lastLogTimeNs == nullptr || messageCount == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "internal error: missing encoder tail result slot";
        }
        return false;
    }

    *lastLogTimeNs = 0;
    *messageCount = 0;

    mcap::McapReader reader;
    const auto openStatus = reader.open(mcapPath);
    if (!openStatus.ok())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to open mcap: " + openStatus.message;
        }
        return false;
    }

    std::string summaryProblem;
    const auto summaryStatus = reader.readSummary(
        mcap::ReadSummaryMethod::AllowFallbackScan,
        [&summaryProblem](const mcap::Status &status) {
            if (summaryProblem.empty())
            {
                summaryProblem = status.message;
            }
        });
    if (!summaryStatus.ok())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to read mcap summary: " + summaryStatus.message;
            if (!summaryProblem.empty())
            {
                *errorMessage += " (" + summaryProblem + ")";
            }
        }
        return false;
    }

    std::vector<mcap::ChannelId> matchingChannels;
    for (const auto &[channelId, channel] : reader.channels())
    {
        if (channel != nullptr && channel->topic == topic)
        {
            matchingChannels.push_back(channelId);
        }
    }

    if (matchingChannels.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "missing topic in mcap: " + topic;
        }
        return false;
    }

    if (reader.statistics().has_value())
    {
        for (const auto channelId : matchingChannels)
        {
            const auto countIt = reader.statistics()->channelMessageCounts.find(channelId);
            if (countIt != reader.statistics()->channelMessageCounts.end())
            {
                *messageCount += countIt->second;
            }
        }
    }

    bool found = false;
    auto *dataSource = reader.dataSource();
    const auto &chunkIndexes = reader.chunkIndexes();
    if (dataSource != nullptr && !chunkIndexes.empty())
    {
        size_t scannedChunks = 0;
        for (auto it = chunkIndexes.rbegin();
             it != chunkIndexes.rend() && scannedChunks < kEncoderTailChunkScanLimit && !found;
             ++it, ++scannedChunks)
        {
            mcap::TypedRecordReader recordReader(
                *dataSource,
                it->chunkStartOffset,
                it->chunkStartOffset + it->chunkLength);
            recordReader.onMessage = [&](const mcap::Message &message,
                                         mcap::ByteOffset,
                                         std::optional<mcap::ByteOffset>) {
                if (std::find(matchingChannels.begin(), matchingChannels.end(), message.channelId) ==
                    matchingChannels.end())
                {
                    return;
                }

                if (!found || message.logTime > *lastLogTimeNs)
                {
                    *lastLogTimeNs = message.logTime;
                    found = true;
                }
            };

            while (recordReader.next())
            {
            }

            if (!recordReader.status().ok() && errorMessage != nullptr && errorMessage->empty())
            {
                *errorMessage = "failed to read tail chunk: " + recordReader.status().message;
            }
        }
    }

    if (!found)
    {
        std::string readProblem;
        mcap::ReadMessageOptions options;
        options.topicFilter = [&topic](std::string_view candidateTopic) {
            return candidateTopic == topic;
        };

        for (const auto &messageView : reader.readMessages(
                 [&readProblem](const mcap::Status &status) {
                     if (readProblem.empty())
                     {
                         readProblem = status.message;
                     }
                 },
                 options))
        {
            if (!found || messageView.message.logTime > *lastLogTimeNs)
            {
                *lastLogTimeNs = messageView.message.logTime;
                found = true;
            }
        }

        if (!found && !readProblem.empty() && errorMessage != nullptr && errorMessage->empty())
        {
            *errorMessage = "failed to scan mcap messages: " + readProblem;
        }
    }

    if (!found)
    {
        if (errorMessage != nullptr && errorMessage->empty())
        {
            *errorMessage = "topic has zero messages: " + topic;
        }
        return false;
    }

    if (*messageCount == 0)
    {
        *messageCount = 1;
    }
    return true;
}
}

GripperLedEffect RecordRuntime::makeLedEffect(LedState state, double progress)
{
    switch (state)
    {
    case RecordRuntime::LedState::Init:
        return {GripperLedEffectState::Init, progress};
    case RecordRuntime::LedState::Ready:
        return {GripperLedEffectState::Ready, progress};
    case RecordRuntime::LedState::Recording:
        return {GripperLedEffectState::Recording, progress};
    case RecordRuntime::LedState::Error1:
        return {GripperLedEffectState::Error1, progress};
    case RecordRuntime::LedState::Error2:
        return {GripperLedEffectState::Error2, progress};
    case RecordRuntime::LedState::Error3:
        return {GripperLedEffectState::Error3, progress};
    case RecordRuntime::LedState::Error4:
        return {GripperLedEffectState::Error4, progress};
    case RecordRuntime::LedState::Error5:
        return {GripperLedEffectState::Error5, progress};
    case RecordRuntime::LedState::CalibPre:
        return {GripperLedEffectState::CalibPre, progress};
    case RecordRuntime::LedState::CalibRun:
        return {GripperLedEffectState::CalibRun, progress};
    case RecordRuntime::LedState::CalibDone:
        return {GripperLedEffectState::CalibDone, progress};
    case RecordRuntime::LedState::Exit:
    default:
        return {GripperLedEffectState::Exit, progress};
    }
}

RecordRuntime::RecordRuntime(RecordRuntimeOptions options)
    : options_(std::move(options))
{
}

RecordRuntime::~RecordRuntime()
{
    requestStop();
    stopRecording(false, "shutdown");
    stopMotionAlertPipe();
    stopAudioPlayer();
    if (ledController_)
    {
        ledController_->stop();
    }
    panelManager_.silenceBeep();
    panelManager_.turnOff();
    panelManager_.disconnect();
}

bool RecordRuntime::initialize()
{
    deviceSn_ = readEnvValue(options_.envFile, "DEVICE_SN");
    if (deviceSn_.empty())
    {
        deviceSn_ = "noname_device";
    }

    language_ = toLower(readEnvValue(options_.envFile, "UGRIPPER_LANG"));
    if (language_.empty())
    {
        language_ = "zh";
    }

    options_.cameraCodec = toLower(readEnvValue(options_.envFile, "CAMERA_CODEC"));
    if (options_.cameraCodec.empty())
    {
        options_.cameraCodec = "h264";
    }
    if (options_.cameraCodec != "h264" && options_.cameraCodec != "h265")
    {
        std::cerr << "[WARN] invalid CAMERA_CODEC, fallback to h264: " << options_.cameraCodec << std::endl;
        options_.cameraCodec = "h264";
    }

    if (options_.gripperPorts.empty())
    {
        options_.gripperPorts.emplace_back("/dev/right_gripper");
        options_.gripperPorts.emplace_back("/dev/left_gripper");
    }

    std::error_code error;
    fs::create_directories(options_.audioTempDir, error);

    packageVersion_ = queryPackageVersion("ugripper");
    updaterVersion_ = queryPackageVersion("ugripper-usb-updater");

    episodeManager_ = std::make_unique<EpisodeManager>(
        options_.diskRoot,
        deviceSn_,
        language_,
        options_.cameraCodec,
        options_.persistCalibrationFile,
        options_.exampleCalibrationFile,
        options_.fallbackCalibrationFile,
        packageVersion_,
        updaterVersion_);

    if (!episodeManager_->initialize())
    {
        std::cerr << "[ERROR] failed to initialize episode manager for disk root: "
                  << options_.diskRoot << std::endl;
        return false;
    }

    if (!panelManager_.connect(options_.gripperPorts))
    {
        std::cerr << "[ERROR] failed to connect any gripper HMI port" << std::endl;
        return false;
    }

    ledController_ = std::make_unique<HmiLedController>(&panelManager_);
    ledController_->start();
    setLedState(LedState::Init);

    startAudioPlayer();
    startStereoDaemon();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    setLedState(LedState::Ready);
    setAudioRecoveryCommand("ready");
    sendAudioCommand("ready");

    initialized_ = true;
    return true;
}

int RecordRuntime::run()
{
    if (!initialized_)
    {
        return 1;
    }

    std::cout << "[INFO] record_runtime started" << std::endl;
    std::cout << "[INFO] device_sn=" << deviceSn_ << std::endl;
    std::cout << "[INFO] episode_root=" << episodeManager_->dataRoot() << std::endl;

    while (!stopRequested_.load())
    {
        const uint64_t loopStartMs = currentSteadyMs();
        maintainAudioPlayer();
        maintainStereoDaemon();
        monitorHardwareHealth();
        pollMotionAlertPipe();

        ButtonSnapshot buttons;
        panelManager_.poll(options_.pollMs, &buttons);
        handleButtons(buttons);

        if (isRecording_ && !checkRecorderProcesses())
        {
            stopRecording(true, "camera or sensor recorder exited unexpectedly");
        }

        const uint64_t loopElapsedMs = currentSteadyMs() - loopStartMs;
        if (loopElapsedMs < static_cast<uint64_t>(options_.pollMs))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(options_.pollMs) - std::chrono::milliseconds(loopElapsedMs));
        }
    }

    stopRecording(false, "shutdown");
    stopStereoDaemon();
    setLedState(LedState::Exit);
    return 0;
}

void RecordRuntime::requestStop()
{
    stopRequested_.store(true);
}

bool RecordRuntime::startMotionAlertPipe(int *writeFd)
{
    stopMotionAlertPipe();
    if (writeFd != nullptr)
    {
        *writeFd = -1;
    }

    int pipefd[2] = {-1, -1};
#ifdef __linux__
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        return false;
    }
#else
    if (pipe(pipefd) != 0)
    {
        return false;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL, 0) | O_NONBLOCK);
#endif

    motionAlertReadFd_ = pipefd[0];
    if (writeFd != nullptr)
    {
        *writeFd = pipefd[1];
    }
    else
    {
        close(pipefd[1]);
    }
    return true;
}

void RecordRuntime::stopMotionAlertPipe()
{
    if (motionAlertReadFd_ >= 0)
    {
        close(motionAlertReadFd_);
        motionAlertReadFd_ = -1;
    }
    clearMotionAlertOutputs();
}

void RecordRuntime::clearMotionAlertOutputs()
{
    motionAlertOutputState_ = {};
    panelManager_.silenceBeep();
}

void RecordRuntime::applyMotionAlertState(const std::string &side, bool active)
{
    bool *currentState = nullptr;
    if (side == "left")
    {
        currentState = &motionAlertOutputState_.leftAlertActive;
    }
    else if (side == "right")
    {
        currentState = &motionAlertOutputState_.rightAlertActive;
    }
    if (currentState == nullptr || *currentState == active)
    {
        return;
    }

    const bool applied = active ? panelManager_.setBeepEnabledForSide(side, true)
                                : panelManager_.silenceBeepForSide(side);
    if (applied)
    {
        *currentState = active;
    }
}

void RecordRuntime::pollMotionAlertPipe()
{
    if (motionAlertReadFd_ < 0)
    {
        return;
    }

    while (true)
    {
        ugripper::MotionAlertMessage message{};
        const ssize_t bytesRead = read(motionAlertReadFd_, &message, sizeof(message));
        if (bytesRead == 0)
        {
            stopMotionAlertPipe();
            return;
        }
        if (bytesRead < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            stopMotionAlertPipe();
            return;
        }
        if (static_cast<size_t>(bytesRead) != sizeof(message) ||
            message.magic != ugripper::kMotionAlertMessageMagic ||
            message.version != ugripper::kMotionAlertMessageVersion)
        {
            continue;
        }

        const std::string side = motionAlertSideName(message.side);
        if (side == "unknown")
        {
            continue;
        }

        applyMotionAlertState(side, message.active != 0);
        if (message.active != 0)
        {
            std::cerr << "[WARN] motion overspeed detected: side=" << side
                      << " reason=" << motionAlertReasonName(message.reason)
                      << " gyro=" << std::fixed << std::setprecision(3) << message.gyroMagnitude
                      << " accel_excess=" << std::fixed << std::setprecision(3) << message.accelExcess
                      << std::endl;
        }
    }
}

bool RecordRuntime::startAudioPlayer()
{
    if (!fs::exists(options_.audioPlayScript))
    {
        std::cerr << "[WARN] audio play script not found: " << options_.audioPlayScript << std::endl;
        return false;
    }

    std::error_code error;
    fs::remove(options_.audioReadyFile, error);
    lastAudioPlayerStartAttemptMs_ = currentSteadyMs();

    std::vector<std::string> audioArguments = resolvePythonCommand();
    audioArguments.push_back(options_.audioPlayScript);

    if (!audioPlayer_.start(audioArguments))
    {
        std::cerr << "[WARN] failed to launch audio player, sound prompts disabled" << std::endl;
        return false;
    }

    const uint64_t deadlineMs = currentSteadyMs() + 5000;
    while (currentSteadyMs() < deadlineMs)
    {
        if (!audioPlayer_.isRunning())
        {
            std::cerr << "[WARN] audio player exited before ready, last_exit=" << audioPlayer_.lastExitCode() << std::endl;
            audioPlayerStarted_ = false;
            return false;
        }
        if (fs::exists(options_.audioPipe))
        {
            const bool playbackReady = fs::exists(options_.audioReadyFile);
            audioPlayerStarted_ = playbackReady;
            if (!playbackReady)
            {
                std::cout << "[INFO] audio player launched; waiting for USB headset sink" << std::endl;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cerr << "[WARN] audio player pipe timeout, audio daemon did not finish bootstrap" << std::endl;
    audioPlayer_.stop(1000);
    audioPlayerStarted_ = false;
    return false;
}

void RecordRuntime::stopAudioPlayer()
{
    if (audioPlayerStarted_ && audioPlayer_.isRunning())
    {
        sendAudioCommand("exit");
        audioPlayer_.wait(1000);
    }
    audioPlayer_.stop(1000);
    audioPlayerStarted_ = false;
}

void RecordRuntime::setAudioRecoveryCommand(std::string command)
{
    audioRecoveryCommand_ = std::move(command);
}

void RecordRuntime::maintainAudioPlayer()
{
    const uint64_t nowMs = currentSteadyMs();
    const bool pipeReady = fs::exists(options_.audioPipe);
    const bool readyMarker = fs::exists(options_.audioReadyFile);

    if (audioPlayer_.isRunning())
    {
        if (!pipeReady)
        {
            if ((nowMs - lastAudioPlayerStartAttemptMs_) < kAudioPlayerReadyGraceMs)
            {
                return;
            }

            std::cerr << "[WARN] audio player running without pipe, restarting" << std::endl;
            audioPlayer_.stop(1000);
        }

        if (readyMarker)
        {
            const bool recovered = !audioPlayerStarted_;
            audioPlayerStarted_ = true;
            if (recovered)
            {
                std::cout << "[INFO] audio player recovered and is ready" << std::endl;
                if (initialized_ && !audioRecoveryCommand_.empty())
                {
                    sendAudioCommand(audioRecoveryCommand_);
                }
            }
            return;
        }

        if (audioPlayerStarted_)
        {
            std::cerr << "[WARN] audio player lost ready marker, waiting for USB headset recovery" << std::endl;
        }
        audioPlayerStarted_ = false;
        return;
    }

    if (audioPlayerStarted_)
    {
        std::cerr << "[WARN] audio player exited, will retry" << std::endl;
        audioPlayerStarted_ = false;
    }

    if ((nowMs - lastAudioPlayerStartAttemptMs_) < kAudioPlayerRestartIntervalMs)
    {
        return;
    }

    startAudioPlayer();
}

bool RecordRuntime::startStereoDaemon()
{
    std::error_code error;
    fs::remove(options_.stereoStatusFile, error);
    fs::remove(options_.stereoControlFile, error);
    lastStereoDaemonStartAttemptMs_ = currentSteadyMs();

    if (!writeStereoControl(false, "", 0, 0))
    {
        std::cerr << "[WARN] failed to reset stereo control file before launch" << std::endl;
    }

    const std::vector<std::string> args = {
        options_.cameraRecorderBin,
        "--stereo-daemon",
        "--codec",
        options_.cameraCodec,
        "--control-file",
        options_.stereoControlFile,
        "--status-file",
        options_.stereoStatusFile,
        "--only",
        kWarmupCameraStreamsCsv,
    };

    if (!stereoDaemon_.start(args))
    {
        std::cerr << "[WARN] failed to launch stereo daemon" << std::endl;
        stereoDaemonStarted_ = false;
        return false;
    }

    stereoDaemonStarted_ = true;
    return true;
}

void RecordRuntime::stopStereoDaemon()
{
    if (stereoDaemonStarted_)
    {
        writeStereoControl(false, "", 0, 0);
    }
    stereoDaemon_.stop(2000);
    stereoDaemonStarted_ = false;
}

void RecordRuntime::maintainStereoDaemon()
{
    const uint64_t nowMs = currentSteadyMs();
    if (stereoDaemon_.isRunning())
    {
        stereoDaemonStarted_ = true;
        return;
    }

    if (stereoDaemonStarted_)
    {
        std::cerr << "[WARN] stereo daemon exited, will retry" << std::endl;
        stereoDaemonStarted_ = false;
    }

    if ((nowMs - lastStereoDaemonStartAttemptMs_) < kStereoDaemonRestartIntervalMs)
    {
        return;
    }
    startStereoDaemon();
}

bool RecordRuntime::writeStereoControl(bool recording,
                                       const std::string &episodeDir,
                                       int64_t startSystemTimeUs,
                                       int64_t stopSystemTimeUs)
{
    json root = json::object();
    root["command_seq"] = ++stereoCommandSeq_;
    root["recording"] = recording;
    root["episode_dir"] = episodeDir;
    root["start_system_time_us"] = startSystemTimeUs;
    root["stop_system_time_us"] = stopSystemTimeUs;

    std::string errorMessage;
    if (!writeTextFileAtomically(options_.stereoControlFile, root.dump(2) + "\n", &errorMessage))
    {
        std::cerr << "[WARN] failed to write stereo control file: " << errorMessage << std::endl;
        return false;
    }
    return true;
}

bool RecordRuntime::waitForStereoFinalize(const std::string &episodeDir, int timeoutMs, std::string *errorMessage)
{
    const uint64_t startMs = currentSteadyMs();
    while ((currentSteadyMs() - startMs) < static_cast<uint64_t>(timeoutMs))
    {
        json status;
        bool statusLoaded = false;
        std::ifstream input(options_.stereoStatusFile);
        if (input.is_open())
        {
            try
            {
                status = json::parse(input);
                statusLoaded = true;
            }
            catch (const std::exception &)
            {
                statusLoaded = false;
            }
        }

        if (statusLoaded)
        {
            const std::string lastFinalizeError = status.value("last_finalize_error", std::string());
            const bool finalizePending = status.value("finalize_pending", false);
            const std::string activeEpisodeDir = status.value("active_episode_dir", std::string());
            const std::string lastFinalizedEpisodeDir = status.value("last_finalized_episode_dir", std::string());
            if (!lastFinalizeError.empty() && !finalizePending &&
                (activeEpisodeDir.empty() || activeEpisodeDir == episodeDir))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = lastFinalizeError;
                }
                return false;
            }

            if (status.contains("last_session") && status["last_session"].is_object())
            {
                const json &lastSession = status["last_session"];
                if (lastSession.value("episode_dir", std::string()) == episodeDir)
                {
                    return true;
                }
            }

            if (!finalizePending && lastFinalizedEpisodeDir == episodeDir)
            {
                if (errorMessage != nullptr && errorMessage->empty())
                {
                    *errorMessage = "stereo daemon finalized the session without session metadata";
                }
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kStereoFinalizeWaitPollMs));
    }
    if (errorMessage != nullptr && errorMessage->empty())
    {
        *errorMessage = "timed out waiting for stereo session metadata";
    }
    return false;
}

bool RecordRuntime::mergeEpisodeInfo(const std::string &episodeDir, std::string *errorMessage) const
{
    const fs::path baseInfoPath = fs::path(episodeDir) / "info.json";

    std::ifstream baseInput(baseInfoPath);
    if (!baseInput.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open base info.json";
        }
        return false;
    }

    json baseInfo;
    try
    {
        baseInfo = json::parse(baseInput);
    }
    catch (const std::exception &ex)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("failed to parse episode info: ") + ex.what();
        }
        return false;
    }

    std::ifstream statusInput(options_.stereoStatusFile);
    if (!statusInput.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open stereo status file";
        }
        return false;
    }

    json status;
    try
    {
        status = json::parse(statusInput);
    }
    catch (const std::exception &ex)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("failed to parse stereo status: ") + ex.what();
        }
        return false;
    }

    if (!status.contains("last_session") || !status["last_session"].is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "stereo status missing last_session";
        }
        return false;
    }

    const json &stereoSession = status["last_session"];
    if (stereoSession.value("episode_dir", std::string()) != episodeDir)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "stereo last_session does not match episode";
        }
        return false;
    }

    if (!stereoSession.contains("cameras") || !stereoSession["cameras"].is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "stereo last_session missing cameras object";
        }
        return false;
    }

    for (const char *cameraName : {"left_stereo", "right_stereo"})
    {
        if (!stereoSession["cameras"].contains(cameraName))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("stereo last_session missing camera entry: ") + cameraName;
            }
            return false;
        }
        const json &cameraInfo = stereoSession["cameras"][cameraName];
        if (!cameraInfo.contains("record_time_offset_us"))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("stereo camera info missing record_time_offset_us: ") + cameraName;
            }
            return false;
        }
        baseInfo[std::string(cameraName) + "_record_time_offset_us"] = cameraInfo["record_time_offset_us"];
    }

    json stereoOnlySession = stereoSession;
    stereoOnlySession["cameras"] = json::object();
    for (const char *cameraName : {"left_stereo", "right_stereo"})
    {
        stereoOnlySession["cameras"][cameraName] = stereoSession["cameras"][cameraName];
    }
    baseInfo["stereo_session"] = stereoOnlySession;

    std::ofstream output(baseInfoPath, std::ios::trunc);
    if (!output.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot rewrite merged info.json";
        }
        return false;
    }
    output << baseInfo.dump(2) << '\n';
    return true;
}

bool RecordRuntime::syncRuntimeLogToDisk(const char *reason) const
{
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);

    const std::string deviceSnLower = toLower(deviceSn_.empty() ? "unknown_device" : deviceSn_);
    const std::string filePrefix = "umi_sys_" + deviceSnLower + "_";
    const fs::path localLogDir("/tmp");
    fs::path sourcePath;
    fs::file_time_type newestWriteTime{};
    bool foundSource = false;

    std::error_code iterError;
    for (fs::directory_iterator it(localLogDir, iterError), end; !iterError && it != end; it.increment(iterError))
    {
        const fs::directory_entry &entry = *it;
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string fileName = entry.path().filename().string();
        if (fileName.size() <= filePrefix.size() + 4 ||
            fileName.compare(0, filePrefix.size(), filePrefix) != 0 ||
            fileName.compare(fileName.size() - 4, 4, ".log") != 0)
        {
            continue;
        }

        std::error_code timeError;
        const fs::file_time_type writeTime = entry.last_write_time(timeError);
        if (timeError)
        {
            continue;
        }

        if (!foundSource || writeTime > newestWriteTime)
        {
            sourcePath = entry.path();
            newestWriteTime = writeTime;
            foundSource = true;
        }
    }

    if (iterError)
    {
        std::cerr << "[WARN] failed to scan runtime log directory for sync";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << iterError.message() << std::endl;
        return false;
    }

    if (!foundSource)
    {
        std::cerr << "[WARN] no runtime log file found for sync";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": prefix=" << filePrefix << std::endl;
        return false;
    }

    const fs::path diskRoot(options_.diskRoot);
    std::error_code diskRootError;
    if (!fs::exists(diskRoot, diskRootError) || !fs::is_directory(diskRoot, diskRootError))
    {
        std::cerr << "[WARN] skip runtime log sync";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": disk root unavailable: " << diskRoot << std::endl;
        return false;
    }

    bool mounted = false;
    std::ifstream mountInfo("/proc/self/mountinfo");
    if (mountInfo.is_open())
    {
        std::string line;
        while (std::getline(mountInfo, line))
        {
            const size_t separator = line.find(" - ");
            const std::string leftPart = separator == std::string::npos ? line : line.substr(0, separator);
            std::istringstream fields(leftPart);
            std::string mountId;
            std::string parentId;
            std::string majorMinor;
            std::string root;
            std::string mountPoint;
            if (!(fields >> mountId >> parentId >> majorMinor >> root >> mountPoint))
            {
                continue;
            }
            if (mountPoint == diskRoot.string())
            {
                mounted = true;
                break;
            }
        }
    }

    if (!mounted)
    {
        std::cerr << "[WARN] skip runtime log sync";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": disk root is not mounted: " << diskRoot << std::endl;
        return false;
    }

    if (access(diskRoot.c_str(), W_OK) != 0)
    {
        std::cerr << "[WARN] skip runtime log sync";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": disk root not writable: " << diskRoot
                  << " error=" << std::strerror(errno) << std::endl;
        return false;
    }

    const fs::path destDir = diskRoot / "logs";
    std::error_code mkdirError;
    fs::create_directories(destDir, mkdirError);
    if (mkdirError)
    {
        std::cerr << "[WARN] failed to create runtime log directory";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << destDir << " error=" << mkdirError.message() << std::endl;
        return false;
    }

    const fs::path destPath = destDir / sourcePath.filename();
    const std::string sourceStem = sourcePath.stem().string();
    const fs::path statePath = fs::path("/tmp") / (sourceStem + ".pos");
    const fs::path lockPath = fs::path("/tmp") / (sourceStem + ".lock");

    const int lockFd = open(lockPath.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (lockFd < 0)
    {
        std::cerr << "[WARN] failed to open runtime log sync lock";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << lockPath << " error=" << std::strerror(errno) << std::endl;
        return false;
    }

    const auto closeLock = [&]() {
        flock(lockFd, LOCK_UN);
        close(lockFd);
    };

    if (flock(lockFd, LOCK_EX) != 0)
    {
        std::cerr << "[WARN] failed to acquire runtime log sync lock";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << lockPath << " error=" << std::strerror(errno) << std::endl;
        close(lockFd);
        return false;
    }

    int64_t lastPos = 0;
    {
        std::ifstream stateInput(statePath);
        if (stateInput.is_open())
        {
            stateInput >> lastPos;
            if (!stateInput.good() && !stateInput.eof())
            {
                lastPos = 0;
            }
        }
    }
    if (lastPos < 0)
    {
        lastPos = 0;
    }

    const int srcFd = open(sourcePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (srcFd < 0)
    {
        std::cerr << "[WARN] failed to open runtime log source";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << sourcePath << " error=" << std::strerror(errno) << std::endl;
        closeLock();
        return false;
    }

    struct stat sourceStat
    {
    };
    if (fstat(srcFd, &sourceStat) != 0)
    {
        std::cerr << "[WARN] failed to stat runtime log source";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << sourcePath << " error=" << std::strerror(errno) << std::endl;
        close(srcFd);
        closeLock();
        return false;
    }

    int64_t currentSize = static_cast<int64_t>(sourceStat.st_size);
    if (currentSize < lastPos)
    {
        lastPos = 0;
    }

    if (currentSize <= lastPos)
    {
        close(srcFd);
        closeLock();
        std::cout << "[INFO] runtime log already synced";
        if (reason != nullptr && *reason != '\0')
        {
            std::cout << " after " << reason;
        }
        std::cout << ": " << destPath << std::endl;
        return true;
    }

    const int destFd = open(destPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (destFd < 0)
    {
        std::cerr << "[WARN] failed to open runtime log destination";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << destPath << " error=" << std::strerror(errno) << std::endl;
        close(srcFd);
        closeLock();
        return false;
    }

    if (lseek(srcFd, static_cast<off_t>(lastPos), SEEK_SET) < 0)
    {
        std::cerr << "[WARN] failed to seek runtime log source";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << sourcePath << " error=" << std::strerror(errno) << std::endl;
        close(destFd);
        close(srcFd);
        closeLock();
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    int64_t remaining = currentSize - lastPos;
    while (remaining > 0)
    {
        const size_t chunkSize = static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(buffer.size())));
        const ssize_t readSize = read(srcFd, buffer.data(), chunkSize);
        if (readSize < 0)
        {
            std::cerr << "[WARN] failed to read runtime log source";
            if (reason != nullptr && *reason != '\0')
            {
                std::cerr << " after " << reason;
            }
            std::cerr << ": " << sourcePath << " error=" << std::strerror(errno) << std::endl;
            close(destFd);
            close(srcFd);
            closeLock();
            return false;
        }
        if (readSize == 0)
        {
            break;
        }

        ssize_t totalWritten = 0;
        while (totalWritten < readSize)
        {
            const ssize_t written = write(destFd, buffer.data() + totalWritten, static_cast<size_t>(readSize - totalWritten));
            if (written <= 0)
            {
                std::cerr << "[WARN] failed to append runtime log destination";
                if (reason != nullptr && *reason != '\0')
                {
                    std::cerr << " after " << reason;
                }
                std::cerr << ": " << destPath << " error=" << std::strerror(errno) << std::endl;
                close(destFd);
                close(srcFd);
                closeLock();
                return false;
            }
            totalWritten += written;
        }

        remaining -= readSize;
    }

    if (::fsync(destFd) != 0)
    {
        std::cerr << "[WARN] failed to flush runtime log destination";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << destPath << " error=" << std::strerror(errno) << std::endl;
        close(destFd);
        close(srcFd);
        closeLock();
        return false;
    }

    close(destFd);
    close(srcFd);

    std::string stateWriteError;
    if (!writeTextFileAtomically(statePath, std::to_string(currentSize), &stateWriteError))
    {
        std::cerr << "[WARN] failed to update runtime log sync state";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << stateWriteError << std::endl;
        closeLock();
        return false;
    }

    std::error_code flushError;
    if (!flushDirectoryToDisk(destDir, &flushError))
    {
        std::cerr << "[WARN] failed to flush runtime log directory";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << destDir << " error=" << flushError.message() << std::endl;
    }
    if (!flushDirectoryToDisk(diskRoot, &flushError))
    {
        std::cerr << "[WARN] failed to flush disk root after runtime log sync";
        if (reason != nullptr && *reason != '\0')
        {
            std::cerr << " after " << reason;
        }
        std::cerr << ": " << diskRoot << " error=" << flushError.message() << std::endl;
    }

    closeLock();

    std::cout << "[INFO] synced runtime log to disk";
    if (reason != nullptr && *reason != '\0')
    {
        std::cout << " after " << reason;
    }
    std::cout << ": src=" << sourcePath
              << " dest=" << destPath
              << " bytes=" << (currentSize - lastPos) << std::endl;
    return true;
}

void RecordRuntime::sendAudioCommand(const std::string &command) const
{
    if (command.empty() || !audioPlayerStarted_)
    {
        return;
    }

    const int fd = open(options_.audioPipe.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
        if (command != "exit")
        {
            std::cerr << "[WARN] failed to open audio pipe for command '" << command << "': " << std::strerror(errno) << std::endl;
        }
        return;
    }

    const std::string payload = command + "\n";
    const ssize_t written = write(fd, payload.data(), payload.size());
    if (written < 0)
    {
        std::cerr << "[WARN] failed to write audio command '" << command << "': " << std::strerror(errno) << std::endl;
    }
    close(fd);
}

bool RecordRuntime::attachPendingPreAudio(const std::string &episodeDir)
{
    if (pendingPreAudioFile_.empty() || !fs::exists(pendingPreAudioFile_))
    {
        return true;
    }

    std::error_code error;
    const fs::path target = fs::path(episodeDir) / "audio_pre.wav";
    if (!moveFileWithCrossDeviceFallback(pendingPreAudioFile_, target, &error))
    {
        std::cerr << "[WARN] failed to move pre audio into episode: " << error.message() << std::endl;
        return false;
    }

    if (!flushFileToDisk(target, &error))
    {
        std::cerr << "[WARN] failed to flush pre audio into episode: " << error.message() << std::endl;
    }

    pendingPreAudioFile_.clear();
    return true;
}

bool RecordRuntime::startRecording(bool resetRecording)
{
    currentEpisodeDir_ = episodeManager_->createNextEpisodeDir();
    if (currentEpisodeDir_.empty())
    {
        std::cerr << "[ERROR] failed to create episode directory" << std::endl;
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    std::string errorMessage;
    const std::string resetSource = resetRecording ? lastEpisodeDir_ : std::string();
    if (!episodeManager_->prepareEpisode(currentEpisodeDir_, resetRecording, resetSource, &errorMessage))
    {
        std::cerr << "[ERROR] prepare episode failed: " << errorMessage << std::endl;
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    attachPendingPreAudio(currentEpisodeDir_);

    if (!fs::exists(options_.cameraRecorderBin))
    {
        std::cerr << "[ERROR] camera recorder binary not found: "
                  << options_.cameraRecorderBin << std::endl;
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }
    if (!fs::exists(options_.sensorRecorderBin))
    {
        std::cerr << "[ERROR] sensor recorder binary not found: "
                  << options_.sensorRecorderBin << std::endl;
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    const int64_t sessionStartSystemTimeUs = currentEpochUs();

    const std::vector<std::string> cameraArgs = {
        options_.cameraRecorderBin,
        "--codec",
        options_.cameraCodec,
        "--output-dir",
        currentEpisodeDir_,
        "--only",
        kSessionCameraStreamsCsv,
    };
    const std::vector<std::string> sensorArgs = {
        options_.sensorRecorderBin,
        currentEpisodeDir_,
    };
    int motionAlertWriteFd = -1;
    if (!startMotionAlertPipe(&motionAlertWriteFd))
    {
        std::cerr << "[ERROR] failed to create motion alert pipe" << std::endl;
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    bool cameraStarted = false;
    bool sensorStarted = false;

    std::thread cameraThread([&]() {
        cameraStarted = cameraRecorder_.start(cameraArgs);
    });
    std::thread sensorThread([&]() {
        std::vector<std::string> sensorArgsWithAlert = sensorArgs;
        if (motionAlertWriteFd >= 0)
        {
            sensorArgsWithAlert.push_back("--motion-alert-fd");
            sensorArgsWithAlert.push_back(std::to_string(motionAlertWriteFd));
        }
        sensorStarted = sensorRecorder_.start(sensorArgsWithAlert, {motionAlertWriteFd});
        if (motionAlertWriteFd >= 0)
        {
            close(motionAlertWriteFd);
            motionAlertWriteFd = -1;
        }
    });

    cameraThread.join();
    sensorThread.join();

    if (!cameraStarted || !sensorStarted)
    {
        if (!cameraStarted)
        {
            std::cerr << "[ERROR] failed to launch camera recorder" << std::endl;
        }
        if (!sensorStarted)
        {
            std::cerr << "[ERROR] failed to launch sensor recorder" << std::endl;
        }

        if (cameraStarted)
        {
            cameraRecorder_.stop(2000);
        }
        if (sensorStarted)
        {
            sensorRecorder_.stop(2000);
        }
        if (motionAlertWriteFd >= 0)
        {
            close(motionAlertWriteFd);
            motionAlertWriteFd = -1;
        }
        stopMotionAlertPipe();

        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    if (!writeStereoControl(true, currentEpisodeDir_, sessionStartSystemTimeUs, 0))
    {
        std::cerr << "[ERROR] failed to start stereo session for episode " << currentEpisodeDir_ << std::endl;
        cameraRecorder_.stop(2000);
        sensorRecorder_.stop(2000);
        stopMotionAlertPipe();
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    isRecording_ = true;
    setLedState(LedState::Recording);
    setAudioRecoveryCommand("");
    sendAudioCommand(resetRecording ? "reset_recording_start" : "recording_started");
    std::cout << "[INFO] recording started: " << currentEpisodeDir_ << std::endl;
    return true;
}

bool RecordRuntime::stopRecording(bool dueToError, const std::string &reason)
{
    if (!isRecording_)
    {
        if (dueToError)
        {
            setLedState(LedState::Error5);
            setAudioRecoveryCommand("error");
            sendAudioCommand("error");
        }
        return true;
    }

    const int64_t stopStartMs = steadyNowMs();
    const int64_t stopSystemTimeUs = currentEpochUs();
    std::cout << "[INFO] stopping recording: " << reason << std::endl;
    std::cout << "[PERF] stop phase begin: reason=" << reason
              << " episode_dir=" << currentEpisodeDir_ << std::endl;

    bool stereoStopOk = writeStereoControl(false, currentEpisodeDir_, 0, stopSystemTimeUs);
    if (!stereoStopOk)
    {
        std::cerr << "[ERROR] failed to send stereo stop command" << std::endl;
    }

    const int64_t cameraStopStartMs = steadyNowMs();
    const bool cameraStopOk = cameraRecorder_.stop(5000);
    std::cout << "[PERF] stop camera_recorder done: ok=" << (cameraStopOk ? "true" : "false")
              << " elapsed_ms=" << (steadyNowMs() - cameraStopStartMs) << std::endl;

    const int64_t sensorStopStartMs = steadyNowMs();
    const bool sensorStopOk = sensorRecorder_.stop(5000);
    stopMotionAlertPipe();
    std::cout << "[PERF] stop sensor_recorder done: ok=" << (sensorStopOk ? "true" : "false")
              << " elapsed_ms=" << (steadyNowMs() - sensorStopStartMs) << std::endl;

    setLedState(LedState::Ready);
    sendAudioCommand("recording_stop");

    isRecording_ = false;
    if (!currentEpisodeDir_.empty())
    {
        lastEpisodeDir_ = currentEpisodeDir_;
    }

    setAudioRecoveryCommand("writing");
    sendAudioCommand("writing");
    setLedState(LedState::Init);
    const int64_t writePhaseStartMs = steadyNowMs();
    std::cout << "[PERF] writing phase begin: episode_dir=" << currentEpisodeDir_ << std::endl;
    const fs::path episodePath(currentEpisodeDir_);
    flushEpisodeArtifactsToDisk(episodePath, "pre_stereo_finalize");
    std::string stereoFinalizeError;
    std::string mergeError;
    if (stereoStopOk && !waitForStereoFinalize(currentEpisodeDir_, 10000, &stereoFinalizeError))
    {
        std::cerr << "[ERROR] stereo finalize failed: " << stereoFinalizeError << std::endl;
        stereoStopOk = false;
    }
    if (stereoStopOk)
    {
        if (!mergeEpisodeInfo(currentEpisodeDir_, &mergeError))
        {
            std::cerr << "[ERROR] failed to merge episode info: " << mergeError << std::endl;
            stereoStopOk = false;
        }
    }
    flushEpisodeArtifactsToDisk(episodePath, "final");
    std::cout << "[PERF] writing phase end: episode_dir=" << currentEpisodeDir_
              << " elapsed_ms=" << (steadyNowMs() - writePhaseStartMs) << std::endl;

    std::string errorMessage;
    const int64_t validatePhaseStartMs = steadyNowMs();
    std::cout << "[PERF] validation phase begin: episode_dir=" << currentEpisodeDir_ << std::endl;
    std::string episodeValidationError;
    const bool episodeValid = episodeManager_->validateEpisode(currentEpisodeDir_, &episodeValidationError);
    if (!stereoStopOk)
    {
        if (!stereoFinalizeError.empty())
        {
            errorMessage = stereoFinalizeError;
        }
        else if (!mergeError.empty())
        {
            errorMessage = mergeError;
        }
        else
        {
            errorMessage = "stereo session finalize failed";
        }
    }
    if (!episodeValid)
    {
        if (errorMessage.empty())
        {
            errorMessage = episodeValidationError.empty() ? "episode validation failed" : episodeValidationError;
        }
        else if (!episodeValidationError.empty())
        {
            errorMessage += "; " + episodeValidationError;
        }
    }
    const bool valid = stereoStopOk && episodeValid;
    std::cout << "[PERF] validation phase end: episode_dir=" << currentEpisodeDir_
              << " valid=" << (valid ? "true" : "false")
              << " elapsed_ms=" << (steadyNowMs() - validatePhaseStartMs) << std::endl;
    if (!valid)
    {
        std::cerr << "[ERROR] episode validation failed: " << errorMessage << std::endl;
        writeValidationErrorLog(episodePath, errorMessage);
    }

    currentEpisodeDir_.clear();

    if (dueToError)
    {
        setLedState(LedState::Error5);
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        std::cout << "[PERF] stop phase end: due_to_error=true"
                  << " total_elapsed_ms=" << (steadyNowMs() - stopStartMs) << std::endl;
        syncRuntimeLogToDisk("video stop");
        return false;
    }

    if (!valid)
    {
        setLedState(LedState::Error1);
        setAudioRecoveryCommand("validation_failed");
        sendAudioCommand("validation_failed");
        std::cout << "[PERF] stop phase end: valid=false"
                  << " total_elapsed_ms=" << (steadyNowMs() - stopStartMs) << std::endl;
        syncRuntimeLogToDisk("video stop");
        return false;
    }

    setLedState(LedState::Ready);
    setAudioRecoveryCommand("ready");
    sendAudioCommand("ready");
    std::cout << "[PERF] stop phase end: valid=true"
              << " total_elapsed_ms=" << (steadyNowMs() - stopStartMs) << std::endl;
    syncRuntimeLogToDisk("video stop");
    return true;
}

bool RecordRuntime::handleShortUpAction()
{
    std::cout << "[INFO] BTN_UP short press" << std::endl;
    if (!isRecording_)
    {
        if (healthStatus_ == HealthStatus::Error)
        {
            std::cout << "[WARN] ignore start recording while hardware health is in error state" << std::endl;
            sendAudioCommand("error");
            return false;
        }
        return startRecording(false);
    }
    return stopRecording(false, "BTN_UP short stop");
}

bool RecordRuntime::handleShortDownAction()
{
    std::cout << "[INFO] BTN_DOWN short press" << std::endl;
    if (!isRecording_)
    {
        if (healthStatus_ == HealthStatus::Error)
        {
            std::cout << "[WARN] ignore reset recording while hardware health is in error state" << std::endl;
            sendAudioCommand("error");
            return false;
        }
        if (lastEpisodeDir_.empty() || !fs::exists(lastEpisodeDir_))
        {
            std::cout << "[INFO] no previous episode, BTN_DOWN reset ignored" << std::endl;
            setAudioRecoveryCommand("ready");
            sendAudioCommand("no_reset_needed");
            setLedState(LedState::Ready);
            return false;
        }
        return startRecording(true);
    }
    return stopRecording(false, "BTN_DOWN short stop");
}

bool RecordRuntime::handleLongUpAction()
{
    std::cout << "[INFO] BTN_UP long press" << std::endl;
    if (isRecording_)
    {
        std::cout << "[WARN] ignore pre-audio while recording" << std::endl;
        return false;
    }
    return recordAudioClip("pre", true);
}

bool RecordRuntime::handleLongDownAction()
{
    std::cout << "[INFO] BTN_DOWN long press" << std::endl;
    if (isRecording_)
    {
        std::cout << "[WARN] ignore post-audio while recording" << std::endl;
        return false;
    }
    return recordAudioClip("post", false);
}

void RecordRuntime::handleDualShutdownAction()
{
    std::cout << "[INFO] dual-button shutdown requested" << std::endl;
    if (isRecording_)
    {
        stopRecording(false, "dual-button shutdown");
    }
    setLedState(LedState::Exit);
    setAudioRecoveryCommand("writing");
    sendAudioCommand("writing");

    std::ofstream requestFile(options_.shutdownRequestFile, std::ios::trunc);
    if (!requestFile.is_open())
    {
        std::cerr << "[ERROR] failed to create shutdown request file: " << options_.shutdownRequestFile << std::endl;
        setLedState(LedState::Error5);
        return;
    }
    requestFile << "shutdown\n";
    requestFile.close();
    stopRequested_.store(true);
}

bool RecordRuntime::recordAudioClip(const std::string &audioType, bool monitorUpButton)
{
    if (!fs::exists(options_.audioRecordScript))
    {
        std::cerr << "[ERROR] audio record script not found: " << options_.audioRecordScript << std::endl;
        setAudioRecoveryCommand("error");
        sendAudioCommand("error");
        return false;
    }

    const std::string timestamp = makeTimestampString();
    const fs::path tempDir(options_.audioTempDir);
    const fs::path captureFile = tempDir / ("audio_" + audioType + "_" + timestamp + "_capture.wav");
    const fs::path ch1File = tempDir / ("audio_" + audioType + "_" + timestamp + "_ch1.wav");
    const fs::path ch1Denoised = tempDir / ("audio_" + audioType + "_" + timestamp + "_ch1_denoised.wav");
    const fs::path outputFile = tempDir / ("audio_" + audioType + "_" + timestamp + ".wav");

    sendAudioCommand(audioType == "pre" ? "pre_audio_recording" : "post_audio_recording");

    ProcessRunner audioRecorder("audio_recorder");
    std::vector<std::string> audioRecorderArgs = resolvePythonCommand();
    audioRecorderArgs.push_back(options_.audioRecordScript);
    audioRecorderArgs.push_back("--output");
    audioRecorderArgs.push_back(captureFile.string());
    if (!audioRecorder.start(audioRecorderArgs))
    {
        std::cerr << "[ERROR] failed to start audio recorder" << std::endl;
        sendAudioCommand("audio_recording_stop");
        return false;
    }

    const auto finishAudioRecording = [&](bool ok) {
        sendAudioCommand("audio_recording_stop");
        syncRuntimeLogToDisk(audioType == "pre" ? "pre-audio stop" : "post-audio stop");
        return ok;
    };

    while (!stopRequested_.load())
    {
        ButtonSnapshot buttons;
        panelManager_.poll(options_.pollMs, &buttons);
        const bool stillPressed = monitorUpButton ? buttons.upPressed : buttons.downPressed;
        if (!stillPressed)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    audioRecorder.sendSignal(SIGINT);
    if (!audioRecorder.wait(5000))
    {
        audioRecorder.stop(1000);
    }

    if (!fileExistsAndNotEmpty(captureFile.string()) || fs::file_size(captureFile) <= 44)
    {
        std::cerr << "[WARN] audio capture is empty" << std::endl;
        fs::remove(captureFile);
        return finishAudioRecording(false);
    }

    bool ok = false;
    if (runCommandSync({"sox", captureFile.string(), ch1File.string(), "remix", "1"}))
    {
        if (fs::exists(options_.noiseProfile) &&
            runCommandSync({"sox", ch1File.string(), ch1Denoised.string(), "noisered", options_.noiseProfile, "0.15"}) &&
            runCommandSync({"sox", ch1Denoised.string(), outputFile.string(), "norm"}))
        {
            ok = true;
        }
        else if (runCommandSync({"sox", ch1File.string(), outputFile.string(), "norm"}))
        {
            ok = true;
        }
    }

    fs::remove(captureFile);
    fs::remove(ch1File);
    fs::remove(ch1Denoised);

    if (!ok || !fileExistsAndNotEmpty(outputFile.string()))
    {
        std::cerr << "[ERROR] failed to process audio clip" << std::endl;
        fs::remove(outputFile);
        return finishAudioRecording(false);
    }

    if (audioType == "pre")
    {
        pendingPreAudioFile_ = outputFile.string();
        std::cout << "[INFO] pre-audio prepared: " << pendingPreAudioFile_ << std::endl;
    }
    else
    {
        if (!lastEpisodeDir_.empty() && fs::exists(lastEpisodeDir_))
        {
            std::error_code error;
            if (!moveFileWithCrossDeviceFallback(outputFile, fs::path(lastEpisodeDir_) / "audio_post.wav", &error))
            {
                std::cerr << "[ERROR] failed to move post-audio into last episode: " << error.message() << std::endl;
                fs::remove(outputFile);
            }
            else if (!flushFileToDisk(fs::path(lastEpisodeDir_) / "audio_post.wav", &error))
            {
                std::cerr << "[WARN] failed to flush post-audio into last episode: " << error.message() << std::endl;
            }
        }
        else
        {
            std::cerr << "[WARN] no previous episode for post-audio" << std::endl;
            fs::remove(outputFile);
        }
    }

    return finishAudioRecording(true);
}

void RecordRuntime::handleButtons(const ButtonSnapshot &buttons)
{
    const uint64_t nowMs = currentSteadyMs();

    if (buttons.upPressed && !lastButtons_.upPressed)
    {
        buttonTracker_.upPressedSinceMs = nowMs;
        buttonTracker_.upLongHandled = false;
    }
    if (buttons.downPressed && !lastButtons_.downPressed)
    {
        buttonTracker_.downPressedSinceMs = nowMs;
        buttonTracker_.downLongHandled = false;
    }

    if (!buttons.upPressed)
    {
        buttonTracker_.upPressedSinceMs = 0;
    }
    if (!buttons.downPressed)
    {
        buttonTracker_.downPressedSinceMs = 0;
    }

    if (buttons.upPressed && buttons.downPressed)
    {
        if (!buttonTracker_.dualChordActive)
        {
            buttonTracker_.dualChordActive = true;
            buttonTracker_.bothPressedSinceMs = nowMs;
            buttonTracker_.dualLongHandled = false;
            buttonTracker_.shutdownPromptPlayed = false;
            std::cout << "[INFO] dual-button chord armed" << std::endl;
        }

        const uint64_t dualHeldMs = nowMs - buttonTracker_.bothPressedSinceMs;
        if (dualHeldMs >= kShutdownPromptThresholdMs && !buttonTracker_.shutdownPromptPlayed)
        {
            buttonTracker_.shutdownPromptPlayed = true;
            sendAudioCommand("shutdown");
        }
        if (dualHeldMs >= kDualLongPressThresholdMs && !buttonTracker_.dualLongHandled)
        {
            buttonTracker_.dualLongHandled = true;
            lastButtonActionMs_ = nowMs;
            handleDualShutdownAction();
        }

        lastButtons_ = buttons;
        return;
    }

    if (buttonTracker_.dualChordActive)
    {
        if (!buttons.upPressed && !buttons.downPressed)
        {
            buttonTracker_.dualChordActive = false;
            buttonTracker_.bothPressedSinceMs = 0;
            buttonTracker_.dualLongHandled = false;
            buttonTracker_.shutdownPromptPlayed = false;
        }
        lastButtons_ = buttons;
        return;
    }

    if (buttons.upPressed && !buttonTracker_.upLongHandled && buttonTracker_.upPressedSinceMs > 0 &&
        (nowMs - buttonTracker_.upPressedSinceMs) >= kLongPressThresholdMs)
    {
        buttonTracker_.upLongHandled = true;
        lastButtonActionMs_ = nowMs;
        handleLongUpAction();
    }

    if (buttons.downPressed && !buttonTracker_.downLongHandled && buttonTracker_.downPressedSinceMs > 0 &&
        (nowMs - buttonTracker_.downPressedSinceMs) >= kLongPressThresholdMs)
    {
        buttonTracker_.downLongHandled = true;
        lastButtonActionMs_ = nowMs;
        handleLongDownAction();
    }

    const bool upReleased = lastButtons_.upPressed && !buttons.upPressed;
    const bool downReleased = lastButtons_.downPressed && !buttons.downPressed;

    if (upReleased && !buttonTracker_.upLongHandled && (nowMs - lastButtonActionMs_) >= kActionDebounceMs)
    {
        lastButtonActionMs_ = nowMs;
        handleShortUpAction();
    }

    if (downReleased && !buttonTracker_.downLongHandled && (nowMs - lastButtonActionMs_) >= kActionDebounceMs)
    {
        lastButtonActionMs_ = nowMs;
        handleShortDownAction();
    }

    if (upReleased)
    {
        buttonTracker_.upLongHandled = false;
    }
    if (downReleased)
    {
        buttonTracker_.downLongHandled = false;
    }

    lastButtons_ = buttons;
}

bool RecordRuntime::checkRecorderProcesses()
{
    const bool cameraOk = cameraRecorder_.isRunning();
    const bool sensorOk = sensorRecorder_.isRunning();
    if (!cameraOk)
    {
        std::cerr << "[ERROR] camera recorder exited, last_exit=" << cameraRecorder_.lastExitCode() << std::endl;
    }
    if (!sensorOk)
    {
        std::cerr << "[ERROR] sensor recorder exited, last_exit=" << sensorRecorder_.lastExitCode() << std::endl;
    }
    return cameraOk && sensorOk;
}

std::optional<RecordRuntime::HealthFault> RecordRuntime::evaluateHardwareHealth() const
{
    if (access(options_.diskRoot.c_str(), W_OK) != 0)
    {
        return HealthFault{
            LedState::Error1,
            "disk_not_writable",
            "Disk not writable or mount lost: " + options_.diskRoot,
        };
    }

    std::vector<std::string> missingDevicePaths;
    for (const char *path : kCriticalDevicePaths)
    {
        if (path == nullptr || *path == '\0')
        {
            continue;
        }
        if (!fs::exists(path))
        {
            missingDevicePaths.emplace_back(path);
        }
    }
    if (!missingDevicePaths.empty())
    {
        return HealthFault{
            LedState::Error2,
            "critical_devices_missing",
            "Critical device nodes missing: " + joinStrings(missingDevicePaths, ", "),
        };
    }

    if (stereoDaemon_.pid() <= 0)
    {
        return HealthFault{
            LedState::Error2,
            "stereo_daemon_not_running",
            "Stereo warmup daemon is not running",
        };
    }

    std::ifstream stereoStatusInput(options_.stereoStatusFile);
    if (!stereoStatusInput.is_open())
    {
        return HealthFault{
            LedState::Error2,
            "stereo_status_missing",
            "Stereo status file missing: " + options_.stereoStatusFile,
        };
    }
    try
    {
        json stereoStatus = json::parse(stereoStatusInput);
        if (!stereoStatus.value("ready", false))
        {
            return HealthFault{
                LedState::Error2,
                "stereo_not_ready",
                "Stereo warmup not ready: " + stereoStatus.value("service_state", std::string("unknown")),
            };
        }
    }
    catch (const std::exception &ex)
    {
        return HealthFault{
            LedState::Error2,
            "stereo_status_invalid",
            std::string("Stereo status invalid: ") + ex.what(),
        };
    }

    const auto hmiHealth = panelManager_.getHealthSnapshot(kHmiActiveTimeoutMs);
    if (!hmiHealth.hasConnectedDevice)
    {
        return HealthFault{
            LedState::Error2,
            "hmi_all_disconnected",
            "All HMI ports are disconnected",
        };
    }
    if (!hmiHealth.disconnectedPorts.empty())
    {
        return HealthFault{
            LedState::Error2,
            "hmi_ports_disconnected",
            "HMI ports disconnected: " + joinStrings(hmiHealth.disconnectedPorts, ", "),
        };
    }
    if (!hmiHealth.inputConnected)
    {
        return HealthFault{
            LedState::Error2,
            "hmi_input_disconnected",
            "Input HMI port disconnected",
        };
    }
    if (!hmiHealth.inputActive)
    {
        return HealthFault{
            LedState::Error2,
            "hmi_input_inactive",
            "Input HMI port inactive for more than " + std::to_string(kHmiActiveTimeoutMs) + "ms",
        };
    }
    if (!hmiHealth.inactivePorts.empty())
    {
        return HealthFault{
            LedState::Error2,
            "hmi_ports_inactive",
            "HMI ports inactive: " + joinStrings(hmiHealth.inactivePorts, ", "),
        };
    }

    return std::nullopt;
}

void RecordRuntime::monitorHardwareHealth()
{
    const uint64_t nowMs = currentSteadyMs();
    if ((nowMs - lastHealthCheckMs_) < kHealthCheckIntervalMs)
    {
        return;
    }
    lastHealthCheckMs_ = nowMs;

    const auto fault = evaluateHardwareHealth();
    if (fault.has_value())
    {
        const std::string errorKey = fault->key + "|" + fault->detail;
        if (healthStatus_ != HealthStatus::Error || lastHealthErrorKey_ != errorKey)
        {
            std::cerr << "[ERROR] hardware health fault (" << fault->key << "): "
                      << fault->detail << std::endl;
            setLedState(fault->ledState);
            sendAudioCommand("error");
        }
        healthStatus_ = HealthStatus::Error;
        lastHealthErrorKey_ = errorKey;
        return;
    }

    if (healthStatus_ == HealthStatus::Error)
    {
        std::cout << "[INFO] hardware health recovered" << std::endl;
        if (isRecording_)
        {
            setLedState(LedState::Recording);
        }
        else
        {
            setLedState(LedState::Ready);
            sendAudioCommand("ready");
        }
    }

    healthStatus_ = HealthStatus::Ok;
    lastHealthErrorKey_.clear();
}

void RecordRuntime::setLedState(LedState state, double progress)
{
    if (ledController_)
    {
        ledController_->setState(state, progress);
    }
}

std::string RecordRuntime::readEnvValue(const std::string &envFile, const std::string &key)
{
    std::ifstream input(envFile);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.rfind(key + "=", 0) != 0)
        {
            continue;
        }
        std::string value = line.substr(key.size() + 1);
        value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        {
            value.pop_back();
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        {
            value.erase(value.begin());
        }
        return value;
    }
    return {};
}

std::string RecordRuntime::queryPackageVersion(const std::string &packageName)
{
    const std::string command = "dpkg-query -W -f='${Version}' " + packageName + " 2>/dev/null";
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
    {
        return "unknown";
    }

    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    pclose(pipe);

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    {
        output.pop_back();
    }

    if (output.empty())
    {
        return "unknown";
    }
    return output;
}

std::string RecordRuntime::toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string RecordRuntime::jsonEscape(const std::string &value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        default:
            result += ch;
            break;
        }
    }
    return result;
}

uint64_t RecordRuntime::currentSteadyMs()
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

uint64_t RecordRuntime::currentEpochMs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

int64_t RecordRuntime::currentEpochUs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

bool RecordRuntime::fileExistsAndNotEmpty(const std::string &path)
{
    std::error_code error;
    return fs::exists(path, error) && fs::is_regular_file(path, error) && fs::file_size(path, error) > 0;
}

bool RecordRuntime::runCommandSync(const std::vector<std::string> &arguments)
{
    if (arguments.empty())
    {
        return false;
    }

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

const char *RecordRuntime::motionAlertSideName(uint8_t side)
{
    switch (static_cast<ugripper::MotionAlertSide>(side))
    {
    case ugripper::MotionAlertSide::Left:
        return "left";
    case ugripper::MotionAlertSide::Right:
        return "right";
    case ugripper::MotionAlertSide::Unknown:
    default:
        return "unknown";
    }
}

const char *RecordRuntime::motionAlertReasonName(uint8_t reason)
{
    switch (static_cast<ugripper::MotionAlertReasonCode>(reason))
    {
    case ugripper::MotionAlertReasonCode::Gyro:
        return "gyro";
    case ugripper::MotionAlertReasonCode::Accel:
        return "accel";
    case ugripper::MotionAlertReasonCode::AccelAndGyro:
        return "accel+gyro";
    case ugripper::MotionAlertReasonCode::Recovered:
        return "recovered";
    case ugripper::MotionAlertReasonCode::None:
    default:
        return "none";
    }
}

RecordRuntime::ProcessRunner::ProcessRunner(std::string name)
    : name_(std::move(name))
{
}

RecordRuntime::ProcessRunner::~ProcessRunner()
{
    stop(1000);
}

bool RecordRuntime::ProcessRunner::start(const std::vector<std::string> &arguments,
                                        const std::vector<int> &inheritedFileDescriptors)
{
    if (arguments.empty())
    {
        return false;
    }

    stop(1000);

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
    {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::cout << "[INFO] launching " << name_ << ": " << joinArguments(arguments) << std::endl;

    pid_ = fork();
    if (pid_ < 0)
    {
        perror("fork");
        pid_ = -1;
        return false;
    }

    if (pid_ == 0)
    {
        setpgid(0, 0);
        for (int fd : inheritedFileDescriptors)
        {
            if (fd < 0)
            {
                continue;
            }
            const int currentFlags = fcntl(fd, F_GETFD);
            if (currentFlags >= 0)
            {
                fcntl(fd, F_SETFD, currentFlags & ~FD_CLOEXEC);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (setpgid(pid_, pid_) != 0 && errno != EACCES)
    {
        std::cerr << "[WARN] failed to assign process group for " << name_
                  << ": " << std::strerror(errno) << std::endl;
    }

    processGroupId_ = pid_;
    lastExitCode_ = 0;
    return true;
}

bool RecordRuntime::ProcessRunner::isRunning()
{
    if (pid_ <= 0)
    {
        return false;
    }
    if (pollExit(false))
    {
        return false;
    }
    return true;
}

bool RecordRuntime::ProcessRunner::wait(int timeoutMs)
{
    if (pid_ <= 0)
    {
        return true;
    }

    const uint64_t startMs = RecordRuntime::currentSteadyMs();
    while ((RecordRuntime::currentSteadyMs() - startMs) < static_cast<uint64_t>(timeoutMs))
    {
        if (pollExit(false))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool RecordRuntime::ProcessRunner::sendSignal(int signalNumber)
{
    const pid_t target = signalTarget();
    if (target == 0)
    {
        return false;
    }
    return kill(target, signalNumber) == 0;
}

bool RecordRuntime::ProcessRunner::stop(int timeoutMs)
{
    if (pid_ <= 0)
    {
        return true;
    }

    if (!sendSignal(SIGTERM) && errno != ESRCH)
    {
        std::cerr << "[WARN] failed to send SIGTERM to " << name_
                  << ": " << std::strerror(errno) << std::endl;
    }
    if (wait(timeoutMs))
    {
        return true;
    }

    std::cerr << "[WARN] " << name_ << " did not exit after SIGTERM, escalating to SIGKILL" << std::endl;
    if (!sendSignal(SIGKILL) && errno != ESRCH)
    {
        std::cerr << "[WARN] failed to send SIGKILL to " << name_
                  << ": " << std::strerror(errno) << std::endl;
    }
    pollExit(true);
    return pid_ <= 0;
}

void RecordRuntime::ProcessRunner::reset()
{
    pid_ = -1;
    processGroupId_ = -1;
    lastExitCode_ = 0;
}

int RecordRuntime::ProcessRunner::lastExitCode() const
{
    return lastExitCode_;
}

int RecordRuntime::ProcessRunner::pid() const
{
    return pid_;
}

pid_t RecordRuntime::ProcessRunner::signalTarget() const
{
    if (processGroupId_ > 0)
    {
        return -processGroupId_;
    }
    if (pid_ > 0)
    {
        return pid_;
    }
    return 0;
}

bool RecordRuntime::ProcessRunner::pollExit(bool blocking)
{
    if (pid_ <= 0)
    {
        return true;
    }

    int status = 0;
    const int flags = blocking ? 0 : WNOHANG;
    const pid_t waitResult = waitpid(pid_, &status, flags);
    if (waitResult == 0)
    {
        return false;
    }
    if (waitResult < 0)
    {
        return false;
    }

    if (WIFEXITED(status))
    {
        lastExitCode_ = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        lastExitCode_ = 128 + WTERMSIG(status);
    }
    pid_ = -1;
    processGroupId_ = -1;
    return true;
}

bool RecordRuntime::GripperPanelManager::connect(const std::vector<std::string> &ports)
{
    disconnect();
    drivers_.clear();
    reconnectAttemptMs_.clear();
    inputDriverIndex_ = 0;
    hasDedicatedRightInput_ = false;
    hasLedEffect_ = false;
    hasDirectLedColor_ = false;
    hasBeepState_ = false;

    for (size_t index = 0; index < ports.size(); ++index)
    {
        auto driver = std::make_unique<GripperHmiDriver>(ports[index], 115200, "RecordRuntimeHmi" + std::to_string(index));
        if (!hasDedicatedRightInput_ && ports[index].find("right_gripper") != std::string::npos)
        {
            inputDriverIndex_ = index;
            hasDedicatedRightInput_ = true;
        }
        if (!driver->connect())
        {
            std::cerr << "[WARN] failed to connect HMI port: " << ports[index] << std::endl;
        }
        drivers_.push_back(std::move(driver));
        reconnectAttemptMs_.push_back(0);
        currentDriverBeepStates_.push_back({});
    }

    return hasConnectedDevice();
}

void RecordRuntime::GripperPanelManager::disconnect()
{
    drivers_.clear();
    reconnectAttemptMs_.clear();
    currentDriverBeepStates_.clear();
}

bool RecordRuntime::GripperPanelManager::hasConnectedDevice() const
{
    for (const auto &driver : drivers_)
    {
        if (driver != nullptr && driver->isConnected())
        {
            return true;
        }
    }
    return false;
}

RecordRuntime::GripperPanelManager::HealthSnapshot RecordRuntime::GripperPanelManager::getHealthSnapshot(uint64_t activeTimeoutMs) const
{
    HealthSnapshot health;

    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        const auto &driver = drivers_[index];
        if (driver == nullptr)
        {
            continue;
        }

        const bool connected = driver->isConnected();
        if (connected)
        {
            health.hasConnectedDevice = true;
        }
        else
        {
            health.disconnectedPorts.push_back(driver->getPort());
        }

        if (index == inputDriverIndex_)
        {
            health.inputConnected = connected;
        }

        if (!connected)
        {
            continue;
        }

        const auto snapshot = driver->getSnapshot(activeTimeoutMs);
        if (!snapshot.active)
        {
            health.inactivePorts.push_back(driver->getPort());
        }
        if (index == inputDriverIndex_)
        {
            health.inputActive = snapshot.active;
        }
    }

    return health;
}

bool RecordRuntime::GripperPanelManager::setBeepState(const GripperBeepState &state)
{
    currentBeepState_ = state;
    hasBeepState_ = true;
    bool wroteAny = false;
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        currentDriverBeepStates_[index] = state;
        auto &driver = drivers_[index];
        if (driver != nullptr && driver->isConnected())
        {
            wroteAny = driver->setBeepState(state) || wroteAny;
        }
    }
    return wroteAny;
}

bool RecordRuntime::GripperPanelManager::setBeepEnabled(bool enabled)
{
    return enabled ? setBeepState(GripperBeepState{
                         GripperHmiDriver::kDefaultBeepDuty,
                         GripperHmiDriver::kDefaultBeepFrequency,
                     })
                   : silenceBeep();
}

bool RecordRuntime::GripperPanelManager::silenceBeep()
{
    return setBeepState(GripperBeepState{0, 0});
}

bool RecordRuntime::GripperPanelManager::setBeepStateForSide(const std::string &side, const GripperBeepState &state)
{
    bool wroteAny = false;
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        auto &driver = drivers_[index];
        if (driver == nullptr)
        {
            continue;
        }
        const std::string &port = driver->getPort();
        const bool match =
            (side == "right" && port.find("right_gripper") != std::string::npos) ||
            (side == "left" && port.find("left_gripper") != std::string::npos);
        if (!match)
        {
            continue;
        }
        currentDriverBeepStates_[index] = state;
        if (driver->isConnected())
        {
            wroteAny = driver->setBeepState(state) || wroteAny;
        }
    }
    return wroteAny;
}

bool RecordRuntime::GripperPanelManager::setBeepEnabledForSide(const std::string &side, bool enabled)
{
    return enabled ? setBeepStateForSide(side, GripperBeepState{
                                             GripperHmiDriver::kDefaultBeepDuty,
                                             GripperHmiDriver::kDefaultBeepFrequency,
                                         })
                   : silenceBeepForSide(side);
}

bool RecordRuntime::GripperPanelManager::silenceBeepForSide(const std::string &side)
{
    return setBeepStateForSide(side, GripperBeepState{0, 0});
}

void RecordRuntime::GripperPanelManager::maybeReconnectDriver(size_t index)
{
    if (index >= drivers_.size() || drivers_[index] == nullptr)
    {
        return;
    }
    if (drivers_[index]->isConnected())
    {
        return;
    }

    const uint64_t nowMs = RecordRuntime::currentSteadyMs();
    if (index < reconnectAttemptMs_.size() &&
        reconnectAttemptMs_[index] != 0 &&
        (nowMs - reconnectAttemptMs_[index]) < kReconnectIntervalMs)
    {
        return;
    }

    if (index < reconnectAttemptMs_.size())
    {
        reconnectAttemptMs_[index] = nowMs;
    }

    if (!drivers_[index]->connect())
    {
        return;
    }

    std::cout << "[INFO] reconnected HMI port: " << drivers_[index]->getPort() << std::endl;
    if (index < currentDriverBeepStates_.size())
    {
        drivers_[index]->setBeepState(currentDriverBeepStates_[index]);
    }
    else if (hasBeepState_)
    {
        drivers_[index]->setBeepState(currentBeepState_);
    }
    if (hasDirectLedColor_)
    {
        drivers_[index]->setLedColor(currentLedColor_);
    }
    else if (hasLedEffect_)
    {
        drivers_[index]->setLedEffect(currentLedEffect_);
    }
}

bool RecordRuntime::GripperPanelManager::poll(int timeoutMs, ButtonSnapshot *snapshot)
{
    ButtonSnapshot current;
    bool received = false;
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        auto &driver = drivers_[index];
        maybeReconnectDriver(index);
        if (!driver->isConnected())
        {
            continue;
        }
        if (index == inputDriverIndex_)
        {
            // Only the designated input side is allowed to consume the poll budget.
            received = driver->pollOnce(timeoutMs) || received;
        }
        if (index != inputDriverIndex_)
        {
            continue;
        }

        const auto state = driver->getSnapshot();
        if (state.keyPressed.size() > kBtnUpKeyIndex)
        {
            current.upPressed = state.keyPressed[kBtnUpKeyIndex];
        }
        if (state.keyPressed.size() > kBtnDownKeyIndex)
        {
            current.downPressed = state.keyPressed[kBtnDownKeyIndex];
        }
    }

    if (snapshot != nullptr)
    {
        *snapshot = current;
    }
    return received;
}

void RecordRuntime::GripperPanelManager::setLedColor(uint8_t red, uint8_t green, uint8_t blue)
{
    currentLedColor_ = GripperLedColor{red, green, blue};
    hasDirectLedColor_ = true;
    hasLedEffect_ = false;
    for (auto &driver : drivers_)
    {
        if (driver != nullptr)
        {
            driver->setLedColor(red, green, blue);
        }
    }
}

void RecordRuntime::GripperPanelManager::setLedEffect(const GripperLedEffect &effect)
{
    currentLedEffect_ = effect;
    hasLedEffect_ = true;
    hasDirectLedColor_ = false;
    for (auto &driver : drivers_)
    {
        if (driver != nullptr)
        {
            driver->setLedEffect(effect);
        }
    }
}

void RecordRuntime::GripperPanelManager::turnOff()
{
    setLedColor(0, 0, 0);
}

RecordRuntime::HmiLedController::HmiLedController(GripperPanelManager *panelManager)
    : panelManager_(panelManager)
{
}

RecordRuntime::HmiLedController::~HmiLedController()
{
    stop();
}

void RecordRuntime::HmiLedController::start()
{
    if (panelManager_ == nullptr)
    {
        return;
    }
}

void RecordRuntime::HmiLedController::stop()
{
    if (panelManager_ != nullptr)
    {
        panelManager_->turnOff();
    }
}

void RecordRuntime::HmiLedController::setState(LedState state, double progress)
{
    if (panelManager_ == nullptr)
    {
        return;
    }
    panelManager_->setLedEffect(makeLedEffect(state, progress));
}

RecordRuntime::EpisodeManager::EpisodeManager(std::string diskRoot,
                                              std::string deviceSn,
                                              std::string language,
                                              std::string cameraCodec,
                                              std::string persistCalibrationFile,
                                              std::string exampleCalibrationFile,
                                              std::string fallbackCalibrationFile,
                                              std::string packageVersion,
                                              std::string updaterVersion)
    : diskRoot_(std::move(diskRoot)),
      deviceSn_(std::move(deviceSn)),
      deviceSnLower_(RecordRuntime::toLower(deviceSn_)),
      language_(std::move(language)),
      cameraCodec_(std::move(cameraCodec)),
      persistCalibrationFile_(std::move(persistCalibrationFile)),
      exampleCalibrationFile_(std::move(exampleCalibrationFile)),
      fallbackCalibrationFile_(std::move(fallbackCalibrationFile)),
      packageVersion_(std::move(packageVersion)),
      updaterVersion_(std::move(updaterVersion))
{
    dataRoot_ = diskRoot_ + "/" + deviceSnLower_;
    episodeRoot_ = dataRoot_ + "/data";
}

bool RecordRuntime::EpisodeManager::initialize()
{
    std::error_code error;
    fs::create_directories(episodeRoot_, error);
    if (error)
    {
        std::cerr << "[ERROR] create_directories failed for " << episodeRoot_
                  << ": " << error.message() << std::endl;
        return false;
    }
    return true;
}

std::string RecordRuntime::EpisodeManager::createNextEpisodeDir()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&nowTime, &localTime);

    char dateBuffer[16] = {0};
    std::strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &localTime);
    const std::string prefix = std::string("episode_") + dateBuffer + "_";

    int maxId = 0;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(episodeRoot_, error))
    {
        if (error || !entry.is_directory())
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0)
        {
            continue;
        }
        const std::string suffix = name.substr(prefix.size());
        if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
        {
            maxId = std::max(maxId, std::stoi(suffix));
        }
    }

    const int nextId = maxId + 1;
    char idBuffer[16] = {0};
    std::snprintf(idBuffer, sizeof(idBuffer), "%04d", nextId);
    const fs::path episodePath = fs::path(episodeRoot_) / (prefix + idBuffer);
    fs::create_directories(episodePath, error);
    if (error)
    {
        return {};
    }
    flushEpisodeDirectoriesToDisk(episodePath, "create_episode_dir");
    return episodePath.string();
}

bool RecordRuntime::EpisodeManager::prepareEpisode(const std::string &episodeDir,
                                                   bool resetRecording,
                                                   const std::string &resetSourceDir,
                                                   std::string *errorMessage) const
{
    if (!writeMetadata(episodeDir, resetRecording, resetSourceDir, errorMessage))
    {
        return false;
    }
    if (!writeFilteredCalibration(episodeDir, errorMessage))
    {
        return false;
    }
    if (!prepareEpisodeOutputs(episodeDir, errorMessage))
    {
        return false;
    }
    return true;
}

bool RecordRuntime::EpisodeManager::validateEpisode(const std::string &episodeDir, std::string *errorMessage) const
{
    const int64_t validateStartMs = steadyNowMs();
    std::cout << "[PERF] validateEpisode begin: episode_dir=" << episodeDir << std::endl;
    const std::vector<std::string> requiredFiles = {
        "sensor_data_left.mcap",
        "sensor_data_right.mcap",
        "metadata.json",
        "calibration.json",
        "info.json",
    };

    for (const auto &artifact : kEpisodeVideoArtifacts)
    {
        const std::string path = episodeDir + "/" + artifact.fileName;
        if (!RecordRuntime::fileExistsAndNotEmpty(path))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "missing or empty video file: " + path;
            }
            return false;
        }
    }

    for (const auto &file : requiredFiles)
    {
        const std::string path = episodeDir + "/" + file;
        if (!RecordRuntime::fileExistsAndNotEmpty(path))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "missing or empty file: " + path;
            }
            return false;
        }
    }

    if (!commandExists("ffprobe"))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe is required for episode validation but was not found in PATH";
        }
        return false;
    }

    const std::string infoPath = episodeDir + "/info.json";
    std::ifstream infoInput(infoPath);
    if (!infoInput.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open info.json for read";
        }
        return false;
    }
    std::ostringstream infoBuffer;
    infoBuffer << infoInput.rdbuf();
    const std::string infoJson = infoBuffer.str();
    json infoRoot;
    try
    {
        infoRoot = json::parse(infoJson);
    }
    catch (const std::exception &ex)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("failed to parse info.json: ") + ex.what();
        }
        return false;
    }

    double bootTimeOffset = 0.0;
    int64_t bootTimeOffsetUsFromFile = 0;
    if (!extractJsonNumberField(infoJson, "boot_time_offset", &bootTimeOffset) || bootTimeOffset <= 0.0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "info.json missing or invalid numeric field: boot_time_offset";
        }
        return false;
    }
    if (!extractJsonIntegerField(infoJson, "boot_time_offset_us", &bootTimeOffsetUsFromFile) || bootTimeOffsetUsFromFile <= 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "info.json missing or invalid integer field: boot_time_offset_us";
        }
        return false;
    }
    if (std::fabs((bootTimeOffset * 1000000.0) - static_cast<double>(bootTimeOffsetUsFromFile)) > 1.0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "info.json boot_time_offset and boot_time_offset_us are inconsistent";
        }
        return false;
    }

    std::map<std::string, int64_t> recordTimeOffsetUsByCamera;
    for (const auto &artifact : kEpisodeVideoArtifacts)
    {
        int64_t recordTimeOffsetUs = 0;
        const std::string fieldName = std::string(artifact.cameraName) + "_record_time_offset_us";
        if (!extractJsonIntegerField(infoJson, fieldName, &recordTimeOffsetUs) || recordTimeOffsetUs <= 0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "info.json missing or invalid integer field: " + fieldName;
            }
            return false;
        }
        recordTimeOffsetUsByCamera[artifact.cameraName] = recordTimeOffsetUs;
    }

    std::map<std::string, int64_t> explicitTailVideoEndNsByCamera;
    if (infoRoot.contains("stereo_session") && infoRoot["stereo_session"].is_object())
    {
        const json &stereoSession = infoRoot["stereo_session"];
        const int64_t stopSystemTimeUs = stereoSession.value("stop_system_time_us", static_cast<int64_t>(0));
        if (stopSystemTimeUs > 0)
        {
            // Stereo session files are written from the warmup stream and can contain buffered
            // pre-roll around the command window. For encoder tail validation we care about the
            // commanded session end, not the full muxed file duration seen by ffprobe.
            explicitTailVideoEndNsByCamera["left_stereo"] = stopSystemTimeUs * 1000LL;
            explicitTailVideoEndNsByCamera["right_stereo"] = stopSystemTimeUs * 1000LL;
        }
    }

    std::vector<VideoProbeResult> probes;
    probes.reserve(kEpisodeVideoArtifacts.size());
    double referenceSpanSec = 0.0;
    for (const auto &artifact : kEpisodeVideoArtifacts)
    {
        VideoProbeResult probe;
        probe.cameraName = artifact.cameraName;
        probe.fileName = artifact.fileName;

        std::string probeError;
        if (!probeVideoFile(episodeDir + "/" + artifact.fileName, &probe, &probeError))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "video unreadable or missing timing metadata: " + std::string(artifact.fileName) + " (" + probeError + ")";
            }
            return false;
        }

        if (probe.spanSec < kMinReasonableVideoSpanSec)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("video span too short: ") + artifact.fileName +
                                " span=" + formatSeconds(probe.spanSec) +
                                "s, expected >=" + formatSeconds(kMinReasonableVideoSpanSec) + "s";
            }
            return false;
        }

        referenceSpanSec = std::max(referenceSpanSec, probe.spanSec);
        probes.push_back(probe);
    }

    for (const auto &probe : probes)
    {
        const double gapSec = referenceSpanSec - probe.spanSec;
        if (gapSec > kMaxVideoSpanGapSec)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "video span gap too large: " + probe.fileName +
                                " span=" + formatSeconds(probe.spanSec) +
                                "s, reference=" + formatSeconds(referenceSpanSec) +
                                "s, gap=" + formatSeconds(gapSec) + "s";
            }
            return false;
        }
    }

    for (const auto &target : kEncoderTailCheckTargets)
    {
        int64_t tailVideoEndNs = 0;
        for (const auto *cameraName : target.referenceCameraNames)
        {
            const auto explicitEndIt = explicitTailVideoEndNsByCamera.find(cameraName);
            if (explicitEndIt != explicitTailVideoEndNsByCamera.end())
            {
                tailVideoEndNs = std::max(tailVideoEndNs, explicitEndIt->second);
                continue;
            }

            const auto offsetIt = recordTimeOffsetUsByCamera.find(cameraName);
            if (offsetIt == recordTimeOffsetUsByCamera.end())
            {
                continue;
            }

            const auto probeIt = std::find_if(
                probes.begin(),
                probes.end(),
                [cameraName](const VideoProbeResult &probe) {
                    return probe.cameraName == cameraName;
                });
            if (probeIt == probes.end())
            {
                continue;
            }

            const int64_t candidateEndNs =
                offsetIt->second * 1000LL +
                static_cast<int64_t>(std::llround(probeIt->durationSec * 1000000000.0));
            tailVideoEndNs = std::max(tailVideoEndNs, candidateEndNs);
        }

        if (tailVideoEndNs <= 0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("failed to build encoder tail reference for side=") + target.side;
            }
            return false;
        }

        uint64_t lastEncoderLogTimeNs = 0;
        uint64_t encoderMessageCount = 0;
        std::string encoderTailError;
        if (!loadLastTopicLogTimeFromTailChunks(
                episodeDir + "/" + target.mcapFileName,
                target.encoderTopic,
                &lastEncoderLogTimeNs,
                &encoderMessageCount,
                &encoderTailError))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("encoder tail check failed for side=") + target.side +
                                ": " + encoderTailError;
            }
            return false;
        }

        const int64_t tailWindowStartNs = std::max<int64_t>(0, tailVideoEndNs - kEncoderTailWindowNs);
        if (static_cast<int64_t>(lastEncoderLogTimeNs) < tailWindowStartNs)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("encoder tail has no samples near episode end for side=") +
                                target.side +
                                " (last_encoder_ns=" + std::to_string(lastEncoderLogTimeNs) +
                                ", tail_video_start_ns=" + std::to_string(tailWindowStartNs) +
                                ", tail_video_end_ns=" + std::to_string(tailVideoEndNs) + ")";
            }
            return false;
        }

        int64_t lagNs = tailVideoEndNs - static_cast<int64_t>(lastEncoderLogTimeNs);
        if (lagNs < 0)
        {
            lagNs = 0;
        }
        if (lagNs > kEncoderTailMaxLagNs)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("encoder tail lag too large for side=") + target.side +
                                " (lag_ns=" + std::to_string(lagNs) +
                                ", threshold_ns=" + std::to_string(kEncoderTailMaxLagNs) +
                                ", tail_video_end_ns=" + std::to_string(tailVideoEndNs) +
                                ", last_encoder_ns=" + std::to_string(lastEncoderLogTimeNs) + ")";
            }
            return false;
        }

        std::cout << "[PERF] encoder tail check pass:"
                  << " side=" << target.side
                  << " encoder_count=" << encoderMessageCount
                  << " tail_video_end_ns=" << tailVideoEndNs
                  << " last_encoder_ns=" << lastEncoderLogTimeNs
                  << " lag_ms=" << (lagNs / 1000000.0) << std::endl;
    }

    std::cout << "[PERF] validateEpisode end: episode_dir=" << episodeDir
              << " elapsed_ms=" << (steadyNowMs() - validateStartMs)
              << " reference_span_sec=" << formatSeconds(referenceSpanSec) << std::endl;
    return true;
}

const std::string &RecordRuntime::EpisodeManager::dataRoot() const
{
    return episodeRoot_;
}

bool RecordRuntime::EpisodeManager::writeMetadata(const std::string &episodeDir,
                                                  bool resetRecording,
                                                  const std::string &resetSourceDir,
                                                  std::string *errorMessage) const
{
    std::ofstream output(episodeDir + "/metadata.json");
    if (!output.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open metadata.json for write";
        }
        return false;
    }

    output
        << "{\n"
        << "  \"device_type\": \"UMI\",\n"
        << "  \"device_model\": \"ugripper\",\n"
        << "  \"device_id\": \"" << RecordRuntime::jsonEscape(deviceSn_) << "\",\n"
        << "  \"collector\": \"default_user\",\n"
        << "  \"data_path\": \"data/episode_{date:08d}_{episode_index:04d}\",\n"
        << "  \"camera_codec\": \"" << RecordRuntime::jsonEscape(cameraCodec_) << "\",\n"
        << "  \"ugripper_lang\": \"" << RecordRuntime::jsonEscape(language_) << "\",\n"
        << "  \"ugripper_version\": \"" << RecordRuntime::jsonEscape(packageVersion_) << "\",\n"
        << "  \"ugripper_usb_updater_version\": \"" << RecordRuntime::jsonEscape(updaterVersion_) << "\",\n"
        << "  \"data_format_version\": \"1\",\n"
        << "  \"record_runtime\": \"cpp\",\n"
        << "  \"reset_recording\": " << (resetRecording ? "true" : "false") << ",\n"
        << "  \"reset_source_episode_dir\": \"" << RecordRuntime::jsonEscape(resetSourceDir) << "\"\n"
        << "}\n";
    return true;
}

bool RecordRuntime::EpisodeManager::writeFilteredCalibration(const std::string &episodeDir, std::string *errorMessage) const
{
    json calibrationJson;
    std::vector<std::string> candidateFiles;
    if (fs::exists(persistCalibrationFile_))
    {
        candidateFiles.push_back(persistCalibrationFile_);
    }
    if (fs::exists(exampleCalibrationFile_) && exampleCalibrationFile_ != persistCalibrationFile_)
    {
        candidateFiles.push_back(exampleCalibrationFile_);
    }
    if (fs::exists(fallbackCalibrationFile_) &&
        fallbackCalibrationFile_ != persistCalibrationFile_ &&
        fallbackCalibrationFile_ != exampleCalibrationFile_)
    {
        candidateFiles.push_back(fallbackCalibrationFile_);
    }

    std::string sourceFile;
    std::string lastError;
    bool loaded = false;
    for (const std::string &candidate : candidateFiles)
    {
        if (loadCalibrationJsonFile(candidate, &calibrationJson, &lastError))
        {
            sourceFile = candidate;
            loaded = true;
            break;
        }
    }

    if (!loaded)
    {
        if (errorMessage != nullptr)
        {
            if (!lastError.empty())
            {
                *errorMessage = lastError;
            }
            else
            {
                *errorMessage = "no usable calibration source found";
            }
        }
        return false;
    }

    if (sourceFile != persistCalibrationFile_ && fs::exists(persistCalibrationFile_))
    {
        std::cerr << "[WARN] invalid persist calibration, fallback to " << sourceFile << std::endl;
    }

    std::map<std::string, json> sourceCalibrationEntries;
    for (const auto &target : kTactileCalibrationTargets)
    {
        if (calibrationJson.contains(target.jsonPath) && calibrationJson[target.jsonPath].is_object())
        {
            sourceCalibrationEntries.emplace(target.jsonPath, calibrationJson[target.jsonPath]);
        }
    }
    for (const std::string side : {"left", "right"})
    {
        const std::string legacyJsonPath = legacyTactileJsonPathForSide(side);
        if (calibrationJson.contains(legacyJsonPath) && calibrationJson[legacyJsonPath].is_object())
        {
            sourceCalibrationEntries.emplace(legacyJsonPath, calibrationJson[legacyJsonPath]);
        }
    }

    json leftStereoTemplate;
    json rightStereoTemplate;
    const json *leftStereoTemplatePtr = nullptr;
    const json *rightStereoTemplatePtr = nullptr;
    if (calibrationJson.contains("observation.images.left_stereo") && calibrationJson["observation.images.left_stereo"].is_object())
    {
        leftStereoTemplate = calibrationJson["observation.images.left_stereo"];
        leftStereoTemplatePtr = &leftStereoTemplate;
    }
    else if (calibrationJson.contains("observation.images.fays_cam0") && calibrationJson["observation.images.fays_cam0"].is_object())
    {
        leftStereoTemplate = calibrationJson["observation.images.fays_cam0"];
        leftStereoTemplatePtr = &leftStereoTemplate;
    }

    if (calibrationJson.contains("observation.images.right_stereo") && calibrationJson["observation.images.right_stereo"].is_object())
    {
        rightStereoTemplate = calibrationJson["observation.images.right_stereo"];
        rightStereoTemplatePtr = &rightStereoTemplate;
    }
    else if (calibrationJson.contains("observation.images.fays_cam1") && calibrationJson["observation.images.fays_cam1"].is_object())
    {
        rightStereoTemplate = calibrationJson["observation.images.fays_cam1"];
        rightStereoTemplatePtr = &rightStereoTemplate;
    }

    removeIfPresent(&calibrationJson, "observation.images.fays_cam0");
    removeIfPresent(&calibrationJson, "observation.images.fays_cam1");
    removeIfPresent(&calibrationJson, "observation.imu.fays_imu0");

    calibrationJson["observation.images.left_stereo"] = makeStereoPlaceholderFromTemplate(leftStereoTemplatePtr);
    calibrationJson["observation.images.right_stereo"] = makeStereoPlaceholderFromTemplate(rightStereoTemplatePtr);

    for (const auto &target : kTactileCalibrationTargets)
    {
        const json *templateEntry = findSourceCalibrationEntry(
            sourceCalibrationEntries,
            tactileTemplateCandidateKeys(target));
        calibrationJson[target.jsonPath] = makeTactileCalibrationEntry(templateEntry, target.serialPlaceholder);
    }

    if (!calibrationJson.contains("metadata") || !calibrationJson["metadata"].is_object())
    {
        calibrationJson["metadata"] = json::object();
    }
    calibrationJson["metadata"]["format_version"] = calibrationJson["metadata"].value("format_version", "1.0");
    calibrationJson["metadata"]["description"] = calibrationJson["metadata"].value("description", "Camera calibration parameters");

    if (!calibrationJson.contains("calibration_info") || !calibrationJson["calibration_info"].is_object())
    {
        calibrationJson["calibration_info"] = json::object();
    }

    json &calibrationInfo = calibrationJson["calibration_info"];
    calibrationInfo.erase("fays_imu_bundle");
    appendCalibrationNote(
        &calibrationInfo,
        "Stereo calibration entries are placeholder values migrated from the legacy Fays template until dedicated stereo calibration is available.");
    appendCalibrationNote(
        &calibrationInfo,
        "Episode tactile calibration now includes only per-camera entries for left_tcam_l, left_tcam_r, right_tcam_l and right_tcam_r; persist calibration is not rewritten.");

    if (!commandExists("udevadm"))
    {
        std::cerr << "[WARN] udevadm not found in PATH, skip runtime tactile serial injection" << std::endl;
    }
    else
    {
        std::map<std::string, std::string> sideReplacementSerials;
        for (const auto &target : kTactileCalibrationTargets)
        {
            std::string detail;
            const auto serial = probeUsbSerialForDeviceNode(target.devicePath, &detail);
            if (!serial.has_value())
            {
                std::cerr << "[WARN] failed to resolve runtime tactile serial for camera=" << target.cameraName
                          << " device=" << target.devicePath
                          << " detail=" << detail
                          << "; keep source calibration value" << std::endl;
                continue;
            }

            injectRuntimeTactileSerial(
                &calibrationJson,
                target,
                *serial,
                detail,
                sourceFile,
                persistCalibrationFile_);

            if (sideReplacementSerials.find(target.side) == sideReplacementSerials.end())
            {
                sideReplacementSerials.emplace(target.side, *serial);
            }
        }

        for (const auto &entry : sideReplacementSerials)
        {
            replaceJsonStringValues(
                &calibrationJson,
                legacyTactileSerialPlaceholderForSide(entry.first),
                entry.second);
        }
    }

    removeIfPresent(&calibrationJson, "observation.images.gripper_left_tactile");
    removeIfPresent(&calibrationJson, "observation.images.gripper_right_tactile");

    std::ofstream output(episodeDir + "/calibration.json");
    if (!output.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open calibration.json for write";
        }
        return false;
    }

    output << calibrationJson.dump(4) << "\n";
    return true;
}

bool RecordRuntime::EpisodeManager::prepareEpisodeOutputs(const std::string &episodeDir, std::string *errorMessage) const
{
    (void)episodeDir;
    (void)errorMessage;
    return true;
}
