#include "camera_recorder/camera_timing.h"

#include <gtest/gtest.h>

TEST(CameraTimingTest, UsesV4l2ElapsedTimeForPts)
{
    const auto result = ugripper::camera::MainCameraPtsFromV4l2TimeUs(
        1'785'296'245'838'786,
        1'785'296'245'872'120,
        16'667);

    EXPECT_EQ(result.pts_us, 33'334);
    EXPECT_FALSE(result.clamped);
}

TEST(CameraTimingTest, PreservesRealCaptureGap)
{
    const auto result = ugripper::camera::MainCameraPtsFromV4l2TimeUs(
        1'000'000,
        1'100'000,
        33'334);

    EXPECT_EQ(result.pts_us, 100'000);
    EXPECT_FALSE(result.clamped);
}

TEST(CameraTimingTest, MinimallyClampsDuplicateTimestamp)
{
    const auto result = ugripper::camera::MainCameraPtsFromV4l2TimeUs(
        1'000'000,
        1'016'667,
        16'667);

    EXPECT_EQ(result.raw_pts_us, 16'667);
    EXPECT_EQ(result.pts_us, 16'668);
    EXPECT_EQ(result.adjustment_us, 1);
    EXPECT_TRUE(result.clamped);
    EXPECT_FALSE(result.timestamp_rollback);
}

TEST(CameraTimingTest, MinimallyClampsTimestampRollback)
{
    const auto result = ugripper::camera::MainCameraPtsFromV4l2TimeUs(
        1'000'000,
        1'010'000,
        16'667);

    EXPECT_EQ(result.raw_pts_us, 10'000);
    EXPECT_EQ(result.pts_us, 16'668);
    EXPECT_EQ(result.adjustment_us, 6'668);
    EXPECT_TRUE(result.clamped);
    EXPECT_TRUE(result.timestamp_rollback);
}
