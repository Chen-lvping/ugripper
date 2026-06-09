set(APP_NAME UgripperRuntime)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

set(UGRIPPERRUNTIME_MCAP_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../SensorRecorder/third_party/mcap/include")
set(UGRIPPERRUNTIME_MCAP_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../SensorRecorder/third_party/mcap/src/lib.cc")

if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 QUIET IMPORTED_TARGET liblz4)
endif()

set(UUID_SEARCH_DIRS)
if(CMAKE_CROSSCOMPILING)
    foreach(root IN LISTS CMAKE_FIND_ROOT_PATH)
        list(APPEND UUID_SEARCH_DIRS
            "${root}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
            "${root}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
        )
    endforeach()
else()
    list(APPEND UUID_SEARCH_DIRS
        "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
        "/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
    )
endif()
find_library(UUID_LIBRARY
    NAMES uuid
    REQUIRED
    PATHS ${UUID_SEARCH_DIRS}
    NO_DEFAULT_PATH
)
message(STATUS "UgripperRuntime: using libuuid at ${UUID_LIBRARY}")

add_library(ugripperruntime_mcap STATIC ${UGRIPPERRUNTIME_MCAP_SOURCE})
set_target_properties(ugripperruntime_mcap PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(ugripperruntime_mcap
    PUBLIC
        "${UGRIPPERRUNTIME_MCAP_INCLUDE_DIR}"
)
if(LZ4_FOUND)
    target_link_libraries(ugripperruntime_mcap PUBLIC PkgConfig::LZ4)
else()
    target_compile_definitions(ugripperruntime_mcap PUBLIC MCAP_COMPRESSION_NO_LZ4)
endif()
target_compile_definitions(ugripperruntime_mcap PUBLIC MCAP_COMPRESSION_NO_ZSTD)

add_library(ugripperruntime_support_utils STATIC
    ${CMAKE_SOURCE_DIR}/src/utils/src/env_utils.cc
    ${CMAKE_SOURCE_DIR}/src/utils/src/time_utils.cc
)
set_target_properties(ugripperruntime_support_utils PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(ugripperruntime_support_utils
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src/utils/include
)

add_library(runtime_process_boundary STATIC
    runtime_process.cc
)
set_target_properties(runtime_process_boundary PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(runtime_process_boundary
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(runtime_process_boundary
    PUBLIC
        ugripperruntime_support_utils
        PP::utils
        nlohmann_json::nlohmann_json
        Threads::Threads
)

add_library(runtime_control_plane STATIC
    runtime_domain.cc
)
set_target_properties(runtime_control_plane PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(runtime_control_plane
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(runtime_control_plane
    PUBLIC
        runtime_process_boundary
        ugripperruntime_support_utils
        PP::utils
        nlohmann_json::nlohmann_json
        Threads::Threads
)

add_executable(${APP_NAME}
    main.cc
    record_runtime.cc
)
set_target_properties(${APP_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "UgripperRuntime"
)
target_include_directories(${APP_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../CameraRecorder/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../GripperHmiTool/include
)
target_link_libraries(${APP_NAME}
    PRIVATE
        runtime_control_plane
        runtime_process_boundary
        ugripperruntime_mcap
        ugripperruntime_support_utils
        gripper_hmi
        PP::utils
        nlohmann_json::nlohmann_json
        Threads::Threads
        ${UUID_LIBRARY}
)
target_compile_definitions(${APP_NAME}
    PRIVATE
        UGRIPPER_ENABLE_PERF_LOG=$<BOOL:${UGRIPPER_ENABLE_PERF_LOG}>
)

install(TARGETS ${APP_NAME}
    RUNTIME DESTINATION bin/UgripperRuntime
    LIBRARY DESTINATION lib
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/
    DESTINATION bin/UgripperRuntime/audio
    USE_SOURCE_PERMISSIONS
    PATTERN "__pycache__" EXCLUDE
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_en/
    DESTINATION bin/UgripperRuntime/audio_en
    USE_SOURCE_PERMISSIONS
    FILES_MATCHING PATTERN "*.wav"
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/ego/
    DESTINATION bin/UgripperRuntime/ego
    USE_SOURCE_PERMISSIONS
    PATTERN "__pycache__" EXCLUDE
)

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/adb/
    DESTINATION bin/UgripperRuntime/adb
    USE_SOURCE_PERMISSIONS
    PATTERN "__pycache__" EXCLUDE
)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/config/fakeCamCalib.json
    DESTINATION bin/UgripperRuntime/config
)
