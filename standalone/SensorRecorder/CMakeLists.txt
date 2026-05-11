set(APP_NAME SensorRecorder)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

set(SENSORRECORDER_HAS_SYSTEM_LIBSERIALPORT OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSERIALPORT QUIET IMPORTED_TARGET libserialport)
    if(LIBSERIALPORT_FOUND)
        set(SENSORRECORDER_HAS_SYSTEM_LIBSERIALPORT ON)
    endif()
endif()

set(SENSORRECORDER_LIBSERIALPORT_TARGET "")
if(SENSORRECORDER_HAS_SYSTEM_LIBSERIALPORT)
    set(SENSORRECORDER_LIBSERIALPORT_TARGET PkgConfig::LIBSERIALPORT)
    message(STATUS "SensorRecorder: using system libserialport")
else()
    set(SENSORRECORDER_UGRIPPER_ROOT "${CMAKE_SOURCE_DIR}")
    set(SENSORRECORDER_LIBSERIALPORT_ROOT
        "${SENSORRECORDER_UGRIPPER_ROOT}/.local-deps/libserialport/root/usr")
    set(SENSORRECORDER_LIBSERIALPORT_INCLUDE_DIR
        "${SENSORRECORDER_LIBSERIALPORT_ROOT}/include")
    set(SENSORRECORDER_LIBSERIALPORT_LIBRARY
        "${SENSORRECORDER_LIBSERIALPORT_ROOT}/lib/x86_64-linux-gnu/libserialport.so")
    set(SENSORRECORDER_LIBSERIALPORT_LIBRARY_DIR
        "${SENSORRECORDER_LIBSERIALPORT_ROOT}/lib/x86_64-linux-gnu")

    if(EXISTS "${SENSORRECORDER_LIBSERIALPORT_INCLUDE_DIR}/libserialport.h" AND
       EXISTS "${SENSORRECORDER_LIBSERIALPORT_LIBRARY}")
        add_library(sensorrecorder_local_libserialport SHARED IMPORTED GLOBAL)
        set_target_properties(sensorrecorder_local_libserialport PROPERTIES
            IMPORTED_LOCATION "${SENSORRECORDER_LIBSERIALPORT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SENSORRECORDER_LIBSERIALPORT_INCLUDE_DIR}"
        )
        set(SENSORRECORDER_LIBSERIALPORT_TARGET sensorrecorder_local_libserialport)
        message(WARNING
            "SensorRecorder: system libserialport not found; using sibling ugripper .local-deps fallback for x86 validation")
    else()
        message(FATAL_ERROR
            "SensorRecorder requires libserialport. Install libserialport-dev or provide sibling ugripper .local-deps/libserialport.")
    endif()
endif()

set(SENSORRECORDER_MCAP_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mcap/include")
set(SENSORRECORDER_MCAP_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mcap/src/lib.cc")

if(PkgConfig_FOUND)
    pkg_check_modules(LZ4 QUIET IMPORTED_TARGET liblz4)
endif()

add_library(sensorrecorder_mcap STATIC ${SENSORRECORDER_MCAP_SOURCE})
set_target_properties(sensorrecorder_mcap PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(sensorrecorder_mcap
    PUBLIC
        "${SENSORRECORDER_MCAP_INCLUDE_DIR}"
)
if(LZ4_FOUND)
    target_link_libraries(sensorrecorder_mcap PUBLIC PkgConfig::LZ4)
else()
    target_compile_definitions(sensorrecorder_mcap PUBLIC MCAP_COMPRESSION_NO_LZ4)
endif()
target_compile_definitions(sensorrecorder_mcap PUBLIC MCAP_COMPRESSION_NO_ZSTD)

add_library(sensor_protocol STATIC
    sensor_protocol.cc
    bsp_crc.cc
)
set_target_properties(sensor_protocol PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(sensor_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(sensor_domain STATIC
    sensor_domain.cc
)
set_target_properties(sensor_domain PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(sensor_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(SENSORRECORDER_COMMON_SOURCES
    encoder_driver.cc
)

add_executable(${APP_NAME}
    main.cc
    ${SENSORRECORDER_COMMON_SOURCES}
)
set_target_properties(${APP_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "SensorRecorder"
)
target_include_directories(${APP_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../UgripperRuntime/include
)
target_link_libraries(${APP_NAME}
    PRIVATE
        sensor_protocol
        sensor_domain
        sensorrecorder_mcap
        PP::utils
        ${SENSORRECORDER_LIBSERIALPORT_TARGET}
        Threads::Threads
)
if(NOT SENSORRECORDER_HAS_SYSTEM_LIBSERIALPORT)
    set_target_properties(${APP_NAME} PROPERTIES
        BUILD_RPATH "${SENSORRECORDER_LIBSERIALPORT_LIBRARY_DIR}"
        INSTALL_RPATH "${SENSORRECORDER_LIBSERIALPORT_LIBRARY_DIR}"
    )
endif()

add_executable(zeroing
    zeroing.cc
    encoder_driver.cc
)
set_target_properties(zeroing PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "zeroing"
)
target_include_directories(zeroing
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(zeroing
    PRIVATE
        sensor_protocol
        PP::utils
        ${SENSORRECORDER_LIBSERIALPORT_TARGET}
        Threads::Threads
)
if(NOT SENSORRECORDER_HAS_SYSTEM_LIBSERIALPORT)
    set_target_properties(zeroing PROPERTIES
        BUILD_RPATH "${SENSORRECORDER_LIBSERIALPORT_LIBRARY_DIR}"
        INSTALL_RPATH "${SENSORRECORDER_LIBSERIALPORT_LIBRARY_DIR}"
    )
endif()

install(TARGETS ${APP_NAME} zeroing
    RUNTIME DESTINATION bin/SensorRecorder
    LIBRARY DESTINATION lib
)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/99-serial.rules
    DESTINATION bin/SensorRecorder
)
