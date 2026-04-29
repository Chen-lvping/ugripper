#include "record_runtime.h"
#include "utils/logger.h"
#include "record_runtime/gripper_refresh_logic.h"
#include "record_runtime/motion_alert_ipc.h"
#include "utils/env_utils.h"
#include "utils/file_utils.hpp"
#include "utils/time_utils.h"

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
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
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
constexpr const char *kChestCameraEnvKey = "ENABLE_CHEST_CAM_MAIN";
constexpr uint64_t kActionDebounceMs = 250;
constexpr uint64_t kLongPressThresholdMs = 800;
constexpr uint64_t kDualLongPressThresholdMs = 4000;
constexpr uint64_t kShutdownPromptThresholdMs = 2000;
constexpr uint64_t kSystemActionResultWaitMs = 8000;
constexpr uint64_t kPostUmountAudioDelayMs = 2000;
constexpr uint64_t kAudioPlayerRestartIntervalMs = 2000;
constexpr uint64_t kAudioPlayerReadyGraceMs = 3000;
constexpr uint64_t kStereoDaemonRestartIntervalMs = 2000;
constexpr uint64_t kStereoFinalizeWaitPollMs = 100;
constexpr uint64_t kHealthCheckIntervalMs = 1000;
constexpr uint64_t kHmiActiveTimeoutMs = 2500;
constexpr uint64_t kGripperRefreshActiveTimeoutMs = 1500;
constexpr double kMinReasonableVideoSpanSec = 0.2;
constexpr double kMaxVideoSpanGapSec = 5.0;
constexpr int64_t kEncoderTailWindowNs = 1000LL * 1000LL * 1000LL;
constexpr int64_t kEncoderTailMaxLagNs = 1000LL * 1000LL * 1000LL;
constexpr size_t kEncoderTailChunkScanLimit = 4;
constexpr int kVideoProbeTimeoutMs = 1500;
constexpr int kUdevadmProbeTimeoutMs = 2000;
constexpr int kTactileFrameWidth = 160;
constexpr int kTactileFrameHeight = 120;
constexpr size_t kTactileFrameBytes = static_cast<size_t>(kTactileFrameWidth * kTactileFrameHeight);
constexpr double kTactileEpisodeProbeSec = 0.12;
constexpr double kTactileMeanAbsThreshold = 3.0;
constexpr double kTactileMaskRatioThreshold = 0.05;
constexpr double kTactileCorrelationThreshold = 0.94;
constexpr size_t kTactileHistoryWindow = 3;
constexpr int kTactileSnapshotTimeoutMs = 2500;
constexpr uint64_t kTactilePersistentBaselineRefreshMs = 12ULL * 60ULL * 60ULL * 1000ULL;

struct EpisodeVideoArtifact
{
    const char *cameraName;
    const char *fileName;
};

constexpr std::array<EpisodeVideoArtifact, 9> kAllEpisodeVideoArtifacts = {{
    {"left_cam_main", "left_cam_main.mkv"},
    {"right_cam_main", "right_cam_main.mkv"},
    {"chest_cam_main", "chest_cam_main.mkv"},
    {"left_stereo", "left_stereo.mkv"},
    {"right_stereo", "right_stereo.mkv"},
    {"left_tcam_l", "left_tcam_l.mkv"},
    {"left_tcam_r", "left_tcam_r.mkv"},
    {"right_tcam_l", "right_tcam_l.mkv"},
    {"right_tcam_r", "right_tcam_r.mkv"},
}};

constexpr std::array<const char *, 15> kAllCriticalDevicePaths = {{
    "/dev/right_cam_main",
    "/dev/left_cam_main",
    "/dev/chest_cam_main",
    "/dev/right_stereo",
    "/dev/left_stereo",
    "/dev/right_fays_imu",
    "/dev/left_fays_imu",
    "/dev/right_tcam_l",
    "/dev/right_tcam_r",
    "/dev/left_tcam_l",
    "/dev/left_tcam_r",
    "/dev/right_encoder",
    "/dev/left_encoder",
    "/dev/right_imu",
    "/dev/left_imu",
}};

constexpr std::array<const char *, 7> kLeftCriticalDevicePaths = {{
    "/dev/left_cam_main",
    "/dev/left_stereo",
    "/dev/left_fays_imu",
    "/dev/left_tcam_l",
    "/dev/left_tcam_r",
    "/dev/left_encoder",
    "/dev/left_imu",
}};

constexpr std::array<const char *, 7> kRightCriticalDevicePaths = {{
    "/dev/right_cam_main",
    "/dev/right_stereo",
    "/dev/right_fays_imu",
    "/dev/right_tcam_l",
    "/dev/right_tcam_r",
    "/dev/right_encoder",
    "/dev/right_imu",
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

struct TactileFrameMetrics
{
    double meanAbsDiff = 0.0;
    double maskRatio = 0.0;
    double correlation = 1.0;
    bool damaged = false;
};

bool writeTextFileAtomically(const fs::path &path, const std::string &content, std::string *errorMessage);

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

const char *boolText(bool value)
{
    return value ? "1" : "0";
}

std::string sanitizeFileComponent(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (const char ch : text)
    {
        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            ch == '.' || ch == '_' || ch == '-')
        {
            result.push_back(ch);
        }
        else
        {
            result.push_back('_');
        }
    }
    return result.empty() ? "unknown" : result;
}

fs::path tactileReferenceDir(const fs::path &root)
{
    return root / "reference";
}

fs::path tactilePersistentDir(const fs::path &root)
{
    return root / "persistent";
}

fs::path tactileHistoryPath(const fs::path &root)
{
    return root / "history.json";
}

fs::path tactilePersistentResetPath(const fs::path &root)
{
    return root / "persistent_reset.json";
}

fs::path tactileReferenceRawPath(const fs::path &root, const std::string &serial)
{
    return tactileReferenceDir(root) / (sanitizeFileComponent(serial) + ".gray");
}

fs::path tactileReferenceMetaPath(const fs::path &root, const std::string &serial)
{
    return tactileReferenceDir(root) / (sanitizeFileComponent(serial) + ".json");
}

fs::path tactilePersistentRawPath(const fs::path &root, const std::string &serial)
{
    return tactilePersistentDir(root) / (sanitizeFileComponent(serial) + ".gray");
}

std::string formatFixed(double value, int precision)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

bool isFalseLikeValue(const std::string &value)
{
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered == "0" || lowered == "false" || lowered == "no" ||
           lowered == "off" || lowered == "disable" || lowered == "disabled";
}

std::vector<EpisodeVideoArtifact> activeEpisodeVideoArtifacts(bool chestCameraEnabled)
{
    std::vector<EpisodeVideoArtifact> artifacts;
    artifacts.reserve(kAllEpisodeVideoArtifacts.size());
    for (const auto &artifact : kAllEpisodeVideoArtifacts)
    {
        if (!chestCameraEnabled && std::strcmp(artifact.cameraName, "chest_cam_main") == 0)
        {
            continue;
        }
        artifacts.push_back(artifact);
    }
    return artifacts;
}

std::vector<const char *> activeCriticalDevicePaths(bool chestCameraEnabled)
{
    std::vector<const char *> paths;
    paths.reserve(kAllCriticalDevicePaths.size());
    for (const char *path : kAllCriticalDevicePaths)
    {
        if (!chestCameraEnabled && std::strcmp(path, "/dev/chest_cam_main") == 0)
        {
            continue;
        }
        paths.push_back(path);
    }
    return paths;
}

std::vector<std::string> sessionCameraStreams(bool chestCameraEnabled)
{
    std::vector<std::string> streams = {
        "left_cam_main",
        "right_cam_main",
        "left_tcam_l",
        "left_tcam_r",
        "right_tcam_l",
        "right_tcam_r",
    };
    if (chestCameraEnabled)
    {
        streams.insert(streams.begin() + 2, "chest_cam_main");
    }
    return streams;
}

std::string joinCsv(const std::vector<std::string> &items)
{
    std::ostringstream stream;
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (index > 0)
        {
            stream << ",";
        }
        stream << items[index];
    }
    return stream.str();
}

std::string currentDateString()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&nowTime, &localTime);

    char dateBuffer[16] = {0};
    std::strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", &localTime);
    return std::string(dateBuffer);
}

std::set<std::string> loadPersistentResetSerials(const fs::path &path)
{
    std::set<std::string> result;
    std::ifstream input(path);
    if (!input.is_open())
    {
        return result;
    }

    try
    {
        json root = json::parse(input);
        if (root.contains("serials") && root["serials"].is_array())
        {
            for (const auto &entry : root["serials"])
            {
                if (entry.is_string() && !entry.get<std::string>().empty())
                {
                    result.insert(entry.get<std::string>());
                }
            }
        }
    }
    catch (const std::exception &ex)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to parse tactile persistent reset list: " << ex.what() << std::endl).str());
    }
    return result;
}

bool storePersistentResetSerials(const fs::path &path,
                                 const std::set<std::string> &serials,
                                 std::string *errorMessage)
{
    json root = {
        {"serials", json::array()},
    };
    for (const auto &serial : serials)
    {
        root["serials"].push_back(serial);
    }
    return writeTextFileAtomically(path, root.dump(2) + "\n", errorMessage);
}

bool writeBinaryFile(const fs::path &path, const std::vector<uint8_t> &bytes, std::string *errorMessage)
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
    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open temp file for write: " + tempPath.string();
        }
        return false;
    }
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
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
    output.close();

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

bool readBinaryFileExact(const fs::path &path,
                         size_t expectedBytes,
                         std::vector<uint8_t> *bytes,
                         std::string *errorMessage)
{
    if (bytes == nullptr)
    {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "cannot open file: " + path.string();
        }
        return false;
    }

    std::vector<uint8_t> buffer(expectedBytes, 0);
    input.read(reinterpret_cast<char *>(buffer.data()),
               static_cast<std::streamsize>(expectedBytes));
    const std::streamsize actual = input.gcount();
    if (actual != static_cast<std::streamsize>(expectedBytes) ||
        input.peek() != std::ifstream::traits_type::eof())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "unexpected tactile frame size: " + path.string();
        }
        return false;
    }
    *bytes = std::move(buffer);
    return true;
}

TactileFrameMetrics computeTactileFrameMetrics(const std::vector<uint8_t> &baseline,
                                               const std::vector<uint8_t> &current)
{
    TactileFrameMetrics metrics;
    if (baseline.size() != current.size() || baseline.empty())
    {
        metrics.damaged = true;
        metrics.correlation = 0.0;
        metrics.maskRatio = 1.0;
        return metrics;
    }

    double sumBase = 0.0;
    double sumCurrent = 0.0;
    for (size_t index = 0; index < baseline.size(); ++index)
    {
        sumBase += static_cast<double>(baseline[index]);
        sumCurrent += static_cast<double>(current[index]);
    }
    const double meanBase = sumBase / static_cast<double>(baseline.size());
    const double meanCurrent = sumCurrent / static_cast<double>(current.size());

    double absDiffSum = 0.0;
    size_t maskCount = 0;
    double covariance = 0.0;
    double baseVariance = 0.0;
    double currentVariance = 0.0;
    for (size_t index = 0; index < baseline.size(); ++index)
    {
        const double baseValue = static_cast<double>(baseline[index]);
        const double currentValue = static_cast<double>(current[index]);
        const double diff = std::fabs(baseValue - currentValue);
        absDiffSum += diff;
        if (diff >= 30.0)
        {
            ++maskCount;
        }

        const double baseCentered = baseValue - meanBase;
        const double currentCentered = currentValue - meanCurrent;
        covariance += baseCentered * currentCentered;
        baseVariance += baseCentered * baseCentered;
        currentVariance += currentCentered * currentCentered;
    }

    metrics.meanAbsDiff = absDiffSum / static_cast<double>(baseline.size());
    metrics.maskRatio = static_cast<double>(maskCount) / static_cast<double>(baseline.size());
    if (baseVariance > 0.0 && currentVariance > 0.0)
    {
        metrics.correlation = covariance / std::sqrt(baseVariance * currentVariance);
    }
    if (!std::isfinite(metrics.correlation))
    {
        metrics.correlation = 1.0;
    }

    metrics.damaged =
        metrics.meanAbsDiff >= kTactileMeanAbsThreshold ||
        metrics.maskRatio >= kTactileMaskRatioThreshold ||
        metrics.correlation <= kTactileCorrelationThreshold;
    return metrics;
}

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
    if (root == nullptr || !root->is_object() || key.empty())
    {
        return;
    }

    const size_t separator = key.rfind('.');
    if (separator == std::string::npos)
    {
        root->erase(key);
        return;
    }

    json *cursor = root;
    size_t start = 0;
    while (start < separator)
    {
        const size_t dot = key.find('.', start);
        if (dot == std::string::npos)
        {
            break;
        }
        const std::string token = key.substr(start, dot - start);
        if (!cursor->contains(token) || !(*cursor)[token].is_object())
        {
            return;
        }
        cursor = &(*cursor)[token];
        start = dot + 1;
    }

    const std::string leaf = key.substr(separator + 1);
    if (!leaf.empty() && cursor->is_object())
    {
        cursor->erase(leaf);
    }
}

json *findJsonPath(json *root, const std::string &path, bool createMissing)
{
    if (root == nullptr || path.empty())
    {
        return nullptr;
    }

    json *cursor = root;
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t dot = path.find('.', start);
        const std::string token =
            path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (token.empty())
        {
            return nullptr;
        }

        if (dot == std::string::npos)
        {
            if (!cursor->is_object())
            {
                return nullptr;
            }
            if (!cursor->contains(token))
            {
                if (!createMissing)
                {
                    return nullptr;
                }
                (*cursor)[token] = json::object();
            }
            return &(*cursor)[token];
        }

        if (!cursor->is_object())
        {
            return nullptr;
        }
        if (!cursor->contains(token))
        {
            if (!createMissing)
            {
                return nullptr;
            }
            (*cursor)[token] = json::object();
        }
        if (!(*cursor)[token].is_object())
        {
            if (!createMissing)
            {
                return nullptr;
            }
            (*cursor)[token] = json::object();
        }
        cursor = &(*cursor)[token];
        start = dot + 1;
    }

    return cursor;
}

const json *findJsonPathConst(const json *root, const std::string &path)
{
    if (root == nullptr || !root->is_object() || path.empty())
    {
        return nullptr;
    }

    const json *cursor = root;
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t dot = path.find('.', start);
        const std::string token =
            path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (token.empty() || !cursor->is_object() || !cursor->contains(token))
        {
            return nullptr;
        }
        cursor = &(*cursor)[token];
        if (dot == std::string::npos)
        {
            return cursor;
        }
        start = dot + 1;
    }

    return nullptr;
}

