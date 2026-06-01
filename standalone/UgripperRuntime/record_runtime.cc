#include "record_runtime.h"
#include "main_camera_calibration_data.h"
#include "utils/logger.h"
#include "record_runtime/gripper_refresh_logic.h"
#include "utils/env_utils.h"
#include "utils/file_utils.hpp"
#include "utils/time_utils.h"

#include <mcap/reader.hpp>
#include <nlohmann/json.hpp>
#include <uuid/uuid.h>

#include <algorithm>
#include <array>
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
#include <future>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <utility>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {
constexpr const char *kChestCameraEnvKey = "ENABLE_CHEST_CAM_MAIN";
constexpr const char *kPerfLogEnvKey = "UGRIPPER_PERF_LOG";
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
constexpr uint64_t kEgoFinalizeWaitPollMs = 100;
constexpr uint64_t kHealthCheckIntervalMs = 1000;
constexpr uint64_t kHmiActiveTimeoutMs = 2500;
constexpr uint64_t kGripperRefreshActiveTimeoutMs = 1500;
constexpr double kMinReasonableVideoSpanSec = 0.2;
constexpr double kMaxVideoSpanGapSec = 5.0;
constexpr int64_t kEncoderTailWindowNs = 1000LL * 1000LL * 1000LL;
constexpr int64_t kEncoderTailMaxLagNs = 1000LL * 1000LL * 1000LL;
constexpr size_t kEncoderTailChunkScanLimit = 4;
constexpr int64_t kFaysImuTailMaxLagNs = 1000LL * 1000LL * 1000LL;
constexpr int64_t kFaysMcapMinSpanNs = 500LL * 1000LL * 1000LL;
constexpr size_t kFaysTailChunkScanLimit = 4;
constexpr mcap::ChannelId kFaysImuChannelId = 1;
constexpr mcap::ChannelId kFaysCameraChannelId = 2;
constexpr uint64_t kFaysImuPayloadBytes = sizeof(double) * 6;
constexpr uint64_t kFaysCameraPayloadBytes = sizeof(uint32_t);
constexpr int kVideoProbeTimeoutMs = 1500;
constexpr int kUdevadmProbeTimeoutMs = 2000;
constexpr uint8_t kYuzhouXuUnitId = 0x03;
constexpr uint8_t kYuzhouXuSelector = 0x17;
constexpr size_t kYuzhouXuPacketSize = 9;
constexpr size_t kYuzhouPayloadChunkSize = 8;
constexpr size_t kYuzhouPayloadSize = 1024;
constexpr size_t kYuzhouPayloadChunkCount = kYuzhouPayloadSize / kYuzhouPayloadChunkSize;
constexpr size_t kYuzhouSnOffset = 0x010;
constexpr size_t kYuzhouSnLength = 16;
constexpr uint64_t kMainCameraRefreshDelayMs = 1500;
constexpr uint64_t kMainCameraRefreshRetryMs = 3000;
constexpr int kTactileFrameWidth = 160;
constexpr int kTactileFrameHeight = 120;
constexpr size_t kTactileFrameBytes = static_cast<size_t>(kTactileFrameWidth * kTactileFrameHeight);
constexpr double kTactileEpisodeProbeSec = 0.12;
constexpr double kTactileRobustResidualAreaThreshold = 0.002;
constexpr double kTactileResidualFloorThreshold = 20.0;
constexpr double kTactileResidualMadMultiplier = 6.0;
constexpr double kTactileMadToSigma = 1.4826;
constexpr int kTactileResidualMinNeighborCount = 4;
constexpr size_t kTactileResidualMinComponentPixels = 8;
constexpr size_t kTactileHistoryWindow = 3;
constexpr int kTactileSnapshotTimeoutMs = 2500;
constexpr uint64_t kTactilePersistentBaselineRefreshMs = 12ULL * 60ULL * 60ULL * 1000ULL;
constexpr const char *kEpisodeTimingFileName = ".recording_timing.json";
constexpr const char *kEpisodeTimingShmPrefix = "ugripper_recording_timing_";
constexpr const char *kEgoSyncStatusFileName = "ego_sync.json";

bool g_perfLogEnabled = true;

bool perfLogEnabled()
{
    return g_perfLogEnabled;
}

void logPerf(const std::string &message)
{
    if (!perfLogEnabled())
    {
        return;
    }
    DM_LOG_INFO("{}", (::DA::utils::LogString() << message << std::endl).str());
}

struct EpisodeVideoArtifact
{
    const char *cameraName;
    const char *fileName;
};

constexpr std::array<EpisodeVideoArtifact, 9> kAllEpisodeVideoArtifacts = {{
    {"left_cam_main", "cam_left.mkv"},
    {"right_cam_main", "cam_right.mkv"},
    {"chest_cam_main", "cam_chest.mkv"},
    {"left_stereo", "stereo_left.mkv"},
    {"right_stereo", "stereo_right.mkv"},
    {"left_tcam_l", "tcam_left_l.mkv"},
    {"left_tcam_r", "tcam_left_r.mkv"},
    {"right_tcam_l", "tcam_right_l.mkv"},
    {"right_tcam_r", "tcam_right_r.mkv"},
}};

std::optional<ugripper::runtime::HealthFault> MakeDiskHealthFault(const std::string &key,
                                                                  const std::string &detail)
{
    return ugripper::runtime::HealthFault{
        .led_state = ugripper::runtime::RuntimeLedState::Error3,
        .side = ugripper::runtime::HardwareFaultSide::Unknown,
        .key = key,
        .detail = detail,
    };
}

bool isMountedDiskRoot(const fs::path &diskRoot)
{
    std::ifstream mountInfo("/proc/self/mountinfo");
    if (!mountInfo.is_open())
    {
        return false;
    }

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
            return true;
        }
    }
    return false;
}

std::optional<ugripper::runtime::HealthFault> detectDiskHealthFault(const std::string &diskRootPath)
{
    if (diskRootPath.empty())
    {
        return MakeDiskHealthFault("disk_mount_lost", "Disk root path is empty");
    }

    const fs::path diskRoot(diskRootPath);
    std::error_code pathError;
    if (!fs::exists(diskRoot, pathError) || !fs::is_directory(diskRoot, pathError))
    {
        const std::string suffix = pathError ? " error=" + pathError.message() : "";
        return MakeDiskHealthFault("disk_mount_lost",
                                   "Disk root unavailable: " + diskRootPath + suffix);
    }

    if (!isMountedDiskRoot(diskRoot))
    {
        return MakeDiskHealthFault("disk_mount_lost", "Disk root is not mounted: " + diskRootPath);
    }

    errno = 0;
    if (access(diskRoot.c_str(), W_OK) != 0)
    {
        const int savedErrno = errno;
        const std::string errorText = std::strerror(savedErrno);
        if (savedErrno == EROFS)
        {
            return MakeDiskHealthFault("disk_not_writable",
                                       "Disk root is read-only: " + diskRootPath + " error=" + errorText);
        }
        return MakeDiskHealthFault("disk_not_writable",
                                   "Disk root not writable: " + diskRootPath + " error=" + errorText);
    }

    std::error_code spaceError;
    const fs::space_info spaceInfo = fs::space(diskRoot, spaceError);
    if (spaceError)
    {
        return MakeDiskHealthFault("disk_space_unavailable",
                                   "Disk root space info failed: " + diskRootPath + " error=" + spaceError.message());
    }
    if (spaceInfo.available == 0)
    {
        return MakeDiskHealthFault(
            "disk_full",
            "Disk root has no available space: " + diskRootPath +
                " available_bytes=0 capacity_bytes=" + std::to_string(spaceInfo.capacity));
    }

    return std::nullopt;
}

constexpr std::array<const char *, 13> kAllCriticalDevicePaths = {{
    "/dev/cam_right",
    "/dev/cam_left",
    "/dev/cam_chest",
    "/dev/stereo_right",
    "/dev/stereo_left",
    "/dev/right_fays_imu",
    "/dev/left_fays_imu",
    "/dev/tcam_right_l",
    "/dev/tcam_right_r",
    "/dev/tcam_left_l",
    "/dev/tcam_left_r",
    "/dev/right_encoder",
    "/dev/left_encoder",
}};

constexpr std::array<const char *, 6> kLeftCriticalDevicePaths = {{
    "/dev/cam_left",
    "/dev/stereo_left",
    "/dev/left_fays_imu",
    "/dev/tcam_left_l",
    "/dev/tcam_left_r",
    "/dev/left_encoder",
}};

constexpr std::array<const char *, 6> kRightCriticalDevicePaths = {{
    "/dev/cam_right",
    "/dev/stereo_right",
    "/dev/right_fays_imu",
    "/dev/tcam_right_l",
    "/dev/tcam_right_r",
    "/dev/right_encoder",
}};

struct VideoProbeResult
{
    std::string cameraName;
    std::string fileName;
    double startTimeSec = 0.0;
    double durationSec = 0.0;
    double spanSec = 0.0;
};

struct VideoProbeTaskResult
{
    bool ok = false;
    VideoProbeResult probe;
    std::string errorMessage;
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
     "sensor_left.mcap",
     "encoder_left",
     {"left_cam_main", "left_stereo", "left_tcam_l", "left_tcam_r"}},
    {"right",
     "sensor_right.mcap",
     "encoder_right",
     {"right_cam_main", "right_stereo", "right_tcam_l", "right_tcam_r"}},
}};

struct FaysTailCheckTarget
{
    const char *side;
    const char *mcapFileName;
    const char *imuTopic;
    const char *cameraTopic;
};

constexpr std::array<FaysTailCheckTarget, 2> kFaysTailCheckTargets = {{
    {"left", "fays_data_left.mcap", "i", "c"},
    {"right", "fays_data_right.mcap", "i", "c"},
}};

struct FaysMcapSummary
{
    uint64_t lastImuLogTimeNs = 0;
    uint64_t lastCameraLogTimeNs = 0;
    uint64_t imuMessageCount = 0;
    uint64_t cameraMessageCount = 0;
    uint64_t spanNs = 0;
};

struct TailCheckTaskResult
{
    bool ok = false;
    std::string detail;
    std::string errorMessage;
};

struct CommandCaptureResult
{
    bool success = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output;
};

struct TactileFrameMetrics
{
    double robustResidualArea = 0.0;
    double rawResidualArea = 0.0;
    double residualThreshold = 0.0;
    double residualMedian = 0.0;
    double residualMad = 0.0;
    double residualMean = 0.0;
    double residualP99 = 0.0;
    double gain = 1.0;
    double offset = 0.0;
    size_t maxResidualComponentPixels = 0;
    bool damaged = false;
};

bool writeTextFileAtomically(const fs::path &path, const std::string &content, std::string *errorMessage);
bool loadJsonFile(const std::string &path, json *output, std::string *errorMessage);

struct TactileCalibrationTarget
{
    const char *cameraName;
    const char *side;
    const char *devicePath;
    const char *jsonPath;
    const char *serialPlaceholder;
};

