#include <string>
#include <thread>
#include <memory>
#include <iostream>
#include <fstream>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <cstdio>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <cstddef>
#include <mcap/writer.hpp>
#include "fays_atrak/fays_atrak_types.h"
#include "fays_atrak/fays_atrak_vimod.h"

// --- IMU recording switches ---
#define ENABLE_IMU_MCAP  0  // Set to 0 to disable IMU MCAP recording
#define ENABLE_IMU_CSV   1  // Set to 0 to disable IMU CSV recording

// --- JSON Payload generation (without orientation) ---
#if ENABLE_IMU_MCAP
std::string create_imu_json(uint64_t timestamp_ns, const AtrakIMU& imuData) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), 
        "{"
            "\"frame_id\":\"imu_link\","
            "\"angular_velocity\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},"
            "\"linear_acceleration\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}"
        "}",
        imuData.gyro[0], imuData.gyro[1], imuData.gyro[2],
        imuData.acc[0], imuData.acc[1], imuData.acc[2]
    );
    return std::string(buffer);
}
#endif

// --- CSV 日志记录器 ---
class TimestampLogger {
public:
    TimestampLogger() {}
    ~TimestampLogger() { if (file_.is_open()) file_.close(); }

    bool Open(const std::string& path) {
        file_.open(path);
        if (!file_.is_open()) {
            std::cerr << "[CSV] Failed to open " << path << " for writing!" << std::endl;
            return false;
        }
        // 写入表头
        file_ << "frame_index,timestamp_ns" << std::endl;
        return true;
    }

    void Log(long frameIndex, uint64_t timestamp) {
        if (file_.is_open()) {
            file_ << frameIndex << "," << timestamp << "\n";
        }
    }

    void Flush() {
        if (file_.is_open()) {
            file_.flush();
        }
    }

private:
    std::ofstream file_;
};

// --- IMU MCAP 日志记录器 ---
#if ENABLE_IMU_MCAP
class ImuLogger {
public:
    ImuLogger() : writer_(), is_open_(false), seq_(0) {}
    
    ~ImuLogger() { 
        if (is_open_) {
            writer_.close();
        }
    }

    bool Open(const std::string& path) {
        if (is_open_) {
            std::cerr << "[IMU MCAP] Already open, closing previous file." << std::endl;
            writer_.close();
        }

        mcap::McapWriterOptions options("");
        options.compression = mcap::Compression::Lz4; // 开启压缩

        auto status = writer_.open(path, options);
        if (!status.ok()) {
            std::cerr << "[IMU MCAP] Failed to open MCAP writer: " << status.message << std::endl;
            return false;
        }

        // 注册 Foxglove IMU Schema（无 orientation）
        std::string schemaJson = R"({
            "type": "object",
            "properties": {
                "frame_id": { "type": "string" },
                "angular_velocity": {
                    "type": "object",
                    "properties": { "x": {"type":"number"}, "y": {"type":"number"}, "z": {"type":"number"} }
                },
                "linear_acceleration": {
                    "type": "object",
                    "properties": { "x": {"type":"number"}, "y": {"type":"number"}, "z": {"type":"number"} }
                }
            }
        })";
        mcap::Schema schema("foxglove.Imu", "jsonschema", schemaJson);
        writer_.addSchema(schema);

        // 注册 Channel
        mcap::Channel channel("imu_raw", "json", schema.id);
        writer_.addChannel(channel);
        channel_id_ = channel.id;

        is_open_ = true;
        seq_ = 0;
        return true;
    }

    void Log(const AtrakIMU& imuData) {
        if (!is_open_) return;

        // 构建 JSON Payload
        std::string payload = create_imu_json(imuData.timestamp, imuData);

        // 构建并写入消息
        mcap::Message msg;
        msg.channelId = channel_id_;
        msg.sequence = seq_++;
        msg.logTime = imuData.timestamp;
        msg.publishTime = imuData.timestamp;
        msg.data = reinterpret_cast<const std::byte*>(payload.data());
        msg.dataSize = payload.size();

        auto writeStatus = writer_.write(msg);
        if (!writeStatus.ok()) {
            std::cerr << "[IMU MCAP] Error writing frame: " << writeStatus.message << std::endl;
        }
    }

    void Flush() {
        // MCAP Writer 会自动处理刷新，但我们可以显式调用
        if (is_open_) {
            // MCAP 的 flush 操作在 close 时自动执行
        }
    }

