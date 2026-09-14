#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace alienfx {

// Set ALIENFX_TRAY_TIMING=1 for a per-operation breakdown on stderr.
//
// This hardware is undocumented and its latencies are not guessable. Measured on an
// Alienware m15 R7 AMD: a keyboard colour set costs about 20 ms, while a single external
// power-light set costs about 175 ms, because the APIv4 power path walks control IDs
// 0x5b..0x60 for every light. That one measurement is why the engine sends only what
// changed - six zones at 175 ms each would put a colour wheel drag a second behind the
// cursor. Keeping the means to re-measure is worth twenty lines.
class PhaseTimer {
   public:
    explicit PhaseTimer(const char* label) : label_(label) {
        if (enabled()) start_ = std::chrono::steady_clock::now();
    }

    ~PhaseTimer() {
        if (!enabled()) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_);
        std::fprintf(stderr, "[alienfx timing] %-28s %8.2f ms\n", label_,
                     elapsed.count() / 1000.0);
    }

    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;

    static bool enabled() {
        static const bool on = std::getenv("ALIENFX_TRAY_TIMING") != nullptr;
        return on;
    }

   private:
    const char* label_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace alienfx