constexpr std::array<TactileCalibrationTarget, 4> kTactileCalibrationTargets = {{
    {"left_tcam_l", "left", "/dev/tcam_left_l", "observation.images.tcam_left_l", "{{LEFT_TCAM_L_SERIAL}}"},
    {"left_tcam_r", "left", "/dev/tcam_left_r", "observation.images.tcam_left_r", "{{LEFT_TCAM_R_SERIAL}}"},
    {"right_tcam_l", "right", "/dev/tcam_right_l", "observation.images.tcam_right_l", "{{RIGHT_TCAM_L_SERIAL}}"},
    {"right_tcam_r", "right", "/dev/tcam_right_r", "observation.images.tcam_right_r", "{{RIGHT_TCAM_R_SERIAL}}"},
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

fs::path episodeTimingPath(const fs::path &episodeDir)
{
    const std::string parent = sanitizeFileComponent(episodeDir.parent_path().filename().string());
    const std::string name = sanitizeFileComponent(episodeDir.filename().string());
    return fs::path("/dev/shm") / (std::string(kEpisodeTimingShmPrefix) + parent + "_" + name + ".json");
}

std::string stripEpisodeTempSuffix(const std::string &name)
{
    constexpr const char *suffix = "-temp";
    constexpr size_t suffixLen = 5;
    if (name.size() >= suffixLen && name.compare(name.size() - suffixLen, suffixLen, suffix) == 0)
    {
        return name.substr(0, name.size() - suffixLen);
    }
    return name;
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

const char *episodeVideoFileNameForCamera(const char *cameraName)
{
    for (const auto &artifact : kAllEpisodeVideoArtifacts)
    {
        if (std::strcmp(artifact.cameraName, cameraName) == 0)
        {
            return artifact.fileName;
        }
    }
    return nullptr;
}

std::vector<const char *> activeCriticalDevicePaths(bool chestCameraEnabled)
{
    std::vector<const char *> paths;
    paths.reserve(kAllCriticalDevicePaths.size());
    for (const char *path : kAllCriticalDevicePaths)
    {
        if (!chestCameraEnabled && std::strcmp(path, "/dev/cam_chest") == 0)
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

double percentileFromSorted(const std::vector<double> &sortedValues, double percentile)
{
    if (sortedValues.empty())
    {
        return 0.0;
    }
    if (sortedValues.size() == 1)
    {
        return sortedValues.front();
    }

    const double clamped = std::clamp(percentile, 0.0, 1.0);
    const double position = clamped * static_cast<double>(sortedValues.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = std::min(lower + 1, sortedValues.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sortedValues[lower] * (1.0 - weight) + sortedValues[upper] * weight;
}

std::vector<double> sortedFrameValues(const std::vector<uint8_t> &frame)
{
    std::vector<double> values;
    values.reserve(frame.size());
    for (const uint8_t value : frame)
    {
        values.push_back(static_cast<double>(value));
    }
    std::sort(values.begin(), values.end());
    return values;
}

std::string formatTactileMetrics(const TactileFrameMetrics &metrics)
{
    return "robust_residual_area=" + formatFixed(metrics.robustResidualArea, 4) +
           " area_threshold=" + formatFixed(kTactileRobustResidualAreaThreshold, 4) +
           " raw_residual_area=" + formatFixed(metrics.rawResidualArea, 4) +
           " residual_thr=" + formatFixed(metrics.residualThreshold, 2) +
           " residual_median=" + formatFixed(metrics.residualMedian, 2) +
           " residual_mad=" + formatFixed(metrics.residualMad, 2) +
           " residual_p99=" + formatFixed(metrics.residualP99, 2) +
           " max_component_px=" + std::to_string(metrics.maxResidualComponentPixels) +
           " min_component_px=" + std::to_string(kTactileResidualMinComponentPixels) +
           " gain=" + formatFixed(metrics.gain, 4) +
           " offset=" + formatFixed(metrics.offset, 2);
}

size_t countFilteredResidualMask(const std::vector<uint8_t> &rawMask,
                                 size_t width,
                                 size_t height,
                                 size_t minComponentPixels,
                                 size_t *maxComponentPixels)
{
    if (rawMask.size() != width * height || rawMask.empty())
    {
        if (maxComponentPixels != nullptr)
        {
            *maxComponentPixels = 0;
        }
        return 0;
    }

    std::vector<uint8_t> neighborMask(rawMask.size(), 0);
    for (size_t y = 0; y < height; ++y)
    {
        for (size_t x = 0; x < width; ++x)
        {
            const size_t index = y * width + x;
            if (rawMask[index] == 0)
            {
                continue;
            }
            int neighborCount = 0;
            const size_t yBegin = (y == 0) ? 0 : y - 1;
            const size_t yEnd = std::min(height - 1, y + 1);
            const size_t xBegin = (x == 0) ? 0 : x - 1;
            const size_t xEnd = std::min(width - 1, x + 1);
            for (size_t yy = yBegin; yy <= yEnd; ++yy)
            {
                for (size_t xx = xBegin; xx <= xEnd; ++xx)
                {
                    if (rawMask[yy * width + xx] != 0)
                    {
                        ++neighborCount;
                    }
                }
            }
            if (neighborCount >= kTactileResidualMinNeighborCount)
            {
                neighborMask[index] = 1;
            }
        }
    }

    std::vector<uint8_t> visited(rawMask.size(), 0);
    std::vector<size_t> stack;
    size_t retainedPixels = 0;
    size_t maxComponent = 0;
    for (size_t start = 0; start < neighborMask.size(); ++start)
    {
        if (neighborMask[start] == 0 || visited[start] != 0)
        {
            continue;
        }

        stack.clear();
        stack.push_back(start);
        visited[start] = 1;
        size_t componentPixels = 0;
        for (size_t cursor = 0; cursor < stack.size(); ++cursor)
        {
            const size_t index = stack[cursor];
            ++componentPixels;
            const size_t y = index / width;
            const size_t x = index % width;
            const size_t yBegin = (y == 0) ? 0 : y - 1;
            const size_t yEnd = std::min(height - 1, y + 1);
            const size_t xBegin = (x == 0) ? 0 : x - 1;
            const size_t xEnd = std::min(width - 1, x + 1);
            for (size_t yy = yBegin; yy <= yEnd; ++yy)
            {
                for (size_t xx = xBegin; xx <= xEnd; ++xx)
                {
                    const size_t next = yy * width + xx;
                    if (neighborMask[next] != 0 && visited[next] == 0)
                    {
                        visited[next] = 1;
                        stack.push_back(next);
                    }
                }
            }
        }

        maxComponent = std::max(maxComponent, componentPixels);
        if (componentPixels >= minComponentPixels)
        {
            retainedPixels += componentPixels;
        }
    }

    if (maxComponentPixels != nullptr)
    {
        *maxComponentPixels = maxComponent;
    }
    return retainedPixels;
}

TactileFrameMetrics computeTactileFrameMetrics(const std::vector<uint8_t> &baseline,
                                               const std::vector<uint8_t> &current)
{
    TactileFrameMetrics metrics;
    if (baseline.size() != current.size() || baseline.empty())
    {
        metrics.damaged = true;
        metrics.robustResidualArea = 1.0;
        return metrics;
    }

    const auto baselineSorted = sortedFrameValues(baseline);
    const auto currentSorted = sortedFrameValues(current);
    const double baselineP5 = percentileFromSorted(baselineSorted, 0.05);
    const double baselineP50 = percentileFromSorted(baselineSorted, 0.50);
    const double baselineP95 = percentileFromSorted(baselineSorted, 0.95);
    const double currentP5 = percentileFromSorted(currentSorted, 0.05);
    const double currentP50 = percentileFromSorted(currentSorted, 0.50);
    const double currentP95 = percentileFromSorted(currentSorted, 0.95);
    const double currentSpan = std::max(1e-6, currentP95 - currentP5);
    metrics.gain = (baselineP95 - baselineP5) / currentSpan;
    metrics.offset = baselineP50 - metrics.gain * currentP50;

    std::vector<double> residuals;
    residuals.reserve(baseline.size());
    double residualSum = 0.0;
    for (size_t index = 0; index < baseline.size(); ++index)
    {
        const double baseValue = static_cast<double>(baseline[index]);
        const double currentValue = static_cast<double>(current[index]);
        const double correctedCurrent = std::clamp(currentValue * metrics.gain + metrics.offset, 0.0, 255.0);
        const double residual = std::fabs(correctedCurrent - baseValue);
        residuals.push_back(residual);
        residualSum += residual;
    }

    metrics.residualMean = residualSum / static_cast<double>(baseline.size());
    std::vector<double> sortedResiduals = residuals;
    std::sort(sortedResiduals.begin(), sortedResiduals.end());
    metrics.residualMedian = percentileFromSorted(sortedResiduals, 0.50);
    metrics.residualP99 = percentileFromSorted(sortedResiduals, 0.99);

    std::vector<double> absoluteMedianDeviations;
    absoluteMedianDeviations.reserve(residuals.size());
    for (const double residual : residuals)
    {
        absoluteMedianDeviations.push_back(std::fabs(residual - metrics.residualMedian));
    }
    std::sort(absoluteMedianDeviations.begin(), absoluteMedianDeviations.end());
    metrics.residualMad = percentileFromSorted(absoluteMedianDeviations, 0.50);
    metrics.residualThreshold = std::max(
        kTactileResidualFloorThreshold,
        metrics.residualMedian + kTactileResidualMadMultiplier * kTactileMadToSigma * metrics.residualMad);

    std::vector<uint8_t> rawResidualMask;
    rawResidualMask.reserve(residuals.size());
    size_t rawResidualMaskCount = 0;
    for (const double residual : residuals)
    {
        if (residual >= metrics.residualThreshold)
        {
            rawResidualMask.push_back(1);
            ++rawResidualMaskCount;
        }
        else
        {
            rawResidualMask.push_back(0);
        }
    }
    metrics.rawResidualArea = static_cast<double>(rawResidualMaskCount) / static_cast<double>(baseline.size());
    const size_t filteredResidualMaskCount = countFilteredResidualMask(
        rawResidualMask,
        static_cast<size_t>(kTactileFrameWidth),
        static_cast<size_t>(kTactileFrameHeight),
        kTactileResidualMinComponentPixels,
        &metrics.maxResidualComponentPixels);
    metrics.robustResidualArea = static_cast<double>(filteredResidualMaskCount) / static_cast<double>(baseline.size());
    metrics.damaged = metrics.robustResidualArea >= kTactileRobustResidualAreaThreshold;
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
            const int savedErrno = errno;
            if (errorMessage != nullptr)
            {
                *errorMessage = "cannot open temp file for write: " + tempPath.string() +
                                (savedErrno != 0 ? " error=" + std::string(std::strerror(savedErrno)) : "");
            }
            return false;
        }
        output << content;
        output.flush();
        if (!output.good())
        {
            const int savedErrno = errno;
            if (errorMessage != nullptr)
            {
                *errorMessage = "failed to write temp file: " + tempPath.string() +
                                (savedErrno != 0 ? " error=" + std::string(std::strerror(savedErrno))
                                                 : " error=stream_write_failed");
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

json makeRuntimeTactileCalibrationEntry(const std::string &serial)
{
    return json::object({
        {"names", json::array({"height", "width", "channels"})},
        {"serial", serial},
        {"shape", json::array({480, 640, 3})},
    });
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
    return side == "left" ? "observation.images.cam_left_main"
                          : "observation.images.cam_right_main";
}

const char *stereoJsonPathForSide(const std::string &side)
{
    return side == "left" ? "observation.images.stereo_left"
                          : "observation.images.stereo_right";
}

const char *imuJsonPathForSide(const std::string &side)
{
    return side == "left" ? "observation.imu.imu_left"
                          : "observation.imu.imu_right";
}

json makeMainCameraCalibrationEntryFromPayload(const std::array<uint8_t, kYuzhouPayloadSize> &payload)
{
    main_camera::MainCameraCalibrationDataV1 data{};
    std::memcpy(&data, payload.data(), sizeof(data));
    const int width = static_cast<int>(std::lround(data.mainCamera.resolution[0]));
    const int height = static_cast<int>(std::lround(data.mainCamera.resolution[1]));
    const int normalizedWidth = width > 0 ? width : 1920;
    const int normalizedHeight = height > 0 ? height : 1080;
    return json::object({
        {"camera_model", "pinhole"},
        {"distortion_coeffs",
         json::array({
             data.mainCamera.distortionCoefficients[0],
             data.mainCamera.distortionCoefficients[1],
             data.mainCamera.distortionCoefficients[2],
             data.mainCamera.distortionCoefficients[3],
         })},
        {"distortion_model", "equidistant"},
        {"fps", 60.0},
        {"intrinsics",
         json::object({{std::to_string(normalizedWidth) + "x" +
                             std::to_string(normalizedHeight),
                         json::object({
                             {"fx", data.mainCamera.intrinsics[0]},
                             {"fy", data.mainCamera.intrinsics[1]},
                             {"ppx", data.mainCamera.intrinsics[2]},
                             {"ppy", data.mainCamera.intrinsics[3]},
                         })}})},
        {"names", json::array({"height", "width", "channels"})},
        {"shape", json::array({normalizedHeight, normalizedWidth, 3})},
    });
}

json makeDefaultMainCameraCalibrationEntry()
{
    return json::object({
        {"camera_model", "pinhole"},
        {"distortion_coeffs", json::array({0.0, 0.0, 0.0, 0.0})},
        {"distortion_model", "equidistant"},
        {"fps", 60.0},
        {"intrinsics",
         json::object({
             {"1920x1080",
              json::object({
                  {"fx", 1920.0},
                  {"fy", 1080.0},
                  {"ppx", 960.0},
                  {"ppy", 540.0},
              })},
         })},
        {"names", json::array({"height", "width", "channels"})},
        {"shape", json::array({1080, 1920, 3})},
    });
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
    entry["intrinsics"] = camera.contains("intrinsics") && camera["intrinsics"].is_object() && !camera["intrinsics"].empty()
                              ? camera["intrinsics"]
                              : json::object({
                                    {"640x400",
                                     json::object({
                                         {"fx", 640.0},
                                         {"fy", 400.0},
                                         {"ppx", 320.0},
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
    stereo["cam0"] = makeFaysCameraCalibrationEntry(
        faysCalibration.contains("cam0") && faysCalibration["cam0"].is_object()
            ? faysCalibration["cam0"]
            : json::object());
    stereo["cam1"] = makeFaysCameraCalibrationEntry(
        faysCalibration.contains("cam1") && faysCalibration["cam1"].is_object()
            ? faysCalibration["cam1"]
            : json::object());
    stereo["camera_model"] = faysCalibration.contains("camera_model") ? faysCalibration["camera_model"] : json("pinhole");
    stereo["distortion_model"] = normalizeDistortionModel(
        faysCalibration.contains("distortion_model") ? faysCalibration["distortion_model"] : json("ADM_KB4"));
    stereo["extrinsics"] = faysCalibration.contains("extrinsics") && faysCalibration["extrinsics"].is_object()
                               ? faysCalibration["extrinsics"]
                               : json::object();
    stereo["fps"] = 25.0;
    stereo["names"] = json::array({"height", "width", "channels"});
    stereo["shape"] = json::array({800, 640, 3});
    return stereo;
}

json makeDefaultStereoCalibrationEntry()
{
    const auto makeCamera = []() {
        return json::object({
            {"distortion_coeffs", json::array({0.0, 0.0, 0.0, 0.0})},
            {"intrinsics",
             json::object({
                 {"640x400",
                  json::object({
                      {"fx", 640.0},
                      {"fy", 400.0},
                      {"ppx", 320.0},
                      {"ppy", 200.0},
                  })},
             })},
        });
    };
    const auto identity = []() {
        return json::array({
            json::array({1.0, 0.0, 0.0, 0.0}),
            json::array({0.0, 1.0, 0.0, 0.0}),
            json::array({0.0, 0.0, 1.0, 0.0}),
            json::array({0.0, 0.0, 0.0, 1.0}),
        });
    };
    return json::object({
        {"cam0", makeCamera()},
        {"cam1", makeCamera()},
        {"camera_model", "pinhole"},
        {"distortion_model", "equidistant"},
        {"extrinsics",
         json::object({
             {"T_ic_cam0_to_imu0", identity()},
             {"T_ic_cam1_to_imu0", identity()},
             {"timeshift_cam0_to_imu0", 0.0},
             {"timeshift_cam1_to_imu0", 0.0},
         })},
        {"fps", 25.0},
        {"names", json::array({"height", "width", "channels"})},
        {"shape", json::array({800, 640, 3})},
    });
}

json makeFaysImuCalibrationEntry(const json &faysCalibration)
{
    json imu = json::object();
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

json makeDefaultFaysImuCalibrationEntry()
{
    return json::object({
        {"accelerometer",
         json::object({
             {"noise_density_discrete", 0.0},
             {"random_walk", 0.0},
         })},
        {"gyroscope",
         json::object({
             {"noise_density_discrete", 0.0},
             {"random_walk", 0.0},
         })},
        {"update_rate_hz", 0},
    });
}

json makeLockedCalibrationInfo(const std::string &calibrationStatus)
{
    return json::object({
        {"calibration_status", calibrationStatus},
        {"format_version", "3.0"},
    });
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
            logPerf((::DA::utils::LogString() << "[PERF] flush dir skip missing: phase=" << phaseLabel
                      << " path=" << directory).str());
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

        logPerf((::DA::utils::LogString() << "[PERF] flush dir done: phase=" << phaseLabel
                  << " path=" << directory
                  << " elapsed_ms=" << (steadyNowMs() - dirFlushStartMs)).str());
    }
}

void flushDirectoryTreeToDisk(const fs::path &rootDir, const char *phaseLabel)
{
    if (rootDir.empty())
    {
        return;
    }

    std::error_code rootStatusError;
    if (!fs::is_directory(rootDir, rootStatusError))
    {
        return;
    }

    std::vector<fs::path> directories;
    directories.push_back(rootDir);

    std::error_code walkError;
    for (fs::recursive_directory_iterator it(rootDir, fs::directory_options::skip_permission_denied, walkError), end;
         it != end;
         it.increment(walkError))
    {
        if (walkError)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to walk directory tree for flush: phase="
                          << phaseLabel << " root=" << rootDir
                          << " error=" << walkError.message() << std::endl).str());
            walkError.clear();
            continue;
        }

        std::error_code statusError;
        if (it->is_directory(statusError))
        {
            directories.push_back(it->path());
        }
    }

    for (auto rit = directories.rbegin(); rit != directories.rend(); ++rit)
    {
        const int64_t dirFlushStartMs = steadyNowMs();
        std::error_code error;
        if (!flushDirectoryToDisk(*rit, &error))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to flush directory tree entry: phase="
                          << phaseLabel << " path=" << *rit
                          << " error=" << error.message() << std::endl).str());
            continue;
        }

        logPerf((::DA::utils::LogString() << "[PERF] flush dir tree done: phase=" << phaseLabel
                  << " path=" << *rit
                  << " elapsed_ms=" << (steadyNowMs() - dirFlushStartMs)).str());
    }
}

bool shouldFlushVideoArtifactForPhase(const EpisodeVideoArtifact &artifact, const std::string &phase)
{
    const bool isStereo = std::string(artifact.fileName).find("stereo_") == 0;
    if (phase == "pre_stereo_finalize")
    {
        return !isStereo;
    }
    if (phase == "final")
    {
        return isStereo;
    }
    return false;
}

bool shouldFlushPathForPhase(const fs::path &episodeDir,
                             const fs::path &path,
                             const std::string &phase,
                             const std::set<std::string> &selectedVideoFileNames)
{
    (void)episodeDir;
    const std::string fileName = path.filename().string();
    if (selectedVideoFileNames.find(fileName) != selectedVideoFileNames.end())
    {
        return true;
    }

    if (phase == "pre_stereo_finalize")
    {
        return fileName == "sensor_left.mcap" ||
               fileName == "sensor_right.mcap" ||
               fileName == "calibration.json" ||
               fileName == "audio_pre.wav" ||
               fileName == "audio_post.wav";
    }
    if (phase == "final")
    {
        if (fileName == kEgoSyncStatusFileName)
        {
            return true;
        }
        const fs::path egoRelativePath = path.lexically_relative(episodeDir / "ego");
        if (!egoRelativePath.empty() &&
            egoRelativePath != fs::path(".") &&
            egoRelativePath.string().find("..") != 0)
        {
            return true;
        }
        return fileName == "fays_data_left.mcap" ||
               fileName == "fays_data_right.mcap";
    }
    if (phase == "metadata_final")
    {
        return fileName == "metadata.json" ||
               fileName == "validation_error.log";
    }
    return true;
}

void flushEpisodeArtifactsToDisk(const fs::path &episodeDir, bool chestCameraEnabled, const char *phaseLabel)
{
    if (episodeDir.empty())
    {
        return;
    }

    const std::string phase = phaseLabel != nullptr ? phaseLabel : "";
    std::vector<fs::path> paths;
    const auto artifacts = activeEpisodeVideoArtifacts(chestCameraEnabled);
    std::set<std::string> selectedVideoFileNames;
    paths.reserve(artifacts.size() + 9);

    for (const auto &artifact : artifacts)
    {
        if (shouldFlushVideoArtifactForPhase(artifact, phase))
        {
            paths.push_back(episodeDir / artifact.fileName);
            selectedVideoFileNames.insert(artifact.fileName);
        }
    }

    paths.push_back(episodeDir / "sensor_left.mcap");
    paths.push_back(episodeDir / "sensor_right.mcap");
    paths.push_back(episodeDir / "fays_data_left.mcap");
    paths.push_back(episodeDir / "fays_data_right.mcap");
    paths.push_back(episodeDir / "metadata.json");
    paths.push_back(episodeDir / "calibration.json");
    paths.push_back(episodeDir / "validation_error.log");
    paths.push_back(episodeDir / kEgoSyncStatusFileName);

    const fs::path egoDir = episodeDir / "ego";
    if (phase == "final" && fs::exists(egoDir))
    {
        std::error_code walkError;
        for (fs::recursive_directory_iterator it(egoDir, fs::directory_options::skip_permission_denied, walkError), end;
             it != end;
             it.increment(walkError))
        {
            if (walkError)
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to walk ego artifacts for flush: "
                              << egoDir << " error=" << walkError.message() << std::endl).str());
                walkError.clear();
                continue;
            }

            std::error_code statusError;
            if (it->is_regular_file(statusError))
            {
                paths.push_back(it->path());
            }
        }
    }

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
    std::vector<fs::path> filteredPaths;
    filteredPaths.reserve(paths.size());
    for (const auto &path : paths)
    {
        if (shouldFlushPathForPhase(episodeDir, path, phase, selectedVideoFileNames))
        {
            filteredPaths.push_back(path);
        }
    }

    logPerf((::DA::utils::LogString() << "[PERF] flush begin: phase=" << phaseLabel
              << " episode_dir=" << episodeDir
              << " candidate_paths=" << filteredPaths.size()).str());

    for (const auto &path : filteredPaths)
    {
        if (!fs::exists(path))
        {
            logPerf((::DA::utils::LogString() << "[PERF] flush skip missing: phase=" << phaseLabel
                     << " path=" << path).str());
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

        logPerf((::DA::utils::LogString() << "[PERF] flush done: phase=" << phaseLabel
                  << " path=" << path
                  << " elapsed_ms=" << (steadyNowMs() - artifactFlushStartMs)).str());
    }

    if (phase == "final")
    {
        flushDirectoryTreeToDisk(egoDir, phaseLabel);
    }
    flushEpisodeDirectoriesToDisk(episodeDir, phaseLabel);

    logPerf((::DA::utils::LogString() << "[PERF] flush end: phase=" << phaseLabel
              << " episode_dir=" << episodeDir
              << " elapsed_ms=" << (steadyNowMs() - flushStartMs)).str());
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

bool isYuzhouMainCameraSn(const std::string &sn)
{
    if (sn.size() != kYuzhouSnLength || sn.rfind("FE", 0) != 0)
    {
        return false;
    }
    return std::all_of(sn.begin() + 2, sn.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

struct YuzhouMainCameraIdentity
{
    std::string cameraName;
    std::string devicePath;
    std::string resolvedTarget;
    std::string serialNumber;
    bool calibrationPayloadCached = false;
    std::array<uint8_t, kYuzhouPayloadSize> calibrationPayload{};
};

bool readYuzhouXuChunk(int fd, size_t chunkIndex, uint8_t *chunkData, std::string *detail)
{
    if (chunkIndex >= kYuzhouPayloadChunkCount)
    {
        if (detail != nullptr)
        {
            *detail = "chunk index out of range: " + std::to_string(chunkIndex);
        }
        return false;
    }

    std::array<uint8_t, kYuzhouXuPacketSize> readIndexCommand = {
        0x97, 0x97, 0x97, 0x97, 0x97, 0x97, 0x97, 0x01, static_cast<uint8_t>(chunkIndex)};
    struct uvc_xu_control_query setQuery = {};
    setQuery.unit = kYuzhouXuUnitId;
    setQuery.selector = kYuzhouXuSelector;
    setQuery.query = UVC_SET_CUR;
    setQuery.size = kYuzhouXuPacketSize;
    setQuery.data = readIndexCommand.data();
    if (ioctl(fd, UVCIOC_CTRL_QUERY, &setQuery) < 0)
    {
        if (detail != nullptr)
        {
            *detail = "UVC SET_CUR failed at chunk " + std::to_string(chunkIndex) + ": " +
                      std::strerror(errno);
        }
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::array<uint8_t, kYuzhouXuPacketSize> response{};
    struct uvc_xu_control_query getQuery = {};
    getQuery.unit = kYuzhouXuUnitId;
    getQuery.selector = kYuzhouXuSelector;
    getQuery.query = UVC_GET_CUR;
    getQuery.size = kYuzhouXuPacketSize;
    getQuery.data = response.data();
    if (ioctl(fd, UVCIOC_CTRL_QUERY, &getQuery) < 0)
    {
        if (detail != nullptr)
        {
            *detail = "UVC GET_CUR failed at chunk " + std::to_string(chunkIndex) + ": " +
                      std::strerror(errno);
        }
        return false;
    }

    std::memcpy(chunkData, response.data() + 1, kYuzhouPayloadChunkSize);
    return true;
}

bool validateYuzhouMainCameraCalibrationPayload(const std::array<uint8_t, kYuzhouPayloadSize> &payload,
                                                std::string *detail)
{
    main_camera::MainCameraCalibrationDataV1 data{};
    std::memcpy(&data, payload.data(), sizeof(data));
    if (std::memcmp(data.header.magic, "MCAL", 4) != 0)
    {
        if (detail != nullptr)
        {
            *detail = "invalid MCAL payload magic";
        }
        return false;
    }
    if (data.header.dataFormatVersion != main_camera::kCalibrationDataFormatVersion1)
    {
        if (detail != nullptr)
        {
            *detail = "unsupported MCAL data_format_version=" +
                      std::to_string(data.header.dataFormatVersion);
        }
        return false;
    }
    if (data.header.headerSize != sizeof(main_camera::MainCameraCalibrationHeader))
    {
        if (detail != nullptr)
        {
            *detail = "invalid MCAL header_size=" + std::to_string(data.header.headerSize);
        }
        return false;
    }
    const uint16_t minimumPayloadSize =
        static_cast<uint16_t>(sizeof(main_camera::MainCameraCalibrationHeader) +
                              sizeof(main_camera::MainCameraIntrinsicsBlock));
    if (data.header.payloadSize < minimumPayloadSize ||
        data.header.payloadSize > main_camera::kCalibrationPayloadSize)
    {
        if (detail != nullptr)
        {
            *detail = "invalid MCAL payload_size=" + std::to_string(data.header.payloadSize);
        }
        return false;
    }

    constexpr uint32_t requiredFields =
        main_camera::kCalibrationValidCameraModel |
        main_camera::kCalibrationValidDistortion |
        main_camera::kCalibrationValidIntrinsics |
        main_camera::kCalibrationValidResolution;
    if ((data.header.validFields & requiredFields) != requiredFields)
    {
        if (detail != nullptr)
        {
            *detail = "MCAL valid_fields missing required bits";
        }
        return false;
    }

    if (data.mainCamera.resolution[0] <= 0.0f || data.mainCamera.resolution[1] <= 0.0f ||
        data.mainCamera.intrinsics[0] <= 0.0f || data.mainCamera.intrinsics[1] <= 0.0f)
    {
        if (detail != nullptr)
        {
            *detail = "MCAL intrinsics or resolution is empty";
        }
        return false;
    }
    return true;
}

std::optional<YuzhouMainCameraIdentity> readYuzhouMainCameraIdentity(const std::string &cameraName,
                                                                    const std::string &devicePath,
                                                                    const std::string &resolvedTarget,
                                                                    std::string *detail)
{
    if (!fs::exists(devicePath))
    {
        if (detail != nullptr)
        {
            *detail = "device node missing";
        }
        return std::nullopt;
    }

    const int fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        if (detail != nullptr)
        {
            *detail = std::string("open failed: ") + std::strerror(errno);
        }
        return std::nullopt;
    }

    std::array<uint8_t, kYuzhouPayloadSize> payload{};
    for (size_t chunkIndex = 0; chunkIndex < 10; ++chunkIndex)
    {
        if (!readYuzhouXuChunk(fd,
                               chunkIndex,
                               payload.data() + chunkIndex * kYuzhouPayloadChunkSize,
                               detail))
        {
            close(fd);
            return std::nullopt;
        }
    }

    close(fd);

    std::string calibrationDetail;
    if (!validateYuzhouMainCameraCalibrationPayload(payload, &calibrationDetail))
    {
        if (detail != nullptr)
        {
            *detail = calibrationDetail;
        }
        return std::nullopt;
    }

    const std::string sn(reinterpret_cast<const char *>(payload.data() + kYuzhouSnOffset), kYuzhouSnLength);
    if (!isYuzhouMainCameraSn(sn))
    {
        if (detail != nullptr)
        {
            *detail = "invalid or empty Yuzhou SN field";
        }
        return std::nullopt;
    }
    if (detail != nullptr)
    {
        *detail = "yuzhou_xu_fast";
    }

    YuzhouMainCameraIdentity result;
    result.cameraName = cameraName;
    result.devicePath = devicePath;
    result.resolvedTarget = resolvedTarget;
    result.serialNumber = sn;
    result.calibrationPayloadCached = true;
    result.calibrationPayload = payload;
    return result;
}

std::string probeMainCameraSerialForDeviceNode(const std::string &devicePath)
{
    std::string detail;
    const auto identity = readYuzhouMainCameraIdentity("", devicePath, "", &detail);
    if (identity.has_value())
    {
        return identity->serialNumber;
    }
    DM_LOG_WARN("{}", (::DA::utils::LogString()
                       << "failed to read Yuzhou XU SN from " << devicePath
                       << ": " << detail << std::endl).str());
    return "";
}

std::string resolveDeviceNodeTarget(const std::string &devicePath)
{
    std::error_code error;
    fs::path target;
    if (fs::is_symlink(devicePath, error))
    {
        target = fs::read_symlink(devicePath, error);
        if (!error)
        {
            if (target.is_relative())
            {
                target = fs::path(devicePath).parent_path() / target;
            }
            return target.lexically_normal().string();
        }
    }
    target = fs::canonical(devicePath, error);
    if (!error)
    {
        return target.string();
    }
    return devicePath;
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
    logPerf((::DA::utils::LogString() << "[PERF] ffprobe begin: file=" << filePath).str());
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
        logPerf((::DA::utils::LogString() << "[PERF] ffprobe timeout: file=" << filePath
                  << " elapsed_ms=" << (steadyNowMs() - probeStartMs)).str());
        if (errorMessage != nullptr)
        {
            *errorMessage = "ffprobe timeout after " + std::to_string(kVideoProbeTimeoutMs) + "ms";
        }
        return false;
    }
    if (!probe.success)
    {
        logPerf((::DA::utils::LogString() << "[PERF] ffprobe failed: file=" << filePath
                  << " elapsed_ms=" << (steadyNowMs() - probeStartMs)
                  << " exit_code=" << probe.exitCode).str());
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

    logPerf((::DA::utils::LogString() << "[PERF] ffprobe done: file=" << filePath
              << " elapsed_ms=" << (steadyNowMs() - probeStartMs)
              << " start_sec=" << formatSeconds(result->startTimeSec)
              << " duration_sec=" << formatSeconds(result->durationSec)
              << " span_sec=" << formatSeconds(result->spanSec)).str());
    return true;
}

std::vector<VideoProbeTaskResult> probeVideoFilesParallel(
    const std::string &episodeDir,
    const std::vector<EpisodeVideoArtifact> &artifacts)
{
    std::vector<std::future<VideoProbeTaskResult>> futures;
    futures.reserve(artifacts.size());
    for (const auto &artifact : artifacts)
    {
        futures.push_back(std::async(
            std::launch::async,
            [episodeDir, artifact]() {
                VideoProbeTaskResult task;
                task.probe.cameraName = artifact.cameraName;
                task.probe.fileName = artifact.fileName;
                task.ok = probeVideoFile(
                    (fs::path(episodeDir) / artifact.fileName).string(),
                    &task.probe,
                    &task.errorMessage);
                return task;
            }));
    }

    std::vector<VideoProbeTaskResult> results;
    results.reserve(futures.size());
    for (auto &future : futures)
    {
        results.push_back(future.get());
    }
    return results;
}

bool writeVideoProbeCacheToTimingFile(const std::string &episodeDir,
                                      const std::vector<VideoProbeResult> &probes,
                                      std::string *errorMessage)
{
    const fs::path timingPath = episodeTimingPath(episodeDir);
    json timing = json::object();
    if (!loadJsonFile(timingPath.string(), &timing, errorMessage))
    {
        return false;
    }
    if (!timing.is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "internal timing file top-level value must be an object";
        }
        return false;
    }

    json cache = json::array();
    for (const auto &probe : probes)
    {
        cache.push_back(json::object({
            {"camera_name", probe.cameraName},
            {"file_name", probe.fileName},
            {"start_time_sec", probe.startTimeSec},
            {"duration_sec", probe.durationSec},
            {"span_sec", probe.spanSec},
        }));
    }
    timing["video_probes"] = std::move(cache);
    return writeTextFileAtomically(timingPath, timing.dump(2) + "\n", errorMessage);
}

bool loadCachedVideoProbe(const json &timing,
                          const EpisodeVideoArtifact &artifact,
                          VideoProbeResult *probe)
{
    if (probe == nullptr || !timing.contains("video_probes") || !timing["video_probes"].is_array())
    {
        return false;
    }

    for (const auto &entry : timing["video_probes"])
    {
        if (!entry.is_object() ||
            entry.value("camera_name", std::string()) != artifact.cameraName ||
            entry.value("file_name", std::string()) != artifact.fileName)
        {
            continue;
        }
        if (!entry.contains("start_time_sec") ||
            !entry.contains("duration_sec") ||
            !entry.contains("span_sec"))
        {
            return false;
        }
        const json &start = entry["start_time_sec"];
        const json &duration = entry["duration_sec"];
        const json &span = entry["span_sec"];
        if (!start.is_number() || !duration.is_number() || !span.is_number())
        {
            return false;
        }

        probe->cameraName = artifact.cameraName;
        probe->fileName = artifact.fileName;
        probe->startTimeSec = start.get<double>();
        probe->durationSec = duration.get<double>();
        probe->spanSec = span.get<double>();
        return std::isfinite(probe->durationSec) && probe->durationSec > 0.0 &&
               std::isfinite(probe->spanSec) && probe->spanSec > 0.0;
    }
    return false;
}

bool validateMetadataVideoDetails(const fs::path &metadataPath,
                                  const std::vector<EpisodeVideoArtifact> &artifacts,
                                  std::string *errorMessage)
{
    json metadata = json::object();
    if (!loadJsonFile(metadataPath.string(), &metadata, errorMessage))
    {
        return false;
    }
    if (!metadata.is_object())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "metadata.json top-level value must be an object";
        }
        return false;
    }
    if (!metadata.contains("video_details") || !metadata["video_details"].is_array())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "metadata.json missing video_details array";
        }
        return false;
    }

    std::set<std::string> requiredFileNames;
    for (const auto &artifact : artifacts)
    {
        requiredFileNames.insert(artifact.fileName);
    }

    std::set<std::string> seenFileNames;
    for (const auto &detail : metadata["video_details"])
    {
        if (!detail.is_object() ||
            !detail.contains("name") || !detail["name"].is_string() ||
            !detail.contains("start_offset_us") || !detail["start_offset_us"].is_number_integer() ||
            !detail.contains("duration_s") || !detail["duration_s"].is_number())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "metadata.json has invalid video_details entry";
            }
            return false;
        }

        const std::string fileName = detail["name"].get<std::string>();
        if (requiredFileNames.find(fileName) == requiredFileNames.end())
        {
            continue;
        }

        const int64_t startOffsetUs = detail["start_offset_us"].get<int64_t>();
        if (startOffsetUs < 0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "metadata.json has negative start_offset_us for " + fileName;
            }
            return false;
        }

        const double durationS = detail["duration_s"].get<double>();
        if (!std::isfinite(durationS) || durationS <= 0.0)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "metadata.json has invalid duration_s for " + fileName;
            }
            return false;
        }
        seenFileNames.insert(fileName);
    }

    for (const auto &fileName : requiredFileNames)
    {
        if (seenFileNames.find(fileName) == seenFileNames.end())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "metadata.json missing video_details entry for " + fileName;
            }
            return false;
        }
    }
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
        mcap::ReadSummaryMethod::NoFallbackScan,
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

