#pragma once

#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace DA::utils {

class LogStringBuilder {
public:
    template <typename T>
    LogStringBuilder& operator<<(T&& value) {
        stream_ << std::forward<T>(value);
        return *this;
    }

    LogStringBuilder& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (manip != static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
            manip(stream_);
        }
        return *this;
    }

    LogStringBuilder& operator<<(std::ios_base& (*manip)(std::ios_base&)) {
        manip(stream_);
        return *this;
    }

    std::string str() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

inline LogStringBuilder LogString() { return {}; }

}  // namespace DA::utils
