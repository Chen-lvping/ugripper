cmake_minimum_required(VERSION 3.10)

include(GoogleTest)

function(add_host_only_gtest target_name)
    add_executable(${target_name} ${ARGN})
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

add_host_only_gtest(test_bsp_crc
    test_bsp_crc.cc
)

add_host_only_gtest(test_encoder_protocol
    test_encoder_protocol.cc
)

add_host_only_gtest(test_imu_batch_timestamp
    test_imu_batch_timestamp.cc
)