bool loadFaysMcapSummary(const std::string &mcapPath,
                         const std::string &imuTopic,
                         const std::string &cameraTopic,
                         FaysMcapSummary *summary,
                         std::string *errorMessage)
{
    (void)imuTopic;
    (void)cameraTopic;
    if (summary == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "internal error: missing Fays tail result slot";
        }
        return false;
    }

    *summary = FaysMcapSummary{};

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
        mcap::ReadSummaryMethod::NoFallbackScan,
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

    if (reader.statistics().has_value())
    {
        const auto imuCountIt = reader.statistics()->channelMessageCounts.find(kFaysImuChannelId);
        if (imuCountIt != reader.statistics()->channelMessageCounts.end())
        {
            summary->imuMessageCount = imuCountIt->second;
        }
        const auto cameraCountIt = reader.statistics()->channelMessageCounts.find(kFaysCameraChannelId);
        if (cameraCountIt != reader.statistics()->channelMessageCounts.end())
        {
            summary->cameraMessageCount = cameraCountIt->second;
        }
        const auto messageStartTime = reader.statistics()->messageStartTime;
        const auto messageEndTime = reader.statistics()->messageEndTime;
        if (messageEndTime > messageStartTime)
        {
            summary->spanNs = messageEndTime - messageStartTime;
        }
    }

    if (summary->imuMessageCount == 0 || summary->cameraMessageCount == 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Fays summary has zero messages: imu_count=" +
                            std::to_string(summary->imuMessageCount) +
                            ", camera_count=" + std::to_string(summary->cameraMessageCount);
        }
        return false;
    }

    const auto classifyFaysMessage = [](const mcap::Message &message) {
        if (message.channelId == kFaysImuChannelId || message.dataSize == kFaysImuPayloadBytes)
        {
            return 'i';
        }
        if (message.channelId == kFaysCameraChannelId || message.dataSize == kFaysCameraPayloadBytes)
        {
            return 'c';
        }
        return '\0';
    };
    bool foundImu = false;
    bool foundCamera = false;
    auto *dataSource = reader.dataSource();
    const auto &chunkIndexes = reader.chunkIndexes();
    if (dataSource != nullptr && !chunkIndexes.empty())
    {
        size_t scannedChunks = 0;
        for (auto it = chunkIndexes.rbegin();
             it != chunkIndexes.rend() && scannedChunks < kFaysTailChunkScanLimit && !(foundImu && foundCamera);
             ++it, ++scannedChunks)
        {
            mcap::TypedRecordReader recordReader(
                *dataSource,
                it->chunkStartOffset,
                it->chunkStartOffset + it->chunkLength);
            recordReader.onMessage = [&](const mcap::Message &message,
                                         mcap::ByteOffset,
                                         std::optional<mcap::ByteOffset>) {
                const char kind = classifyFaysMessage(message);
                if (kind == 'i')
                {
                    if (!foundImu || message.logTime > summary->lastImuLogTimeNs)
                    {
                        summary->lastImuLogTimeNs = message.logTime;
                        foundImu = true;
                    }
                }
                else if (kind == 'c')
                {
                    if (!foundCamera || message.logTime > summary->lastCameraLogTimeNs)
                    {
                        summary->lastCameraLogTimeNs = message.logTime;
                        foundCamera = true;
                    }
                }
            };

            while (recordReader.next())
            {
            }

            if (!recordReader.status().ok() && errorMessage != nullptr && errorMessage->empty())
            {
                *errorMessage = "failed to read Fays tail chunk: " + recordReader.status().message;
            }
        }
    }
    else
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "missing Fays chunk indexes";
        }
        return false;
    }
    if (!foundImu || !foundCamera)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Fays topic has zero messages: imu=" +
                            std::string(foundImu ? "present" : "zero") +
                            ", camera=" +
                            std::string(foundCamera ? "present" : "zero");
        }
        return false;
    }
    if (summary->spanNs == 0 && !reader.chunkIndexes().empty())
    {
        uint64_t firstChunkMessageNs = std::numeric_limits<uint64_t>::max();
        uint64_t lastChunkMessageNs = 0;
        for (const auto &chunkIndex : reader.chunkIndexes())
        {
            if (chunkIndex.messageStartTime < firstChunkMessageNs)
            {
                firstChunkMessageNs = chunkIndex.messageStartTime;
            }
            if (chunkIndex.messageEndTime > lastChunkMessageNs)
            {
                lastChunkMessageNs = chunkIndex.messageEndTime;
            }
        }
        if (lastChunkMessageNs > firstChunkMessageNs && firstChunkMessageNs != std::numeric_limits<uint64_t>::max())
        {
            summary->spanNs = lastChunkMessageNs - firstChunkMessageNs;
        }
    }
    if (summary->spanNs < static_cast<uint64_t>(kFaysMcapMinSpanNs))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Fays mcap span too short: span_ns=" + std::to_string(summary->spanNs) +
                            ", threshold_ns=" + std::to_string(kFaysMcapMinSpanNs);
        }
        return false;
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
    requestStopBackgroundTactileValidation();
    waitForMainCameraRefreshes();
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

    perfLogEnabled_ = !isFalseLikeValue(readEnvValue(options_.envFile, kPerfLogEnvKey));
    g_perfLogEnabled = perfLogEnabled_;
    DM_LOG_INFO("{}", (::DA::utils::LogString() << kPerfLogEnvKey << "="
                         << (perfLogEnabled_ ? "true" : "false") << std::endl).str());

    hardwareVersion_ = readEnvValue(options_.envFile, "UGRIPPER_HARDWARE_VERSION");
    if (hardwareVersion_.empty())
    {
        hardwareVersion_ = "v2.5";
    }

    if (options_.gripperPorts.empty())
    {
        options_.gripperPorts.emplace_back("/dev/right_gripper");
        options_.gripperPorts.emplace_back("/dev/left_gripper");
    }

    std::error_code error;
    fs::create_directories(options_.audioTempDir, error);

    packageVersion_ = queryPackageVersion("ugripper");
    updaterVersion_ = queryPackageVersion("das-usb-updater");
    if (updaterVersion_ == "unknown")
    {
        updaterVersion_ = queryPackageVersion("ugripper-usb-updater");
    }

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
        hardwareVersion_,
        packageVersion_,
        updaterVersion_);

    if (!episodeManager_->initialize())
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to initialize episode manager for disk root: "
                  << options_.diskRoot << std::endl).str());
        return false;
    }
    initializeMainCameraRuntimeStates();

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
                    "--control-fifo",
                    options_.stereoControlPipe,
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
            .control_pipe = options_.stereoControlPipe,
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
            .stereo_startup_grace_ms = 12000,
        },
        ugripper::runtime::HealthMonitor::Dependencies{
            .get_disk_fault =
                [](const std::string& path) {
                    return detectDiskHealthFault(path);
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
                    if (episodeManager_ == nullptr)
                    {
                        return false;
                    }
                    return episodeManager_->validateEpisode(episode_dir, error_message, nullptr);
                },
            .prepare_sensor_start = nullptr,
            .finalize_sensor_start = nullptr,
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
            .start_ego_recording =
                [this](const std::string& episode_dir, int64_t start_system_time_us, std::string* error_message) {
                    return startEgoRecording(episode_dir, start_system_time_us, error_message);
                },
            .stop_ego_recording =
                [this](const std::string& episode_dir, int64_t stop_system_time_us, std::string* error_message) {
                    return stopEgoRecording(episode_dir, stop_system_time_us, error_message);
                },
            .wait_for_ego_finalize =
                [this](const std::string& episode_dir, int timeout_ms, std::string* error_message) {
                    return waitForEgoFinalize(episode_dir, timeout_ms, error_message);
                },
            .wait_for_stereo_finalize =
                [this](const std::string& episode_dir, int timeout_ms, std::string* error_message) {
                    return waitForStereoFinalize(episode_dir, timeout_ms, error_message);
                },
            .flush_episode_artifacts =
                [this](const std::string& episode_dir, const char* stage) {
                    flushEpisodeArtifactsToDisk(fs::path(episode_dir), chestCameraEnabled_, stage);
                },
            .write_episode_metadata =
                [this](const std::string& episode_dir, bool quality_ok, const std::string& error_message) {
                    if (episodeManager_ == nullptr)
                    {
                        return;
                    }
                    std::string metadataError;
                    if (!episodeManager_->writeFinalMetadata(episode_dir, quality_ok, error_message, &metadataError))
                    {
                        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write metadata.json: "
                                      << metadataError << std::endl).str());
                        return;
                    }
                    flushEpisodeArtifactsToDisk(fs::path(episode_dir), chestCameraEnabled_, "metadata_final");
                },
            .write_validation_error_log =
                [](const std::string& episode_dir, const std::string& error_message) {
                    writeValidationErrorLog(fs::path(episode_dir), error_message);
                },
            .finalize_episode_dir =
                [this](const std::string& episode_dir, std::string* error_message) {
                    if (episodeManager_ == nullptr)
                    {
                        if (error_message != nullptr)
                        {
                            *error_message = "episode manager not initialized";
                        }
                        return std::string();
                    }
                    return episodeManager_->finalizeEpisodeDir(episode_dir, error_message);
                },
            .write_recording_lock =
                [this](const std::string& episode_dir) {
                    return writeRecordingLock(episode_dir);
                },
            .remove_recording_lock =
                [this]() {
                    removeRecordingLock();
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
            .log_perf =
                [this](const std::string& message) {
                    if (perfLogEnabled_)
                    {
                        DM_LOG_INFO("{}", (::DA::utils::LogString() << message << std::endl).str());
                    }
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
    if (episodeManager_ != nullptr)
    {
        episodeManager_->setGripperRuntimeStates(gripperRuntimeStates_);
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
        maintainMainCameraRuntimeStates();
        maintainAudioPlayer();
        maintainStereoDaemon();
        maintainBackgroundTactileValidation();
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
    stopStereoDaemon();
    setLedState(LedState::Exit);
    return 0;
}

void RecordRuntime::requestStop()
{
    stopRequested_.store(true);
    if (egoRecordingWorker_.has_value())
    {
        std::string ignoredError;
        egoRecordingWorker_->Stop(
            ugripper::runtime::ProcessStopMode::SigTermThenKill,
            1000,
            &ignoredError);
        egoRecordingWorker_.reset();
    }
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write stereo control pipe: " << errorMessage << std::endl).str());
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

std::vector<std::string> egoWorkerArgs(const std::string &script,
                                       const std::string &command,
                                       const std::string &episodeDir)
{
    std::vector<std::string> args = resolvePythonCommand();
    args.push_back(script);
    args.push_back(command);
    args.push_back("--episode-dir");
    args.push_back(episodeDir);
    args.push_back("--status-file");
    args.push_back((fs::path(episodeDir) / kEgoSyncStatusFileName).string());
    return args;
}

bool RecordRuntime::startEgoRecording(const std::string &episodeDir,
                                      int64_t,
                                      std::string *errorMessage)
{
    egoRecordingAttempted_ = false;
    if (!fs::exists(options_.egoRecordingScript))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ego recording script not found: " + options_.egoRecordingScript;
        }
        return false;
    }

    if (egoRecordingWorker_.has_value())
    {
        std::string ignoredError;
        egoRecordingWorker_->Stop(
            ugripper::runtime::ProcessStopMode::SigTermThenKill,
            1000,
            &ignoredError);
        egoRecordingWorker_.reset();
    }

    egoRecordingWorker_.emplace("ego_recording_worker");
    egoRecordingAttempted_ = true;
    if (!egoRecordingWorker_->Start(egoWorkerArgs(options_.egoRecordingScript, "start", episodeDir),
                                   {},
                                   errorMessage))
    {
        egoRecordingWorker_.reset();
        egoRecordingAttempted_ = false;
        return false;
    }
    return true;
}

bool RecordRuntime::stopEgoRecording(const std::string &episodeDir,
                                     int64_t,
                                     std::string *errorMessage)
{
    if (egoRecordingWorker_.has_value())
    {
        std::string ignoredError;
        egoRecordingWorker_->Stop(
            ugripper::runtime::ProcessStopMode::SigTermThenKill,
            1500,
            &ignoredError);
        egoRecordingWorker_.reset();
    }

    if (!fs::exists(options_.egoRecordingScript))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ego recording script not found: " + options_.egoRecordingScript;
        }
        return false;
    }

    return runCommandSync(egoWorkerArgs(options_.egoRecordingScript, "stop", episodeDir));
}

