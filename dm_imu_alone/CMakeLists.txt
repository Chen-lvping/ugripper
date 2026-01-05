cmake_minimum_required(VERSION 3.10)
project(dm_imu_no_ros)

set(CMAKE_CXX_STANDARD 17)

# 包含头文件
include_directories(include)

# 找 libserialport
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED libserialport)

include_directories(${LIBSERIALPORT_INCLUDE_DIRS})
link_directories(${LIBSERIALPORT_LIBRARY_DIRS})

add_executable(dm_imu
    src/main.cpp
    src/imu_driver.cpp
    src/bsp_crc.cpp
)

target_link_libraries(dm_imu
    ${LIBSERIALPORT_LIBRARIES}
    pthread
)

add_executable(imu_calib
    src/imu_calibration.cpp
    src/imu_driver.cpp
    src/bsp_crc.cpp
)

target_link_libraries(imu_calib
    ${LIBSERIALPORT_LIBRARIES}
    pthread
)
    