#include "record_runtime.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace fs = std::filesystem;

TEST(EgoBindingConfigTest, DefaultBindingUsesBootScopedSharedMemory)
{
    const RecordRuntimeOptions options;

    EXPECT_EQ(options.egoBindingFile, "/dev/shm/ugripper/ego_binding.json");
    EXPECT_EQ(fs::path(options.egoBindingFile).parent_path(),
              fs::path(options.egoBindingStatusFile).parent_path());
}