bool RecordRuntime::waitForEgoFinalize(const std::string &episodeDir,
                                       int timeoutMs,
                                       std::string *errorMessage)
{
    if (!egoRecordingAttempted_)
    {
        return true;
    }
    const fs::path statusPath = fs::path(episodeDir) / kEgoSyncStatusFileName;
    const uint64_t startMs = currentSteadyMs();
    while ((currentSteadyMs() - startMs) < static_cast<uint64_t>(std::max(timeoutMs, 0)))
    {
        json status = json::object();
        std::string loadError;
        if (loadJsonFile(statusPath.string(), &status, &loadError) && status.is_object())
        {
            const std::string state = status.value("status", std::string());
            if (state == "finalized" || state == "not_found" || state == "stop_no_remote_episode")
            {
                return true;
            }
            if (state == "finalize_timeout" || state == "error")
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = status.value("error", state);
                }
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kEgoFinalizeWaitPollMs));
    }

    if (errorMessage != nullptr)
    {
        *errorMessage = "timed out waiting for ego finalize status";
    }
    return false;
}

bool RecordRuntime::mergeEpisodeInfo(const std::string &episodeDir, std::string *errorMessage) const
{
    const fs::path baseInfoPath = episodeTimingPath(episodeDir);

    std::ifstream baseInput(baseInfoPath);
    if (!baseInput.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("cannot open internal timing file: ") + kEpisodeTimingFileName;
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
            *errorMessage = std::string("cannot rewrite internal timing file: ") + kEpisodeTimingFileName;
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
            .last_error = runtimeState.lastError,
        };

        ugripper::runtime::ApplyGripperConnectionEvent(&view, &pendingGripperRefresh_[index], event.side, event.connected);
        runtimeState.side = view.side;
        runtimeState.connected = view.connected;
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
    state.lastError = errorMessage;
}