void removeLegacyFlattenedCalibrationKeys(json *root)
{
    if (root == nullptr || !root->is_object())
    {
        return;
    }

    std::vector<std::string> keysToErase;
    for (auto it = root->begin(); it != root->end(); ++it)
    {
        if (it.key().rfind("observation.", 0) == 0)
        {
            keysToErase.push_back(it.key());
        }
    }
    for (const std::string &key : keysToErase)
    {
        root->erase(key);
    }
}

void migrateLegacyFlattenedCalibrationKeys(json *root)
{
    if (root == nullptr || !root->is_object())
    {
        return;
    }

    std::vector<std::pair<std::string, json>> flattenedEntries;
    for (auto it = root->begin(); it != root->end(); ++it)
    {
        if (it.key().rfind("observation.", 0) == 0)
        {
            flattenedEntries.emplace_back(it.key(), it.value());
        }
    }

    for (const auto &[path, value] : flattenedEntries)
    {
        json *target = findJsonPath(root, path, true);
        if (target != nullptr && target->is_object() && value.is_object())
        {
            *target = value;
        }
    }

    removeLegacyFlattenedCalibrationKeys(root);
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

size_t gripperStateIndexForSide(const std::string &side)
{
    return side == "left" ? 0u : 1u;
}

const char *mainCameraJsonPathForSide(const std::string &side)
{
    return side == "left" ? "observation.images.left_cam_main"
                          : "observation.images.right_cam_main";
}

const char *stereoJsonPathForSide(const std::string &side)
{
    return side == "left" ? "observation.images.left_stereo"
                          : "observation.images.right_stereo";
}

const char *imuJsonPathForSide(const std::string &side)
{
    return side == "left" ? "observation.imu.left_imu"
                          : "observation.imu.right_imu";
}

json makeRgbCalibrationEntryFromPayload(const gripper_hmi::GripperCalibrationDataV1 &payload)
{
    const int width = static_cast<int>(std::lround(payload.rgbCamera.resolution[0]));
    const int height = static_cast<int>(std::lround(payload.rgbCamera.resolution[1]));
    return json::object({
        {"shape", json::array({height > 0 ? height : 1080, width > 0 ? width : 1920, 3})},
        {"names", json::array({"height", "width", "channels"})},
        {"info", nullptr},
        {"intrinsics",
         json::object({{std::to_string(width > 0 ? width : 1920) + "x" +
                             std::to_string(height > 0 ? height : 1080),
                         json::object({
                             {"fx", payload.rgbCamera.intrinsics[0]},
                             {"fy", payload.rgbCamera.intrinsics[1]},
                             {"ppx", payload.rgbCamera.intrinsics[2]},
                             {"ppy", payload.rgbCamera.intrinsics[3]},
                         })}})},
        {"camera_model", "pinhole"},
        {"distortion_model", "equidistant"},
        {"distortion_coeffs",
         json::array({
             payload.rgbCamera.distortionCoefficients[0],
             payload.rgbCamera.distortionCoefficients[1],
             payload.rgbCamera.distortionCoefficients[2],
             payload.rgbCamera.distortionCoefficients[3],
         })},
        {"dtype", "video"},
        {"fps", "unknown"},
    });
}

json makeMainCameraPlaceholderFromTemplate(const json *templateEntry)
{
    const json source = (templateEntry != nullptr && templateEntry->is_object()) ? *templateEntry : json::object();
    json normalized = json::object();
    normalized["shape"] = source.contains("shape") && source["shape"].is_array()
                              ? source["shape"]
                              : json::array({1080, 1920, 3});
    normalized["names"] = source.contains("names") && source["names"].is_array()
                              ? source["names"]
                              : json::array({"height", "width", "channels"});
    normalized["info"] = source.contains("info") ? source["info"] : json(nullptr);
    normalized["intrinsics"] = source.contains("intrinsics") && source["intrinsics"].is_object() && !source["intrinsics"].empty()
                                   ? source["intrinsics"]
                                   : json::object({
                                         {"1920x1080",
                                          json::object({
                                              {"fx", 1920.0},
                                              {"fy", 1080.0},
                                              {"ppx", 960.0},
                                              {"ppy", 540.0},
                                          })},
                                     });
    normalized["camera_model"] = source.contains("camera_model") ? source["camera_model"] : json("pinhole");
    normalized["distortion_model"] = source.contains("distortion_model") ? source["distortion_model"] : json("equidistant");
    normalized["distortion_coeffs"] = source.contains("distortion_coeffs") && source["distortion_coeffs"].is_array()
                                          ? source["distortion_coeffs"]
                                          : json::array({0.0, 0.0, 0.0, 0.0});
    normalized["dtype"] = source.contains("dtype") ? source["dtype"] : json("video");
    normalized["fps"] = source.contains("fps") ? source["fps"] : json("unknown");
    return normalized;
}

bool loadFaysCalibrationFromStatus(const std::string &stereoStatusFile,
                                   const std::string &cameraName,
                                   json *calibration,
                                   std::string *statusMessage)
{
    if (calibration == nullptr)
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = "calibration output is null";
        }
        return false;
    }

    json status;
    std::string loadError;
    if (!loadJsonFile(stereoStatusFile, &status, &loadError))
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = loadError;
        }
        return false;
    }

    const json *camera = findJsonPathConst(&status, std::string("cameras.") + cameraName);
    if (camera == nullptr || !camera->is_object())
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = "stereo status missing camera entry: " + cameraName;
        }
        return false;
    }
    if (!camera->contains("calibration") || !(*camera)["calibration"].is_object())
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = "stereo status missing calibration entry: " + cameraName;
        }
        return false;
    }

    const json &calibrationState = (*camera)["calibration"];
    if (!calibrationState.value("valid", false))
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = "Fays calibration not ready for " + cameraName +
                             ": " + calibrationState.value("status", std::string("unknown"));
        }
        return false;
    }

    const std::string path = calibrationState.value("path", std::string());
    if (path.empty())
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = "Fays calibration path is empty for " + cameraName;
        }
        return false;
    }

    json faysCalibration;
    if (!loadJsonFile(path, &faysCalibration, &loadError))
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = loadError;
        }
        return false;
    }
    if (!faysCalibration.is_object() || !faysCalibration.value("valid", false))
    {
        if (statusMessage != nullptr)
        {
            *statusMessage = "invalid Fays calibration payload: " + path;
        }
        return false;
    }

    faysCalibration.erase("valid");
    if (!faysCalibration.contains("dtype"))
    {
        faysCalibration["dtype"] = "video";
    }
    if (!faysCalibration.contains("fps"))
    {
        faysCalibration["fps"] = 25;
    }
    *calibration = std::move(faysCalibration);
    if (statusMessage != nullptr)
    {
        *statusMessage = "loaded " + path;
    }
    return true;
}

json makeFaysCameraCalibrationEntry(const json &camera)
{
    const auto normalizedDistortionCoeffs = [](const json &source, const std::string &model) {
        const size_t coeffCount =
            (model == "ADM_BROWN_CONRADY") ? 5u :
            (model == "ADM_CVBASIC") ? 8u :
            4u;
        json coeffs = json::array();
        if (source.is_array())
        {
            for (size_t index = 0; index < coeffCount && index < source.size(); ++index)
            {
                coeffs.push_back(source[index]);
            }
        }
        while (coeffs.size() < coeffCount)
        {
            coeffs.push_back(0.0);
        }
        return coeffs;
    };

    const std::string distortionModel = camera.value("distortion_model", std::string("ADM_KB4"));
    json entry = json::object();
    entry["camera_model_enum"] = camera.contains("camera_model_enum")
                                     ? camera["camera_model_enum"]
                                     : json(0);
    entry["intrinsics"] = camera.contains("intrinsics") && camera["intrinsics"].is_object() && !camera["intrinsics"].empty()
                              ? camera["intrinsics"]
                              : json::object({
                                    {"1280x400",
                                     json::object({
                                         {"fx", 1280.0},
                                         {"fy", 400.0},
                                         {"ppx", 640.0},
                                         {"ppy", 200.0},
                                     })},
                                });
    entry["distortion_coeffs"] = normalizedDistortionCoeffs(
        camera.contains("distortion_coeffs") ? camera["distortion_coeffs"] : json::array(),
        distortionModel);
    return entry;
}

json makeFaysStereoCalibrationEntry(const json &faysCalibration)
{
    const auto normalizeDistortionModel = [](const json &model) {
        const std::string value = model.is_string() ? model.get<std::string>() : std::string("ADM_KB4");
        if (value == "ADM_KB4")
        {
            return json("equidistant");
        }
        if (value == "ADM_RADTAN")
        {
            return json("radtan");
        }
        if (value == "ADM_BROWN_CONRADY")
        {
            return json("brown_conrady");
        }
        if (value == "ADM_CVBASIC")
        {
            return json("opencv_basic");
        }
        return model.is_string() ? model : json("equidistant");
    };

    json stereo = json::object();
    stereo["shape"] = faysCalibration.contains("shape") && faysCalibration["shape"].is_array()
                          ? faysCalibration["shape"]
                          : json::array({400, 1280, 3});
    stereo["names"] = faysCalibration.contains("names") && faysCalibration["names"].is_array()
                          ? faysCalibration["names"]
                          : json::array({"height", "width", "channels"});
    stereo["info"] = faysCalibration.contains("info") ? faysCalibration["info"] : json(nullptr);
    stereo["camera_model"] = faysCalibration.contains("camera_model") ? faysCalibration["camera_model"] : json("pinhole");
    stereo["distortion_model"] = normalizeDistortionModel(
        faysCalibration.contains("distortion_model") ? faysCalibration["distortion_model"] : json("ADM_KB4"));
    stereo["cam0"] = makeFaysCameraCalibrationEntry(
        faysCalibration.contains("cam0") && faysCalibration["cam0"].is_object()
            ? faysCalibration["cam0"]
            : json::object());
    stereo["cam1"] = makeFaysCameraCalibrationEntry(
        faysCalibration.contains("cam1") && faysCalibration["cam1"].is_object()
            ? faysCalibration["cam1"]
            : json::object());
    stereo["extrinsics"] = faysCalibration.contains("extrinsics") && faysCalibration["extrinsics"].is_object()
                               ? faysCalibration["extrinsics"]
                               : json::object();
    if (faysCalibration.contains("residuals") && faysCalibration["residuals"].is_object())
    {
        stereo["residuals"] = faysCalibration["residuals"];
    }
    stereo["dtype"] = faysCalibration.contains("dtype") ? faysCalibration["dtype"] : json("video");
    stereo["fps"] = faysCalibration.contains("fps") ? faysCalibration["fps"] : json(25);
    return stereo;
}

json makeFaysImuCalibrationEntry(const json &faysCalibration)
{
    json imu = json::object({
        {"dtype", "imu"},
        {"model", "fays_vikit"},
    });
    if (faysCalibration.contains("imu") && faysCalibration["imu"].is_object())
    {
        const json &source = faysCalibration["imu"];
        if (source.contains("update_rate"))
        {
            imu["update_rate_hz"] = source["update_rate"];
        }
        if (source.contains("accelerometer_noise_density") || source.contains("accelerometer_random_walk"))
        {
            imu["accelerometer"] = json::object();
            if (source.contains("accelerometer_noise_density"))
            {
                imu["accelerometer"]["noise_density_discrete"] = source["accelerometer_noise_density"];
            }
            if (source.contains("accelerometer_random_walk"))
            {
                imu["accelerometer"]["random_walk"] = source["accelerometer_random_walk"];
            }
        }
        if (source.contains("gyroscope_noise_density") || source.contains("gyroscope_random_walk"))
        {
            imu["gyroscope"] = json::object();
            if (source.contains("gyroscope_noise_density"))
            {
                imu["gyroscope"]["noise_density_discrete"] = source["gyroscope_noise_density"];
            }
            if (source.contains("gyroscope_random_walk"))
            {
                imu["gyroscope"]["random_walk"] = source["gyroscope_random_walk"];
            }
        }
    }
    return imu;
}

json makeStereoCalibrationEntryFromPayload(const std::string &side,
                                           const gripper_hmi::GripperCalibrationDataV1 &payload)
{
    (void)side;
    const auto makeIntrinsics = [](float fx, float fy, float ppx, float ppy) {
        return json::object({
            {"640x400",
             json::object({
                 {"fx", fx},
                 {"fy", fy},
                 {"ppx", ppx},
                 {"ppy", ppy},
             })},
        });
    };
    const auto makeDistortion = [](const float *coeffs) {
        return json::array({coeffs[0], coeffs[1], coeffs[2], coeffs[3]});
    };
    const auto matrixToJson = [](const float *values) {
        json rows = json::array();
        for (size_t row = 0; row < 4; ++row)
        {
            rows.push_back(json::array({
                values[row * 4 + 0],
                values[row * 4 + 1],
                values[row * 4 + 2],
                values[row * 4 + 3],
            }));
        }
        return rows;
    };
    const auto statsToJson = [](const gripper_hmi::GripperStatisticsBlock &stats) {
        return json::object({
            {"mean", stats.mean},
            {"median", stats.median},
            {"stddev", stats.stddev},
        });
    };

    return json::object({
        {"shape", json::array({400, 1280, 3})},
        {"names", json::array({"height", "width", "channels"})},
        {"info", nullptr},
        {"camera_model", "pinhole"},
        {"distortion_model", "equidistant"},
        {"cam0",
         json::object({
             {"camera_model_enum", payload.stereoCam0.cameraModelEnum},
             {"intrinsics",
              makeIntrinsics(payload.stereoCam0.focalLength[0],
                             payload.stereoCam0.focalLength[1],
                             payload.stereoCam0.principalPoint[0],
                             payload.stereoCam0.principalPoint[1])},
             {"distortion_coeffs", makeDistortion(payload.stereoCam0.distortionCoefficients)},
         })},
        {"cam1",
         json::object({
             {"camera_model_enum", payload.stereoCam1.cameraModelEnum},
             {"intrinsics",
              makeIntrinsics(payload.stereoCam1.focalLength[0],
                             payload.stereoCam1.focalLength[1],
                             payload.stereoCam1.principalPoint[0],
                             payload.stereoCam1.principalPoint[1])},
             {"distortion_coeffs", makeDistortion(payload.stereoCam1.distortionCoefficients)},
         })},
        {"extrinsics",
         json::object({
             {"T_ic_cam0_to_imu0", matrixToJson(payload.extrinsics.tIcCam0ToImu0)},
             {"timeshift_cam0_to_imu0", payload.extrinsics.timeshiftCam0ToImu0},
             {"T_ic_cam1_to_imu0", matrixToJson(payload.extrinsics.tIcCam1ToImu0)},
             {"timeshift_cam1_to_imu0", payload.extrinsics.timeshiftCam1ToImu0},
             {"baseline_norm", payload.extrinsics.baselineNorm},
         })},
        {"residuals",
         json::object({
             {"reprojection_error_cam0_px", statsToJson(payload.residuals.reprojectionErrorCam0Px)},
             {"reprojection_error_cam1_px", statsToJson(payload.residuals.reprojectionErrorCam1Px)},
             {"gyroscope_error_imu0_rad_s", statsToJson(payload.residuals.gyroscopeErrorImu0RadS)},
             {"accelerometer_error_imu0_m_s2", statsToJson(payload.residuals.accelerometerErrorImu0MS2)},
         })},
        {"dtype", "video"},
        {"fps", 60},
    });
}

