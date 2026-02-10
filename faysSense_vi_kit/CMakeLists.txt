cmake_minimum_required(VERSION 3.12)
project(fays_vikit)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FAYS_ATRAK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/fays_atrak/${CMAKE_SYSTEM_PROCESSOR}/Release" CACHE PATH "Path to fays_atrak libraries")
message(STATUS "FAYS_ATRAK_LIB_DIR: ${FAYS_ATRAK_LIB_DIR}")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(Record_SRC_CPP
    example/record.cpp
)

# opencv
# Use libs directory from project root (CMAKE_SOURCE_DIR when building from root)
set(OPENCV_LOCAL_DIR "/usr/local/opencv-4.2.0-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(OpenCV_DIR "${OPENCV_LOCAL_DIR}/lib/cmake/opencv4")
message(STATUS "OpenCV_DIR: ${OpenCV_DIR}")
find_package(OpenCV REQUIRED PATHS ${OpenCV_DIR} NO_DEFAULT_PATH)

find_library(FAYS_ATRAK_VI_MODULE_LIB
    NAMES fays_vikit
    PATHS ${FAYS_ATRAK_LIB_DIR}
    NO_DEFAULT_PATH
    REQUIRED
)
find_package(Threads REQUIRED)
if(NOT FAYS_ATRAK_VI_MODULE_LIB)
    message(FATAL_ERROR "Failed to find libfays_atrak_vi_module.so")
endif()

# set(TBB_LIB_PATH "/usr/lib/aarch64-linux-gnu")

# FTDI
# Use libs directory from project root (CMAKE_SOURCE_DIR when building from root)
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(FTDPATH "${CMAKE_SOURCE_DIR}/libs/ft602-linux-x86_64")
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(FTDPATH "${CMAKE_SOURCE_DIR}/libs/ft602-linux-aarch64")
endif()
find_library(FTD3XX_LIBRARY ft602 PATHS ${FTDPATH})
message(STATUS "FTD3XX_LIBRARY: ${FTD3XX_LIBRARY}")
if (NOT FTD3XX_LIBRARY)
    message(FATAL_ERROR "ft602 library not found")
endif()

include_directories(${FTDPATH} ${OpenCV_INCLUDE_DIRS})

add_executable(fays_record_example ${Record_SRC_CPP})

# --- 链接库 ---
target_link_libraries(fays_record_example
    ${FAYS_ATRAK_VI_MODULE_LIB}
    Threads::Threads
    ${OpenCV_LIBS}
    ${FTD3XX_LIBRARY}
    mcap
)

# --- Copy config files and scripts to build directory ---
# Copy config files to build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/config
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
     FILES_MATCHING PATTERN "*.yaml")

# Copy scripts to build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/scripts
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
     FILES_MATCHING PATTERN "*.sh")

# Make scripts executable
file(GLOB SCRIPT_FILES "${CMAKE_CURRENT_BINARY_DIR}/scripts/*.sh")
foreach(SCRIPT ${SCRIPT_FILES})
    file(CHMOD ${SCRIPT} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
         GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()