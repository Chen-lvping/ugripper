cmake_minimum_required(VERSION 3.10)

include(GoogleTest)

function(add_host_only_camera_gtest target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name}
        PRIVATE
            camera_domain
            GTest::gtest_main
    )
    gtest_discover_tests(${target_name}
        PROPERTIES
            LABELS "host-only;unit;camera"
    )
endfunction()

add_host_only_camera_gtest(test_camera_config
    test_camera_config.cc
)

add_host_only_camera_gtest(test_camera_command_builder
    test_camera_command_builder.cc
)

add_host_only_camera_gtest(test_stereo_control_json
    test_stereo_control_json.cc
)
