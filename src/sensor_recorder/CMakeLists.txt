cmake_minimum_required(VERSION 3.10)
project(sensor_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED libserialport)

set(SENSOR_RECORDER_SOURCES
    src/main.cpp
    src/im648_CMD.cpp
    src/im648_driver.cpp
    src/bsp_crc.cpp
    src/encoder_driver.cpp
)

add_executable(sensor_recorder ${SENSOR_RECORDER_SOURCES})

target_include_directories(sensor_recorder
    PRIVATE
        ${LIBSERIALPORT_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

link_directories(${LIBSERIALPORT_LIBRARY_DIRS})

target_link_libraries(sensor_recorder
    PRIVATE
        ${LIBSERIALPORT_LIBRARIES}
        pthread
        mcap
)
