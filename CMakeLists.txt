cmake_minimum_required(VERSION 3.10)
project(ugripper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# ========= third_party =========
add_subdirectory(src/third_party/mcap_builder)

# ========= 子模块 =========
add_subdirectory(src/sensor_recorder)
add_subdirectory(src/camera_recorder)
add_subdirectory(src/gripper_hmi)
add_subdirectory(src/record_runtime)
