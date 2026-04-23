#include "sensor_recorder/sensor_protocol.h"

#include <gtest/gtest.h>

#include <array>

namespace {

TEST(SensorProtocolTest, DetectsReadFrameLengthForPositionResponse)
{
    const std::array<uint8_t, 3> header = {0x01, 0x03, 0x02};

    const auto frameLength = ugripper::sensor::TryGetEncoderResponseFrameLength(header);

    ASSERT_TRUE(frameLength.has_value());
    EXPECT_EQ(*frameLength, 7U);
}

TEST(SensorProtocolTest, DetectsWriteAckFrameLength)
{
    const std::array<uint8_t, 3> header = {0x01, 0x06, 0x00};

    const auto frameLength = ugripper::sensor::TryGetEncoderResponseFrameLength(header);

    ASSERT_TRUE(frameLength.has_value());
    EXPECT_EQ(*frameLength, 8U);
}

TEST(SensorProtocolTest, RejectsUnsupportedReadPayloadLength)
{
    const std::array<uint8_t, 3> header = {0x01, 0x03, 0x03};

    EXPECT_FALSE(ugripper::sensor::TryGetEncoderResponseFrameLength(header).has_value());
}

TEST(SensorProtocolTest, VerifiesModbusCrcBytes)
{
    const std::array<uint8_t, 6> request = {0x01, 0x03, 0x00, 0x41, 0x00, 0x01};

    EXPECT_TRUE(ugripper::sensor::VerifyCrc16Modbus(request, 0xD4, 0x1E));
    EXPECT_FALSE(ugripper::sensor::VerifyCrc16Modbus(request, 0x00, 0x00));
}

}  // namespace
