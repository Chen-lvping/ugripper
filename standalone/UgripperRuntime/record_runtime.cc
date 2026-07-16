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
#include <exception>
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
#include <sys/syscall.h>
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
int ChildFileDescriptorLimit()
{
    const long openMax = sysconf(_SC_OPEN_MAX);
    return openMax > 0 ? static_cast<int>(openMax) : 1024;
}

void CloseChildFileDescriptors(int fdLimit)
{
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0)
    {
        return;
    }
#endif
    for (int fd = 3; fd < fdLimit; ++fd)
    {
        close(fd);
    }
}

constexpr const char *kChestCameraEnvKey = "ENABLE_CHEST_CAM_MAIN";
#ifndef UGRIPPER_ENABLE_STEREO
#define UGRIPPER_ENABLE_STEREO 1
#endif
#if UGRIPPER_ENABLE_STEREO
constexpr bool kPackageStereoEnabled = true;
constexpr const char *kPackageStereoBuildMarker = "UGRIPPER_STEREO_BUILD=ON";
#else
constexpr bool kPackageStereoEnabled = false;
constexpr const char *kPackageStereoBuildMarker = "UGRIPPER_STEREO_BUILD=OFF";
#endif
constexpr uint64_t kActionDebounceMs = 80;
constexpr uint64_t kButtonPressDebounceMs = 40;
constexpr uint64_t kButtonReleaseDebounceMs = 40;
constexpr uint64_t kLongPressThresholdMs = 800;
constexpr uint64_t kDualLongPressThresholdMs = 4000;
constexpr uint64_t kShutdownPromptThresholdMs = 2000;
constexpr uint64_t kSystemActionResultWaitMs = 8000;
constexpr uint64_t kPostUmountAudioDelayMs = 2000;
constexpr uint64_t kAudioPlayerRestartIntervalMs = 2000;
constexpr uint64_t kAudioPlayerReadyGraceMs = 3000;
constexpr uint64_t kStereoDaemonRestartIntervalMs = 2000;
constexpr uint64_t kStereoStartupReadyTimeoutMs = 40000;
constexpr uint64_t kStereoFinalizeWaitPollMs = 100;
constexpr uint64_t kRecordControlDebounceMs = 300;
constexpr uint64_t kPhysicalRecordDoubleClickMs = 1000;
constexpr uint64_t kRestoreUsbSymlinkPollIntervalMs = 1000;
constexpr uint64_t kRestoreUsbSymlinkMaxWaitMs = 25000;
constexpr uint64_t kRestoreUsbReadyCheckDelayMs = 45000;
constexpr uint64_t kRestoreUsbFailureAlarmDurationMs = 5000;
constexpr int kBeepSilenceRetryLimit = 5;
constexpr int kBeepSilenceConfirmMs = 150;
constexpr uint64_t kRestoreUsbManualInsertGraceMs = 20000;
constexpr uint64_t kRestoreUsbTriggerStableMs = 6000;
constexpr uint64_t kRestoreUsbPreflightFailureCooldownMs = 30000;
constexpr int kOperatorMarkBeepOnMs = 120;
constexpr int kOperatorMarkBeepOffMs = 120;
constexpr int kOperatorMarkBeepSilenceRetryAttempts = 5;
constexpr int kOperatorMarkBeepSilenceRetryIntervalMs = 80;
constexpr uint8_t kOperatorMarkLedRed = 180;
constexpr uint8_t kOperatorMarkLedGreen = 0;
constexpr uint8_t kOperatorMarkLedBlue = 255;
constexpr int kRestoreUsbMaxAttemptsPerError = 3;
constexpr uint64_t kCameraRecorderDStateFaultMs = 5000;
constexpr uint64_t kCameraRecorderDStateLogIntervalMs = 3000;
constexpr uint64_t kEgoFinalizeWaitPollMs = 100;
constexpr uint64_t kEgoStartAckWaitMs = 5000;
constexpr uint64_t kEgoStartAckPollMs = 50;
constexpr uint64_t kHealthCheckIntervalMs = 1000;
constexpr uint64_t kHmiActiveTimeoutMs = 5500;
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
constexpr uint64_t kTactileSerialRefreshRetryMs = 3000;
constexpr int kTactileFrameWidth = 160;
constexpr int kTactileFrameHeight = 120;
constexpr size_t kTactileFrameBytes = static_cast<size_t>(kTactileFrameWidth * kTactileFrameHeight);
constexpr double kTactileEpisodeProbeSec = 0.12;
constexpr double kTactileRobustResidualAreaThreshold = 0.003;
constexpr double kTactileResidualFloorThreshold = 10.0;
constexpr double kTactileResidualMadMultiplier = 6.0;
constexpr double kTactileMadToSigma = 1.4826;
constexpr int kTactileResidualMinNeighborCount = 3;
constexpr size_t kTactileResidualMinComponentPixels = 8;
constexpr int kTactileResidualLowFrequencyBlurRadius = 2;
constexpr size_t kTactileResidualArtifactMaxComponentPixels = 25;
constexpr double kTactileResidualArtifactLowFrequencyMeanThreshold = 5.0;
constexpr size_t kTactileHistoryWindow = 3;
constexpr int kTactileSnapshotTimeoutMs = 2500;
constexpr const char *kEpisodeTimingFileName = ".recording_timing.json";
constexpr const char *kEpisodeTimingShmPrefix = "ugripper_recording_timing_";
constexpr const char *kCameraRecorderFaultShmPrefix = "ugripper_camera_recorder_fault_";
constexpr const char *kEgoSyncStatusFileName = "ego_sync.json";
constexpr const char *kMainCameraV4l2KernelHangKey = "main_camera_v4l2_kernel_hang";
constexpr const char *kMainCameraV4l2StartupFailureKey = "main_camera_v4l2_startup_failure";
constexpr const char *kMainCameraShortStreamKey = "main_camera_short_stream";

bool IsStereoStartupFaultKey(const std::string& key)
{
    return key == "stereo_daemon_not_running" ||
           key == "stereo_status_missing" ||
           key == "stereo_status_invalid" ||
           key == "stereo_not_ready" ||
           key == ugripper::runtime::kErrorTypeStereoControlFailed;
}

#ifndef UGRIPPER_ENABLE_PERF_LOG
#define UGRIPPER_ENABLE_PERF_LOG 0
#endif

constexpr bool kPerfLogEnabled = UGRIPPER_ENABLE_PERF_LOG != 0;
bool g_perfLogEnabled = kPerfLogEnabled;

bool perfLogEnabled()
{
    return g_perfLogEnabled;
}

