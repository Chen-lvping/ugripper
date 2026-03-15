#pragma once

#include "im648_CMD.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <libserialport.h>
#include <mutex>
#include <string>
#include <thread>

namespace dmbot_serial {

struct IM648_Data {
    float accx = 0.0f;
    float accy = 0.0f;
    float accz = 0.0f;
    float gyrox = 0.0f;
    float gyroy = 0.0f;
    float gyroz = 0.0f;
    float roll = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float quat_x = 0.0f;
    float quat_y = 0.0f;
    float quat_z = 0.0f;
    float quat_w = 1.0f;
    uint64_t timestamp = 0;
};

class Im648Driver {
public:
    Im648Driver(const std::string &port_name, int baudrate = 115200);
    ~Im648Driver();

    void start();
    void stop();
    bool tryConsumeData(IM648_Data *out);

private:
    static int writeThunk(const U8 *buf, int len, void *user_data);

    void initSerial();
    void configureDevice();
    void readThread();
    int write(const U8 *buf, int len);

    std::string port_name_;
    int baudrate_;
    std::thread th_;
    std::atomic<bool> stop_flag_{false};
    struct sp_port *port_ = nullptr;

    std::mutex data_mutex_;
    IM648_Data data_;
    bool data_updated_ = false;
    Im648ProtocolContext protocol_ctx_{};
};

}  // namespace dmbot_serial
