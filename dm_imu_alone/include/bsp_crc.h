#ifndef __BSP_CRC_H
#define __BSP_CRC_H

#include <cstdint>

enum class CRC16Type : uint8_t
{
    MODBUS,  // LSB-first, 高低字节交换
    ISO13239 // MSB-first
};

/**
 * @brief          计算CRC8
 * @param[in]      init_value: crc初始值
 * @param[in]      ptr: 数据指针
 * @param[in]      len: 校验长度
 * @retval         CRC-8值
 */
uint8_t Get_CRC8(uint8_t init_value, uint8_t *ptr, uint8_t len);

/**
 * @brief          计算CRC16
 * @param[in]      ptr: 数据指针
 * @param[in]      len: 校验长度
 * @param[in]      type: CRC16类型（MODBUS/ISO13239）
 * @retval         CRC-16值
 */
uint16_t Get_CRC16(uint8_t *ptr, uint16_t len, CRC16Type type);

#endif