void RecordRuntime::refreshGripperRuntimeStateForSide(const std::string &side)
{
    auto &state = gripperRuntimeStates_[gripperStateIndexForSide(side)];
    clearGripperRuntimeStateForSide(side, true, "refreshing", "");

    std::string serialNumber;
    bool hasSerialNumber = false;
    std::string runtimeReadError;
    if (!panelManager_.readRuntimeIdentityForSide(side, &serialNumber, &hasSerialNumber, &runtimeReadError))
    {
        if (!runtimeReadError.empty())
        {
            state.lastError = runtimeReadError;
        }
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "gripper runtime cache refresh failed: side=" << side
            << " detail=" << (runtimeReadError.empty() ? "readRuntimeIdentityForSide failed" : runtimeReadError)
            << std::endl).str());
        return;
    }

    state.connected = true;
    state.hasSerialNumber = hasSerialNumber;
    state.serialNumber = hasSerialNumber ? serialNumber : "";
    state.lastError.clear();
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "gripper runtime cache refreshed: side=" << side
        << " has_sn=" << boolText(hasSerialNumber)
        << " sn=" << state.serialNumber
        << std::endl).str());
    refreshTactileReferenceCachesForSide(side);
}

void RecordRuntime::initializeMainCameraRuntimeStates()
{
    mainCameraRuntimeStates_.clear();
    const std::array<std::tuple<const char *, const char *, bool>, 3> cameras = {{
        {"right_cam_main", "/dev/cam_right", true},
        {"left_cam_main", "/dev/cam_left", true},
        {"chest_cam_main", "/dev/cam_chest", chestCameraEnabled_},
    }};

    const uint64_t nowMs = currentSteadyMs();
    for (const auto &[cameraName, devicePath, enabled] : cameras)
    {
        MainCameraRuntimeCache state;
        state.cameraName = cameraName;
        state.devicePath = devicePath;
        state.enabled = enabled;
        state.nextRefreshAllowedMs = nowMs + kMainCameraRefreshDelayMs;
        mainCameraRuntimeStates_.push_back(std::move(state));
    }
    syncMainCameraRuntimeStatesToEpisodeManager();
}

void RecordRuntime::maintainMainCameraRuntimeStates()
{
    bool updatedAny = false;
    const uint64_t nowMs = currentSteadyMs();

    for (auto &state : mainCameraRuntimeStates_)
    {
        if (!state.enabled)
        {
            continue;
        }

        if (state.refreshFuture.valid() &&
            state.refreshFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            const auto result = state.refreshFuture.get();
            if (state.present && result.resolvedTarget == state.resolvedTarget && result.errorMessage.empty())
            {
                state.serialNumber = result.serialNumber;
                state.calibrationPayloadCached = result.calibrationPayloadCached;
                state.calibrationPayload = result.calibrationPayload;
                state.lastError.clear();
                state.nextRefreshAllowedMs = std::numeric_limits<uint64_t>::max();
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                    << "main camera runtime cache refreshed: camera=" << state.cameraName
                    << " device=" << state.devicePath
                    << " target=" << state.resolvedTarget
                    << " sn=" << state.serialNumber << std::endl).str());
            }
            else if (state.present && result.resolvedTarget == state.resolvedTarget)
            {
                const bool shouldLogFailure =
                    state.lastError.empty() ||
                    state.lastError == "waiting for XU cache refresh" ||
                    state.lastError == "refreshing";
                state.serialNumber.clear();
                state.calibrationPayloadCached = false;
                state.calibrationPayload = {};
                state.lastError = result.errorMessage.empty() ? "unknown XU read failure" : result.errorMessage;
                state.nextRefreshAllowedMs = nowMs + kMainCameraRefreshRetryMs;
                if (shouldLogFailure)
                {
                    DM_LOG_WARN("{}", (::DA::utils::LogString()
                        << "main camera runtime cache refresh failed: camera=" << state.cameraName
                        << " device=" << state.devicePath
                        << " target=" << state.resolvedTarget
                        << " detail=" << state.lastError << std::endl).str());
                }
            }
            updatedAny = true;
        }

        const bool exists = fs::exists(state.devicePath);
        if (!exists)
        {
            if (state.present || !state.serialNumber.empty() || state.calibrationPayloadCached)
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "main camera device removed, clear runtime cache: camera=" << state.cameraName
                    << " device=" << state.devicePath << std::endl).str());
                state.present = false;
                state.resolvedTarget.clear();
                state.serialNumber.clear();
                state.calibrationPayloadCached = false;
                state.calibrationPayload = {};
                state.lastError = "device node missing";
                updatedAny = true;
            }
            continue;
        }

        const std::string resolvedTarget = resolveDeviceNodeTarget(state.devicePath);
        if (!state.present || state.resolvedTarget != resolvedTarget)
        {
            state.present = true;
            state.resolvedTarget = resolvedTarget;
            state.serialNumber.clear();
            state.calibrationPayloadCached = false;
            state.calibrationPayload = {};
            state.lastError = "waiting for XU cache refresh";
            state.nextRefreshAllowedMs = nowMs + kMainCameraRefreshDelayMs;
            updatedAny = true;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "main camera device detected, schedule runtime cache refresh: camera=" << state.cameraName
                << " device=" << state.devicePath
                << " target=" << state.resolvedTarget << std::endl).str());
        }

        if (state.refreshFuture.valid() || nowMs < state.nextRefreshAllowedMs)
        {
            continue;
        }

        const std::string cameraName = state.cameraName;
        const std::string devicePath = state.devicePath;
        const std::string resolvedTargetForRead = state.resolvedTarget;
        if (state.lastError.empty() || state.lastError == "waiting for XU cache refresh")
        {
            state.lastError = "refreshing";
        }
        state.refreshFuture = std::async(std::launch::async,
            [cameraName, devicePath, resolvedTargetForRead]() {
                MainCameraRefreshResult result;
                result.cameraName = cameraName;
                result.devicePath = devicePath;
                result.resolvedTarget = resolvedTargetForRead;
                std::string detail;
                const auto identity =
                    readYuzhouMainCameraIdentity(cameraName, devicePath, resolvedTargetForRead, &detail);
                if (!identity.has_value())
                {
                    result.errorMessage = detail.empty() ? "failed to read Yuzhou XU identity" : detail;
                    return result;
                }
                result.serialNumber = identity->serialNumber;
                result.calibrationPayloadCached = identity->calibrationPayloadCached;
                result.calibrationPayload = identity->calibrationPayload;
                return result;
            });
        updatedAny = true;
    }

    if (updatedAny)
    {
        syncMainCameraRuntimeStatesToEpisodeManager();
    }
}

