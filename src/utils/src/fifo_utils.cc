#include "utils/fifo_utils.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace utils {
namespace {

void SetError(std::string* error_message, const std::string& message)
{
    if (error_message != nullptr)
    {
        *error_message = message;
    }
}

std::string ErrnoMessage(const std::string& prefix, int saved_errno)
{
    return prefix + ": " + std::strerror(saved_errno);
}

}  // namespace

bool EnsureFifo(const std::string& path, mode_t mode, std::string* error_message)
{
    struct stat st {};
    if (stat(path.c_str(), &st) == 0)
    {
        if (!S_ISFIFO(st.st_mode))
        {
            SetError(error_message, "path exists but is not a FIFO: " + path);
            return false;
        }
        chmod(path.c_str(), mode);
        SetError(error_message, "");
        return true;
    }

    if (errno != ENOENT)
    {
        SetError(error_message, ErrnoMessage("stat FIFO failed for " + path, errno));
        return false;
    }

    if (mkfifo(path.c_str(), mode) != 0 && errno != EEXIST)
    {
        SetError(error_message, ErrnoMessage("failed to create FIFO " + path, errno));
        return false;
    }
    chmod(path.c_str(), mode);
    SetError(error_message, "");
    return true;
}

bool WriteFifoLine(const std::string& path,
                   const std::string& line,
                   bool non_blocking,
                   std::string* error_message)
{
    int flags = O_WRONLY | O_CLOEXEC;
    if (non_blocking)
    {
        flags |= O_NONBLOCK;
    }

    const int fd = open(path.c_str(), flags);
    if (fd < 0)
    {
        SetError(error_message, ErrnoMessage("open FIFO failed for " + path, errno));
        return false;
    }

    std::string payload = line;
    if (payload.empty() || payload.back() != '\n')
    {
        payload.push_back('\n');
    }

    const char* data = payload.data();
    size_t remaining = payload.size();
    while (remaining > 0)
    {
        const ssize_t written = write(fd, data, remaining);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            const int saved_errno = errno;
            close(fd);
            SetError(error_message, ErrnoMessage("write FIFO failed for " + path, saved_errno));
            return false;
        }
        if (written == 0)
        {
            close(fd);
            SetError(error_message, "short write to FIFO " + path);
            return false;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }

    close(fd);
    SetError(error_message, "");
    return true;
}

BufferedFifoLineReader::BufferedFifoLineReader(size_t max_buffer_bytes)
    : max_buffer_bytes_(max_buffer_bytes)
{
}

BufferedFifoLineReader::~BufferedFifoLineReader()
{
    Close();
}

bool BufferedFifoLineReader::Open(const std::string& path, mode_t mode, std::string* error_message)
{
    Close();
    if (!EnsureFifo(path, mode, error_message))
    {
        return false;
    }

    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        SetError(error_message, ErrnoMessage("open FIFO failed for " + path, errno));
        return false;
    }

    fd_ = fd;
    path_ = path;
    pending_.clear();
    SetError(error_message, "");
    return true;
}

void BufferedFifoLineReader::Close()
{
    if (fd_ >= 0)
    {
        close(fd_);
        fd_ = -1;
    }
    path_.clear();
    pending_.clear();
}

bool BufferedFifoLineReader::IsOpen() const
{
    return fd_ >= 0;
}

int BufferedFifoLineReader::fd() const
{
    return fd_;
}

bool BufferedFifoLineReader::ReadAvailable(std::vector<std::string>* lines, std::string* error_message)
{
    if (lines == nullptr)
    {
        SetError(error_message, "line output vector is null");
        return false;
    }
    lines->clear();
    if (fd_ < 0)
    {
        SetError(error_message, "FIFO reader is not open");
        return false;
    }

    char buffer[1024];
    while (true)
    {
        const ssize_t count = read(fd_, buffer, sizeof(buffer));
        if (count > 0)
        {
            pending_.append(buffer, static_cast<size_t>(count));
            if (pending_.size() > max_buffer_bytes_)
            {
                pending_.clear();
                SetError(error_message, "FIFO line buffer overflow for " + path_);
                return false;
            }
            continue;
        }
        if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        SetError(error_message, ErrnoMessage("read FIFO failed for " + path_, errno));
        return false;
    }

    size_t newline = std::string::npos;
    while ((newline = pending_.find('\n')) != std::string::npos)
    {
        std::string line = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines->push_back(std::move(line));
    }

    SetError(error_message, "");
    return true;
}

void BufferedFifoLineReader::ClearBuffer()
{
    pending_.clear();
}

}  // namespace utils
