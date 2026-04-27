set(APP_NAME CameraRecorder)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-base-1.0)
set(CAMERA_RECORDER_HAS_LIBUSB ON)
set(CAMERA_RECORDER_HAS_GSTREAMER ON)
message(STATUS "CameraRecorder: enabling libusb-backed UVC roll control")
message(STATUS "CameraRecorder: enabling internal main-camera appsrc pipeline")

set(CAMERA_RECORDER_SOURCES
    main.cc
    camera_recorder.cc
)

set(CAMERA_DOMAIN_SOURCES
    camera_domain.cc
)

add_library(camera_domain STATIC ${CAMERA_DOMAIN_SOURCES})
set_target_properties(camera_domain PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(camera_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(camera_domain
    PUBLIC
        PP::utils
        yaml-cpp::yaml-cpp
        nlohmann_json::nlohmann_json
)

add_executable(${APP_NAME} ${CAMERA_RECORDER_SOURCES})
set_target_properties(${APP_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "CameraRecorder"
)
target_include_directories(${APP_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(${APP_NAME}
    PRIVATE
        camera_domain
        PP::utils
        yaml-cpp::yaml-cpp
        nlohmann_json::nlohmann_json
        Threads::Threads
)
target_compile_definitions(${APP_NAME}
    PRIVATE
        CAMERA_RECORDER_HAS_LIBUSB=$<BOOL:${CAMERA_RECORDER_HAS_LIBUSB}>
        CAMERA_RECORDER_HAS_GSTREAMER=$<BOOL:${CAMERA_RECORDER_HAS_GSTREAMER}>
)

if(CAMERA_RECORDER_HAS_LIBUSB)
    target_link_libraries(${APP_NAME}
        PRIVATE
            PkgConfig::LIBUSB
    )
endif()

if(CAMERA_RECORDER_HAS_GSTREAMER)
    target_link_libraries(${APP_NAME}
        PRIVATE
            PkgConfig::GSTREAMER
    )
endif()

install(TARGETS ${APP_NAME}
    RUNTIME DESTINATION bin/CameraRecorder
    LIBRARY DESTINATION lib
)

install(DIRECTORY config/
    DESTINATION bin/CameraRecorder/config
    FILES_MATCHING PATTERN "*.yaml"
)
