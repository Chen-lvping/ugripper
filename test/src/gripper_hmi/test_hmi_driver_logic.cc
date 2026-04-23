#include "gripper_hmi_driver_logic.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {

using gripper_hmi::driver_logic::CalibrationWriteChunkStatusAction;

TEST(GripperHmiDriverLogicTest, ClassifiesRetryableAndAbortRecoveryStatuses)
{
    EXPECT_TRUE(gripper_hmi::driver_logic::IsRetryableCalibrationStatus(GripperHmiProtocol::kStatusMissingData));
    EXPECT_TRUE(
        gripper_hmi::driver_logic::IsRetryableCalibrationStatus(GripperHmiProtocol::kStatusZeroDataChecksumError));
    EXPECT_TRUE(gripper_hmi::driver_logic::IsRetryableCalibrationStatus(GripperHmiProtocol::kStatusChecksumError));
    EXPECT_TRUE(gripper_hmi::driver_logic::IsRetryableCalibrationStatus(GripperHmiProtocol::kStatusAddressOutOfLimit));
    EXPECT_FALSE(gripper_hmi::driver_logic::IsRetryableCalibrationStatus(GripperHmiProtocol::kStatusInvalidCommand));

    EXPECT_TRUE(
        gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(GripperHmiProtocol::kStatusMissingData));
    EXPECT_TRUE(
        gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(GripperHmiProtocol::kStatusZeroDataChecksumError));
    EXPECT_FALSE(
        gripper_hmi::driver_logic::IsCalibrationAbortRecoveryStatus(GripperHmiProtocol::kStatusChecksumError));
}

TEST(GripperHmiDriverLogicTest, AcceptsCalibrationAckTokenForCurrentAndNextPacket)
{
    EXPECT_TRUE(gripper_hmi::driver_logic::IsCalibrationWriteAckToken(0, 0));
    EXPECT_TRUE(gripper_hmi::driver_logic::IsCalibrationWriteAckToken(0, 1));
    EXPECT_TRUE(gripper_hmi::driver_logic::IsCalibrationWriteAckToken(7, 7));
    EXPECT_TRUE(gripper_hmi::driver_logic::IsCalibrationWriteAckToken(7, 8));
    EXPECT_FALSE(gripper_hmi::driver_logic::IsCalibrationWriteAckToken(7, 6));
    EXPECT_FALSE(gripper_hmi::driver_logic::IsCalibrationWriteAckToken(7, 9));
}

TEST(GripperHmiDriverLogicTest, ParsesCalibrationHeaderAndComputesRequiredChunkCount)
{
    std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> raw = {};
    gripper_hmi::GripperCalibrationHeader header{};
    header.payloadSize = 432;
    std::memcpy(raw.data(), &header, sizeof(header));

    std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> received = {};
    received[0] = true;
    received[1] = true;

    gripper_hmi::GripperCalibrationHeader parsed{};
    size_t required_chunk_count = 0;
    ASSERT_TRUE(gripper_hmi::driver_logic::TryParseCalibrationHeader(raw, received, &parsed, &required_chunk_count));
    EXPECT_EQ(std::memcmp(parsed.magic, "UCAL", 4), 0);
    EXPECT_EQ(required_chunk_count, 27U);
}

TEST(GripperHmiDriverLogicTest, RejectsCalibrationHeaderWhenHeaderChunksMissingOrInvalid)
{
    std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> raw = {};
    gripper_hmi::GripperCalibrationHeader header{};
    std::memcpy(raw.data(), &header, sizeof(header));

    std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> received = {};
    received[0] = true;

    gripper_hmi::GripperCalibrationHeader parsed{};
    size_t required_chunk_count = 0;
    EXPECT_FALSE(gripper_hmi::driver_logic::TryParseCalibrationHeader(raw, received, &parsed, &required_chunk_count));

    received[1] = true;
    header.magic[0] = 'B';
    std::memcpy(raw.data(), &header, sizeof(header));
    EXPECT_FALSE(gripper_hmi::driver_logic::TryParseCalibrationHeader(raw, received, &parsed, &required_chunk_count));
}