std::string shellQuote(const std::string &value)
{
    std::string quoted = "'";
    for (const char ch : value)
    {
        if (ch == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
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

struct ProcTaskDState
{
    std::string taskId;
    std::string comm;
    std::string wchan;
};

bool ReadProcTaskState(const fs::path &statPath, char *state, std::string *comm)
{
    std::ifstream input(statPath);
    std::string line;
    if (!input.is_open() || !std::getline(input, line))
    {
        return false;
    }
    const size_t leftParen = line.find('(');
    const size_t rightParen = line.rfind(')');
    if (leftParen == std::string::npos ||
        rightParen == std::string::npos ||
        rightParen + 2 >= line.size())
    {
        return false;
    }
    if (state != nullptr)
    {
        *state = line[rightParen + 2];
    }
    if (comm != nullptr)
    {
        *comm = line.substr(leftParen + 1, rightParen - leftParen - 1);
    }
    return true;
}

std::string ReadSmallTextFileTrimmed(const fs::path &path)
{
    std::ifstream input(path);
    std::string text;
    if (!input.is_open() || !std::getline(input, text))
    {
        return std::string();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    return text;
}

std::optional<ProcTaskDState> FindFirstDStateTask(int pid)
{
    if (pid <= 0)
    {
        return std::nullopt;
    }
    const fs::path taskRoot = fs::path("/proc") / std::to_string(pid) / "task";
    std::error_code error;
    if (!fs::exists(taskRoot, error) || !fs::is_directory(taskRoot, error))
    {
        return std::nullopt;
    }

    for (const auto &entry : fs::directory_iterator(taskRoot, error))
    {
        if (error)
        {
            break;
        }
        if (!entry.is_directory(error))
        {
            continue;
        }
        const std::string taskId = entry.path().filename().string();
        char state = '\0';
        std::string comm;
        if (!ReadProcTaskState(entry.path() / "stat", &state, &comm))
        {
            continue;
        }
        if (state != 'D')
        {
            continue;
        }
        return ProcTaskDState{
            taskId,
            comm,
            ReadSmallTextFileTrimmed(entry.path() / "wchan"),
        };
    }
    return std::nullopt;
}

bool IsCameraRecorderKernelHangReason(const std::string &reason)
{
    return reason.find(kMainCameraV4l2KernelHangKey) != std::string::npos;
}

bool IsCameraRecorderMainCameraFault(const ugripper::runtime::HealthFault &fault)
{
    return fault.key == kMainCameraV4l2KernelHangKey ||
           fault.key == kMainCameraV4l2StartupFailureKey ||
           fault.key == kMainCameraShortStreamKey;
}

bool IsDiskHealthFault(const ugripper::runtime::HealthFault &fault)
{
    return fault.key == "disk_mount_lost" ||
           fault.key == "disk_not_writable" ||
           fault.key == "disk_full" ||
           fault.key == "disk_space_unavailable" ||
           fault.led_state == ugripper::runtime::RuntimeLedState::Error3;
}

bool IsImmediateRestoreUsbTriggerCause(const std::string &cause)
{
    return cause == kMainCameraV4l2KernelHangKey ||
           cause == kMainCameraV4l2StartupFailureKey ||
           cause == kMainCameraShortStreamKey;
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

ugripper::runtime::HardwareFaultSide restoreUsbSideForText(const std::string &text)
{
    const bool hasLeft = text.find("left") != std::string::npos ||
                         text.find("/dev/cam_left") != std::string::npos ||
                         text.find("/dev/stereo_left") != std::string::npos ||
                         text.find("/dev/left_") != std::string::npos ||
                         text.find("/dev/tcam_left") != std::string::npos;
    const bool hasRight = text.find("right") != std::string::npos ||
                          text.find("/dev/cam_right") != std::string::npos ||
                          text.find("/dev/stereo_right") != std::string::npos ||
                          text.find("/dev/right_") != std::string::npos ||
                          text.find("/dev/tcam_right") != std::string::npos;
    if (hasLeft && hasRight)
    {
        return ugripper::runtime::HardwareFaultSide::Both;
    }
    if (hasLeft)
    {
        return ugripper::runtime::HardwareFaultSide::Left;
    }
    if (hasRight)
    {
        return ugripper::runtime::HardwareFaultSide::Right;
    }
    return ugripper::runtime::HardwareFaultSide::Unknown;
}

const char *restoreUsbSideName(ugripper::runtime::HardwareFaultSide side)
{
    switch (side)
    {
    case ugripper::runtime::HardwareFaultSide::Left:
        return "left";
    case ugripper::runtime::HardwareFaultSide::Right:
        return "right";
    case ugripper::runtime::HardwareFaultSide::Both:
        return "both";
    case ugripper::runtime::HardwareFaultSide::Unknown:
    default:
        return "unknown";
    }
}

std::string compactRestoreUsbEvidence(std::string detail)
{
    const std::string prefix = "Critical device nodes missing:";
    const size_t prefixPos = detail.find(prefix);
    if (prefixPos != std::string::npos)
    {
        detail = detail.substr(prefixPos + prefix.size());
    }
    detail.erase(detail.begin(), std::find_if(detail.begin(), detail.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    detail.erase(std::find_if(detail.rbegin(), detail.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), detail.end());
    std::string compact;
    compact.reserve(detail.size());
    bool previousWasSpace = false;
    for (const char ch : detail)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            if (!previousWasSpace)
            {
                compact.push_back(' ');
                previousWasSpace = true;
            }
            continue;
        }
        compact.push_back(ch);
        previousWasSpace = false;
    }
    return compact.empty() ? "none" : compact;
}

bool isCriticalDeviceMissingEvidence(const std::string &detail)
{
    return detail.find("Critical device nodes missing:") != std::string::npos;
}

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
    uint64_t firstCameraLogTimeNs = 0;
    uint64_t lastCameraLogTimeNs = 0;
    uint64_t imuMessageCount = 0;
    uint64_t cameraMessageCount = 0;
    uint64_t spanNs = 0;
    uint64_t cameraSpanNs = 0;
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
    size_t droppedArtifactComponentPixels = 0;
    size_t droppedArtifactComponentCount = 0;
    bool damaged = false;
};

bool writeTextFileAtomically(const fs::path &path, const std::string &content, std::string *errorMessage);
bool loadJsonFile(const std::string &path, json *output, std::string *errorMessage);
std::string resolveDeviceNodeTarget(const std::string &devicePath);

struct TactileCalibrationTarget
{
    const char *cameraName;
    const char *side;
    const char *sensorSlot;
    const char *devicePath;
    const char *jsonPath;
    const char *serialPlaceholder;
};

constexpr std::array<TactileCalibrationTarget, 4> kTactileCalibrationTargets = {{
    {"left_tcam_l", "left", "l", "/dev/tcam_left_l", "observation.images.tcam_left_l", "{{LEFT_TCAM_L_SERIAL}}"},
    {"left_tcam_r", "left", "r", "/dev/tcam_left_r", "observation.images.tcam_left_r", "{{LEFT_TCAM_R_SERIAL}}"},
    {"right_tcam_l", "right", "l", "/dev/tcam_right_l", "observation.images.tcam_right_l", "{{RIGHT_TCAM_L_SERIAL}}"},
    {"right_tcam_r", "right", "r", "/dev/tcam_right_r", "observation.images.tcam_right_r", "{{RIGHT_TCAM_R_SERIAL}}"},
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

fs::path cameraRecorderFaultPath(const fs::path &episodeDir)
{
    const std::string parent = sanitizeFileComponent(episodeDir.parent_path().filename().string());
    const std::string name = sanitizeFileComponent(episodeDir.filename().string());
    return fs::path("/dev/shm") / (std::string(kCameraRecorderFaultShmPrefix) + parent + "_" + name + ".json");
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

std::string currentBootId()
{
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string bootId;
    if (input.is_open())
    {
        std::getline(input, bootId);
    }
    return bootId;
}

bool writeTactileReferenceMeta(const fs::path &root,
                               const std::string &serial,
                               const TactileCalibrationTarget &target,
                               uint64_t updatedAtMs,
                               std::string *errorMessage)
{
    json meta = {
        {"serial", serial},
        {"camera_name", target.cameraName},
        {"device_path", target.devicePath},
        {"updated_at_ms", updatedAtMs},
    };
    return writeTextFileAtomically(
        tactileReferenceMetaPath(root, serial), meta.dump(2) + "\n", errorMessage);
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

bool isStereoCameraName(const char *cameraName)
{
    return cameraName != nullptr && std::strstr(cameraName, "stereo") != nullptr;
}

bool isStereoDevicePath(const char *path)
{
    return path != nullptr &&
           (std::strstr(path, "/stereo_") != nullptr || std::strstr(path, "_fays_imu") != nullptr);
}

std::vector<EpisodeVideoArtifact> activeEpisodeVideoArtifacts(bool chestCameraEnabled,
                                                              bool stereoEnabled)
{
    std::vector<EpisodeVideoArtifact> artifacts;
    artifacts.reserve(kAllEpisodeVideoArtifacts.size());
    for (const auto &artifact : kAllEpisodeVideoArtifacts)
    {
        if (!chestCameraEnabled && std::strcmp(artifact.cameraName, "chest_cam_main") == 0)
        {
            continue;
        }
        if (!stereoEnabled && isStereoCameraName(artifact.cameraName))
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

std::vector<const char *> activeCriticalDevicePaths(bool chestCameraEnabled, bool stereoEnabled)
{
    std::vector<const char *> paths;
    paths.reserve(kAllCriticalDevicePaths.size());
    for (const char *path : kAllCriticalDevicePaths)
    {
        if (!chestCameraEnabled && std::strcmp(path, "/dev/cam_chest") == 0)
        {
            continue;
        }
        if (!stereoEnabled && isStereoDevicePath(path))
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

std::vector<double> boxBlurFrame(const std::vector<double> &frame,
                                 size_t width,
                                 size_t height,
                                 int radius)
{
    std::vector<double> blurred(frame.size(), 0.0);
    if (frame.size() != width * height || frame.empty() || radius <= 0)
    {
        return frame;
    }

    const int radiusValue = std::max(0, radius);
    for (size_t y = 0; y < height; ++y)
    {
        for (size_t x = 0; x < width; ++x)
        {
            double sum = 0.0;
            size_t count = 0;
            const size_t yBegin = static_cast<size_t>(
                std::max<int64_t>(0, static_cast<int64_t>(y) - radiusValue));
            const size_t yEnd = static_cast<size_t>(
                std::min<int64_t>(static_cast<int64_t>(height) - 1, static_cast<int64_t>(y) + radiusValue));
            const size_t xBegin = static_cast<size_t>(
                std::max<int64_t>(0, static_cast<int64_t>(x) - radiusValue));
            const size_t xEnd = static_cast<size_t>(
                std::min<int64_t>(static_cast<int64_t>(width) - 1, static_cast<int64_t>(x) + radiusValue));
            for (size_t yy = yBegin; yy <= yEnd; ++yy)
            {
                for (size_t xx = xBegin; xx <= xEnd; ++xx)
                {
                    sum += frame[yy * width + xx];
                    ++count;
                }
            }
            blurred[y * width + x] = count > 0 ? sum / static_cast<double>(count) : frame[y * width + x];
        }
    }
    return blurred;
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
           " artifact_drop_px=" + std::to_string(metrics.droppedArtifactComponentPixels) +
           " artifact_drop_components=" + std::to_string(metrics.droppedArtifactComponentCount) +
           " artifact_max_component_px=" + std::to_string(kTactileResidualArtifactMaxComponentPixels) +
           " artifact_lowfreq_mean_thr=" + formatFixed(kTactileResidualArtifactLowFrequencyMeanThreshold, 2) +
           " gain=" + formatFixed(metrics.gain, 4) +
           " offset=" + formatFixed(metrics.offset, 2);
}

size_t countFilteredResidualMask(const std::vector<uint8_t> &rawMask,
                                 size_t width,
                                 size_t height,
                                 size_t minComponentPixels,
                                 const std::vector<double> *lowFrequencyDiff,
                                 size_t *maxComponentPixels,
                                 size_t *droppedArtifactPixelsOut,
                                 size_t *droppedArtifactComponentsOut)
{
    if (rawMask.size() != width * height || rawMask.empty())
    {
        if (maxComponentPixels != nullptr)
        {
            *maxComponentPixels = 0;
        }
        if (droppedArtifactPixelsOut != nullptr)
        {
            *droppedArtifactPixelsOut = 0;
        }
        if (droppedArtifactComponentsOut != nullptr)
        {
            *droppedArtifactComponentsOut = 0;
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
    size_t droppedArtifactPixelCount = 0;
    size_t droppedArtifactComponents = 0;
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
        double lowFrequencySum = 0.0;
        for (size_t cursor = 0; cursor < stack.size(); ++cursor)
        {
            const size_t index = stack[cursor];
            ++componentPixels;
            if (lowFrequencyDiff != nullptr && lowFrequencyDiff->size() == rawMask.size())
            {
                lowFrequencySum += (*lowFrequencyDiff)[index];
            }
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
            const double lowFrequencyMean =
                (lowFrequencyDiff != nullptr && lowFrequencyDiff->size() == rawMask.size() && componentPixels > 0)
                    ? lowFrequencySum / static_cast<double>(componentPixels)
                    : std::numeric_limits<double>::infinity();
            if (componentPixels <= kTactileResidualArtifactMaxComponentPixels &&
                lowFrequencyMean < kTactileResidualArtifactLowFrequencyMeanThreshold)
            {
                droppedArtifactPixelCount += componentPixels;
                ++droppedArtifactComponents;
            }
            else
            {
                retainedPixels += componentPixels;
            }
        }
    }

    if (maxComponentPixels != nullptr)
    {
        *maxComponentPixels = maxComponent;
    }
    if (droppedArtifactPixelsOut != nullptr)
    {
        *droppedArtifactPixelsOut = droppedArtifactPixelCount;
    }
    if (droppedArtifactComponentsOut != nullptr)
    {
        *droppedArtifactComponentsOut = droppedArtifactComponents;
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
    std::vector<double> baselineValues;
    baselineValues.reserve(baseline.size());
    std::vector<double> correctedCurrentValues;
    correctedCurrentValues.reserve(current.size());
    double residualSum = 0.0;
    for (size_t index = 0; index < baseline.size(); ++index)
    {
        const double baseValue = static_cast<double>(baseline[index]);
        const double currentValue = static_cast<double>(current[index]);
        const double correctedCurrent = std::clamp(currentValue * metrics.gain + metrics.offset, 0.0, 255.0);
        const double residual = std::fabs(correctedCurrent - baseValue);
        baselineValues.push_back(baseValue);
        correctedCurrentValues.push_back(correctedCurrent);
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
    for (size_t index = 0; index < residuals.size(); ++index)
    {
        if (residuals[index] >= metrics.residualThreshold)
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
    const std::vector<double> baselineLowFrequency = boxBlurFrame(
        baselineValues,
        static_cast<size_t>(kTactileFrameWidth),
        static_cast<size_t>(kTactileFrameHeight),
        kTactileResidualLowFrequencyBlurRadius);
    const std::vector<double> currentLowFrequency = boxBlurFrame(
        correctedCurrentValues,
        static_cast<size_t>(kTactileFrameWidth),
        static_cast<size_t>(kTactileFrameHeight),
        kTactileResidualLowFrequencyBlurRadius);
    std::vector<double> lowFrequencyDiff;
    lowFrequencyDiff.reserve(baseline.size());
    for (size_t index = 0; index < baseline.size(); ++index)
    {
        lowFrequencyDiff.push_back(std::fabs(currentLowFrequency[index] - baselineLowFrequency[index]));
    }
    const size_t filteredResidualMaskCount = countFilteredResidualMask(
        rawResidualMask,
        static_cast<size_t>(kTactileFrameWidth),
        static_cast<size_t>(kTactileFrameHeight),
        kTactileResidualMinComponentPixels,
        &lowFrequencyDiff,
        &metrics.maxResidualComponentPixels,
        &metrics.droppedArtifactComponentPixels,
        &metrics.droppedArtifactComponentCount);
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
    if (phase == "final" || phase == "ego_cleanup")
    {
        if (phase == "ego_cleanup")
        {
            const fs::path statusPath = episodeDir / "ego" / kEgoSyncStatusFileName;
            return path == statusPath;
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

void flushEpisodeArtifactsToDisk(const fs::path &episodeDir,
                                 bool chestCameraEnabled,
                                 bool stereoEnabled,
                                 const char *phaseLabel)
{
    if (episodeDir.empty())
    {
        return;
    }

    const std::string phase = phaseLabel != nullptr ? phaseLabel : "";
    std::vector<fs::path> paths;
    const auto artifacts = activeEpisodeVideoArtifacts(chestCameraEnabled, stereoEnabled);
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
    if (stereoEnabled)
    {
        paths.push_back(episodeDir / "fays_data_left.mcap");
        paths.push_back(episodeDir / "fays_data_right.mcap");
    }
    paths.push_back(episodeDir / "metadata.json");
    paths.push_back(episodeDir / "calibration.json");
    paths.push_back(episodeDir / "validation_error.log");
    paths.push_back(episodeDir / kEgoSyncStatusFileName);

    const fs::path egoDir = episodeDir / "ego";
    if ((phase == "final" || phase == "ego_cleanup") && fs::exists(egoDir))
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

    if (phase == "final" || phase == "ego_cleanup")
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

std::string asciiLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return text;
}

std::string resolveEgoAdbCommand(const std::string &egoRecordingScript)
{
    fs::path bundled;
    if (!egoRecordingScript.empty())
    {
        bundled = fs::path(egoRecordingScript).parent_path().parent_path() / "adb" / "adb";
    }
    if (!bundled.empty() && access(bundled.c_str(), X_OK) == 0)
    {
        return bundled.string();
    }
    return "adb";
}

std::string readTrimmedSysfsText(const fs::path &path)
{
    std::ifstream input(path);
    std::string value;
    if (!std::getline(input, value))
    {
        return {};
    }
    return asciiLower(trim(value));
}

bool hasAdbUsbInterface()
{
    std::error_code error;
    const fs::path usbRoot("/sys/bus/usb/devices");
    if (!fs::exists(usbRoot, error))
    {
        return true;
    }

    for (const auto &entry : fs::directory_iterator(usbRoot, fs::directory_options::skip_permission_denied, error))
    {
        const std::string interfaceClass = readTrimmedSysfsText(entry.path() / "bInterfaceClass");
        const std::string interfaceSubClass = readTrimmedSysfsText(entry.path() / "bInterfaceSubClass");
        const std::string interfaceProtocol = readTrimmedSysfsText(entry.path() / "bInterfaceProtocol");
        if (interfaceClass == "ff" && interfaceSubClass == "42" && interfaceProtocol == "01")
        {
            return true;
        }
    }
    if (error)
    {
        return true;
    }
    return false;
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

    const int childFdLimit = ChildFileDescriptorLimit();
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
        CloseChildFileDescriptors(childFdLimit);
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
    arguments.push_back("crop=iw*0.82:ih:iw*0.18:0,scale=160:120,format=gray");
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

std::optional<std::string> readTextFileTrimmed(const fs::path &path, std::string *detail)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        if (detail != nullptr)
        {
            *detail = "cannot open " + path.string();
        }
        return std::nullopt;
    }

    std::ostringstream stream;
    stream << input.rdbuf();
    std::string value = trim(stream.str());
    if (value.empty())
    {
        if (detail != nullptr)
        {
            *detail = "empty " + path.string();
        }
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> readTactileUsbSerialFromSysfs(const std::string &devicePath,
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

    fs::path videoNode = resolvedTarget.empty() ? fs::path(resolveDeviceNodeTarget(devicePath))
                                                : fs::path(resolvedTarget);
    const std::string videoName = videoNode.filename().string();
    if (videoName.rfind("video", 0) != 0)
    {
        if (detail != nullptr)
        {
            *detail = "resolved target is not a video node: " + videoNode.string();
        }
        return std::nullopt;
    }

    std::error_code error;
    fs::path deviceSysfs = fs::canonical(fs::path("/sys/class/video4linux") / videoName / "device", error);
    if (error)
    {
        if (detail != nullptr)
        {
            *detail = "cannot resolve video sysfs device for " + videoName + ": " + error.message();
        }
        return std::nullopt;
    }

    for (fs::path current = deviceSysfs; !current.empty(); current = current.parent_path())
    {
        if (current == current.parent_path())
        {
            break;
        }

        const fs::path serialPath = current / "serial";
        if (!fs::exists(serialPath, error) || error)
        {
            error.clear();
            continue;
        }

        std::string readDetail;
        const auto serial = readTextFileTrimmed(serialPath, &readDetail);
        if (serial.has_value() && serial->rfind("xhci-", 0) != 0)
        {
            return serial;
        }
    }

    if (detail != nullptr)
    {
        *detail = "no USB serial found under sysfs for " + videoName;
    }
    return std::nullopt;
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

    const int fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
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
    bool foundFirstCamera = false;
    bool foundCamera = false;
    auto *dataSource = reader.dataSource();
    const auto &chunkIndexes = reader.chunkIndexes();
    if (dataSource != nullptr && !chunkIndexes.empty())
    {
        for (auto it = chunkIndexes.begin();
             it != chunkIndexes.end() && !foundFirstCamera;
             ++it)
        {
            mcap::TypedRecordReader recordReader(
                *dataSource,
                it->chunkStartOffset,
                it->chunkStartOffset + it->chunkLength);
            recordReader.onMessage = [&](const mcap::Message &message,
                                         mcap::ByteOffset,
                                         std::optional<mcap::ByteOffset>) {
                const char kind = classifyFaysMessage(message);
                if (kind == 'c' &&
                    (!foundFirstCamera || message.logTime < summary->firstCameraLogTimeNs))
                {
                    summary->firstCameraLogTimeNs = message.logTime;
                    foundFirstCamera = true;
                }
            };

            while (recordReader.next())
            {
            }

            if (!recordReader.status().ok() && errorMessage != nullptr && errorMessage->empty())
            {
                *errorMessage = "failed to read Fays head chunk: " + recordReader.status().message;
            }
        }

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
    if (!foundImu || !foundCamera || !foundFirstCamera)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Fays topic has zero messages: imu=" +
                            std::string(foundImu ? "present" : "zero") +
                            ", camera=" +
                            std::string(foundCamera ? "present" : "zero") +
                            ", first_camera=" +
                            std::string(foundFirstCamera ? "present" : "zero");
        }
        return false;
    }
    if (summary->lastCameraLogTimeNs > summary->firstCameraLogTimeNs)
    {
        summary->cameraSpanNs = summary->lastCameraLogTimeNs - summary->firstCameraLogTimeNs;
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
    if (summary->cameraSpanNs < static_cast<uint64_t>(kFaysMcapMinSpanNs))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Fays camera span too short: camera_span_ns=" + std::to_string(summary->cameraSpanNs) +
                            ", threshold_ns=" + std::to_string(kFaysMcapMinSpanNs);
        }
        return false;
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

const FaysTailCheckTarget *faysTargetForStereoCamera(const std::string &cameraName)
{
    if (cameraName == "left_stereo")
    {
        return &kFaysTailCheckTargets[0];
    }
    if (cameraName == "right_stereo")
    {
        return &kFaysTailCheckTargets[1];
    }
    return nullptr;
}

bool loadEffectiveVideoDurationSec(const std::string &episodeDir,
                                   const EpisodeVideoArtifact &artifact,
                                   const VideoProbeResult &probe,
                                   double fallbackSec,
                                   double *durationSec,
                                   std::string *source,
                                   std::string *errorMessage)
{
    if (durationSec == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "internal error: missing duration output slot";
        }
        return false;
    }

    *durationSec = fallbackSec;
    if (source != nullptr)
    {
        *source = "mkv_probe";
    }

    const FaysTailCheckTarget *target = faysTargetForStereoCamera(artifact.cameraName);
    if (target == nullptr)
    {
        return true;
    }

    FaysMcapSummary summary;
    std::string faysError;
    if (!loadFaysMcapSummary(
            (fs::path(episodeDir) / target->mcapFileName).string(),
            target->imuTopic,
            target->cameraTopic,
            &summary,
            &faysError))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to load stereo Fays camera duration for " +
                            std::string(artifact.fileName) + ": " + faysError;
        }
        return false;
    }

    *durationSec = static_cast<double>(summary.cameraSpanNs) / 1000000000.0;
    if (source != nullptr)
    {
        *source = std::string("fays_mcap_camera:") + target->mcapFileName;
    }
    return true;
}
}

GripperLedEffect RecordRuntime::makeLedEffect(LedState state, double progress)
{
    switch (state)
    {
    case RecordRuntime::LedState::BootInit:
        return {GripperLedEffectState::BootInit, progress};
    case RecordRuntime::LedState::Init:
        return {GripperLedEffectState::Init, progress};
    case RecordRuntime::LedState::Writing:
        return {GripperLedEffectState::Writing, progress};
    case RecordRuntime::LedState::WaitStorage:
        return {GripperLedEffectState::WaitStorage, progress};
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
    case ugripper::runtime::RuntimeLedState::Writing:
        return LedState::Writing;
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
    case LedState::BootInit:
        return "BootInit";
    case LedState::Init:
        return "Init";
    case LedState::Writing:
        return "Writing";
    case LedState::WaitStorage:
        return "WaitStorage";
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
    closeRecordControlPipe();
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

    stereoEnabled_ = kPackageStereoEnabled;
    DM_LOG_INFO("{}", (::DA::utils::LogString() << kPackageStereoBuildMarker
                         << " PACKAGE_STEREO_ENABLED="
                         << (stereoEnabled_ ? "true" : "false") << std::endl).str());

    perfLogEnabled_ = kPerfLogEnabled;
    g_perfLogEnabled = perfLogEnabled_;
    DM_LOG_INFO("{}", (::DA::utils::LogString() << "UGRIPPER_ENABLE_PERF_LOG="
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
        stereoEnabled_,
        options_.tactileStateDir,
        options_.persistCalibrationFile,
        options_.exampleCalibrationFile,
        options_.fallbackCalibrationFile,
        options_.stereoStatusFile,
        hardwareVersion_,
        packageVersion_,
        updaterVersion_);

    episodeManagerInitialized_ = false;
    initializeMainCameraRuntimeStates();
    initializeTactileCameraRuntimeStates();
    maintainTactileCameraRuntimeStates();

    if (!panelManager_.connect(options_.gripperPorts))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "failed to connect any gripper HMI port" << std::endl).str());
        return false;
    }

    ledController_ = std::make_unique<HmiLedController>(&panelManager_);
    ledController_->start();
    setLedState(LedState::BootInit);

    hmiController_ = std::make_unique<ugripper::runtime::HmiController>(
        ugripper::runtime::HmiControllerOptions{
            .press_debounce_ms = kButtonPressDebounceMs,
            .release_debounce_ms = kButtonReleaseDebounceMs,
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

    if (stereoEnabled_)
    {
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
                .left_control_fifo = options_.leftFaysControlFifo,
                .right_control_fifo = options_.rightFaysControlFifo,
                .status_file = options_.stereoStatusFile,
                .daemon_stop_timeout_ms = 2000,
                .restart_interval_ms = kStereoDaemonRestartIntervalMs,
                .finalize_wait_poll_ms = kStereoFinalizeWaitPollMs,
            },
            &utils::CurrentSteadyMs);
    }
    shutdownRequestPort_ =
        ugripper::runtime::CreateFileShutdownRequestPort(
            options_.systemActionRequestFile,
            options_.systemActionResultFile);

    const auto criticalDevicePaths = activeCriticalDevicePaths(chestCameraEnabled_, stereoEnabled_);
    healthMonitor_ = std::make_unique<ugripper::runtime::HealthMonitor>(
        ugripper::runtime::HealthMonitorOptions{
            .disk_root = options_.diskRoot,
            .stereo_status_file = options_.stereoStatusFile,
            .stereo_enabled = stereoEnabled_,
            .critical_device_paths =
                std::vector<std::string>(criticalDevicePaths.begin(), criticalDevicePaths.end()),
            .poll_interval_ms = kHealthCheckIntervalMs,
            .hmi_active_timeout_ms = kHmiActiveTimeoutMs,
            .stereo_startup_grace_ms = kStereoStartupReadyTimeoutMs,
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
            .stereo_enabled = stereoEnabled_,
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
                [this](const std::string& episode_dir,
                       std::string* error_message,
                       std::vector<std::string>* error_types) {
                    if (episodeManager_ == nullptr)
                    {
                        return false;
                    }
                    return episodeManager_->validateEpisode(episode_dir, error_message, error_types, nullptr);
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
            .set_hardware_fault_led_state =
                [this](const ugripper::runtime::HealthFault& fault) {
                    setHardwareFaultLedState(fault);
                },
            .handle_failure_state =
                [this](ugripper::runtime::RuntimeLedState state,
                       const std::vector<std::string>& error_types,
                       const std::string& detail) {
                    handleFailureState(state, error_types, detail);
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
            .cleanup_ego_remote =
                [this](const std::string& episode_dir, std::string* error_message) {
                    return cleanupEgoRemote(episode_dir, error_message);
                },
            .flush_episode_artifacts =
                [this](const std::string& episode_dir, const char* stage) {
                    flushEpisodeArtifactsToDisk(
                        fs::path(episode_dir), chestCameraEnabled_, stereoEnabled_, stage);
                },
            .write_episode_metadata =
                [this](const std::string& episode_dir,
                       bool quality_ok,
                       const std::string& error_message,
                       const std::string& error_type) {
                    if (episodeManager_ == nullptr)
                    {
                        return;
                    }
                    std::string metadataError;
                    if (!episodeManager_->writeFinalMetadata(
                            episode_dir,
                            quality_ok,
                            error_message,
                            error_type,
                            &metadataError))
                    {
                        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write metadata.json: "
                                      << metadataError << std::endl).str());
                        return;
                    }
                    flushEpisodeArtifactsToDisk(
                        fs::path(episode_dir), chestCameraEnabled_, stereoEnabled_, "metadata_final");
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
            .detect_recording_hardware_fault =
                [this](const std::string& episode_dir) {
                    const auto fault = detectCameraRecorderStartupFailureForEpisode(episode_dir);
                    if (fault.has_value())
                    {
                        activeHardwareFault_ = *fault;
                    }
                    return fault;
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
        refreshGripperRuntimeStateForSide(side, false);
    }
    if (episodeManager_ != nullptr)
    {
        episodeManager_->setGripperRuntimeStates(gripperRuntimeStates_);
    }
    panelManager_.consumeConnectionEvents();

    startAudioPlayer();
    healthState_.first_seen_ms = currentSteadyMs();
    startStereoDaemon();
    initializeRecordControlPipe();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    applyIdleState();
    maintainDataStorage();
    if (!tactileWarningActive_ && episodeManagerInitialized_ && !currentHealthFault().has_value())
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

    while (!stopRequested_.load(std::memory_order_relaxed))
    {
        const uint64_t loopStartMs = currentSteadyMs();
        maintainMainCameraRuntimeStates();
        maintainTactileCameraRuntimeStates();
        maintainAudioPlayer();
        maintainStereoDaemon();
        maintainDataStorage();
        maintainBackgroundTactileValidation();
        maintainRestoreUsbDevicePresence();
        maintainRestoreUsbRetry();
        maintainRestoreUsbFailureAlarm();
        monitorHardwareHealth();

        ButtonSnapshot buttons;
        panelManager_.poll(options_.pollMs, &buttons);
        handleGripperConnectionEvents();
        processPendingGripperRefreshes();
        const bool recordControlAccepted = pollRecordControlPipe();
        if (!recordControlAccepted)
        {
            handleButtons(buttons);
            ButtonSnapshot leftButtons;
            if (panelManager_.getButtonsForPortToken("left_gripper", &leftButtons))
            {
                handleLeftButtons(leftButtons);
            }
        }

        if (isRecordingActive() && !checkRecorderProcesses())
        {
            stopRecording(true, "camera or sensor recorder exited unexpectedly", ugripper::runtime::kErrorTypeRuntimeError);
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

void RecordRuntime::requestStop() noexcept
{
    stopRequested_.store(true, std::memory_order_relaxed);
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
    if (!stereoEnabled_)
    {
        return true;
    }
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
    if (restoreUsbStereoDaemonStopped_)
    {
        return;
    }
    if (stereoSessionClient_ != nullptr)
    {
        stereoSessionClient_->MaintainDaemon();
    }
}

std::optional<ugripper::runtime::HealthFault> RecordRuntime::currentHealthFault() const
{
    if (healthMonitor_ == nullptr)
    {
        return std::nullopt;
    }
    return healthMonitor_->EvaluateHealth();
}

bool RecordRuntime::isStereoStartupWaiting() const
{
    if (!stereoEnabled_)
    {
        return false;
    }
    const uint64_t nowMs = currentSteadyMs();
    const uint64_t firstSeenMs = healthState_.first_seen_ms == 0 ? nowMs : healthState_.first_seen_ms;
    if ((nowMs - firstSeenMs) >= kStereoStartupReadyTimeoutMs)
    {
        return false;
    }

    const std::optional<ugripper::runtime::HealthFault> fault = currentHealthFault();
    return fault.has_value() && IsStereoStartupFaultKey(fault->key);
}

void RecordRuntime::suspendStereoDaemonForRestoreUsb(const std::string &reason)
{
    if (!stereoEnabled_)
    {
        return;
    }
    if (restoreUsbStereoDaemonStopped_)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "stereo daemon already suspended for restore usb"
            << " reason=" << reason
            << std::endl).str());
        return;
    }

    restoreUsbStereoDaemonStopped_ = true;
    DM_LOG_WARN("{}", (::DA::utils::LogString()
        << "suspend stereo daemon before restore usb"
        << " reason=" << reason
        << std::endl).str());
    stopStereoDaemon();
}

void RecordRuntime::resumeStereoDaemonAfterRestoreUsb(const std::string &reason)
{
    if (!stereoEnabled_)
    {
        return;
    }
    if (!restoreUsbStereoDaemonStopped_)
    {
        return;
    }

    restoreUsbStereoDaemonStopped_ = false;
    DM_LOG_WARN("{}", (::DA::utils::LogString()
        << "resume stereo daemon after restore usb"
        << " reason=" << reason
        << std::endl).str());
    startStereoDaemon();
}

bool RecordRuntime::writeStereoControl(bool recording,
                                       const std::string &episodeDir,
                                       int64_t startSystemTimeUs,
                                       int64_t stopSystemTimeUs)
{
    if (!stereoEnabled_)
    {
        return true;
    }
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
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "failed to write stereo control command: " << errorMessage << std::endl).str());
    }
    return ok;
}

bool RecordRuntime::waitForStereoFinalize(const std::string &episodeDir, int timeoutMs, std::string *errorMessage)
{
    if (!stereoEnabled_)
    {
        return true;
    }
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
                                       const std::string &episodeDir,
                                       const std::string &cameraCodec = "")
{
    std::vector<std::string> args = resolvePythonCommand();
    args.push_back(script);
    args.push_back(command);
    args.push_back("--episode-dir");
    args.push_back(episodeDir);
    args.push_back("--status-file");
    args.push_back((fs::path(episodeDir) / "ego" / kEgoSyncStatusFileName).string());
    if (command == "start" && !cameraCodec.empty())
    {
        args.push_back("--codec");
        args.push_back(cameraCodec);
    }
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

    if (!hasAdbUsbInterface())
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "skip ego recording sidecar: no ADB USB interface detected"
            << std::endl).str());
        return true;
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
    if (!egoRecordingWorker_->Start(egoWorkerArgs(options_.egoRecordingScript,
                                                 "start",
                                                 episodeDir,
                                                 options_.cameraCodec),
                                   {},
                                   errorMessage))
    {
        egoRecordingWorker_.reset();
        egoRecordingAttempted_ = false;
        return false;
    }
    const fs::path statusPath = fs::path(episodeDir) / "ego" / kEgoSyncStatusFileName;
    const uint64_t startMs = currentSteadyMs();
    while ((currentSteadyMs() - startMs) < kEgoStartAckWaitMs)
    {
        json status = json::object();
        std::string loadError;
        if (loadJsonFile(statusPath.string(), &status, &loadError) && status.is_object())
        {
            const std::string state = status.value("status", std::string());
            if (state == "recording")
            {
                const std::string remoteEpisode = status.value("remote_episode", std::string());
                if (!remoteEpisode.empty())
                {
                    return true;
                }
            }
            if (status.contains("start_broadcast_returncode"))
            {
                const int returnCode = status.value("start_broadcast_returncode", -1);
                if (returnCode != 0)
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = status.value("error", std::string("ego START_RECORDING broadcast failed"));
                    }
                    return false;
                }
            }
            if (state == "not_found")
            {
                return true;
            }
            if (state == "start_timeout")
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = status.value("error", std::string("ego app did not create episode_*-temp"));
                }
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "ego recording sidecar did not report remote episode before gripper start: "
                    << status.value("error", std::string("start_timeout"))
                    << std::endl).str());
                return true;
            }
            if (state == "start_failed" || state == "error")
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = status.value("error", state);
                }
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kEgoStartAckPollMs));
    }

    if (errorMessage != nullptr)
    {
        *errorMessage = "timed out waiting for ego remote episode status";
    }
    DM_LOG_WARN("{}", (::DA::utils::LogString()
        << "ego recording sidecar did not report remote episode before gripper start"
        << std::endl).str());
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

bool RecordRuntime::cleanupEgoRemote(const std::string &episodeDir,
                                     std::string *errorMessage)
{
    if (!egoRecordingAttempted_)
    {
        return true;
    }
    if (!fs::exists(options_.egoRecordingScript))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ego recording script not found: " + options_.egoRecordingScript;
        }
        return false;
    }
    return runCommandSync(egoWorkerArgs(options_.egoRecordingScript, "cleanup", episodeDir));
}

bool RecordRuntime::waitForEgoFinalize(const std::string &episodeDir,
                                       int timeoutMs,
                                       std::string *errorMessage)
{
    if (!egoRecordingAttempted_)
    {
        return true;
    }
    const fs::path statusPath = fs::path(episodeDir) / "ego" / kEgoSyncStatusFileName;
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
            *errorMessage = std::string("cannot open internal timing shm file: ") + baseInfoPath.string() +
                            " (legacy label " + kEpisodeTimingFileName + ")";
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
            *errorMessage = std::string("failed to parse internal timing shm file: ") +
                            baseInfoPath.string() + " error=" + ex.what();
        }
        return false;
    }

    auto timingErrorForCamera = [&baseInfo](const std::string &cameraName) {
        if (!baseInfo.contains("timing_errors") || !baseInfo["timing_errors"].is_array())
        {
            return std::string();
        }
        for (const auto &entry : baseInfo["timing_errors"])
        {
            if (!entry.is_object() || entry.value("camera_name", std::string()) != cameraName)
            {
                continue;
            }
            return entry.value("error", std::string());
        }
        return std::string();
    };
    for (const auto &artifact : activeEpisodeVideoArtifacts(chestCameraEnabled_, stereoEnabled_))
    {
        if (std::string(artifact.cameraName).find("stereo") != std::string::npos)
        {
            continue;
        }
        const std::string key = std::string(artifact.cameraName) + "_record_time_offset_us";
        if (!baseInfo.contains(key) || !baseInfo[key].is_number_integer())
        {
            if (errorMessage != nullptr)
            {
                std::string detail = timingErrorForCamera(artifact.cameraName);
                *errorMessage = "camera timing offset unavailable: " + std::string(artifact.cameraName);
                if (!detail.empty())
                {
                    *errorMessage += " error=" + detail;
                }
                if (baseInfo.contains(key) && baseInfo[key].is_null())
                {
                    *errorMessage += " value=null";
                }
            }
            return false;
        }
    }

    if (!stereoEnabled_)
    {
        return true;
    }

    if (stereoSessionClient_ == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "stereo session client not initialized";
        }
        return false;
    }

    std::string stereoSessionText;
    if (!stereoSessionClient_->LastSessionJson(episodeDir, &stereoSessionText, errorMessage))
    {
        return false;
    }

    json stereoSession;
    try
    {
        stereoSession = json::parse(stereoSessionText);
    }
    catch (const std::exception &ex)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("failed to parse stereo session metadata: ") + ex.what();
        }
        return false;
    }

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
        if (!cameraInfo.contains("record_time_offset_us") || !cameraInfo["record_time_offset_us"].is_number_integer())
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = std::string("stereo camera info missing record_time_offset_us: ") + cameraName;
                if (cameraInfo.contains("record_time_offset_us") && cameraInfo["record_time_offset_us"].is_null())
                {
                    *errorMessage += " value=null";
                }
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
            *errorMessage = std::string("cannot rewrite internal timing shm file: ") + baseInfoPath.string() +
                            " (legacy label " + kEpisodeTimingFileName + ")";
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