private:
    mcap::McapWriter writer_;
    bool is_open_;
    mcap::ChannelId channel_id_;
    uint32_t seq_;
};
#endif

// --- IMU CSV Logger ---
#if ENABLE_IMU_CSV
class ImuCsvLogger {
public:
    ImuCsvLogger() {}
    ~ImuCsvLogger() { if (file_.is_open()) file_.close(); }

    bool Open(const std::string& path) {
        file_.open(path);
        if (!file_.is_open()) {
            std::cerr << "[IMU CSV] Failed to open " << path << " for writing!" << std::endl;
            return false;
        }
        // Write header
        file_ << "timestamp_ns,acc_x,acc_y,acc_z,gyro_x,gyro_y,gyro_z" << std::endl;
        return true;
    }

    void Log(const AtrakIMU& imuData) {
        if (file_.is_open()) {
            file_ << std::fixed << std::setprecision(9)
                  << imuData.timestamp << ","
                  << imuData.acc[0] << "," << imuData.acc[1] << "," << imuData.acc[2] << ","
                  << imuData.gyro[0] << "," << imuData.gyro[1] << "," << imuData.gyro[2]
                  << "\n";
        }
    }

    void Flush() {
        if (file_.is_open()) {
            file_.flush();
        }
    }

private:
    std::ofstream file_;
};
#endif

// --- Thread-safe IMU queue for producer-consumer pattern ---
class ImuQueue {
public:
    ImuQueue() : stopped_(false) {}

    // Push one IMU sample into the queue (called by producer)
    void Push(const AtrakIMU& data) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (queue_.size() >= MAX_SIZE) {
                // Drop oldest data to prevent unbounded memory growth
                queue_.pop_front();
                dropCount_++;
                if (dropCount_ == 1 || dropCount_ % 1000 == 0) {
                    std::cerr << "[ImuQueue] WARNING: Queue full, dropped " 
                              << dropCount_ << " frames total" << std::endl;
                }
            }
            queue_.push_back(data);
        }
        cv_.notify_one();
    }

    // Drain up to maxCount items into 'out' vector (called by consumer)
    // Returns the number of items drained
    size_t DrainTo(std::vector<AtrakIMU>& out, size_t maxCount) {
        std::unique_lock<std::mutex> lock(mtx_);
        // Wait until data is available or stopped
        cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
            return !queue_.empty() || stopped_;
        });
        size_t count = std::min(queue_.size(), maxCount);
        if (count == 0) return 0;
        out.reserve(out.size() + count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(queue_.front());
            queue_.pop_front();
        }
        return count;
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    // Unblock consumer thread waiting in DrainTo()
    void NotifyStop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    static constexpr size_t MAX_SIZE = 8192; // ~8s buffer at 1000Hz
    std::deque<AtrakIMU> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_;
    uint64_t dropCount_ = 0;
};

// --- FFmpeg 录制器 ---
class FFmpegRecorder {
public:
    FFmpegRecorder() : pipe_(nullptr) {}
    
    ~FFmpegRecorder() { Stop(); }

    // startParams: width, height, fps, isColor (true=BGR, false=GRAY)
    bool Start(const std::string& savePath, int width, int height, int fps, bool isColor) {
        if (pipe_) return true; 

        std::stringstream cmd;
        cmd << "ffmpeg -y ";
        
        // 1. 输入设置
        // 根据通道数决定像素格式
        std::string pixFmt = isColor ? "bgr24" : "gray";

        cmd << "-thread_queue_size 512 "
            << "-f rawvideo -vcodec rawvideo "
            << "-pix_fmt " << pixFmt << " "     // 动态设置格式
            << "-s " << width << "x" << height << " "
            << "-r " << fps << " "
            << "-i - "; 

        // 2. 硬件编码设置 (hevc_rkmpp)
        cmd << "-c:v hevc_rkmpp "
            << "-rc_mode CQP "
            << "-qp_init 30 "
            << "-qp_max 38 "
            << "-qp_min 24 "
            << "-qp_max_i 38 "
            << "-qp_min_i 20 ";
        
        // cmd << "-c:v libx265 "
        //     << "-preset medium "
        //     << "-crf 28 ";

        // 3. 输出路径
        cmd << "\"" << savePath << "\"";

        // 屏蔽 FFmpeg 的标准错误输出，避免刷屏 (调试时可注释掉)
        // cmd << " 2>/dev/null"; 

        std::cout << "[FFmpeg] Command: " << cmd.str() << std::endl;

        pipe_ = popen(cmd.str().c_str(), "w");
        if (!pipe_) {
            std::cerr << "[FFmpeg] Failed to open pipe!" << std::endl;
            return false;
        }
        return true;
    }

