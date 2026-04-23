#pragma once

#include "utils/logger.h"

#include <sstream>
#include <string>
#include <utility>

namespace DA::standalone::gripper_hmi {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
};

class LogStreamAdapter {
public:
    explicit LogStreamAdapter(LogLevel level)
        : level_(level) {}

    ~LogStreamAdapter() {
        const std::string message = stream_.str();
        switch (level_) {
            case LogLevel::Trace:
                DM_LOG_TRACE("{}", message);
                break;
            case LogLevel::Debug:
                DM_LOG_DEBUG("{}", message);
                break;
            case LogLevel::Info:
                DM_LOG_INFO("{}", message);
                break;
            case LogLevel::Warn:
                DM_LOG_WARN("{}", message);
                break;
            case LogLevel::Error:
                DM_LOG_ERROR("{}", message);
                break;
            case LogLevel::Critical:
                DM_LOG_CRITICAL("{}", message);
                break;
        }
    }

    template <typename T>
    LogStreamAdapter& operator<<(T&& value) {
        stream_ << std::forward<T>(value);
        return *this;
    }

    LogStreamAdapter& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (manip != static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
            manip(stream_);
        }
        return *this;
    }

    LogStreamAdapter& operator<<(std::ios_base& (*manip)(std::ios_base&)) {
        manip(stream_);
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream stream_;
};

}  // namespace DA::standalone::gripper_hmi

#ifndef DM_LOG_TRACE_STREAM
#define DM_LOG_TRACE_STREAM() ::DA::standalone::gripper_hmi::LogStreamAdapter(::DA::standalone::gripper_hmi::LogLevel::Trace)
#endif

#ifndef DM_LOG_DEBUG_STREAM
#define DM_LOG_DEBUG_STREAM() ::DA::standalone::gripper_hmi::LogStreamAdapter(::DA::standalone::gripper_hmi::LogLevel::Debug)
#endif

#ifndef DM_LOG_INFO_STREAM
#define DM_LOG_INFO_STREAM() ::DA::standalone::gripper_hmi::LogStreamAdapter(::DA::standalone::gripper_hmi::LogLevel::Info)
#endif

#ifndef DM_LOG_WARN_STREAM
#define DM_LOG_WARN_STREAM() ::DA::standalone::gripper_hmi::LogStreamAdapter(::DA::standalone::gripper_hmi::LogLevel::Warn)
#endif

#ifndef DM_LOG_ERROR_STREAM
#define DM_LOG_ERROR_STREAM() ::DA::standalone::gripper_hmi::LogStreamAdapter(::DA::standalone::gripper_hmi::LogLevel::Error)
#endif

#ifndef DM_LOG_CRITICAL_STREAM
#define DM_LOG_CRITICAL_STREAM() ::DA::standalone::gripper_hmi::LogStreamAdapter(::DA::standalone::gripper_hmi::LogLevel::Critical)
#endif
