#include <cstdio>
#include <cstring>
#include <cassert>
#include "bsp_crc.h"

// 测试数据
const uint8_t test_data1[] = {0x01, 0x02, 0x03, 0x04, 0x05};
const uint8_t test_data2[] = "123456789";
const uint8_t test_data3[] = {0x00, 0x00, 0x00, 0x00};
const uint8_t test_data4[] = {0xFF, 0xFF, 0xFF, 0xFF};

// 已知的正确CRC值（需要根据实际算法计算或参考标准值）
// 这些值需要根据实际CRC算法进行验证和更新

void test_crc8()
{
    printf("=== CRC8 Testing ===\n");

    // 测试用例1
    uint8_t crc1 = Get_CRC8(0x00, (uint8_t *)test_data1, sizeof(test_data1));
    printf("Test Data 1 CRC8: 0x%02X\n", crc1);

    // 测试用例2
    uint8_t crc2 = Get_CRC8(0x00, (uint8_t *)test_data2, sizeof(test_data2) - 1); // -1 to exclude null terminator
    printf("Test Data 2 CRC8: 0x%02X\n", crc2);

    // 测试用例3 - 全零数据
    uint8_t crc3 = Get_CRC8(0x00, (uint8_t *)test_data3, sizeof(test_data3));
    printf("Test Data 3 (zeros) CRC8: 0x%02X\n", crc3);

    // 测试用例4 - 全FF数据
    uint8_t crc4 = Get_CRC8(0x00, (uint8_t *)test_data4, sizeof(test_data4));
    printf("Test Data 4 (ones) CRC8: 0x%02X\n", crc4);

    // 测试初始值影响
    uint8_t crc5 = Get_CRC8(0xFF, (uint8_t *)test_data1, sizeof(test_data1));
    printf("Test Data 1 with init 0xFF CRC8: 0x%02X\n", crc5);

    printf("\n");
}

void test_crc16_modbus()
{
    printf("=== CRC16 MODBUS Testing ===\n");

    // MODBUS CRC16测试用例
    uint16_t crc1 = Get_CRC16((uint8_t *)test_data1, sizeof(test_data1), CRC16Type::MODBUS);
    printf("Test Data 1 MODBUS CRC16: 0x%04X\n", crc1);

    uint16_t crc2 = Get_CRC16((uint8_t *)test_data2, sizeof(test_data2) - 1, CRC16Type::MODBUS);
    printf("Test Data 2 MODBUS CRC16: 0x%04X\n", crc2);

    // 空数据测试
    uint16_t crc3 = Get_CRC16((uint8_t *)test_data3, sizeof(test_data3), CRC16Type::MODBUS);
    printf("Test Data 3 (zeros) MODBUS CRC16: 0x%04X\n", crc3);

    uint16_t crc4 = Get_CRC16((uint8_t *)test_data4, sizeof(test_data4), CRC16Type::MODBUS);
    printf("Test Data 4 (ones) MODBUS CRC16: 0x%04X\n", crc4);

    printf("\n");
}

void test_crc16_iso13239()
{
    printf("=== CRC16 ISO13239 Testing ===\n");

    // ISO13239 CRC16测试用例
    uint16_t crc1 = Get_CRC16((uint8_t *)test_data1, sizeof(test_data1), CRC16Type::ISO13239);
    printf("Test Data 1 ISO13239 CRC16: 0x%04X\n", crc1);

    uint16_t crc2 = Get_CRC16((uint8_t *)test_data2, sizeof(test_data2) - 1, CRC16Type::ISO13239);
    printf("Test Data 2 ISO13239 CRC16: 0x%04X\n", crc2);

    // 空数据测试
    uint16_t crc3 = Get_CRC16((uint8_t *)test_data3, sizeof(test_data3), CRC16Type::ISO13239);
    printf("Test Data 3 (zeros) ISO13239 CRC16: 0x%04X\n", crc3);

    uint16_t crc4 = Get_CRC16((uint8_t *)test_data4, sizeof(test_data4), CRC16Type::ISO13239);
    printf("Test Data 4 (ones) ISO13239 CRC16: 0x%04X\n", crc4);

    printf("\n");
}

