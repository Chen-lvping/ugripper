#pragma once
#include <spdlog/async.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>

#include "log_fmtter.h"
#include "singleton.h"
#if __cplusplus >= 202002L
#  include <source_location>
#endif

namespace DA {
  namespace utils {
    class SpdLogger : public Singleton<SpdLogger> {
    public:
      // 初始化日志系统
      static void init(const std::string& name = "app", const std::string& level = "info",
                       const std::string& filepath = "logs/app.log",
                       size_t max_size = 1024 * 1024 * 10,  // 10MB
                       size_t max_files = 5, bool enable_file_logging = true) {
        std::call_once(init_flag_, [&] {
          GetInstance().Initialize(name, level, filepath, max_size, max_files, enable_file_logging);
        });
      }

#if __cplusplus >= 202002L
      // 日志输出接口
      template <typename... Args> inline void trace(const std::source_location& loc,
                                                    fmt::format_string<Args...> fmt,
                                                    Args&&... args) {
        if (logger_) {
          logger_->log(GetInstanceLocation(loc), spdlog::level::trace, fmt,
                       std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void debug(const std::source_location& loc,
                                                    fmt::format_string<Args...> fmt,
                                                    Args&&... args) {
        if (logger_) {
          logger_->log(GetInstanceLocation(loc), spdlog::level::debug, fmt,
                       std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void info(const std::source_location& loc,
                                                   fmt::format_string<Args...> fmt,
                                                   Args&&... args) {
        if (logger_) {
          logger_->log(GetInstanceLocation(loc), spdlog::level::info, fmt,
                       std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void warn(const std::source_location& loc,
                                                   fmt::format_string<Args...> fmt,
                                                   Args&&... args) {
        if (logger_) {
          logger_->log(GetInstanceLocation(loc), spdlog::level::warn, fmt,
                       std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void error(const std::source_location& loc,
                                                    fmt::format_string<Args...> fmt,
                                                    Args&&... args) {
        if (logger_) {
          logger_->log(GetInstanceLocation(loc), spdlog::level::err, fmt,
                       std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void critical(const std::source_location& loc,
                                                       fmt::format_string<Args...> fmt,
                                                       Args&&... args) {
        if (logger_) {
          logger_->log(GetInstanceLocation(loc), spdlog::level::critical, fmt,
                       std::forward<Args>(args)...);
        }
      }
#endif

      // C++17 fallback: 手动传递 source location (file, line, func)
      template <typename... Args>
      inline void trace(const char* file, int line, const char* func,
                       const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->log(spdlog::source_loc{file, line, func}, spdlog::level::trace,
                       fmt::runtime(fmt), std::forward<Args>(args)...);
        }
      }

      template <typename... Args>
      inline void debug(const char* file, int line, const char* func,
                       const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->log(spdlog::source_loc{file, line, func}, spdlog::level::debug,
                       fmt::runtime(fmt), std::forward<Args>(args)...);
        }
      }

      template <typename... Args>
      inline void info(const char* file, int line, const char* func,
                      const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->log(spdlog::source_loc{file, line, func}, spdlog::level::info,
                       fmt::runtime(fmt), std::forward<Args>(args)...);
        }
      }

      template <typename... Args>
      inline void warn(const char* file, int line, const char* func,
                      const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->log(spdlog::source_loc{file, line, func}, spdlog::level::warn,
                       fmt::runtime(fmt), std::forward<Args>(args)...);
        }
      }

      template <typename... Args>
      inline void error(const char* file, int line, const char* func,
                       const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->log(spdlog::source_loc{file, line, func}, spdlog::level::err,
                       fmt::runtime(fmt), std::forward<Args>(args)...);
        }
      }

      template <typename... Args>
      inline void critical(const char* file, int line, const char* func,
                          const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->log(spdlog::source_loc{file, line, func}, spdlog::level::critical,
                       fmt::runtime(fmt), std::forward<Args>(args)...);
        }
      }

      // 不带 source location 的版本（保持向后兼容）
      template <typename... Args> inline void trace(const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->trace(fmt, std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void debug(const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->debug(fmt, std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void info(const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->info(fmt, std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void warn(const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->warn(fmt, std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void error(const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->error(fmt, std::forward<Args>(args)...);
        }
      }

      template <typename... Args> inline void critical(const char* fmt, Args&&... args) {
        if (logger_) {
          logger_->critical(fmt, std::forward<Args>(args)...);
        }
      }

      // 设置日志级别
      void SetLevel(const std::string& level);
      // 获取当前日志级别
      spdlog::level::level_enum GetLevel() const;
      // 获取当前日志级别字符串表示
      std::string GetLevel_str() const;

      // 立刻刷新日志
      void flush() { logger_->flush(); }

      static void EnableFileLogging(bool enable, const std::string& filepath = "",
                                    size_t max_size = 1024 * 1024 * 10, size_t max_files = 5) {
        GetInstance().UpdateFileLogging(enable, filepath, max_size, max_files);
      }

      SpdLogger(const SpdLogger&) = delete;
      SpdLogger& operator=(const SpdLogger&) = delete;

    private:
      friend class Singleton<SpdLogger>;
      SpdLogger() = default;
      ~SpdLogger() override {}

      void Initialize(const std::string& name, const std::string& level,
                      const std::string& filepath, size_t max_size, size_t max_files,
                      bool enable_file_logging);

      void UpdateFileLogging(bool enable, const std::string& filepath, size_t max_size,
                             size_t max_files);
#if __cplusplus >= 202002L
      spdlog::source_loc GetInstanceLocation(const std::source_location& loc) {
        return spdlog::source_loc{loc.file_name(), static_cast<int>(loc.line()),
                                  loc.function_name()};
      }
#endif

      std::shared_ptr<spdlog::logger> logger_;
      static std::once_flag init_flag_;
      std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file_sink_;
      bool file_logging_enabled_;
      std::mutex logger_mutex_;
    };
  }  // namespace utils
}  // namespace DA
