#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace utils {
namespace detail {

inline std::mutex &LogMutex()
{
    static std::mutex mutex;
    return mutex;
}

// Transitional logger shim for ugripper refactor.
// Keep the API surface close to pp_main where practical, but avoid importing
// the full backend before standalone integration.
template <typename... Args>
inline void LogInit(Args&&...)
{
}

class LogStream
{
public:
    LogStream(std::ostream &stream, const char *level)
        : stream_(stream), level_(level)
    {
    }

    ~LogStream()
    {
        const std::lock_guard<std::mutex> lock(LogMutex());
        stream_ << "[" << level_ << "] " << buffer_.str() << std::endl;
    }

    template <typename T>
    LogStream &operator<<(T &&value)
    {
        buffer_ << std::forward<T>(value);
        return *this;
    }

    LogStream &operator<<(std::ostream &(*manip)(std::ostream &))
    {
        if (manip != static_cast<std::ostream &(*)(std::ostream &)>(std::endl))
        {
            manip(buffer_);
        }
        return *this;
    }

    LogStream &operator<<(std::ios_base &(*manip)(std::ios_base &))
    {
        manip(buffer_);
        return *this;
    }

private:
    std::ostream &stream_;
    const char *level_;
    std::ostringstream buffer_;
};

template <typename... Args>
inline void Log(std::ostream &stream, const char *level, Args &&...args)
{
    std::ostringstream buffer;
    ((buffer << std::forward<Args>(args)), ...);
    const std::lock_guard<std::mutex> lock(LogMutex());
    stream << "[" << level << "] " << buffer.str() << std::endl;
}

}  // namespace detail
}  // namespace utils

#define DM_LOG_DEBUG(...) ::utils::detail::Log(::std::cout, "DEBUG", __VA_ARGS__)
#define DM_LOG_INFO(...) ::utils::detail::Log(::std::cout, "INFO", __VA_ARGS__)
#define DM_LOG_TRACE(...) ::utils::detail::Log(::std::cout, "TRACE", __VA_ARGS__)
#define DM_LOG_WARN(...) ::utils::detail::Log(::std::cerr, "WARN", __VA_ARGS__)
#define DM_LOG_ERROR(...) ::utils::detail::Log(::std::cerr, "ERROR", __VA_ARGS__)
#define DM_LOG_CRITICAL(...) ::utils::detail::Log(::std::cerr, "CRITICAL", __VA_ARGS__)

#define DM_LOG_INIT(...) ::utils::detail::LogInit(__VA_ARGS__)

#define DM_LOG_DEBUG_STREAM() ::utils::detail::LogStream(::std::cout, "DEBUG")
#define DM_LOG_INFO_STREAM() ::utils::detail::LogStream(::std::cout, "INFO")
#define DM_LOG_TRACE_STREAM() ::utils::detail::LogStream(::std::cout, "TRACE")
#define DM_LOG_WARN_STREAM() ::utils::detail::LogStream(::std::cerr, "WARN")
#define DM_LOG_ERROR_STREAM() ::utils::detail::LogStream(::std::cerr, "ERROR")
#define DM_LOG_CRITICAL_STREAM() ::utils::detail::LogStream(::std::cerr, "CRITICAL")
