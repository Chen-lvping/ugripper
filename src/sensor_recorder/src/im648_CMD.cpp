#include "im648_CMD.h"

#include "im648_driver.h"

#include <chrono>
#include <cmath>

namespace {

U8 calcSum(const U8 *buf, int len) {
    U8 sum = 0;
    while (len-- > 0) {
        sum += buf[len];
    }
    return sum;
}

void copyBytes(void *dst, const void *src, unsigned int len) {
    auto *out = static_cast<unsigned char *>(dst);
    const auto *in = static_cast<const unsigned char *>(src);
    for (unsigned int i = 0; i < len; ++i) {
        out[i] = in[i];
    }
}

void unpackReport(Im648ProtocolContext *ctx, U8 *buf, U8 data_len) {
    if (ctx == nullptr || ctx->dataPtr == nullptr || ctx->dataUpdatedPtr == nullptr ||
        ctx->dataMutex == nullptr || data_len < 7) {
        return;
    }

    dmbot_serial::IM648_Data next;
    {
        std::lock_guard<std::mutex> lock(*ctx->dataMutex);
        next = *ctx->dataPtr;
    }

    const U16 ctl = static_cast<U16>((static_cast<U16>(buf[2]) << 8) | buf[1]);
    U8 cursor = 7;
    F32 tmp_x = 0.0f;
    F32 tmp_y = 0.0f;
    F32 tmp_z = 0.0f;
    F32 tmp_abs = 0.0f;
    const auto hasBytes = [data_len](U8 current_cursor, U8 need) {
        return static_cast<unsigned int>(current_cursor) + static_cast<unsigned int>(need) <= data_len;
    };

    if ((ctl & 0x0001) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 6);
    }
    if ((ctl & 0x0002) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        tmp_x = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAccel;
        cursor = static_cast<U8>(cursor + 2);
        tmp_y = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAccel;
        cursor = static_cast<U8>(cursor + 2);
        tmp_z = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAccel;
        cursor = static_cast<U8>(cursor + 2);
        next.accx = tmp_x;
        next.accy = tmp_y;
        next.accz = tmp_z;
    }
    if ((ctl & 0x0004) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        tmp_x = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAngleSpeed;
        cursor = static_cast<U8>(cursor + 2);
        tmp_y = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAngleSpeed;
        cursor = static_cast<U8>(cursor + 2);
        tmp_z = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAngleSpeed;
        cursor = static_cast<U8>(cursor + 2);
        next.gyrox = tmp_x * 0.0174532925f;
        next.gyroy = tmp_y * 0.0174532925f;
        next.gyroz = tmp_z * 0.0174532925f;
    }
    if ((ctl & 0x0008) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 6);
    }
    if ((ctl & 0x0010) != 0) {
        if (!hasBytes(cursor, 8)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 8);
    }
    if ((ctl & 0x0020) != 0) {
        if (!hasBytes(cursor, 8)) {
            return;
        }
        tmp_abs = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleQuat;
        cursor = static_cast<U8>(cursor + 2);
        tmp_x = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleQuat;
        cursor = static_cast<U8>(cursor + 2);
        tmp_y = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleQuat;
        cursor = static_cast<U8>(cursor + 2);
        tmp_z = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleQuat;
        cursor = static_cast<U8>(cursor + 2);
        next.quat_w = tmp_abs;
        next.quat_x = tmp_x;
        next.quat_y = tmp_y;
        next.quat_z = tmp_z;
    }
    if ((ctl & 0x0040) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        tmp_x = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAngle;
        cursor = static_cast<U8>(cursor + 2);
        tmp_y = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAngle;
        cursor = static_cast<U8>(cursor + 2);
        tmp_z = static_cast<S16>((static_cast<S16>(buf[cursor + 1]) << 8) | buf[cursor]) * scaleAngle;
        cursor = static_cast<U8>(cursor + 2);
        next.roll = tmp_x;
        next.pitch = tmp_y;
        next.yaw = tmp_z;
    }
    if ((ctl & 0x0080) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 6);
    }
    if ((ctl & 0x0100) != 0) {
        if (!hasBytes(cursor, 5)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 5);
    }
    if ((ctl & 0x0200) != 0) {
        if (!hasBytes(cursor, 6)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 6);
    }
    if ((ctl & 0x0400) != 0) {
        if (!hasBytes(cursor, 2)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 2);
    }
    if ((ctl & 0x0800) != 0) {
        if (!hasBytes(cursor, 1)) {
            return;
        }
        cursor = static_cast<U8>(cursor + 1);
    }
    next.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    {
        std::lock_guard<std::mutex> lock(*ctx->dataMutex);
        *ctx->dataPtr = next;
        *ctx->dataUpdatedPtr = true;
    }
}

