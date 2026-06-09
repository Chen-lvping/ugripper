cmake_minimum_required(VERSION 3.10)
project(ugripper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

option(UGRIPPER_ENABLE_MCAP_BUILDER "Build optional src/third_party/mcap_builder target" OFF)
option(UGRIPPER_ENABLE_PERF_LOG "Build UGripper with verbose PERF timing logs enabled" OFF)

find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()

# ========= third_party =========
if(UGRIPPER_ENABLE_MCAP_BUILDER)
    add_subdirectory(src/third_party/mcap_builder)
endif()
add_subdirectory(src/utils)
if(NOT TARGET PP::utils)
    add_library(PP::utils ALIAS utils)
endif()

# ========= 主链应用 =========
add_subdirectory(standalone)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
