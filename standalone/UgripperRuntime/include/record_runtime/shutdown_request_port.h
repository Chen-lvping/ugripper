#pragma once

#include <memory>
#include <string>

namespace ugripper::runtime {

class ShutdownRequestPort
{
public:
    virtual ~ShutdownRequestPort() = default;

    virtual bool RequestAction(const std::string& action, std::string* error_message) const = 0;
    virtual bool WaitForActionResult(std::string* result,
                                     int timeout_ms,
                                     std::string* error_message) const = 0;

    bool RequestShutdown(std::string* error_message) const
    {
        return RequestAction("shutdown", error_message);
    }
};

std::unique_ptr<ShutdownRequestPort> CreateFileShutdownRequestPort(std::string request_path,
                                                                   std::string result_path);

}  // namespace ugripper::runtime
