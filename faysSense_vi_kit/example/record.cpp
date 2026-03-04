#include <string>
#include <thread>
#include <memory>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <cstddef>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <mcap/writer.hpp>
#include <limits.h>
#include "fays_atrak/fays_atrak_types.h"
#include "fays_atrak/fays_vikit.h"
#include "common/print_helpers.h"

namespace {
std::string TrimCopy(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string ReadCameraCodecFromEnvironmentFile() {
    const std::string defaultCodec = "h264";
    std::ifstream envFile("/etc/environment");
    if (!envFile.is_open()) {
        return defaultCodec;
    }

    std::string line;
    while (std::getline(envFile, line)) {
        line = TrimCopy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = ToLowerCopy(TrimCopy(line.substr(0, pos)));
        if (key != "camera_codec") {
            continue;
        }

        std::string value = TrimCopy(line.substr(pos + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        value = ToLowerCopy(TrimCopy(value));

        if (value == "h264" || value == "h265") {
            return value;
        }

        std::cerr << "[FFmpeg] WARNING: invalid CAMERA_CODEC='" << value
                  << "' in /etc/environment, fallback to h264" << std::endl;
        return defaultCodec;
    }

    return defaultCodec;
}
}  // namespace

struct FaysImuSample {
    double gx;
    double gy;
    double gz;
    double ax;
    double ay;
    double az;
};

struct FaysCamTsSample {
    uint32_t frameIndex;
};

static_assert(sizeof(FaysImuSample) == 48, "FaysImuSample layout changed");
static_assert(sizeof(FaysCamTsSample) == 4, "FaysCamTsSample layout changed");

struct ImuQueuedSample {
    AtrakIMU imu;
    uint64_t publishTimeNs;
    uint64_t sessionId;
};

struct CamTsQueuedSample {
    uint64_t faysTsNs;
    uint64_t publishTimeNs;
    uint32_t frameIndex;
    uint64_t sessionId;
};

struct McapControlCommand {
    enum class Type {
        Start,
        Stop,
    };

    Type type;
    uint64_t sessionId;
    std::string mcapPath;
};

class ImuQueue {
public:
    ImuQueue() : stopped_(false), nextBacklogWarnSize_(8192) {}

    bool TryPushBatch(std::deque<ImuQueuedSample>& pending) {
        if (pending.empty()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(mtx_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }

        queue_.insert(
            queue_.end(),
            std::make_move_iterator(pending.begin()),
            std::make_move_iterator(pending.end()));
        pending.clear();

        if (queue_.size() >= nextBacklogWarnSize_) {
            std::cerr << "[ImuQueue] WARNING: backlog grew to " << queue_.size() << std::endl;
            nextBacklogWarnSize_ = queue_.size() + 4096;
        }
        cv_.notify_one();
        return true;
    }

    size_t DrainTo(std::vector<ImuQueuedSample>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        return DrainToLocked(out, maxCount);
    }

    size_t TryDrainTo(std::vector<ImuQueuedSample>& out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mtx_);
        return DrainToLocked(out, maxCount);
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    size_t DrainToLocked(std::vector<ImuQueuedSample>& out, size_t maxCount) {
        size_t count = queue_.size();
        if (count > maxCount) {
            count = maxCount;
        }
        if (count == 0) {
            return 0;
        }

        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(queue_.front());
            queue_.pop_front();
        }
        return count;
    }

    std::deque<ImuQueuedSample> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    size_t nextBacklogWarnSize_;
};

class CamTsQueue {
public:
    CamTsQueue() : stopped_(false), nextBacklogWarnSize_(2048) {}

    bool TryPushBatch(std::deque<CamTsQueuedSample>& pending) {
        if (pending.empty()) {
            return true;
        }
        std::unique_lock<std::mutex> lock(mtx_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }

        queue_.insert(
            queue_.end(),
            std::make_move_iterator(pending.begin()),
            std::make_move_iterator(pending.end()));
        pending.clear();

        if (queue_.size() >= nextBacklogWarnSize_) {
            std::cerr << "[CamTsQueue] WARNING: backlog grew to " << queue_.size() << std::endl;
            nextBacklogWarnSize_ = queue_.size() + 1024;
        }
        cv_.notify_one();
        return true;
    }

    size_t DrainTo(std::vector<CamTsQueuedSample>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        return DrainToLocked(out, maxCount);
    }

    size_t TryDrainTo(std::vector<CamTsQueuedSample>& out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mtx_);
        return DrainToLocked(out, maxCount);
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    size_t DrainToLocked(std::vector<CamTsQueuedSample>& out, size_t maxCount) {
        size_t count = queue_.size();
        if (count > maxCount) {
            count = maxCount;
        }
        if (count == 0) {
            return 0;
        }

        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(queue_.front());
            queue_.pop_front();
        }
        return count;
    }

    std::deque<CamTsQueuedSample> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    size_t nextBacklogWarnSize_;
};

class McapControlQueue {
public:
    McapControlQueue() : stopped_(false) {}

    void PushStart(uint64_t sessionId, const std::string& mcapPath) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            McapControlCommand cmd{};
            cmd.type = McapControlCommand::Type::Start;
            cmd.sessionId = sessionId;
            cmd.mcapPath = mcapPath;
            queue_.push_back(std::move(cmd));
        }
        cv_.notify_one();
    }

