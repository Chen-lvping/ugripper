cmake_minimum_required(VERSION 3.10)

include(GoogleTest)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(add_host_only_gtest target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name}
        PRIVATE
            GTest::gtest_main
    )
    gtest_discover_tests(${target_name}
        PROPERTIES
            LABELS "host-only;unit;gripper_hmi"
    )
endfunction()

add_host_only_gtest(test_hmi_protocol
    test_hmi_protocol.cc
)
target_include_directories(test_hmi_protocol
    PRIVATE
        ${CMAKE_SOURCE_DIR}/standalone/GripperHmiTool/include
)
target_link_libraries(test_hmi_protocol
    PRIVATE
        gripper_hmi_protocol
)

add_host_only_gtest(test_hmi_led_effects
    test_hmi_led_effects.cc
)
target_include_directories(test_hmi_led_effects
    PRIVATE
        ${CMAKE_SOURCE_DIR}/standalone/GripperHmiTool/include
)
target_link_libraries(test_hmi_led_effects
    PRIVATE
        gripper_hmi_led_effects
)

add_host_only_gtest(test_hmi_transport_resync
    test_hmi_transport_resync.cc
)
target_include_directories(test_hmi_transport_resync
    PRIVATE
        ${CMAKE_SOURCE_DIR}/standalone/GripperHmiTool/include
)
target_link_libraries(test_hmi_transport_resync
    PRIVATE
        gripper_hmi_protocol
)

add_host_only_gtest(test_hmi_driver_logic
    test_hmi_driver_logic.cc
)
target_include_directories(test_hmi_driver_logic
    PRIVATE
        ${CMAKE_SOURCE_DIR}/standalone/GripperHmiTool/include
)
target_link_libraries(test_hmi_driver_logic
    PRIVATE
        gripper_hmi_protocol
)

add_test(
    NAME test_check_gripper_ack_log_pass
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_pass.log
            --require-command read_serial_number
            --require-command write_calibration
)
set_tests_properties(test_check_gripper_ack_log_pass PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
)

add_test(
    NAME test_check_gripper_ack_log_fail
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_fail.log
            --require-command read_serial_number
)
set_tests_properties(test_check_gripper_ack_log_fail PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
    WILL_FAIL TRUE
)

add_test(
    NAME test_check_gripper_ack_log_io_fail
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_io_fail_only.log
            --require-command read_serial_number
)
set_tests_properties(test_check_gripper_ack_log_io_fail PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
    WILL_FAIL TRUE
)

add_test(
    NAME test_check_gripper_ack_log_retry_reconnect_pass
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_retry_reconnect_pass.log
            --allow-timeouts
            --require-command
            read_serial_number
            --require-command-retries-at-least
            read_serial_number:2
            --require-command-recoveries-at-least
            read_serial_number:1
            --require-io-total-at-least
            reconnect_success:1
            --require-io-total-at-least
            exclusive_retries:2
            --require-io-total-at-least
            exclusive_recoveries:1
)
set_tests_properties(test_check_gripper_ack_log_retry_reconnect_pass PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
)

add_test(
    NAME test_check_gripper_ack_log_retry_reconnect_fail
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_retry_reconnect_fail.log
            --require-command
            read_serial_number
            --require-command-retries-at-least
            read_serial_number:2
            --require-command-recoveries-at-least
            read_serial_number:1
            --require-io-total-at-least
            reconnect_success:1
)
set_tests_properties(test_check_gripper_ack_log_retry_reconnect_fail PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
    WILL_FAIL TRUE
)

add_test(
    NAME test_check_gripper_ack_log_periodic_io_totals_pass
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_pass.log
            --require-io-total-at-least
            tx_led:5
            --require-io-total-at-least
            tx_beep:2
            --require-io-total-at-least
            tx_state_req:3
            --require-io-total-at-least
            rx_frames:10
            --require-io-total-at-least
            rx_key_reports:4
            --require-io-total-at-least
            rx_beep_states:6
)
set_tests_properties(test_check_gripper_ack_log_periodic_io_totals_pass PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
)

add_test(
    NAME test_check_gripper_ack_log_periodic_io_totals_fail
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_gripper_ack_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gripper_diag_io_fail_only.log
            --allow-failures
            --require-io-total-at-least
            tx_led:5
            --require-io-total-at-least
            rx_frames:5
)
set_tests_properties(test_check_gripper_ack_log_periodic_io_totals_fail PROPERTIES
    LABELS "host-only;unit;gripper_hmi"
    WILL_FAIL TRUE
)
