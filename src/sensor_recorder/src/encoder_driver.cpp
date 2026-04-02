#include "encoder_driver.h"
#include <thread>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxPendingEncoderSamples = 8192;

} // namespace

static std::string resolveSerialPortPath(const std::string &configuredPort)
{
    std::error_code ec;
    if (fs::exists(configuredPort, ec))
    {
        const fs::path resolved = fs::weakly_canonical(configuredPort, ec);
        if (!ec && !resolved.empty())
        {
            return resolved.string();
        }
    }
    return configuredPort;
}

EncoderDriver::EncoderDriver(uint8_t serialNum, const std::string &port,
                             uint32_t baudrate, const std::string &name)
    : name_(name), port_(port), baudrate_(baudrate), serialNum_(serialNum),
      serialPort_(nullptr)
{
    currentState_.id = serialNum;
    currentState_.com = serialNum;
}

EncoderDriver::~EncoderDriver()
{
    disconnect();
}

void EncoderDriver::markDisconnected(const std::string &operation)
{
    bool wasConnected = isConnected_.exchange(false);
    isActive_ = false;
    isConfigMode_ = false;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentState_.linkSta.is_active = 0;
    }

    if (wasConnected)
    {
        std::cerr << "EncoderDriver: " << operation << " failed on " << port_
                  << ", marking encoder disconnected until next reconnect." << std::endl;
    }
}

ConnectStatus EncoderDriver::connect()
{
    if (isConnected_)
        return ConnectStatus::SUCCESS;

    const std::string resolvedPort = resolveSerialPortPath(port_);

    sp_return result = sp_get_port_by_name(resolvedPort.c_str(), &serialPort_);
    if (result != SP_OK)
    {
        std::cerr << "EncoderDriver: Cannot find port " << port_
                  << " (resolved=" << resolvedPort << ")" << std::endl;
        return ConnectStatus::SERIAL_FAIL;
    }

    result = sp_open(serialPort_, SP_MODE_READ_WRITE);
    if (result != SP_OK)
    {
        std::cerr << "EncoderDriver: Cannot open port " << port_
                  << " (resolved=" << resolvedPort << ")" << std::endl;
        sp_free_port(serialPort_);
        serialPort_ = nullptr;
        return ConnectStatus::SERIAL_FAIL;
    }

    // Configure serial port
    sp_set_baudrate(serialPort_, baudrate_);
    sp_set_bits(serialPort_, 8);
    sp_set_parity(serialPort_, SP_PARITY_NONE);
    sp_set_stopbits(serialPort_, 1);
    sp_set_flowcontrol(serialPort_, SP_FLOWCONTROL_NONE);

    isConnected_ = true;
    isActive_ = true;

    std::cout << "EncoderDriver: Serial port opened at " << baudrate_ << " baud. Verifying encoder..." << std::endl;

    // --- 验证编码器是否有响应 ---
    uint8_t buf[256];
    requestState(false);                                        // 发送请求指令
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 等待回复

    int n = readDataNonBlocking(buf, sizeof(buf));

    if (n == 7) // 根据你的指令帧长度判断
    {
        std::cout << "EncoderDriver: Encoder responded successfully." << std::endl;
        return ConnectStatus::SUCCESS;
    }
    else
    {
        std::cerr << "EncoderDriver: No response from encoder." << std::endl;
        return ConnectStatus::NO_RESPONSE;
    }
}

void EncoderDriver::disconnect()
{
    if (serialPort_)
    {
        sp_close(serialPort_);
        sp_free_port(serialPort_);
        serialPort_ = nullptr;
    }
    isConnected_ = false;
    isActive_ = false;
}

void EncoderDriver::resetBaudrate(uint32_t baudrate)
{
    baudrate_ = baudrate;
}

bool EncoderDriver::sendToEncoder(uint8_t *data, uint8_t len)
{
    if (!isConnected_ || isConfigMode_)
    {
        return false;
    }

    sp_return result = sp_blocking_write(serialPort_, data, len, 100);
    if (result < 0)
    {
        markDisconnected("write");
        return false;
    }

    return true;
}

