#pragma once

#include <string>

namespace utils {

std::string ReadEnvValue(const std::string &envFile, const std::string &key);

}  // namespace utils
