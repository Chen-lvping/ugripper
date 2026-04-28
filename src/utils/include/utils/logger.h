#pragma once

#include "file_logger.h"
#include "log_string.h"

#ifdef ROS1_APP
#  warning "Compiling with ROS1_APP"
#  include <ros/console.h>
#  include <ros/ros.h>
#  include <spdlog/fmt/fmt.h>
#  define DM_LOG_INIT(enable_file, file_path) \
  do { \
    ::DA::utils::FileLogger::Init((enable_file), (file_path)); \
  } while (0)

#  define DM_ROS1_LOG_IMPL(ROS_MACRO, LEVEL_CHAR, ...) \
  do { \
    auto _msg = fmt::format(__VA_ARGS__); \
    ROS_MACRO("%s", _msg.c_str()); \
    ::DA::utils::FileLogger::Log((LEVEL_CHAR), _msg, __FILE__, __LINE__, __func__); \
  } while (0)

#  define DM_LOG_DEBUG(...)    DM_ROS1_LOG_IMPL(ROS_DEBUG,    "D", __VA_ARGS__)
#  define DM_LOG_INFO(...)     DM_ROS1_LOG_IMPL(ROS_INFO,     "I", __VA_ARGS__)
#  define DM_LOG_WARN(...)     DM_ROS1_LOG_IMPL(ROS_WARN,     "W", __VA_ARGS__)
#  define DM_LOG_ERROR(...)    DM_ROS1_LOG_IMPL(ROS_ERROR,    "E", __VA_ARGS__)
#  define DM_LOG_CRITICAL(...) DM_ROS1_LOG_IMPL(ROS_FATAL,    "F", __VA_ARGS__)


// ------------------------- ROS2 -------------------------
#elif ROS2_APP
#  warning "Compiling with ROS2_APP"
#  include "rclcpp/rclcpp.hpp"
#  include <spdlog/fmt/fmt.h>
#  define DM_LOG_INIT(enable_file, file_path) \
  do { \
    ::DA::utils::FileLogger::Init((enable_file), (file_path)); \
  } while (0)

#  define DM_ROS2_LOG_IMPL(RCLCPP_MACRO, LEVEL_CHAR, ...) \
  do { \
    auto _msg = fmt::format(__VA_ARGS__); \
    RCLCPP_MACRO(rclcpp::get_logger(""), "%s", _msg.c_str()); \
    ::DA::utils::FileLogger::Log((LEVEL_CHAR), _msg, __FILE__, __LINE__, __func__); \
  } while (0)

#  define DM_LOG_DEBUG(...)    DM_ROS2_LOG_IMPL(RCLCPP_DEBUG,    "D", __VA_ARGS__)
#  define DM_LOG_INFO(...)     DM_ROS2_LOG_IMPL(RCLCPP_INFO,     "I", __VA_ARGS__)
#  define DM_LOG_WARN(...)     DM_ROS2_LOG_IMPL(RCLCPP_WARN,     "W", __VA_ARGS__)
#  define DM_LOG_ERROR(...)    DM_ROS2_LOG_IMPL(RCLCPP_ERROR,    "E", __VA_ARGS__)
#  define DM_LOG_CRITICAL(...) DM_ROS2_LOG_IMPL(RCLCPP_CRITICAL, "F", __VA_ARGS__)


// ------------------------- Default Logger (spdlog) -------------------------
#else
#  include "spd_logger.h"
#  warning "Compiling with default logger (spdlog)"
#  define LOGGER DA::utils::SpdLogger::GetInstance()
#  define DM_LOG_INIT LOGGER.init
#  if __cplusplus >= 202002L
#    define DM_LOG_TRACE(...) LOGGER.trace(std::source_location::current(), __VA_ARGS__)
#    define DM_LOG_DEBUG(...) LOGGER.debug(std::source_location::current(), __VA_ARGS__)
#    define DM_LOG_INFO(...) LOGGER.info(std::source_location::current(), __VA_ARGS__)
#    define DM_LOG_WARN(...) LOGGER.warn(std::source_location::current(), __VA_ARGS__)
#    define DM_LOG_ERROR(...) LOGGER.error(std::source_location::current(), __VA_ARGS__)
#    define DM_LOG_CRITICAL(...) LOGGER.critical(std::source_location::current(), __VA_ARGS__)
#  else
#    define DM_LOG_TRACE(...) LOGGER.trace(__FILE__, __LINE__, __func__, __VA_ARGS__)
#    define DM_LOG_DEBUG(...) LOGGER.debug(__FILE__, __LINE__, __func__, __VA_ARGS__)
#    define DM_LOG_INFO(...) LOGGER.info(__FILE__, __LINE__, __func__, __VA_ARGS__)
#    define DM_LOG_WARN(...) LOGGER.warn(__FILE__, __LINE__, __func__, __VA_ARGS__)
#    define DM_LOG_ERROR(...) LOGGER.error(__FILE__, __LINE__, __func__, __VA_ARGS__)
#    define DM_LOG_CRITICAL(...) LOGGER.critical(__FILE__, __LINE__, __func__, __VA_ARGS__)
#  endif

#endif  // ROS2_APP