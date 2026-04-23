#include "bsp_crc.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

TEST(BspCrcTest, CalculatesKnownModbusCrc)
{
    std::array<uint8_t, 6> request = {0x01, 0x03, 0x00, 0x41, 0x00, 0x01};

    const uint16_t crc = Get_CRC16(request.data(),
                                   static_cast<uint16_t>(request.size()),
                                   CRC16Type::MODBUS);

    EXPECT_EQ(crc, 0x1ED4);
}

TEST(BspCrcTest, CalculatesKnownIso13239Crc)
{
    std::array<uint8_t, 4> frame = {0x12, 0x34, 0x56, 0x78};

    const uint16_t crc = Get_CRC16(frame.data(),
                                   static_cast<uint16_t>(frame.size()),
                                   CRC16Type::ISO13239);

    EXPECT_EQ(crc, 0x7311);
}

}  // namespace
