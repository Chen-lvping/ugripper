#include "utils/env_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace utils {
namespace {

std::string TrimWhitespace(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    {
        value.erase(value.begin());
    }

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.pop_back();
    }

    return value;
}

}  // namespace

std::string ReadEnvValue(const std::string &envFile, const std::string &key)
{
    std::ifstream input(envFile);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.rfind(key + "=", 0) != 0)
        {
            continue;
        }

        std::string value = line.substr(key.size() + 1);
        value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
        return TrimWhitespace(std::move(value));
    }

    return {};
}

}  // namespace utils
