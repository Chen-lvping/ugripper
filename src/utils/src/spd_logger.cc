#include "spd_logger.h"

namespace DA {
  namespace utils {
    std::once_flag SpdLogger::init_flag_;

    void SpdLogger::Initialize(const std::string& name, const std::string& level,
                               const std::string& filepath, size_t max_size, size_t max_files,
                               bool enable_file_logging) {
      try {
        // 创建线程池
        spdlog::init_thread_pool(8192, 1);

        // 创建 sink 组合
        std::vector<spdlog::sink_ptr> sinks;

        // 控制台 sink（启用颜色）
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_formatter(std::make_unique<CustomizedLogFormatter>(true));
        sinks.push_back(console_sink);

        // 根据设置添加文件 sink
        if (enable_file_logging) {
          file_sink_ = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filepath, max_size,
                                                                              max_files);
          file_sink_->set_formatter(std::make_unique<CustomizedLogFormatter>(false));
          sinks.push_back(file_sink_);
          file_logging_enabled_ = true;
        }

        // 创建异步日志器
        logger_ = std::make_shared<spdlog::async_logger>(name, sinks.begin(), sinks.end(),
                                                         spdlog::thread_pool(),
                                                         spdlog::async_overflow_policy::block);

        SetLevel(level);
        spdlog::register_logger(logger_);
        spdlog::set_default_logger(logger_);

        // 记录初始化消息（使用标准方式）
        logger_->info("SpdLogger initialized with Unix timestamp and color support\n");
      } catch (const spdlog::spdlog_ex& ex) {
        // 失败时降级到控制台日志
        logger_ = spdlog::stdout_color_mt(name);
        logger_->error("Log initialization failed: {}", ex.what());
      }
    }

    void SpdLogger::SetLevel(const std::string& level) {
      if (logger_) {
        if (level.empty()) {
          return;
        }
        if ("error" == level) {
          logger_->set_level(spdlog::level::from_str("err"));
          return;
        }
        logger_->set_level(spdlog::level::from_str(level));
      }
    }

    spdlog::level::level_enum SpdLogger::GetLevel() const {
      if (logger_) {
        return logger_->level();
      }
      // 如果日志器未初始化，返回一个默认级别（例如info）
      return spdlog::level::info;
    }

    std::string SpdLogger::GetLevel_str() const {
      if (logger_) {
        return spdlog::level::to_string_view(logger_->level()).data();
      }
      return "info";
    }

    void SpdLogger::UpdateFileLogging(bool enable, const std::string& filepath, size_t max_size,
                                      size_t max_files) {
      std::lock_guard<std::mutex> lock(logger_mutex_);

      if (enable && !file_logging_enabled_) {
        // 启用文件日志
        try {
          file_sink_ = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filepath, max_size,
                                                                              max_files);
          file_sink_->set_formatter(std::make_unique<CustomizedLogFormatter>(false));
          logger_->sinks().push_back(file_sink_);
          file_logging_enabled_ = true;
          logger_->info("File logging enabled to {}", filepath);
        } catch (const spdlog::spdlog_ex& ex) {
          logger_->error("Failed to enable file logging: {}", ex.what());
        }
      } else if (!enable && file_logging_enabled_) {
        // 禁用文件日志
        auto it = std::find(logger_->sinks().begin(), logger_->sinks().end(), file_sink_);
        if (it != logger_->sinks().end()) {
          logger_->sinks().erase(it);
          file_logging_enabled_ = false;
          logger_->info("File logging disabled");
        }
      }
    }

  }  // namespace utils
}  // namespace DA