bool RecordRuntime::ensureEpisodeManagerInitialized()
{
    if (episodeManagerInitialized_)
    {
        return true;
    }
    if (episodeManager_ == nullptr)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "episode manager is not constructed"
            << std::endl).str());
        return false;
    }

    if (!episodeManager_->initialize())
    {
        if (!dataStorageWaitLogged_)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "waiting for writable data storage: episode manager initialization failed"
                << " disk_root=" << options_.diskRoot
                << std::endl).str());
            dataStorageWaitLogged_ = true;
        }
        setLedState(LedState::WaitStorage);
        return false;
    }

    episodeManagerInitialized_ = true;
    dataStorageWaitLogged_ = false;
    activeHardwareFault_.reset();
    episodeManager_->setGripperRuntimeStates(gripperRuntimeStates_);
    syncMainCameraRuntimeStatesToEpisodeManager();
    syncTactileCameraRuntimeStatesToEpisodeManager();

    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "data storage ready"
        << " episode_root=" << episodeManager_->dataRoot()
        << std::endl).str());
    if (!isRecordingActive())
    {
        applyIdleState();
        if (!tactileWarningActive_ && !currentHealthFault().has_value())
        {
            setAudioRecoveryCommand("ready");
            sendAudioCommand("ready");
        }
    }
    return true;
}

bool RecordRuntime::maintainDataStorage()
{
    if (episodeManagerInitialized_)
    {
        return true;
    }

    const auto diskFault = detectDiskHealthFault(options_.diskRoot);
    if (diskFault.has_value())
    {
        if (!dataStorageWaitLogged_)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "waiting for writable data storage"
                << " disk_root=" << options_.diskRoot
                << " detail=" << diskFault->detail
                << std::endl).str());
            dataStorageWaitLogged_ = true;
        }
        setLedState(LedState::WaitStorage);
        return false;
    }

    dataStorageWaitLogged_ = false;
    return ensureEpisodeManagerInitialized();
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
            noteRestoreUsbManualInsert(event.side, "hmi_reconnected");
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
            refreshGripperRuntimeStateForSide(side, true);
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

void RecordRuntime::refreshGripperRuntimeStateForSide(const std::string &side,
                                                      bool markTactileReferencePending)
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
    if (ugripper::runtime::ShouldMarkTactileReferencePending(markTactileReferencePending))
    {
        markTactileReferencePendingForSide(side, "gripper_reconnected");
    }
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

void RecordRuntime::initializeTactileCameraRuntimeStates()
{
    tactileCameraRuntimeStates_.clear();
    const uint64_t nowMs = currentSteadyMs();
    for (const auto &target : kTactileCalibrationTargets)
    {
        TactileCameraRuntimeCache state;
        state.cameraName = target.cameraName;
        state.side = target.side;
        state.devicePath = target.devicePath;
        state.nextRefreshAllowedMs = nowMs;
        tactileCameraRuntimeStates_.push_back(std::move(state));
    }
    syncTactileCameraRuntimeStatesToEpisodeManager();
}

