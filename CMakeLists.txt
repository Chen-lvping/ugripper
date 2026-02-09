cmake_minimum_required(VERSION 3.10)
project(ugripper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# ========= third_party =========
add_subdirectory(src/third_party/mcap_builder)

# ========= 子模块 =========
add_subdirectory(dm_imu_alone)
add_subdirectory(im648_imu_alone)
add_subdirectory(encoder_refactor)
add_subdirectory(faysSense_vi_kit)