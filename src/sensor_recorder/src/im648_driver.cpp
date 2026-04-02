#include "im648_driver.h"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace {

constexpr int kConfigCommandSettleMs = 40;
constexpr int kFinalFlushSettleMs = 60;
constexpr size_t kMaxPendingSamples = 4096;

std::string resolveSerialPortPath(const std::string &configured_port) {
    std::error_code ec;
    if (fs::exists(configured_port, ec)) {
        const fs::path resolved = fs::weakly_canonical(configured_port, ec);
        if (!ec && !resolved.empty()) {
            return resolved.string();
        }
    }
    return configured_port;
}

void msleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

namespace dmbot_serial {

Im648Driver::Im648Driver(const std::string &port_name, int baudrate)
    : port_name_(port_name), baudrate_(baudrate) {
    initSerial();
    im648_InitContext(&protocol_ctx_,
                      &data_,
                      &data_updated_,
                      &data_mutex_,
                      &Im648Driver::sampleThunk,
                      this,
                      &Im648Driver::writeThunk,
                      this);
    configureDevice();
}

Im648Driver::~Im648Driver() {
    stop();
    if (port_ != nullptr) {
        sp_close(port_);
        sp_free_port(port_);
        port_ = nullptr;
    }
}

void Im648Driver::start() {
    stop_flag_ = false;
    th_ = std::thread(&Im648Driver::readThread, this);
}

void Im648Driver::stop() {
    stop_flag_ = true;
    if (th_.joinable()) {
        th_.join();
    }
}

bool Im648Driver::tryConsumeData(IM648_Data *out) {
    if (out == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(data_mutex_);
    if (pending_samples_.empty()) {
        return false;
    }

    *out = pending_samples_.front();
    pending_samples_.pop_front();
    data_updated_ = !pending_samples_.empty();
    return true;
}

void Im648Driver::sampleThunk(const IM648_Data &sample, void *user_data) {
    if (user_data == nullptr) {
        return;
    }
    static_cast<Im648Driver *>(user_data)->handleParsedSample(sample);
}

int Im648Driver::writeThunk(const U8 *buf, int len, void *user_data) {
    if (user_data == nullptr) {
        return 0;
    }
    return static_cast<Im648Driver *>(user_data)->write(buf, len);
}

void Im648Driver::initSerial() {
    const std::string resolved_port = resolveSerialPortPath(port_name_);

    enum sp_return rc = sp_get_port_by_name(resolved_port.c_str(), &port_);
    if (rc != SP_OK) {
        std::cerr << "Cannot find serial port " << port_name_
                  << " (resolved=" << resolved_port << ")" << std::endl;
        std::exit(1);
    }

    if (sp_open(port_, SP_MODE_READ_WRITE) != SP_OK) {
        std::cerr << "Cannot open port " << port_name_
                  << " (resolved=" << resolved_port << ")" << std::endl;
        std::exit(1);
    }

    sp_set_baudrate(port_, baudrate_);
    sp_set_bits(port_, 8);
    sp_set_parity(port_, SP_PARITY_NONE);
    sp_set_stopbits(port_, 1);
    sp_set_flowcontrol(port_, SP_FLOWCONTROL_NONE);

    std::cout << "IM648 serial port " << port_name_ << " opened successfully" << std::endl;
}

void Im648Driver::configureDevice() {
    protocol_ctx_.targetDeviceAddress = 255;
    im648_Cmd_12(&protocol_ctx_, 5, 255, 0, 0, 2, 200, 1, 3, 5, 0x0026);
    msleep(kConfigCommandSettleMs);
    im648_Cmd_03(&protocol_ctx_);
    msleep(kConfigCommandSettleMs);
    im648_Cmd_19(&protocol_ctx_);
    msleep(kConfigCommandSettleMs);
    msleep(kFinalFlushSettleMs);
    sp_flush(port_, SP_BUF_BOTH);
    std::cout << "IM648 initialization completed" << std::endl;
}

void Im648Driver::handleParsedSample(const IM648_Data &sample) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    data_ = sample;
    if (pending_samples_.size() >= kMaxPendingSamples) {
        pending_samples_.pop_front();
        ++dropped_samples_;
        if (dropped_samples_ == 1 || (dropped_samples_ % 256) == 0) {
            std::cerr << "[IM648] " << port_name_
                      << " pending sample queue full, dropped oldest samples="
                      << dropped_samples_ << std::endl;
        }
    }
    pending_samples_.push_back(sample);
    data_updated_ = true;
}

void Im648Driver::readThread() {
    unsigned char tmpdata[4096];

    std::cout << "IM648 read thread started" << std::endl;

    while (!stop_flag_.load()) {
        const int n = sp_nonblocking_read(port_, tmpdata, sizeof(tmpdata));
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                im648_Cmd_GetPkt(&protocol_ctx_, tmpdata[i]);
            }
        } else if (n < 0) {
            std::cerr << "Error reading from IM648 serial port " << port_name_ << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

int Im648Driver::write(const U8 *buf, int len) {
    if (port_ == nullptr) {
        return 0;
    }
    return sp_blocking_write(port_, buf, len, 50);
}

}  // namespace dmbot_serial