json makeImuCalibrationEntryFromPayload(const gripper_hmi::GripperCalibrationDataV1 &payload)
{
    return json::object({
        {"dtype", "imu"},
        {"model", "calibrated"},
        {"update_rate_hz", payload.imu0.updateRate},
        {"accelerometer",
         json::object({
             {"noise_density_discrete", payload.imu0.accelerometerNoiseDensityDiscrete},
             {"random_walk", payload.imu0.accelerometerRandomWalk},
         })},
        {"gyroscope",
         json::object({
             {"noise_density_discrete", payload.imu0.gyroscopeNoiseDensityDiscrete},
             {"random_walk", payload.imu0.gyroscopeRandomWalk},
         })},
    });
}

json makeLockedCalibrationMetadata(const std::string &generationDate,
                                   const std::string &calibrationStatus)
{
    return json::object({
        {"calibration_status", calibrationStatus},
        {"description", "Camera calibration parameters"},
        {"format_version", "3.0"},
        {"generation_date", generationDate},
    });
}

json makeLockedCalibrationInfo(const std::string &generationDate,
                               const std::string &calibrationStatus,
                               const std::string &notes)
{
    return json::object({
        {"calibration_date", generationDate},
        {"calibration_status", calibrationStatus},
        {"notes", notes},
    });
}

void sanitizeCalibrationTopLevel(json *calibrationJson)
{
    if (calibrationJson == nullptr || !calibrationJson->is_object())
    {
        return;
    }

    json observation = json::object();
    if (calibrationJson->contains("observation") && (*calibrationJson)["observation"].is_object())
    {
        observation = (*calibrationJson)["observation"];
    }

    *calibrationJson = json::object({
        {"calibration_info", json::object()},
        {"metadata", json::object()},
        {"observation", observation},
    });
}

void eraseSideCalibrationEntries(json *calibrationJson, const std::string &side)
{
    if (calibrationJson == nullptr || !calibrationJson->is_object())
    {
        return;
    }

    removeIfPresent(calibrationJson, mainCameraJsonPathForSide(side));
    removeIfPresent(calibrationJson, stereoJsonPathForSide(side));
    removeIfPresent(calibrationJson, imuJsonPathForSide(side));
}

void applySideCalibrationPayload(json *calibrationJson,
                                 const std::string &side,
                                 const gripper_hmi::GripperCalibrationDataV1 &payload)
{
    if (calibrationJson == nullptr || !calibrationJson->is_object())
    {
        return;
    }

    if (json *mainCamera = findJsonPath(calibrationJson, mainCameraJsonPathForSide(side), true))
    {
        *mainCamera = makeRgbCalibrationEntryFromPayload(payload);
    }
    if (json *stereo = findJsonPath(calibrationJson, stereoJsonPathForSide(side), true))
    {
        *stereo = makeStereoCalibrationEntryFromPayload(side, payload);
    }
    if (json *imu = findJsonPath(calibrationJson, imuJsonPathForSide(side), true))
    {
        *imu = makeImuCalibrationEntryFromPayload(payload);
    }
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
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] flush dir skip missing: phase=" << phaseLabel
                      << " path=" << directory << std::endl).str());
            continue;
        }

        const int64_t dirFlushStartMs = steadyNowMs();
        std::error_code error;
        if (!flushDirectoryToDisk(directory, &error))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush episode directory: phase=" << phaseLabel
                      << " path=" << directory
                      << " error=" << error.message() << std::endl).str());
            continue;
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] flush dir done: phase=" << phaseLabel
                  << " path=" << directory
                  << " elapsed_ms=" << (steadyNowMs() - dirFlushStartMs) << std::endl).str());
    }
}

void flushEpisodeArtifactsToDisk(const fs::path &episodeDir, bool chestCameraEnabled, const char *phaseLabel)
{
    if (episodeDir.empty())
    {
        return;
    }

    std::vector<fs::path> paths;
    const auto artifacts = activeEpisodeVideoArtifacts(chestCameraEnabled);
    paths.reserve(artifacts.size() + 6);

    for (const auto &artifact : artifacts)
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
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] flush begin: phase=" << phaseLabel
              << " episode_dir=" << episodeDir
              << " candidate_paths=" << paths.size() << std::endl).str());

    for (const auto &path : paths)
    {
        if (!fs::exists(path))
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] flush skip missing: path=" << path << std::endl).str());
            continue;
        }

        const int64_t artifactFlushStartMs = steadyNowMs();
        std::error_code error;
        if (!flushFileToDisk(path, &error))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush episode artifact: "
                      << path << " error=" << error.message() << std::endl).str());
            continue;
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] flush done: path=" << path
                  << " elapsed_ms=" << (steadyNowMs() - artifactFlushStartMs) << std::endl).str());
    }

    flushEpisodeDirectoriesToDisk(episodeDir, phaseLabel);

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] flush end: phase=" << phaseLabel
              << " episode_dir=" << episodeDir
              << " elapsed_ms=" << (steadyNowMs() - flushStartMs) << std::endl).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write validation_error.log: " << writeError << std::endl).str());
        return;
    }

    std::error_code flushError;
    if (!flushFileToDisk(errorLogPath, &flushError))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush validation_error.log: " << flushError.message() << std::endl).str());
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

std::string shellSeconds(double seconds)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << seconds;
    return stream.str();
}

std::optional<std::vector<uint8_t>> captureTactileGrayFrame(const std::vector<std::string> &inputArguments,
                                                            double seekSeconds,
                                                            std::string *errorMessage)
{
    if (!commandExists("ffmpeg"))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffmpeg not found in PATH";
        }
        return std::nullopt;
    }

    std::vector<std::string> arguments = {
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
    };
    arguments.insert(arguments.end(), inputArguments.begin(), inputArguments.end());
    if (seekSeconds > 0.0)
    {
        arguments.push_back("-ss");
        arguments.push_back(shellSeconds(seekSeconds));
    }
    arguments.push_back("-frames:v");
    arguments.push_back("1");
    arguments.push_back("-vf");
    arguments.push_back("crop=iw*0.84:ih*0.84:iw*0.08:ih*0.08,boxblur=2:1,scale=160:120,format=gray");
    arguments.push_back("-f");
    arguments.push_back("rawvideo");
    arguments.push_back("-");

    const CommandCaptureResult capture = runCommandCapture(arguments, kTactileSnapshotTimeoutMs);
    if (capture.timedOut)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffmpeg timed out";
        }
        return std::nullopt;
    }
    if (!capture.success)
    {
        if (errorMessage != nullptr)
        {
            std::string detail = capture.output;
            detail.erase(std::remove(detail.begin(), detail.end(), '\r'), detail.end());
            while (!detail.empty() &&
                   (detail.back() == '\n' || detail.back() == ' ' || detail.back() == '\t'))
            {
                detail.pop_back();
            }
            *errorMessage = detail.empty() ? "ffmpeg failed" : detail;
        }
        return std::nullopt;
    }

    std::vector<uint8_t> bytes(capture.output.begin(), capture.output.end());
    if (bytes.size() != kTactileFrameBytes)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "unexpected frame bytes=" + std::to_string(bytes.size());
        }
        return std::nullopt;
    }
    return bytes;
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

    json *entry = findJsonPath(calibrationJson, target.jsonPath, true);
    if (entry == nullptr)
    {
        return;
    }
    if (!entry->is_object())
    {
        *entry = json::object();
    }

    const std::string previous = entry->value("serial", std::string());
    if (previous.empty())
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "tactile serial missing in source calibration, inject runtime value:"
                  << " camera=" << target.cameraName
                  << " device=" << target.devicePath
                  << " serial=" << runtimeSerial << std::endl).str());
    }
    else if (previous == target.serialPlaceholder)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "tactile serial placeholder resolved at runtime:"
                  << " camera=" << target.cameraName
                  << " placeholder=" << target.serialPlaceholder
                  << " device=" << target.devicePath
                  << " serial=" << runtimeSerial << std::endl).str());
    }
    else if (previous != runtimeSerial)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "tactile serial mismatch, correcting episode calibration only:"
                  << " camera=" << target.cameraName
                  << " source_serial=" << previous
                  << " runtime_serial=" << runtimeSerial
                  << " source_file=" << sourceFile
                  << " persist_rewritten=false" << std::endl).str());
    }
    else
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "tactile serial matches runtime hardware:"
                  << " camera=" << target.cameraName
                  << " device=" << target.devicePath
                  << " serial=" << runtimeSerial << std::endl).str());
    }

    if (!probeDetail.empty())
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "tactile serial probe detail:"
                  << " camera=" << target.cameraName
                  << " detail=" << probeDetail << std::endl).str());
    }

    replaceJsonStringValues(calibrationJson, target.serialPlaceholder, runtimeSerial);
    (*entry)["serial"] = runtimeSerial;

    if (sourceFile == persistCalibrationFile &&
        previous != runtimeSerial &&
        !previous.empty() &&
        previous != target.serialPlaceholder)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "persist calibration remains unchanged;"
                  << " episode calibration now uses runtime tactile serial for camera=" << target.cameraName
                  << std::endl).str());
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
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] ffprobe begin: file=" << filePath << std::endl).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "[PERF] ffprobe timeout: file=" << filePath
                  << " elapsed_ms=" << (steadyNowMs() - probeStartMs) << std::endl).str());
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe timeout after " + std::to_string(kVideoProbeTimeoutMs) + "ms";
        }
        return false;
    }
    if (!probe.success)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "[PERF] ffprobe failed: file=" << filePath
                  << " elapsed_ms=" << (steadyNowMs() - probeStartMs)
                  << " exit_code=" << probe.exitCode << std::endl).str());
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

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] ffprobe done: file=" << filePath
              << " elapsed_ms=" << (steadyNowMs() - probeStartMs)
              << " start_sec=" << formatSeconds(result->startTimeSec)
              << " duration_sec=" << formatSeconds(result->durationSec)
              << " span_sec=" << formatSeconds(result->spanSec) << std::endl).str());
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
    case RecordRuntime::LedState::Warning:
        return {GripperLedEffectState::Warning, progress};
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

RecordRuntime::LedState RecordRuntime::toLedState(ugripper::runtime::RuntimeLedState state)
{
    switch (state)
    {
    case ugripper::runtime::RuntimeLedState::Init:
        return LedState::Init;
    case ugripper::runtime::RuntimeLedState::Ready:
        return LedState::Ready;
    case ugripper::runtime::RuntimeLedState::Recording:
        return LedState::Recording;
    case ugripper::runtime::RuntimeLedState::Error1:
        return LedState::Error1;
    case ugripper::runtime::RuntimeLedState::Error2:
        return LedState::Error2;
    case ugripper::runtime::RuntimeLedState::Error3:
        return LedState::Error3;
    case ugripper::runtime::RuntimeLedState::Error4:
        return LedState::Error4;
    case ugripper::runtime::RuntimeLedState::Error5:
        return LedState::Error5;
    case ugripper::runtime::RuntimeLedState::CalibPre:
        return LedState::CalibPre;
    case ugripper::runtime::RuntimeLedState::CalibRun:
        return LedState::CalibRun;
    case ugripper::runtime::RuntimeLedState::CalibDone:
        return LedState::CalibDone;
    case ugripper::runtime::RuntimeLedState::Exit:
    default:
        return LedState::Exit;
    }
}

const char *RecordRuntime::ledStateName(LedState state)
{
    switch (state)
    {
    case LedState::Init:
        return "Init";
    case LedState::Ready:
        return "Ready";
    case LedState::Warning:
        return "Warning";
    case LedState::Recording:
        return "Recording";
    case LedState::Error1:
        return "Error1";
    case LedState::Error2:
        return "Error2";
    case LedState::Error3:
        return "Error3";
    case LedState::Error4:
        return "Error4";
    case LedState::Error5:
        return "Error5";
    case LedState::CalibPre:
        return "CalibPre";
    case LedState::CalibRun:
        return "CalibRun";
    case LedState::CalibDone:
        return "CalibDone";
    case LedState::Exit:
        return "Exit";
    }
    return "Unknown";
}

const char *RecordRuntime::hmiEventName(ugripper::runtime::HmiEventType event_type)
{
    switch (event_type)
    {
    case ugripper::runtime::HmiEventType::ShortUpPressed:
        return "ShortUpPressed";
    case ugripper::runtime::HmiEventType::ShortDownPressed:
        return "ShortDownPressed";
    case ugripper::runtime::HmiEventType::LongUpPressed:
        return "LongUpPressed";
    case ugripper::runtime::HmiEventType::LongDownPressed:
        return "LongDownPressed";
    case ugripper::runtime::HmiEventType::ShutdownPromptRequested:
        return "ShutdownPromptRequested";
    case ugripper::runtime::HmiEventType::ShutdownRequested:
        return "ShutdownRequested";
    }
    return "Unknown";
}

RecordRuntime::RecordRuntime(RecordRuntimeOptions options)
    : options_(std::move(options))
{
}