TEST(GripperHmiDriverLogicTest, ChecksRequiredChunkCoverageAndSequentialFallbackGate)
{
    std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> received = {};
    received[0] = true;
    received[1] = true;
    received[2] = true;

    EXPECT_TRUE(gripper_hmi::driver_logic::HasAllCalibrationChunks(received, 3));
    EXPECT_FALSE(gripper_hmi::driver_logic::HasAllCalibrationChunks(received, 4));

    EXPECT_TRUE(gripper_hmi::driver_logic::ShouldAttemptSequentialCalibrationFallback(
        false, true, 4, received, GripperHmiProtocol::kStatusChecksumError));
    EXPECT_FALSE(gripper_hmi::driver_logic::ShouldAttemptSequentialCalibrationFallback(
        true, true, 4, received, GripperHmiProtocol::kStatusChecksumError));
    EXPECT_FALSE(gripper_hmi::driver_logic::ShouldAttemptSequentialCalibrationFallback(
        false, true, 3, received, GripperHmiProtocol::kStatusChecksumError));
    EXPECT_FALSE(gripper_hmi::driver_logic::ShouldAttemptSequentialCalibrationFallback(
        false, true, 4, received, GripperHmiProtocol::kStatusInvalidCommand));
}

TEST(GripperHmiDriverLogicTest, DecidesCalibrationRangeRetryActions)
{
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationRangeRetryAction(
            GripperHmiProtocol::kStatusMissingData, 0, 5),
        gripper_hmi::driver_logic::CalibrationReadRetryAction::AbortRecoverRetry);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationRangeRetryAction(
            GripperHmiProtocol::kStatusZeroDataChecksumError, 4, 5),
        gripper_hmi::driver_logic::CalibrationReadRetryAction::RetryAfterDelay);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationRangeRetryAction(
            GripperHmiProtocol::kStatusChecksumError, 1, 5),
        gripper_hmi::driver_logic::CalibrationReadRetryAction::RetryAfterDelay);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationRangeRetryAction(
            GripperHmiProtocol::kStatusInvalidCommand, 1, 5),
        gripper_hmi::driver_logic::CalibrationReadRetryAction::StopFailure);
}

TEST(GripperHmiDriverLogicTest, DecidesCalibrationWriteChunkStatusActions)
{
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(GripperHmiProtocol::kStatusOk, 0),
        CalibrationWriteChunkStatusAction::Ack);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(GripperHmiProtocol::kStatusMissingData, 0),
        CalibrationWriteChunkStatusAction::AbortAndRecover);
    EXPECT_EQ(gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(
                  GripperHmiProtocol::kStatusZeroDataChecksumError, 0),
              CalibrationWriteChunkStatusAction::Retry);
    EXPECT_EQ(gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(
                  GripperHmiProtocol::kStatusZeroDataChecksumError, 1),
              CalibrationWriteChunkStatusAction::AbortAndRecover);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(GripperHmiProtocol::kStatusChecksumError, 0),
        CalibrationWriteChunkStatusAction::Retry);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationWriteChunkStatus(GripperHmiProtocol::kStatusInvalidCommand, 0),
        CalibrationWriteChunkStatusAction::Fail);
}

TEST(GripperHmiDriverLogicTest, DecidesCalibrationChunkReadActions)
{
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationChunkReadAction(
            gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedData,
            GripperHmiProtocol::kCalibrationChunkSize,
            GripperHmiProtocol::kCalibrationChunkSize,
            GripperHmiProtocol::kStatusOk,
            0,
            8),
        gripper_hmi::driver_logic::CalibrationChunkReadAction::AcceptData);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationChunkReadAction(
            gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedStatus,
            0,
            GripperHmiProtocol::kCalibrationChunkSize,
            GripperHmiProtocol::kStatusMissingData,
            0,
            8),
        gripper_hmi::driver_logic::CalibrationChunkReadAction::AbortRecoverRetry);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationChunkReadAction(
            gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedStatus,
            0,
            GripperHmiProtocol::kCalibrationChunkSize,
            GripperHmiProtocol::kStatusZeroDataChecksumError,
            7,
            8),
        gripper_hmi::driver_logic::CalibrationChunkReadAction::RetryAfterDelay);
    EXPECT_EQ(
        gripper_hmi::driver_logic::DecideCalibrationChunkReadAction(
            gripper_hmi::driver_logic::ExclusiveFrameScanResult::NeedMoreData,
            0,
            GripperHmiProtocol::kCalibrationChunkSize,
            GripperHmiProtocol::kStatusOk,
            0,
            8),
        gripper_hmi::driver_logic::CalibrationChunkReadAction::StopFailure);
}

