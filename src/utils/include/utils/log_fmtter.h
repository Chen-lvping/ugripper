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


namespace DA {
  namespace utils {

    class CustomizedLogFormatter : public spdlog::formatter {
    public:
      explicit CustomizedLogFormatter(bool enable_color) : enable_color_(enable_color) {}

      void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override;

      std::unique_ptr<formatter> clone() const override {
        return std::make_unique<CustomizedLogFormatter>(enable_color_);
      }

    private:
      static std::string_view extract_class_name(std::string_view funcName);

      // 提取函数名
      static std::string_view extract_function_name(std::string_view funcName);

      const char* get_level_color_start(spdlog::level::level_enum level);

      bool enable_color_;
    };

  }  // namespace utils
}  // namespace DA