RecordRuntime::~RecordRuntime()
{
    requestStop();
    stopRecording(false, "shutdown");
    stopAudioPlayer();
    if (ledController_)
    {
        ledController_->stop();
    }
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "invalid CAMERA_CODEC, fallback to h264: " << options_.cameraCodec << std::endl).str());
        options_.cameraCodec = "h264";
    }

    chestCameraEnabled_ = !isFalseLikeValue(readEnvValue(options_.envFile, kChestCameraEnvKey));
    DM_LOG_INFO("{}", (::DA::utils::LogString() << kChestCameraEnvKey << "="
                         << (chestCameraEnabled_ ? "true" : "false") << std::endl).str());

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
        chestCameraEnabled_,
        options_.tactileStateDir,
        options_.persistCalibrationFile,
        options_.exampleCalibrationFile,
        options_.fallbackCalibrationFile,
        options_.stereoStatusFile,
        packageVersion_,
        updaterVersion_);

    if (!episodeManager_->initialize())
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to initialize episode manager for disk root: "
                  << options_.diskRoot << std::endl).str());
        return false;
    }

    if (!panelManager_.connect(options_.gripperPorts))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to connect any gripper HMI port" << std::endl).str());
        return false;
    }

    ledController_ = std::make_unique<HmiLedController>(&panelManager_);
    ledController_->start();
    setLedState(LedState::Init);

    hmiController_ = std::make_unique<ugripper::runtime::HmiController>(
        ugripper::runtime::HmiControllerOptions{
            .action_debounce_ms = kActionDebounceMs,
            .long_press_threshold_ms = kLongPressThresholdMs,
            .dual_long_press_threshold_ms = kDualLongPressThresholdMs,
            .shutdown_prompt_threshold_ms = kShutdownPromptThresholdMs,
        },
        &utils::CurrentSteadyMs);

    std::vector<std::string> audioPlayerArguments = resolvePythonCommand();
    audioPlayerArguments.push_back(options_.audioPlayScript);
    audioCoordinator_ = std::make_unique<ugripper::runtime::AudioCoordinator>(
        &processSupervisor_,
        ugripper::runtime::AudioCoordinatorOptions{
            .player_arguments = std::move(audioPlayerArguments),
            .audio_pipe = options_.audioPipe,
            .audio_ready_file = options_.audioReadyFile,
            .player_stop_timeout_ms = 1000,
            .restart_interval_ms = kAudioPlayerRestartIntervalMs,
            .ready_grace_ms = kAudioPlayerReadyGraceMs,
            .startup_timeout_ms = 5000,
        },
        &utils::CurrentSteadyMs);

    stereoSessionClient_ = std::make_unique<ugripper::runtime::StereoSessionClient>(
        &processSupervisor_,
        ugripper::runtime::StereoSessionClientOptions{
            .daemon_arguments =
                {
                    options_.faysStereoDaemonScript,
                    "--control-file",
                    options_.stereoControlFile,
                    "--status-file",
                    options_.stereoStatusFile,
                    "--left-config",
                    options_.leftFaysConfig,
                    "--right-config",
                    options_.rightFaysConfig,
                    "--left-fifo",
                    options_.leftFaysControlFifo,
                    "--right-fifo",
                    options_.rightFaysControlFifo,
                },
            .control_file = options_.stereoControlFile,
            .status_file = options_.stereoStatusFile,
            .daemon_stop_timeout_ms = 2000,
            .restart_interval_ms = kStereoDaemonRestartIntervalMs,
            .finalize_wait_poll_ms = kStereoFinalizeWaitPollMs,
        },
        &utils::CurrentSteadyMs);
    shutdownRequestPort_ =
        ugripper::runtime::CreateFileShutdownRequestPort(
            options_.systemActionRequestFile,
            options_.systemActionResultFile);

    const auto criticalDevicePaths = activeCriticalDevicePaths(chestCameraEnabled_);
    healthMonitor_ = std::make_unique<ugripper::runtime::HealthMonitor>(
        ugripper::runtime::HealthMonitorOptions{
            .disk_root = options_.diskRoot,
            .stereo_status_file = options_.stereoStatusFile,
            .critical_device_paths =
                std::vector<std::string>(criticalDevicePaths.begin(), criticalDevicePaths.end()),
            .poll_interval_ms = kHealthCheckIntervalMs,
            .hmi_active_timeout_ms = kHmiActiveTimeoutMs,
        },
        ugripper::runtime::HealthMonitor::Dependencies{
            .is_disk_writable =
                [](const std::string& path) {
                    return access(path.c_str(), W_OK) == 0;
                },
            .path_exists =
                [](const std::string& path) {
                    return fs::exists(path);
                },
            .get_process_status =
                [this](ugripper::runtime::WorkerName worker) {
                    return processSupervisor_.GetStatus(worker);
                },
            .get_hmi_health =
                [this](uint64_t active_timeout_ms) {
                    const auto health = panelManager_.getHealthSnapshot(active_timeout_ms);
                    return ugripper::runtime::HmiHealthSnapshot{
                        .has_connected_device = health.hasConnectedDevice,
                        .input_connected = health.inputConnected,
                        .input_active = health.inputActive,
                        .input_last_rx_age_ms = health.inputLastRxAgeMs,
                        .disconnected_ports = health.disconnectedPorts,
                        .inactive_ports = health.inactivePorts,
                        .inactive_port_details = health.inactivePortDetails,
                        .port_activity = health.portActivity,
                    };
                },
        },
        &utils::CurrentSteadyMs);

    recordingOrchestrator_ = std::make_unique<ugripper::runtime::RecordingOrchestrator>(
        ugripper::runtime::RecordingOrchestratorOptions{
            .camera_recorder_bin = options_.cameraRecorderBin,
            .sensor_recorder_bin = options_.sensorRecorderBin,
            .camera_codec = options_.cameraCodec,
            .session_camera_streams_csv = joinCsv(sessionCameraStreams(chestCameraEnabled_)),
            .worker_stop_timeout_ms = 5000,
            .stereo_finalize_timeout_ms = 10000,
        },
        ugripper::runtime::RecordingOrchestrator::Dependencies{
            .create_next_episode_dir =
                [this]() {
                    return episodeManager_ != nullptr ? episodeManager_->createNextEpisodeDir() : std::string();
                },
            .prepare_episode =
                [this](const std::string& episode_dir,
                       bool reset_recording,
                       const std::string& reset_source_dir,
                       std::string* error_message) {
                    return episodeManager_ != nullptr &&
                           episodeManager_->prepareEpisode(
                               episode_dir, reset_recording, reset_source_dir, error_message);
                },
            .validate_episode =
                [this](const std::string& episode_dir, std::string* error_message) {
                    tactileWarningActive_ = false;
                    tactileTriggeredAudioCommand_.clear();
                    if (episodeManager_ == nullptr)
                    {
                        return false;
                    }
                    std::vector<EpisodeManager::TactileValidationFinding> findings;
                    const bool valid = episodeManager_->validateEpisode(episode_dir, error_message, &findings);
                    for (const auto &finding : findings)
                    {
                        if (tactileTriggeredAudioCommand_.empty() && finding.warningTriggered)
                        {
                            tactileTriggeredAudioCommand_ = finding.audioCommand;
                        }
                        if (finding.warningActive)
                        {
                            tactileWarningActive_ = true;
                        }
                    }
                    return valid;
                },
            .prepare_sensor_start =
                [this](std::vector<std::string>* extra_args, std::vector<int>* inherited_fds) {
                    if (extra_args == nullptr || inherited_fds == nullptr)
                    {
                        return;
                    }
                    motionAlertWriteFd_ = -1;
                    if (!startMotionAlertPipe(&motionAlertWriteFd_))
                    {
                        return;
                    }
                    extra_args->push_back("--motion-alert-fd");
                    extra_args->push_back(std::to_string(motionAlertWriteFd_));
                    inherited_fds->push_back(motionAlertWriteFd_);
                },
            .finalize_sensor_start =
                [this]() {
                    if (motionAlertWriteFd_ >= 0)
                    {
                        close(motionAlertWriteFd_);
                        motionAlertWriteFd_ = -1;
                    }
                },
            .attach_pending_pre_audio =
                [this](const std::string& episode_dir) {
                    return attachPendingPreAudio(episode_dir);
                },
            .merge_episode_info =
                [this](const std::string& episode_dir, std::string* error_message) {
                    return mergeEpisodeInfo(episode_dir, error_message);
                },
            .path_exists =
                [](const std::string& path) {
                    return fs::exists(path);
                },
            .set_audio_recovery_command =
                [this](const std::string& command) {
                    setAudioRecoveryCommand(command);
                },
            .send_audio_command =
                [this](const std::string& command) {
                    sendAudioCommand(command);
                },
            .set_led_state =
                [this](ugripper::runtime::RuntimeLedState state, double progress) {
                    setLedState(toLedState(state), progress);
                },
            .start_stereo_session =
                [this](const std::string& episode_dir, int64_t start_system_time_us, std::string* error_message) {
                    if (stereoSessionClient_ == nullptr)
                    {
                        if (error_message != nullptr)
                        {
                            *error_message = "stereo session client not initialized";
                        }
                        return false;
                    }
                    return stereoSessionClient_->StartSession(
                        episode_dir, start_system_time_us, error_message);
                },
            .stop_stereo_session =
                [this](const std::string& episode_dir, int64_t stop_system_time_us, std::string* error_message) {
                    if (stereoSessionClient_ == nullptr)
                    {
                        if (error_message != nullptr)
                        {
                            *error_message = "stereo session client not initialized";
                        }
                        return false;
                    }
                    return stereoSessionClient_->StopSession(
                        episode_dir, stop_system_time_us, error_message);
                },
            .wait_for_stereo_finalize =
                [this](const std::string& episode_dir, int timeout_ms, std::string* error_message) {
                    return waitForStereoFinalize(episode_dir, timeout_ms, error_message);
                },
            .flush_episode_artifacts =
                [this](const std::string& episode_dir, const char* stage) {
                    flushEpisodeArtifactsToDisk(fs::path(episode_dir), chestCameraEnabled_, stage);
                },
            .write_validation_error_log =
                [](const std::string& episode_dir, const std::string& error_message) {
                    writeValidationErrorLog(fs::path(episode_dir), error_message);
                },
            .start_worker =
                [this](ugripper::runtime::WorkerName worker,
                       const ugripper::runtime::ProcessSpec& spec,
                       std::string* error_message) {
                    return processSupervisor_.Start(worker, spec, error_message);
                },
            .stop_worker =
                [this](ugripper::runtime::WorkerName worker,
                       const std::string& reason,
                       std::string* error_message) {
                    return processSupervisor_.Stop(worker, reason, error_message);
                },
            .get_worker_status =
                [this](ugripper::runtime::WorkerName worker) {
                    return processSupervisor_.GetStatus(worker);
                },
            .log_info =
                [](const std::string& message) {
                    DM_LOG_INFO("{}", (::DA::utils::LogString() << message << std::endl).str());
                },
            .log_warn =
                [](const std::string& message) {
                    DM_LOG_WARN("{}", (::DA::utils::LogString() << message << std::endl).str());
                },
            .log_error =
                [](const std::string& message) {
                    DM_LOG_ERROR("{}", (::DA::utils::LogString() << message << std::endl).str());
                },
            .sync_runtime_log =
                [this](const std::string& reason) {
                    syncRuntimeLogToDisk(reason.c_str());
                },
        },
        &currentEpochUs,
        &steadyNowMs);

    pendingGripperRefresh_.fill(false);

    for (const std::string side : {"left", "right"})
    {
        refreshGripperRuntimeStateForSide(side);
    }
    panelManager_.consumeConnectionEvents();

    startAudioPlayer();
    startStereoDaemon();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    applyIdleState();
    if (!tactileWarningActive_)
    {
        setAudioRecoveryCommand("ready");
        sendAudioCommand("ready");
    }

    initialized_ = true;
    return true;
}

int RecordRuntime::run()
{
    if (!initialized_)
    {
        return 1;
    }

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "record_runtime started" << std::endl).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "device_sn=" << deviceSn_ << std::endl).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "episode_root=" << episodeManager_->dataRoot() << std::endl).str());

    while (!stopRequested_.load())
    {
        const uint64_t loopStartMs = currentSteadyMs();
        maintainAudioPlayer();
        maintainStereoDaemon();
        pollMotionAlertPipe();
        monitorHardwareHealth();

        ButtonSnapshot buttons;
        panelManager_.poll(options_.pollMs, &buttons);
        handleGripperConnectionEvents();
        processPendingGripperRefreshes();
        handleButtons(buttons);
        ButtonSnapshot leftButtons;
        if (panelManager_.getButtonsForPortToken("left_gripper", &leftButtons))
        {
            handleLeftButtons(leftButtons);
        }

        if (isRecordingActive() && !checkRecorderProcesses())
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
    stopMotionAlertPipe();
    stopStereoDaemon();
    setLedState(LedState::Exit);
    return 0;
}

void RecordRuntime::requestStop()
{
    stopRequested_.store(true);
}

bool RecordRuntime::startAudioPlayer()
{
    if (!fs::exists(options_.audioPlayScript))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio play script not found: " << options_.audioPlayScript << std::endl).str());
        return false;
    }
    std::string errorMessage;
    const bool started = audioCoordinator_ != nullptr && audioCoordinator_->StartAudioPlayer(&errorMessage);
    return started;
}

void RecordRuntime::stopAudioPlayer()
{
    if (audioCoordinator_ != nullptr)
    {
        audioCoordinator_->StopAudioPlayer();
    }
}

void RecordRuntime::setAudioRecoveryCommand(std::string command)
{
    audioRecoveryCommand_ = std::move(command);
    if (audioCoordinator_ != nullptr)
    {
        audioCoordinator_->SetRecoveryCommand(audioRecoveryCommand_);
    }
}

void RecordRuntime::maintainAudioPlayer()
{
    if (audioCoordinator_ != nullptr)
    {
        audioCoordinator_->MaintainAudioPlayer();
    }
}

bool RecordRuntime::startStereoDaemon()
{
    std::string errorMessage;
    const bool started = stereoSessionClient_ != nullptr && stereoSessionClient_->StartDaemon(&errorMessage);
    if (!started)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to start stereo daemon"
                             << (errorMessage.empty() ? "" : (": " + errorMessage))
                             << std::endl).str());
    }
    return started;
}

void RecordRuntime::stopStereoDaemon()
{
    if (stereoSessionClient_ != nullptr)
    {
        stereoSessionClient_->StopDaemon();
    }
}

void RecordRuntime::maintainStereoDaemon()
{
    if (stereoSessionClient_ != nullptr)
    {
        stereoSessionClient_->MaintainDaemon();
    }
}

