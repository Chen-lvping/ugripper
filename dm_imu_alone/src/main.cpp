#include "imu_driver.h"
#include <iostream>
#include <unistd.h>

int main()
{
    dmbot_serial::DmImu imu("/dev/ttyS7", 921600, dmbot_serial::DmImu::ProtocolType::RS485);
    imu.start();
    while (1)
    {
        auto d = imu.getData();
        std::cout << "acc: " << d.accx << " " << d.accy << " " << d.accz
                  << " gyro: " << d.gyrox << " " << d.gyroy << " " << d.gyroz
                  << " euler: " << d.roll << " " << d.pitch << " " << d.yaw
                  << " quat: " << d.quat_x << " " << d.quat_y << " " << d.quat_z << " " << d.quat_w << "\n";
        usleep(100000);
    }
}
