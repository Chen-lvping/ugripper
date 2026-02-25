#include <string>
#include <thread>
#include <memory>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <cstddef>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <mcap/writer.hpp>
#include "fays_atrak/fays_atrak_types.h"
#include "fays_atrak/fays_atrak_vimod.h"

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

class FFmpegRecorder {
public:
    FFmpegRecorder() : pipe_(nullptr) {}

    ~FFmpegRecorder() { Stop(); }

    bool Start(const std::string& savePath, int width, int height, int fps, bool isColor) {
        if (pipe_) {
            return true;
        }

        std::stringstream cmd;
        cmd << "ffmpeg -y ";

        std::string pixFmt = isColor ? "bgr24" : "gray";

        cmd << "-thread_queue_size 512 "
            << "-f rawvideo -vcodec rawvideo "
            << "-pix_fmt " << pixFmt << " "
            << "-s " << width << "x" << height << " "
            << "-r " << fps << " "
            << "-i - ";

        cmd << "-c:v hevc_rkmpp "
            << "-rc_mode CQP "
            << "-qp_init 30 "
            << "-qp_max 38 "
            << "-qp_min 24 "
            << "-qp_max_i 38 "
            << "-qp_min_i 20 ";

        cmd << "\"" << savePath << "\"";

        std::cout << "[FFmpeg] Command: " << cmd.str() << std::endl;

        pipe_ = popen(cmd.str().c_str(), "w");
        if (!pipe_) {
            std::cerr << "[FFmpeg] Failed to open pipe!" << std::endl;
            return false;
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
          lastImuTimestamp_(0),
          lastImgTimestamp_(0),
          imuGapCount_(0),
          imuRollbackCount_(0),
          imuToSysOffsetNs_(0),
          hasImuTimeOffset_(false) {
        mImgData_.data = new uchar[FAYS_ATRAK_MONO_MAX_BYTES * 3];

        FAYS_VIK_CreateHandleWithConfig(&mptrHandle_, configPath);
        std::cout << "[FaysRecorder] Created handle with config: " << configPath << std::endl;
        std::cout << "[FaysRecorder] Standby mode ready. Waiting for START command." << std::endl;

        mptrImuThr_ = std::thread(&FaysRecorder::ImuOnlineCapture, this);
        mptrImgThr_ = std::thread(&FaysRecorder::ImgOnlineCapture, this);
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

        mRecorder_.Stop();
        mDataLogger_.Close();

        std::cout << "[IMU] Final stats - Gaps: " << imuGapCount_
                  << ", Rollbacks: " << imuRollbackCount_ << std::endl;

        FAYS_VIK_DestroyHandle(mptrHandle_);
        delete[] mImgData_.data;
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

        if (!mDataLogger_.Open(mcapPath)) {
            std::cerr << "[Control] START aborted: failed to open " << mcapPath << std::endl;
            recordingEnabled_ = false;
            return;
        }

        recordingOutputDir_ = normalizedOutputDir;
        recordingEnabled_ = true;
        hasImuTimeOffset_.store(false, std::memory_order_release);

        std::cout << "[Control] START recording. Output directory: " << recordingOutputDir_ << std::endl;
        std::cout << "[Control] MCAP output: " << mcapPath << std::endl;
    }

    void StopRecordingSession() {
        bool wasRecording = recordingEnabled_.exchange(false);
        if (wasRecording) {
            std::cout << "[Control] STOP recording." << std::endl;
        }
        mDataLogger_.Close();
    }

private:
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

    bool GetRecordingState(std::string* outDir = nullptr) const {
        const bool enabled = recordingEnabled_.load();
        if (enabled && outDir != nullptr) {
            std::lock_guard<std::mutex> lock(recordingMtx_);
            *outDir = recordingOutputDir_;
        }
        return enabled;
    }

    void ImuOnlineCapture() {
        AtrakIMU imuData;
        const uint64_t IMU_THRESHOLD_NS = 10000000;

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

                if (GetRecordingState()) {
                    mDataLogger_.LogImu(imuData, systemNowNs);
                }
            }

            if (!gotData) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

    void ImgOnlineCapture() {
        const std::string version = FAYS_VIK_GetVersion(mptrHandle_);
        std::cout << "[SDK] Version: " << version << std::endl;

        const int RECORD_FPS = 25;
        const uint64_t VIDEO_THRESHOLD_NS = 60000000;
        bool videoSessionOpen = false;
        uint32_t frameIndex = 0;
        std::string sessionOutputDir;

        std::cout << "[Record] Waiting for Stereo frames..." << std::endl;

        while (mbIsRunning_) {
            if (EXIT_SUCCESS == FAYS_VIK_GetStereoFrames(mptrHandle_, &mImgData_)) {
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

                std::string outputDir;
                const bool shouldRecord = GetRecordingState(&outputDir);

                if (shouldRecord) {
                    if (!videoSessionOpen && img.cols > 0 && img.rows > 0) {
                        const bool isColor = (img.channels() == 3);
                        std::cout << "[Record] Input Info: " << img.cols << "x" << img.rows
                                  << " Channels: " << img.channels() << std::endl;
                        if (mRecorder_.Start(outputDir + "fays_stereo_output.mkv", img.cols, img.rows, RECORD_FPS, isColor)) {
                            sessionOutputDir = outputDir;
                            videoSessionOpen = true;
                            frameIndex = 0;
                        }
                    }

                    if (videoSessionOpen) {
                        mRecorder_.Write(img);
                        const uint64_t publishTimeNs = AlignFaysTsToSystem(mImgData_.timestamp);
                        mDataLogger_.LogCamTs(mImgData_.timestamp, publishTimeNs, frameIndex);

                        frameIndex++;
                        if (frameIndex % 300 == 0) {
                            std::cout << "[Record] Saved " << frameIndex
                                      << " frames to " << sessionOutputDir
                                      << ". Latest TS: " << mImgData_.timestamp << "\n";
                        }
                    }
                } else if (videoSessionOpen) {
                    mRecorder_.Stop();
                    videoSessionOpen = false;
                    sessionOutputDir.clear();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (videoSessionOpen) {
            mRecorder_.Stop();
        }
    }

private:
    void* mptrHandle_;
    std::thread mptrImgThr_;
    std::thread mptrImuThr_;

    AtrakImage mImgData_;
    std::atomic<bool> mbIsRunning_;
    std::atomic<bool> recordingEnabled_;
    mutable std::mutex recordingMtx_;
    std::string recordingOutputDir_;

    FFmpegRecorder mRecorder_;
    FaysDataLogger mDataLogger_;

    uint64_t lastImuTimestamp_;
    std::atomic<uint64_t> imuGapCount_;
    std::atomic<uint64_t> imuRollbackCount_;

    uint64_t lastImgTimestamp_;
    std::atomic<int64_t> imuToSysOffsetNs_;
    std::atomic<bool> hasImuTimeOffset_;
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
        int fd = open(fifoPath.c_str(), O_RDONLY);
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
