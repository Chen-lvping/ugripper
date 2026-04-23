#pragma once

#include "record_runtime/process_supervisor.h"
#include "record_runtime/runtime_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ugripper::runtime {

struct HealthMonitorOptions
{
    std::string disk_root;
    std::string stereo_status_file;
    std::vector<std::string> critical_device_paths;
    uint64_t poll_interval_ms = 1000;
    uint64_t hmi_active_timeout_ms = 2500;
};

class HealthMonitor
{
public:
    using NowMsFn = uint64_t (*)();
    using DiskWritableFn = std::function<bool(const std::string&)>;
    using PathExistsFn = std::function<bool(const std::string&)>;
    using GetProcessStatusFn = std::function<ProcessStatus(WorkerName)>;
    using GetHmiHealthFn = std::function<HmiHealthSnapshot(uint64_t)>;

    struct Dependencies
    {
        DiskWritableFn is_disk_writable;
        PathExistsFn path_exists;
        GetProcessStatusFn get_process_status;
        GetHmiHealthFn get_hmi_health;
    };

    struct PollResult
    {
        bool checked = false;
        bool should_notify_fault = false;
        bool recovered = false;
        HealthState state;
        std::optional<HealthFault> fault;
    };

    HealthMonitor(HealthMonitorOptions options, Dependencies dependencies, NowMsFn now_ms_fn);

    PollResult Poll(const HealthState& current_state) const;
    std::optional<HealthFault> EvaluateHealth() const;

private:
    HealthMonitorOptions options_;
    Dependencies dependencies_;
    NowMsFn now_ms_fn_ = nullptr;
};

}  // namespace ugripper::runtime
