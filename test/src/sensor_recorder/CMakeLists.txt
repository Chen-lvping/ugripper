include(GoogleTest)
find_package(PkgConfig QUIET)

set(TEST_SENSORRECORDER_MCAP_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/standalone/SensorRecorder/third_party/mcap/include")

add_library(sensor_mcap_checker STATIC
    sensor_mcap_checker.cc
)
set_target_properties(sensor_mcap_checker PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(sensor_mcap_checker
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/standalone/SensorRecorder/include
        ${CMAKE_SOURCE_DIR}/src
        "${TEST_SENSORRECORDER_MCAP_INCLUDE_DIR}"
)
target_link_libraries(sensor_mcap_checker
    PUBLIC
        sensorrecorder_mcap
        nlohmann_json::nlohmann_json
)

function(add_host_only_sensor_gtest target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/standalone/SensorRecorder/include
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(${target_name}
        PRIVATE
            sensor_protocol
            sensor_domain
            GTest::gtest_main
    )
    gtest_discover_tests(${target_name}
        PROPERTIES
            LABELS "host-only;unit;sensor"
    )
endfunction()

add_host_only_sensor_gtest(test_bsp_crc
    test_bsp_crc.cc
)

add_host_only_sensor_gtest(test_encoder_protocol
    test_encoder_protocol.cc
)

add_host_only_sensor_gtest(test_imu_batch_timestamp
    test_imu_batch_timestamp.cc
)

add_host_only_sensor_gtest(test_sensor_mcap_checker
    test_sensor_mcap_checker.cc
)
target_link_libraries(test_sensor_mcap_checker
    PRIVATE
        sensor_mcap_checker
)

add_executable(check_sensor_mcap
    check_sensor_mcap.cc
)
set_target_properties(check_sensor_mcap PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(check_sensor_mcap
    PRIVATE
        ${CMAKE_SOURCE_DIR}/standalone/SensorRecorder/include
        ${CMAKE_SOURCE_DIR}/src
        "${TEST_SENSORRECORDER_MCAP_INCLUDE_DIR}"
)
target_link_libraries(check_sensor_mcap
    PRIVATE
        sensor_mcap_checker
)
