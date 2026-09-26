#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/scheme.h"

namespace alienfx {

// Everything the app remembers between runs. One file, schema-versioned.
//
// Deliberately NOT stored: anything about the device's own memory. This application never
// issues the SDK's save-to-device commands. Login and resume already repaint the lights, so
// persistence in firmware buys nothing and costs the property that makes this hardware safe
// to experiment with at all - that a reboot always restores the factory default.
struct Config {
    static constexpr int kSchemaVersion = 2;

    // The version the data was written in. Anything below kSchemaVersion still needs
    // migrateConfig before it matches the current zone layout.
    int schemaVersion = kSchemaVersion;
    std::vector<Scheme> schemes;
    std::string activeSchemeName;
    bool lightsOn = true;
    // Built-in names the user deleted on purpose. Without a record of the intent the
    // re-seed below would undo every Delete on the next launch, so the button would work
    // for user schemes and silently not stick for the three shipped ones.
    std::vector<std::string> removedBuiltins;

    [[nodiscard]] const Scheme* findScheme(const std::string& name) const;
    [[nodiscard]] Scheme* findScheme(const std::string& name);

    // Always returns something paintable, even if the config names a scheme that no longer
    // exists: a stale name must not mean "no lighting".
    [[nodiscard]] Scheme activeScheme() const;
    [[nodiscard]] LightingState lightingState() const;

    // Adds or replaces by name. Returns true if an existing scheme was replaced.
    bool upsertScheme(const Scheme& scheme);
    bool removeScheme(const std::string& name);

    // Adds back any built-in scheme whose name is neither present nor deliberately removed,
    // leaving everything else alone. Editing a scheme in place rewrites it, so without this
    // a drag of the colour wheel would destroy a shipped preset with no way back short of
    // deleting the config file. Never overwrites: a user scheme that shares a built-in name
    // wins, and a name in removedBuiltins is not re-seeded at all.
    void restoreMissingBuiltins();

    [[nodiscard]] static Config defaults();
};

// Migrates a config written by an older schema to the current zone layout and stamps it with
// the current version, so it runs once per file. Returns true if the config was upgraded.
[[nodiscard]] bool migrateConfig(Config& config);

// Copy the source file to source with ".bak" appended. Returns true if the copy succeeded.
bool backupConfigFile(const std::filesystem::path& source);

// $XDG_CONFIG_HOME/alienfx-tray, falling back to ~/.config/alienfx-tray.
[[nodiscard]] std::filesystem::path configDir();
[[nodiscard]] std::filesystem::path configPath();

struct LoadResult {
    Config config;
    bool fileExisted = false;
    // Set when the file was present but unusable. The config is the default in that case -
    // a corrupt file must degrade to working lights, never to no lights.
    std::string error;
};

[[nodiscard]] LoadResult loadConfig(const std::filesystem::path& path);

// Writes via a temporary file and rename, so an interrupted save cannot leave an
// unparseable config behind.
bool saveConfig(const Config& config, const std::filesystem::path& path, std::string& error);

// Exposed for tests and for anyone who wants to diff a scheme.
[[nodiscard]] std::string serializeConfig(const Config& config);
[[nodiscard]] bool deserializeConfig(const std::string& text, Config& out, std::string& error);

}  // namespace alienfx
