#pragma once

#include <iosfwd>
#include <optional>
#include <string>

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

}  // namespace alienfx
