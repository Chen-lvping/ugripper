#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>

namespace DA {
namespace utils {

class FileLogger {
 public:

  static void Init(bool enable_file, const std::string& file_path);

  // level_char: "I"/"W"/"E"...（你现在宏传的是 const char*）
  static void Log(const char* level_char,
                  const std::string& msg,
                  const char* file,
                  int line,
                  const char* func);

  static bool Enabled();

 private:
  static void OpenFileIfNeededLocked();
  static std::string NowUnixSecUsecLocked();
  static void EnsureParentDirExistsLocked(const std::string& path);

 private:
  static std::atomic<bool> enabled_;
  static std::string file_path_;
  static std::ofstream ofs_;
  static std::mutex mtx_;
};

}  // namespace utils
}  // namespace DA