    void Write(const cv::Mat& frame) {
        if (!pipe_ || frame.empty()) return;
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

// Forward declaration
class FaysRecorder;

// Global pointer for signal handler access
static FaysRecorder* g_recorder = nullptr;

// Signal handler function declaration
void signalHandler(int signal);

// --- 主业务类 ---
class FaysRecorder {
public:
    FaysRecorder(const char* configPath, const std::string& outputDir = ".")
        : mptrHandle_{nullptr}, mbIsRunning_{true}, outputDir_(outputDir),
          lastImuTimestamp_(0), lastImgTimestamp_(0),
          imuGapCount_(0), imuRollbackCount_(0)
    {
        // 分配内存
        mImgData_.data = new uchar[FAYS_ATRAK_MONO_MAX_BYTES * 3]; // 预留足够空间
        
        // 创建句柄
        FAYS_VIK_CreateHandleWithConfig(&mptrHandle_, configPath);
        std::cout << "[FaysRecorder] Created handle with config: " << configPath << std::endl;
        std::cout << "[FaysRecorder] Output directory: " << outputDir_ << std::endl;

        // Start IMU producer thread (reads from SDK, pushes to queue)
        mptrImuThr_ = std::thread(&FaysRecorder::ImuOnlineCapture, this);
#if ENABLE_IMU_CSV
        // Start IMU CSV consumer thread (drains queue, writes to CSV)
        mptrImuCsvWriterThr_ = std::thread(&FaysRecorder::ImuCsvWriterThread, this);
#endif
        // Start Stereo image thread
        mptrImgThr_ = std::thread(&FaysRecorder::ImgOnlineCapture, this);
    }

    ~FaysRecorder() {
        mbIsRunning_ = false;
        
        mRecorder_.Stop();

        // Flush stereo CSV
        mCsvLogger_.Flush();

        // Wait for IMU producer thread to finish (it will call imuQueue_.NotifyStop())
        if (mptrImuThr_.joinable()) mptrImuThr_.join();

#if ENABLE_IMU_CSV
        // Signal queue stop so consumer can drain remaining data and exit
        imuQueue_.NotifyStop();
        if (mptrImuCsvWriterThr_.joinable()) mptrImuCsvWriterThr_.join();
#endif

#if ENABLE_IMU_MCAP
        mImuLogger_.Flush();
#endif

        if (mptrImgThr_.joinable()) mptrImgThr_.join();
        
        // Print final IMU statistics
        std::cout << "[IMU] Final stats - Gaps: " << imuGapCount_ 
                  << ", Rollbacks: " << imuRollbackCount_ << std::endl;

        FAYS_VIK_DestroyHandle(mptrHandle_);
        delete[] mImgData_.data;
    }

    bool IsRunning() const { return mbIsRunning_; }
    
    void Stop() {
        mbIsRunning_ = false;
    }

private:
    void ImuOnlineCapture() {
        // IMU producer thread: read data from SDK and push to queue
        // CSV/MCAP writing is handled by the separate ImuCsvWriterThread

#if ENABLE_IMU_MCAP
        std::string mcapPath = outputDir_ + "fays_imu_data.mcap";
        if (!mImuLogger_.Open(mcapPath)) {
            std::cerr << "Error: Could not create IMU MCAP file: " << mcapPath << std::endl;
        } else {
            std::cout << "[IMU MCAP] Logging to " << mcapPath << std::endl;
        }
#endif

        AtrakIMU imuData;
        // Threshold: 2ms gap at 1000Hz means missing at least 1 frame
        const uint64_t IMU_THRESHOLD_NS = 10000000;
        uint64_t imuReadCount = 0;

        while (mbIsRunning_) {
            bool gotData = false;
            // Drain all available IMU data in a tight loop
            while (FAYS_VIK_GetImuData(mptrHandle_, &imuData) == EXIT_SUCCESS) {
                gotData = true;
                imuReadCount++;

                // Lightweight gap detection (counter only, no expensive cout)
                if (lastImuTimestamp_ != 0) {
                    if (imuData.timestamp < lastImuTimestamp_) {
                        imuRollbackCount_++;
                    } else {
                        uint64_t timeDiff = imuData.timestamp - lastImuTimestamp_;
                        if (timeDiff > IMU_THRESHOLD_NS) {
                            imuGapCount_++;
                        }
                    }
                }
                lastImuTimestamp_ = imuData.timestamp;

                // Push to thread-safe queue (fast, no I/O in hot path)
                imuQueue_.Push(imuData);

#if ENABLE_IMU_MCAP
                mImuLogger_.Log(imuData);
#endif
            }

            // Periodic summary report (every ~5 seconds at 1000Hz)
            // if (imuReadCount > 0 && imuReadCount % 5000 == 0) {
            //     std::cout << "[IMU] Read: " << imuReadCount
            //               << ", Gaps: " << imuGapCount_
            //               << ", Rollbacks: " << imuRollbackCount_
            //               << ", Queue: " << imuQueue_.Size() << std::endl;
            // }

            // Only sleep when SDK has no more data available
            if (!gotData) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        // Signal the queue to unblock the consumer thread
        imuQueue_.NotifyStop();
    }

#if ENABLE_IMU_CSV
    void ImuCsvWriterThread() {
        // IMU consumer thread: drain queue and write to CSV file

        std::string csvPath = outputDir_ + "fays_imu_data.csv";
        if (!mImuCsvLogger_.Open(csvPath)) {
            std::cerr << "Error: Could not create IMU CSV file: " << csvPath << std::endl;
            return;
        }
        std::cout << "[IMU CSV] Logging to " << csvPath << std::endl;

        std::vector<AtrakIMU> batch;
        batch.reserve(256);
        uint64_t totalWritten = 0;
        uint64_t flushCounter = 0;

        // Keep running until stopped AND queue is fully drained
        while (mbIsRunning_ || !imuQueue_.Empty()) {
            size_t count = imuQueue_.DrainTo(batch, 256);
            if (count > 0) {
                for (const auto& imu : batch) {
                    mImuCsvLogger_.Log(imu);
                }
                totalWritten += count;
                flushCounter += count;
                batch.clear();

                // Periodic flush every ~1000 frames (~1 second at 1000Hz)
                if (flushCounter >= 1000) {
                    mImuCsvLogger_.Flush();
                    flushCounter = 0;
                }
            }
            // DrainTo already waits with timeout when queue is empty
        }

        // Final flush to ensure all data is written to disk
        mImuCsvLogger_.Flush();
        std::cout << "[IMU CSV] Writer stopped. Total written: " << totalWritten << " frames." << std::endl;
    }
#endif

    void ImgOnlineCapture() {
        std::string version = FAYS_VIK_GetVersion(mptrHandle_);
        std::cout << "[SDK] Version: " << version << std::endl;

        const int RECORD_FPS = 50; // 假设帧率为30，根据实际相机配置调整
        const uint64_t VIDEO_THRESHOLD_NS = 60000000;  // 3 * (1.0 / RECORD_FPS) * 1e9 = 60ms in nanoseconds
        bool isInitialized = false;
        long frameIndex = 0;

        // 打开 CSV 文件
        if (!mCsvLogger_.Open(outputDir_ + "fays_stereo_timestamp.csv")) {
            std::cerr << "Error: Could not create timestamp CSV file." << std::endl;
        }

        std::cout << "[Record] Waiting for Stereo frames..." << std::endl;

        while (mbIsRunning_) {
            if (EXIT_SUCCESS == FAYS_VIK_GetStereoFrames(mptrHandle_, &mImgData_)) {
                // Check time gap between consecutive video frames
                if (lastImgTimestamp_ != 0) {
                    // Check for timestamp rollback (out-of-order)
                    if (mImgData_.timestamp < lastImgTimestamp_) {
                        std::cout << "[Video] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Timestamp rollback detected: "
                                  << "prev: " << lastImgTimestamp_ 
                                  << ", curr: " << mImgData_.timestamp 
                                  << " (diff: " << (static_cast<int64_t>(mImgData_.timestamp) - static_cast<int64_t>(lastImgTimestamp_)) << " ns)" << std::endl;
                    } else {
                        uint64_t timeDiff = mImgData_.timestamp - lastImgTimestamp_;
                        if (timeDiff > VIDEO_THRESHOLD_NS) {
                            double timeDiffMs = timeDiff / 1e6;
                            std::cout << "[Video] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Time gap detected: " << std::fixed << std::setprecision(3) 
                                      << timeDiffMs << " ms (prev: " << lastImgTimestamp_ 
                                      << ", curr: " << mImgData_.timestamp << ", expected: ~20ms)" << std::endl;
                        }
                    }
                }
                lastImgTimestamp_ = mImgData_.timestamp;
                
                // 1. 构造 Mat (不拷贝数据)
                // 这里的 img 包含了 Left 和 Right (通常是上下拼接)
                int type = (mImgData_.channel == 1) ? CV_8UC1 : CV_8UC3;
                cv::Mat img(mImgData_.height, mImgData_.width, type, mImgData_.data);

                // 2. 初始化录制 (第一帧执行)
                if (!isInitialized && img.cols > 0 && img.rows > 0) {
                    bool isColor = (img.channels() == 3);
                    std::cout << "[Record] Input Info: " << img.cols << "x" << img.rows 
                              << " Channels: " << img.channels() << std::endl;
                    
                    if (mRecorder_.Start(outputDir_ + "fays_stereo_output.mkv", img.cols, img.rows, RECORD_FPS, isColor)) {
                        isInitialized = true;
                    }
                }

                // 3. 写入数据
                if (isInitialized) {
                    // 写入视频帧 (包含左右目)
                    mRecorder_.Write(img);
                    
                    // 写入 CSV (使用 SDK 返回的硬件时间戳)
                    mCsvLogger_.Log(frameIndex, mImgData_.timestamp);

                    frameIndex++;
                    if (frameIndex % 300 == 0) {
                        std::cout << "[Record] Saved " << frameIndex << " frames. Latest TS: " << mImgData_.timestamp << "\n";
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    void* mptrHandle_;
    std::thread mptrImgThr_;
    std::thread mptrImuThr_;
#if ENABLE_IMU_CSV
    std::thread mptrImuCsvWriterThr_;  // Dedicated CSV writer thread
#endif

    AtrakImage mImgData_;
    std::atomic<bool> mbIsRunning_;
    std::string outputDir_;

    FFmpegRecorder mRecorder_;
    TimestampLogger mCsvLogger_;
#if ENABLE_IMU_MCAP
    ImuLogger mImuLogger_;
#endif
#if ENABLE_IMU_CSV
    ImuCsvLogger mImuCsvLogger_;
#endif

    // IMU producer-consumer queue
    ImuQueue imuQueue_;

    // IMU gap detection statistics (updated by producer thread only)
    uint64_t lastImuTimestamp_;
    std::atomic<uint64_t> imuGapCount_;
    std::atomic<uint64_t> imuRollbackCount_;

    uint64_t lastImgTimestamp_;   // Last image timestamp for gap detection
};

// Signal handler function implementation
void signalHandler(int signal) {
    if (g_recorder != nullptr) {
        std::cout << "\n[Signal] Received signal " << signal << ", stopping recorder..." << std::endl;
        g_recorder->Stop();
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./app <config_path> [output_directory]" << std::endl;
        std::cerr << "  - config_path: Path to configuration YAML file" << std::endl;
        std::cerr << "  - output_directory: (Optional) Output directory for data files (default: current directory)" << std::endl;
        return 1;
    }

    // Parse output directory argument
    std::string outputDir = ".";
    if (argc > 2) {
        outputDir = argv[2];
    } else {
        std::cout << "Warning: No output directory provided, using current directory." << std::endl;
    }
     // 确保输出目录以 '/' 结尾
     if (!outputDir.empty() && outputDir.back() != '/') {
        outputDir += "/";
    }

    // Register signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    FaysRecorder recorder(argv[1], outputDir);
    
    // Set global pointer for signal handler access
    g_recorder = &recorder;

    std::cout << "========================================" << std::endl;
    std::cout << "   Stereo Recorder (Headless) Started" << std::endl;
    std::cout << "   Video: " << outputDir << "fays_stereo_output.mkv" << std::endl;
    std::cout << "   Time : " << outputDir << "fays_stereo_timestamp.csv" << std::endl;
#if ENABLE_IMU_MCAP
    std::cout << "   IMU MCAP: " << outputDir << "fays_imu_data.mcap" << std::endl;
#endif
#if ENABLE_IMU_CSV
    std::cout << "   IMU CSV : " << outputDir << "fays_imu_data.csv" << std::endl;
#endif
    std::cout << "   Press Ctrl+C to stop recording" << std::endl;
    std::cout << "========================================" << std::endl;

    while (recorder.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Clear global pointer before recorder is destroyed
    g_recorder = nullptr;

    return 0;
}