void RecordRuntime::syncMainCameraRuntimeStatesToEpisodeManager()
{
    if (episodeManager_ == nullptr)
    {
        return;
    }

    std::array<EpisodeManager::MainCameraRuntimeState, 3> states{};
    for (size_t index = 0; index < states.size() && index < mainCameraRuntimeStates_.size(); ++index)
    {
        const auto &source = mainCameraRuntimeStates_[index];
        auto &target = states[index];
        target.cameraName = source.cameraName;
        target.devicePath = source.devicePath;
        target.enabled = source.enabled;
        target.present = source.present;
        target.serialNumber = source.serialNumber;
        target.calibrationPayloadCached = source.calibrationPayloadCached;
        target.calibrationPayload = source.calibrationPayload;
        target.lastError = source.lastError;
    }
    episodeManager_->setMainCameraRuntimeStates(states);
}

void RecordRuntime::waitForMainCameraRefreshes()
{
    for (auto &state : mainCameraRuntimeStates_)
    {
        if (state.refreshFuture.valid())
        {
            state.refreshFuture.wait();
        }
    }
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

void RecordRuntime::applyTactileValidationFindings(
    const std::vector<EpisodeManager::TactileValidationFinding> &findings)
{
    bool warningActive = false;
    for (const auto &finding : findings)
    {
        if (tactileTriggeredAudioCommand_.empty() && finding.warningTriggered)
        {
            tactileTriggeredAudioCommand_ = finding.audioCommand;
        }
        if (finding.warningActive)
        {
            warningActive = true;
        }
    }
    tactileWarningActive_ = warningActive;
}

void RecordRuntime::scheduleBackgroundTactileValidation(const std::string &episodeDir)
{
    if (episodeDir.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(backgroundTactileMutex_);
        pendingBackgroundTactileEpisodeDir_ = episodeDir;
    }
    logPerf((::DA::utils::LogString()
             << "[PERF] tactile background validation queued: episode_dir=" << episodeDir).str());
}

void RecordRuntime::maintainBackgroundTactileValidation()
{
    if (backgroundTactileFuture_.valid() &&
        backgroundTactileFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
    {
        BackgroundTactileValidationResult result = backgroundTactileFuture_.get();
        if (!result.findings.empty())
        {
            applyTactileValidationFindings(result.findings);
            if (!isRecordingActive())
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
        }
        logPerf((::DA::utils::LogString()
                 << "[PERF] tactile background validation applied: episode_dir="
                 << result.episodeDir
                 << " findings=" << result.findings.size()).str());
    }

    if (backgroundTactileFuture_.valid() || isRecordingActive() || episodeManager_ == nullptr)
    {
        return;
    }

    std::string episodeDir;
    {
        std::lock_guard<std::mutex> lock(backgroundTactileMutex_);
        episodeDir.swap(pendingBackgroundTactileEpisodeDir_);
    }
    if (episodeDir.empty())
    {
        return;
    }

    stopBackgroundTactileValidation_.store(false);
    logPerf((::DA::utils::LogString()
             << "[PERF] tactile background validation start: episode_dir=" << episodeDir).str());
    EpisodeManager *manager = episodeManager_.get();
    std::atomic<bool> *stopFlag = &stopBackgroundTactileValidation_;
    backgroundTactileFuture_ = std::async(
        std::launch::async,
        [manager, episodeDir, stopFlag]() {
            BackgroundTactileValidationResult result;
            result.episodeDir = episodeDir;
            if (stopFlag->load())
            {
                logPerf((::DA::utils::LogString()
                         << "[PERF] tactile background validation skipped: episode_dir="
                         << episodeDir
                         << " reason=stop_requested").str());
                return result;
            }
            manager->validateTactileEpisode(episodeDir, &result.findings);
            if (stopFlag->load())
            {
                result.findings.clear();
                logPerf((::DA::utils::LogString()
                         << "[PERF] tactile background validation discarded: episode_dir="
                         << episodeDir
                         << " reason=stop_requested").str());
            }
            return result;
        });
}

void RecordRuntime::requestStopBackgroundTactileValidation()
{
    stopBackgroundTactileValidation_.store(true);
    {
        std::lock_guard<std::mutex> lock(backgroundTactileMutex_);
        pendingBackgroundTactileEpisodeDir_.clear();
    }
    if (backgroundTactileFuture_.valid())
    {
        logPerf("[PERF] tactile background validation wait for stop");
        backgroundTactileFuture_.wait();
        backgroundTactileFuture_.get();
    }
}

void RecordRuntime::cancelBackgroundTactileValidation()
{
    stopBackgroundTactileValidation_.store(true);
    {
        std::lock_guard<std::mutex> lock(backgroundTactileMutex_);
        pendingBackgroundTactileEpisodeDir_.clear();
    }
    if (backgroundTactileFuture_.valid())
    {
        logPerf("[PERF] tactile background validation cancel requested");
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

bool RecordRuntime::writeRecordingLock(const std::string &episodeDir)
{
    json lock = {
        {"pid", static_cast<int>(getpid())},
        {"episode_dir", episodeDir},
        {"started_at_ms", currentEpochMs()},
    };
    std::string errorMessage;
    if (!writeTextFileAtomically(options_.recordingLockFile, lock.dump(2) + "\n", &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to write recording lock "
                                                   << options_.recordingLockFile << ": "
                                                   << errorMessage << std::endl).str());
        return false;
    }
    return true;
}

void RecordRuntime::removeRecordingLock()
{
    std::error_code error;
    fs::remove(options_.recordingLockFile, error);
    if (error)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to remove recording lock "
                                                  << options_.recordingLockFile << ": "
                                                  << error.message() << std::endl).str());
    }
}

bool RecordRuntime::startRecording(bool resetRecording)
{
    if (recordingOrchestrator_ == nullptr)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "recording orchestrator not initialized" << std::endl).str());
        return false;
    }
    cancelBackgroundTactileValidation();
    tactileTriggeredAudioCommand_.clear();
    activeHardwareFault_.reset();
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
    const std::string completedEpisodeDir = lastEpisodeDir();
    if (ok && !dueToError && !completedEpisodeDir.empty())
    {
        scheduleBackgroundTactileValidation(completedEpisodeDir);
    }
    if (ok && dueToError && activeHardwareFault_.has_value())
    {
        setHardwareFaultLedState(*activeHardwareFault_);
    }
    if (ok && !dueToError)
    {
        activeHardwareFault_.reset();
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

    const auto previousHealthState = healthState_;
    const auto result = healthMonitor_->Poll(healthState_);
    healthState_ = result.state;
    if (!result.checked)
    {
        return;
    }

    if (result.fault.has_value())
    {
        activeHardwareFault_ = *result.fault;
        if (isRecordingActive())
        {
            const std::string stopReason =
                "recording hardware fault (" + result.fault->key + "): " + result.fault->detail;
            if (result.should_notify_fault)
            {
                DM_LOG_ERROR("{}", (::DA::utils::LogString() << stopReason << std::endl).str());
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                    << "[HMI_DIAG] category=health_fault"
                    << " key=" << result.fault->key
                    << " led_state=" << ledStateName(toLedState(result.fault->led_state))
                    << " action=stop_recording").str());
            }
            stopRecording(true, stopReason);
            return;
        }
        if (result.should_notify_fault)
        {
            DM_LOG_ERROR("{}", (::DA::utils::LogString() << "hardware health fault (" << result.fault->key << "): "
                                  << result.fault->detail << std::endl).str());
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "[HMI_DIAG] category=health_fault"
                << " key=" << result.fault->key
                << " led_state=" << ledStateName(toLedState(result.fault->led_state))).str());
            setHardwareFaultLedState(*result.fault);
            sendAudioCommand("error");
        }
        return;
    }

    if (result.recovered)
    {
        activeHardwareFault_.reset();
        DM_LOG_INFO("{}", (::DA::utils::LogString()
                           << "hardware health recovered"
                           << (previousHealthState.last_error_key.empty()
                                   ? std::string()
                                   : " from " + previousHealthState.last_error_key)
                           << std::endl)
                              .str());
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

void RecordRuntime::setHardwareFaultLedState(const ugripper::runtime::HealthFault &fault)
{
    if (fault.led_state != ugripper::runtime::RuntimeLedState::Error2 || !ledController_)
    {
        setLedState(toLedState(fault.led_state));
        return;
    }

    const GripperLedEffect missingEffect{GripperLedEffectState::Error2, 0.0};
    const GripperLedEffect unknownEffect{GripperLedEffectState::Error2Unknown, 0.0};
    switch (fault.side)
    {
    case ugripper::runtime::HardwareFaultSide::Left:
        panelManager_.setLedEffectForSide("left", missingEffect);
        panelManager_.setLedColorForSide("right", 255, 0, 0);
        break;
    case ugripper::runtime::HardwareFaultSide::Right:
        panelManager_.setLedColorForSide("left", 255, 0, 0);
        panelManager_.setLedEffectForSide("right", missingEffect);
        break;
    case ugripper::runtime::HardwareFaultSide::Both:
        panelManager_.setLedEffect(missingEffect);
        break;
    case ugripper::runtime::HardwareFaultSide::Unknown:
    default:
        panelManager_.setLedEffect(unknownEffect);
        break;
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
    if (index < perSideLedStates_.size())
    {
        const auto &sideLed = perSideLedStates_[index];
        if (sideLed.hasColor)
        {
            drivers_[index]->setLedColor(sideLed.color);
        }
        else if (sideLed.hasEffect)
        {
            drivers_[index]->setLedEffect(sideLed.effect);
        }
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

    const bool serialOk = driver->readSerialNumber(serialNumber);
    if (serialOk)
    {
        if (hasSerialNumber != nullptr)
        {
            *hasSerialNumber = true;
        }
    }
    else
    {
        if (hasSerialNumber != nullptr)
        {
            *hasSerialNumber = false;
        }
        if (serialNumber != nullptr)
        {
            serialNumber->clear();
        }
    }

    if (!serialOk)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = driver->getLastCommandError();
        }
        return false;
    }

    if (errorMessage != nullptr)
    {
        errorMessage->clear();
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
    perSideLedStates_.clear();
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
    perSideLedStates_.clear();
    for (auto &driver : drivers_)
    {
        if (driver != nullptr)
        {
            driver->setLedEffect(effect);
        }
    }
}

void RecordRuntime::GripperPanelManager::setLedColorForSide(const std::string &side, uint8_t red, uint8_t green, uint8_t blue)
{
    const int index = findDriverIndexForSide(side);
    if (index < 0)
    {
        return;
    }
    if (perSideLedStates_.size() < drivers_.size())
    {
        perSideLedStates_.resize(drivers_.size());
    }
    auto &sideLed = perSideLedStates_[static_cast<size_t>(index)];
    sideLed.color = GripperLedColor{red, green, blue};
    sideLed.hasColor = true;
    sideLed.hasEffect = false;
    if (drivers_[static_cast<size_t>(index)] != nullptr)
    {
        drivers_[static_cast<size_t>(index)]->setLedColor(red, green, blue);
    }
}

void RecordRuntime::GripperPanelManager::setLedEffectForSide(const std::string &side, const GripperLedEffect &effect)
{
    const int index = findDriverIndexForSide(side);
    if (index < 0)
    {
        return;
    }
    if (perSideLedStates_.size() < drivers_.size())
    {
        perSideLedStates_.resize(drivers_.size());
    }
    auto &sideLed = perSideLedStates_[static_cast<size_t>(index)];
    sideLed.effect = effect;
    sideLed.hasEffect = true;
    sideLed.hasColor = false;
    if (drivers_[static_cast<size_t>(index)] != nullptr)
    {
        drivers_[static_cast<size_t>(index)]->setLedEffect(effect);
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
                                              std::string hardwareVersion,
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
      hardwareVersion_(std::move(hardwareVersion)),
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

void RecordRuntime::EpisodeManager::setMainCameraRuntimeStates(
    const std::array<MainCameraRuntimeState, 3> &states)
{
    mainCameraRuntimeStates_ = states;
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
        const std::string normalizedName = stripEpisodeTempSuffix(name);
        if (normalizedName.rfind(prefix, 0) != 0)
        {
            continue;
        }
        const std::string suffix = normalizedName.substr(prefix.size());
        if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
        {
            maxId = std::max(maxId, std::stoi(suffix));
        }
    }

    const int nextId = maxId + 1;
    char idBuffer[16] = {0};
    std::snprintf(idBuffer, sizeof(idBuffer), "%04d", nextId);
    const fs::path episodePath = fs::path(episodeRoot_) / (prefix + idBuffer + "-temp");
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
                cameraHistory["persistent_recent"] = json::array();
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
    logPerf((::DA::utils::LogString() << "[PERF] validateEpisode begin: episode_dir=" << episodeDir).str());
    const auto artifacts = activeEpisodeVideoArtifacts(chestCameraEnabled_);
    const std::vector<std::string> requiredFiles = {
        "sensor_left.mcap",
        "sensor_right.mcap",
        "calibration.json",
        "metadata.json",
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

    for (const auto &state : mainCameraRuntimeStates_)
    {
        if (!state.enabled)
        {
            continue;
        }
        if (!state.present)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "main camera calibration cache invalid: camera=" + state.cameraName +
                                " device=" + state.devicePath + " missing";
            }
            return false;
        }
        if (!isYuzhouMainCameraSn(state.serialNumber))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "main camera SN cache invalid: camera=" + state.cameraName +
                                " device=" + state.devicePath +
                                " detail=" + (state.lastError.empty() ? "empty SN" : state.lastError);
            }
            return false;
        }
        std::string calibrationDetail;
        if (!state.calibrationPayloadCached ||
            !validateYuzhouMainCameraCalibrationPayload(state.calibrationPayload, &calibrationDetail))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "main camera calibration cache invalid: camera=" + state.cameraName +
                                " device=" + state.devicePath +
                                " detail=" + (!calibrationDetail.empty()
                                                   ? calibrationDetail
                                                   : (state.lastError.empty() ? "empty calibration" : state.lastError));
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

    std::string metadataError;
    if (!validateMetadataVideoDetails(fs::path(episodeDir) / "metadata.json",
                                      artifacts,
                                      &metadataError))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "metadata validation failed: " + metadataError;
        }
        return false;
    }

    logPerf((::DA::utils::LogString() << "[PERF] validation setup done: episode_dir=" << episodeDir
             << " elapsed_ms=" << (steadyNowMs() - validateStartMs)).str());

    double referenceSpanSec = 0.0;
    const int64_t videoProbeStartMs = steadyNowMs();
    const auto probeTasks = probeVideoFilesParallel(episodeDir, artifacts);
    std::vector<VideoProbeResult> probes;
    probes.reserve(probeTasks.size());
    for (const auto &task : probeTasks)
    {
        if (!task.ok)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "video unreadable or missing timing metadata: " + task.probe.fileName +
                                " (" + task.errorMessage + ")";
            }
            return false;
        }

        if (task.probe.spanSec < kMinReasonableVideoSpanSec)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "video span too short: " + task.probe.fileName +
                                " span=" + formatSeconds(task.probe.spanSec) +
                                "s, expected >=" + formatSeconds(kMinReasonableVideoSpanSec) + "s";
            }
            return false;
        }

        referenceSpanSec = std::max(referenceSpanSec, task.probe.spanSec);
        probes.push_back(task.probe);
    }
    logPerf((::DA::utils::LogString() << "[PERF] parallel video probe done: episode_dir=" << episodeDir
                  << " count=" << probes.size()
                  << " elapsed_ms=" << (steadyNowMs() - videoProbeStartMs)).str());

    std::string probeCacheError;
    if (!writeVideoProbeCacheToTimingFile(episodeDir, probes, &probeCacheError))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to cache video probe results in shm timing file: "
                         << probeCacheError << std::endl).str());
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

    const int64_t tailCheckStartMs = steadyNowMs();
    std::vector<std::future<TailCheckTaskResult>> tailCheckFutures;
    tailCheckFutures.reserve(kEncoderTailCheckTargets.size() + kFaysTailCheckTargets.size());

    for (const auto &target : kEncoderTailCheckTargets)
    {
        tailCheckFutures.push_back(std::async(
            std::launch::async,
            [episodeDir, target]() {
                TailCheckTaskResult result;
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
                    result.errorMessage = std::string("encoder tail check failed for side=") + target.side +
                                          ": " + encoderTailError;
                    return result;
                }

                result.ok = true;
                result.detail = (::DA::utils::LogString()
                                 << "encoder_" << target.side
                                 << "{count=" << encoderMessageCount
                                 << ", last_ns=" << lastEncoderLogTimeNs
                                 << "}").str();
                return result;
            }));
    }

    for (const auto &target : kFaysTailCheckTargets)
    {
        tailCheckFutures.push_back(std::async(
            std::launch::async,
            [episodeDir, target]() {
                TailCheckTaskResult result;
                FaysMcapSummary faysSummary;
                std::string faysTailError;
                if (!loadFaysMcapSummary(
                        episodeDir + "/" + target.mcapFileName,
                        target.imuTopic,
                        target.cameraTopic,
                        &faysSummary,
                        &faysTailError))
                {
                    result.errorMessage = std::string("Fays MCAP tail check failed for side=") + target.side +
                                          ": " + faysTailError;
                    return result;
                }

                int64_t lagNs = static_cast<int64_t>(faysSummary.lastCameraLogTimeNs) -
                                static_cast<int64_t>(faysSummary.lastImuLogTimeNs);
                if (lagNs < 0)
                {
                    lagNs = 0;
                }
                if (lagNs > kFaysImuTailMaxLagNs)
                {
                    result.errorMessage = std::string("Fays IMU tail lag too large for side=") + target.side +
                                          " (lag_ns=" + std::to_string(lagNs) +
                                          ", threshold_ns=" + std::to_string(kFaysImuTailMaxLagNs) +
                                          ", last_camera_ns=" + std::to_string(faysSummary.lastCameraLogTimeNs) +
                                          ", last_imu_ns=" + std::to_string(faysSummary.lastImuLogTimeNs) + ")";
                    return result;
                }

                result.ok = true;
                result.detail = (::DA::utils::LogString()
                                 << "fays_" << target.side
                                 << "{imu_count=" << faysSummary.imuMessageCount
                                 << ", camera_count=" << faysSummary.cameraMessageCount
                                 << ", span_ms=" << (faysSummary.spanNs / 1000000.0)
                                 << ", last_camera_ns=" << faysSummary.lastCameraLogTimeNs
                                 << ", last_imu_ns=" << faysSummary.lastImuLogTimeNs
                                 << ", lag_ms=" << (lagNs / 1000000.0)
                                 << "}").str();
                return result;
            }));
    }

    std::vector<std::string> tailDetails;
    tailDetails.reserve(tailCheckFutures.size());
    for (auto &future : tailCheckFutures)
    {
        TailCheckTaskResult result = future.get();
        if (!result.ok)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = result.errorMessage.empty() ? "tail check failed" : result.errorMessage;
            }
            return false;
        }
        tailDetails.push_back(result.detail);
    }
    logPerf((::DA::utils::LogString() << "[PERF] tail checks done: episode_dir=" << episodeDir
             << " count=" << tailDetails.size()
             << " elapsed_ms=" << (steadyNowMs() - tailCheckStartMs)
             << " details=" << joinStrings(tailDetails, "; ")).str());

    logPerf((::DA::utils::LogString() << "[PERF] validateEpisode end: episode_dir=" << episodeDir
              << " elapsed_ms=" << (steadyNowMs() - validateStartMs)
              << " reference_span_sec=" << formatSeconds(referenceSpanSec)).str());
    return true;
}