    void PushStop(uint64_t sessionId) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            McapControlCommand cmd{};
            cmd.type = McapControlCommand::Type::Stop;
            cmd.sessionId = sessionId;
            queue_.push_back(std::move(cmd));
        }
        cv_.notify_one();
    }

    size_t DrainTo(std::vector<McapControlCommand>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        return DrainToLocked(out, maxCount);
    }

    size_t TryDrainTo(std::vector<McapControlCommand>& out, size_t maxCount) {
        std::lock_guard<std::mutex> lock(mtx_);
        return DrainToLocked(out, maxCount);
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    size_t DrainToLocked(std::vector<McapControlCommand>& out, size_t maxCount) {
        size_t count = queue_.size();
        if (count > maxCount) {
            count = maxCount;
        }
        if (count == 0) {
            return 0;
        }

        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return count;
    }

    std::deque<McapControlCommand> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
};

class FaysDataLogger {
public:
    FaysDataLogger() : isOpen_(false), imuSeq_(0), camSeq_(0) {}

    ~FaysDataLogger() {
        Close();
    }

    bool Open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx_);

        if (isOpen_) {
            writer_.close();
            isOpen_ = false;
        }

        mcap::McapWriterOptions options("fays_recorder");
        options.noChunking = false;
        options.chunkSize = 256 * 1024;
        options.compression = mcap::Compression::Lz4;
        options.compressionLevel = mcap::CompressionLevel::Default;
        options.forceCompression = false;
        options.noRepeatedSchemas = true;
        options.noRepeatedChannels = true;
        options.noMessageIndex = true;

        auto openStatus = writer_.open(path, options);
        if (!openStatus.ok()) {
            std::cerr << "[MCAP] Failed to open " << path << ": " << openStatus.message << std::endl;
            return false;
        }

        auto imuSchema = mcap::Schema(
            "fays.Imu", "jsonschema", R"({
                "type": "object",
                "title": "FaysImuBinary",
                "description": "little-endian float64[6]: gx,gy,gz,ax,ay,az"
            })");
        auto camSchema = mcap::Schema(
            "fays.CamTs", "jsonschema", R"({
                "type": "object",
                "title": "FaysCamTsBinary",
                "description": "little-endian uint32 frameIndex"
            })");

        writer_.addSchema(imuSchema);
        writer_.addSchema(camSchema);

        auto imuChannel = mcap::Channel("i", "binary", imuSchema.id);
        auto camChannel = mcap::Channel("c", "binary", camSchema.id);

        writer_.addChannel(imuChannel);
        writer_.addChannel(camChannel);

        imuChannelId_ = imuChannel.id;
        camChannelId_ = camChannel.id;
        imuSeq_ = 0;
        camSeq_ = 0;
        isOpen_ = true;
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (isOpen_) {
            writer_.close();
            isOpen_ = false;
        }
    }

    void LogImu(const AtrakIMU& imuData, uint64_t publishTimeNs) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!isOpen_) {
            return;
        }

        FaysImuSample sample{};
        sample.gx = imuData.gyro[0];
        sample.gy = imuData.gyro[1];
        sample.gz = imuData.gyro[2];
        sample.ax = imuData.acc[0];
        sample.ay = imuData.acc[1];
        sample.az = imuData.acc[2];

        mcap::Message msg;
        msg.channelId = imuChannelId_;
        msg.sequence = imuSeq_++;
        msg.logTime = imuData.timestamp;
        msg.publishTime = publishTimeNs;
        msg.data = reinterpret_cast<const std::byte*>(&sample);
        msg.dataSize = sizeof(sample);

        auto writeStatus = writer_.write(msg);
        if (!writeStatus.ok()) {
            std::cerr << "[MCAP] Failed to write IMU frame: " << writeStatus.message << std::endl;
        }
    }

    void LogCamTs(uint64_t faysTsNs, uint64_t publishTimeNs, uint32_t frameIndex) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!isOpen_) {
            return;
        }

        FaysCamTsSample sample{};
        sample.frameIndex = frameIndex;

        mcap::Message msg;
        msg.channelId = camChannelId_;
        msg.sequence = camSeq_++;
        msg.logTime = faysTsNs;
        msg.publishTime = publishTimeNs;
        msg.data = reinterpret_cast<const std::byte*>(&sample);
        msg.dataSize = sizeof(sample);

        auto writeStatus = writer_.write(msg);
        if (!writeStatus.ok()) {
            std::cerr << "[MCAP] Failed to write camera timestamp: " << writeStatus.message << std::endl;
        }
    }

private:
    mcap::McapWriter writer_;
    bool isOpen_;
    mcap::ChannelId imuChannelId_;
    mcap::ChannelId camChannelId_;
    uint32_t imuSeq_;
    uint32_t camSeq_;
    std::mutex mtx_;
};

struct VideoFrame {
    cv::Mat image;
    uint64_t faysTsNs;
    uint64_t publishTimeNs;
};

class VideoFrameQueue {
public:
    static constexpr size_t kDefaultCapacity = 8;

    explicit VideoFrameQueue(size_t capacity = kDefaultCapacity)
        : capacity_(capacity), stopped_(false), dropCount_(0) {}

    bool Push(VideoFrame&& frame) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stopped_) return false;
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            const uint64_t count = dropCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((count & (count - 1)) == 0 || count % 50 == 0) {
                std::cerr << "[Video] Encode queue full, dropped oldest frame (total drops: "
                          << count << ")" << std::endl;
            }
        }
        queue_.push_back(std::move(frame));
        cv_.notify_one();
        return true;
    }

    bool Pop(VideoFrame& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mtx_);
        stopped_ = true;
        cv_.notify_all();
    }

    uint64_t GetDropCount() const { return dropCount_.load(std::memory_order_relaxed); }

