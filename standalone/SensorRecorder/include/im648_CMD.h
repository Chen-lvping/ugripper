/******************************************************************************
                        设备与IM648模块之间的串口通信库
*******************************************************************************/
#ifndef _im648_CMD_h
#define _im648_CMD_h

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

typedef signed char S8;
typedef unsigned char U8;
typedef signed short S16;
typedef unsigned short U16;
typedef signed int S32;
typedef unsigned int U32;
typedef float F32;

#define pow2(x) ((x) * (x))

// 传输时转换比例--------------
#define scaleAccel 0.00478515625f       // 加速度 [-16g~+16g]    9.8*16/32768
#define scaleQuat 0.000030517578125f    // 四元数 [-1~+1]         1/32768
#define scaleAngle 0.0054931640625f     // 角度   [-180~+180]     180/32768
#define scaleAngleSpeed 0.06103515625f  // 角速度 [-2000~+2000]   2000/32768
#define scaleMag 0.15106201171875f      // 磁场 [-4950~+4950]     4950/32768
#define scaleTemperature 0.01f          // 温度
#define scaleAirPressure 0.0002384185791f
#define scaleHeight 0.0010728836f

#define CmdPacket_Begin 0x49
#define CmdPacket_End 0x4D
#define CmdPacketMaxDatSizeRx 73
#define CmdPacketMaxDatSizeTx 31

namespace dmbot_serial {
struct IM648_Data;
}

using Im648WriteCallback = int (*)(const U8 *buf, int len, void *user_data);
using Im648SampleCallback = void (*)(const dmbot_serial::IM648_Data &sample, void *user_data);

struct Im648ProtocolContext {
    U8 targetDeviceAddress = 255;
    U8 rxChecksum = 0;
    U8 rxWriteIndex = 0;
    U8 rxState = 0;
    std::array<U8, 5 + CmdPacketMaxDatSizeRx> rxBuf{};
    dmbot_serial::IM648_Data *dataPtr = nullptr;
    bool *dataUpdatedPtr = nullptr;
    std::mutex *dataMutex = nullptr;
    Im648SampleCallback sampleCallback = nullptr;
    void *sampleUserData = nullptr;
    Im648WriteCallback writeCallback = nullptr;
    void *writeUserData = nullptr;
};

void im648_InitContext(Im648ProtocolContext *ctx,
                       dmbot_serial::IM648_Data *data_ptr,
                       bool *data_updated_ptr,
                       std::mutex *data_mutex,
                       Im648SampleCallback sample_callback,
                       void *sample_user_data,
                       Im648WriteCallback write_callback,
                       void *write_user_data);

int im648_SendCommand(Im648ProtocolContext *ctx, const U8 *payload, U8 payload_len);
U8 im648_Cmd_GetPkt(Im648ProtocolContext *ctx, U8 byte);

void im648_Cmd_03(Im648ProtocolContext *ctx);  // 唤醒传感器
void im648_Cmd_12(Im648ProtocolContext *ctx,
                  U8 accStill,
                  U8 stillToZero,
                  U8 moveToZero,
                  U8 isCompassOn,
                  U8 barometerFilter,
                  U8 reportHz,
                  U8 gyroFilter,
                  U8 accFilter,
                  U8 compassFilter,
                  U16 reportTag);
void im648_Cmd_19(Im648ProtocolContext *ctx);  // 开启数据主动上报

#endif
