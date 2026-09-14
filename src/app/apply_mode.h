#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include "core/config.h"
#include "core/scheme.h"

namespace alienfx {

// Headless entry points. None of these create a QApplication or touch a display: the resume
// hook runs as a bare shell with no session bus, no DISPLAY and no WAYLAND_DISPLAY.
//
// All of them return a process exit code.

// Applies the active scheme from the config file. This is what login and resume run.
int runApplyMode(std::ostream& out, std::ostream& err);

// Paints everything black without touching the saved state. For scripts and panic buttons.
int runOffMode(std::ostream& out, std::ostream& err);

// Identification probe for the external controller.
//
// With no argument: lights IDs 0-5 six distinct colours at once. Fast, but a report like
// "the ring is green and yellow" is then ambiguous between two IDs.
//
// With an ID: lights that one white and everything else black. Slower to work through, but
// it produces an answer that cannot be misread - which is how a provisional zone becomes a
// confirmed one.
int runProbeExternalMode(std::optional<int> singleLightId, std::ostream& out,
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
