#include "gripper_hmi_protocol.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

std::vector<uint8_t> MakeRecvFrame(uint8_t index_word,
                                   uint8_t data3,
                                   uint8_t data4,
                                   uint8_t data5,
                                   uint8_t data6)
{
    std::vector<uint8_t> frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        index_word,
        data3,
        data4,
        data5,
        data6,
        0x00,
    };
    frame.back() = GripperHmiProtocol::calculateXor(frame.data(), GripperHmiProtocol::kFrameLength - 1);
    return frame;
}

TEST(GripperHmiTransportResyncTest, SkipsBadChecksumAndResyncsToNextFrame)
{
    auto corrupted = MakeRecvFrame(GripperHmiProtocol::kIndexKeyReport, 0x00, 0x00, 0x00, 0x01);
    corrupted.back() ^= 0xFF;

    auto valid = MakeRecvFrame(GripperHmiProtocol::kIndexKeyReport, 0x00, 0x00, 0x00, 0x02);

    std::vector<uint8_t> buffer;
    buffer.insert(buffer.end(), corrupted.begin(), corrupted.end());
    buffer.insert(buffer.end(), valid.begin(), valid.end());

    const auto frame = GripperHmiProtocol::tryConsumeFrame(buffer);

    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->type, GripperFrameType::KeyReport);
    EXPECT_EQ(frame->keyReport.rawCode, 0x02);
    EXPECT_EQ(frame->keyReport.keyIndex, 1);
    EXPECT_TRUE(frame->keyReport.pressed);
    EXPECT_TRUE(buffer.empty());
}

TEST(GripperHmiTransportResyncTest, WaitsForFullFrameBeforeParsing)
{
    const auto full_frame = MakeRecvFrame(GripperHmiProtocol::kIndexBeepBase, 0x00, 0x32, 0x03, 0xE8);

    std::vector<uint8_t> buffer(full_frame.begin(), full_frame.begin() + 5);
    EXPECT_FALSE(GripperHmiProtocol::tryConsumeFrame(buffer).has_value());
    EXPECT_EQ(buffer.size(), 5U);

    buffer.insert(buffer.end(), full_frame.begin() + 5, full_frame.end());
    const auto parsed = GripperHmiProtocol::tryConsumeFrame(buffer);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, GripperFrameType::BeepState);
    EXPECT_EQ(parsed->beepState.duty, 0x0032);
    EXPECT_EQ(parsed->beepState.frequency, 0x03E8);
    EXPECT_TRUE(buffer.empty());
}

}  // namespace