void RecordRuntime::maintainTactileCameraRuntimeStates()
{
    bool updatedAny = false;
    const uint64_t nowMs = currentSteadyMs();

    for (auto &state : tactileCameraRuntimeStates_)
    {
        const bool exists = fs::exists(state.devicePath);
        if (!exists)
        {
            if (state.present || !state.serialNumber.empty())
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "tactile camera device removed, clear runtime serial cache: camera=" << state.cameraName
                    << " device=" << state.devicePath << std::endl).str());
                state.present = false;
                state.resolvedTarget.clear();
                state.serialNumber.clear();
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
            state.lastError = "waiting for tactile serial cache refresh";
            state.nextRefreshAllowedMs = nowMs;
            updatedAny = true;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "tactile camera device detected, schedule runtime serial cache refresh: camera=" << state.cameraName
                << " device=" << state.devicePath
                << " target=" << state.resolvedTarget << std::endl).str());
        }

        if (nowMs < state.nextRefreshAllowedMs)
        {
            continue;
        }

        std::string detail;
        const auto serial = readTactileUsbSerialFromSysfs(state.devicePath, state.resolvedTarget, &detail);
        if (serial.has_value() && !serial->empty())
        {
            state.serialNumber = *serial;
            state.lastError.clear();
            state.nextRefreshAllowedMs = std::numeric_limits<uint64_t>::max();
            updatedAny = true;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "tactile camera runtime serial cache refreshed: camera=" << state.cameraName
                << " device=" << state.devicePath
                << " target=" << state.resolvedTarget
                << " serial=" << state.serialNumber << std::endl).str());
            continue;
        }

        const std::string newError = detail.empty() ? "failed to read tactile USB serial from sysfs" : detail;
        const bool shouldLogFailure =
            state.lastError.empty() ||
            state.lastError == "waiting for tactile serial cache refresh" ||
            state.lastError != newError;
        state.serialNumber.clear();
        state.lastError = newError;
        state.nextRefreshAllowedMs = nowMs + kTactileSerialRefreshRetryMs;
        updatedAny = true;
        if (shouldLogFailure)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "tactile camera runtime serial cache refresh failed: camera=" << state.cameraName
                << " device=" << state.devicePath
                << " target=" << state.resolvedTarget
                << " detail=" << state.lastError << std::endl).str());
        }
    }

    if (updatedAny)
    {
        syncTactileCameraRuntimeStatesToEpisodeManager();
    }
}

void RecordRuntime::syncTactileCameraRuntimeStatesToEpisodeManager()
{
    if (episodeManager_ == nullptr)
    {
        return;
    }

    std::array<EpisodeManager::TactileCameraRuntimeState, 4> states{};
    for (size_t index = 0; index < states.size() && index < tactileCameraRuntimeStates_.size(); ++index)
    {
        const auto &source = tactileCameraRuntimeStates_[index];
        auto &target = states[index];
        target.cameraName = source.cameraName;
        target.side = source.side;
        target.devicePath = source.devicePath;
        target.present = source.present;
        target.serialNumber = source.serialNumber;
        target.lastError = source.lastError;
    }
    episodeManager_->setTactileCameraRuntimeStates(states);
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

void RecordRuntime::markTactileReferencePendingForSide(const std::string &side,
                                                       const std::string &reason)
{
    if (episodeManager_ == nullptr)
    {
        return;
    }
    episodeManager_->markTactileReferencePendingForSide(side, reason);
}

void RecordRuntime::applyTactileValidationFindings(
    const std::vector<EpisodeManager::TactileValidationFinding> &findings)
{
    bool warningActive = false;
    tactileWarningSides_ = {};
    for (const auto &finding : findings)
    {
        if (tactileTriggeredAudioCommand_.empty() && finding.warningTriggered)
        {
            tactileTriggeredAudioCommand_ = finding.audioCommand;
        }
        if (finding.warningActive)
        {
            warningActive = true;
            TactileWarningSideState *sideState = nullptr;
            if (finding.side == "left")
            {
                sideState = &tactileWarningSides_[0];
            }
            else if (finding.side == "right")
            {
                sideState = &tactileWarningSides_[1];
            }
            if (sideState != nullptr)
            {
                if (finding.sensorSlot == "l")
                {
                    sideState->leftSensor = true;
                }
                else if (finding.sensorSlot == "r")
                {
                    sideState->rightSensor = true;
                }
            }
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
    resetRestoreUsbErrorWindow();
    if (!episodeManagerInitialized_)
    {
        setLedState(LedState::WaitStorage);
        return;
    }
    const std::optional<ugripper::runtime::HealthFault> fault = currentHealthFault();
    if (fault.has_value())
    {
        if (isStereoStartupWaiting())
        {
            setLedState(LedState::Init);
        }
        else
        {
            setHardwareFaultLedState(*fault);
        }
        return;
    }
    if (tactileWarningActive_)
    {
        setTactileWarningLedState();
        return;
    }
    setLedState(LedState::Ready);
}

bool RecordRuntime::areSideCriticalDevicesReady(const std::string &side) const
{
    const auto &paths = side == "left" ? kLeftCriticalDevicePaths : kRightCriticalDevicePaths;
    for (const char *path : paths)
    {
        if (!stereoEnabled_ && isStereoDevicePath(path))
        {
            continue;
        }
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

void RecordRuntime::handleFailureState(ugripper::runtime::RuntimeLedState state,
                                       const std::vector<std::string> &errorTypes,
                                       const std::string &detail)
{
    bool checkMainCameraRecovery = false;
    if (!shouldRestoreUsbForFailure(state, errorTypes, detail, &checkMainCameraRecovery))
    {
        return;
    }
    const std::string reason = "failure_state:" + std::string(ledStateName(toLedState(state))) +
                               " detail=" + detail;
    const auto side = activeHardwareFault_.has_value()
                          ? activeHardwareFault_->side
                          : restoreUsbSideForText(detail);
    if (shouldDeferRestoreUsbForSide(side, reason))
    {
        return;
    }
    RestoreUsbTriggerContext context;
    context.cause = std::string("failure_") + ledStateName(toLedState(state));
    context.evidence = compactRestoreUsbEvidence(detail);
    context.side = restoreUsbSideName(side);
    if (isCriticalDeviceMissingEvidence(detail))
    {
        if (!confirmRestoreUsbTrigger(context))
        {
            return;
        }
    }
    else
    {
        logRestoreUsbRequested(context);
    }
    triggerRestoreUsbOnError(reason, checkMainCameraRecovery);
}

bool RecordRuntime::shouldRestoreUsbForFailure(ugripper::runtime::RuntimeLedState state,
                                               const std::vector<std::string> &errorTypes,
                                               const std::string &detail,
                                               bool *checkMainCameraRecovery) const
{
    if (checkMainCameraRecovery != nullptr)
    {
        *checkMainCameraRecovery = false;
    }

    if (state == ugripper::runtime::RuntimeLedState::Error2 ||
        state == ugripper::runtime::RuntimeLedState::Error4)
    {
        return true;
    }

    if (state == ugripper::runtime::RuntimeLedState::Error3 ||
        state == ugripper::runtime::RuntimeLedState::Error5)
    {
        return false;
    }

    if (state != ugripper::runtime::RuntimeLedState::Error1)
    {
        return false;
    }

    const std::string lowered = toLower(detail);
    const bool mainCameraText =
        lowered.find("main camera") != std::string::npos ||
        lowered.find("cam_left") != std::string::npos ||
        lowered.find("cam_right") != std::string::npos ||
        lowered.find("cam_chest") != std::string::npos ||
        lowered.find("/dev/cam_") != std::string::npos;
    if (!mainCameraText)
    {
        return false;
    }

    const bool relevantErrorType =
        std::find(errorTypes.begin(), errorTypes.end(), ugripper::runtime::kErrorTypeCalibrationError) != errorTypes.end() ||
        std::find(errorTypes.begin(), errorTypes.end(), ugripper::runtime::kErrorTypeMissingFile) != errorTypes.end() ||
        std::find(errorTypes.begin(), errorTypes.end(), ugripper::runtime::kErrorTypeFrameLoss) != errorTypes.end() ||
        std::find(errorTypes.begin(), errorTypes.end(), ugripper::runtime::kErrorTypeCollectionDurationTooShort) != errorTypes.end();
    if (!relevantErrorType)
    {
        return false;
    }

    if (checkMainCameraRecovery != nullptr)
    {
        *checkMainCameraRecovery = true;
    }
    return true;
}

void RecordRuntime::resetCameraRecorderDStateTracking()
{
    cameraRecorderDStatePid_ = -1;
    cameraRecorderDStateTaskId_.clear();
    cameraRecorderDStateFirstSeenMs_ = 0;
    cameraRecorderDStateLastLogMs_ = 0;
}

std::optional<ugripper::runtime::HealthFault> RecordRuntime::detectCameraRecorderStartupFailure()
{
    if (!isRecordingActive())
    {
        return std::nullopt;
    }

    const std::string episodeDir = currentEpisodeDir();
    if (episodeDir.empty())
    {
        return std::nullopt;
    }

    return detectCameraRecorderStartupFailureForEpisode(episodeDir);
}

std::optional<ugripper::runtime::HealthFault> RecordRuntime::detectCameraRecorderStartupFailureForEpisode(const std::string &episodeDir)
{
    if (episodeDir.empty())
    {
        return std::nullopt;
    }

    const fs::path faultPath = cameraRecorderFaultPath(episodeDir);
    std::error_code error;
    if (!fs::exists(faultPath, error) || error)
    {
        return std::nullopt;
    }

    std::ifstream input(faultPath);
    if (!input.is_open())
    {
        return std::nullopt;
    }

    json root;
    try
    {
        root = json::parse(input);
    }
    catch (const std::exception &ex)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "camera recorder fault file parse failed: path=" << faultPath
            << " error=" << ex.what()
            << std::endl).str());
        return std::nullopt;
    }

    const std::string faultType = root.value("fault_type", std::string());
    if (faultType != kMainCameraV4l2StartupFailureKey &&
        faultType != kMainCameraShortStreamKey)
    {
        return std::nullopt;
    }

    const std::string faultEpisodeDir = root.value("episode_dir", std::string());
    if (!faultEpisodeDir.empty() && faultEpisodeDir != episodeDir)
    {
        return std::nullopt;
    }

    const std::string cameraName = root.value("camera_name", std::string());
    const std::string device = root.value("device", std::string());
    const std::string faultError = root.value("error", std::string());
    if (cameraName.empty() && device.empty() && faultError.empty())
    {
        return std::nullopt;
    }

    const bool shortStreamFault = faultType == kMainCameraShortStreamKey;
    const char *faultKey = shortStreamFault
        ? kMainCameraShortStreamKey
        : kMainCameraV4l2StartupFailureKey;
    std::ostringstream detail;
    detail << (shortStreamFault
                   ? "CameraRecorder main camera short stream"
                   : "CameraRecorder main camera startup failure")
           << " camera=" << (cameraName.empty() ? "unknown" : cameraName)
           << " device=" << (device.empty() ? "unknown" : device)
           << " error=" << (faultError.empty() ? "unknown" : faultError);
    if (shortStreamFault)
    {
        detail << " frame_count=" << root.value("frame_count", static_cast<uint64_t>(0))
               << " span_us=" << root.value("span_us", static_cast<int64_t>(-1))
               << " session_us=" << root.value("session_us", static_cast<int64_t>(-1))
               << " phase=" << root.value("phase", std::string("unknown"));
    }
    detail << " fault_file=" << faultPath.string();

    const std::string sideText = cameraName + " " + device + " " + faultError;
    return ugripper::runtime::HealthFault{
        ugripper::runtime::RuntimeLedState::Error2,
        restoreUsbSideForText(sideText),
        faultKey,
        detail.str(),
    };
}

std::optional<ugripper::runtime::HealthFault> RecordRuntime::detectCameraRecorderKernelHang()
{
    if (!isRecordingActive())
    {
        resetCameraRecorderDStateTracking();
        return std::nullopt;
    }

    const auto status = processSupervisor_.GetStatus(ugripper::runtime::WorkerName::CameraRecorder);
    if (!status.running || status.pid <= 0)
    {
        resetCameraRecorderDStateTracking();
        return std::nullopt;
    }

    const auto dStateTask = FindFirstDStateTask(status.pid);
    if (!dStateTask.has_value())
    {
        if (cameraRecorderDStateFirstSeenMs_ != 0)
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "camera recorder D-state cleared"
                << " pid=" << cameraRecorderDStatePid_
                << " tid=" << cameraRecorderDStateTaskId_
                << std::endl).str());
        }
        resetCameraRecorderDStateTracking();
        return std::nullopt;
    }

    const uint64_t nowMs = currentSteadyMs();
    const bool newTask =
        cameraRecorderDStatePid_ != status.pid ||
        cameraRecorderDStateTaskId_ != dStateTask->taskId ||
        cameraRecorderDStateFirstSeenMs_ == 0;
    if (newTask)
    {
        cameraRecorderDStatePid_ = status.pid;
        cameraRecorderDStateTaskId_ = dStateTask->taskId;
        cameraRecorderDStateFirstSeenMs_ = nowMs;
        cameraRecorderDStateLastLogMs_ = 0;
    }

    const uint64_t stuckMs = nowMs >= cameraRecorderDStateFirstSeenMs_
                                 ? nowMs - cameraRecorderDStateFirstSeenMs_
                                 : 0;
    std::ostringstream detail;
    detail << "CameraRecorder task stuck in D state"
           << " pid=" << status.pid
           << " tid=" << dStateTask->taskId
           << " comm=" << dStateTask->comm
           << " wchan=" << (dStateTask->wchan.empty() ? "unknown" : dStateTask->wchan)
           << " stuck_ms=" << stuckMs;

    if (cameraRecorderDStateLastLogMs_ == 0 ||
        nowMs - cameraRecorderDStateLastLogMs_ >= kCameraRecorderDStateLogIntervalMs)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "camera recorder D-state observed"
            << " pid=" << status.pid
            << " tid=" << dStateTask->taskId
            << " comm=" << dStateTask->comm
            << " wchan=" << (dStateTask->wchan.empty() ? "unknown" : dStateTask->wchan)
            << " stuck_ms=" << stuckMs
            << " threshold_ms=" << kCameraRecorderDStateFaultMs
            << std::endl).str());
        cameraRecorderDStateLastLogMs_ = nowMs;
    }

    if (stuckMs < kCameraRecorderDStateFaultMs)
    {
        return std::nullopt;
    }

    return ugripper::runtime::HealthFault{
        ugripper::runtime::RuntimeLedState::Error2,
        ugripper::runtime::HardwareFaultSide::Unknown,
        kMainCameraV4l2KernelHangKey,
        detail.str(),
    };
}

bool RecordRuntime::mainCameraRestoreTargetsHealthy() const
{
    for (const auto &state : mainCameraRuntimeStates_)
    {
        if (!state.enabled)
        {
            continue;
        }
        std::string calibrationDetail;
        if (!state.present ||
            !isYuzhouMainCameraSn(state.serialNumber) ||
            !state.calibrationPayloadCached ||
            !validateYuzhouMainCameraCalibrationPayload(state.calibrationPayload, &calibrationDetail))
        {
            return false;
        }
    }
    return true;
}

bool RecordRuntime::restoreUsbTargetRecovered() const
{
    if (healthState_.status == ugripper::runtime::HealthStatus::Error)
    {
        return false;
    }
    if (restoreUsbCheckMainCameraRecovery_ && !mainCameraRestoreTargetsHealthy())
    {
        return false;
    }
    return true;
}

bool RecordRuntime::restoreUsbCriticalSymlinksPresent(std::string *detail) const
{
    std::vector<std::string> missing;
    const auto paths = activeCriticalDevicePaths(chestCameraEnabled_, stereoEnabled_);
    for (const char *path : paths)
    {
        std::error_code error;
        if (path == nullptr || *path == '\0')
        {
            continue;
        }
        if (!fs::exists(path, error))
        {
            missing.push_back(path);
        }
    }

    if (detail != nullptr)
    {
        if (missing.empty())
        {
            detail->clear();
        }
        else
        {
            std::ostringstream stream;
            for (size_t index = 0; index < missing.size(); ++index)
            {
                if (index > 0)
                {
                    stream << ", ";
                }
                stream << missing[index];
            }
            *detail = stream.str();
        }
    }
    return missing.empty();
}

bool RecordRuntime::sideHasAnyRestoreUsbDevice(const std::string &side) const
{
    const char *gripperPath = side == "left" ? "/dev/left_gripper" : "/dev/right_gripper";
    std::error_code error;
    if (fs::exists(gripperPath, error))
    {
        return true;
    }

    const auto &paths = side == "left" ? kLeftCriticalDevicePaths : kRightCriticalDevicePaths;
    for (const char *path : paths)
    {
        if (!stereoEnabled_ && isStereoDevicePath(path))
        {
            continue;
        }
        error.clear();
        if (path != nullptr && *path != '\0' && fs::exists(path, error))
        {
            return true;
        }
    }
    return false;
}

