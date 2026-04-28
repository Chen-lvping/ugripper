cmake_minimum_required(VERSION 3.10)
project(utils LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)

add_library(utils
    src/env_utils.cc
    src/file_logger.cc
    src/log_fmtter.cc
    src/spd_logger.cc
    src/time_utils.cc
)

target_include_directories(utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/utils
)

target_link_libraries(utils
    PUBLIC
        PkgConfig::SPDLOG
        PkgConfig::FMT
)

add_executable(utils_smoke_test
    src/utils_smoke_test.cc
)

target_link_libraries(utils_smoke_test
    PRIVATE
        utils
)