TEST(GripperHmiDriverLogicTest, NormalizesBufferToHeaderOrKeepsTrailingByte)
{
    std::vector<uint8_t> buffer = {0x10, 0x11, 0x12};
    EXPECT_FALSE(gripper_hmi::driver_logic::NormalizeBufferToRecvHeader(&buffer));
    ASSERT_EQ(buffer.size(), 1U);
    EXPECT_EQ(buffer[0], 0x12);

    buffer = {0x10, 0x11, GripperHmiProtocol::kRecvHead1, GripperHmiProtocol::kRecvHead2, 0x01};
    EXPECT_TRUE(gripper_hmi::driver_logic::NormalizeBufferToRecvHeader(&buffer));
    ASSERT_EQ(buffer.size(), 3U);
    EXPECT_EQ(buffer[0], GripperHmiProtocol::kRecvHead1);
    EXPECT_EQ(buffer[1], GripperHmiProtocol::kRecvHead2);
}

TEST(GripperHmiDriverLogicTest, ScansExpectedExclusiveStatusFrameWithSkipRules)
{
    std::vector<uint8_t> buffer = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x42,
        0x01,
        0x02,
        0x03,
        0x04,
        0x00,
    };
    buffer[7] = GripperHmiProtocol::calculateXor(buffer.data(), 7);
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanExpectedExclusiveStatusFrame(&buffer, 0x52, nullptr, nullptr),
        gripper_hmi::driver_logic::StatusFrameScanResult::ContinueScanning);
    EXPECT_TRUE(buffer.empty());

    std::vector<uint8_t> matched = {
        0x00,
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x52,
        GripperHmiProtocol::kStatusOk,
        0x00,
        0x00,
        0xA5,
        0x00,
    };
    matched.back() = GripperHmiProtocol::calculateXor(matched.data() + 1, 7);
    uint8_t token = 0;
    uint8_t status = 0;
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanExpectedExclusiveStatusFrame(&matched, 0x52, &token, &status),
        gripper_hmi::driver_logic::StatusFrameScanResult::MatchedStatus);
    EXPECT_EQ(token, 0x52);
    EXPECT_EQ(status, GripperHmiProtocol::kStatusOk);
}

TEST(GripperHmiDriverLogicTest, ScansCalibrationDataOrStatusFrameAcrossWrongTokenAndData)
{
    std::vector<uint8_t> wrong_token_status = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x51,
        GripperHmiProtocol::kStatusMissingData,
        0x00,
        0x00,
        0xA5,
        0x00,
    };
    wrong_token_status[7] = GripperHmiProtocol::calculateXor(wrong_token_status.data(), 7);

    std::vector<uint8_t> data_frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x52,
    };
    for (uint8_t i = 0; i < GripperHmiProtocol::kCalibrationChunkSize; ++i)
    {
        data_frame.push_back(i);
    }
    data_frame.push_back(0x00);
    data_frame.back() = GripperHmiProtocol::calculateXor(data_frame.data() + 3, GripperHmiProtocol::kCalibrationChunkSize);

    std::vector<uint8_t> buffer = wrong_token_status;
    buffer.insert(buffer.end(), data_frame.begin(), data_frame.end());

    std::vector<uint8_t> payload;
    uint8_t token = 0;
    uint8_t status = 0;
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanCalibrationDataOrStatusFrame(&buffer, 0x52, GripperHmiProtocol::kCalibrationChunkSize,
                                                                    &payload, &token, &status),
        gripper_hmi::driver_logic::ExclusiveFrameScanResult::ContinueScanning);
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanCalibrationDataOrStatusFrame(&buffer, 0x52, GripperHmiProtocol::kCalibrationChunkSize,
                                                                    &payload, &token, &status),
        gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedData);
    ASSERT_EQ(payload.size(), GripperHmiProtocol::kCalibrationChunkSize);
    EXPECT_EQ(payload.front(), 0);
    EXPECT_EQ(payload.back(), GripperHmiProtocol::kCalibrationChunkSize - 1);
}