bool RecordRuntime::writeStereoControl(bool recording,
                                       const std::string &episodeDir,
                                       int64_t startSystemTimeUs,
                                       int64_t stopSystemTimeUs)
{
    std::string errorMessage;
    bool ok = false;
    if (stereoSessionClient_ != nullptr)
    {
        if (recording)
        {
            ok = stereoSessionClient_->StartSession(episodeDir, startSystemTimeUs, &errorMessage);
        }
        else
        {
            ok = stereoSessionClient_->StopSession(episodeDir, stopSystemTimeUs, &errorMessage);
        }
    }
    if (!ok)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write stereo control file: " << errorMessage << std::endl).str());
    }
    return ok;
}

bool RecordRuntime::waitForStereoFinalize(const std::string &episodeDir, int timeoutMs, std::string *errorMessage)
{
    if (stereoSessionClient_ == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "stereo session client not initialized";
        }
        return false;
    }
    return stereoSessionClient_->WaitForFinalize(episodeDir, timeoutMs, errorMessage);
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
    const std::string reasonSuffix =
        (reason != nullptr && *reason != '\0') ? " after " + std::string(reason) : std::string();
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to scan runtime log directory for sync" << reasonSuffix
                             << ": " << iterError.message()).str());
        return false;
    }

    if (!foundSource)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "no runtime log file found for sync" << reasonSuffix
                             << ": prefix=" << filePrefix).str());
        return false;
    }

    const fs::path diskRoot(options_.diskRoot);
    std::error_code diskRootError;
    if (!fs::exists(diskRoot, diskRootError) || !fs::is_directory(diskRoot, diskRootError))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "skip runtime log sync" << reasonSuffix
                             << ": disk root unavailable: " << diskRoot).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "skip runtime log sync" << reasonSuffix
                             << ": disk root is not mounted: " << diskRoot).str());
        return false;
    }

    if (access(diskRoot.c_str(), W_OK) != 0)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "skip runtime log sync" << reasonSuffix
                             << ": disk root not writable: " << diskRoot
                             << " error=" << std::strerror(errno)).str());
        return false;
    }

    const fs::path destDir = diskRoot / "logs";
    std::error_code mkdirError;
    fs::create_directories(destDir, mkdirError);
    if (mkdirError)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to create runtime log directory" << reasonSuffix
                             << ": " << destDir << " error=" << mkdirError.message()).str());
        return false;
    }

    const fs::path destPath = destDir / sourcePath.filename();
    const std::string sourceStem = sourcePath.stem().string();
    const fs::path statePath = fs::path("/tmp") / (sourceStem + ".pos");
    const fs::path lockPath = fs::path("/tmp") / (sourceStem + ".lock");

    const int lockFd = open(lockPath.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (lockFd < 0)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to open runtime log sync lock" << reasonSuffix
                             << ": " << lockPath << " error=" << std::strerror(errno)).str());
        return false;
    }

    const auto closeLock = [&]() {
        flock(lockFd, LOCK_UN);
        close(lockFd);
    };

    if (flock(lockFd, LOCK_EX) != 0)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to acquire runtime log sync lock" << reasonSuffix
                             << ": " << lockPath << " error=" << std::strerror(errno)).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to open runtime log source" << reasonSuffix
                             << ": " << sourcePath << " error=" << std::strerror(errno)).str());
        closeLock();
        return false;
    }

    struct stat sourceStat
    {
    };
    if (fstat(srcFd, &sourceStat) != 0)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to stat runtime log source" << reasonSuffix
                             << ": " << sourcePath << " error=" << std::strerror(errno)).str());
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
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "runtime log already synced" << reasonSuffix
                             << ": " << destPath).str());
        return true;
    }

    const int destFd = open(destPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (destFd < 0)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to open runtime log destination" << reasonSuffix
                             << ": " << destPath << " error=" << std::strerror(errno)).str());
        close(srcFd);
        closeLock();
        return false;
    }

    if (lseek(srcFd, static_cast<off_t>(lastPos), SEEK_SET) < 0)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to seek runtime log source" << reasonSuffix
                             << ": " << sourcePath << " error=" << std::strerror(errno)).str());
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
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to read runtime log source" << reasonSuffix
                                 << ": " << sourcePath << " error=" << std::strerror(errno)).str());
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
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to append runtime log destination" << reasonSuffix
                                     << ": " << destPath << " error=" << std::strerror(errno)).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush runtime log destination" << reasonSuffix
                             << ": " << destPath << " error=" << std::strerror(errno)).str());
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to update runtime log sync state" << reasonSuffix
                             << ": " << stateWriteError).str());
        closeLock();
        return false;
    }

    std::error_code flushError;
    if (!flushDirectoryToDisk(destDir, &flushError))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush runtime log directory" << reasonSuffix
                             << ": " << destDir << " error=" << flushError.message()).str());
    }
    if (!flushDirectoryToDisk(diskRoot, &flushError))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush disk root after runtime log sync" << reasonSuffix
                             << ": " << diskRoot << " error=" << flushError.message()).str());
    }

    closeLock();

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "synced runtime log to disk" << reasonSuffix
                         << ": src=" << sourcePath
                         << " dest=" << destPath
                         << " bytes=" << (currentSize - lastPos)).str());
    return true;
}

bool RecordRuntime::startMotionAlertPipe(int *writeFd)
{
    stopMotionAlertPipe();

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
    {
        return false;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL, 0) | O_NONBLOCK);

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
    if (motionAlertWriteFd_ >= 0)
    {
        close(motionAlertWriteFd_);
        motionAlertWriteFd_ = -1;
    }
    clearMotionAlertOutputs();
}

void RecordRuntime::clearMotionAlertOutputs()
{
    leftMotionAlertActive_ = false;
    rightMotionAlertActive_ = false;
    panelManager_.silenceBeep();
}

void RecordRuntime::applyMotionAlertState(const std::string &side, bool active)
{
    bool *currentState = nullptr;
    if (side == "left")
    {
        currentState = &leftMotionAlertActive_;
    }
    else if (side == "right")
    {
        currentState = &rightMotionAlertActive_;
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

void RecordRuntime::handleGripperConnectionEvents()
{
    bool updatedAny = false;
    for (const auto &event : panelManager_.consumeConnectionEvents())
    {
        if (event.side.empty())
        {
            continue;
        }

        const size_t index = gripperStateIndexForSide(event.side);
        auto &runtimeState = gripperRuntimeStates_[index];
        ugripper::runtime::GripperRefreshRuntimeView view{
            .side = runtimeState.side,
            .connected = runtimeState.connected,
            .calibration_valid = runtimeState.calibrationValid,
            .calibration_status = runtimeState.calibrationStatus,
            .calibration_source = runtimeState.calibrationSource,
            .last_error = runtimeState.lastError,
        };

        ugripper::runtime::ApplyGripperConnectionEvent(&view, &pendingGripperRefresh_[index], event.side, event.connected);
        runtimeState.side = view.side;
        runtimeState.connected = view.connected;
        runtimeState.calibrationValid = view.calibration_valid;
        runtimeState.calibrationStatus = view.calibration_status;
        runtimeState.calibrationSource = view.calibration_source;
        runtimeState.lastError = view.last_error;

        if (event.connected)
        {
            updatedAny = true;
            continue;
        }

        updatedAny = true;
    }

    if (updatedAny && episodeManager_ != nullptr)
    {
        episodeManager_->setGripperRuntimeStates(gripperRuntimeStates_);
    }
}

void RecordRuntime::processPendingGripperRefreshes()
{
    bool updatedAny = false;
    for (const std::string side : {"left", "right"})
    {
        const size_t index = gripperStateIndexForSide(side);
        if (!pendingGripperRefresh_[index])
        {
            continue;
        }

        auto &state = gripperRuntimeStates_[index];
        ugripper::runtime::GripperRefreshRuntimeView view{
            .side = state.side,
            .connected = state.connected,
            .calibration_valid = state.calibrationValid,
            .calibration_status = state.calibrationStatus,
            .calibration_source = state.calibrationSource,
            .last_error = state.lastError,
        };
        const auto action = ugripper::runtime::AdvancePendingGripperRefresh(
            &view,
            &pendingGripperRefresh_[index],
            side,
            areSideCriticalDevicesReady(side),
            panelManager_.isSideReadyForRefresh(side, kGripperRefreshActiveTimeoutMs));
        state.side = view.side;
        state.connected = view.connected;
        state.calibrationValid = view.calibration_valid;
        state.calibrationStatus = view.calibration_status;
        state.calibrationSource = view.calibration_source;
        state.lastError = view.last_error;

        if (action == ugripper::runtime::GripperRefreshAction::RequestState)
        {
            panelManager_.requestStateForSide(side);
            updatedAny = true;
            continue;
        }

        if (action == ugripper::runtime::GripperRefreshAction::RefreshRuntimeState)
        {
            refreshGripperRuntimeStateForSide(side);
        }
        updatedAny = true;
    }

    if (updatedAny && episodeManager_ != nullptr)
    {
        episodeManager_->setGripperRuntimeStates(gripperRuntimeStates_);
    }
}

void RecordRuntime::clearGripperRuntimeStateForSide(const std::string &side,
                                                    bool connected,
                                                    const std::string &status,
                                                    const std::string &errorMessage)
{
    auto &state = gripperRuntimeStates_[gripperStateIndexForSide(side)];
    state.side = side;
    state.connected = connected;
    state.hasSerialNumber = false;
    state.serialNumber.clear();
    state.calibrationValid = false;
    state.calibrationStatus = status;
    state.calibrationSource.clear();
    state.lastError = errorMessage;
    state.calibrationPayloadCached = false;
    state.calibrationPayload = {};
}

void RecordRuntime::pollMotionAlertPipe()
{
    if (motionAlertReadFd_ < 0)
    {
        return;
    }

    const auto motionAlertSideName = [](uint8_t side) {
        switch (static_cast<ugripper::MotionAlertSide>(side))
        {
        case ugripper::MotionAlertSide::Left:
            return "left";
        case ugripper::MotionAlertSide::Right:
            return "right";
        default:
            return "unknown";
        }
    };
    const auto motionAlertReasonName = [](uint8_t reason) {
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
        default:
            return "none";
        }
    };

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
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "motion overspeed detected: side=" << side
                                 << " reason=" << motionAlertReasonName(message.reason)
                                 << " gyro=" << std::fixed << std::setprecision(3) << message.gyroMagnitude
                                 << " accel_excess=" << std::fixed << std::setprecision(3) << message.accelExcess
                                 << std::endl).str());
        }
    }
}

void RecordRuntime::refreshGripperRuntimeStateForSide(const std::string &side)
{
    auto &state = gripperRuntimeStates_[gripperStateIndexForSide(side)];
    clearGripperRuntimeStateForSide(side, true, "refreshing", "");

    std::string serialNumber;
    bool hasSerialNumber = false;
    gripper_hmi::GripperCalibrationDataV1 calibrationData{};
    std::string runtimeReadError;
    if (!panelManager_.readRuntimeIdentityForSide(
            side, &serialNumber, &hasSerialNumber, &calibrationData, &runtimeReadError))
    {
        if (!runtimeReadError.empty())
        {
            state.lastError = runtimeReadError;
        }
        return;
    }

    state.connected = true;
    state.hasSerialNumber = hasSerialNumber;
    state.serialNumber = hasSerialNumber ? serialNumber : "";
    state.calibrationValid = true;
    state.calibrationStatus = hasSerialNumber ? "calibrated" : "calibrated_no_sn";
    state.calibrationSource = "gripper_runtime_cache";
    state.calibrationPayloadCached = true;
    state.calibrationPayload = calibrationData;
    state.lastError.clear();
    refreshTactileReferenceCachesForSide(side);
}

void RecordRuntime::refreshTactileReferenceCachesForSide(const std::string &side)
{
    if (episodeManager_ == nullptr)
    {
        return;
    }
    std::vector<EpisodeManager::TactileValidationFinding> findings;
    episodeManager_->refreshTactileReferenceCacheForSide(side, &findings);
    for (const auto &finding : findings)
    {
        if (tactileTriggeredAudioCommand_.empty() && finding.warningTriggered)
        {
            tactileTriggeredAudioCommand_ = finding.audioCommand;
        }
        if (finding.warningActive)
        {
            tactileWarningActive_ = true;
        }
    }
}

void RecordRuntime::applyIdleState()
{
    setLedState(tactileWarningActive_ ? LedState::Warning : LedState::Ready);
}

bool RecordRuntime::areSideCriticalDevicesReady(const std::string &side) const
{
    const auto &paths = side == "left" ? kLeftCriticalDevicePaths : kRightCriticalDevicePaths;
    for (const char *path : paths)
    {
        std::error_code error;
        if (!fs::exists(path, error))
        {
            return false;
        }
    }
    return true;
}

void RecordRuntime::sendAudioCommand(const std::string &command) const
{
    if (audioCoordinator_ == nullptr)
    {
        return;
    }
    audioCoordinator_->SendCommand(command);
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to move pre audio into episode: " << error.message() << std::endl).str());
        return false;
    }

    if (!flushFileToDisk(target, &error))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush pre audio into episode: " << error.message() << std::endl).str());
    }

    pendingPreAudioFile_.clear();
    return true;
}

bool RecordRuntime::startRecording(bool resetRecording)
{
    if (recordingOrchestrator_ == nullptr)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "recording orchestrator not initialized" << std::endl).str());
        return false;
    }
    tactileTriggeredAudioCommand_.clear();
    return recordingOrchestrator_->StartRecording(resetRecording, nullptr);
}

bool RecordRuntime::stopRecording(bool dueToError, const std::string &reason)
{
    if (recordingOrchestrator_ == nullptr)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "recording orchestrator not initialized" << std::endl).str());
        return false;
    }
    const bool ok = recordingOrchestrator_->StopRecording(dueToError, reason, nullptr);
    stopMotionAlertPipe();
    if (ok && !dueToError)
    {
        if (!tactileTriggeredAudioCommand_.empty())
        {
            setAudioRecoveryCommand(tactileWarningActive_ ? "" : tactileTriggeredAudioCommand_);
            sendAudioCommand(tactileTriggeredAudioCommand_);
        }
        else if (!tactileWarningActive_)
        {
            setAudioRecoveryCommand("ready");
        }
        applyIdleState();
    }
    return ok;
}

