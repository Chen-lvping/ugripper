cmake_minimum_required(VERSION 3.10)

include(GoogleTest)

function(add_host_only_gtest target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name}
        PRIVATE
            utils
            GTest::gtest_main
    )
    gtest_discover_tests(${target_name}
        PROPERTIES
            LABELS "host-only;unit;utils"
    )
endfunction()

add_host_only_gtest(test_env_utils
    test_env_utils.cc
)

add_host_only_gtest(test_file_utils
    test_file_utils.cc
)

add_host_only_gtest(test_time_utils
    test_time_utils.cc
)