bool EncoderDriver::sendConfigToEncoder(uint8_t *data, uint8_t len)
{
    if (!isConnected_)
    {
        return false;
    }

    isConfigMode_ = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    sp_return result = sp_blocking_write(serialPort_, data, len, 100);
    if (result < 0)
    {
        markDisconnected("config write");
        isConfigMode_ = false;
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    isConfigMode_ = false;

    return true;
}

int EncoderDriver::readDataNonBlocking(uint8_t *buffer, size_t bufferSize)
{
    if (!isConnected_ || !serialPort_)
    {
        return -1;
    }

    // 使用非阻塞读取
    sp_return result = sp_nonblocking_read(serialPort_, buffer, bufferSize);

    if (result > 0)
    {
        // 成功读取数据
        return static_cast<int>(result);
    }
    else if (result == 0)
    {
        // 没有数据可读
        return 0;
    }
    else
    {
        // 读取错误
        markDisconnected("read");
        return -1;
    }
}

int EncoderDriver::parseReceivedData(uint8_t *data, size_t size)
{
    // 成功解析的帧数量
    int parsedFrames = 0;

    // 1. 追加到接收缓冲区
    rxBuffer_.insert(rxBuffer_.end(), data, data + size);

    // 2. 循环解析，直到 rxBuffer_ 不够一帧
    while (rxBuffer_.size() >= ENC_FRAME_MIN_LENGTH)
    {
        // (1) 同步帧头 —— 地址必须匹配
        if (rxBuffer_[0] != ENCODER_DEFAULT_ADDR)
        {
            rxBuffer_.erase(rxBuffer_.begin());
            continue;
        }

        // (2) 功能码：RD / WR
        uint8_t func = rxBuffer_[1];
        if (func != ENCODER_FUNC_RD && func != ENCODER_FUNC_WR)
        {
            rxBuffer_.erase(rxBuffer_.begin());
            continue;
        }

        // (3) Len
        uint8_t len = rxBuffer_[2];

        size_t frame_len = 0;

        // 读取数据帧（pos/speed）
        if (func == ENCODER_FUNC_RD)
        {
            if (!(len == 0x02 || len == 0x04))
            {
                rxBuffer_.erase(rxBuffer_.begin());
                continue;
            }

            frame_len = len + 5; // addr + func + len + payload + crc(2)
        }
        // 写配置响应帧（固定 8 字节）
        else if (func == ENCODER_FUNC_WR)
        {
            frame_len = 8;
        }

        // (4) 不够一帧 → 等下一次 read
        if (rxBuffer_.size() < frame_len)
            return parsedFrames;

        // (5) CRC 校验
        if (func == ENCODER_FUNC_RD)
        {
            if (!verifyCRC16(rxBuffer_.data(), 3 + len,
                             rxBuffer_[3 + len], rxBuffer_[4 + len]))
            {
                rxBuffer_.erase(rxBuffer_.begin());
                continue;
            }
        }

        // ------------------ 解析有效帧 ------------------

        if (func == ENCODER_FUNC_RD)
        {
            if (len == 0x02)
            {
                const auto hostTimestampNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());

                // Position
                int16_t raw_pos = (rxBuffer_[3] << 8) | rxBuffer_[4];
                int32_t pos = (raw_pos > ENCODER_PRECISION_HALF)
                                  ? (raw_pos - ENCODER_PRECISION)
                                  : raw_pos;

                std::lock_guard<std::mutex> lock(stateMutex_);
                currentState_.currentPosition = pos;
                currentState_.currentPositionRad =
                    pos * (2.0 * M_PI / ENCODER_PRECISION);

                if (pendingSamples_.size() >= kMaxPendingEncoderSamples)
                {
                    pendingSamples_.pop_front();
                    ++droppedSamples_;
                    if (droppedSamples_ == 1 || (droppedSamples_ % 256) == 0)
                    {
                        std::cerr << "[EncoderDriver] " << port_
                                  << " pending sample queue full, dropped oldest samples="
                                  << droppedSamples_ << std::endl;
                    }
                }

                EncoderQueuedSample queuedSample;
                queuedSample.state = currentState_;
                queuedSample.hostTimestampNs = hostTimestampNs;
                pendingSamples_.push_back(std::move(queuedSample));
            }
            else if (len == 0x04)
            {
                // Speed
                int32_t raw_speed = 0;
                memcpy(&raw_speed, &rxBuffer_[3], 4);
                raw_speed = SW32(raw_speed);

                std::lock_guard<std::mutex> lock(stateMutex_);
                currentState_.currentSpeed = raw_speed / 100.0f;
                currentState_.currentSpeedRad =
                    currentState_.currentSpeed * (2.0f * M_PI / 60.0f);
            }
        }
        else if (func == ENCODER_FUNC_WR)
        {
            // 写配置响应帧打印
            std::cout << "EncoderDriver " << name_ << " config response: ";
            for (size_t i = 0; i < frame_len; ++i)
                printf("%02X ", rxBuffer_[i]);
            printf("\n");
        }

        // 解析成功 → 计数 +1
        parsedFrames++;

        // 删除已解析的帧
        rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + frame_len);
    }

    return parsedFrames;
}

