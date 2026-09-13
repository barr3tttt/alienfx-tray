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

// How many preserved copies to try beside the canonical backup before giving up.
constexpr int kMaxPreservedCopies = 100;

// Whether the script about to be replaced still has to be preserved somewhere. A readable
// one that already has a backup does not: the first backup is the original, which is the
// thing worth keeping. An unreadable one always does, because nobody ever saw what was in
// it, so there is no way to judge it the lesser copy.
bool needsPreserving(const IntegrationStatus& status) {
    if (!status.needsBackup()) return false;
    return !status.backupExists || status.shim == ShimState::Unreadable;
}

// The canonical backup path while it is free, otherwise the first unused sibling. Empty
// when every candidate is taken, which has to fail the install rather than overwrite one.
std::filesystem::path preservePath(const IntegrationStatus& status) {
    if (!status.backupExists) return status.backupPath;

    std::error_code ec;
    for (int n = 1; n <= kMaxPreservedCopies; ++n) {
        std::filesystem::path candidate = status.backupPath.string() + "." + std::to_string(n);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
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
    return "An alienfx-apply script exists but could not be read. Enabling this will keep a "
           "copy of it before replacing it.";
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

    std::error_code ec;
    std::filesystem::create_directories(status.shimPath.parent_path(), ec);
    if (ec) {
        error = "could not create " + status.shimPath.parent_path().string() + ": " + ec.message();
        return false;
    }

    // Back up anything that is not already ours. The copy deliberately carries no
    // overwrite option, so "never destroy a backup" is enforced by the filesystem rather
    // than by the path choice above being right.
    if (needsPreserving(status)) {
        const std::filesystem::path destination = preservePath(status);
        if (destination.empty()) {
            error = "could not find a free path to preserve " + status.shimPath.string();
            return false;
        }

        std::filesystem::copy_file(status.shimPath, destination,
                                   std::filesystem::copy_options::none, ec);
        if (ec) {
            error = "could not back up " + status.shimPath.string() + ": " + ec.message();
            return false;
        }
        std::filesystem::permissions(destination,
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
