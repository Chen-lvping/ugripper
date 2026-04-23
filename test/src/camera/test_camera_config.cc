#include "camera_recorder/camera_config.h"
#include "camera_recorder/camera_registry.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

std::filesystem::path CameraSampleDir()
{
    return std::filesystem::path(__FILE__).parent_path() / "config_samples";
}

TEST(CameraConfigTest, LoadsLegacyFlatSample)
{
    const auto configs =
        ugripper::camera::LoadCameraConfigList(CameraSampleDir() / "legacy_flat_valid.yaml");

    ASSERT_EQ(configs.size(), 1U);
    EXPECT_EQ(configs[0].name, "step4_demo_camera");
    EXPECT_EQ(configs[0].device, "/dev/null");
    EXPECT_EQ(configs[0].mode, ugripper::camera::CameraRecordMode::DirectCopyH265);
    EXPECT_EQ(ugripper::camera::PrimaryOutputFileName(configs[0]), "step4_demo_camera.mkv");
}

TEST(CameraConfigTest, LoadsSchemaV1Sample)
{
    const auto configs =
        ugripper::camera::LoadCameraConfigList(CameraSampleDir() / "schema_v1_valid.yaml");

    ASSERT_EQ(configs.size(), 1U);
    EXPECT_EQ(configs[0].name, "step4_demo_camera");
    EXPECT_EQ(configs[0].role, ugripper::camera::CameraRole::Main);
    EXPECT_EQ(configs[0].side, ugripper::camera::CameraSide::Left);
    EXPECT_EQ(configs[0].kind, ugripper::camera::CameraKind::DirectH265);
    EXPECT_TRUE(ugripper::camera::HasGroup(configs[0], "session"));
    EXPECT_TRUE(ugripper::camera::HasGroup(configs[0], "critical"));
    EXPECT_EQ(configs[0].device, "/dev/null");
    EXPECT_EQ(configs[0].fps, 30);
    EXPECT_EQ(ugripper::camera::PrimaryOutputFileName(configs[0]), "step4_demo_camera.mkv");
}

TEST(CameraConfigTest, RejectsLegacySampleWithoutOutputFiles)
{
    EXPECT_THROW(
        ugripper::camera::LoadCameraConfigList(
            CameraSampleDir() / "legacy_flat_invalid_missing_output_files.yaml"),
        std::runtime_error);
}

}  // namespace