private:
    const size_t capacity_;
    std::deque<VideoFrame> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    std::atomic<uint64_t> dropCount_;
};

class FFmpegRecorder {
public:
    FFmpegRecorder() : pipe_(nullptr) {}

    ~FFmpegRecorder() { Stop(); }

    bool Start(const std::string& savePath, int width, int height, int fps) {
        if (pipe_) {
            return true;
        }

        const std::string cameraCodec = ReadCameraCodecFromEnvironmentFile();
        const std::string ffmpegVideoEncoder = (cameraCodec == "h265") ? "hevc_rkmpp" : "h264_rkmpp";

        std::stringstream cmd;
        // Keep ffmpeg output minimal: suppress banner/progress spam in system logs.
        cmd << "ffmpeg -hide_banner -loglevel error -nostats -y ";

        cmd << "-thread_queue_size 512 "
            << "-f rawvideo -vcodec rawvideo "
            << "-pix_fmt bgr24 "
            << "-s " << width << "x" << height << " "
            << "-r " << fps << " "
            << "-i - ";

        cmd << "-c:v " << ffmpegVideoEncoder << " "
            << "-rc_mode CQP "
            << "-qp_init 30 "
            << "-qp_max 38 "
            << "-qp_min 24 "
            << "-qp_max_i 38 "
            << "-qp_min_i 20 ";

        cmd << "\"" << savePath << "\"";

        std::cout << "[FFmpeg] CAMERA_CODEC=" << cameraCodec
                  << " -> " << ffmpegVideoEncoder << std::endl;
        std::cout << "[FFmpeg] Command: " << cmd.str() << std::endl;

        pipe_ = popen(cmd.str().c_str(), "w");
        if (!pipe_) {
            std::cerr << "[FFmpeg] Failed to open pipe!" << std::endl;
            return false;
        }
        const int fd = fileno(pipe_);
        if (fd >= 0) {
            constexpr int kTargetPipeSz = 1048576;
            const int actual = fcntl(fd, F_SETPIPE_SZ, kTargetPipeSz);
            if (actual > 0) {
                std::cout << "[FFmpeg] Pipe buffer expanded to " << actual << " bytes" << std::endl;
            }
        }
        return true;
    }

    void Write(const cv::Mat& frame) {
        if (!pipe_ || frame.empty()) {
            return;
        }
        fwrite(frame.data, 1, frame.total() * frame.elemSize(), pipe_);
    }

    void Stop() {
        if (pipe_) {
            pclose(pipe_);
            pipe_ = nullptr;
            std::cout << "[FFmpeg] Recording stopped." << std::endl;
        }
    }

private:
    FILE* pipe_;
};

class FaysRecorder;
static FaysRecorder* g_recorder = nullptr;
void signalHandler(int signal);

class FaysRecorder {
public:
    explicit FaysRecorder(const char* configPath)
        : mptrHandle_(nullptr),
          mbIsRunning_(true),
          recordingEnabled_(false),
          recordingSessionId_(0),
          recordingSessionSeed_(0),
          lastImuTimestamp_(0),
          lastImgTimestamp_(0),
          imuGapCount_(0),
          imuRollbackCount_(0),
          imuToSysOffsetNs_(0),
          hasImuTimeOffset_(false) {
        mImgData_.data = new uchar[FAYS_ATRAK_MONO_MAX_BYTES * 3];

        FAYS_VIK_CreateHandleWithConfig(&mptrHandle_, configPath);
        PrintDeviceInfo(mptrHandle_);
        PrintCalibrationInfo(mptrHandle_);

        LoadMonitoredDevicePaths(configPath);
        const std::string fpsStr = ReadConfigValue(configPath, "stereo_fps");
        recordFps_ = fpsStr.empty() ? 25 : std::stoi(fpsStr);
        std::cout << "[FaysRecorder] Created handle with config: " << configPath << std::endl;
        std::cout << "[FaysRecorder] Standby mode ready. Waiting for START command." << std::endl;

        mptrImuThr_ = std::thread(&FaysRecorder::ImuOnlineCapture, this);
        mptrMcapWriterThr_ = std::thread(&FaysRecorder::McapWriteThread, this);
        mptrImgThr_ = std::thread(&FaysRecorder::ImgOnlineCapture, this);
        mptrEncThr_ = std::thread(&FaysRecorder::VideoEncodeThread, this);
        mptrUsbWatchThr_ = std::thread(&FaysRecorder::UsbConnectionWatchdog, this);
    }

    ~FaysRecorder() {
        StopRecordingSession();
        mbIsRunning_ = false;

        if (mptrImuThr_.joinable()) {
            mptrImuThr_.join();
        }
        if (mptrImgThr_.joinable()) {
            mptrImgThr_.join();
        }

        videoFrameQueue_.Stop();
        if (mptrEncThr_.joinable()) {
            mptrEncThr_.join();
        }

        if (mptrUsbWatchThr_.joinable()) {
            mptrUsbWatchThr_.join();
        }

        imuQueue_.NotifyStop();
        camTsQueue_.NotifyStop();
        mcapControlQueue_.NotifyStop();

        if (mptrMcapWriterThr_.joinable()) {
            mptrMcapWriterThr_.join();
        }

        mRecorder_.Stop();

        std::cout << "[IMU] Final stats - Gaps: " << imuGapCount_
                  << ", Rollbacks: " << imuRollbackCount_ << std::endl;
        std::cout << "[Video] Final stats - Encode queue drops: "
                  << videoFrameQueue_.GetDropCount() << std::endl;

        FAYS_VIK_DestroyHandle(mptrHandle_);
        std::cout << "[FaysRecorder] Destroyed handle" << std::endl;
        delete[] mImgData_.data;
        std::cout << "[FaysRecorder] Deleted image data" << std::endl;
    }

