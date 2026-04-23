cmake_minimum_required(VERSION 3.10)
project(utils LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(utils
    src/env_utils.cc
    src/time_utils.cc
)

target_include_directories(utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(utils_smoke_test
    src/utils_smoke_test.cc
)

target_link_libraries(utils_smoke_test
    PRIVATE
        utils
)