void RecordRuntime::EpisodeManager::validateTactileEpisode(
    const std::string &episodeDir,
    std::vector<TactileValidationFinding> *tactileFindings) const
{
    const int64_t tactileValidationStartMs = steadyNowMs();
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
        const char *fileName = episodeVideoFileNameForCamera(target.cameraName);
        if (fileName == nullptr || *fileName == '\0')
        {
            continue;
        }

        const auto currentFrame = captureTactileGrayFrame(
            {"-i", episodeDir + "/" + std::string(fileName)},
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
        if (!cameraHistory.contains("persistent_recent") || !cameraHistory["persistent_recent"].is_array())
        {
            cameraHistory["persistent_recent"] = json::array();
        }

        const bool persistentWarningActiveBefore =
            cameraHistory.value("persistent_fault_active", false);
        bool persistentWarningActiveAfter = persistentWarningActiveBefore;
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
                                &persistentBaselineError))
        {
            const TactileFrameMetrics persistentMetrics =
                computeTactileFrameMetrics(persistentBaseline, *currentFrame);
            json &persistentRecent = cameraHistory["persistent_recent"];
            persistentRecent.push_back(persistentMetrics.damaged);
            while (persistentRecent.size() > kTactileHistoryWindow)
            {
                persistentRecent.erase(persistentRecent.begin());
            }

            bool persistentWindowActive = false;
            bool persistentWindowClean = false;
            if (persistentRecent.size() >= kTactileHistoryWindow)
            {
                persistentWindowActive = std::all_of(
                    persistentRecent.begin(),
                    persistentRecent.end(),
                    [](const json &entry) { return entry.is_boolean() && entry.get<bool>(); });
                persistentWindowClean = std::all_of(
                    persistentRecent.begin(),
                    persistentRecent.end(),
                    [](const json &entry) { return entry.is_boolean() && !entry.get<bool>(); });
            }

            if (persistentWindowActive)
            {
                persistentDetail = "persistent_record " + formatTactileMetrics(persistentMetrics);
                if (!persistentWarningActiveBefore)
                {
                    persistentWarningTriggered = true;
                }
                persistentWarningActiveAfter = true;
            }
            else if (persistentWindowClean)
            {
                persistentWarningActiveAfter = false;
                persistentDetail.clear();
            }

            cameraHistory["persistent_fault_active"] = persistentWarningActiveAfter;
            cameraHistory["persistent_fault_detail"] = persistentDetail;
            historyDirty = true;
        }

        TactileValidationFinding finding;
        finding.cameraName = target.cameraName;
        finding.serialNumber = *serial;
        finding.damaged = metrics.damaged;
        finding.warningActive = warningActive || persistentWarningActiveAfter;
        finding.warningTriggered =
            (warningActive && !warningActiveBefore) || persistentWarningTriggered;
        finding.audioCommand = std::string(target.cameraName) + "_damaged";
        finding.detail = formatTactileMetrics(metrics);
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
    logPerf((::DA::utils::LogString() << "[PERF] tactile validation done: episode_dir=" << episodeDir
             << " elapsed_ms=" << (steadyNowMs() - tactileValidationStartMs)).str());
}

const std::string &RecordRuntime::EpisodeManager::dataRoot() const
{
    return episodeRoot_;
}

namespace
{
std::string toUpperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string addVersionPrefix(const std::string &version)
{
    if (version.empty() || version == "unknown" || version.front() == 'v' || version.front() == 'V')
    {
        return version;
    }
    return "v" + version;
}

double roundToOneDecimal(double value)
{
    if (!std::isfinite(value))
    {
        return 0.0;
    }
    return std::round(value * 10.0) / 10.0;
}

std::string lowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string generateUuid()
{
    std::ifstream input("/proc/sys/kernel/random/uuid");
    std::string uuid;
    if (input.is_open() && std::getline(input, uuid))
    {
        uuid = trim(uuid);
        if (!uuid.empty())
        {
            return uuid;
        }
    }

    uuid_t fallbackUuid;
    char uuidString[37] = {0};
    uuid_generate(fallbackUuid);
    uuid_unparse(fallbackUuid, uuidString);
    return std::string(uuidString);
}

std::string classifyQualityError(const std::string &message)
{
    const std::string lowered = lowerCopy(message);
    if (lowered.find("recording hardware fault") != std::string::npos ||
        lowered.find("disconnected") != std::string::npos ||
        lowered.find("disconnect") != std::string::npos ||
        lowered.find("disappeared") != std::string::npos ||
        lowered.find("device node") != std::string::npos)
    {
        return "device_disconnected";
    }
    if (lowered.find("finalize") != std::string::npos ||
        lowered.find("stereo session") != std::string::npos ||
        lowered.find("fays recorder unhealthy") != std::string::npos ||
        lowered.find("fays devices missing") != std::string::npos ||
        lowered.find("duplicate fays serial") != std::string::npos ||
        lowered.find("failed to stop") != std::string::npos ||
        lowered.find("failed to start") != std::string::npos)
    {
        return "finalize_error";
    }
    if (lowered.find("main camera") != std::string::npos ||
        lowered.find("calibration") != std::string::npos ||
        lowered.find("mcal") != std::string::npos ||
        lowered.find("yuzhou") != std::string::npos ||
        lowered.find(" xu ") != std::string::npos ||
        lowered.find(" sn ") != std::string::npos)
    {
        return "calibration_error";
    }
    if (lowered.find("missing") != std::string::npos ||
        lowered.find("cannot open") != std::string::npos)
    {
        return "missing_file";
    }
    if (lowered.find("span too short") != std::string::npos ||
        lowered.find("duration too short") != std::string::npos)
    {
        return "collection_duration_too_short";
    }
    if (lowered.find("gap") != std::string::npos ||
        lowered.find("tail") != std::string::npos ||
        lowered.find("lag too large") != std::string::npos ||
        lowered.find("no samples near episode end") != std::string::npos)
    {
        return "frame_loss";
    }
    return "unknown";
}

double nominalFpsForCamera(const std::string &cameraName)
{
    if (cameraName.find("tcam") != std::string::npos)
    {
        return 120.0;
    }
    if (cameraName.find("stereo") != std::string::npos)
    {
        return 25.0;
    }
    return 60.0;
}

std::vector<EpisodeVideoArtifact> metadataVideoArtifacts(bool chestCameraEnabled)
{
    std::vector<EpisodeVideoArtifact> artifacts = {
        {"left_cam_main", "cam_left.mkv"},
        {"left_tcam_l", "tcam_left_l.mkv"},
        {"left_tcam_r", "tcam_left_r.mkv"},
        {"left_stereo", "stereo_left.mkv"},
        {"right_cam_main", "cam_right.mkv"},
        {"right_tcam_l", "tcam_right_l.mkv"},
        {"right_tcam_r", "tcam_right_r.mkv"},
        {"right_stereo", "stereo_right.mkv"},
    };
    if (chestCameraEnabled)
    {
        artifacts.push_back({"chest_cam_main", "cam_chest.mkv"});
    }
    return artifacts;
}

std::string jsonStringPath(const json &root, const std::string &path)
{
    const json *value = findJsonPathConst(&root, path);
    if (value != nullptr && value->is_string())
    {
        return value->get<std::string>();
    }
    return "";
}
}