    bool IsRunning() const { return mbIsRunning_; }

    void Stop() {
        mbIsRunning_ = false;
        StopRecordingSession();
    }

    void StartRecording(const std::string& outputDir) {
        std::lock_guard<std::mutex> lock(recordingMtx_);
        const std::string normalizedOutputDir = NormalizeOutputDir(outputDir);
        const std::string mcapPath = normalizedOutputDir + "fays_data.mcap";

        recordingOutputDir_ = normalizedOutputDir;
        const uint64_t sessionId = recordingSessionSeed_.fetch_add(1, std::memory_order_relaxed) + 1;
        recordingSessionId_.store(sessionId, std::memory_order_release);
        recordingEnabled_.store(true, std::memory_order_release);
        hasImuTimeOffset_.store(false, std::memory_order_release);
        mcapControlQueue_.PushStart(sessionId, mcapPath);

        std::cout << "[Control] START recording. Output directory: " << recordingOutputDir_ << std::endl;
        std::cout << "[Control] MCAP output: " << mcapPath << std::endl;
    }

    void StopRecordingSession() {
        bool wasRecording = recordingEnabled_.exchange(false, std::memory_order_acq_rel);
        const uint64_t stoppedSessionId = recordingSessionId_.exchange(0, std::memory_order_acq_rel);
        if (wasRecording) {
            std::cout << "[Control] STOP recording." << std::endl;
        }
        mcapControlQueue_.PushStop(stoppedSessionId);
    }

private:
    struct UsbWatchdogStatus {
        size_t consecutiveFailures = 0;
        std::chrono::steady_clock::time_point firstFailureTs;
        std::string lastFailureSignature;
    };

    static constexpr int kUsbWatchdogPollIntervalMs = 100;
    static constexpr int kUsbDisconnectDebounceMs = 500;

    static std::string TrimCopy(const std::string& input) {
        const std::string whitespace = " \t\r\n";
        const size_t start = input.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const size_t end = input.find_last_not_of(whitespace);
        return input.substr(start, end - start + 1);
    }

    static std::string ReadConfigValue(const std::string& configPath, const std::string& key) {
        std::ifstream in(configPath);
        if (!in.is_open()) {
            std::cerr << "[USB] Failed to open config file for USB watch: " << configPath << std::endl;
            return "";
        }

        const std::string prefix = key + ":";
        std::string line;
        while (std::getline(in, line)) {
            const size_t commentPos = line.find('#');
            if (commentPos != std::string::npos) {
                line = line.substr(0, commentPos);
            }

            line = TrimCopy(line);
            if (line.rfind(prefix, 0) != 0) {
                continue;
            }

            std::string value = TrimCopy(line.substr(prefix.size()));
            if (value == "NULL" || value == "null") {
                return "";
            }
            return value;
        }

        return "";
    }

    static std::string ErrnoToString(int err) {
        if (err == 0) {
            return "0";
        }
        std::ostringstream oss;
        oss << err << "(" << std::strerror(err) << ")";
        return oss.str();
    }

    static std::string ReadSymlinkTarget(const std::string& path, int* outErrno = nullptr) {
        char linkTarget[PATH_MAX];
        errno = 0;
        const ssize_t len = readlink(path.c_str(), linkTarget, sizeof(linkTarget) - 1);
        if (len < 0) {
            if (outErrno != nullptr) {
                *outErrno = errno;
            }
            return "";
        }

        linkTarget[len] = '\0';
        if (outErrno != nullptr) {
            *outErrno = 0;
        }
        return std::string(linkTarget);
    }

    static bool ResolveDevicePath(const std::string& path, std::string* outResolved, int* outErrno = nullptr) {
        char resolvedPath[PATH_MAX];
        errno = 0;
        if (realpath(path.c_str(), resolvedPath) == nullptr) {
            if (outResolved != nullptr) {
                outResolved->clear();
            }
            if (outErrno != nullptr) {
                *outErrno = errno;
            }
            return false;
        }
        if (outResolved != nullptr) {
            *outResolved = std::string(resolvedPath);
        }
        if (outErrno != nullptr) {
            *outErrno = 0;
        }
        return true;
    }

    void LoadMonitoredDevicePaths(const std::string& configPath) {
        monitoredDevicePaths_.clear();
        monitoredResolvedPaths_.clear();
        monitoredWatchdogStatus_.clear();

        const std::string stereoPort = ReadConfigValue(configPath, "stereo_dev_port");
        const std::string imuPort = ReadConfigValue(configPath, "imu_dev_port");

        if (!stereoPort.empty()) {
            monitoredDevicePaths_.push_back(stereoPort);
        }
        if (!imuPort.empty() && imuPort != stereoPort) {
            monitoredDevicePaths_.push_back(imuPort);
        }

        if (monitoredDevicePaths_.empty()) {
            std::cerr << "[USB] No Fays device nodes parsed from config. USB disconnection watch disabled." << std::endl;
            return;
        }

        for (const auto& devPath : monitoredDevicePaths_) {
            int resolveErrno = 0;
            int linkErrno = 0;
            std::string resolved;
            const bool resolvedOk = ResolveDevicePath(devPath, &resolved, &resolveErrno);
            const std::string symlinkTarget = ReadSymlinkTarget(devPath, &linkErrno);
            monitoredResolvedPaths_.push_back(resolvedOk ? resolved : "");
            monitoredWatchdogStatus_.emplace_back();

            if (!resolvedOk) {
                std::cerr << "[USB] Watching device node: " << devPath
                          << " (target unresolved at startup, realpath_errno=" << ErrnoToString(resolveErrno)
                          << ", readlink_target="
                          << (symlinkTarget.empty() ? "<unavailable>" : symlinkTarget)
                          << ", readlink_errno=" << ErrnoToString(linkErrno) << ")"
                          << std::endl;
                continue;
            }

            std::cout << "[USB] Watching device node: " << devPath
                      << " -> " << resolved << std::endl;
        }
    }

