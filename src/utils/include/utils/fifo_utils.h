#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace utils {

bool EnsureFifo(const std::string& path, mode_t mode, std::string* error_message = nullptr);

bool WriteFifoLine(const std::string& path,
                   const std::string& line,
                   bool non_blocking = true,
                   std::string* error_message = nullptr);

class BufferedFifoLineReader
{
public:
    explicit BufferedFifoLineReader(size_t max_buffer_bytes = 64 * 1024);
    ~BufferedFifoLineReader();

    BufferedFifoLineReader(const BufferedFifoLineReader&) = delete;
    BufferedFifoLineReader& operator=(const BufferedFifoLineReader&) = delete;

    bool Open(const std::string& path, mode_t mode, std::string* error_message = nullptr);
    void Close();
    bool IsOpen() const;
    int fd() const;

    bool ReadAvailable(std::vector<std::string>* lines, std::string* error_message = nullptr);
    void ClearBuffer();

private:
    int fd_ = -1;
    std::string path_;
    std::string pending_;
    size_t max_buffer_bytes_ = 0;
};

}  // namespace utils
