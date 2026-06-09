set(APP_NAME FaysStereoRecorder)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FAYS_LZ4 QUIET IMPORTED_TARGET liblz4)
endif()

set(FAYS_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(FAYS_ARCH MATCHES "amd64|x86_64")
    set(FAYS_ARCH "x86_64")
elseif(FAYS_ARCH MATCHES "arm64|aarch64|armv8")
    set(FAYS_ARCH "aarch64")
endif()

set(FAYS_SDK_ROOT "${CMAKE_SOURCE_DIR}/src/third_party/fays_lib")
set(FAYS_INCLUDE_DIR "${FAYS_SDK_ROOT}/include")
set(FAYS_FTDI_DIR "${FAYS_SDK_ROOT}/ft602-linux-${FAYS_ARCH}")
set(FAYS_ATRAK_LIB_DIR "${FAYS_SDK_ROOT}/lib/fays_atrak/${FAYS_ARCH}/Release")

set(FAYS_ATRAK_LIBRARY "${FAYS_ATRAK_LIB_DIR}/libfays_vikit.so")
set(FAYS_FTDI_LIBRARY "${FAYS_FTDI_DIR}/libft602.so")

find_library(FAYS_OPENCV_CORE_LIBRARY
    NAMES opencv_core
    PATHS /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu
)
find_library(FAYS_OPENCV_IMGPROC_LIBRARY
    NAMES opencv_imgproc
    PATHS /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu
)
find_path(FAYS_OPENCV_INCLUDE_DIR
    NAMES opencv2/imgproc.hpp
    PATH_SUFFIXES opencv4
)

foreach(required_path
        "${FAYS_INCLUDE_DIR}/fays_atrak/fays_vikit.h"
        "${FAYS_FTDI_LIBRARY}"
        "${FAYS_ATRAK_LIBRARY}"
        "${FAYS_OPENCV_CORE_LIBRARY}"
        "${FAYS_OPENCV_IMGPROC_LIBRARY}"
        "${FAYS_OPENCV_INCLUDE_DIR}/opencv2/imgproc.hpp")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "FaysStereoRecorder dependency missing: ${required_path}")
    endif()
endforeach()

set(FAYS_MCAP_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../SensorRecorder/third_party/mcap/include")
set(FAYS_MCAP_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../SensorRecorder/third_party/mcap/src/lib.cc")

add_library(fays_mcap STATIC ${FAYS_MCAP_SOURCE})
set_target_properties(fays_mcap PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(fays_mcap
    PUBLIC
        "${FAYS_MCAP_INCLUDE_DIR}"
)
if(FAYS_LZ4_FOUND)
    target_link_libraries(fays_mcap PUBLIC PkgConfig::FAYS_LZ4)
else()
    target_compile_definitions(fays_mcap PUBLIC MCAP_COMPRESSION_NO_LZ4)
endif()
target_compile_definitions(fays_mcap PUBLIC MCAP_COMPRESSION_NO_ZSTD)

add_executable(fays_record_example
    src/record.cpp
)
set_target_properties(fays_record_example PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN/lib/ft602-linux-${FAYS_ARCH};\$ORIGIN/lib/fays_atrak/${FAYS_ARCH}/Release"
)
target_include_directories(fays_record_example
    PRIVATE
        "${FAYS_INCLUDE_DIR}"
        "${FAYS_OPENCV_INCLUDE_DIR}"
)
target_link_libraries(fays_record_example
    PRIVATE
        "${FAYS_ATRAK_LIBRARY}"
        "${FAYS_FTDI_LIBRARY}"
        "${FAYS_OPENCV_IMGPROC_LIBRARY}"
        "${FAYS_OPENCV_CORE_LIBRARY}"
        fays_mcap
        Threads::Threads
)
target_compile_definitions(fays_record_example
    PRIVATE
        UGRIPPER_ENABLE_PERF_LOG=$<BOOL:${UGRIPPER_ENABLE_PERF_LOG}>
)

file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/config"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
     FILES_MATCHING PATTERN "*.yaml")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
     FILES_MATCHING PATTERN "*.sh")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/fays_atrak/${FAYS_ARCH}/Release")
file(COPY "${FAYS_ATRAK_LIBRARY}"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/lib/fays_atrak/${FAYS_ARCH}/Release")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib/ft602-linux-${FAYS_ARCH}")
file(COPY
        "${FAYS_FTDI_DIR}/libft602.so"
        "${FAYS_FTDI_DIR}/libft602.so.1.0.17"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/lib/ft602-linux-${FAYS_ARCH}")

file(GLOB FAYS_RECORDER_SCRIPT_FILES "${CMAKE_CURRENT_BINARY_DIR}/scripts/*.sh")
foreach(script_file ${FAYS_RECORDER_SCRIPT_FILES})
    execute_process(COMMAND chmod +x "${script_file}")
endforeach()
