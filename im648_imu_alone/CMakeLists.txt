cmake_minimum_required(VERSION 3.10)
project(im648_imu)

set(CMAKE_CXX_STANDARD 20)

# Include headers
include_directories(include)

# Find libserialport
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED libserialport)

include_directories(${LIBSERIALPORT_INCLUDE_DIRS})
link_directories(${LIBSERIALPORT_LIBRARY_DIRS})

# IM648 IMU data logger executable
add_executable(im648_imu
    src/main.cpp
    src/im648_CMD.cpp
    src/im648_driver.cpp
)

target_link_libraries(im648_imu
    ${LIBSERIALPORT_LIBRARIES}
    pthread
    mcap
)
