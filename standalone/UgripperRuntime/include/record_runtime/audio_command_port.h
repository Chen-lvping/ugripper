#pragma once

#include <memory>
#include <string>

namespace ugripper::runtime {

class AudioCommandPort
{
public:
    virtual ~AudioCommandPort() = default;

    virtual void ResetReadyState() = 0;
    virtual bool HasCommandChannel() const = 0;
    virtual bool IsBackendReady() const = 0;
    virtual bool SendCommand(const std::string& command, std::string* error_message) const = 0;
};

std::unique_ptr<AudioCommandPort> CreateFileAudioCommandPort(std::string command_path,
                                                             std::string ready_path);

}  // namespace ugripper::runtime
