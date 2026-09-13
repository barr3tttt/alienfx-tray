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
    static constexpr int kSchemaVersion = 1;

    std::vector<Scheme> schemes;
    std::string activeSchemeName;
    bool lightsOn = true;

    [[nodiscard]] const Scheme* findScheme(const std::string& name) const;
    [[nodiscard]] Scheme* findScheme(const std::string& name);

    // Always returns something paintable, even if the config names a scheme that no longer
    // exists: a stale name must not mean "no lighting".
    [[nodiscard]] Scheme activeScheme() const;
    [[nodiscard]] LightingState lightingState() const;

    // Adds or replaces by name. Returns true if an existing scheme was replaced.
    bool upsertScheme(const Scheme& scheme);
    bool removeScheme(const std::string& name);

    [[nodiscard]] static Config defaults();
};

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