TEST(GripperHmiDriverLogicTest, ScansRawDataOrStatusFrameWithOversizedGarbageDrop)
{
    std::vector<uint8_t> buffer = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x99,
        0x88,
        0x77,
        0x66,
        0x55,
        0x44,
        0x33,
    };

    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanRawDataOrStatusFrame(&buffer, 4, nullptr, nullptr, nullptr),
        gripper_hmi::driver_logic::ExclusiveFrameScanResult::ContinueScanning);
    ASSERT_EQ(buffer.front(), GripperHmiProtocol::kRecvHead2);

    std::vector<uint8_t> status_frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x57,
        GripperHmiProtocol::kStatusChecksumError,
        0x00,
        0x00,
        0xA5,
        0x00,
    };
    status_frame[7] = GripperHmiProtocol::calculateXor(status_frame.data(), 7);
    uint8_t token = 0;
    uint8_t status = 0;
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanRawDataOrStatusFrame(&status_frame, 4, nullptr, &token, &status),
        gripper_hmi::driver_logic::ExclusiveFrameScanResult::MatchedStatus);
    EXPECT_EQ(token, 0x57);
    EXPECT_EQ(status, GripperHmiProtocol::kStatusChecksumError);
}

TEST(GripperHmiDriverLogicTest, ScansSizedFrameAcrossBadChecksumThenValidFrame)
{
    std::vector<uint8_t> invalid_frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x41,
        0x42,
        0x43,
        0x00,
    };
    invalid_frame[5] = 0x5A;

    std::vector<uint8_t> valid_frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x51,
        0x52,
        0x53,
        0x00,
    };
    valid_frame[5] = GripperHmiProtocol::calculateXor(valid_frame.data(), 5);

    std::vector<uint8_t> buffer = invalid_frame;
    buffer.insert(buffer.end(), valid_frame.begin(), valid_frame.end());

    std::vector<uint8_t> frame;
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanSizedRecvFrame(&buffer, valid_frame.size(), &frame),
        gripper_hmi::driver_logic::SizedFrameScanResult::ContinueScanning);
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanSizedRecvFrame(&buffer, valid_frame.size(), &frame),
        gripper_hmi::driver_logic::SizedFrameScanResult::MatchedFrame);
    EXPECT_EQ(frame, valid_frame);
}

TEST(GripperHmiDriverLogicTest, ScansExpectedCalibrationFrameAcrossWrongTokenAndGarbage)
{
    std::vector<uint8_t> wrong_token_frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x20,
    };
    for (uint8_t i = 0; i < GripperHmiProtocol::kCalibrationChunkSize; ++i)
    {
        wrong_token_frame.push_back(i);
    }
    wrong_token_frame.push_back(0x00);
    wrong_token_frame.back() = GripperHmiProtocol::calculateXor(
        wrong_token_frame.data() + 3, GripperHmiProtocol::kCalibrationChunkSize);

    std::vector<uint8_t> valid_frame = {
        0xFF,
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x21,
    };
    for (uint8_t i = 0; i < GripperHmiProtocol::kCalibrationChunkSize; ++i)
    {
        valid_frame.push_back(static_cast<uint8_t>(i + 1));
    }
    valid_frame.push_back(0x00);
    valid_frame.back() = GripperHmiProtocol::calculateXor(
        valid_frame.data() + 4, GripperHmiProtocol::kCalibrationChunkSize);

    std::vector<uint8_t> buffer = wrong_token_frame;
    buffer.insert(buffer.end(), valid_frame.begin(), valid_frame.end());

    std::vector<uint8_t> payload;
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanExpectedCalibrationFrame(&buffer, 0x21, &payload),
        gripper_hmi::driver_logic::CalibrationFrameScanResult::ContinueScanning);
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanExpectedCalibrationFrame(&buffer, 0x21, &payload),
        gripper_hmi::driver_logic::CalibrationFrameScanResult::MatchedFrame);
    ASSERT_EQ(payload.size(), GripperHmiProtocol::kCalibrationChunkSize);
    EXPECT_EQ(payload.front(), 1);
    EXPECT_EQ(payload.back(), GripperHmiProtocol::kCalibrationChunkSize);
}

