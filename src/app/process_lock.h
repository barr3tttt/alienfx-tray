#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace alienfx {

// An advisory file lock serialising USB access between the running tray and the one-shot
// `--apply` invocations that login and resume fire off.
//
// Without it, a resume landing while the control panel is mid-drag means two processes
// racing to claim the same interface. With it, whichever gets there first finishes its
// batch and the other waits its turn.
//
// Held only while the devices are actually open, never for the lifetime of the process.
class ProcessLock {
   public:
    ProcessLock() = default;
    ~ProcessLock();

    ProcessLock(const ProcessLock&) = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;
    ProcessLock(ProcessLock&& other) noexcept;
    ProcessLock& operator=(ProcessLock&& other) noexcept;

    // Polls until the lock is taken or the timeout expires. Returns false on timeout.
    bool acquire(std::chrono::milliseconds timeout);
    void release();

    [[nodiscard]] bool held() const { return fd_ >= 0; }

    // Whether another holder may have had the lock between this object's last release and its
    // latest acquire. Every acquire bumps a counter kept in the lock file, so a count that
    // moved on while we were away means someone else - the resume hook, the login unit, a
    // manual --apply - claimed the devices and may have painted them. True on the first
    // acquire and whenever the counter could not be read or written, because then nothing is
    // known.
    [[nodiscard]] bool othersHeldSinceLastRelease() const { return othersHeld_; }

    // $XDG_RUNTIME_DIR/alienfx-tray.lock, falling back to a temp directory.
    [[nodiscard]] static std::filesystem::path lockPath();

   private:
    void recordAcquire();

    int fd_ = -1;
    std::optional<std::uint64_t> lastCount_;
    bool othersHeld_ = true;
};

}  // namespace alienfx
