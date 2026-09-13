#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alienfx {

// What the argument scan that runs before any QApplication exists decided to do.
//
// The scan has to come first because the resume hook runs `--apply` from a root sleep
// script with no display and no session bus. It is a pure function so the classification
// can be exercised without a display, and so the options Qt claims for itself stay a
// listed set rather than "anything we did not recognise" - a mistyped `--aply` has to be
// reported, while an X11 session manager's `-session <id>` has to start the tray.
enum class CliMode {
    Tray,     // nothing headless was asked for; QApplication takes the arguments
    Help,
    Version,
    Apply,
    Off,
    ProbeExternal,
    Error,    // unusable arguments; `message` says which one and why
};

struct CliRequest {
    CliMode mode = CliMode::Tray;
    std::optional<int> probeLightId;
    std::string message;

    friend bool operator==(const CliRequest&, const CliRequest&) = default;
};

[[nodiscard]] CliRequest parseCommandLine(const std::vector<std::string_view>& arguments);

}  // namespace alienfx
