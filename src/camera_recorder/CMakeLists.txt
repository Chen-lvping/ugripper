cmake_minimum_required(VERSION 3.10)
project(camera_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(yaml-cpp REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_executable(camera_recorder
    src/camera_recorder.cpp
    src/main.cpp
)

target_include_directories(camera_recorder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LIBUSB_INCLUDE_DIRS}
)

target_link_directories(camera_recorder
    PRIVATE
        ${LIBUSB_LIBRARY_DIRS}
)

target_compile_options(camera_recorder
    PRIVATE
        ${LIBUSB_CFLAGS_OTHER}
)

target_link_libraries(camera_recorder
    PRIVATE
        pthread
        ${LIBUSB_LIBRARIES}
        yaml-cpp
)