bool RecordRuntime::EpisodeManager::writeFinalMetadata(const std::string &episodeDir,
                                                       bool qualityOk,
                                                       const std::string &qualityErrorMessage,
                                                       std::string *errorMessage) const
{
    const fs::path episodePath(episodeDir);
    json existingMetadata = json::object();
    std::string ignoredError;
    loadJsonFile((episodePath / "metadata.json").string(), &existingMetadata, &ignoredError);
    if (!existingMetadata.is_object())
    {
        existingMetadata = json::object();
    }

    ordered_json hardwareList = ordered_json::object();
    hardwareList["gripper_right_sn"] = gripperRuntimeStates_[gripperStateIndexForSide("right")].serialNumber;
    hardwareList["gripper_left_sn"] = gripperRuntimeStates_[gripperStateIndexForSide("left")].serialNumber;

    const auto probeSerial = [](const std::string &devicePath) {
        std::string detail;
        const auto serial = probeUsbSerialForDeviceNode(devicePath, &detail);
        return serial.value_or(std::string());
    };
    const auto cachedMainCameraSn = [this](const std::string &cameraName) {
        const auto it = std::find_if(
            mainCameraRuntimeStates_.begin(),
            mainCameraRuntimeStates_.end(),
            [&cameraName](const MainCameraRuntimeState &state) {
                return state.cameraName == cameraName;
            });
        return it != mainCameraRuntimeStates_.end() ? it->serialNumber : std::string();
    };
    hardwareList["cam_right_sn"] = cachedMainCameraSn("right_cam_main");
    hardwareList["cam_left_sn"] = cachedMainCameraSn("left_cam_main");
    hardwareList["cam_chest_sn"] = chestCameraEnabled_ ? cachedMainCameraSn("chest_cam_main") : "";
    hardwareList["tactile_right_l_sn"] = probeSerial("/dev/tcam_right_l");
    hardwareList["tactile_right_r_sn"] = probeSerial("/dev/tcam_right_r");
    hardwareList["tactile_left_l_sn"] = probeSerial("/dev/tcam_left_l");
    hardwareList["tactile_left_r_sn"] = probeSerial("/dev/tcam_left_r");

    json stereoStatus = json::object();
    loadJsonFile(stereoStatusFile_, &stereoStatus, &ignoredError);
    hardwareList["stereo_right_sn"] = jsonStringPath(stereoStatus, "cameras.right_stereo.serial_number");
    hardwareList["stereo_left_sn"] = jsonStringPath(stereoStatus, "cameras.left_stereo.serial_number");

    json infoRoot = json::object();
    loadJsonFile(episodeTimingPath(episodePath).string(), &infoRoot, &ignoredError);
    std::map<std::string, int64_t> offsetByCamera;
    int64_t minOffsetUs = std::numeric_limits<int64_t>::max();
    for (const auto &artifact : metadataVideoArtifacts(chestCameraEnabled_))
    {
        const std::string key = std::string(artifact.cameraName) + "_record_time_offset_us";
        if (infoRoot.contains(key) && infoRoot[key].is_number_integer())
        {
            const int64_t offsetUs = infoRoot[key].get<int64_t>();
            if (offsetUs > 0)
            {
                offsetByCamera[artifact.cameraName] = offsetUs;
                minOffsetUs = std::min(minOffsetUs, offsetUs);
            }
        }
    }
    if (minOffsetUs == std::numeric_limits<int64_t>::max())
    {
        minOffsetUs = 0;
    }

    if (qualityOk)
    {
        const auto metadataArtifacts = metadataVideoArtifacts(chestCameraEnabled_);
        if (!infoRoot.is_object())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "internal shm timing file top-level value must be an object";
            }
            return false;
        }
        if (offsetByCamera.size() != metadataArtifacts.size())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "internal shm timing file missing video offset cache";
            }
            return false;
        }
    }

    ordered_json videoDetails = ordered_json::array();
    double collectionDurationS = 0.0;
    for (const auto &artifact : metadataVideoArtifacts(chestCameraEnabled_))
    {
        const fs::path videoPath = episodePath / artifact.fileName;
        if (!RecordRuntime::fileExistsAndNotEmpty(videoPath.string()))
        {
            continue;
        }

        VideoProbeResult probe;
        if (!loadCachedVideoProbe(infoRoot, artifact, &probe))
        {
            probe.cameraName = artifact.cameraName;
            probe.fileName = artifact.fileName;
            std::string probeError;
            if (!probeVideoFile(videoPath.string(), &probe, &probeError))
            {
                continue;
            }
        }

        const auto offsetIt = offsetByCamera.find(artifact.cameraName);
        const int64_t startOffsetUs =
            offsetIt != offsetByCamera.end() && minOffsetUs > 0
                ? std::max<int64_t>(0, offsetIt->second - minOffsetUs)
                : 0;

        ordered_json detail = ordered_json::object();
        detail["name"] = artifact.fileName;
        detail["fps"] = roundToOneDecimal(nominalFpsForCamera(artifact.cameraName));
        detail["duration_s"] = roundToOneDecimal(probe.durationSec);
        detail["start_offset_us"] = startOffsetUs;
        videoDetails.push_back(std::move(detail));
        collectionDurationS = std::max(collectionDurationS, probe.durationSec);
    }

    ordered_json requiredFiles = ordered_json::array({
        "metadata.json",
        "calibration.json",
        "cam_left.mkv",
        "cam_right.mkv",
        "stereo_left.mkv",
        "stereo_right.mkv",
        "tcam_left_l.mkv",
        "tcam_left_r.mkv",
        "tcam_right_l.mkv",
        "tcam_right_r.mkv",
        "sensor_left.mcap",
        "sensor_right.mcap",
        "fays_data_left.mcap",
        "fays_data_right.mcap",
    });
    if (chestCameraEnabled_)
    {
        requiredFiles.push_back("cam_chest.mkv");
    }

    const bool hasAudio =
        RecordRuntime::fileExistsAndNotEmpty((episodePath / "audio_pre.wav").string()) ||
        RecordRuntime::fileExistsAndNotEmpty((episodePath / "audio_post.wav").string());

    ordered_json metadata = ordered_json::object();
    metadata["device_sn"] = toUpperAscii(deviceSn_);
    metadata["device_type"] = "ugripper";
    metadata["device_mode"] = "dual";
    metadata["camera_codec"] = cameraCodec_;
    metadata["hardware_version"] = hardwareVersion_;
    metadata["software_version"] = addVersionPrefix(packageVersion_);
    metadata["das_usb_updater_version"] = updaterVersion_;
    metadata["data_version"] = "3.0";
    metadata["hardware_list"] = std::move(hardwareList);
    metadata["episode_name"] = stripEpisodeTempSuffix(episodePath.filename().string());
    metadata["data_uuid"] = existingMetadata.value("data_uuid", generateUuid());
    metadata["audio_uuid"] = hasAudio ? existingMetadata.value("audio_uuid", generateUuid()) : "";
    metadata["quality_check_status"] = qualityOk ? "success" : "fail";
    metadata["quality_check_err_type"] = qualityOk ? "" : classifyQualityError(qualityErrorMessage);
    metadata["collection_duration_s"] = roundToOneDecimal(collectionDurationS);
    metadata["require_files"] = std::move(requiredFiles);
    metadata["video_details"] = std::move(videoDetails);

    if (!writeTextFileAtomically(episodePath / "metadata.json", metadata.dump(2) + "\n", errorMessage))
    {
        return false;
    }
    return true;
}

std::string RecordRuntime::EpisodeManager::finalizeEpisodeDir(const std::string &episodeDir,
                                                              std::string *errorMessage) const
{
    const fs::path sourcePath(episodeDir);
    const std::string sourceName = sourcePath.filename().string();
    const std::string finalName = stripEpisodeTempSuffix(sourceName);
    if (finalName == sourceName)
    {
        return episodeDir;
    }

    const fs::path targetPath = sourcePath.parent_path() / finalName;
    std::error_code error;
    if (fs::exists(targetPath, error))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "final episode directory already exists: " + targetPath.string();
        }
        return {};
    }

    fs::rename(sourcePath, targetPath, error);
    if (error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "rename episode directory failed: " + error.message();
        }
        return {};
    }

    std::error_code removeError;
    fs::remove(episodeTimingPath(sourcePath), removeError);
    if (removeError)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to remove internal timing file: "
                      << episodeTimingPath(sourcePath)
                      << " error=" << removeError.message() << std::endl).str());
    }
    flushEpisodeDirectoriesToDisk(targetPath, "episode_finalize");
    return targetPath.string();
}

bool RecordRuntime::EpisodeManager::writeFilteredCalibration(const std::string &episodeDir, std::string *errorMessage) const
{
    ordered_json mainImages = ordered_json::object();
    ordered_json tactileImages = ordered_json::object();
    ordered_json images = ordered_json::object();
    ordered_json imu = ordered_json::object();
    images["stereo_left"] = makeDefaultStereoCalibrationEntry();
    images["stereo_right"] = makeDefaultStereoCalibrationEntry();
    imu["imu_left"] = makeDefaultFaysImuCalibrationEntry();
    imu["imu_right"] = makeDefaultFaysImuCalibrationEntry();

    bool allMainCameraCalibrationValid = true;
    for (const auto &state : mainCameraRuntimeStates_)
    {
        if (!state.enabled)
        {
            continue;
        }
        std::string jsonPath;
        if (state.cameraName == "left_cam_main")
        {
            jsonPath = "cam_left_main";
        }
        else if (state.cameraName == "right_cam_main")
        {
            jsonPath = "cam_right_main";
        }
        else if (state.cameraName == "chest_cam_main")
        {
            jsonPath = "cam_chest_main";
        }
        if (jsonPath.empty())
        {
            continue;
        }

        mainImages[jsonPath] = makeDefaultMainCameraCalibrationEntry();
        std::string calibrationDetail;
        if (state.present &&
            isYuzhouMainCameraSn(state.serialNumber) &&
            state.calibrationPayloadCached &&
            validateYuzhouMainCameraCalibrationPayload(state.calibrationPayload, &calibrationDetail))
        {
            mainImages[jsonPath] = makeMainCameraCalibrationEntryFromPayload(state.calibrationPayload);
        }
        else
        {
            allMainCameraCalibrationValid = false;
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "main camera calibration cache unavailable for episode calibration: camera="
                << state.cameraName
                << " device=" << state.devicePath
                << " detail=" << (state.lastError.empty() ? calibrationDetail : state.lastError)
                << std::endl).str());
        }
    }

    bool allFaysCalibrationValid = true;
    for (const auto &entry : std::vector<std::pair<std::string, std::string>>{
             {"left", "left_stereo"},
             {"right", "right_stereo"},
         })
    {
        json faysCalibration;
        std::string faysStatus;
        if (!loadFaysCalibrationFromStatus(stereoStatusFile_, entry.second, &faysCalibration, &faysStatus))
        {
            allFaysCalibrationValid = false;
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "Fays calibration unavailable for "
                                 << entry.second << ": " << faysStatus << std::endl).str());
            continue;
        }

        images[entry.first == "left" ? "stereo_left" : "stereo_right"] =
            makeFaysStereoCalibrationEntry(faysCalibration);
        imu[entry.first == "left" ? "imu_left" : "imu_right"] =
            makeFaysImuCalibrationEntry(faysCalibration);
        DM_LOG_INFO("{}", (::DA::utils::LogString() << "Fays calibration loaded for "
                            << entry.second << ": " << faysStatus << std::endl).str());
    }

    for (const auto &target : kTactileCalibrationTargets)
    {
        std::string serial;
        std::string detail;
        const auto runtimeSerial = probeUsbSerialForDeviceNode(target.devicePath, &detail);
        if (runtimeSerial.has_value())
        {
            serial = *runtimeSerial;
        }
        else
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "failed to resolve runtime tactile serial for camera=" << target.cameraName
                << " device=" << target.devicePath
                << " detail=" << detail
                << "; write empty serial" << std::endl).str());
        }
        std::string imageKey = target.cameraName;
        if (imageKey.rfind("left_tcam_", 0) == 0)
        {
            imageKey = "tcam_left_" + imageKey.substr(std::string("left_tcam_").size());
        }
        else if (imageKey.rfind("right_tcam_", 0) == 0)
        {
            imageKey = "tcam_right_" + imageKey.substr(std::string("right_tcam_").size());
        }
        tactileImages[imageKey] = makeRuntimeTactileCalibrationEntry(serial);
    }

    ordered_json orderedImages = ordered_json::object();
    if (mainImages.contains("cam_chest_main"))
    {
        orderedImages["cam_chest_main"] = mainImages["cam_chest_main"];
    }
    orderedImages["stereo_left"] = images["stereo_left"];
    orderedImages["tcam_left_l"] = tactileImages.value("tcam_left_l", makeRuntimeTactileCalibrationEntry(""));
    orderedImages["tcam_left_r"] = tactileImages.value("tcam_left_r", makeRuntimeTactileCalibrationEntry(""));
    orderedImages["stereo_right"] = images["stereo_right"];
    orderedImages["tcam_right_l"] = tactileImages.value("tcam_right_l", makeRuntimeTactileCalibrationEntry(""));
    orderedImages["tcam_right_r"] = tactileImages.value("tcam_right_r", makeRuntimeTactileCalibrationEntry(""));
    orderedImages["cam_left_main"] = mainImages.value("cam_left_main", makeDefaultMainCameraCalibrationEntry());
    orderedImages["cam_right_main"] = mainImages.value("cam_right_main", makeDefaultMainCameraCalibrationEntry());

    ordered_json calibrationJson = ordered_json::object();
    const std::string calibrationStatus = (allMainCameraCalibrationValid && allFaysCalibrationValid)
                                             ? "calibrated"
                                             : "uncalibrated";
    calibrationJson["calibration_info"] = makeLockedCalibrationInfo(calibrationStatus);
    calibrationJson["observation"] = ordered_json::object();
    calibrationJson["observation"]["images"] = std::move(orderedImages);
    calibrationJson["observation"]["imu"] = std::move(imu);

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
