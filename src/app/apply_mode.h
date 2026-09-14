#pragma once

#include <chrono>
#include <iosfwd>
#include <optional>
#include <string>

#include "core/config.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"

namespace alienfx {

// Headless entry points. None of these create a QApplication or touch a display: the resume
// hook runs as a bare shell with no session bus, no DISPLAY and no WAYLAND_DISPLAY.
//
// The exit codes are a contract: the login unit and the resume hook both read them.
//
//   0  the lighting was applied. For --apply only, this also covers standing down because
//      another alienfx-tray process holds the device lock: that process is already painting
//      the active scheme, which is exactly what --apply asked for, so the login unit must
//      not be marked failed for losing a race it did not need to win.
//   1  nothing was applied. That includes both controllers being undrivable and --off or
//      --probe-external standing down for the lock: neither of those has anyone else doing
//      its job for it, so a lock it could not take means the command did not happen.
//   2  the arguments were wrong.

// How long a headless run waits for the device lock before standing down. A parameter rather
// than a constant only so a test can reach the stand-down without a 25 second wait.
inline constexpr std::chrono::seconds kLockWaitTimeout{25};

// Applies the active scheme from the config file. This is what login and resume run.
int runApplyMode(std::ostream& out, std::ostream& err,
                 std::chrono::milliseconds lockTimeout = kLockWaitTimeout);

// Paints everything black without touching the saved state. For scripts and panic buttons.
int runOffMode(std::ostream& out, std::ostream& err,
               std::chrono::milliseconds lockTimeout = kLockWaitTimeout);

// Identification probe for the external controller.
//
// With no argument: lights IDs 0-5 six distinct colours at once. Fast, but a report like
// "the ring is green and yellow" is then ambiguous between two IDs.
//
// With an ID: lights that one white and everything else black. Slower to work through, but
// it produces an answer that cannot be misread - which is how a provisional zone becomes a
// confirmed one.
int runProbeExternalMode(std::optional<int> singleLightId, std::ostream& out,
                         std::ostream& err,
                         std::chrono::milliseconds lockTimeout = kLockWaitTimeout);

// Drives each domain to a settled state, retrying only the ones that might still come good.
//
// Split out of the headless modes so the rule that matters most here is assertable without
// hardware: a domain is handed back the moment it is finished with, not at the end of the
// poll. The keyboard controller IS the interface the user types on, and this poll exists to
// wait for a controller that has not re-enumerated yet - which on the resume path means
// waiting at the unlock screen. Holding the keyboard claimed through that wait would leave
// the machine unable to type for the whole of it.
[[nodiscard]] ApplyResult applyDomainsWithRetry(LightingEngine& engine,
                                                const ResolvedScheme& scheme,
                                                std::chrono::milliseconds pollTimeout,
                                                std::chrono::milliseconds retryInterval,
                                                std::ostream& err);

// Exactly what a probe run paints, with no device, no config file and no output involved.
//
// Split out so the one thing this mode must get right is stated in one place and can be
// asserted without hardware. Everything the probe paints on the external controller is its
// own; only the keyboard half is inherited, so the mode does not repaint the lighting
// surrounding the lights the user is being asked to look at.
[[nodiscard]] ResolvedScheme buildProbeOutput(const Config& config,
                                              std::optional<int> singleLightId);

}  // namespace alienfx