TEST(GripperHmiDriverLogicTest, ScansSizedFrameKeepsTrailingByteWhenHeaderMissing)
{
    std::vector<uint8_t> buffer = {0x31, 0x32, 0x33};
    EXPECT_EQ(
        gripper_hmi::driver_logic::ScanSizedRecvFrame(&buffer, 6, nullptr),
        gripper_hmi::driver_logic::SizedFrameScanResult::NeedMoreData);
    ASSERT_EQ(buffer.size(), 1U);
    EXPECT_EQ(buffer.front(), 0x33);
}

TEST(GripperHmiDriverLogicTest, EvaluatesExclusiveDrainStepForIdleExtensionAndCutoff)
{
    const auto continue_after_bytes = gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(100, 140, 220, 8, 40);
    EXPECT_EQ(continue_after_bytes.action, gripper_hmi::driver_logic::ExclusiveDrainAction::ContinueAfterBytes);
    EXPECT_EQ(continue_after_bytes.next_idle_deadline_ms, 140);

    const auto continue_sleeping = gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(120, 160, 220, 0, 40);
    EXPECT_EQ(continue_sleeping.action, gripper_hmi::driver_logic::ExclusiveDrainAction::ContinueSleeping);
    EXPECT_EQ(continue_sleeping.next_idle_deadline_ms, 160);

    const auto drained_by_idle = gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(161, 160, 260, 0, 40);
    EXPECT_EQ(drained_by_idle.action, gripper_hmi::driver_logic::ExclusiveDrainAction::StopDrained);

    const auto drained_by_max = gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(220, 260, 220, 4, 40);
    EXPECT_EQ(drained_by_max.action, gripper_hmi::driver_logic::ExclusiveDrainAction::StopDrained);

    const auto io_failure = gripper_hmi::driver_logic::EvaluateExclusiveDrainStep(120, 160, 220, -1, 40);
    EXPECT_EQ(io_failure.action, gripper_hmi::driver_logic::ExclusiveDrainAction::StopIoFailure);
}

TEST(GripperHmiDriverLogicTest, EvaluatesReadBytesStepForDataTimeoutAndIoFailure)
{
    EXPECT_EQ(
        gripper_hmi::driver_logic::EvaluateReadBytesStep(false, 0),
        gripper_hmi::driver_logic::ReadBytesAction::ContinueSleeping);
    EXPECT_EQ(
        gripper_hmi::driver_logic::EvaluateReadBytesStep(false, 7),
        gripper_hmi::driver_logic::ReadBytesAction::ReturnSuccessWithData);
    EXPECT_EQ(
        gripper_hmi::driver_logic::EvaluateReadBytesStep(true, 0),
        gripper_hmi::driver_logic::ReadBytesAction::ReturnSuccessNoData);
    EXPECT_EQ(
        gripper_hmi::driver_logic::EvaluateReadBytesStep(false, -1),
        gripper_hmi::driver_logic::ReadBytesAction::ReturnIoFailure);
}

