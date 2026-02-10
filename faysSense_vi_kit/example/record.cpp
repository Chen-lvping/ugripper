#include <string>
#include <thread>
#include <memory>
#include <iostream>
#include <fstream>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <cstddef>
#include <mcap/writer.hpp>
#include "fays_atrak/fays_atrak_types.h"
#include "fays_atrak/fays_atrak_vimod.h"

// --- JSON Payload 生成函数（无 orientation） ---
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
        : mptrHandle_{nullptr}, mbIsRunning_{true}, outputDir_(outputDir)
    {
        // 确保输出目录以 '/' 结尾
        if (!outputDir_.empty() && outputDir_.back() != '/') {
            outputDir_ += "/";
        }
        
        // 分配内存
        mImgData_.data = new uchar[FAYS_ATRAK_MONO_MAX_BYTES * 3]; // 预留足够空间
        
        // 创建句柄
        FAYS_VIK_CreateHandleWithConfig(&mptrHandle_, configPath);

        // 启动 IMU 和 Stereo 图像线程
        mptrImuThr_ = std::thread(&FaysRecorder::ImuOnlineCapture, this);
        mptrImgThr_ = std::thread(&FaysRecorder::ImgOnlineCapture, this);
    }

    ~FaysRecorder() {
        mbIsRunning_ = false;
        
        mRecorder_.Stop();

        // Flush CSV files to ensure data is written to disk
        mCsvLogger_.Flush();
        mImuLogger_.Flush();

        if (mptrImgThr_.joinable()) mptrImgThr_.join();
        if (mptrImuThr_.joinable()) mptrImuThr_.join();
        
        FAYS_VIK_DestroyHandle(mptrHandle_);
        delete[] mImgData_.data;
    }

    bool IsRunning() const { return mbIsRunning_; }
    
    void Stop() {
        mbIsRunning_ = false;
    }

private:
    void ImuOnlineCapture() {
        // IMU 线程保持空转以消耗数据
        
        std::string mcapPath = outputDir_ + "fays_imu_data.mcap";
        if (!mImuLogger_.Open(mcapPath)) {
            std::cerr << "Error: Could not create IMU MCAP file: " << mcapPath << std::endl;
        } else {
            std::cout << "[IMU MCAP] Logging to " << mcapPath << std::endl;
        }
        
        AtrakIMU imuData;
        while (mbIsRunning_) {
            if (FAYS_VIK_GetImuData(mptrHandle_, &imuData) == EXIT_SUCCESS) {
                mImuLogger_.Log(imuData);
                std::this_thread::sleep_for(std::chrono::nanoseconds(100));
            }
        }
    }

    void ImgOnlineCapture() {
        std::string version = FAYS_VIK_GetVersion(mptrHandle_);
        std::cout << "[SDK] Version: " << version << std::endl;

        const int RECORD_FPS = 50; // 假设帧率为30，根据实际相机配置调整
        bool isInitialized = false;
        long frameIndex = 0;

        // 打开 CSV 文件
        if (!mCsvLogger_.Open("timestamps.csv")) {
            std::cerr << "Error: Could not create timestamp CSV file." << std::endl;
        }

        std::cout << "[Record] Waiting for Stereo frames..." << std::endl;

        while (mbIsRunning_) {
            if (EXIT_SUCCESS == FAYS_VIK_GetStereoFrames(mptrHandle_, &mImgData_)) {
                // 1. 构造 Mat (不拷贝数据)
                // 这里的 img 包含了 Left 和 Right (通常是上下拼接)
                int type = (mImgData_.channel == 1) ? CV_8UC1 : CV_8UC3;
                cv::Mat img(mImgData_.height, mImgData_.width, type, mImgData_.data);

                // 2. 初始化录制 (第一帧执行)
                if (!isInitialized && img.cols > 0 && img.rows > 0) {
                    bool isColor = (img.channels() == 3);
                    std::cout << "[Record] Input Info: " << img.cols << "x" << img.rows 
                              << " Channels: " << img.channels() << std::endl;
                    
                    if (mRecorder_.Start("stereo_output.mkv", img.cols, img.rows, RECORD_FPS, isColor)) {
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

    AtrakImage mImgData_;
    std::atomic<bool> mbIsRunning_;
    std::string outputDir_;

    FFmpegRecorder mRecorder_;
    TimestampLogger mCsvLogger_;
    ImuLogger mImuLogger_;
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

    // Register signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    FaysRecorder recorder(argv[1], outputDir);
    
    // Set global pointer for signal handler access
    g_recorder = &recorder;

    std::cout << "========================================" << std::endl;
    std::cout << "   Stereo Recorder (Headless) Started" << std::endl;
    std::cout << "   Video: " << outputDir << "stereo_output.mkv" << std::endl;
    std::cout << "   Time : " << outputDir << "timestamps.csv" << std::endl;
    std::cout << "   IMU  : " << outputDir << "fays_imu_data.mcap" << std::endl;
    std::cout << "   Press Ctrl+C to stop recording" << std::endl;
    std::cout << "========================================" << std::endl;

    while (recorder.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Clear global pointer before recorder is destroyed
    g_recorder = nullptr;

    return 0;
}