bool RecordRuntime::handleShortUpAction()
{
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "BTN_UP short press" << std::endl).str());
    if (!isRecordingActive())
    {
        if (healthState_.status == ugripper::runtime::HealthStatus::Error)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "ignore start recording while hardware health is in error state" << std::endl).str());
            sendAudioCommand("error");
            return false;
        }
        return startRecording(false);
    }
    return stopRecording(false, "BTN_UP short stop");
}

bool RecordRuntime::handleShortDownAction()
{
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "BTN_DOWN short press" << std::endl).str());
    if (!isRecordingActive())
    {
        if (healthState_.status == ugripper::runtime::HealthStatus::Error)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "ignore reset recording while hardware health is in error state" << std::endl).str());
            sendAudioCommand("error");
            return false;
        }
        if (lastEpisodeDir().empty() || !fs::exists(lastEpisodeDir()))
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "no previous episode, BTN_DOWN reset ignored" << std::endl).str());
            setAudioRecoveryCommand(tactileWarningActive_ ? "" : "ready");
            sendAudioCommand("no_reset_needed");
            applyIdleState();
            return false;
        }
        return startRecording(true);
    }
    return stopRecording(false, "BTN_DOWN short stop");
}

bool RecordRuntime::handleLongUpAction()
{
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "BTN_UP long press" << std::endl).str());
    if (isRecordingActive())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "ignore pre-audio while recording" << std::endl).str());
        return false;
    }
    return recordAudioClip("pre", true);
}

bool RecordRuntime::handleLongDownAction()
{
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "BTN_DOWN long press" << std::endl).str());
    if (isRecordingActive())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "ignore post-audio while recording" << std::endl).str());
        return false;
    }
    return recordAudioClip("post", false);
}

void RecordRuntime::handleDualShutdownAction()
{
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "dual-button shutdown requested" << std::endl).str());
    if (isRecordingActive())
    {
        stopRecording(false, "dual-button shutdown");
    }
    setLedState(LedState::Exit);
    setAudioRecoveryCommand("writing");
    sendAudioCommand("writing");

    std::string errorMessage;
    if (shutdownRequestPort_ == nullptr ||
        !shutdownRequestPort_->RequestAction("shutdown", &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to request shutdown action: "
                              << options_.systemActionRequestFile
                              << ", error=" << errorMessage << std::endl).str());
        setLedState(LedState::Error5);
        return;
    }
    stopRequested_.store(true);
}

bool RecordRuntime::handleLeftDualUmountAction()
{
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "left dual-button umount requested" << std::endl).str());
    if (isRecordingActive())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "ignore left dual-button umount while recording" << std::endl).str());
        setAudioRecoveryCommand("recording");
        sendAudioCommand("error");
        return false;
    }

    setLedState(LedState::Init);
    setAudioRecoveryCommand("writing");
    sendAudioCommand("writing");
    syncRuntimeLogToDisk("left dual-button umount");

    std::string errorMessage;
    if (shutdownRequestPort_ == nullptr ||
        !shutdownRequestPort_->RequestAction("umount", &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to request umount: " << errorMessage << std::endl).str());
        setAudioRecoveryCommand(tactileWarningActive_ ? "" : "ready");
        applyIdleState();
        sendAudioCommand("error");
        return false;
    }

    std::string result;
    if (!shutdownRequestPort_->WaitForActionResult(
            &result,
            static_cast<int>(kSystemActionResultWaitMs),
            &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to wait for umount result: " << errorMessage << std::endl).str());
        setAudioRecoveryCommand(tactileWarningActive_ ? "" : "ready");
        applyIdleState();
        sendAudioCommand("error");
        return false;
    }

    if (result == "ok")
    {
        setAudioRecoveryCommand("umount");
        sendAudioCommand("umount");
        std::this_thread::sleep_for(std::chrono::milliseconds(kPostUmountAudioDelayMs));
        stopRequested_.store(true);
        return true;
    }

    DM_LOG_ERROR("{}", (::DA::utils::LogString() << "umount request failed with result: " << result << std::endl).str());
    setAudioRecoveryCommand(tactileWarningActive_ ? "" : "ready");
    applyIdleState();
    sendAudioCommand("error");
    return false;
}

bool RecordRuntime::recordAudioClip(const std::string &audioType, bool monitorUpButton)
{
    if (!fs::exists(options_.audioRecordScript))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "audio record script not found: " << options_.audioRecordScript << std::endl).str());
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

    ugripper::runtime::SubprocessHandle audioRecorder("audio_recorder");
    std::vector<std::string> audioRecorderArgs = resolvePythonCommand();
    audioRecorderArgs.push_back(options_.audioRecordScript);
    audioRecorderArgs.push_back("--output");
    audioRecorderArgs.push_back(captureFile.string());
    if (!audioRecorder.Start(audioRecorderArgs))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to start audio recorder" << std::endl).str());
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

    audioRecorder.SendSignal(SIGINT);
    if (!audioRecorder.Wait(5000))
    {
        audioRecorder.Stop(ugripper::runtime::ProcessStopMode::SigTermThenKill, 1000, nullptr);
    }

    if (!fileExistsAndNotEmpty(captureFile.string()) || fs::file_size(captureFile) <= 44)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "audio capture is empty" << std::endl).str());
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
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to process audio clip" << std::endl).str());
        fs::remove(outputFile);
        return finishAudioRecording(false);
    }

    if (audioType == "pre")
    {
        pendingPreAudioFile_ = outputFile.string();
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "pre-audio prepared: " << pendingPreAudioFile_ << std::endl).str());
    }
    else
    {
        if (!lastEpisodeDir().empty() && fs::exists(lastEpisodeDir()))
        {
            std::error_code error;
            if (!moveFileWithCrossDeviceFallback(outputFile, fs::path(lastEpisodeDir()) / "audio_post.wav", &error))
            {
                DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to move post-audio into last episode: " << error.message() << std::endl).str());
                fs::remove(outputFile);
            }
            else if (!flushFileToDisk(fs::path(lastEpisodeDir()) / "audio_post.wav", &error))
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush post-audio into last episode: " << error.message() << std::endl).str());
            }
        }
        else
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "no previous episode for post-audio" << std::endl).str());
            fs::remove(outputFile);
        }
    }

    return finishAudioRecording(true);
}

void RecordRuntime::handleButtons(const ButtonSnapshot &buttons)
{
    if (hmiController_ == nullptr)
    {
        return;
    }

    if (!hasLastLoggedButtons_ ||
        lastLoggedButtons_.upPressed != buttons.upPressed ||
        lastLoggedButtons_.downPressed != buttons.downPressed)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "[HMI_DIAG] category=button_raw"
            << " up=" << boolText(buttons.upPressed)
            << " down=" << boolText(buttons.downPressed)).str());
        lastLoggedButtons_ = buttons;
        hasLastLoggedButtons_ = true;
    }

    const auto events = hmiController_->HandleButtons(
        ugripper::runtime::ButtonSnapshot{.up_pressed = buttons.upPressed, .down_pressed = buttons.downPressed});
    for (const auto &event : events)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "[HMI_DIAG] category=button_event"
            << " event=" << hmiEventName(event.type)
            << " up=" << boolText(buttons.upPressed)
            << " down=" << boolText(buttons.downPressed)).str());
        switch (event.type)
        {
        case ugripper::runtime::HmiEventType::ShortUpPressed:
            handleShortUpAction();
            break;
        case ugripper::runtime::HmiEventType::ShortDownPressed:
            handleShortDownAction();
            break;
        case ugripper::runtime::HmiEventType::LongUpPressed:
            handleLongUpAction();
            break;
        case ugripper::runtime::HmiEventType::LongDownPressed:
            handleLongDownAction();
            break;
        case ugripper::runtime::HmiEventType::ShutdownPromptRequested:
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "dual-button chord armed" << std::endl).str());
            sendAudioCommand("shutdown");
            break;
        case ugripper::runtime::HmiEventType::ShutdownRequested:
            handleDualShutdownAction();
            break;
        }
    }
}

void RecordRuntime::handleLeftButtons(const ButtonSnapshot &buttons)
{
    if (!hasLastLoggedLeftButtons_ ||
        lastLoggedLeftButtons_.upPressed != buttons.upPressed ||
        lastLoggedLeftButtons_.downPressed != buttons.downPressed)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "[HMI_DIAG] category=button_raw_left"
            << " up=" << boolText(buttons.upPressed)
            << " down=" << boolText(buttons.downPressed)).str());
        lastLoggedLeftButtons_ = buttons;
        hasLastLoggedLeftButtons_ = true;
    }

    const uint64_t nowMs = currentSteadyMs();
    if (buttons.upPressed && buttons.downPressed)
    {
        if (!leftDualChordActive_)
        {
            leftDualChordActive_ = true;
            leftBothPressedSinceMs_ = nowMs;
            leftDualLongHandled_ = false;
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "left dual-button chord armed" << std::endl).str());
        }

        const uint64_t dualHeldMs = nowMs - leftBothPressedSinceMs_;
        if (dualHeldMs >= kDualLongPressThresholdMs && !leftDualLongHandled_)
        {
            leftDualLongHandled_ = true;
            handleLeftDualUmountAction();
        }

        lastLeftButtons_ = buttons;
        return;
    }

    if (leftDualChordActive_ && !buttons.upPressed && !buttons.downPressed)
    {
        leftDualChordActive_ = false;
        leftBothPressedSinceMs_ = 0;
        leftDualLongHandled_ = false;
    }
    lastLeftButtons_ = buttons;
}

bool RecordRuntime::checkRecorderProcesses()
{
    return recordingOrchestrator_ != nullptr && recordingOrchestrator_->CheckRecorderProcesses();
}

void RecordRuntime::monitorHardwareHealth()
{
    if (healthMonitor_ == nullptr)
    {
        return;
    }

    const auto result = healthMonitor_->Poll(healthState_);
    healthState_ = result.state;
    if (!result.checked)
    {
        return;
    }

    if (result.fault.has_value())
    {
        if (result.should_notify_fault)
        {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "hardware health fault (" << result.fault->key << "): "
                                  << result.fault->detail << std::endl).str());
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "[HMI_DIAG] category=health_fault"
                << " key=" << result.fault->key
                << " led_state=" << ledStateName(toLedState(result.fault->led_state))).str());
            setLedState(toLedState(result.fault->led_state));
            sendAudioCommand("error");
        }
        return;
    }

    if (result.recovered)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "hardware health recovered" << std::endl).str());
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "[HMI_DIAG] category=health_recovered"
            << " recording=" << boolText(isRecordingActive())).str());
        if (isRecordingActive())
        {
            setLedState(LedState::Recording);
        }
        else
        {
            applyIdleState();
            if (!tactileWarningActive_)
            {
                sendAudioCommand("ready");
            }
        }
    }
}

bool RecordRuntime::isRecordingActive() const
{
    return recordingOrchestrator_ != nullptr && recordingOrchestrator_->state().is_recording;
}

const std::string &RecordRuntime::currentEpisodeDir() const
{
    static const std::string empty;
    if (recordingOrchestrator_ == nullptr)
    {
        return empty;
    }
    return recordingOrchestrator_->state().current_episode_dir;
}

const std::string &RecordRuntime::lastEpisodeDir() const
{
    static const std::string empty;
    if (recordingOrchestrator_ == nullptr)
    {
        return empty;
    }
    return recordingOrchestrator_->state().last_episode_dir;
}

void RecordRuntime::setLedState(LedState state, double progress)
{
    const bool progressChanged =
        !hasLastLoggedLedState_ || std::fabs(lastLoggedLedProgress_ - progress) > 0.001;
    if (!hasLastLoggedLedState_ || lastLoggedLedState_ != state || progressChanged)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "[HMI_DIAG] category=led_target"
            << " state=" << ledStateName(state)
            << " progress=" << std::fixed << std::setprecision(3) << progress).str());
        lastLoggedLedState_ = state;
        lastLoggedLedProgress_ = progress;
        hasLastLoggedLedState_ = true;
    }

    if (ledController_)
    {
        ledController_->setState(state, progress);
    }
}

std::string RecordRuntime::readEnvValue(const std::string &envFile, const std::string &key)
{
    return utils::ReadEnvValue(envFile, key);
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
    return utils::CurrentSteadyMs();
}

uint64_t RecordRuntime::currentEpochMs()
{
    return utils::CurrentEpochMs();
}

int64_t RecordRuntime::currentEpochUs()
{
    return utils::CurrentEpochUs();
}

bool RecordRuntime::fileExistsAndNotEmpty(const std::string &path)
{
    std::error_code error;
    return fs::exists(path, error) &&
           fs::is_regular_file(path, error) &&
           fs::file_size(path, error) > 0;
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

bool RecordRuntime::GripperPanelManager::connect(const std::vector<std::string> &ports)
{
    disconnect();
    drivers_.clear();
    reconnectAttemptMs_.clear();
    delayedStateRequestDueMs_.clear();
    lastKnownConnectedStates_.clear();
    pendingConnectionEvents_.clear();
    currentDriverBeepStates_.clear();
    inputDriverIndex_ = 0;
    hasDedicatedRightInput_ = false;
    hasLedEffect_ = false;
    hasDirectLedColor_ = false;

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
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to connect HMI port: " << ports[index] << std::endl).str());
        }
        drivers_.push_back(std::move(driver));
        reconnectAttemptMs_.push_back(0);
        delayedStateRequestDueMs_.push_back(0);
        lastKnownConnectedStates_.push_back(false);
        currentDriverBeepStates_.push_back({});
        recordConnectionEventIfChanged(index, drivers_.back() != nullptr && drivers_.back()->isConnected());
    }

    return hasConnectedDevice();
}

void RecordRuntime::GripperPanelManager::disconnect()
{
    drivers_.clear();
    reconnectAttemptMs_.clear();
    delayedStateRequestDueMs_.clear();
    lastKnownConnectedStates_.clear();
    pendingConnectionEvents_.clear();
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
        const std::string port_activity =
            driver->getPort() + "(age_ms=" + std::to_string(snapshot.lastRxAgeMs) + ",active=" +
            (snapshot.active ? "1" : "0") + ")";
        health.portActivity.push_back(port_activity);
        if (!snapshot.active)
        {
            health.inactivePorts.push_back(driver->getPort());
            health.inactivePortDetails.push_back(port_activity);
        }
        if (index == inputDriverIndex_)
        {
            health.inputActive = snapshot.active;
            health.inputLastRxAgeMs = snapshot.lastRxAgeMs;
        }
    }

    return health;
}