TEST(GripperHmiDriverLogicTest, CollectsCalibrationRangeChunkAndParsesHeaderCompletion)
{
    std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> raw = {};
    gripper_hmi::GripperCalibrationHeader header{};
    header.payloadSize =
        GripperHmiProtocol::kCalibrationChunkSize * gripper_hmi::driver_logic::kCalibrationHeaderChunkCount;
    std::memcpy(raw.data(), &header, sizeof(header));

    std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> received = {};
    for (size_t i = 0; i + 1 < gripper_hmi::driver_logic::kCalibrationHeaderChunkCount; ++i)
    {
        received[i] = true;
    }

    const uint8_t token = static_cast<uint8_t>(gripper_hmi::driver_logic::kCalibrationHeaderChunkCount - 1);
    std::vector<uint8_t> buffer = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        token,
    };
    const size_t chunk_offset = static_cast<size_t>(token) * GripperHmiProtocol::kCalibrationChunkSize;
    buffer.insert(buffer.end(),
                  raw.begin() + static_cast<std::ptrdiff_t>(chunk_offset),
                  raw.begin() + static_cast<std::ptrdiff_t>(chunk_offset + GripperHmiProtocol::kCalibrationChunkSize));
    buffer.push_back(0x00);
    buffer.back() = GripperHmiProtocol::calculateXor(
        buffer.data() + 3, GripperHmiProtocol::kCalibrationChunkSize);

    bool required_chunk_count_known = false;
    size_t required_chunk_count = gripper_hmi::driver_logic::kCalibrationChunkCount;
    uint8_t status_token = 0;
    uint8_t status_code = 0;
    EXPECT_EQ(
        gripper_hmi::driver_logic::CollectCalibrationRangeFrame(
            &buffer,
            gripper_hmi::driver_logic::CalibrationRangeCollectState{
                .raw = &raw,
                .received = &received,
                .required_chunk_count_known = &required_chunk_count_known,
                .required_chunk_count = &required_chunk_count,
                .status_token = &status_token,
                .status_code = &status_code,
            }),
        gripper_hmi::driver_logic::CalibrationRangeCollectResult::CollectionComplete);
    EXPECT_TRUE(required_chunk_count_known);
    EXPECT_EQ(required_chunk_count, gripper_hmi::driver_logic::kCalibrationHeaderChunkCount);
    EXPECT_TRUE(received[token]);
    EXPECT_TRUE(buffer.empty());
}

TEST(GripperHmiDriverLogicTest, CollectCalibrationRangeFrameConsumesStatusAndDropsGarbage)
{
    std::array<uint8_t, gripper_hmi::kCalibrationPayloadSize> raw = {};
    std::array<bool, gripper_hmi::driver_logic::kCalibrationChunkCount> received = {};
    bool required_chunk_count_known = false;
    size_t required_chunk_count = gripper_hmi::driver_logic::kCalibrationChunkCount;
    uint8_t status_token = 0;
    uint8_t status_code = 0;

    std::vector<uint8_t> status_frame = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x44,
        GripperHmiProtocol::kStatusChecksumError,
        0x00,
        0x00,
        0xA5,
        0x00,
    };
    status_frame[7] = GripperHmiProtocol::calculateXor(status_frame.data(), 7);
    EXPECT_EQ(
        gripper_hmi::driver_logic::CollectCalibrationRangeFrame(
            &status_frame,
            gripper_hmi::driver_logic::CalibrationRangeCollectState{
                .raw = &raw,
                .received = &received,
                .required_chunk_count_known = &required_chunk_count_known,
                .required_chunk_count = &required_chunk_count,
                .status_token = &status_token,
                .status_code = &status_code,
            }),
        gripper_hmi::driver_logic::CalibrationRangeCollectResult::ContinueScanning);
    EXPECT_EQ(status_token, 0x44);
    EXPECT_EQ(status_code, GripperHmiProtocol::kStatusChecksumError);
    EXPECT_TRUE(status_frame.empty());

    std::vector<uint8_t> garbage = {
        GripperHmiProtocol::kRecvHead1,
        GripperHmiProtocol::kRecvHead2,
        0x99,
        0x88,
        0x77,
        0x66,
        0x55,
        0x44,
        0x33,
        0x22,
        0x11,
        0x00,
        0xFE,
        0xED,
        0xDC,
        0xCB,
        0xBA,
        0xA9,
        0x98,
        0x87,
        0x76,
        0x65,
        0x54,
        0x43,
        0x32,
        0x21,
        0x10,
        0x0F,
        0xEE,
        0xDD,
        0xCC,
        0xBB,
        0xAA,
        0x99,
        0x88,
        0x77,
    };
    EXPECT_EQ(
        gripper_hmi::driver_logic::CollectCalibrationRangeFrame(
            &garbage,
            gripper_hmi::driver_logic::CalibrationRangeCollectState{
                .raw = &raw,
                .received = &received,
                .required_chunk_count_known = &required_chunk_count_known,
                .required_chunk_count = &required_chunk_count,
                .status_token = &status_token,
                .status_code = &status_code,
            }),
        gripper_hmi::driver_logic::CalibrationRangeCollectResult::ContinueScanning);
    ASSERT_FALSE(garbage.empty());
    EXPECT_EQ(garbage.front(), GripperHmiProtocol::kRecvHead2);
}

}  // namespace
