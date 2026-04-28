#include "log_fmtter.h"

#if __cplusplus >= 202002L
#  include <source_location>
#endif

namespace DA {
  namespace utils {

    void CustomizedLogFormatter::format(const spdlog::details::log_msg& msg,
                                             spdlog::memory_buf_t& dest) {
      // 将时间转换为 Unix 时间戳（微秒）
      auto duration = msg.time.time_since_epoch();
      auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
      uint64_t seconds = static_cast<uint64_t>(micros) / 1000000;
      uint32_t microseconds = static_cast<uint32_t>(micros % 1000000);

      // 格式化时间戳
      fmt::format_to(std::back_inserter(dest), "[{}.{:06d}]", seconds, microseconds);

      // 添加线程ID
      fmt::format_to(std::back_inserter(dest), "[tid {}]", msg.thread_id);

      // 添加带颜色的日志级别
      if (enable_color_) {
        // ANSI 颜色代码
        const char* color_start = get_level_color_start(msg.level);
        const char* color_end = "\033[0m";  // 重置颜色

        fmt::format_to(std::back_inserter(dest), "{}[{}]{}", color_start,
                       spdlog::level::to_short_c_str(msg.level), color_end);
      } else {
        // 无颜色版本
        fmt::format_to(std::back_inserter(dest), "[{}]", spdlog::level::to_short_c_str(msg.level));
      }
      // // 添加类名和函数名（如果存在）
      if (msg.source.funcname) {
        // 解析类名和函数名
        std::string_view funcView(msg.source.funcname);
        std::string_view classView = extract_class_name(funcView);
         std::string_view funcName = extract_function_name(funcView);

        // 如果无法提取函数名（如 __func__ 返回的简单名称），直接使用原始字符串
        if (funcName.empty()) {
          funcName = funcView;
        }

        if (!classView.empty()) {
          fmt::format_to(std::back_inserter(dest), "[{}.", classView);
        } else {
          fmt::format_to(std::back_inserter(dest), "[");
        }
        fmt::format_to(std::back_inserter(dest), "{}", funcName);
        if (msg.source.line) {
          fmt::format_to(std::back_inserter(dest), " {}", msg.source.line);
        }
        fmt::format_to(std::back_inserter(dest), "]");
      }

      // 添加日志消息
      fmt::format_to(std::back_inserter(dest), " {}\n", msg.payload);
    }

    std::string_view CustomizedLogFormatter::extract_class_name(std::string_view funcName) {
      size_t firstBracket = funcName.find_first_of("(");
      if (firstBracket == std::string_view::npos) {
        return "";
      }
      auto funcName_ = funcName.substr(0, firstBracket);

      // 查找作用域解析运算符
      size_t last_colon = funcName_.find_last_of("::");
      if (last_colon == std::string_view::npos) {
        return "";
      }
      auto className = funcName_.substr(0, last_colon - 1);
      if (className.find("::") != std::string_view::npos) {
        return className.substr(className.find_last_of("::") + 1);
      }
      return className;
    }

    std::string_view CustomizedLogFormatter::extract_function_name(std::string_view funcName) {
      size_t firstBracket = funcName.find_first_of("(");
      if (firstBracket == std::string_view::npos) {
        return "";
      }
      auto funcName_ = funcName.substr(0, firstBracket);

      // 查找作用域解析运算符
      size_t last_colon = funcName_.find_last_of("::");
      if (last_colon == std::string_view::npos) {
        size_t firstBlank = funcName_.find_last_of(" ");
        if (firstBlank == std::string_view::npos) {
          return "";
        }
        return funcName_.substr(firstBlank + 1);
      }
      return funcName_.substr(last_colon + 1);
    }

    const char* CustomizedLogFormatter::get_level_color_start(
        spdlog::level::level_enum level) {
      switch (level) {
        case spdlog::level::trace:
          return "\033[90m";  // 灰色
        case spdlog::level::debug:
          return "\033[94m";  // 蓝色
        case spdlog::level::info:
          return "\033[92m";  // 绿色
        case spdlog::level::warn:
          return "\033[93m";  // 黄色
        case spdlog::level::err:
          return "\033[91m";  // 红色
        case spdlog::level::critical:
          return "\033[95m";  // 紫色
        default:
          return "";
      }
    }

  }  // namespace utils
}  // namespace DA
