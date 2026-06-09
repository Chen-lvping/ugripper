#include "record_runtime/health_monitor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

uint64_t g_now_ms = 0;

uint64_t FakeNowMs()
{
    return g_now_ms;
}

fs::path MakeTempDir()
{
    char dir_template[] = "/tmp/ugripper_health_testXXXXXX";
    const char* created = mkdtemp(dir_template);
    EXPECT_NE(created, nullptr);
    return fs::path(created);
}

using ugripper::runtime::HealthMonitor;
using ugripper::runtime::HealthFault;
using ugripper::runtime::HealthState;
using ugripper::runtime::HealthStatus;
using ugripper::runtime::HardwareFaultSide;
using ugripper::runtime::HmiHealthSnapshot;
using ugripper::runtime::ProcessState;
using ugripper::runtime::ProcessStatus;
using ugripper::runtime::RuntimeLedState;
using ugripper::runtime::WorkerName;

TEST(HealthMonitorTest, ReportsHealthyStateWhenDependenciesAreReady)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    std::map<WorkerName, ProcessStatus> process_status = {
        {WorkerName::StereoDaemon,
         ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 42}},
    };

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string& path) {
                 return path == "/dev/cam0";
             },
         .get_process_status =
             [&process_status](WorkerName worker) {
                 return process_status.at(worker);
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    EXPECT_FALSE(result.fault.has_value());
    EXPECT_EQ(result.state.status, HealthStatus::Ok);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsFaultOnlyOnceUntilStateChanges)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/missing_cam"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return false;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1200;
    auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_TRUE(result.should_notify_fault);
    EXPECT_EQ(result.fault->key, "critical_devices_missing");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Unknown);

    g_now_ms = 2500;
    result = monitor.Poll(result.state);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_FALSE(result.should_notify_fault);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsRecoveryAfterFaultClears)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    bool device_available = false;
    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [&device_available](const std::string&) {
                 return device_available;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1200;
    auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.fault.has_value());

    device_available = true;
    g_now_ms = 2500;
    result = monitor.Poll(result.state);
    EXPECT_TRUE(result.checked);
    EXPECT_FALSE(result.fault.has_value());
    EXPECT_TRUE(result.recovered);
    EXPECT_EQ(result.state.status, HealthStatus::Ok);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ClassifiesMissingLeftAndRightCriticalDevices)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/stereo_left", "/dev/right_encoder"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return false;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "critical_devices_missing");
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Both);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, IgnoresStereoStartupFaultsDuringGraceWindow)
{
    const fs::path temp_dir = MakeTempDir();
    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = (temp_dir / "missing_stereo_status.json").string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500,
         .stereo_startup_grace_ms = 10000},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1000;
    auto result = monitor.Poll(HealthState{});
    EXPECT_TRUE(result.checked);
    EXPECT_FALSE(result.fault.has_value());
    EXPECT_EQ(result.state.status, HealthStatus::Unknown);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsInputHmiDisconnected)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = false,
                     .input_active = true,
                     .disconnected_ports = {"/dev/right_gripper"},
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_ports_disconnected");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Right);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsInputHmiInactive)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = false,
                     .input_last_rx_age_ms = 3200,
                     .inactive_ports = {"/dev/right_gripper"},
                     .inactive_port_details = {"/dev/right_gripper(age_ms=3200,active=0)"},
                     .port_activity = {"/dev/right_gripper(age_ms=3200,active=0)"},
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_input_inactive");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_NE(result.fault->detail.find("input_age_ms=3200"), std::string::npos);
    EXPECT_NE(result.fault->detail.find("port_activity=/dev/right_gripper(age_ms=3200,active=0)"),
              std::string::npos);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsAllHmiDisconnected)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = false,
                     .input_connected = false,
                     .input_active = false,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_all_disconnected");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Both);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsInactiveAuxiliaryHmiPorts)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                     .inactive_ports = {"/dev/left_gripper"},
                     .inactive_port_details = {"/dev/left_gripper(age_ms=2875,active=0)"},
                     .port_activity = {"/dev/right_gripper(age_ms=15,active=1)",
                                       "/dev/left_gripper(age_ms=2875,active=0)"},
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_ports_inactive");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Left);
    EXPECT_NE(result.fault->detail.find("/dev/left_gripper(age_ms=2875,active=0)"), std::string::npos);
    EXPECT_NE(result.fault->detail.find("port_activity=/dev/right_gripper(age_ms=15,active=1), "
                                        "/dev/left_gripper(age_ms=2875,active=0)"),
              std::string::npos);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsRecoveryAfterAuxiliaryHmiPortBecomesActiveAgain)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    bool left_aux_inactive = true;
    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [&left_aux_inactive](uint64_t) {
                 HmiHealthSnapshot snapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
                 if (left_aux_inactive)
                 {
                     snapshot.inactive_ports = {"/dev/left_gripper"};
                 }
                 return snapshot;
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_ports_inactive");
    EXPECT_TRUE(result.should_notify_fault);

    left_aux_inactive = false;
    g_now_ms = 2600;
    result = monitor.Poll(result.state);
    ASSERT_TRUE(result.checked);
    EXPECT_FALSE(result.fault.has_value());
    EXPECT_TRUE(result.recovered);
    EXPECT_EQ(result.state.status, HealthStatus::Ok);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsAuxiliaryHmiPortInactivityAgainAfterRecovery)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    bool left_aux_inactive = true;
    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [&left_aux_inactive](uint64_t) {
                 HmiHealthSnapshot snapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
                 if (left_aux_inactive)
                 {
                     snapshot.inactive_ports = {"/dev/left_gripper"};
                 }
                 return snapshot;
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_ports_inactive");
    EXPECT_TRUE(result.should_notify_fault);

    left_aux_inactive = false;
    g_now_ms = 2600;
    result = monitor.Poll(result.state);
    ASSERT_FALSE(result.fault.has_value());
    EXPECT_TRUE(result.recovered);

    left_aux_inactive = true;
    g_now_ms = 3700;
    result = monitor.Poll(result.state);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "hmi_ports_inactive");
    EXPECT_TRUE(result.should_notify_fault);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsDiskNotWritableBeforeDeviceChecks)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return false;
             },
         .path_exists =
             [](const std::string&) {
                 return false;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "disk_not_writable");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error3);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsDiskFullBeforeDeviceChecks)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":true,"service_state":"ready"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.get_disk_fault =
             [](const std::string& path) -> std::optional<HealthFault> {
                 return HealthFault{
                     .led_state = RuntimeLedState::Error3,
                     .side = HardwareFaultSide::Unknown,
                     .key = "disk_full",
                     .detail = "Disk root has no available space: " + path,
                 };
             },
         .path_exists =
             [](const std::string&) {
                 return false;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "disk_full");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error3);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsStereoStatusMissing)
{
    const fs::path temp_dir = MakeTempDir();

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = (temp_dir / "missing_status.json").string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "stereo_status_missing");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsStereoDaemonNotRunning)
{
    const fs::path temp_dir = MakeTempDir();

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = (temp_dir / "stereo_status.json").string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::ExitedUnexpected, .running = false, .pid = 0};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "stereo_daemon_not_running");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsStereoStatusInvalid)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << "{invalid_json";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "stereo_status_invalid");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsStereoNotReady)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":false,"service_state":"warming","cameras":{"left_stereo":{"ready":false},"right_stereo":{"ready":true}}})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "stereo_not_ready");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Left);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsRecognizedFaysControlFailureAsError4)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({
        "ready": false,
        "service_state": "not-ready",
        "cameras": {
            "left_stereo": {
                "ready": true,
                "state": "ready",
                "stereo_symlink_online": true,
                "imu_symlink_online": true
            },
            "right_stereo": {
                "ready": false,
                "state": "process-not-ready",
                "stereo_symlink_online": true,
                "imu_symlink_online": true
            }
        }
    })";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, ugripper::runtime::kErrorTypeStereoControlFailed);
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error4);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Right);
    EXPECT_NE(result.fault->detail.find("unplug/replug the right gripper"), std::string::npos);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ReportsMissingRecognizedFaysCameraAsError2)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({
        "ready": false,
        "service_state": "not-ready",
        "cameras": {
            "left_stereo": {
                "ready": false,
                "state": "stereo-symlink-missing",
                "stereo_symlink_online": false,
                "imu_symlink_online": true
            },
            "right_stereo": {
                "ready": true,
                "state": "ready",
                "stereo_symlink_online": true,
                "imu_symlink_online": true
            }
        }
    })";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.checked);
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "stereo_not_ready");
    EXPECT_EQ(result.fault->led_state, RuntimeLedState::Error2);
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Left);

    fs::remove_all(temp_dir);
}