std::string RecordRuntime::GripperPanelManager::sideForPort(const std::string &port)
{
    if (port.find("right_gripper") != std::string::npos)
    {
        return "right";
    }
    if (port.find("left_gripper") != std::string::npos)
    {
        return "left";
    }
    return "";
}

int RecordRuntime::GripperPanelManager::findDriverIndexForSide(const std::string &side) const
{
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        const auto &driver = drivers_[index];
        if (driver != nullptr && sideForPort(driver->getPort()) == side)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void RecordRuntime::GripperPanelManager::recordConnectionEventIfChanged(size_t index, bool connected)
{
    if (index >= drivers_.size() || index >= lastKnownConnectedStates_.size() || drivers_[index] == nullptr)
    {
        return;
    }
    if (lastKnownConnectedStates_[index] == connected)
    {
        return;
    }

    lastKnownConnectedStates_[index] = connected;
    const std::string side = sideForPort(drivers_[index]->getPort());
    if (side.empty())
    {
        return;
    }
    pendingConnectionEvents_.push_back(ConnectionEvent{side, connected});
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

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "reconnected HMI port: " << drivers_[index]->getPort() << std::endl).str());
    recordConnectionEventIfChanged(index, true);
    if (index < delayedStateRequestDueMs_.size())
    {
        delayedStateRequestDueMs_[index] = nowMs + 1000;
    }
    if (index < currentDriverBeepStates_.size())
    {
        const auto &beepState = currentDriverBeepStates_[index];
        if (beepState.duty == 0)
        {
            drivers_[index]->silenceBeep();
        }
        else
        {
            drivers_[index]->setBeepState(beepState);
        }
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
        recordConnectionEventIfChanged(index, driver != nullptr && driver->isConnected());
        if (!driver->isConnected())
        {
            continue;
        }
        const uint64_t nowMs = RecordRuntime::currentSteadyMs();
        if (index < delayedStateRequestDueMs_.size() &&
            delayedStateRequestDueMs_[index] != 0 &&
            nowMs >= delayedStateRequestDueMs_[index])
        {
            driver->requestState();
            delayedStateRequestDueMs_[index] = 0;
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

bool RecordRuntime::GripperPanelManager::getButtonsForPortToken(const std::string &token,
                                                                ButtonSnapshot *snapshot) const
{
    if (snapshot != nullptr)
    {
        *snapshot = {};
    }
    if (token.empty())
    {
        return false;
    }

    for (const auto &driver : drivers_)
    {
        if (driver == nullptr || !driver->isConnected())
        {
            continue;
        }
        if (driver->getPort().find(token) == std::string::npos)
        {
            continue;
        }

        const auto state = driver->getSnapshot();
        if (snapshot != nullptr)
        {
            if (state.keyPressed.size() > kBtnUpKeyIndex)
            {
                snapshot->upPressed = state.keyPressed[kBtnUpKeyIndex];
            }
            if (state.keyPressed.size() > kBtnDownKeyIndex)
            {
                snapshot->downPressed = state.keyPressed[kBtnDownKeyIndex];
            }
        }
        return true;
    }
    return false;
}

bool RecordRuntime::GripperPanelManager::requestStateForSide(const std::string &side)
{
    const int index = findDriverIndexForSide(side);
    if (index < 0)
    {
        return false;
    }

    auto &driver = drivers_[static_cast<size_t>(index)];
    return driver != nullptr && driver->isConnected() && driver->requestState();
}

bool RecordRuntime::GripperPanelManager::readRuntimeIdentityForSide(
    const std::string &side,
    std::string *serialNumber,
    bool *hasSerialNumber,
    gripper_hmi::GripperCalibrationDataV1 *calibrationData,
    std::string *errorMessage)
{
    if (serialNumber != nullptr)
    {
        serialNumber->clear();
    }
    if (hasSerialNumber != nullptr)
    {
        *hasSerialNumber = false;
    }
    if (calibrationData == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "calibration output is null";
        }
        return false;
    }

    const int index = findDriverIndexForSide(side);
    if (index < 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "no HMI driver for side=" + side;
        }
        return false;
    }

    auto &driver = drivers_[static_cast<size_t>(index)];
    if (driver == nullptr || !driver->isConnected())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "HMI driver disconnected for side=" + side;
        }
        return false;
    }

    const bool ok = driver->readSerialNumberAndCalibration(serialNumber, hasSerialNumber, calibrationData);
    if (!ok)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = driver->getLastCommandError().empty()
                                ? "readSerialNumberAndCalibration failed"
                                : driver->getLastCommandError();
        }
        return false;
    }

    if (errorMessage != nullptr)
    {
        if (hasSerialNumber != nullptr && !*hasSerialNumber)
        {
            *errorMessage = driver->getLastCommandError();
        }
        else
        {
            errorMessage->clear();
        }
    }
    return true;
}

std::vector<RecordRuntime::GripperPanelManager::ConnectionEvent> RecordRuntime::GripperPanelManager::consumeConnectionEvents()
{
    std::vector<ConnectionEvent> events = pendingConnectionEvents_;
    pendingConnectionEvents_.clear();
    return events;
}

bool RecordRuntime::GripperPanelManager::isSideReadyForRefresh(const std::string &side, uint64_t activeTimeoutMs) const
{
    const int index = findDriverIndexForSide(side);
    if (index < 0)
    {
        return false;
    }

    const auto &driver = drivers_[static_cast<size_t>(index)];
    if (driver == nullptr || !driver->isConnected())
    {
        return false;
    }

    const auto snapshot = driver->getSnapshot(activeTimeoutMs);
    return snapshot.active;
}

bool RecordRuntime::GripperPanelManager::setBeepEnabledForSide(const std::string &side, bool enabled)
{
    const GripperBeepState nextState = enabled
                                           ? GripperBeepState{
                                                 GripperHmiDriver::kDefaultBeepDuty,
                                                 GripperHmiDriver::kDefaultBeepFrequency,
                                             }
                                           : GripperBeepState{0, 0};
    bool wroteAny = false;
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        auto &driver = drivers_[index];
        if (index < currentDriverBeepStates_.size() &&
            sideForPort(driver != nullptr ? driver->getPort() : std::string()) == side)
        {
            currentDriverBeepStates_[index] = nextState;
        }
        if (driver == nullptr || !driver->isConnected())
        {
            continue;
        }
        if (sideForPort(driver->getPort()) != side)
        {
            continue;
        }
        wroteAny = enabled ? (driver->setBeepState(nextState) || wroteAny)
                           : (driver->silenceBeep() || wroteAny);
    }
    return wroteAny;
}

bool RecordRuntime::GripperPanelManager::silenceBeepForSide(const std::string &side)
{
    return setBeepEnabledForSide(side, false);
}

void RecordRuntime::GripperPanelManager::silenceBeep()
{
    for (size_t index = 0; index < drivers_.size(); ++index)
    {
        if (index < currentDriverBeepStates_.size())
        {
            currentDriverBeepStates_[index] = GripperBeepState{0, 0};
        }
        auto &driver = drivers_[index];
        if (driver != nullptr && driver->isConnected())
        {
            driver->silenceBeep();
        }
    }
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
                                              bool chestCameraEnabled,
                                              std::string tactileStateDir,
                                              std::string persistCalibrationFile,
                                              std::string exampleCalibrationFile,
                                              std::string fallbackCalibrationFile,
                                              std::string stereoStatusFile,
                                              std::string packageVersion,
                                              std::string updaterVersion)
    : diskRoot_(std::move(diskRoot)),
      deviceSn_(std::move(deviceSn)),
      deviceSnLower_(RecordRuntime::toLower(deviceSn_)),
      language_(std::move(language)),
      cameraCodec_(std::move(cameraCodec)),
      chestCameraEnabled_(chestCameraEnabled),
      tactileStateDir_(std::move(tactileStateDir)),
      persistCalibrationFile_(std::move(persistCalibrationFile)),
      exampleCalibrationFile_(std::move(exampleCalibrationFile)),
      fallbackCalibrationFile_(std::move(fallbackCalibrationFile)),
      stereoStatusFile_(std::move(stereoStatusFile)),
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
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "create_directories failed for " << episodeRoot_
                  << ": " << error.message() << std::endl).str());
        return false;
    }
    fs::create_directories(tactileReferenceDir(tactileStateDir_), error);
    if (error)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "create_directories failed for "
                              << tactileReferenceDir(tactileStateDir_) << ": "
                              << error.message() << std::endl).str());
        return false;
    }
    fs::create_directories(tactilePersistentDir(tactileStateDir_), error);
    if (error)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "create_directories failed for "
                              << tactilePersistentDir(tactileStateDir_) << ": "
                              << error.message() << std::endl).str());
        return false;
    }
    return true;
}

void RecordRuntime::EpisodeManager::setGripperRuntimeStates(
    const std::array<GripperRuntimeState, 2> &states)
{
    gripperRuntimeStates_ = states;
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

void RecordRuntime::EpisodeManager::refreshTactileReferenceCacheForSide(
    const std::string &side,
    std::vector<TactileValidationFinding> *findings)
{
    json tactileHistory = json::object();
    {
        std::ifstream historyInput(tactileHistoryPath(tactileStateDir_));
        if (historyInput.is_open())
        {
            try
            {
                historyInput >> tactileHistory;
            }
            catch (const std::exception &ex)
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to parse tactile history, reset in-memory state: "
                                     << ex.what() << std::endl).str());
                tactileHistory = json::object();
            }
        }
    }
    if (!tactileHistory.is_object())
    {
        tactileHistory = json::object();
    }
    if (!tactileHistory.contains("cameras") || !tactileHistory["cameras"].is_object())
    {
        tactileHistory["cameras"] = json::object();
    }

    bool historyDirty = false;
    for (const auto &target : kTactileCalibrationTargets)
    {
        if (side != target.side)
        {
            continue;
        }

        std::string serialDetail;
        const auto serial = probeUsbSerialForDeviceNode(target.devicePath, &serialDetail);
        if (!serial.has_value() || serial->empty())
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "tactile reference cache skipped: camera=" << target.cameraName
                                 << " reason="
                                 << (serialDetail.empty() ? "missing tactile serial" : serialDetail)
                                 << std::endl).str());
            continue;
        }

        std::string captureError;
        const auto frame = captureTactileGrayFrame(
            {
                "-f",
                "video4linux2",
                "-input_format",
                "mjpeg",
                "-video_size",
                "640x480",
                "-framerate",
                "120",
                "-i",
                target.devicePath,
            },
            0.0,
            &captureError);
        if (!frame.has_value())
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "tactile reference capture failed: camera=" << target.cameraName
                                 << " serial=" << *serial
                                 << " reason=" << captureError << std::endl).str());
            continue;
        }

        std::string writeError;
        if (!writeBinaryFile(tactileReferenceRawPath(tactileStateDir_, *serial), *frame, &writeError))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "tactile reference write failed: camera=" << target.cameraName
                                 << " serial=" << *serial
                                 << " reason=" << writeError << std::endl).str());
            continue;
        }

        json meta = {
            {"serial", *serial},
            {"camera_name", target.cameraName},
            {"device_path", target.devicePath},
            {"updated_at_ms", RecordRuntime::currentEpochMs()},
        };
        if (!writeTextFileAtomically(
                tactileReferenceMetaPath(tactileStateDir_, *serial), meta.dump(2) + "\n", &writeError))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "tactile reference meta write failed: camera=" << target.cameraName
                                 << " serial=" << *serial
                                 << " reason=" << writeError << std::endl).str());
        }

        json &cameraHistory = tactileHistory["cameras"][*serial];
        if (!cameraHistory.is_object())
        {
            cameraHistory = json::object();
        }
        cameraHistory["camera_name"] = target.cameraName;

        std::vector<uint8_t> persistentBaseline;
        std::string persistentBaselineError;
        const bool hasPersistentBaseline = readBinaryFileExact(
            tactilePersistentRawPath(tactileStateDir_, *serial),
            kTactileFrameBytes,
            &persistentBaseline,
            &persistentBaselineError);
        const uint64_t persistentBaselineUpdatedAtMs =
            cameraHistory.value("persistent_baseline_updated_at_ms", static_cast<uint64_t>(0));

        if (!hasPersistentBaseline || persistentBaselineUpdatedAtMs == 0 ||
            (RecordRuntime::currentEpochMs() - persistentBaselineUpdatedAtMs) >=
                kTactilePersistentBaselineRefreshMs)
        {
            if (writeBinaryFile(tactilePersistentRawPath(tactileStateDir_, *serial), *frame, &writeError))
            {
                cameraHistory["persistent_baseline_updated_at_ms"] = RecordRuntime::currentEpochMs();
                cameraHistory["persistent_fault_active"] = false;
                cameraHistory["persistent_fault_detail"] = "";
                historyDirty = true;
            }
        }

        const bool persistentFaultActive = cameraHistory.value("persistent_fault_active", false);
        if (persistentFaultActive && findings != nullptr)
        {
            TactileValidationFinding finding;
            finding.cameraName = target.cameraName;
            finding.serialNumber = *serial;
            finding.warningActive = true;
            finding.audioCommand = std::string(target.cameraName) + "_damaged";
            finding.detail = cameraHistory.value(
                "persistent_fault_detail", std::string("persistent baseline mismatch"));
            findings->push_back(std::move(finding));
        }
    }

    if (historyDirty)
    {
        std::string writeError;
        if (!writeTextFileAtomically(tactileHistoryPath(tactileStateDir_),
                                     tactileHistory.dump(2) + "\n",
                                     &writeError))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to persist tactile history: " << writeError << std::endl).str());
        }
    }
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

