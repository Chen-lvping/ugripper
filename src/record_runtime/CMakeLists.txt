cmake_minimum_required(VERSION 3.10)
project(record_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(record_runtime
    src/record_runtime.cpp
    src/main.cpp
)

target_include_directories(record_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/common/include
)

target_link_libraries(record_runtime
    PRIVATE
        gripper_hmi
        pthread
        mcap
)
