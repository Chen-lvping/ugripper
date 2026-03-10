cmake_minimum_required(VERSION 3.10)
project(camera_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(camera_recorder
    src/main.cpp
)

target_link_libraries(camera_recorder
    PRIVATE
        pthread
)