bool RecordRuntime::shouldDeferRestoreUsbForSide(ugripper::runtime::HardwareFaultSide side,
                                                 const std::string &reason) const
{
    const uint64_t nowMs = currentSteadyMs();
    const auto sideDeferred = [&](const std::string &sideName) {
        const size_t index = gripperStateIndexForSide(sideName);
        if (!sideHasAnyRestoreUsbDevice(sideName))
        {
            return true;
        }
        const uint64_t graceUntilMs = restoreUsbInsertGraceUntilMs_[index];
        if (graceUntilMs != 0 && nowMs < graceUntilMs)
        {
            return true;
        }
        return false;
    };

    switch (side)
    {
    case ugripper::runtime::HardwareFaultSide::Left:
        return sideDeferred("left");
    case ugripper::runtime::HardwareFaultSide::Right:
        return sideDeferred("right");
    case ugripper::runtime::HardwareFaultSide::Both:
    {
        const bool leftDeferred = sideDeferred("left");
        const bool rightDeferred = sideDeferred("right");
        return leftDeferred || rightDeferred;
    }
    case ugripper::runtime::HardwareFaultSide::Unknown:
    default:
    {
        const bool leftDeferred = sideDeferred("left");
        const bool rightDeferred = sideDeferred("right");
        return leftDeferred || rightDeferred;
    }
    }
}

void RecordRuntime::triggerRestoreUsbFailureAlarm(const std::string &reason)
{
    if (restoreUsbFailureAlarmActive_)
    {
        return;
    }
    restoreUsbFailureAlarmActive_ = true;
    restoreUsbFailureAlarmStartMs_ = currentSteadyMs();

    bool wroteAny = false;
    wroteAny = panelManager_.setBeepEnabledForSide("left", true) || wroteAny;
    wroteAny = panelManager_.setBeepEnabledForSide("right", true) || wroteAny;

    DM_LOG_ERROR("{}", (::DA::utils::LogString()
        << "restore usb failed after max attempts; hmi beep alarm triggered"
        << " attempts=" << restoreUsbAttemptsInCurrentError_
        << " side=both"
        << " wrote_any=" << boolText(wroteAny)
        << " reason=" << reason
        << std::endl).str());
}

void RecordRuntime::maintainRestoreUsbFailureAlarm()
{
    if (!restoreUsbFailureAlarmActive_ || restoreUsbFailureAlarmStartMs_ == 0)
    {
        return;
    }
    const uint64_t nowMs = currentSteadyMs();
    if (nowMs < restoreUsbFailureAlarmStartMs_ ||
        nowMs - restoreUsbFailureAlarmStartMs_ < kRestoreUsbFailureAlarmDurationMs)
    {
        return;
    }
    panelManager_.silenceBeep();
    restoreUsbFailureAlarmActive_ = false;
    restoreUsbFailureAlarmStartMs_ = 0;
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "restore usb failure hmi beep alarm auto silenced"
        << " duration_ms=" << kRestoreUsbFailureAlarmDurationMs
        << std::endl).str());
}

void RecordRuntime::noteRestoreUsbManualInsert(const std::string &side, const std::string &reason)
{
    if (side != "left" && side != "right")
    {
        return;
    }
    if (restoreUsbInProgress_->load() || restoreUsbPendingRetryCheck_)
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "ignore restore usb manual insert during software restore settle"
            << " side=" << side
            << " reason=" << reason
            << std::endl).str());
        return;
    }
    const size_t index = gripperStateIndexForSide(side);
    const uint64_t nowMs = currentSteadyMs();
    resetRestoreUsbErrorWindow();
    restoreUsbSidePresent_[index] = true;
    restoreUsbInsertGraceUntilMs_[index] = nowMs + kRestoreUsbManualInsertGraceMs;
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "restore usb manual insert detected; reset attempts and start settle grace"
        << " side=" << side
        << " grace_ms=" << kRestoreUsbManualInsertGraceMs
        << " reason=" << reason
        << std::endl).str());
}

void RecordRuntime::maintainRestoreUsbDevicePresence()
{
    if (restoreUsbInProgress_->load() || restoreUsbPendingRetryCheck_)
    {
        return;
    }

    for (const std::string side : {"left", "right"})
    {
        const size_t index = gripperStateIndexForSide(side);
        const bool present = sideHasAnyRestoreUsbDevice(side);
        if (present && !restoreUsbSidePresent_[index])
        {
            noteRestoreUsbManualInsert(side, "device_node_inserted");
            continue;
        }
        if (!present && restoreUsbSidePresent_[index])
        {
            resetRestoreUsbErrorWindow();
            restoreUsbSidePresent_[index] = false;
            restoreUsbInsertGraceUntilMs_[index] = 0;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "restore usb side appears unplugged"
                << " side=" << side
                << std::endl).str());
            continue;
        }
        if (present)
        {
            const uint64_t nowMs = currentSteadyMs();
            if (restoreUsbInsertGraceUntilMs_[index] != 0 && nowMs >= restoreUsbInsertGraceUntilMs_[index])
            {
                restoreUsbInsertGraceUntilMs_[index] = 0;
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                    << "restore usb manual insert settle grace expired"
                    << " side=" << side
                    << std::endl).str());
            }
        }
    }
}

void RecordRuntime::resetRestoreUsbErrorWindow()
{
    restoreUsbAttemptsInCurrentError_ = 0;
    restoreUsbPendingRetryCheck_ = false;
    restoreUsbSymlinkCheckPassed_ = false;
    restoreUsbSymlinkPassedMs_ = 0;
    restoreUsbLastSymlinkProbeMs_ = 0;
    restoreUsbLastMissingLogMs_ = 0;
    restoreUsbLastMissingDetail_.clear();
    restoreUsbCheckMainCameraRecovery_ = false;
    restoreUsbLastReason_.clear();
    restoreUsbLastFinishMs_->store(0);
    restoreUsbPendingTriggerKey_.clear();
    restoreUsbPendingTriggerCause_.clear();
    restoreUsbPendingTriggerEvidence_.clear();
    restoreUsbPendingTriggerSide_.clear();
    restoreUsbPendingTriggerFirstSeenMs_ = 0;
    restoreUsbPendingTriggerLastSeenMs_ = 0;
    restoreUsbPendingTriggerLogged_ = false;
    restoreUsbLastSkipLogKey_.clear();
    if (restoreUsbFailureAlarmActive_)
    {
        panelManager_.silenceBeep();
        restoreUsbFailureAlarmActive_ = false;
    }
    restoreUsbFailureAlarmStartMs_ = 0;
    restoreUsbPreflightFailureUntilMs_ = 0;
    if (!restoreUsbInProgress_->load())
    {
        resumeStereoDaemonAfterRestoreUsb("restore_error_window_reset");
    }
}

bool RecordRuntime::isDataDiskMounted() const
{
    const fs::path diskRoot(options_.diskRoot);
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

bool RecordRuntime::requestDataDiskUmountForRestoreUsb(const std::string &reason)
{
    if (shutdownRequestPort_ == nullptr)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "skip restore usb: cannot umount data disk, system action port not initialized"
            << " reason=" << reason
            << std::endl).str());
        return false;
    }

    std::string errorMessage;
    DM_LOG_WARN("{}", (::DA::utils::LogString()
        << "restore usb preflight request data disk umount"
        << " disk_root=" << options_.diskRoot
        << " reason=" << reason
        << std::endl).str());
    if (!shutdownRequestPort_->RequestAction("umount", &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "skip restore usb: failed to request data disk umount"
            << " error=" << errorMessage
            << " reason=" << reason
            << std::endl).str());
        return false;
    }

    std::string result;
    if (!shutdownRequestPort_->WaitForActionResult(
            &result,
            static_cast<int>(kSystemActionResultWaitMs),
            &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "skip restore usb: timed out waiting for data disk umount"
            << " error=" << errorMessage
            << " reason=" << reason
            << std::endl).str());
        return false;
    }

    if (result != "ok")
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "skip restore usb: data disk umount failed"
            << " result=" << result
            << " reason=" << reason
            << std::endl).str());
        return false;
    }

    DM_LOG_WARN("{}", (::DA::utils::LogString()
        << "restore usb preflight data disk umounted"
        << " disk_root=" << options_.diskRoot
        << " reason=" << reason
        << std::endl).str());
    return true;
}

bool RecordRuntime::prepareDataDiskForRestoreUsb(const std::string &reason)
{
    const bool allowKernelHangRestore = IsCameraRecorderKernelHangReason(reason);
    if (isRecordingActive())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "restore usb preflight stop recording before power reset"
            << " reason=" << reason
            << std::endl).str());
        const std::string stopReason = "restore usb preflight before power reset: " + reason;
        if (!stopRecording(true, stopReason, ugripper::runtime::kErrorTypeRuntimeError))
        {
            if (!allowKernelHangRestore)
            {
                DM_LOG_ERROR("{}", (::DA::utils::LogString()
                    << "skip restore usb: failed to stop recording before power reset"
                    << " reason=" << reason
                    << std::endl).str());
                return false;
            }
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "continue restore usb despite stop failure for camera recorder kernel hang"
                << " reason=" << reason
                << std::endl).str());
        }
    }

    if (!isDataDiskMounted())
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "restore usb preflight skip data disk umount: disk is not mounted"
            << " disk_root=" << options_.diskRoot
            << " reason=" << reason
            << std::endl).str());
        return true;
    }

    syncRuntimeLogToDisk("restore usb preflight");
    std::error_code flushError;
    if (!flushDirectoryToDisk(fs::path(options_.diskRoot), &flushError))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "restore usb preflight failed to flush data disk root before umount"
            << " disk_root=" << options_.diskRoot
            << " error=" << flushError.message()
            << " reason=" << reason
            << std::endl).str());
    }

    if (!requestDataDiskUmountForRestoreUsb(reason))
    {
        if (!allowKernelHangRestore)
        {
            return false;
        }
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "continue restore usb despite data disk umount failure for camera recorder kernel hang"
            << " disk_root=" << options_.diskRoot
            << " reason=" << reason
            << std::endl).str());
    }
    return true;
}

void RecordRuntime::logRestoreUsbRequested(const RestoreUsbTriggerContext &context)
{
    DM_LOG_WARN("{}", (::DA::utils::LogString()
        << "restore usb requested"
        << " cause=" << context.cause
        << " evidence=" << context.evidence
        << " side=" << context.side
        << std::endl).str());
}

bool RecordRuntime::confirmRestoreUsbTrigger(const RestoreUsbTriggerContext &context)
{
    const uint64_t nowMs = currentSteadyMs();
    if (IsImmediateRestoreUsbTriggerCause(context.cause))
    {
        if (!restoreUsbPendingTriggerKey_.empty())
        {
            const uint64_t ageMs = restoreUsbPendingTriggerFirstSeenMs_ == 0
                                       ? 0
                                       : nowMs - restoreUsbPendingTriggerFirstSeenMs_;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "restore usb pending cleared"
                << " cause=" << restoreUsbPendingTriggerCause_
                << " evidence=" << restoreUsbPendingTriggerEvidence_
                << " side=" << restoreUsbPendingTriggerSide_
                << " age_ms=" << ageMs
                << std::endl).str());
        }
        restoreUsbPendingTriggerKey_.clear();
        restoreUsbPendingTriggerCause_.clear();
        restoreUsbPendingTriggerEvidence_.clear();
        restoreUsbPendingTriggerSide_.clear();
        restoreUsbPendingTriggerFirstSeenMs_ = 0;
        restoreUsbPendingTriggerLastSeenMs_ = 0;
        restoreUsbPendingTriggerLogged_ = false;
        return true;
    }

    const std::string key = context.cause + "|" + context.side + "|" + context.evidence;
    if (restoreUsbPendingTriggerKey_ != key)
    {
        if (!restoreUsbPendingTriggerKey_.empty())
        {
            const uint64_t ageMs = restoreUsbPendingTriggerFirstSeenMs_ == 0
                                       ? 0
                                       : nowMs - restoreUsbPendingTriggerFirstSeenMs_;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "restore usb pending cleared"
                << " cause=" << restoreUsbPendingTriggerCause_
                << " evidence=" << restoreUsbPendingTriggerEvidence_
                << " side=" << restoreUsbPendingTriggerSide_
                << " age_ms=" << ageMs
                << std::endl).str());
        }
        restoreUsbPendingTriggerKey_ = key;
        restoreUsbPendingTriggerCause_ = context.cause;
        restoreUsbPendingTriggerEvidence_ = context.evidence;
        restoreUsbPendingTriggerSide_ = context.side;
        restoreUsbPendingTriggerFirstSeenMs_ = nowMs;
        restoreUsbPendingTriggerLastSeenMs_ = nowMs;
        restoreUsbPendingTriggerLogged_ = false;
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "restore usb pending wait"
            << " cause=" << context.cause
            << " evidence=" << context.evidence
            << " side=" << context.side
            << " stable_ms=" << kRestoreUsbTriggerStableMs
            << std::endl).str());
        return false;
    }

    restoreUsbPendingTriggerLastSeenMs_ = nowMs;
    const uint64_t ageMs = restoreUsbPendingTriggerFirstSeenMs_ == 0
                               ? 0
                               : nowMs - restoreUsbPendingTriggerFirstSeenMs_;
    if (ageMs < kRestoreUsbTriggerStableMs)
    {
        return false;
    }

    if (!restoreUsbPendingTriggerLogged_)
    {
        logRestoreUsbRequested(context);
        restoreUsbPendingTriggerLogged_ = true;
    }
    return true;
}

void RecordRuntime::maintainRestoreUsbRetry()
{
    if (!restoreUsbPendingRetryCheck_ || restoreUsbInProgress_->load())
    {
        return;
    }

    const uint64_t finishMs = restoreUsbLastFinishMs_->load();
    if (finishMs == 0)
    {
        return;
    }

    const uint64_t nowMs = currentSteadyMs();
    if (nowMs < finishMs)
    {
        return;
    }

    const uint64_t elapsedMs = nowMs - finishMs;
    if (restoreUsbTargetRecovered())
    {
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "restore usb recovered"
            << " attempt="
            << restoreUsbAttemptsInCurrentError_
            << " elapsed_ms=" << elapsedMs
            << std::endl).str());
        resetRestoreUsbErrorWindow();
        return;
    }

    if (!restoreUsbSymlinkCheckPassed_)
    {
        if (restoreUsbLastSymlinkProbeMs_ != 0 &&
            nowMs >= restoreUsbLastSymlinkProbeMs_ &&
            nowMs - restoreUsbLastSymlinkProbeMs_ < kRestoreUsbSymlinkPollIntervalMs)
        {
            return;
        }
        restoreUsbLastSymlinkProbeMs_ = nowMs;

        std::string missingDetail;
        if (!restoreUsbCriticalSymlinksPresent(&missingDetail))
        {
            if (elapsedMs < kRestoreUsbSymlinkMaxWaitMs)
            {
                if (restoreUsbLastMissingDetail_ != missingDetail ||
                    restoreUsbLastMissingLogMs_ == 0 ||
                    nowMs - restoreUsbLastMissingLogMs_ >= 5000)
                {
                    restoreUsbLastMissingDetail_ = missingDetail;
                    restoreUsbLastMissingLogMs_ = nowMs;
                    DM_LOG_INFO("{}", (::DA::utils::LogString()
                        << "restore usb symlinks waiting"
                        << " attempt=" << restoreUsbAttemptsInCurrentError_
                        << " elapsed_ms=" << elapsedMs
                        << " evidence=" << missingDetail
                        << std::endl).str());
                }
                return;
            }
            DM_LOG_ERROR("{}", (::DA::utils::LogString()
                << "restore usb symlinks timeout"
                << " attempt=" << restoreUsbAttemptsInCurrentError_
                << " elapsed_ms=" << elapsedMs
                << " evidence=" << missingDetail
                << std::endl).str());
            restoreUsbPendingRetryCheck_ = false;
            if (restoreUsbAttemptsInCurrentError_ >= kRestoreUsbMaxAttemptsPerError)
            {
                resumeStereoDaemonAfterRestoreUsb(restoreUsbLastReason_ + ":critical_symlinks_missing");
                triggerRestoreUsbFailureAlarm(
                    restoreUsbLastReason_ + ":critical_symlinks_missing:" + missingDetail);
                return;
            }
            triggerRestoreUsbOnError(restoreUsbLastReason_ + ":critical_symlinks_missing_retry",
                                     restoreUsbCheckMainCameraRecovery_);
            return;
        }

        restoreUsbSymlinkCheckPassed_ = true;
        resumeStereoDaemonAfterRestoreUsb(restoreUsbLastReason_ + ":critical_symlinks_present");
        restoreUsbSymlinkPassedMs_ = currentSteadyMs();
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "restore usb symlinks ready"
            << " attempt="
            << restoreUsbAttemptsInCurrentError_
            << " elapsed_ms=" << elapsedMs
            << std::endl).str());
    }

    const uint64_t symlinkPassedMs = restoreUsbSymlinkPassedMs_ != 0 ? restoreUsbSymlinkPassedMs_ : finishMs;
    const uint64_t readyElapsedMs = nowMs >= symlinkPassedMs ? nowMs - symlinkPassedMs : 0;
    if (readyElapsedMs < kRestoreUsbReadyCheckDelayMs)
    {
        return;
    }

    restoreUsbPendingRetryCheck_ = false;
    if (restoreUsbTargetRecovered())
    {
        const int attempt = restoreUsbAttemptsInCurrentError_;
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "restore usb recovered"
            << " attempt="
            << attempt
            << " elapsed_ms=" << elapsedMs
            << " ready_elapsed_ms=" << readyElapsedMs
            << std::endl).str());
        resetRestoreUsbErrorWindow();
        return;
    }

    DM_LOG_ERROR("{}", (::DA::utils::LogString()
        << "restore usb ready timeout"
        << " attempt=" << restoreUsbAttemptsInCurrentError_
        << " elapsed_ms=" << elapsedMs
        << " ready_elapsed_ms=" << readyElapsedMs
        << std::endl).str());
    if (restoreUsbAttemptsInCurrentError_ >= kRestoreUsbMaxAttemptsPerError)
    {
        triggerRestoreUsbFailureAlarm(restoreUsbLastReason_ + ":ready_timeout");
        return;
    }

    triggerRestoreUsbOnError(restoreUsbLastReason_ + ":ready_retry", restoreUsbCheckMainCameraRecovery_);
}

