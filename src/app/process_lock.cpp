#include "app/process_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>
#include <thread>
#include <utility>

namespace alienfx {

ProcessLock::~ProcessLock() { release(); }

ProcessLock::ProcessLock(ProcessLock&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

ProcessLock& ProcessLock::operator=(ProcessLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

std::filesystem::path ProcessLock::lockPath() {
    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir)
        return std::filesystem::path(runtimeDir) / "alienfx-tray.lock";
    return std::filesystem::temp_directory_path() / "alienfx-tray.lock";
}

bool ProcessLock::acquire(std::chrono::milliseconds timeout) {
    if (fd_ >= 0) return true;

    const std::string path = lockPath().string();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        // No lock file means no serialisation, but refusing to light the keyboard because a
        // lock file could not be created would be a worse outcome than the race it prevents.
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            fd_ = fd;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::close(fd);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void ProcessLock::release() {
    if (fd_ < 0) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

}  // namespace alienfx
