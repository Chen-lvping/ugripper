cmake_minimum_required(VERSION 3.10)
project(gripper_hmi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSERIALPORT REQUIRED libserialport)

add_library(gripper_hmi
    src/gripper_hmi_protocol.cpp
    src/gripper_hmi_driver.cpp
    src/gripper_hmi_led_effects.cpp
)

target_include_directories(gripper_hmi
    PUBLIC
        ${LIBSERIALPORT_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

link_directories(${LIBSERIALPORT_LIBRARY_DIRS})

target_link_libraries(gripper_hmi
    PUBLIC
        ${LIBSERIALPORT_LIBRARIES}
)

add_executable(gripper_hmi_test
    test/gripper_hmi_test.cpp
)

target_link_libraries(gripper_hmi_test
    PRIVATE
        gripper_hmi
        pthread
)
