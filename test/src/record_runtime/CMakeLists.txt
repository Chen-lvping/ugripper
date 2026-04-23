cmake_minimum_required(VERSION 3.10)

include(GoogleTest)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(add_host_only_runtime_gtest target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}/standalone/UgripperRuntime/include
            ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(${target_name}
        PRIVATE
            runtime_control_plane
            runtime_process_boundary
            PP::utils
            GTest::gtest_main
    )
    gtest_discover_tests(${target_name}
        PROPERTIES
            LABELS "host-only;unit;record_runtime"
    )
endfunction()

add_host_only_runtime_gtest(test_process_policy
    test_process_policy.cc
)

add_host_only_runtime_gtest(test_audio_coordinator
    test_audio_coordinator.cc
)

add_host_only_runtime_gtest(test_stereo_session_client
    test_stereo_session_client.cc
)

add_host_only_runtime_gtest(test_button_logic
    test_button_logic.cc
)

add_host_only_runtime_gtest(test_button_event_timing
    test_button_event_timing.cc
)

add_host_only_runtime_gtest(test_gripper_refresh_logic
    test_gripper_refresh_logic.cc
)

add_host_only_runtime_gtest(test_runtime_health
    test_runtime_health.cc
)

add_host_only_runtime_gtest(test_recording_orchestrator
    test_recording_orchestrator.cc
)

add_host_only_runtime_gtest(test_led_state_bridge
    test_led_state_bridge.cc
)

add_host_only_runtime_gtest(test_shutdown_request_port
    test_shutdown_request_port.cc
)

add_test(
    NAME test_check_hmi_event_log_pass
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_hmi_event_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hmi_diag_pass.log
            --require-event ShortUpPressed
            --require-led-state Error2
)
set_tests_properties(test_check_hmi_event_log_pass PROPERTIES
    LABELS "host-only;unit;record_runtime"
)

add_test(
    NAME test_check_hmi_event_log_fail
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_hmi_event_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hmi_diag_fail.log
)
set_tests_properties(test_check_hmi_event_log_fail PROPERTIES
    LABELS "host-only;unit;record_runtime"
    WILL_FAIL TRUE
)

add_test(
    NAME test_check_hmi_event_log_led_mismatch
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_hmi_event_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hmi_diag_led_mismatch.log
)
set_tests_properties(test_check_hmi_event_log_led_mismatch PROPERTIES
    LABELS "host-only;unit;record_runtime"
    WILL_FAIL TRUE
)

add_test(
    NAME test_check_hmi_event_log_health_faults_pass
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_hmi_event_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hmi_diag_health_faults_pass.log
            --require-health-fault
            hmi_all_disconnected
            --require-health-fault
            hmi_ports_inactive
            --require-health-fault
            stereo_daemon_not_running
            --require-health-fault-led
            hmi_all_disconnected:Error2
            --require-health-fault-led
            hmi_ports_inactive:Error2
            --require-health-fault-led
            stereo_daemon_not_running:Error2
)
set_tests_properties(test_check_hmi_event_log_health_faults_pass PROPERTIES
    LABELS "host-only;unit;record_runtime"
)

add_test(
    NAME test_check_hmi_event_log_health_faults_fail
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/check_hmi_event_log.py
            --input
            ${CMAKE_CURRENT_SOURCE_DIR}/testdata/hmi_diag_health_faults_fail.log
            --require-health-fault
            hmi_ports_inactive
            --require-health-fault-led
            hmi_ports_inactive:Error2
            --require-health-fault
            stereo_daemon_not_running
)
set_tests_properties(test_check_hmi_event_log_health_faults_fail PROPERTIES
    LABELS "host-only;unit;record_runtime"
    WILL_FAIL TRUE
)

add_test(
    NAME test_summarize_episode_reports
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/test/scripts/test_summarize_episode_reports.py
)
set_tests_properties(test_summarize_episode_reports PROPERTIES
    LABELS "host-only;unit;record_runtime"
)