bool RecordRuntime::EpisodeManager::validateEpisode(const std::string &episodeDir,
                                                    std::string *errorMessage,
                                                    std::vector<TactileValidationFinding> *tactileFindings) const
{
    const int64_t validateStartMs = steadyNowMs();
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] validateEpisode begin: episode_dir=" << episodeDir << std::endl).str());
    const auto artifacts = activeEpisodeVideoArtifacts(chestCameraEnabled_);
    const std::vector<std::string> requiredFiles = {
        "sensor_data_left.mcap",
        "sensor_data_right.mcap",
        "metadata.json",
        "calibration.json",
        "info.json",
    };

    for (const auto &artifact : artifacts)
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
    if (!infoRoot.is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "info.json top-level value must be an object";
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
    for (const auto &artifact : artifacts)
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

    std::vector<VideoProbeResult> probes;
    probes.reserve(artifacts.size());
    double referenceSpanSec = 0.0;
    for (const auto &artifact : artifacts)
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

        DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] encoder tail check pass:"
                  << " side=" << target.side
                  << " encoder_count=" << encoderMessageCount
                  << " tail_video_end_ns=" << tailVideoEndNs
                  << " last_encoder_ns=" << lastEncoderLogTimeNs
                  << " lag_ms=" << (lagNs / 1000000.0) << std::endl).str());
    }

    json tactileHistory = json::object();
    {
        std::ifstream historyInput(tactileHistoryPath(tactileStateDir_));
        if (historyInput.is_open())
        {
            try
            {
                historyInput >> tactileHistory;
            }
            catch (const std::exception &ex)
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to parse tactile history, reset in-memory state: "
                                     << ex.what() << std::endl).str());
                tactileHistory = json::object();
            }
        }
    }
    if (!tactileHistory.is_object())
    {
        tactileHistory = json::object();
    }
    if (!tactileHistory.contains("cameras") || !tactileHistory["cameras"].is_object())
    {
        tactileHistory["cameras"] = json::object();
    }

    bool historyDirty = false;
    if (tactileFindings != nullptr)
    {
        tactileFindings->clear();
    }
    for (const auto &target : kTactileCalibrationTargets)
    {
        const auto offsetIt = recordTimeOffsetUsByCamera.find(target.cameraName);
        if (offsetIt == recordTimeOffsetUsByCamera.end() || offsetIt->second <= 0)
        {
            continue;
        }

        std::string serialDetail;
        const auto serial = probeUsbSerialForDeviceNode(target.devicePath, &serialDetail);
        if (!serial.has_value() || serial->empty())
        {
            continue;
        }

        std::vector<uint8_t> baselineFrame;
        std::string baselineError;
        if (!readBinaryFileExact(tactileReferenceRawPath(tactileStateDir_, *serial),
                                 kTactileFrameBytes,
                                 &baselineFrame,
                                 &baselineError))
        {
            continue;
        }

        std::string frameError;
        const auto currentFrame = captureTactileGrayFrame(
            {"-i", episodeDir + "/" + std::string(target.cameraName) + ".mkv"},
            kTactileEpisodeProbeSec,
            &frameError);
        if (!currentFrame.has_value())
        {
            continue;
        }

        const TactileFrameMetrics metrics = computeTactileFrameMetrics(baselineFrame, *currentFrame);
        json &cameraHistory = tactileHistory["cameras"][*serial];
        if (!cameraHistory.is_object())
        {
            cameraHistory = json::object();
        }
        if (!cameraHistory.contains("recent") || !cameraHistory["recent"].is_array())
        {
            cameraHistory["recent"] = json::array();
        }

        const bool persistentWarningActiveBefore =
            cameraHistory.value("persistent_fault_active", false);
        std::string persistentDetail =
            cameraHistory.value("persistent_fault_detail", std::string());

        bool warningActiveBefore = false;
        const json &recentBefore = cameraHistory["recent"];
        if (recentBefore.size() >= kTactileHistoryWindow)
        {
            warningActiveBefore = std::all_of(
                recentBefore.begin(),
                recentBefore.end(),
                [](const json &entry) { return entry.is_boolean() && entry.get<bool>(); });
        }

        json &recent = cameraHistory["recent"];
        recent.push_back(metrics.damaged);
        while (recent.size() > kTactileHistoryWindow)
        {
            recent.erase(recent.begin());
        }
        cameraHistory["camera_name"] = target.cameraName;
        cameraHistory["updated_at_ms"] = RecordRuntime::currentEpochMs();
        historyDirty = true;

        bool warningActive = false;
        if (recent.size() >= kTactileHistoryWindow)
        {
            warningActive = std::all_of(
                recent.begin(),
                recent.end(),
                [](const json &entry) { return entry.is_boolean() && entry.get<bool>(); });
        }

        bool persistentWarningTriggered = false;
        std::vector<uint8_t> persistentBaseline;
        std::string persistentBaselineError;
        if (readBinaryFileExact(tactilePersistentRawPath(tactileStateDir_, *serial),
                                kTactileFrameBytes,
                                &persistentBaseline,
                                &persistentBaselineError) &&
            !persistentWarningActiveBefore)
        {
            const TactileFrameMetrics persistentMetrics =
                computeTactileFrameMetrics(persistentBaseline, *currentFrame);
            if (persistentMetrics.damaged)
            {
                persistentWarningTriggered = true;
                persistentDetail =
                    "persistent_record mean_abs=" + formatFixed(persistentMetrics.meanAbsDiff, 2) +
                    " mask_ratio=" + formatFixed(persistentMetrics.maskRatio, 4) +
                    " corr=" + formatFixed(persistentMetrics.correlation, 4);
                cameraHistory["persistent_fault_active"] = true;
                cameraHistory["persistent_fault_detail"] = persistentDetail;
                historyDirty = true;
            }
        }

        TactileValidationFinding finding;
        finding.cameraName = target.cameraName;
        finding.serialNumber = *serial;
        finding.damaged = metrics.damaged;
        finding.warningActive = warningActive || persistentWarningActiveBefore;
        finding.warningTriggered =
            (warningActive && !warningActiveBefore) || persistentWarningTriggered;
        finding.audioCommand = std::string(target.cameraName) + "_damaged";
        finding.detail =
            "mean_abs=" + formatFixed(metrics.meanAbsDiff, 2) +
            " mask_ratio=" + formatFixed(metrics.maskRatio, 4) +
            " corr=" + formatFixed(metrics.correlation, 4);
        if (!persistentDetail.empty())
        {
            finding.detail += " | " + persistentDetail;
        }
        if (tactileFindings != nullptr)
        {
            tactileFindings->push_back(std::move(finding));
        }
    }

    if (historyDirty)
    {
        std::string writeError;
        if (!writeTextFileAtomically(tactileHistoryPath(tactileStateDir_),
                                     tactileHistory.dump(2) + "\n",
                                     &writeError))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to persist tactile history: " << writeError << std::endl).str());
        }
    }

    DM_LOG_INFO("{}", (::DA::utils::LogString() << "[PERF] validateEpisode end: episode_dir=" << episodeDir
              << " elapsed_ms=" << (steadyNowMs() - validateStartMs)
              << " reference_span_sec=" << formatSeconds(referenceSpanSec) << std::endl).str());
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

    const auto makeGripperMetadata = [this](const std::string &side) {
        const auto &state = gripperRuntimeStates_[gripperStateIndexForSide(side)];
        std::ostringstream stream;
        stream << "{\n"
               << "    \"serial_number\": " << json(state.serialNumber).dump() << ",\n"
               << "    \"calibration_status\": " << json(state.calibrationStatus).dump() << "\n"
               << "  }";
        return stream.str();
    };

    output << "{\n"
           << "  \"device_type\": \"UMI\",\n"
           << "  \"device_model\": \"ugripper\",\n"
           << "  \"device_id\": " << json(deviceSn_).dump() << ",\n"
           << "  \"collector\": \"default_user\",\n"
           << "  \"data_path\": \"data/episode_{date:08d}_{episode_index:04d}\",\n"
           << "  \"camera_codec\": " << json(cameraCodec_).dump() << ",\n"
           << "  \"ugripper_lang\": " << json(language_).dump() << ",\n"
           << "  \"ugripper_version\": " << json(packageVersion_).dump() << ",\n"
           << "  \"ugripper_usb_updater_version\": " << json(updaterVersion_).dump() << ",\n"
           << "  \"data_format_version\": \"3\",\n"
           << "  \"record_runtime\": \"cpp\",\n"
           << "  \"reset_recording\": " << (resetRecording ? "true" : "false") << ",\n"
           << "  \"reset_source_episode_dir\": " << json(resetSourceDir).dump() << ",\n"
           << "  \"gripper_left\": " << makeGripperMetadata("left") << ",\n"
           << "  \"gripper_right\": " << makeGripperMetadata("right") << "\n"
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "invalid persist calibration, fallback to " << sourceFile << std::endl).str());
    }

    const std::string generationDate =
        calibrationJson.contains("metadata") && calibrationJson["metadata"].is_object()
            ? calibrationJson["metadata"].value("generation_date", currentDateString())
            : currentDateString();
    const std::string notes =
        calibrationJson.contains("calibration_info") && calibrationJson["calibration_info"].is_object()
            ? calibrationJson["calibration_info"].value(
                  "notes",
                  "Main/stereo/imu entries are filled from gripper calibration payload when available; tactile serials are resolved from runtime devices.")
            : "Main/stereo/imu entries are filled from gripper calibration payload when available; tactile serials are resolved from runtime devices.";

    migrateLegacyFlattenedCalibrationKeys(&calibrationJson);
    sanitizeCalibrationTopLevel(&calibrationJson);

    std::map<std::string, json> sourceCalibrationEntries;
    for (const auto &target : kTactileCalibrationTargets)
    {
        if (const json *entry = findJsonPathConst(&calibrationJson, target.jsonPath);
            entry != nullptr && entry->is_object())
        {
            sourceCalibrationEntries.emplace(target.jsonPath, *entry);
        }
    }
    for (const std::string side : {"left", "right"})
    {
        const std::string legacyJsonPath = legacyTactileJsonPathForSide(side);
        if (const json *entry = findJsonPathConst(&calibrationJson, legacyJsonPath);
            entry != nullptr && entry->is_object())
        {
            sourceCalibrationEntries.emplace(legacyJsonPath, *entry);
        }
    }

    json leftStereoTemplate;
    json rightStereoTemplate;
    json chestCameraTemplate;
    const json *leftStereoTemplatePtr = nullptr;
    const json *rightStereoTemplatePtr = nullptr;
    const json *chestCameraTemplatePtr = nullptr;
    if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.left_stereo");
        entry != nullptr && entry->is_object())
    {
        leftStereoTemplate = *entry;
        leftStereoTemplatePtr = &leftStereoTemplate;
    }
    else if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.fays_cam0");
             entry != nullptr && entry->is_object())
    {
        leftStereoTemplate = *entry;
        leftStereoTemplatePtr = &leftStereoTemplate;
    }

    if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.right_stereo");
        entry != nullptr && entry->is_object())
    {
        rightStereoTemplate = *entry;
        rightStereoTemplatePtr = &rightStereoTemplate;
    }
    else if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.fays_cam1");
             entry != nullptr && entry->is_object())
    {
        rightStereoTemplate = *entry;
        rightStereoTemplatePtr = &rightStereoTemplate;
    }
    if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.chest_cam_main");
        entry != nullptr && entry->is_object())
    {
        chestCameraTemplate = *entry;
        chestCameraTemplatePtr = &chestCameraTemplate;
    }
    else if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.left_cam_main");
             entry != nullptr && entry->is_object())
    {
        chestCameraTemplate = *entry;
        chestCameraTemplatePtr = &chestCameraTemplate;
    }
    else if (const json *entry = findJsonPathConst(&calibrationJson, "observation.images.right_cam_main");
             entry != nullptr && entry->is_object())
    {
        chestCameraTemplate = *entry;
        chestCameraTemplatePtr = &chestCameraTemplate;
    }

    removeIfPresent(&calibrationJson, "observation.images.fays_cam0");
    removeIfPresent(&calibrationJson, "observation.images.fays_cam1");
    removeIfPresent(&calibrationJson, "observation.imu.fays_imu0");

    if (json *leftStereo = findJsonPath(&calibrationJson, "observation.images.left_stereo", true))
    {
        *leftStereo = makeStereoPlaceholderFromTemplate(leftStereoTemplatePtr);
    }
    if (json *rightStereo = findJsonPath(&calibrationJson, "observation.images.right_stereo", true))
    {
        *rightStereo = makeStereoPlaceholderFromTemplate(rightStereoTemplatePtr);
    }
    if (chestCameraEnabled_)
    {
        if (json *chestCamera = findJsonPath(&calibrationJson, "observation.images.chest_cam_main", true))
        {
            *chestCamera = makeMainCameraPlaceholderFromTemplate(chestCameraTemplatePtr);
        }
    }
    else
    {
        removeIfPresent(&calibrationJson, "observation.images.chest_cam_main");
    }

    for (const auto &target : kTactileCalibrationTargets)
    {
        const json *templateEntry = findSourceCalibrationEntry(
            sourceCalibrationEntries,
            tactileTemplateCandidateKeys(target));
        if (json *entry = findJsonPath(&calibrationJson, target.jsonPath, true))
        {
            *entry = makeTactileCalibrationEntry(templateEntry, target.serialPlaceholder);
        }
    }

    if (!commandExists("udevadm"))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "udevadm not found in PATH, skip runtime tactile serial injection" << std::endl).str());
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
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to resolve runtime tactile serial for camera=" << target.cameraName
                          << " device=" << target.devicePath
                          << " detail=" << detail
                          << "; keep source calibration value" << std::endl).str());
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
    removeLegacyFlattenedCalibrationKeys(&calibrationJson);

    bool anyCalibrationValid = false;
    for (const auto &state : gripperRuntimeStates_)
    {
        if (state.calibrationValid && state.calibrationPayloadCached)
        {
            applySideCalibrationPayload(&calibrationJson, state.side, state.calibrationPayload);
            anyCalibrationValid = true;
        }
    }

    bool anyFaysCalibrationValid = false;
    for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
             {"left", "left_stereo"},
             {"right", "right_stereo"},
         })
    {
        json faysCalibration;
        std::string faysStatus;
        if (!loadFaysCalibrationFromStatus(stereoStatusFile_, entry.second, &faysCalibration, &faysStatus))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "Fays calibration unavailable for "
                                 << entry.second << ": " << faysStatus << std::endl).str());
            continue;
        }

        if (json *stereo = findJsonPath(&calibrationJson, stereoJsonPathForSide(entry.first), true))
        {
            *stereo = makeFaysStereoCalibrationEntry(faysCalibration);
        }
        if (json *imu = findJsonPath(&calibrationJson, imuJsonPathForSide(entry.first), true))
        {
            *imu = makeFaysImuCalibrationEntry(faysCalibration);
        }
        anyFaysCalibrationValid = true;
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "Fays calibration loaded for "
                            << entry.second << ": " << faysStatus << std::endl).str());
    }

    const std::string calibrationStatus = (anyCalibrationValid || anyFaysCalibrationValid)
                                             ? "calibrated"
                                             : "uncalibrated";
    calibrationJson["metadata"] =
        makeLockedCalibrationMetadata(generationDate, calibrationStatus);
    calibrationJson["calibration_info"] =
        makeLockedCalibrationInfo(generationDate, calibrationStatus, notes);

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
