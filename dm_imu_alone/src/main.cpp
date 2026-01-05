#include "imu_driver.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <cmath>
#include <unistd.h>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <fstream>
#include <csignal>

// --- 全局控制变量 ---
std::atomic<bool> g_stopFlag(false);
#define VIEW_IMU_LOG

// --- 信号处理函数 ---
void signalHandler(int signum)
{
    std::cout << "\nInterrupt signal (" << signum << ") received. Stopping..." << std::endl;
    g_stopFlag = true;
}

// --- 可视化辅助函数：生成进度条 (保持不变) ---
std::string getVisualBar(float value, float range, int width)
{
    std::string bar = "[";
    float ratio = value / range;
    if (ratio > 1.0f)
        ratio = 1.0f;
    if (ratio < -1.0f)
        ratio = -1.0f;

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

// --- UDP 数据包结构 ---
struct __attribute__((packed)) ImuPacket
{
    float w, x, y, z;
};

int main(int argc, char *argv[])
{
    // 1. 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 2. 参数解析
    // 优先获取输出目录，其次检查是否开启可视化
    std::string outputDir = ".";
    bool enable_vis = false;

    // 简单的参数处理逻辑
    if (argc > 1)
    {
        // 假设第一个参数是路径（如果不是 -view）
        if (std::string(argv[1]) != "-view")
        {
            outputDir = argv[1];
        }

        // 遍历查找 -view
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "-view") == 0)
            {
                enable_vis = true;
            }
        }
    }
    else
    {
        std::cout << "Warning: No output directory provided, using current directory." << std::endl;
    }

    // 路径处理
    if (outputDir.back() != '/')
    {
        outputDir += "/";
    }
    std::string filename = outputDir + "imu_data.csv";

    // 3. 初始化 CSV
    std::ofstream csvFile(filename);
    if (!csvFile.is_open())
    {
        std::cerr << "Failed to open file for logging: " << filename << std::endl;
        return -1;
    }
    else
    {
        // 写入表头
        csvFile << "Timestamp_us,Acc_X,Acc_Y,Acc_Z,Gyro_X,Gyro_Y,Gyro_Z,Quat_X,Quat_Y,Quat_Z,Quat_W" << std::endl;
        std::cout << "Logging IMU data to " << filename << std::endl;
    }

    // 4. 初始化 UDP (如果需要)
    int sock = -1;
    struct sockaddr_in addr;
    if (enable_vis)
    {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0)
        {
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(9999);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            std::cout << "[Info] UDP Visualization Enabled (127.0.0.1:9999)\n";
        }
    }

    // 5. 初始化 IMU
    // 注意：根据你的实际波特率调整，这里保留原代码的 921600
    dmbot_serial::DmImu imu("/dev/ttyS2", 921600, dmbot_serial::DmImu::ProtocolType::RS485);
    imu.start();

    std::cout << "Starting IMU monitoring (1kHz logging)... Press Ctrl+C to stop." << std::endl;

    // 设置输出格式
    std::cout << std::fixed << std::setprecision(3);
    std::cout.setf(std::ios::unitbuf);

    // 6. 主循环控制变量
    auto next_time = std::chrono::steady_clock::now();
    auto loopPeriod = std::chrono::microseconds(1000); // 1 kHz = 1000us

    // 用于降低打印频率的计数器 (1000Hz loop / 50 = 20Hz print)
    int printDivisor = 0;
    const int printThreshold = 50;

    while (!g_stopFlag.load())
    {
        next_time += loopPeriod; // 设定下一次唤醒时间点

        // --- 获取数据 ---
        // 假设 imu.getData() 获取的是最新缓存的数据
        auto d = imu.getData();

        // --- 获取时间戳 ---
        auto now = std::chrono::system_clock::now();
        auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                now.time_since_epoch())
                                .count();

        // --- 写入 CSV (1kHz) ---
        if (csvFile.is_open())
        {
            csvFile << timestamp_us << ","
                    << d.accx << "," << d.accy << "," << d.accz << ","
                    << d.gyrox << "," << d.gyroy << "," << d.gyroz << ","
                    << d.quat_x << "," << d.quat_y << "," << d.quat_z << "," << d.quat_w
                    << std::endl;
        }

        // --- 终端打印与UDP (降频处理: 20Hz) ---
        // 频繁打印 IO 会阻塞线程，导致无法维持 1kHz 记录频率，所以必须降频
        printDivisor++;
        if (printDivisor >= printThreshold)
        {
            printDivisor = 0;

// 只有定义VIEW_IMU_LOG才打印
#ifdef VIEW_IMU_LOG
            // 清屏并回位 (注意：这在 log 模式下可能会干扰查看，建议仅在调试时开启)
            std::cout << "\033[2J\033[H";
            std::cout << "================= IMU Data Dashboard =================\n";
            std::cout << std::setfill(' ') << std::right;

            std::cout << "Time: " << timestamp_us << " us\n";
            std::cout << "ACC  (g): " << "X:" << std::setw(8) << d.accx << " Y:" << std::setw(8) << d.accy << " Z:" << std::setw(8) << d.accz << "\n";
            std::cout << "GYRO (d/s): " << "X:" << std::setw(8) << d.gyrox << " Y:" << std::setw(8) << d.gyroy << " Z:" << std::setw(8) << d.gyroz << "\n";
            std::cout << "QUAT    : " << "X:" << std::setw(7) << d.quat_x << " Y:" << std::setw(7) << d.quat_y << " Z:" << std::setw(7) << d.quat_z << " W:" << std::setw(7) << d.quat_w << "\n";
            std::cout << "------------------------------------------------------\n";
            std::cout << "ROLL : " << std::setw(8) << d.roll << " deg " << getVisualBar(d.roll, 90.0f, 15) << "\n";
            std::cout << "PITCH: " << std::setw(8) << d.pitch << " deg " << getVisualBar(d.pitch, 90.0f, 15) << "\n";
            std::cout << "YAW  : " << std::setw(8) << d.yaw << " deg " << getVisualBar(d.yaw, 180.0f, 15) << "\n";
            std::cout << "======================================================\n";
#endif

            // UDP 发送也放在低频循环里
            if (enable_vis && sock >= 0)
            {
                ImuPacket packet;
                packet.w = d.quat_w;
                packet.x = d.quat_x;
                packet.y = d.quat_y;
                packet.z = d.quat_z;
                sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
            }
        }

        // --- 周期控制 ---
        std::this_thread::sleep_until(next_time);
    }

    // --- 清理资源 ---
    std::cout << "Shutting down IMU..." << std::endl;

    if (csvFile.is_open())
    {
        csvFile.close();
        std::cout << "CSV log saved." << std::endl;
    }

    if (sock >= 0)
        close(sock);

    // 假设 imu 析构函数会处理关闭串口，如果需要显式停止：
    // imu.stop();

    return 0;
}
