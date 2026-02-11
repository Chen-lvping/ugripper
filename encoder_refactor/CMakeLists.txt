cmake_minimum_required(VERSION 3.10)
project(EncoderProject)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含头文件
include_directories(${PROJECT_SOURCE_DIR}/include)

# 查找 libserialport
find_library(SERIALPORT_LIB serialport)
if(NOT SERIALPORT_LIB)
    message(FATAL_ERROR "libserialport not found! Install with: sudo apt install libserialport-dev")
endif()

# -----------------------------
# 公共源码
# -----------------------------
set(ENCODER_SOURCES
    src/bsp_crc.cpp
    src/encoder_driver.cpp
)

# -----------------------------
# 编译主程序 main (记录数据用)
# -----------------------------
add_executable(main
    src/main.cpp
    ${ENCODER_SOURCES}
)

target_link_libraries(main
  PRIVATE
    ${SERIALPORT_LIB}
    mcap
)

# -----------------------------
# 编译归零程序 zeroing
# -----------------------------
add_executable(zeroing
    src/zeroing.cpp
    ${ENCODER_SOURCES}
)

target_link_libraries(zeroing PRIVATE ${SERIALPORT_LIB})

# -----------------------------
# 编译 CRC 单元测试
# -----------------------------
add_executable(crc_test
    test/crc_test.cpp
    src/bsp_crc.cpp
)
