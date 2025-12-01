#include "imu_driver.h"
#include <iostream>
#include <iomanip> // 格式化需要
#include <string>
#include <cmath>
#include <unistd.h>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>

// --- 可视化辅助函数：生成进度条 ---
std::string getVisualBar(float value, float range, int width)
{
    std::string bar = "[";

    // 归一化到 [-1, 1]
    float ratio = value / range;
    if (ratio > 1.0f)
        ratio = 1.0f;
    if (ratio < -1.0f)
        ratio = -1.0f;

    int fill_len = static_cast<int>(std::abs(ratio) * width);
    std::string left_space(width, ' ');
    std::string right_space(width, ' ');

    if (ratio < 0)
    { // 负值向左填
        for (int i = 0; i < fill_len; ++i)
            left_space[width - 1 - i] = '#';
    }
    else
    { // 正值向右填
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
    // 1. 参数解析 (-v 开启 UDP 转发)
    bool enable_vis = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-view") == 0)
        {
            enable_vis = true;
            break;
        }
        else
        {
            std::cout << "error wrong arg" << "\n";
        }
    }

    // 2. 初始化 UDP (如果需要)
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
            sleep(1); // 让用户看一眼提示
        }
    }

    // 3. 初始化 IMU
    dmbot_serial::DmImu imu("/dev/ttyS0", 921600, dmbot_serial::DmImu::ProtocolType::RS485);
    imu.start();

    // 4. 设置全局流格式 (固定小数位，清除缓冲区)
    std::cout << std::fixed << std::setprecision(3);
    std::cout.setf(std::ios::unitbuf); // 无缓冲输出，防止打印延迟

    while (1)
    {
        auto d = imu.getData();

        // --- 终端打印部分 ---

        // 清屏并回位
        std::cout << "\033[2J\033[H";

        std::cout << "================= IMU Data Dashboard =================\n";

        // 关键修复：setfill(' ') 用空格填充，std::right 保证负号贴着数字
        std::cout << std::setfill(' ') << std::right;

        // 1. 加速度
        std::cout << "ACC  (g): "
                  << "X:" << std::setw(8) << d.accx
                  << " Y:" << std::setw(8) << d.accy
                  << " Z:" << std::setw(8) << d.accz << "\n";

        // 2. 角速度
        std::cout << "GYRO (d/s): "
                  << "X:" << std::setw(8) << d.gyrox
                  << " Y:" << std::setw(8) << d.gyroy
                  << " Z:" << std::setw(8) << d.gyroz << "\n";

        // 3. 四元数
        std::cout << "QUAT    : "
                  << "X:" << std::setw(7) << d.quat_x
                  << " Y:" << std::setw(7) << d.quat_y
                  << " Z:" << std::setw(7) << d.quat_z
                  << " W:" << std::setw(7) << d.quat_w << "\n";

        std::cout << "------------------------------------------------------\n";

        // 4. 欧拉角 (保留你想要的可视化)
        std::cout << "ROLL : " << std::setw(8) << d.roll << " deg " << getVisualBar(d.roll, 90.0f, 15) << "\n";
        std::cout << "PITCH: " << std::setw(8) << d.pitch << " deg " << getVisualBar(d.pitch, 90.0f, 15) << "\n";
        std::cout << "YAW  : " << std::setw(8) << d.yaw << " deg " << getVisualBar(d.yaw, 180.0f, 15) << "\n";

        std::cout << "======================================================\n";

        // --- UDP 发送部分 ---
        if (enable_vis && sock >= 0)
        {
            std::cout << "Please open vis_script" << "\n";
            ImuPacket packet;
            packet.w = d.quat_w;
            packet.x = d.quat_x;
            packet.y = d.quat_y;
            packet.z = d.quat_z;
            sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
        }

        usleep(50000); // 50ms 刷新 (20Hz)，保证动画流畅
    }

    if (sock >= 0)
        close(sock);
    return 0;
}
