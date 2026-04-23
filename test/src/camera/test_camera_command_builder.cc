#include "camera_recorder/camera_command_builder.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

ugripper::camera::CameraConfig MakeHybridCamera()
{
    ugripper::camera::CameraConfig config;
    config.name = "left_main";
    config.device = "/dev/video-left";
    config.mode = ugripper::camera::CameraRecordMode::HybridDecodeEncode;
    config.input_format = "mjpeg";
    config.capture_width = 1280;
    config.capture_height = 720;
    config.width = 640;
    config.height = 360;
    config.fps = 60;
    config.output_fps = 30;
    config.video_filter = "transpose=1";
    config.output_files = {"left_main.mkv"};
    config.input_thread_queue_size = 32;
    config.qp_init = 31;
    config.qp_max = 39;
    config.qp_min = 23;
    config.qp_max_i = 38;
    config.qp_min_i = 21;
    return config;
}

TEST(CameraCommandBuilderTest, BuildsHybridVideoFilterWithScaleAndFrameDrop)
{
    const auto config = MakeHybridCamera();

    const auto filter = ugripper::camera::BuildVideoFilter(config);

    ASSERT_TRUE(filter.has_value());
    EXPECT_NE(filter->find("scale=640:360:flags=neighbor"), std::string::npos);
    EXPECT_NE(filter->find("transpose=1"), std::string::npos);
    EXPECT_NE(filter->find("select=not(mod(n\\,2))"), std::string::npos);
}

TEST(CameraCommandBuilderTest, BuildsHybridCommandWithStableFlags)
{
    const auto config = MakeHybridCamera();
    const ugripper::camera::CommandBuildOptions options{
        .output_dir = "/tmp/episode_001",
        .codec = "h265",
        .ffmpeg_bin = "/usr/bin/ffmpeg",
        .gst_bin = "/usr/bin/gst-launch-1.0",
    };

    const std::string command = ugripper::camera::BuildHybridCameraCommand(config, options);

    EXPECT_NE(command.find("/usr/bin/ffmpeg"), std::string::npos);
    EXPECT_NE(command.find("-thread_queue_size 32"), std::string::npos);
    EXPECT_NE(command.find("-f v4l2 -input_format mjpeg"), std::string::npos);
    EXPECT_NE(command.find("-video_size 1280x720"), std::string::npos);
    EXPECT_NE(command.find("-vf 'scale=640:360:flags=neighbor,transpose=1,select=not(mod(n\\,2))'"),
              std::string::npos);
    EXPECT_NE(command.find("-fps_mode passthrough -enc_time_base -1"), std::string::npos);
    EXPECT_NE(command.find("'/tmp/episode_001/left_main.mkv'"), std::string::npos);
}

TEST(CameraCommandBuilderTest, BuildsStereoSessionCommand)
{
    auto config = MakeHybridCamera();
    config.mode = ugripper::camera::CameraRecordMode::StereoHybridDecodeEncode;
    config.output_files = {"left_stereo.mkv"};
    const ugripper::camera::CommandBuildOptions options{
        .output_dir = "/tmp/ignored",
        .codec = "h264",
        .ffmpeg_bin = "ffmpeg",
        .gst_bin = "gst-launch-1.0",
    };

    const std::string command = ugripper::camera::BuildStereoSessionCommand(
        config, options, "/tmp/stereo_episode/left_stereo.mkv");

    EXPECT_NE(command.find("-fflags +genpts"), std::string::npos);
    EXPECT_NE(command.find("-framerate 30"), std::string::npos);
    EXPECT_NE(command.find("-map 0:v:0 -an"), std::string::npos);
    EXPECT_NE(command.find("-profile:v main -level 5.1"), std::string::npos);
    EXPECT_NE(command.find("'/tmp/stereo_episode/left_stereo.mkv'"), std::string::npos);
}

}  // namespace
