#include "file_logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace DA {
namespace utils {

std::atomic<bool> FileLogger::enabled_{false};
std::string FileLogger::file_path_;
std::ofstream FileLogger::ofs_;
std::mutex FileLogger::mtx_;

void FileLogger::Init(bool enable_file, const std::string& file_path) {
  enabled_.store(enable_file, std::memory_order_release);

  std::lock_guard<std::mutex> lk(mtx_);

  // 关闭场景
  if (!enable_file) {
    if (ofs_.is_open()) ofs_.close();
    file_path_.clear();
    return;
  }

  // 保存路径
  file_path_ = file_path;

  // 创建父目录
  try {
    std::filesystem::path p(file_path_);
    auto parent = p.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
  } catch (const std::exception& e) {
    std::cerr << "[FileLogger] create_directories failed: " << e.what()
              << " path=" << file_path_ << std::endl;
  }

  // 打开文件
  OpenFileIfNeededLocked();

  // 如果打开失败，给个提示（否则你又会遇到“啥也没有”）
  if (!ofs_.is_open()) {
    std::cerr << "[FileLogger] open failed: " << file_path_ << std::endl;
  }
}

bool FileLogger::Enabled() {
  return enabled_.load(std::memory_order_acquire);
}

void FileLogger::Log(const char* level_char,
                     const std::string& msg,
                     const char* file,
                     int line,
                     const char* func) {
  if (!Enabled()) return;

  std::lock_guard<std::mutex> lk(mtx_);

  if (!ofs_.is_open()) {
    OpenFileIfNeededLocked();
    if (!ofs_.is_open()) return;
  }

  ofs_ << "[" << NowUnixSecUsecLocked() << "]"
       << "[" << (level_char ? level_char : "?") << "] "
       << file << ":" << line << " " << func << " - "
       << msg << "\n";

  ofs_.flush();
}

void FileLogger::OpenFileIfNeededLocked() {
  if (file_path_.empty()) return;
  if (ofs_.is_open()) return;

  try {
    EnsureParentDirExistsLocked(file_path_);
    ofs_.open(file_path_, std::ios::out | std::ios::app);
  } catch (...) {
    // 保持静默也行，但不建议
  }
}

void FileLogger::EnsureParentDirExistsLocked(const std::string& path) {
  try {
    std::filesystem::path p(path);
    auto parent = p.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
  } catch (...) {
  }
}

std::string FileLogger::NowUnixSecUsecLocked() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto sec = time_point_cast<seconds>(now);
  auto usec = duration_cast<microseconds>(now - sec).count();
  auto epoch_sec = sec.time_since_epoch().count();

  std::ostringstream oss;
  oss << epoch_sec << "." << std::setw(6) << std::setfill('0') << usec;
  return oss.str();
}

}  // namespace utils
}  // namespace DA
