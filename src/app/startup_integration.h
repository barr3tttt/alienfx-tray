#pragma once

#include <filesystem>
#include <string>

namespace alienfx {

// "Apply at login" and "survive resume" are already solved on this machine, by two things
// that exist outside this repository:
//
//   ~/.config/systemd/user/alienfx.service    - a user unit, WantedBy graphical-session
//   /usr/lib/systemd/system-sleep/alienfx     - a root sleep hook, runs on resume
//
// Both of them invoke the same script, ~/.local/bin/alienfx-apply. So this application adds
// no startup mechanism of its own. It offers to make that one script delegate here, and
// leaves the unit and the hook untouched.
//
// (The sleep hook is at system level for a reason: the *user* systemd manager has no
// suspend.target, so a user unit wanting resume would silently never fire. Do not "fix"
// that with a user unit.)

enum class ShimState {
    Missing,          // ~/.local/bin/alienfx-apply does not exist
    Foreign,          // exists, but is not ours - the original CLI script, probably
    Managed,          // exists and delegates to alienfx-tray --apply
    Unreadable,       // exists but we cannot read it
};

struct IntegrationStatus {
    ShimState shim = ShimState::Missing;
    std::filesystem::path shimPath;
    std::filesystem::path backupPath;
    bool backupExists = false;

    [[nodiscard]] bool isManaged() const { return shim == ShimState::Managed; }

    // Anything present that is not ours has to be preserved before it is replaced. An
    // unreadable file counts twice over: its contents were never inspected, so losing it
    // would be the one unrecoverable outcome here.
    [[nodiscard]] bool needsBackup() const {
        return shim == ShimState::Foreign || shim == ShimState::Unreadable;
    }
    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] std::filesystem::path applyShimPath();
[[nodiscard]] IntegrationStatus checkStartupIntegration();

// Rewrites the apply script to delegate here, backing up whatever was there first.
//
// Never called automatically. This edits a file outside the application's own config
// directory, so it happens when the user presses the button and at no other time.
bool installStartupIntegration(std::string& error);

// Puts the backed-up script back.
bool removeStartupIntegration(std::string& error);

// Exposed for tests.
[[nodiscard]] std::string shimScriptContents();
[[nodiscard]] bool looksManaged(const std::string& contents);

}  // namespace alienfx