    void UsbConnectionWatchdog() {
        while (mbIsRunning_) {
            for (size_t idx = 0; idx < monitoredDevicePaths_.size(); ++idx) {
                const std::string& devPath = monitoredDevicePaths_[idx];
                if (devPath.empty()) {
                    continue;
                }

                if (idx >= monitoredResolvedPaths_.size()) {
                    monitoredResolvedPaths_.resize(idx + 1);
                }
                if (idx >= monitoredWatchdogStatus_.size()) {
                    monitoredWatchdogStatus_.resize(idx + 1);
                }
                std::string& baseline = monitoredResolvedPaths_[idx];
                UsbWatchdogStatus& status = monitoredWatchdogStatus_[idx];

                int accessErrno = 0;
                int resolveErrno = 0;
                int linkErrno = 0;
                std::string resolved;
                std::string failureType;
                std::string failureSignature;

                const std::string symlinkTarget = ReadSymlinkTarget(devPath, &linkErrno);

                errno = 0;
                if (access(devPath.c_str(), F_OK) != 0) {
                    accessErrno = errno;
                    failureType = "missing_node";
                } else if (!ResolveDevicePath(devPath, &resolved, &resolveErrno)) {
                    failureType = "unresolved_target";
                } else if (!baseline.empty() && resolved != baseline) {
                    failureType = "target_remapped";
                }

                if (failureType.empty()) {
                    if (baseline.empty()) {
                        baseline = resolved;
                    }

                    if (status.consecutiveFailures > 0) {
                        const auto unstableMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - status.firstFailureTs).count();
                        std::cout << "[USB] Watchdog recovered: path=" << devPath
                                  << ", resolved=" << resolved
                                  << ", unstable_ms=" << unstableMs
                                  << ", previous_consecutive_failures=" << status.consecutiveFailures
                                  << std::endl;
                    }

                    status.consecutiveFailures = 0;
                    status.lastFailureSignature.clear();
                    continue;
                }

                failureSignature = failureType + "|" + baseline + "|" + resolved + "|" +
                                   ErrnoToString(accessErrno) + "|" + ErrnoToString(resolveErrno) +
                                   "|" + ErrnoToString(linkErrno) + "|" + symlinkTarget;

                const auto now = std::chrono::steady_clock::now();
                if (status.consecutiveFailures == 0) {
                    status.firstFailureTs = now;
                }
                status.consecutiveFailures++;

                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - status.firstFailureTs).count();
                const bool signatureChanged = (failureSignature != status.lastFailureSignature);
                if (signatureChanged || status.consecutiveFailures == 1) {
                    uint64_t sessionId = 0;
                    const bool isRecording = GetRecordingState(nullptr, &sessionId);
                    std::cerr << "[USB] Watchdog anomaly: type=" << failureType
                              << ", path=" << devPath
                              << ", baseline_target=" << (baseline.empty() ? "<unset>" : baseline)
                              << ", current_target=" << (resolved.empty() ? "<unresolved>" : resolved)
                              << ", symlink_target=" << (symlinkTarget.empty() ? "<unavailable>" : symlinkTarget)
                              << ", access_errno=" << ErrnoToString(accessErrno)
                              << ", realpath_errno=" << ErrnoToString(resolveErrno)
                              << ", readlink_errno=" << ErrnoToString(linkErrno)
                              << ", recording=" << (isRecording ? "1" : "0")
                              << ", session_id=" << sessionId
                              << ", consecutive_failures=" << status.consecutiveFailures
                              << ", elapsed_ms=" << elapsedMs
                              << ", debounce_ms=" << kUsbDisconnectDebounceMs
                              << std::endl;
                    status.lastFailureSignature = failureSignature;
                }

                if (elapsedMs >= kUsbDisconnectDebounceMs) {
                    std::cerr << "[USB] Fays USB disconnect confirmed after debounce: path=" << devPath
                              << ", failure_type=" << failureType
                              << ", elapsed_ms=" << elapsedMs
                              << ", consecutive_failures=" << status.consecutiveFailures
                              << ". Exiting fays_record_example." << std::endl;
                    Stop();
                    std::fflush(nullptr);
                    std::exit(2);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kUsbWatchdogPollIntervalMs));
        }
    }

    static std::string NormalizeOutputDir(const std::string& outputDir) {
        std::string normalized = outputDir;
        if (normalized.empty()) {
            normalized = ".";
        }
        if (!normalized.empty() && normalized.back() != '/') {
            normalized += '/';
        }
        return normalized;
    }

    static uint64_t SystemNowNs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
    }

    void UpdateImuTimeOffset(uint64_t imuTsNs, uint64_t systemTsNs) {
        const int64_t offsetNs = static_cast<int64_t>(systemTsNs) - static_cast<int64_t>(imuTsNs);
        imuToSysOffsetNs_.store(offsetNs, std::memory_order_relaxed);
        hasImuTimeOffset_.store(true, std::memory_order_release);
    }

    uint64_t AlignFaysTsToSystem(uint64_t faysTsNs) const {
        if (!hasImuTimeOffset_.load(std::memory_order_acquire)) {
            return SystemNowNs();
        }
        const int64_t offsetNs = imuToSysOffsetNs_.load(std::memory_order_relaxed);
        const int64_t aligned = static_cast<int64_t>(faysTsNs) + offsetNs;
        return aligned > 0 ? static_cast<uint64_t>(aligned) : 0ULL;
    }

    bool GetRecordingState(std::string* outDir = nullptr, uint64_t* outSessionId = nullptr) const {
        const bool enabled = recordingEnabled_.load(std::memory_order_acquire);
        if (outSessionId != nullptr) {
            *outSessionId = recordingSessionId_.load(std::memory_order_relaxed);
        }
        if (enabled && outDir != nullptr) {
            std::lock_guard<std::mutex> lock(recordingMtx_);
            *outDir = recordingOutputDir_;
        }
        return enabled;
    }

    void ImuOnlineCapture() {
        AtrakIMU imuData;
        const uint64_t IMU_THRESHOLD_NS = 10000000;
        std::deque<ImuQueuedSample> pendingSamples;

        while (mbIsRunning_) {
            bool gotData = false;
            while (FAYS_VIK_GetImuData(mptrHandle_, &imuData) == EXIT_SUCCESS) {
                gotData = true;

                if (lastImuTimestamp_ != 0) {
                    if (imuData.timestamp < lastImuTimestamp_) {
                        imuRollbackCount_++;
                    } else {
                        const uint64_t timeDiff = imuData.timestamp - lastImuTimestamp_;
                        if (timeDiff > IMU_THRESHOLD_NS) {
                            imuGapCount_++;
                        }
                    }
                }
                lastImuTimestamp_ = imuData.timestamp;

                const uint64_t systemNowNs = SystemNowNs();
                UpdateImuTimeOffset(imuData.timestamp, systemNowNs);

                uint64_t sessionId = 0;
                if (GetRecordingState(nullptr, &sessionId) && sessionId != 0) {
                    ImuQueuedSample sample{};
                    sample.imu = imuData;
                    sample.publishTimeNs = systemNowNs;
                    sample.sessionId = sessionId;
                    pendingSamples.push_back(sample);
                    imuQueue_.TryPushBatch(pendingSamples);
                }
            }

            if (!pendingSamples.empty()) {
                imuQueue_.TryPushBatch(pendingSamples);
            }
            if (!gotData) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        while (!pendingSamples.empty()) {
            if (!imuQueue_.TryPushBatch(pendingSamples)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        imuQueue_.NotifyStop();
    }

    void McapWriteThread() {
        std::vector<ImuQueuedSample> imuBatch;
        std::vector<CamTsQueuedSample> camBatch;
        std::vector<McapControlCommand> controlBatch;
        imuBatch.reserve(512);
        camBatch.reserve(256);
        controlBatch.reserve(16);
        uint64_t lastBacklogReportNs = 0;
        uint64_t activeLoggerSessionId = 0;
        bool loggerOpen = false;

        auto processControlCommand = [&](const McapControlCommand& cmd) {
            if (cmd.type == McapControlCommand::Type::Start) {
                if (loggerOpen) {
                    mDataLogger_.Close();
                    loggerOpen = false;
                    activeLoggerSessionId = 0;
                }
                if (!mDataLogger_.Open(cmd.mcapPath)) {
                    std::cerr << "[Control] START aborted in MCAP thread: failed to open "
                              << cmd.mcapPath << std::endl;
                    recordingSessionId_.store(0, std::memory_order_release);
                    recordingEnabled_.store(false, std::memory_order_release);
                    return;
                }
                loggerOpen = true;
                activeLoggerSessionId = cmd.sessionId;
                return;
            }

            if (cmd.type == McapControlCommand::Type::Stop) {
                if (!loggerOpen) {
                    return;
                }
                if (cmd.sessionId != 0 && cmd.sessionId != activeLoggerSessionId) {
                    return;
                }
                mDataLogger_.Close();
                loggerOpen = false;
                activeLoggerSessionId = 0;
            }
        };

        while (mbIsRunning_.load(std::memory_order_acquire) || !imuQueue_.Empty() || !camTsQueue_.Empty() || !mcapControlQueue_.Empty()) {
            size_t controlCount = mcapControlQueue_.TryDrainTo(controlBatch, 16);
            if (controlCount > 0) {
                for (const auto& cmd : controlBatch) {
                    processControlCommand(cmd);
                }
                controlBatch.clear();
            }

            const size_t imuCount = imuQueue_.DrainTo(imuBatch, 512);
            size_t camCount = camTsQueue_.TryDrainTo(camBatch, 256);
            if (imuCount == 0 && camCount == 0 && controlCount == 0) {
                controlCount = mcapControlQueue_.DrainTo(controlBatch, 16);
                if (controlCount > 0) {
                    for (const auto& cmd : controlBatch) {
                        processControlCommand(cmd);
                    }
                    controlBatch.clear();
                }
            }
            if (imuCount == 0 && camCount == 0 && controlCount == 0) {
                camCount = camTsQueue_.DrainTo(camBatch, 64);
            }
            if (imuCount == 0 && camCount == 0 && controlCount == 0) {
                continue;
            }

            for (const auto& sample : imuBatch) {
                if (sample.sessionId == 0) {
                    continue;
                }
                if (!loggerOpen) {
                    continue;
                }
                if (sample.sessionId != activeLoggerSessionId) {
                    continue;
                }
                mDataLogger_.LogImu(sample.imu, sample.publishTimeNs);
            }
            for (const auto& sample : camBatch) {
                if (sample.sessionId == 0) {
                    continue;
                }
                if (!loggerOpen) {
                    continue;
                }
                if (sample.sessionId != activeLoggerSessionId) {
                    continue;
                }
                mDataLogger_.LogCamTs(sample.faysTsNs, sample.publishTimeNs, sample.frameIndex);
            }

            imuBatch.clear();
            camBatch.clear();

            const size_t imuBacklog = imuQueue_.Size();
            const size_t camBacklog = camTsQueue_.Size();
            if (imuBacklog > 4096 || camBacklog > 512) {
                const uint64_t nowNs = SystemNowNs();
                if (lastBacklogReportNs == 0 || nowNs - lastBacklogReportNs > 2000000000ULL) {
                    std::cerr << "[McapWriter] WARNING: queue backlog imu=" << imuBacklog
                              << ", cam=" << camBacklog << std::endl;
                    lastBacklogReportNs = nowNs;
                }
            }
        }

        if (loggerOpen) {
            mDataLogger_.Close();
        }
    }

    void ImgOnlineCapture() {
        const std::string version = FAYS_VIK_GetVersion(mptrHandle_);
        std::cout << "[SDK] Version: " << version << std::endl;

        const uint64_t VIDEO_THRESHOLD_NS = 60000000;

        std::cout << "[Record] Waiting for Stereo frames..." << std::endl;

        while (mbIsRunning_) {
            bool gotFrame = false;
            if (EXIT_SUCCESS == FAYS_VIK_GetStereoFrames(mptrHandle_, &mImgData_)) {
                gotFrame = true;
                if (lastImgTimestamp_ != 0) {
                    if (mImgData_.timestamp < lastImgTimestamp_) {
                        std::cout << "[Video] Timestamp rollback detected: prev=" << lastImgTimestamp_
                                  << ", curr=" << mImgData_.timestamp
                                  << " diff=" << (static_cast<int64_t>(mImgData_.timestamp) - static_cast<int64_t>(lastImgTimestamp_))
                                  << " ns" << std::endl;
                    } else {
                        const uint64_t timeDiff = mImgData_.timestamp - lastImgTimestamp_;
                        if (timeDiff > VIDEO_THRESHOLD_NS) {
                            const double timeDiffMs = timeDiff / 1e6;
                            std::cout << "[Video] Time gap detected: " << std::fixed << std::setprecision(3)
                                      << timeDiffMs << " ms"
                                      << " (prev=" << lastImgTimestamp_ << ", curr=" << mImgData_.timestamp << ")"
                                      << std::endl;
                        }
                    }
                }
                lastImgTimestamp_ = mImgData_.timestamp;

                const int type = (mImgData_.channel == 1) ? CV_8UC1 : CV_8UC3;
                cv::Mat img(mImgData_.height, mImgData_.width, type, mImgData_.data);

                VideoFrame vf;
                vf.image = img.clone();
                vf.faysTsNs = mImgData_.timestamp;
                vf.publishTimeNs = AlignFaysTsToSystem(mImgData_.timestamp);
                videoFrameQueue_.Push(std::move(vf));
            }

            if (!gotFrame) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }

        videoFrameQueue_.Stop();
    }

    void VideoEncodeThread() {
        bool videoSessionOpen = false;
        uint32_t frameIndex = 0;
        uint64_t activeVideoSessionId = 0;
        std::string sessionOutputDir;
        std::deque<CamTsQueuedSample> pendingCamSamples;
        cv::Mat bgrBuffer;

        VideoFrame vf;
        while (videoFrameQueue_.Pop(vf)) {
            std::string outputDir;
            uint64_t sessionId = 0;
            const bool shouldRecord = GetRecordingState(&outputDir, &sessionId);

            if (shouldRecord && sessionId != 0) {
                if (videoSessionOpen && sessionId != activeVideoSessionId) {
                    mRecorder_.Stop();
                    videoSessionOpen = false;
                    sessionOutputDir.clear();
                    activeVideoSessionId = 0;
                }
                if (!videoSessionOpen && vf.image.cols > 0 && vf.image.rows > 0) {
                    std::cout << "[Record] Input Info: " << vf.image.cols << "x" << vf.image.rows
                              << " Channels: " << vf.image.channels() << std::endl;
                    if (mRecorder_.Start(outputDir + "fays_stereo_output.mkv",
                                         vf.image.cols, vf.image.rows, recordFps_)) {
                        sessionOutputDir = outputDir;
                        videoSessionOpen = true;
                        activeVideoSessionId = sessionId;
                        frameIndex = 0;
                    }
                }

                if (videoSessionOpen) {
                    if (vf.image.channels() == 1) {
                        cv::cvtColor(vf.image, bgrBuffer, cv::COLOR_GRAY2BGR);
                        mRecorder_.Write(bgrBuffer);
                    } else {
                        mRecorder_.Write(vf.image);
                    }

                    CamTsQueuedSample camSample{};
                    camSample.faysTsNs = vf.faysTsNs;
                    camSample.publishTimeNs = vf.publishTimeNs;
                    camSample.frameIndex = frameIndex;
                    camSample.sessionId = activeVideoSessionId;
                    pendingCamSamples.push_back(camSample);
                    camTsQueue_.TryPushBatch(pendingCamSamples);

                    frameIndex++;
                }
            } else if (videoSessionOpen) {
                mRecorder_.Stop();
                videoSessionOpen = false;
                activeVideoSessionId = 0;
                sessionOutputDir.clear();
            }
        }

        if (videoSessionOpen) {
            mRecorder_.Stop();
        }
        while (!pendingCamSamples.empty()) {
            if (!camTsQueue_.TryPushBatch(pendingCamSamples)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        camTsQueue_.NotifyStop();
    }

private:
    void* mptrHandle_;
    std::thread mptrImgThr_;
    std::thread mptrEncThr_;
    std::thread mptrImuThr_;
    std::thread mptrMcapWriterThr_;
    std::thread mptrUsbWatchThr_;

    VideoFrameQueue videoFrameQueue_;
    AtrakImage mImgData_;
    std::atomic<bool> mbIsRunning_;
    std::atomic<bool> recordingEnabled_;
    std::atomic<uint64_t> recordingSessionId_;
    std::atomic<uint64_t> recordingSessionSeed_;
    mutable std::mutex recordingMtx_;
    std::string recordingOutputDir_;

    int recordFps_;
    FFmpegRecorder mRecorder_;
    FaysDataLogger mDataLogger_;
    ImuQueue imuQueue_;
    CamTsQueue camTsQueue_;
    McapControlQueue mcapControlQueue_;

    uint64_t lastImuTimestamp_;
    std::atomic<uint64_t> imuGapCount_;
    std::atomic<uint64_t> imuRollbackCount_;

    uint64_t lastImgTimestamp_;
    std::atomic<int64_t> imuToSysOffsetNs_;
    std::atomic<bool> hasImuTimeOffset_;
    std::vector<std::string> monitoredDevicePaths_;
    std::vector<std::string> monitoredResolvedPaths_;
    std::vector<UsbWatchdogStatus> monitoredWatchdogStatus_;
};

void signalHandler(int signal) {
    if (g_recorder != nullptr) {
        std::cout << "\n[Signal] Received signal " << signal << ", stopping recorder..." << std::endl;
        g_recorder->Stop();
    }
}

static std::string Trim(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const size_t start = input.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(whitespace);
    return input.substr(start, end - start + 1);
}

static bool EnsureControlFifo(const std::string& fifoPath) {
    struct stat st {};
    if (stat(fifoPath.c_str(), &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            std::cerr << "[Control] Path exists but is not FIFO: " << fifoPath << std::endl;
            return false;
        }
        return true;
    }

    if (mkfifo(fifoPath.c_str(), 0666) != 0) {
        if (errno == EEXIST) {
            return true;
        }
        std::cerr << "[Control] Failed to create FIFO " << fifoPath
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

static void RunControlLoop(FaysRecorder& recorder, const std::string& fifoPath) {
    std::cout << "[Control] Entering command loop. FIFO: " << fifoPath << std::endl;
    std::cout << "[Control] Supported commands: START|<output_dir>, STOP, EXIT" << std::endl;

    while (recorder.IsRunning()) {
        // Keep FIFO opened in RDWR mode so open() won't block waiting for an external writer.
        // This makes the control endpoint visible immediately after daemon startup.
        int fd = open(fifoPath.c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "[Control] Failed to open FIFO: " << std::strerror(errno) << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        FILE* stream = fdopen(fd, "r");
        if (stream == nullptr) {
            std::cerr << "[Control] fdopen failed: " << std::strerror(errno) << std::endl;
            close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        char buffer[1024];
        while (recorder.IsRunning() && fgets(buffer, sizeof(buffer), stream) != nullptr) {
            std::string cmd = Trim(buffer);
            if (cmd.empty()) {
                continue;
            }

            if (cmd.rfind("START|", 0) == 0) {
                std::string outputDir = Trim(cmd.substr(6));
                if (outputDir.empty()) {
                    std::cerr << "[Control] START command missing output directory." << std::endl;
                    continue;
                }
                recorder.StartRecording(outputDir);
            } else if (cmd == "STOP") {
                recorder.StopRecordingSession();
            } else if (cmd == "EXIT") {
                recorder.Stop();
                break;
            } else {
                std::cerr << "[Control] Unknown command: " << cmd << std::endl;
            }
        }

        fclose(stream);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  ./fays_record_example <config_path> [output_directory]" << std::endl;
        std::cerr << "  ./fays_record_example <config_path> --control-fifo <fifo_path>" << std::endl;
        return 1;
    }

    const std::string configPath = argv[1];
    bool controlMode = false;
    std::string outputDir = ".";
    std::string controlFifoPath;

    if (argc >= 4 && std::string(argv[2]) == "--control-fifo") {
        controlMode = true;
        controlFifoPath = argv[3];
    } else if (argc >= 3) {
        outputDir = argv[2];
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    FaysRecorder recorder(configPath.c_str());
    g_recorder = &recorder;

    if (controlMode) {
        if (!EnsureControlFifo(controlFifoPath)) {
            g_recorder = nullptr;
            return 1;
        }
        RunControlLoop(recorder, controlFifoPath);
    } else {
        recorder.StartRecording(outputDir);
        std::string normalizedOutputDir = outputDir;
        if (!normalizedOutputDir.empty() && normalizedOutputDir.back() != '/') {
            normalizedOutputDir += '/';
        }

        std::cout << "========================================" << std::endl;
        std::cout << "   Stereo Recorder (Headless) Started" << std::endl;
        std::cout << "   Video: " << normalizedOutputDir << "fays_stereo_output.mkv" << std::endl;
        std::cout << "   Data : " << normalizedOutputDir << "fays_data.mcap" << std::endl;
        std::cout << "   Press Ctrl+C to stop recording" << std::endl;
        std::cout << "========================================" << std::endl;

        while (recorder.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    g_recorder = nullptr;
    return 0;
}
