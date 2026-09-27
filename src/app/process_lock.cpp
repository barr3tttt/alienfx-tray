#include "app/process_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <QString>
#include <QtGlobal>

#include <cstdlib>
#include <thread>
#include <utility>

namespace alienfx {

ProcessLock::~ProcessLock() { release(); }

ProcessLock::ProcessLock(ProcessLock&& other) noexcept
    : fd_(other.fd_),
      lastCount_(other.lastCount_),
      othersHeld_(other.othersHeld_),
      unserialised_(other.unserialised_) {
    other.fd_ = -1;
}

ProcessLock& ProcessLock::operator=(ProcessLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        lastCount_ = other.lastCount_;
        othersHeld_ = other.othersHeld_;
        unserialised_ = other.unserialised_;
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
        othersHeld_ = !unserialised_;
        if (!unserialised_)
            qWarning("alienfx-tray: cannot open the device lock %s; applying without it",
                     qPrintable(QString::fromStdString(path)));
        unserialised_ = true;
        lastCount_.reset();
        return true;
    }
    unserialised_ = false;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            fd_ = fd;
            recordAcquire();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::close(fd);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void ProcessLock::recordAcquire() {
    std::uint64_t count = 0;
    const ssize_t got = ::pread(fd_, &count, sizeof count, 0);
    const bool readable = got == 0 || got == static_cast<ssize_t>(sizeof count);
    if (!readable) count = 0;

    othersHeld_ = !readable || !lastCount_ || *lastCount_ != count;

    const std::uint64_t next = count + 1;
    if (::pwrite(fd_, &next, sizeof next, 0) == static_cast<ssize_t>(sizeof next))
        lastCount_ = next;
    else
        lastCount_.reset();
}

void ProcessLock::release() {
    if (fd_ < 0) return;
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

}  // namespace alienfx
