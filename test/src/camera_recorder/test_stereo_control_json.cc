#include "camera_recorder/stereo_control_json.h"

#include <gtest/gtest.h>

namespace {

using json = nlohmann::json;

TEST(StereoControlJsonTest, ParsesStereoControlCommand)
{
    const json root = {
        {"command_seq", 7},
        {"recording", true},
        {"episode_dir", "/tmp/episode-7"},
        {"start_system_time_us", 123456},
        {"stop_system_time_us", 0},
    };

    const auto command = ugripper::camera::ParseStereoControlCommand(root);

    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->command_seq, 7U);
    EXPECT_TRUE(command->recording);
    EXPECT_EQ(command->episode_dir, "/tmp/episode-7");
    EXPECT_EQ(command->start_system_time_us, 123456);
    EXPECT_EQ(command->stop_system_time_us, 0);
}

TEST(StereoControlJsonTest, RejectsNonObjectControlJson)
{
    EXPECT_FALSE(ugripper::camera::ParseStereoControlCommand(json::array()).has_value());
}

TEST(StereoControlJsonTest, BuildsStableServiceStatusJson)
{
    ugripper::camera::StereoServiceStatus status;
    status.recording = true;
    status.finalize_pending = false;
    status.active_episode_dir = "/tmp/episode-8";
    status.last_finalized_episode_dir = "/tmp/episode-7";
    status.last_finalize_error.clear();
    status.last_session = json{{"episode_dir", "/tmp/episode-7"}};
    status.ready = false;
    status.service_state = "recording_recovering";

    ugripper::camera::StereoTrackStatus left_camera;
    left_camera.state = "recording";
    left_camera.device = "/dev/video-left";
    left_camera.ready = true;
    left_camera.first_frame_system_time_us = 11;
    left_camera.last_frame_system_time_us = 22;
    left_camera.session_recording = true;
    status.cameras["left_stereo"] = left_camera;

    const json value = ugripper::camera::BuildStereoServiceStatusJson(status);

    EXPECT_TRUE(value.at("recording").get<bool>());
    EXPECT_EQ(value.at("service_state").get<std::string>(), "recording_recovering");
    EXPECT_EQ(value.at("active_episode_dir").get<std::string>(), "/tmp/episode-8");
    ASSERT_TRUE(value.at("cameras").contains("left_stereo"));
    const json& camera = value.at("cameras").at("left_stereo");
    EXPECT_EQ(camera.at("state").get<std::string>(), "recording");
    EXPECT_EQ(camera.at("device").get<std::string>(), "/dev/video-left");
    EXPECT_EQ(camera.at("first_frame_system_time_us").get<int64_t>(), 11);
    EXPECT_EQ(camera.at("last_frame_system_time_us").get<int64_t>(), 22);
    EXPECT_TRUE(camera.at("session_recording").get<bool>());
}

}  // namespace
