#include "app/startup_integration.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace alienfx {
namespace {

constexpr const char* kManagedMarker = "# managed-by: alienfx-tray";

std::filesystem::path homeDir() {
    if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home);
    return std::filesystem::current_path();
}

std::string readFile(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    ok = true;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

std::filesystem::path applyShimPath() { return homeDir() / ".local" / "bin" / "alienfx-apply"; }

std::string shimScriptContents() {
    std::ostringstream out;
    out << "#!/usr/bin/env bash\n"
        << kManagedMarker << "\n"
        << "#\n"
        << "# Applies the active alienfx-tray scheme. Invoked by the two things that were\n"
        << "# already set up to call this path:\n"
        << "#\n"
        << "#   ~/.config/systemd/user/alienfx.service   - at login\n"
        << "#   /usr/lib/systemd/system-sleep/alienfx    - on resume\n"
        << "#\n"
        << "# Neither of those is modified by alienfx-tray. Whatever this file contained\n"
        << "# before is saved beside it as alienfx-apply.pre-tray.bak and is used as a\n"
        << "# fallback if the binary ever goes missing.\n"
        << "\n"
        << "set -u\n"
        << "\n"
        << "TRAY_BIN=\"${ALIENFX_TRAY_BIN:-$HOME/.local/bin/alienfx-tray}\"\n"
        << "\n"
        << "if [[ -x \"$TRAY_BIN\" ]]; then\n"
        << "  exec \"$TRAY_BIN\" --apply\n"
        << "fi\n"
        << "\n"
        << "PREVIOUS=\"$(dirname \"$0\")/alienfx-apply.pre-tray.bak\"\n"
        << "if [[ -x \"$PREVIOUS\" ]]; then\n"
        << "  exec \"$PREVIOUS\"\n"
        << "fi\n"
        << "\n"
        << "echo \"alienfx-tray not found at $TRAY_BIN\" >&2\n"
        << "exit 1\n";
    return out.str();
}

bool looksManaged(const std::string& contents) {
    return contents.find(kManagedMarker) != std::string::npos;
}

std::string IntegrationStatus::summary() const {
    switch (shim) {
        case ShimState::Managed:
            return "Applied at login and after resume, through the existing alienfx.service "
                   "and system-sleep hook.";
        case ShimState::Foreign:
            return "An alienfx-apply script exists but does not use alienfx-tray yet. "
                   "Enabling this will back it up and replace it.";
        case ShimState::Missing:
            return "No alienfx-apply script found. Enabling this will create one.";
        case ShimState::Unreadable:
            break;
    }
    return "An alienfx-apply script exists but could not be read, so it cannot be backed up. "
           "Make it readable or move it aside, then enable this.";
}

IntegrationStatus checkStartupIntegration() {
    IntegrationStatus status;
    status.shimPath = applyShimPath();
    status.backupPath = status.shimPath.string() + ".pre-tray.bak";

    std::error_code ec;
    status.backupExists = std::filesystem::exists(status.backupPath, ec);

    if (!std::filesystem::exists(status.shimPath, ec)) {
        status.shim = ShimState::Missing;
        return status;
    }

    bool ok = false;
    const std::string contents = readFile(status.shimPath, ok);
    if (!ok) {
        status.shim = ShimState::Unreadable;
        return status;
    }
    status.shim = looksManaged(contents) ? ShimState::Managed : ShimState::Foreign;
    return status;
}

bool installStartupIntegration(std::string& error) {
    const IntegrationStatus status = checkStartupIntegration();

    // Refuse rather than replace a file nobody could read. Copying it would fail for the
    // same reason the read did, so there is no way to keep it, and losing a script whose
    // contents were never inspected is the one unrecoverable outcome here.
    if (status.shim == ShimState::Unreadable) {
        error = "could not read " + status.shimPath.string() +
                ", so it cannot be backed up before being replaced. Make it readable "
                "(chmod u+r) or move it aside, then try again.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(status.shimPath.parent_path(), ec);
    if (ec) {
        error = "could not create " + status.shimPath.parent_path().string() + ": " + ec.message();
        return false;
    }

    // Back up anything that is not already ours, and never overwrite an existing backup:
    // the first thing that was there is the thing worth keeping. The copy carries no
    // overwrite option, so that rule is enforced by the filesystem rather than by the
    // guard above being right.
    if (status.shim == ShimState::Foreign && !status.backupExists) {
        std::filesystem::copy_file(status.shimPath, status.backupPath,
                                   std::filesystem::copy_options::none, ec);
        if (ec) {
            error = "could not back up " + status.shimPath.string() + ": " + ec.message();
            return false;
        }
        std::filesystem::permissions(status.backupPath,
                                     std::filesystem::perms::owner_all |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::others_read,
                                     ec);
    }

    const std::filesystem::path temp = status.shimPath.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not write " + temp.string();
            return false;
        }
        out << shimScriptContents();
        out.flush();
        if (!out) {
            error = "write failed for " + temp.string();
            return false;
        }
    }

    std::filesystem::permissions(temp,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_read |
                                     std::filesystem::perms::others_exec,
                                 ec);
    if (ec) {
        // Without the executable bit the shim lands but never runs, and the failure only
        // shows up at the next login. That is worth a checked error, not a rename anyway.
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        error = "could not make " + temp.string() + " executable: " + ec.message();
        return false;
    }
    std::filesystem::rename(temp, status.shimPath, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        error = "could not replace " + status.shimPath.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool removeStartupIntegration(std::string& error) {
    const IntegrationStatus status = checkStartupIntegration();
    if (!status.isManaged()) {
        error = "alienfx-tray does not manage " + status.shimPath.string();
        return false;
    }

    std::error_code ec;
    if (status.backupExists) {
        std::filesystem::rename(status.backupPath, status.shimPath, ec);
        if (ec) {
            error = "could not restore " + status.backupPath.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    std::filesystem::remove(status.shimPath, ec);
    if (ec) {
        error = "could not remove " + status.shimPath.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace alienfx
