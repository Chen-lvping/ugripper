cmake_minimum_required(VERSION 3.10)
project(sensor_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED libserialport)

set(SENSOR_COMMON_SOURCES
    src/im648_CMD.cpp
    src/im648_driver.cpp
    src/bsp_crc.cpp
    src/encoder_driver.cpp
)

add_executable(sensor_recorder
    src/main.cpp
    ${SENSOR_COMMON_SOURCES}
)

add_executable(zeroing
    src/zeroing.cpp
    src/bsp_crc.cpp
    src/encoder_driver.cpp
)

target_include_directories(sensor_recorder
    PRIVATE
        ${LIBSERIALPORT_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/common/include
)

target_include_directories(zeroing
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

target_link_libraries(zeroing
    PRIVATE
        ${LIBSERIALPORT_LIBRARIES}
        pthread
)