TEST(HealthMonitorTest, ClassifiesUnknownStereoNotReadyWithoutSideDetails)
{
    const fs::path temp_dir = MakeTempDir();
    const fs::path stereo_status = temp_dir / "stereo_status.json";
    std::ofstream(stereo_status) << R"({"ready":false,"service_state":"warming"})";

    HealthMonitor monitor(
        {.disk_root = temp_dir.string(),
         .stereo_status_file = stereo_status.string(),
         .critical_device_paths = {"/dev/cam0"},
         .poll_interval_ms = 1000,
         .hmi_active_timeout_ms = 2500},
        {.is_disk_writable =
             [](const std::string&) {
                 return true;
             },
         .path_exists =
             [](const std::string&) {
                 return true;
             },
         .get_process_status =
             [](WorkerName) {
                 return ProcessStatus{.state = ProcessState::Running, .running = true, .pid = 7};
             },
         .get_hmi_health =
             [](uint64_t) {
                 return HmiHealthSnapshot{
                     .has_connected_device = true,
                     .input_connected = true,
                     .input_active = true,
                 };
             }},
        &FakeNowMs);

    g_now_ms = 1500;
    const auto result = monitor.Poll(HealthState{});
    ASSERT_TRUE(result.fault.has_value());
    EXPECT_EQ(result.fault->key, "stereo_not_ready");
    EXPECT_EQ(result.fault->side, HardwareFaultSide::Unknown);

    fs::remove_all(temp_dir);
}

}  // namespace
