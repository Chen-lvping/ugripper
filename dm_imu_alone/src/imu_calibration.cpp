#include "imu_driver.h"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <numeric>
#include <csignal>
#include <string>

// --- 全局控制变量 ---
std::atomic<bool> g_stopFlag(false);

// --- 信号处理 ---
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

// --- 辅助结构体：用于统计平均值 ---
struct GyroData {
    float x, y, z;
};

// --- 可视化辅助函数：生成进度条 ---
std::string getVisualBar(float value, float range, int width)
{
    std::string bar = "[";
    float ratio = value / range;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < -1.0f) ratio = -1.0f;

    int fill_len = static_cast<int>(std::abs(ratio) * width);
    std::string left_space(width, ' ');
    std::string right_space(width, ' ');

    if (ratio < 0)
    {
        for (int i = 0; i < fill_len; ++i)
            left_space[width - 1 - i] = '#';
    }
    else
    {
        for (int i = 0; i < fill_len; ++i)
            right_space[i] = '#';
    }
    bar += left_space + "|" + right_space + "]";
    return bar;
}

// --- 实时监控模式 (自动倒计时退出) ---
void monitorImuStatusAuto(dmbot_serial::DmImu &imu, int duration_sec)
{
    int refresh_rate = 20; // 20Hz 刷新
    int total_frames = duration_sec * refresh_rate;

    for (int i = 0; i < total_frames; ++i)
    {
        if (g_stopFlag.load()) return;

        auto d = imu.getData();
        int remaining_sec = duration_sec - (i / refresh_rate);

        // ANSI 转义序列：清屏并移动光标到左上角
        std::cout << "\033[2J\033[H";
        std::cout << "======================================================\n";
        std::cout << "             IMU Leveling Monitor (Auto)              \n";
        std::cout << "======================================================\n";
        std::cout << " Auto-calibrating in " << remaining_sec << " seconds...\n";
        std::cout << " Ensure device is STATIONARY and LEVEL.\n";
        std::cout << "------------------------------------------------------\n";
        std::cout << std::fixed << std::setprecision(3);
        
        std::cout << "GYRO (d/s): X:" << std::setw(7) << d.gyrox 
                  << " Y:" << std::setw(7) << d.gyroy 
                  << " Z:" << std::setw(7) << d.gyroz << "\n";
        
        std::cout << "------------------------------------------------------\n";
        std::cout << "ROLL : " << std::setw(8) << d.roll  << " deg " << getVisualBar(d.roll, 90.0f, 15) << "\n";
        std::cout << "PITCH: " << std::setw(8) << d.pitch << " deg " << getVisualBar(d.pitch, 90.0f, 15) << "\n";
        std::cout << "YAW  : " << std::setw(8) << d.yaw   << " deg " << getVisualBar(d.yaw, 180.0f, 15) << "\n";
        std::cout << "======================================================\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / refresh_rate));
    }
    
    // 清除残留显示
    std::cout << "\033[2J\033[H"; 
    std::cout << "Monitoring finished. Proceeding to calibration...\n" << std::endl;
}

// --- 采集N帧数据并计算平均值 ---
GyroData collectAverageGyro(dmbot_serial::DmImu &imu, int samples, const std::string& label) {
    std::vector<float> gx, gy, gz;
    gx.reserve(samples);
    gy.reserve(samples);
    gz.reserve(samples);

    std::cout << "Collecting " << samples << " samples for " << label << "..." << std::flush;

    for (int i = 0; i < samples; ++i) {
        if (g_stopFlag.load()) break;
        
        auto d = imu.getData();
        gx.push_back(d.gyrox);
        gy.push_back(d.gyroy);
        gz.push_back(d.gyroz);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 100Hz 采样
    }
    std::cout << " Done." << std::endl;

    GyroData avg = {0, 0, 0};
    if (gx.empty()) return avg;

    float sum_x = std::accumulate(gx.begin(), gx.end(), 0.0f);
    float sum_y = std::accumulate(gy.begin(), gy.end(), 0.0f);
    float sum_z = std::accumulate(gz.begin(), gz.end(), 0.0f);

    avg.x = sum_x / gx.size();
    avg.y = sum_y / gy.size();
    avg.z = sum_z / gz.size();

    return avg;
}

int main(int argc, char *argv[])
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "========================================" << std::endl;
    std::cout << "      IMU Gyroscope Calibration Tool    " << std::endl;
    std::cout << "========================================" << std::endl;

    // 1. 初始化 IMU
    std::string port = "/dev/ttyS2";
    int baud = 921600;
    
    std::cout << "Connecting to IMU on " << port << " @ " << baud << "..." << std::endl;
    // 正式连接
    dmbot_serial::DmImu imu(port, baud, dmbot_serial::DmImu::ProtocolType::RS485);
    imu.start();

    // 等待数据流稳定
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // --- 新增：实时数据显示与调平 (3秒倒计时) ---
    if (!g_stopFlag.load()) {
        monitorImuStatusAuto(imu, 3); // 显示3秒后自动退出
    }

    // 2. 检查校准前状态
    if (g_stopFlag.load()) return 0;
    
    std::cout << "\n[Step 1] Checking Pre-Calibration Status:" << std::endl;
    std::cout << "PLEASE KEEP IMU STATIONARY!" << std::endl;
    // std::this_thread::sleep_for(std::chrono::seconds(1)); // monitorImuStatusAuto 已经充当了等待时间

    GyroData preCalib = collectAverageGyro(imu, 100, "Pre-Calib Analysis");
    
    std::cout << std::fixed << std::setprecision(4);
    std::cout << " > Avg Gyro X: " << std::setw(8) << preCalib.x << " d/s" << std::endl;
    std::cout << " > Avg Gyro Y: " << std::setw(8) << preCalib.y << " d/s" << std::endl;
    std::cout << " > Avg Gyro Z: " << std::setw(8) << preCalib.z << " d/s" << std::endl;

    // 3. 执行校准
    if (g_stopFlag.load()) return 0;

    std::cout << "\n[Step 2] Sending Calibration Command..." << std::endl;
    
    imu.enterGyroCalib();

    std::cout << "Calibration command sent. Waiting for sensor to calibrate..." << std::endl;
    for (int i = 10; i > 0; --i) {
        if (g_stopFlag.load()) break;
        std::cout << "Waiting " << i << "s..." << "\r" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "Waiting 0s... Done.        " << std::endl;

    // 4. 验证结果
    if (g_stopFlag.load()) return 0;

    std::cout << "\n[Step 3] Verifying Result..." << std::endl;
    GyroData postCalib = collectAverageGyro(imu, 100, "Post-Calib Verification");

    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Result Comparison (Avg Bias):" << std::endl;
    std::cout << "Axis | Before (d/s) | After (d/s)  | Status" << std::endl;
    std::cout << "-----|--------------|--------------|-------" << std::endl;

    auto checkAxis = [](char axis, float before, float after) {
        // 阈值设为 0.2 d/s
        bool pass = std::abs(after) < 0.2f; 
        std::cout << "  " << axis << "  | " 
                  << std::setw(12) << before << " | " 
                  << std::setw(12) << after << " | "
                  << (pass ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m") 
                  << std::endl;
    };

    checkAxis('X', preCalib.x, postCalib.x);
    checkAxis('Y', preCalib.y, postCalib.y);
    checkAxis('Z', preCalib.z, postCalib.z);
    std::cout << "----------------------------------------" << std::endl;

    std::cout << "\nCalibration process finished." << std::endl;
    
    return 0;
}