void RecordRuntime::logRestoreUsbSkipOnce(const std::string &key, const std::string &message)
{
    if (key.empty() || restoreUsbLastSkipLogKey_ == key)
    {
        return;
    }
    restoreUsbLastSkipLogKey_ = key;
    DM_LOG_WARN("{}", (::DA::utils::LogString() << message << std::endl).str());
}

void RecordRuntime::triggerRestoreUsbOnError(const std::string &reason, bool checkMainCameraRecovery)
{
    if (restoreUsbPendingRetryCheck_)
    {
        return;
    }

    if (restoreUsbAttemptsInCurrentError_ >= kRestoreUsbMaxAttemptsPerError)
    {
        return;
    }

    if (options_.restoreUsbCommand.empty())
    {
        logRestoreUsbSkipOnce("restore_command_empty",
                              "skip restore usb on error: restore command is empty");
        return;
    }

    std::error_code error;
    if (!fs::exists(options_.restoreUsbCommand, error))
    {
        logRestoreUsbSkipOnce(
            "restore_command_not_found:" + options_.restoreUsbCommand,
            (::DA::utils::LogString()
                << "skip restore usb on error: command not found: "
                << options_.restoreUsbCommand).str());
        return;
    }

    restoreUsbLastReason_ = reason;
    restoreUsbCheckMainCameraRecovery_ = restoreUsbCheckMainCameraRecovery_ || checkMainCameraRecovery;

    const uint64_t nowMs = currentSteadyMs();
    if (restoreUsbPreflightFailureUntilMs_ != 0 && nowMs < restoreUsbPreflightFailureUntilMs_)
    {
        return;
    }

    if (!prepareDataDiskForRestoreUsb(reason))
    {
        restoreUsbPreflightFailureUntilMs_ = currentSteadyMs() + kRestoreUsbPreflightFailureCooldownMs;
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "skip restore usb on error: data disk preflight failed"
            << " cooldown_ms=" << kRestoreUsbPreflightFailureCooldownMs
            << " attempts=" << restoreUsbAttemptsInCurrentError_
            << " reason=" << reason
            << std::endl).str());
        return;
    }
    restoreUsbPreflightFailureUntilMs_ = 0;

    bool expected = false;
    if (!restoreUsbInProgress_->compare_exchange_strong(expected, true))
    {
        return;
    }

    restoreUsbLastSkipLogKey_.clear();
    suspendStereoDaemonForRestoreUsb(reason);

    ++restoreUsbAttemptsInCurrentError_;
    restoreUsbPendingRetryCheck_ = true;
    restoreUsbSymlinkCheckPassed_ = false;
    restoreUsbSymlinkPassedMs_ = 0;
    restoreUsbLastSymlinkProbeMs_ = 0;
    restoreUsbLastMissingLogMs_ = 0;
    restoreUsbLastMissingDetail_.clear();
    restoreUsbLastFinishMs_->store(0);

    const std::string command = "sudo -n " + shellQuote(options_.restoreUsbCommand);
    const auto inProgress = restoreUsbInProgress_;
    const auto lastFinishMs = restoreUsbLastFinishMs_;
    const int attempt = restoreUsbAttemptsInCurrentError_;
    std::thread([command, inProgress, lastFinishMs, attempt]() {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "restore usb start"
            << " attempt=" << attempt
            << std::endl).str());

        const int rc = std::system(command.c_str());
        if (rc == -1)
        {
            DM_LOG_ERROR("{}", (::DA::utils::LogString()
                << "restore usb command failed to start"
                << " command=" << command
                << std::endl).str());
        }
        else if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0)
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "restore usb command completed"
                << std::endl).str());
        }
        else
        {
            DM_LOG_ERROR("{}", (::DA::utils::LogString()
                << "restore usb command failed"
                << " command=" << command
                << " status=" << rc
                << std::endl).str());
        }
        lastFinishMs->store(RecordRuntime::currentSteadyMs());
        inProgress->store(false);
    }).detach();
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
    if (!maintainDataStorage())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "ignore start recording while data storage is not ready"
            << " disk_root=" << options_.diskRoot
            << std::endl).str());
        setLedState(LedState::WaitStorage);
        return false;
    }
    if (isStereoStartupWaiting())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "ignore start recording while Fays stereo recorder is still starting"
            << std::endl).str());
        setLedState(LedState::Init);
        return false;
    }
    if (const std::optional<ugripper::runtime::HealthFault> fault = currentHealthFault())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "ignore start recording while hardware health fault is present"
            << " key=" << fault->key
            << std::endl).str());
        setHardwareFaultLedState(*fault);
        sendAudioCommand("error");
        return false;
    }
    cancelBackgroundTactileValidation();
    tactileTriggeredAudioCommand_.clear();
    activeHardwareFault_.reset();
    resetRestoreUsbErrorWindow();
    return recordingOrchestrator_->StartRecording(resetRecording, nullptr);
}

bool RecordRuntime::stopRecording(bool dueToError, const std::string &reason, const std::string &errorType)
{
    if (recordingOrchestrator_ == nullptr)
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString() << "recording orchestrator not initialized" << std::endl).str());
        return false;
    }
    const bool ok = recordingOrchestrator_->StopRecording(dueToError, reason, errorType, nullptr);
    const std::string completedEpisodeDir = lastEpisodeDir();
    if (!dueToError && activeHardwareFault_.has_value() && IsCameraRecorderMainCameraFault(*activeHardwareFault_))
    {
        maybeTriggerRestoreUsbForHardwareFault(*activeHardwareFault_);
    }
    if (ok && !dueToError && !completedEpisodeDir.empty())
    {
        scheduleBackgroundTactileValidation(completedEpisodeDir);
    }
    if (dueToError && activeHardwareFault_.has_value())
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

bool RecordRuntime::handlePhysicalRecordShortPress(ugripper::runtime::HmiEventType eventType)
{
    const uint64_t nowMs = currentSteadyMs();
    const char *eventName = hmiEventName(eventType);
    if (hasPendingPhysicalRecordShortPress_)
    {
        const uint64_t elapsedMs = nowMs >= pendingPhysicalRecordShortPressMs_
                                       ? nowMs - pendingPhysicalRecordShortPressMs_
                                       : 0;
        if (pendingPhysicalRecordShortPress_ == eventType && elapsedMs <= kPhysicalRecordDoubleClickMs)
        {
            hasPendingPhysicalRecordShortPress_ = false;
            pendingPhysicalRecordShortPressMs_ = 0;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "[HMI_DIAG] category=record_double_click"
                << " event=" << eventName
                << " result=accepted"
                << " elapsed_ms=" << elapsedMs
                << " window_ms=" << kPhysicalRecordDoubleClickMs).str());
            if (eventType == ugripper::runtime::HmiEventType::ShortUpPressed)
            {
                return handleShortUpAction();
            }
            return handleShortDownAction();
        }

        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "[HMI_DIAG] category=record_double_click"
            << " event=" << eventName
            << " result=cancel_previous"
            << " previous_event=" << hmiEventName(pendingPhysicalRecordShortPress_)
            << " elapsed_ms=" << elapsedMs
            << " window_ms=" << kPhysicalRecordDoubleClickMs).str());
    }

    hasPendingPhysicalRecordShortPress_ = true;
    pendingPhysicalRecordShortPress_ = eventType;
    pendingPhysicalRecordShortPressMs_ = nowMs;
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[HMI_DIAG] category=record_double_click"
        << " event=" << eventName
        << " result=armed"
        << " window_ms=" << kPhysicalRecordDoubleClickMs).str());
    return false;
}

void RecordRuntime::clearPendingPhysicalRecordShortPress(const char *reason, uint64_t nowMs)
{
    if (!hasPendingPhysicalRecordShortPress_)
    {
        return;
    }
    const uint64_t elapsedMs = nowMs >= pendingPhysicalRecordShortPressMs_
                                   ? nowMs - pendingPhysicalRecordShortPressMs_
                                   : 0;
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[HMI_DIAG] category=record_double_click"
        << " result=cleared"
        << " reason=" << (reason == nullptr ? "" : reason)
        << " pending_event=" << hmiEventName(pendingPhysicalRecordShortPress_)
        << " elapsed_ms=" << elapsedMs
        << " window_ms=" << kPhysicalRecordDoubleClickMs).str());
    hasPendingPhysicalRecordShortPress_ = false;
    pendingPhysicalRecordShortPressMs_ = 0;
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

    setLedState(LedState::Writing);
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

bool RecordRuntime::markEpisodeOperatorFailed(const std::string &episodeDir, std::string *errorMessage) const
{
    if (episodeDir.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "no completed episode available";
        }
        return false;
    }

    const fs::path episodePath(episodeDir);
    const fs::path metadataPath = episodePath / "metadata.json";
    std::ifstream input(metadataPath);
    if (!input.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "metadata.json not found: " + metadataPath.string();
        }
        return false;
    }

    ordered_json metadata;
    try
    {
        input >> metadata;
    }
    catch (const std::exception &ex)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = std::string("failed to parse metadata.json: ") + ex.what();
        }
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

    metadata["quality_check_status"] = "fail";
    metadata["quality_check_err_type"] = ugripper::runtime::kErrorTypeOperatorMarkedFailed;

    std::string writeError;
    if (!writeTextFileAtomically(metadataPath, metadata.dump(2) + "\n", &writeError))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to write metadata.json: " + writeError;
        }
        return false;
    }

    std::error_code flushError;
    if (!flushFileToDisk(metadataPath, &flushError))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to flush metadata.json: " + flushError.message();
        }
        return false;
    }

    writeValidationErrorLog(episodePath, "operator marked episode failed by left-hand single long press");
    if (!flushDirectoryToDisk(episodePath, &flushError))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "failed to flush episode directory after operator mark: " + flushError.message();
        }
        return false;
    }
    return true;
}

void RecordRuntime::playOperatorMarkFailedFeedback()
{
    for (int pulse = 0; pulse < 2; ++pulse)
    {
        panelManager_.setLedColor(kOperatorMarkLedRed, kOperatorMarkLedGreen, kOperatorMarkLedBlue);
        panelManager_.setBeepEnabledForSide("left", true);
        panelManager_.setBeepEnabledForSide("right", true);
        std::this_thread::sleep_for(std::chrono::milliseconds(kOperatorMarkBeepOnMs));
        panelManager_.turnOff();
        for (int attempt = 0; attempt < kOperatorMarkBeepSilenceRetryAttempts; ++attempt)
        {
            panelManager_.silenceBeepForSide("left");
            panelManager_.silenceBeepForSide("right");
            if (attempt + 1 < kOperatorMarkBeepSilenceRetryAttempts)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(kOperatorMarkBeepSilenceRetryIntervalMs));
            }
        }
        if (pulse == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kOperatorMarkBeepOffMs));
        }
    }
    applyIdleState();
}

bool RecordRuntime::handleLeftSingleLongMarkFailedAction()
{
    if (isRecordingActive())
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString() << "ignore operator fail mark while recording" << std::endl).str());
        sendAudioCommand("error");
        return false;
    }

    const std::string episodeDir = lastEpisodeDir();
    std::string errorMessage;
    if (!markEpisodeOperatorFailed(episodeDir, &errorMessage))
    {
        DM_LOG_ERROR("{}", (::DA::utils::LogString()
            << "failed to mark episode as operator failed"
            << " episode_dir=" << episodeDir
            << " error=" << errorMessage << std::endl).str());
        sendAudioCommand("error");
        return false;
    }

    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "episode marked as operator failed"
        << " episode_dir=" << episodeDir << std::endl).str());
    playOperatorMarkFailedFeedback();
    syncRuntimeLogToDisk("operator mark failed");
    return true;
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
            handlePhysicalRecordShortPress(event.type);
            break;
        case ugripper::runtime::HmiEventType::ShortDownPressed:
            handlePhysicalRecordShortPress(event.type);
            break;
        case ugripper::runtime::HmiEventType::LongUpPressed:
            clearPendingPhysicalRecordShortPress("long_up", currentSteadyMs());
            handleLongUpAction();
            break;
        case ugripper::runtime::HmiEventType::LongDownPressed:
            clearPendingPhysicalRecordShortPress("long_down", currentSteadyMs());
            handleLongDownAction();
            break;
        case ugripper::runtime::HmiEventType::ShutdownPromptRequested:
            clearPendingPhysicalRecordShortPress("shutdown_prompt", currentSteadyMs());
            DM_LOG_INFO("{}", (::DA::utils::LogString() << "dual-button chord armed" << std::endl).str());
            sendAudioCommand("shutdown");
            break;
        case ugripper::runtime::HmiEventType::ShutdownRequested:
            clearPendingPhysicalRecordShortPress("shutdown", currentSteadyMs());
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
            leftSinglePressedSinceMs_ = 0;
            leftSingleLongHandled_ = false;
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

    if (leftDualChordActive_)
    {
        if (!buttons.upPressed && !buttons.downPressed)
        {
            leftDualChordActive_ = false;
            leftBothPressedSinceMs_ = 0;
            leftDualLongHandled_ = false;
        }
        lastLeftButtons_ = buttons;
        return;
    }

    if (buttons.upPressed || buttons.downPressed)
    {
        const bool freshSinglePress =
            (!lastLeftButtons_.upPressed && !lastLeftButtons_.downPressed) ||
            buttons.upPressed != lastLeftButtons_.upPressed ||
            buttons.downPressed != lastLeftButtons_.downPressed;
        if (freshSinglePress)
        {
            leftSinglePressedSinceMs_ = nowMs;
            leftSingleLongHandled_ = false;
        }

        const uint64_t heldMs = leftSinglePressedSinceMs_ > 0 && nowMs >= leftSinglePressedSinceMs_
                                    ? nowMs - leftSinglePressedSinceMs_
                                    : 0;
        if (heldMs >= kLongPressThresholdMs && !leftSingleLongHandled_)
        {
            leftSingleLongHandled_ = true;
            handleLeftSingleLongMarkFailedAction();
        }
        lastLeftButtons_ = buttons;
        return;
    }

    leftSinglePressedSinceMs_ = 0;
    leftSingleLongHandled_ = false;
    lastLeftButtons_ = buttons;
}

bool RecordRuntime::initializeRecordControlPipe()
{
    if (options_.recordControlPipe.empty())
    {
        return false;
    }

    std::string errorMessage;
    if (!recordControlReader_.Open(options_.recordControlPipe, 0620, &errorMessage))
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "failed to open record control FIFO: " << options_.recordControlPipe
            << " error=" << errorMessage << std::endl).str());
        return false;
    }

    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "record control FIFO ready: " << options_.recordControlPipe << std::endl).str());
    return true;
}

void RecordRuntime::closeRecordControlPipe()
{
    recordControlReader_.Close();
}

bool RecordRuntime::pollRecordControlPipe()
{
    if (!recordControlReader_.IsOpen())
    {
        return false;
    }

    bool accepted = false;
    std::vector<std::string> commands;
    std::string errorMessage;
    if (!recordControlReader_.ReadAvailable(&commands, &errorMessage))
    {
        if (errorMessage.find("buffer overflow") != std::string::npos)
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "[CONTROL_DIAG] source=fifo result=ignored reason=buffer_overflow").str());
            return false;
        }
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "record control FIFO read failed: " << errorMessage << std::endl).str());
        closeRecordControlPipe();
        return false;
    }

    for (const std::string& command : commands)
    {
        if (processRecordControlCommand(command))
        {
            accepted = true;
        }
    }
    return accepted;
}

