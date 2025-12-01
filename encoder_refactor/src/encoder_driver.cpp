#include "encoder_driver.h"
#include <thread>
#include <chrono>

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

bool EncoderDriver::connect()
{
    if (isConnected_)
    {
        return true;
    }

    sp_return result = sp_get_port_by_name(port_.c_str(), &serialPort_);
    if (result != SP_OK)
    {
        std::cerr << "EncoderDriver: Cannot find port " << port_ << std::endl;
        return false;
    }

    result = sp_open(serialPort_, SP_MODE_READ_WRITE);
    if (result != SP_OK)
    {
        std::cerr << "EncoderDriver: Cannot open port " << port_ << std::endl;
        sp_free_port(serialPort_);
        serialPort_ = nullptr;
        return false;
    }

    // Configure serial port
    sp_set_baudrate(serialPort_, baudrate_);
    sp_set_bits(serialPort_, 8);
    sp_set_parity(serialPort_, SP_PARITY_NONE);
    sp_set_stopbits(serialPort_, 1);
    sp_set_flowcontrol(serialPort_, SP_FLOWCONTROL_NONE);

    isConnected_ = true;
    isActive_ = true;

    std::cout << "EncoderDriver: Connected to " << port_ << " at " << baudrate_ << " baud" << std::endl;
    return true;
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

bool EncoderDriver::sendToEncoder(uint8_t *data, uint8_t len)
{
    if (!isConnected_ || isConfigMode_)
    {
        return false;
    }

    sp_return result = sp_blocking_write(serialPort_, data, len, 100);
    if (result < 0)
    {
        std::cerr << "EncoderDriver: Write error on " << port_ << std::endl;
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
        std::cerr << "EncoderDriver: Write error on " << port_ << std::endl;
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
        std::cerr << "EncoderDriver: Read error on " << port_ << std::endl;
        return -1;
    }
}

void EncoderDriver::parseReceivedData(uint8_t *data, size_t size)
{
    uint8_t temp_frame[ENC_FRAME_MAX_LENGTH] = {0};
    size_t index = 0;
    int need_read = size - ENC_FRAME_MIN_LENGTH + 1;

    // std::cout << "serial  exec ----------------------------------------------------------------------" << std::endl;

    while (need_read > 0)
    {
        if ((data[index] == ENCODER_DEFAULT_ADDR) && (data[index + 1] == ENCODER_FUNC_RD))
        {
            uint8_t len = data[index + 2];

            if (len == 0x02)
            {
                if (index + len + 5 <= size)
                {
                    memcpy(temp_frame, data + index, len + 5);

                    if (verifyCRC16(temp_frame, 3 + len, temp_frame[3 + len], temp_frame[4 + len]))
                    {
                        int32_t temp_pos = (temp_frame[3] << 8) | temp_frame[4];

                        std::lock_guard<std::mutex> lock(stateMutex_);
                        currentState_.currentPosition = (temp_pos > ENCODER_PRECISION_HALF) ? (temp_pos - ENCODER_PRECISION) : temp_pos;
                        currentState_.currentPositionRad = static_cast<float>(currentState_.currentPosition) *
                                                           M_PI * 2.0f / ENCODER_PRECISION;

                        need_read -= (4 + len);
                        index += (4 + len);
                        continue;
                    }
                    else
                    {
                        std::cout << "crc failed" << std::endl;
                    }
                }
            }
            else if (len == 0x04)
            {
                if (index + len + 5 <= size)
                {
                    memcpy(temp_frame, data + index, len + 5);

                    if (verifyCRC16(temp_frame, 3 + len, temp_frame[3 + len], temp_frame[4 + len]))
                    {
                        int32_t temp_data_32 = 0;
                        memcpy(&temp_data_32, temp_frame + 3, sizeof(int32_t));

                        std::lock_guard<std::mutex> lock(stateMutex_);
                        currentState_.currentSpeed = SW32(temp_data_32) / 100;
                        currentState_.currentSpeedRad = static_cast<float>(currentState_.currentSpeed) *
                                                        M_PI * 2.0f / 60.0f;

                        need_read -= (4 + len);
                        index += (4 + len);
                        continue;
                    }
                }
            }
        }
        else if ((data[index] == ENCODER_DEFAULT_ADDR) &&
                 (data[index + 1] == ENCODER_FUNC_WR) &&
                 (data[index + 2] == 0x00))
        {
            // Configuration response - log for debugging
            if (index + 8 <= size)
            {
                memcpy(temp_frame, data + index, 8);
                std::cout << "EncoderDriver " << name_ << " config response: ";
                for (int i = 0; i < 8; i++)
                {
                    printf("%02x ", temp_frame[i]);
                }
                printf("\n");
            }
        }

        index++;
        need_read--;
    }
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

bool EncoderDriver::setBaudrate(uint16_t baudrate)
{
    uint8_t type = 4; // 115200 baud
    // Add baudrate to type mapping as needed

    if (type >= 5)
    {
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
    return sendConfigToEncoder(sendBuf, sizeof(sendBuf));
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
    return sendConfigToEncoder(sendBuf, sizeof(sendBuf));
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