void test_consistency()
{
    printf("=== Consistency Testing ===\n");

    // 测试相同数据在不同时间计算的结果是否一致
    uint8_t data[] = {0x11, 0x22, 0x33, 0x44, 0x55};

    uint8_t crc8_1 = Get_CRC8(0x00, data, sizeof(data));
    uint8_t crc8_2 = Get_CRC8(0x00, data, sizeof(data));
    printf("CRC8 Consistency: %s\n", crc8_1 == crc8_2 ? "PASS" : "FAIL");

    uint16_t crc16_modbus_1 = Get_CRC16(data, sizeof(data), CRC16Type::MODBUS);
    uint16_t crc16_modbus_2 = Get_CRC16(data, sizeof(data), CRC16Type::MODBUS);
    printf("CRC16 MODBUS Consistency: %s\n", crc16_modbus_1 == crc16_modbus_2 ? "PASS" : "FAIL");

    uint16_t crc16_iso_1 = Get_CRC16(data, sizeof(data), CRC16Type::ISO13239);
    uint16_t crc16_iso_2 = Get_CRC16(data, sizeof(data), CRC16Type::ISO13239);
    printf("CRC16 ISO13239 Consistency: %s\n", crc16_iso_1 == crc16_iso_2 ? "PASS" : "FAIL");

    printf("\n");
}

void test_edge_cases()
{
    printf("=== Edge Cases Testing ===\n");

    // 空数据测试
    uint8_t crc8_empty = Get_CRC8(0x00, nullptr, 0);
    printf("Empty data CRC8: 0x%02X\n", crc8_empty);

    uint16_t crc16_modbus_empty = Get_CRC16(nullptr, 0, CRC16Type::MODBUS);
    printf("Empty data MODBUS CRC16: 0x%04X\n", crc16_modbus_empty);

    uint16_t crc16_iso_empty = Get_CRC16(nullptr, 0, CRC16Type::ISO13239);
    printf("Empty data ISO13239 CRC16: 0x%04X\n", crc16_iso_empty);

    // 单字节测试
    uint8_t single_byte = 0xAA;
    uint8_t crc8_single = Get_CRC8(0x00, &single_byte, 1);
    printf("Single byte CRC8: 0x%02X\n", crc8_single);

    uint16_t crc16_modbus_single = Get_CRC16(&single_byte, 1, CRC16Type::MODBUS);
    printf("Single byte MODBUS CRC16: 0x%04X\n", crc16_modbus_single);

    uint16_t crc16_iso_single = Get_CRC16(&single_byte, 1, CRC16Type::ISO13239);
    printf("Single byte ISO13239 CRC16: 0x%04X\n", crc16_iso_single);

    printf("\n");
}

// 验证已知的标准测试向量
void test_known_vectors()
{
    printf("=== Known Test Vectors ===\n");

    // 标准CRC测试："123456789" 的CRC结果
    // 注意：这些值需要根据实际的CRC多项式进行验证
    const uint8_t standard_test[] = "123456789";

    uint8_t crc8_result = Get_CRC8(0x00, (uint8_t *)standard_test, 9);
    printf("Standard test '123456789' CRC8: 0x%02X\n", crc8_result);

    uint16_t crc16_modbus_result = Get_CRC16((uint8_t *)standard_test, 9, CRC16Type::MODBUS);
    printf("Standard test '123456789' MODBUS CRC16: 0x%04X\n", crc16_modbus_result);

    uint16_t crc16_iso_result = Get_CRC16((uint8_t *)standard_test, 9, CRC16Type::ISO13239);
    printf("Standard test '123456789' ISO13239 CRC16: 0x%04X\n", crc16_iso_result);

    printf("\n");
}

int main()
{
    printf("CRC Algorithm Test Suite\n\n");

    test_crc8();
    test_crc16_modbus();
    test_crc16_iso13239();
    test_consistency();
    test_edge_cases();
    test_known_vectors();

    printf("All tests completed.\n");
    return 0;
}