bool RecordRuntime::processRecordControlCommand(const std::string &rawCommand)
{
    std::string command = trim(rawCommand);
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (command.empty())
    {
        return false;
    }

    const bool recording = isRecordingActive();
    std::string eventName;
    std::string action;
    if (command == "SHORT_UP")
    {
        eventName = "ShortUpPressed";
        action = "short_up";
    }
    else if (command == "SHORT_DOWN")
    {
        eventName = "ShortDownPressed";
        action = "short_down";
    }
    else if (command == "START")
    {
        if (recording)
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "[CONTROL_DIAG] source=fifo command=" << command
                << " result=ignored reason=already_recording").str());
            return false;
        }
        eventName = "ShortUpPressed";
        action = "start";
    }
    else if (command == "STOP")
    {
        if (!recording)
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "[CONTROL_DIAG] source=fifo command=" << command
                << " result=ignored reason=not_recording").str());
            return false;
        }
        eventName = "ShortDownPressed";
        action = "stop";
    }
    else
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "[CONTROL_DIAG] source=fifo command=" << command
            << " result=ignored reason=unknown_command").str());
        return false;
    }

    const uint64_t nowMs = currentSteadyMs();
    if (lastRecordControlActionMs_ > 0 && nowMs - lastRecordControlActionMs_ < kRecordControlDebounceMs)
    {
        DM_LOG_WARN("{}", (::DA::utils::LogString()
            << "[CONTROL_DIAG] source=fifo command=" << command
            << " action=" << action
            << " result=ignored reason=debounce").str());
        return false;
    }

    lastRecordControlActionMs_ = nowMs;
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[CONTROL_DIAG] source=fifo command=" << command
        << " action=" << action
        << " result=accepted"
        << " mapped_event=" << eventName).str());
    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[HMI_DIAG] category=button_event"
        << " source=fifo"
        << " event=" << eventName
        << " up=0 down=0").str());

    if (eventName == "ShortUpPressed")
    {
        handleShortUpAction();
    }
    else
    {
        handleShortDownAction();
    }
    return true;
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
    if (!episodeManagerInitialized_)
    {
        setLedState(LedState::WaitStorage);
        return;
    }

    const auto previousHealthState = healthState_;
    const auto result = healthMonitor_->Poll(healthState_);
    healthState_ = result.state;
    if (!result.checked)
    {
        return;
    }

    auto handleHardwareFault =
        [this](const ugripper::runtime::HealthFault &fault, bool shouldNotifyFault) {
            activeHardwareFault_ = fault;
            if (isRecordingActive())
            {
                const std::string stopReason =
                    "recording hardware fault (" + fault.key + "): " + fault.detail;
                if (shouldNotifyFault)
                {
                    DM_LOG_ERROR("{}", (::DA::utils::LogString() << stopReason << std::endl).str());
                    DM_LOG_INFO("{}", (::DA::utils::LogString()
                        << "[HMI_DIAG] category=health_fault"
                        << " key=" << fault.key
                        << " led_state=" << ledStateName(toLedState(fault.led_state))
                        << " action=stop_recording").str());
                }
                const bool stopOk = stopRecording(true, stopReason, fault.key);
                const bool recordingStopped = !isRecordingActive();
                if (stopOk || recordingStopped || IsCameraRecorderMainCameraFault(fault))
                {
                    if (!stopOk && !recordingStopped)
                    {
                        DM_LOG_WARN("{}", (::DA::utils::LogString()
                            << "recording did not stop; continue restore usb for camera recorder main camera fault"
                            << " key=" << fault.key
                            << std::endl).str());
                    }
                    else if (!stopOk)
                    {
                        DM_LOG_WARN("{}", (::DA::utils::LogString()
                            << "recording stopped with failed episode; keep restore usb pending"
                            << " key=" << fault.key
                            << std::endl).str());
                    }
                    maybeTriggerRestoreUsbForHardwareFault(fault);
                }
                else
                {
                    DM_LOG_ERROR("{}", (::DA::utils::LogString()
                        << "skip restore usb on recording hardware fault: recording still active after stop"
                        << " key=" << fault.key
                        << std::endl).str());
                }
                return;
            }
            maybeTriggerRestoreUsbForHardwareFault(fault);
            if (shouldNotifyFault)
            {
                DM_LOG_ERROR("{}", (::DA::utils::LogString() << "hardware health fault (" << fault.key << "): "
                                      << fault.detail << std::endl).str());
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                    << "[HMI_DIAG] category=health_fault"
                    << " key=" << fault.key
                    << " led_state=" << ledStateName(toLedState(fault.led_state))).str());
                setHardwareFaultLedState(fault);
                sendAudioCommand("error");
            }
        };

    if (result.fault.has_value())
    {
        handleHardwareFault(*result.fault, result.should_notify_fault);
        return;
    }

    if (const auto cameraStartupFault = detectCameraRecorderStartupFailure())
    {
        const std::string errorKey = cameraStartupFault->key + "|" + cameraStartupFault->detail;
        const bool shouldNotifyFault =
            healthState_.status != ugripper::runtime::HealthStatus::Error ||
            healthState_.last_error_key != errorKey;
        healthState_.status = ugripper::runtime::HealthStatus::Error;
        healthState_.last_error_key = errorKey;
        healthState_.last_check_ms = currentSteadyMs();
        handleHardwareFault(*cameraStartupFault, shouldNotifyFault);
        return;
    }

    if (const auto cameraHangFault = detectCameraRecorderKernelHang())
    {
        const std::string errorKey = cameraHangFault->key + "|" + cameraHangFault->detail;
        const bool shouldNotifyFault =
            healthState_.status != ugripper::runtime::HealthStatus::Error ||
            healthState_.last_error_key != errorKey;
        healthState_.status = ugripper::runtime::HealthStatus::Error;
        healthState_.last_error_key = errorKey;
        healthState_.last_check_ms = currentSteadyMs();
        handleHardwareFault(*cameraHangFault, shouldNotifyFault);
        return;
    }

    const bool becameReadyAfterStartup =
        previousHealthState.status == ugripper::runtime::HealthStatus::Unknown &&
        healthState_.status == ugripper::runtime::HealthStatus::Ok;
    if (result.recovered || becameReadyAfterStartup)
    {
        if (!restoreUsbPendingTriggerKey_.empty())
        {
            const uint64_t nowMs = currentSteadyMs();
            const uint64_t ageMs = restoreUsbPendingTriggerFirstSeenMs_ == 0
                                       ? 0
                                       : nowMs - restoreUsbPendingTriggerFirstSeenMs_;
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "restore usb pending cleared"
                << " cause=" << restoreUsbPendingTriggerCause_
                << " evidence=" << restoreUsbPendingTriggerEvidence_
                << " side=" << restoreUsbPendingTriggerSide_
                << " age_ms=" << ageMs
                << std::endl).str());
            restoreUsbPendingTriggerKey_.clear();
            restoreUsbPendingTriggerCause_.clear();
            restoreUsbPendingTriggerEvidence_.clear();
            restoreUsbPendingTriggerSide_.clear();
            restoreUsbPendingTriggerFirstSeenMs_ = 0;
            restoreUsbPendingTriggerLastSeenMs_ = 0;
            restoreUsbPendingTriggerLogged_ = false;
        }
        activeHardwareFault_.reset();
        if (result.recovered)
        {
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
        }
        else
        {
            DM_LOG_INFO("{}", (::DA::utils::LogString()
                << "[HMI_DIAG] category=health_ready"
                << " recording=" << boolText(isRecordingActive())).str());
        }
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

void RecordRuntime::setTactileWarningLedState()
{
    if (!ledController_)
    {
        return;
    }

    const auto effectForSide = [](const TactileWarningSideState &state) -> GripperLedEffect {
        if (state.leftSensor && state.rightSensor)
        {
            return GripperLedEffect{GripperLedEffectState::TactileWarningBothSensors, 0.0};
        }
        if (state.leftSensor)
        {
            return GripperLedEffect{GripperLedEffectState::TactileWarningLeftSensor, 0.0};
        }
        if (state.rightSensor)
        {
            return GripperLedEffect{GripperLedEffectState::TactileWarningRightSensor, 0.0};
        }
        return GripperLedEffect{GripperLedEffectState::Ready, 0.0};
    };

    DM_LOG_INFO("{}", (::DA::utils::LogString()
        << "[HMI_DIAG] category=led_target"
        << " state=TactileWarning"
        << " left_l=" << boolText(tactileWarningSides_[0].leftSensor)
        << " left_r=" << boolText(tactileWarningSides_[0].rightSensor)
        << " right_l=" << boolText(tactileWarningSides_[1].leftSensor)
        << " right_r=" << boolText(tactileWarningSides_[1].rightSensor)).str());

    panelManager_.setLedEffectForSide("left", effectForSide(tactileWarningSides_[0]));
    panelManager_.setLedEffectForSide("right", effectForSide(tactileWarningSides_[1]));
}

void RecordRuntime::setHardwareFaultLedState(const ugripper::runtime::HealthFault &fault)
{
    const bool side_specific_error = fault.led_state == ugripper::runtime::RuntimeLedState::Error2 ||
                                     fault.led_state == ugripper::runtime::RuntimeLedState::Error4;
    if (!side_specific_error || !ledController_)
    {
        setLedState(toLedState(fault.led_state));
        return;
    }

    const GripperLedEffect faultEffect = makeLedEffect(toLedState(fault.led_state), 0.0);
    const GripperLedEffect unknownEffect{GripperLedEffectState::Error2Unknown, 0.0};
    switch (fault.side)
    {
    case ugripper::runtime::HardwareFaultSide::Left:
        panelManager_.setLedEffectForSide("left", faultEffect);
        panelManager_.setLedColorForSide("right", 255, 0, 0);
        break;
    case ugripper::runtime::HardwareFaultSide::Right:
        panelManager_.setLedColorForSide("left", 255, 0, 0);
        panelManager_.setLedEffectForSide("right", faultEffect);
        break;
    case ugripper::runtime::HardwareFaultSide::Both:
        panelManager_.setLedEffect(faultEffect);
        break;
    case ugripper::runtime::HardwareFaultSide::Unknown:
    default:
        panelManager_.setLedEffect(fault.led_state == ugripper::runtime::RuntimeLedState::Error2
                                       ? unknownEffect
                                       : faultEffect);
        break;
    }
}

void RecordRuntime::maybeTriggerRestoreUsbForHardwareFault(const ugripper::runtime::HealthFault &fault)
{
    if (IsDiskHealthFault(fault))
    {
        logRestoreUsbSkipOnce("disk_fault:" + fault.key,
                              "skip restore usb on data disk fault: " + fault.key);
        return;
    }

    if (fault.led_state != ugripper::runtime::RuntimeLedState::Error2 &&
        fault.led_state != ugripper::runtime::RuntimeLedState::Error4)
    {
        return;
    }

    const std::string reason = std::string("hardware_fault:") + fault.key;
    if (shouldDeferRestoreUsbForSide(fault.side, reason))
    {
        return;
    }
    RestoreUsbTriggerContext context;
    context.cause = fault.key;
    context.evidence = compactRestoreUsbEvidence(fault.detail);
    context.side = restoreUsbSideName(fault.side);
    if (!confirmRestoreUsbTrigger(context))
    {
        return;
    }
    triggerRestoreUsbOnError(reason, IsCameraRecorderMainCameraFault(fault));
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

    const int childFdLimit = ChildFileDescriptorLimit();
    const pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        CloseChildFileDescriptors(childFdLimit);
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
            if (!enabled && driver != nullptr &&
                sideForPort(driver->getPort()) == side)
            {
                driver->silenceBeep();
            }
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
        if (driver != nullptr)
        {
            bool silenced = false;
            for (int attempt = 1; attempt <= kBeepSilenceRetryLimit; ++attempt)
            {
                const uint64_t previousStateCount = driver->getSnapshot().beepStateCount;
                const bool commandOk = driver->silenceBeep();
                driver->requestState();

                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(kBeepSilenceConfirmMs);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    driver->pollOnce(10);
                    const auto snapshot = driver->getSnapshot();
                    if (snapshot.beepStateCount > previousStateCount)
                    {
                        silenced = snapshot.beepState.duty == 0 && snapshot.beepState.frequency == 0;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (silenced)
                {
                    break;
                }
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "failed to confirm hmi beep silence"
                    << " port=" << driver->getPort()
                    << " attempt=" << attempt
                    << " command_ok=" << boolText(commandOk)
                    << std::endl).str());
            }
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
                                              bool stereoEnabled,
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
      stereoEnabled_(stereoEnabled),
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

void RecordRuntime::EpisodeManager::setTactileCameraRuntimeStates(
    const std::array<TactileCameraRuntimeState, 4> &states)
{
    tactileCameraRuntimeStates_ = states;
}

const RecordRuntime::EpisodeManager::TactileCameraRuntimeState *
RecordRuntime::EpisodeManager::tactileCameraStateForName(const std::string &cameraName) const
{
    const auto it = std::find_if(
        tactileCameraRuntimeStates_.begin(),
        tactileCameraRuntimeStates_.end(),
        [&cameraName](const TactileCameraRuntimeState &state) {
            return state.cameraName == cameraName;
        });
    return it != tactileCameraRuntimeStates_.end() ? &(*it) : nullptr;
}

std::string RecordRuntime::EpisodeManager::cachedTactileSerialForName(const std::string &cameraName) const
{
    const auto *state = tactileCameraStateForName(cameraName);
    return state != nullptr ? state->serialNumber : std::string();
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

void RecordRuntime::EpisodeManager::markTactileReferencePendingForSide(
    const std::string &side,
    const std::string &reason)
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
    const uint64_t nowMs = RecordRuntime::currentEpochMs();
    for (const auto &target : kTactileCalibrationTargets)
    {
        if (side != target.side)
        {
            continue;
        }

        const auto *state = tactileCameraStateForName(target.cameraName);
        const std::string serial = state != nullptr ? state->serialNumber : std::string();
        if (serial.empty())
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString() << "tactile reference pending skipped: camera=" << target.cameraName
                                 << " reason="
                                 << (state == nullptr
                                         ? "missing tactile serial cache state"
                                         : (state->lastError.empty() ? "missing tactile serial cache" : state->lastError))
                                 << std::endl).str());
            continue;
        }

        json &cameraHistory = tactileHistory["cameras"][serial];
        if (!cameraHistory.is_object())
        {
            cameraHistory = json::object();
        }
        cameraHistory["camera_name"] = target.cameraName;
        cameraHistory["reference_pending"] = true;
        cameraHistory["reference_pending_reason"] = reason;
        cameraHistory["reference_pending_at_ms"] = nowMs;
        cameraHistory["recent"] = json::array();
        historyDirty = true;
        DM_LOG_INFO("{}", (::DA::utils::LogString()
            << "tactile reference pending: camera=" << target.cameraName
            << " side=" << side
            << " serial=" << serial
            << " reason=" << reason
            << std::endl).str());
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

namespace
{
bool setEpisodeValidationFailure(std::string *errorMessage,
                                 std::vector<std::string> *errorTypes,
                                 const std::string &errorType,
                                 const std::string &message)
{
    if (errorMessage != nullptr)
    {
        *errorMessage = message;
    }
    if (errorTypes != nullptr && !errorType.empty())
    {
        if (std::find(errorTypes->begin(), errorTypes->end(), errorType) == errorTypes->end())
        {
            errorTypes->push_back(errorType);
        }
    }
    return false;
}
}