bool EncoderDriver::getEncoderPosition()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_RD,
        0x00,
        ENCODER_REGISTER_ANGLE_LOW,
        0x00,
        0x01,
        0xd4,
        0x1e};
    return sendToEncoder(sendBuf, sizeof(sendBuf));
}

bool EncoderDriver::getEncoderVelocity()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_RD,
        0x00,
        ENCODER_REGISTER_SPEED_HIGH,
        0x00,
        0x02,
        0x64,
        0x1f};
    return sendToEncoder(sendBuf, sizeof(sendBuf));
}

bool EncoderDriver::requestState(bool needSpeed)
{
    if (needSpeed)
    {
        getEncoderVelocity();
    }
    return getEncoderPosition();
}

EncoderData EncoderDriver::getState()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return currentState_;
}

bool EncoderDriver::tryConsumeSample(EncoderQueuedSample *out)
{
    if (out == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    if (pendingSamples_.empty())
    {
        return false;
    }

    *out = pendingSamples_.front();
    pendingSamples_.pop_front();
    return true;
}

bool EncoderDriver::calculateCRC16(uint8_t *data, uint8_t len, uint8_t *crcLow, uint8_t *crcHigh)
{
    uint16_t crc = Get_CRC16(data, len, CRC16Type::MODBUS);

    *crcLow = crc & 0xFF;
    *crcHigh = (crc >> 8) & 0xFF;
    return true;
}

bool EncoderDriver::verifyCRC16(uint8_t *data, uint8_t len, uint8_t crcLow, uint8_t crcHigh)
{
    uint8_t calcLow, calcHigh;
    calculateCRC16(data, len, &calcLow, &calcHigh);
    return (calcLow == crcLow) && (calcHigh == crcHigh);
}

bool EncoderDriver::setDeviceAddress(uint8_t addr)
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_SET_ADDR,
        0x00,
        addr,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    sendConfigToEncoder(sendBuf, sizeof(sendBuf));

    return setRestart();
}

bool EncoderDriver::setBaudrate(uint32_t baudrate)
{
    uint8_t type = 0xFF; // 默认无效

    switch (baudrate)
    {
    case 9600:
        type = 0x00;
        break;
    case 19200:
        type = 0x01;
        break;
    case 38400:
        type = 0x02;
        break;
    case 57600:
        type = 0x03;
        break;
    case 115200:
        type = 0x04;
        break;
    case 1000000:
        type = 0x07;
        break;
    default:
        std::cerr << "Unsupported baudrate: " << baudrate << std::endl;
        return false;
    }

    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_SET_BAUDRATE,
        0x00,
        type,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    sendConfigToEncoder(sendBuf, sizeof(sendBuf));

    return setRestart();
}

bool EncoderDriver::setCurrentAsMiddle()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_MID_SET,
        0x00,
        0x01,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    sendConfigToEncoder(sendBuf, sizeof(sendBuf));
    return setConfigStore();
}

bool EncoderDriver::setCurrentAsZero()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_ZERO_SET,
        0x00,
        0x01,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    sendConfigToEncoder(sendBuf, sizeof(sendBuf));
    return setConfigStore();
}

bool EncoderDriver::setRotationDirection(bool isInverted)
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_SET_DIR,
        0x00,
        static_cast<uint8_t>(isInverted ? 0x01 : 0x00),
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    return sendConfigToEncoder(sendBuf, sizeof(sendBuf));
}

bool EncoderDriver::setConfigStore()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_SET_STORE,
        0x00,
        0x01,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    return sendConfigToEncoder(sendBuf, sizeof(sendBuf));
}

bool EncoderDriver::setRestart()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_RESTART,
        0x00,
        0x01,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    return sendConfigToEncoder(sendBuf, sizeof(sendBuf));
}

bool EncoderDriver::setRefactory()
{
    uint8_t sendBuf[8] = {
        ENCODER_DEFAULT_ADDR,
        ENCODER_FUNC_WR,
        0x00,
        ENCODER_REGISTER_REFACTORY,
        0x00,
        0x01,
        0x00, // Will be filled by CRC
        0x00  // Will be filled by CRC
    };

    calculateCRC16(sendBuf, 6, &sendBuf[6], &sendBuf[7]);
    return sendConfigToEncoder(sendBuf, sizeof(sendBuf));
}

bool EncoderDriver::reset()
{
    return setRestart();
}

int8_t EncoderDriver::updateActiveStatus()
{
    if (isActive_)
    {
        if (currentState_.linkSta.hb_snapshot != currentState_.linkSta.heartbeat)
        {
            currentState_.linkSta.hb_snapshot = currentState_.linkSta.heartbeat;
            currentState_.linkSta.is_active = 1;
        }
        else
        {
            currentState_.linkSta.is_active = 0;
        }
    }
    return 0;
}