void unpackPacket(Im648ProtocolContext *ctx, U8 *buf, U8 data_len) {
    if (ctx == nullptr || data_len == 0) {
        return;
    }

    switch (buf[0]) {
    case 0x11:
        unpackReport(ctx, buf, data_len);
        break;
    default:
        break;
    }
}

}  // namespace

void im648_InitContext(Im648ProtocolContext *ctx,
                       dmbot_serial::IM648_Data *data_ptr,
                       bool *data_updated_ptr,
                       std::mutex *data_mutex,
                       Im648WriteCallback write_callback,
                       void *write_user_data) {
    if (ctx == nullptr) {
        return;
    }

    *ctx = Im648ProtocolContext{};
    ctx->dataPtr = data_ptr;
    ctx->dataUpdatedPtr = data_updated_ptr;
    ctx->dataMutex = data_mutex;
    ctx->writeCallback = write_callback;
    ctx->writeUserData = write_user_data;
}

int im648_SendCommand(Im648ProtocolContext *ctx, const U8 *payload, U8 payload_len) {
    if (ctx == nullptr || payload == nullptr || payload_len == 0 || payload_len > CmdPacketMaxDatSizeTx ||
        ctx->writeCallback == nullptr) {
        return -1;
    }

    U8 buf[50 + 5 + CmdPacketMaxDatSizeTx] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff};

    buf[50] = CmdPacket_Begin;
    buf[51] = ctx->targetDeviceAddress;
    buf[52] = payload_len;
    copyBytes(&buf[53], payload, payload_len);
    buf[53 + payload_len] = calcSum(&buf[51], payload_len + 2);
    buf[54 + payload_len] = CmdPacket_End;

    return ctx->writeCallback(buf, payload_len + 55, ctx->writeUserData);
}

U8 im648_Cmd_GetPkt(Im648ProtocolContext *ctx, U8 byte) {
    if (ctx == nullptr) {
        return 0;
    }

    ctx->rxChecksum = static_cast<U8>(ctx->rxChecksum + byte);

    switch (ctx->rxState) {
    case 0:
        if (byte == CmdPacket_Begin) {
            ctx->rxWriteIndex = 0;
            ctx->rxBuf[ctx->rxWriteIndex++] = CmdPacket_Begin;
            ctx->rxChecksum = 0;
            ctx->rxState = 1;
        }
        break;
    case 1:
        ctx->rxBuf[ctx->rxWriteIndex++] = byte;
        if (byte == 255) {
            ctx->rxState = 0;
            break;
        }
        ctx->rxState = 2;
        break;
    case 2:
        ctx->rxBuf[ctx->rxWriteIndex++] = byte;
        if (byte == 0 || byte > CmdPacketMaxDatSizeRx) {
            ctx->rxState = 0;
            break;
        }
        ctx->rxState = 3;
        break;
    case 3:
        ctx->rxBuf[ctx->rxWriteIndex++] = byte;
        if (ctx->rxWriteIndex >= static_cast<U8>(ctx->rxBuf[2] + 3)) {
            ctx->rxState = 4;
        }
        break;
    case 4:
        ctx->rxChecksum = static_cast<U8>(ctx->rxChecksum - byte);
        if (ctx->rxChecksum == byte) {
            ctx->rxBuf[ctx->rxWriteIndex++] = byte;
            ctx->rxState = 5;
        } else {
            ctx->rxState = 0;
        }
        break;
    case 5:
        ctx->rxState = 0;
        if (byte == CmdPacket_End) {
            ctx->rxBuf[ctx->rxWriteIndex++] = byte;
            const U8 cmd_address = ctx->rxBuf[1];
            if (ctx->targetDeviceAddress == 255 || ctx->targetDeviceAddress == cmd_address) {
                unpackPacket(ctx, &ctx->rxBuf[3], static_cast<U8>(ctx->rxWriteIndex - 5));
                return 1;
            }
        }
        break;
    default:
        ctx->rxState = 0;
        break;
    }

    return 0;
}

void im648_Cmd_03(Im648ProtocolContext *ctx) {
    const U8 payload[1] = {0x03};
    im648_SendCommand(ctx, payload, 1);
}

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
                  U16 reportTag) {
    U8 payload[11] = {0x12};
    payload[1] = accStill;
    payload[2] = stillToZero;
    payload[3] = moveToZero;
    payload[4] = static_cast<U8>(((barometerFilter & 3) << 1) | (isCompassOn & 1));
    payload[5] = reportHz;
    payload[6] = gyroFilter;
    payload[7] = accFilter;
    payload[8] = compassFilter;
    payload[9] = static_cast<U8>(reportTag & 0xff);
    payload[10] = static_cast<U8>((reportTag >> 8) & 0xff);
    im648_SendCommand(ctx, payload, 11);
}

void im648_Cmd_19(Im648ProtocolContext *ctx) {
    const U8 payload[1] = {0x19};
    im648_SendCommand(ctx, payload, 1);
}
