#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace utils {

inline bool FileExistsAndNotEmpty(const std::string &path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) &&
           std::filesystem::is_regular_file(path, error) &&
           std::filesystem::file_size(path, error) > 0;
}

}  // namespace utils
