cmake_minimum_required(VERSION 3.10)
project(camera_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(yaml-cpp REQUIRED)

add_executable(camera_recorder
    src/camera_recorder.cpp
    src/main.cpp
)

target_include_directories(camera_recorder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(camera_recorder
    PRIVATE
        pthread
        yaml-cpp
)