bool RecordRuntime::EpisodeManager::validateEpisode(const std::string &episodeDir,
                                                    std::string *errorMessage,
                                                    std::vector<std::string> *errorTypes,
                                                    std::vector<TactileValidationFinding> *tactileFindings) const
{
    const int64_t validateStartMs = steadyNowMs();
    logPerf((::DA::utils::LogString() << "[PERF] validateEpisode begin: episode_dir=" << episodeDir).str());
    const auto artifacts = activeEpisodeVideoArtifacts(chestCameraEnabled_, stereoEnabled_);
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
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeMissingFile,
                "missing or empty video file: " + path);
        }
    }

    for (const auto &file : requiredFiles)
    {
        const std::string path = episodeDir + "/" + file;
        if (!RecordRuntime::fileExistsAndNotEmpty(path))
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeMissingFile,
                "missing or empty file: " + path);
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
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeCalibrationError,
                "main camera calibration cache invalid: camera=" + state.cameraName +
                    " device=" + state.devicePath + " missing");
        }
        if (!isYuzhouMainCameraSn(state.serialNumber))
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeCalibrationError,
                "main camera SN cache invalid: camera=" + state.cameraName +
                    " device=" + state.devicePath +
                    " detail=" + (state.lastError.empty() ? "empty SN" : state.lastError));
        }
        std::string calibrationDetail;
        if (!state.calibrationPayloadCached ||
            !validateYuzhouMainCameraCalibrationPayload(state.calibrationPayload, &calibrationDetail))
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeCalibrationError,
                "main camera calibration cache invalid: camera=" + state.cameraName +
                    " device=" + state.devicePath +
                    " detail=" + (!calibrationDetail.empty()
                                       ? calibrationDetail
                                       : (state.lastError.empty() ? "empty calibration" : state.lastError)));
        }
    }

    if (!commandExists("ffprobe"))
    {
        return setEpisodeValidationFailure(
            errorMessage,
            errorTypes,
            ugripper::runtime::kErrorTypeRuntimeError,
            "ffprobe is required for episode validation but was not found in PATH");
    }

    std::string metadataError;
    if (!validateMetadataVideoDetails(fs::path(episodeDir) / "metadata.json",
                                      artifacts,
                                      &metadataError))
    {
        return setEpisodeValidationFailure(
            errorMessage,
            errorTypes,
            ugripper::runtime::kErrorTypeMissingFile,
            "metadata validation failed: " + metadataError);
    }

    logPerf((::DA::utils::LogString() << "[PERF] validation setup done: episode_dir=" << episodeDir
             << " elapsed_ms=" << (steadyNowMs() - validateStartMs)).str());

    double referenceSpanSec = 0.0;
    const int64_t videoProbeStartMs = steadyNowMs();
    const auto probeTasks = probeVideoFilesParallel(episodeDir, artifacts);
    std::vector<VideoProbeResult> probes;
    std::map<std::string, double> effectiveSpanByFile;
    std::map<std::string, std::string> effectiveSpanSourceByFile;
    probes.reserve(probeTasks.size());
    for (const auto &task : probeTasks)
    {
        if (!task.ok)
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeMissingFile,
                "video unreadable or missing timing metadata: " + task.probe.fileName +
                    " (" + task.errorMessage + ")");
        }

        const EpisodeVideoArtifact artifact{task.probe.cameraName.c_str(), task.probe.fileName.c_str()};
        double effectiveSpanSec = task.probe.spanSec;
        std::string effectiveSpanSource;
        std::string effectiveSpanError;
        if (!loadEffectiveVideoDurationSec(
                episodeDir,
                artifact,
                task.probe,
                task.probe.spanSec,
                &effectiveSpanSec,
                &effectiveSpanSource,
                &effectiveSpanError))
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeFrameLoss,
                effectiveSpanError);
        }

        if (effectiveSpanSec < kMinReasonableVideoSpanSec)
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeCollectionDurationTooShort,
                "video effective span too short: " + task.probe.fileName +
                    " span=" + formatSeconds(effectiveSpanSec) +
                    "s, source=" + effectiveSpanSource +
                    ", expected >=" + formatSeconds(kMinReasonableVideoSpanSec) + "s");
        }

        referenceSpanSec = std::max(referenceSpanSec, effectiveSpanSec);
        effectiveSpanByFile[task.probe.fileName] = effectiveSpanSec;
        effectiveSpanSourceByFile[task.probe.fileName] = effectiveSpanSource;
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
        const auto effectiveIt = effectiveSpanByFile.find(probe.fileName);
        const double effectiveSpanSec =
            effectiveIt != effectiveSpanByFile.end() ? effectiveIt->second : probe.spanSec;
        const auto sourceIt = effectiveSpanSourceByFile.find(probe.fileName);
        const std::string effectiveSpanSource =
            sourceIt != effectiveSpanSourceByFile.end() ? sourceIt->second : "mkv_probe";
        const double gapSec = referenceSpanSec - effectiveSpanSec;
        if (gapSec > kMaxVideoSpanGapSec)
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeFrameLoss,
                "video span gap too large: " + probe.fileName +
                    " span=" + formatSeconds(effectiveSpanSec) +
                    "s, source=" + effectiveSpanSource +
                    ", reference=" + formatSeconds(referenceSpanSec) +
                    "s, gap=" + formatSeconds(gapSec) + "s");
        }
    }

    const int64_t tailCheckStartMs = steadyNowMs();
    std::vector<std::future<TailCheckTaskResult>> tailCheckFutures;
    tailCheckFutures.reserve(
        kEncoderTailCheckTargets.size() + (stereoEnabled_ ? kFaysTailCheckTargets.size() : 0));

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

    if (stereoEnabled_)
    {
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
                                     << ", camera_span_ms=" << (faysSummary.cameraSpanNs / 1000000.0)
                                     << ", first_camera_ns=" << faysSummary.firstCameraLogTimeNs
                                     << ", last_camera_ns=" << faysSummary.lastCameraLogTimeNs
                                     << ", last_imu_ns=" << faysSummary.lastImuLogTimeNs
                                     << ", lag_ms=" << (lagNs / 1000000.0)
                                     << "}").str();
                    return result;
                }));
        }
    }

    std::vector<std::string> tailDetails;
    tailDetails.reserve(tailCheckFutures.size());
    for (auto &future : tailCheckFutures)
    {
        TailCheckTaskResult result = future.get();
        if (!result.ok)
        {
            return setEpisodeValidationFailure(
                errorMessage,
                errorTypes,
                ugripper::runtime::kErrorTypeFrameLoss,
                result.errorMessage.empty() ? "tail check failed" : result.errorMessage);
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
    const std::string bootId = currentBootId();
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
        const std::string serial = cachedTactileSerialForName(target.cameraName);
        if (serial.empty())
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
            const json &cameraHistory = tactileHistory["cameras"][serial];
            if (cameraHistory.is_object() &&
                (cameraHistory.value("reference_pending", false) ||
                 cameraHistory.value("persistent_baseline_pending", false) ||
                 cameraHistory.value("persistent_baseline_refresh_on_next_boot", false)))
            {
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "tactile reference deferred: camera=" << target.cameraName
                    << " serial=" << serial
                    << " episode=" << episodeDir
                    << " reason=" << frameError
                    << std::endl).str());
            }
            continue;
        }

        json &cameraHistory = tactileHistory["cameras"][serial];
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
        cameraHistory["camera_name"] = target.cameraName;
        const uint64_t nowMs = RecordRuntime::currentEpochMs();
        cameraHistory["updated_at_ms"] = nowMs;

        std::vector<uint8_t> baselineFrame;
        std::string baselineError;
        const bool hasReferenceBaseline =
            readBinaryFileExact(tactileReferenceRawPath(tactileStateDir_, serial),
                                kTactileFrameBytes,
                                &baselineFrame,
                                &baselineError);
        const bool referencePending = cameraHistory.value("reference_pending", false);
        bool initializedReferenceFromEpisode = false;
        if (!hasReferenceBaseline || referencePending)
        {
            std::string writeError;
            if (writeBinaryFile(tactileReferenceRawPath(tactileStateDir_, serial),
                                *currentFrame,
                                &writeError) &&
                writeTactileReferenceMeta(tactileStateDir_,
                                          serial,
                                          target,
                                          RecordRuntime::currentEpochMs(),
                                          &writeError))
            {
                cameraHistory["reference_pending"] = false;
                cameraHistory["reference_pending_reason"] = "";
                cameraHistory["reference_pending_at_ms"] = 0;
                cameraHistory["reference_updated_at_ms"] = RecordRuntime::currentEpochMs();
                cameraHistory["recent"] = json::array();
                historyDirty = true;
                initializedReferenceFromEpisode = true;
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                    << "tactile reference initialized from episode: camera=" << target.cameraName
                    << " serial=" << serial
                    << " episode=" << episodeDir
                    << (referencePending ? " reason=pending" : " reason=missing")
                    << std::endl).str());
            }
            else
            {
                cameraHistory["reference_pending"] = true;
                cameraHistory["reference_pending_reason"] =
                    referencePending ? cameraHistory.value("reference_pending_reason", std::string("write_failed"))
                                     : std::string("missing_reference_write_failed");
                historyDirty = true;
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "tactile reference deferred: camera=" << target.cameraName
                    << " serial=" << serial
                    << " episode=" << episodeDir
                    << " reason=" << writeError
                    << std::endl).str());
            }
        }

        const bool persistentWarningActiveBefore =
            cameraHistory.value("persistent_fault_active", false);
        bool persistentWarningActiveAfter = persistentWarningActiveBefore;
        std::string persistentDetail =
            cameraHistory.value("persistent_fault_detail", std::string());

        bool persistentWarningTriggered = false;
        std::vector<uint8_t> persistentBaseline;
        std::string persistentBaselineError;
        const bool hasPersistentBaseline =
            readBinaryFileExact(tactilePersistentRawPath(tactileStateDir_, serial),
                                kTactileFrameBytes,
                                &persistentBaseline,
                                &persistentBaselineError);
        const uint64_t persistentBaselineUpdatedAtMs =
            cameraHistory.value("persistent_baseline_updated_at_ms", static_cast<uint64_t>(0));
        const bool legacyPersistentPending = cameraHistory.value("persistent_baseline_pending", false);
        const bool persistentRefreshOnNextBoot =
            cameraHistory.value("persistent_baseline_refresh_on_next_boot", false);
        const std::string persistentRefreshRequestBootId =
            cameraHistory.value("persistent_baseline_refresh_request_boot_id", std::string());
        const bool persistentRefreshAfterReboot =
            persistentRefreshOnNextBoot &&
            !persistentRefreshRequestBootId.empty() &&
            !bootId.empty() &&
            persistentRefreshRequestBootId != bootId;
        const bool persistentNeedsUpdate =
            !hasPersistentBaseline || persistentBaselineUpdatedAtMs == 0 ||
            persistentRefreshAfterReboot;
        const std::string persistentUpdateReason =
            (!hasPersistentBaseline || persistentBaselineUpdatedAtMs == 0)
                ? "missing"
                : (persistentRefreshAfterReboot ? "alarm_reboot" : "");
        if (legacyPersistentPending && !persistentNeedsUpdate)
        {
            cameraHistory["persistent_baseline_pending"] = false;
            cameraHistory["persistent_baseline_pending_reason"] = "";
            cameraHistory["persistent_baseline_pending_at_ms"] = 0;
            historyDirty = true;
        }
        bool initializedPersistentFromEpisode = false;
        if (persistentNeedsUpdate)
        {
            std::string writeError;
            if (writeBinaryFile(tactilePersistentRawPath(tactileStateDir_, serial),
                                *currentFrame,
                                &writeError))
            {
                cameraHistory["persistent_baseline_pending"] = false;
                cameraHistory["persistent_baseline_pending_reason"] = "";
                cameraHistory["persistent_baseline_pending_at_ms"] = 0;
                cameraHistory["persistent_baseline_updated_at_ms"] = nowMs;
                cameraHistory["persistent_baseline_refresh_on_next_boot"] = false;
                cameraHistory["persistent_baseline_refresh_request_boot_id"] = "";
                cameraHistory["persistent_baseline_refresh_request_at_ms"] = 0;
                cameraHistory["persistent_fault_active"] = false;
                cameraHistory["persistent_fault_detail"] = "";
                cameraHistory["persistent_recent"] = json::array();
                persistentWarningActiveAfter = false;
                persistentDetail.clear();
                historyDirty = true;
                initializedPersistentFromEpisode = true;
                DM_LOG_INFO("{}", (::DA::utils::LogString()
                    << "tactile persistent baseline initialized from episode: camera=" << target.cameraName
                    << " serial=" << serial
                    << " episode=" << episodeDir
                    << " reason=" << persistentUpdateReason
                    << std::endl).str());
            }
            else
            {
                cameraHistory["persistent_baseline_pending"] = true;
                cameraHistory["persistent_baseline_pending_reason"] = persistentUpdateReason + "_write_failed";
                cameraHistory["persistent_baseline_pending_at_ms"] = nowMs;
                historyDirty = true;
                DM_LOG_WARN("{}", (::DA::utils::LogString()
                    << "tactile persistent baseline deferred: camera=" << target.cameraName
                    << " serial=" << serial
                    << " episode=" << episodeDir
                    << " reason=" << writeError
                    << std::endl).str());
            }
        }

        if (!hasReferenceBaseline || referencePending || initializedReferenceFromEpisode)
        {
            continue;
        }

        const TactileFrameMetrics metrics = computeTactileFrameMetrics(baselineFrame, *currentFrame);
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
        historyDirty = true;

        bool warningActive = false;
        if (recent.size() >= kTactileHistoryWindow)
        {
            warningActive = std::all_of(
                recent.begin(),
                recent.end(),
                [](const json &entry) { return entry.is_boolean() && entry.get<bool>(); });
        }

        if (hasPersistentBaseline && !persistentNeedsUpdate && !initializedPersistentFromEpisode)
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
                cameraHistory["persistent_baseline_refresh_on_next_boot"] = !bootId.empty();
                cameraHistory["persistent_baseline_refresh_request_boot_id"] = bootId;
                cameraHistory["persistent_baseline_refresh_request_at_ms"] = nowMs;
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
        finding.serialNumber = serial;
        finding.side = target.side;
        finding.sensorSlot = target.sensorSlot;
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

std::vector<EpisodeVideoArtifact> metadataVideoArtifacts(bool chestCameraEnabled, bool stereoEnabled)
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
    if (!stereoEnabled)
    {
        artifacts.erase(
            std::remove_if(
                artifacts.begin(), artifacts.end(), [](const EpisodeVideoArtifact &artifact) {
                    return isStereoCameraName(artifact.cameraName);
                }),
            artifacts.end());
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
                                                       const std::string &qualityErrorType,
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
    hardwareList["tactile_right_l_sn"] = cachedTactileSerialForName("right_tcam_l");
    hardwareList["tactile_right_r_sn"] = cachedTactileSerialForName("right_tcam_r");
    hardwareList["tactile_left_l_sn"] = cachedTactileSerialForName("left_tcam_l");
    hardwareList["tactile_left_r_sn"] = cachedTactileSerialForName("left_tcam_r");

    json stereoStatus = json::object();
    if (stereoEnabled_)
    {
        loadJsonFile(stereoStatusFile_, &stereoStatus, &ignoredError);
    }
    hardwareList["stereo_right_sn"] =
        stereoEnabled_ ? jsonStringPath(stereoStatus, "cameras.right_stereo.serial_number") : "";
    hardwareList["stereo_left_sn"] =
        stereoEnabled_ ? jsonStringPath(stereoStatus, "cameras.left_stereo.serial_number") : "";

    json infoRoot = json::object();
    loadJsonFile(episodeTimingPath(episodePath).string(), &infoRoot, &ignoredError);
    std::map<std::string, int64_t> offsetByCamera;
    int64_t minOffsetUs = std::numeric_limits<int64_t>::max();
    for (const auto &artifact : metadataVideoArtifacts(chestCameraEnabled_, stereoEnabled_))
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
        const auto metadataArtifacts = metadataVideoArtifacts(chestCameraEnabled_, stereoEnabled_);
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
    for (const auto &artifact : metadataVideoArtifacts(chestCameraEnabled_, stereoEnabled_))
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

        double detailDurationSec = probe.durationSec;
        std::string durationSource;
        std::string durationError;
        if (!loadEffectiveVideoDurationSec(
                episodeDir,
                artifact,
                probe,
                probe.durationSec,
                &detailDurationSec,
                &durationSource,
                &durationError))
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                               << "failed to load effective video duration, falling back to mkv probe: "
                               << durationError << std::endl).str());
            detailDurationSec = probe.durationSec;
        }

        const auto offsetIt = offsetByCamera.find(artifact.cameraName);
        const int64_t startOffsetUs =
            offsetIt != offsetByCamera.end() && minOffsetUs > 0
                ? std::max<int64_t>(0, offsetIt->second - minOffsetUs)
                : 0;

        ordered_json detail = ordered_json::object();
        detail["name"] = artifact.fileName;
        detail["fps"] = roundToOneDecimal(nominalFpsForCamera(artifact.cameraName));
        detail["duration_s"] = roundToOneDecimal(detailDurationSec);
        detail["start_offset_us"] = startOffsetUs;
        videoDetails.push_back(std::move(detail));
        collectionDurationS = std::max(collectionDurationS, detailDurationSec);
    }

    ordered_json requiredFiles = ordered_json::array({
        "metadata.json",
        "calibration.json",
        "cam_left.mkv",
        "cam_right.mkv",
        "tcam_left_l.mkv",
        "tcam_left_r.mkv",
        "tcam_right_l.mkv",
        "tcam_right_r.mkv",
        "sensor_left.mcap",
        "sensor_right.mcap",
    });
    if (stereoEnabled_)
    {
        requiredFiles.push_back("stereo_left.mkv");
        requiredFiles.push_back("stereo_right.mkv");
        requiredFiles.push_back("fays_data_left.mcap");
        requiredFiles.push_back("fays_data_right.mcap");
    }
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
    metadata["quality_check_err_type"] =
        qualityOk ? "" : (qualityErrorType.empty() ? ugripper::runtime::kErrorTypeUnknown : qualityErrorType);
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
    if (stereoEnabled_)
    {
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
    }

    for (const auto &target : kTactileCalibrationTargets)
    {
        const auto *state = tactileCameraStateForName(target.cameraName);
        const std::string serial = state != nullptr ? state->serialNumber : std::string();
        if (serial.empty())
        {
            DM_LOG_WARN("{}", (::DA::utils::LogString()
                << "tactile serial cache unavailable for episode calibration: camera=" << target.cameraName
                << " device=" << target.devicePath
                << " detail=" << (state == nullptr
                                      ? "missing tactile serial cache state"
                                      : (state->lastError.empty() ? "empty tactile serial cache" : state->lastError))
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
