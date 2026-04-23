#ifndef ENCODER_DRIVER_HPP
#define ENCODER_DRIVER_HPP

#include <libserialport.h>
#include <cstdint>
#include <string>
#include <cstring>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <iostream>
#include <cmath>
#include <deque>
#include <vector>
#include "bsp_crc.h"
#include "sensor_recorder/sensor_protocol.h"

// Constants
#define MAX_ENCODER_NUM 56 // 8 DOF per ARM
#define MAX_EVENTS_NUM MAX_ENCODER_NUM * 2
#define ENCODER_PRECISION 65536 // 1024 for 10bits， 65536 for 16bits
#define ENCODER_PRECISION_HALF 32768

#define ENC_FRAME_MIN_LENGTH 7
#define ENC_FRAME_MAX_LENGTH 10

// MODBUS FRAME SEND:[ADDR] [FUNC CODE] [REGISTER] [CRC16]
// MODBUS FRAME RECV:[ADDR] [FUNC CODE=0X03] [DATA LENGTH] [DATA] [CRC16]
// MODBUS FRAME RECV:[ADDR] [FUNC CODE=0X06] [REGISTER] [CRC16]
#define ENCODER_DEFAULT_ADDR 0x01
#define ENCODER_FUNC_RD 0x03
#define ENCODER_FUNC_WR 0x06

// read:func 0x03
#define ENCODER_REGISTER_ANGLE_HIGH 0x40 // 2bytes
#define ENCODER_REGISTER_ANGLE_LOW 0x41  // 2bytes

#define ENCODER_REGISTER_SPEED_HIGH 0x42 // 2bytes
#define ENCODER_REGISTER_SPEED_LOW 0x43  // 2bytes

#define ENCODER_REGISTER_TURNS 0x44 // 2bytes

#define ENCODER_REGISTER_MULTITURN_HIGH 0x46 // 2bytes
#define ENCODER_REGISTER_MULTITURN_LOW 0x47  // 2bytes

// write:func 0x06
#define ENCODER_REGISTER_MID_SET 0x51
#define ENCODER_REGISTER_ZERO_SET 0x52
#define ENCODER_REGISTER_REFACTORY 0x53
#define ENCODER_REGISTER_RESTART 0x54
#define ENCODER_REGISTER_SET_STORE 0x55

#define ENCODER_REGISTER_SET_BAUDRATE 0x00
#define ENCODER_REGISTER_SET_ADDR 0x05 // range：1~110
#define ENCODER_REGISTER_SET_DIR 0x06  // 0X01：CW ADD  0X00：CCW ADD

// 取数据 SWAP_ENDIAN_32_SIGNED & SWAP_ENDIAN_16_SIGNED
#define SW16(A) ((int16_t)((((uint16_t)(A) & 0x00ff) << 8) | (((uint16_t)(A) & 0xff00) >> 8)))
#define SW32(A) ((int32_t)((((uint32_t)(A) & 0xff000000) >> 24)) | (((uint32_t)(A) & 0x00ff0000) >> 8) | (((uint32_t)(A) & 0x0000ff00) << 8) | (((uint32_t)(A) & 0x000000ff) << 24))

// Life status structure
struct LifeStatus
{
    uint16_t heartbeat;
    uint16_t hb_snapshot;
    uint8_t is_active;
};

// Encoder data structure
struct EncoderData
{
    uint8_t com;
    uint8_t id;
    int32_t currentPosition;
    float currentPositionRad;
    int32_t currentSpeed;
    float currentSpeedRad;
    int32_t zeroOffset;
    uint16_t turnNum;
    uint8_t rotationDir;
    LifeStatus linkSta;
};

struct EncoderQueuedSample
{
    EncoderData state;
    uint64_t hostTimestampNs = 0;
};

enum class ConnectStatus
{
    SUCCESS,     // 编码器成功响应
    SERIAL_FAIL, // 串口打开失败
    NO_RESPONSE  // 编码器未响应
};

class EncoderDriver
{
public:
    using DataCallback = std::function<void(const uint8_t *data, size_t size)>;

    EncoderDriver(uint8_t serialNum, const std::string &port, uint32_t baudrate, const std::string &name = "EncoderDriver");
    ~EncoderDriver();

    // Connection management
    ConnectStatus connect();
    void disconnect();
    void resetBaudrate(uint32_t baudrate);
    bool isConnected() const { return isConnected_; }

    // Device reset
    bool reset();

    // State management
    EncoderData getState();
    bool tryConsumeSample(EncoderQueuedSample *out);
    bool requestState(bool needSpeed = false);
    int8_t updateActiveStatus();

    // Configuration methods
    bool setRefactory();
    bool setRestart();
    bool setBaudrate(uint32_t baudrate);
    bool setDeviceAddress(uint8_t addr);
    bool setRotationDirection(bool isInverted);
    bool setCurrentAsZero();
    bool setCurrentAsMiddle();
    bool setConfigStore();

    // Data reception parsing
    int readDataNonBlocking(uint8_t *buffer, size_t bufferSize);
    int parseReceivedData(uint8_t *data, size_t size);

    // Getters
    std::string getName() const { return name_; }
    std::string getPort() const { return port_; }
    uint32_t getBaudrate() const { return baudrate_; }
    uint8_t getSerialNumber() const { return serialNum_; }
    sp_port *getSerialPort() const { return serialPort_; }

private:
    bool sendToEncoder(uint8_t *data, uint8_t len);
    bool sendConfigToEncoder(uint8_t *data, uint8_t len);
    bool getEncoderPosition();
    bool getEncoderVelocity();
    void markDisconnected(const std::string &operation);

    bool calculateCRC16(uint8_t *data, uint8_t len, uint8_t *crcLow, uint8_t *crcHigh);
    bool verifyCRC16(uint8_t *data, uint8_t len, uint8_t crcLow, uint8_t crcHigh);

    std::string name_;
    std::string port_;
    uint32_t baudrate_;
    uint8_t serialNum_;

    struct sp_port *serialPort_;

    mutable std::mutex stateMutex_;
    EncoderData currentState_;

    std::atomic<bool> isConnected_{false};
    std::atomic<bool> isConfigMode_{false};
    std::atomic<bool> isActive_{false};

    int debugCounter_ = 0;
    std::vector<uint8_t> rxBuffer_;
    std::deque<EncoderQueuedSample> pendingSamples_;
    size_t droppedSamples_ = 0;
};

#endif // ENCODER_DRIVER_HPP
