#include "core/config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace alienfx {
namespace {

using nlohmann::json;

// The parse is deliberately non-throwing, and these readers are what keep it that way.
// nlohmann's own value() throws on a type mismatch, so a hand-edited "lightsOn": "yes"
// would escape all the way out of loadConfig - past the one invariant this file has, that a
// bad config degrades to working lights rather than to no lights.
bool readBool(const json& object, const char* key, bool fallback) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

int readInt(const json& object, const char* key, int fallback) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

std::string readString(const json& object, const char* key, std::string fallback) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

json readMember(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() ? *it : json{};
}

bool isBuiltinSchemeName(const std::string& name) {
    for (const Scheme& builtin : builtinSchemes())
        if (builtin.name == name) return true;
    return false;
}

json encodeColor(Rgb color) { return toHex(color); }

Rgb decodeColor(const json& value, Rgb fallback) {
    if (!value.is_string()) return fallback;
    Rgb parsed;
    return parseHex(value.get<std::string>(), parsed) ? parsed : fallback;
}

json encodeKeyboard(const KeyboardSettings& keyboard) {
    return json{{"effect", std::string(toString(keyboard.effect))},
                {"color", encodeColor(keyboard.color)},
                {"brightness", keyboard.brightness.percent()}};
}

KeyboardSettings decodeKeyboard(const json& value) {
    KeyboardSettings keyboard;
    if (!value.is_object()) return keyboard;

    if (value.contains("effect") && value["effect"].is_string())
        keyboard.effect = parseEffect(value["effect"].get<std::string>());
    keyboard.color = decodeColor(readMember(value, "color"), keyboard.color);
    if (value.contains("brightness") && value["brightness"].is_number_integer())
        keyboard.brightness = Brightness::fromPercent(value["brightness"].get<int>());
    return keyboard;
}

json encodeZone(const ExternalZone& zone) {
    json out{{"id", zone.lightId},
             {"name", zone.name},
             {"color", encodeColor(zone.color.ac)}};
    if (zone.color.separateBatteryColor) out["batteryColor"] = encodeColor(zone.color.battery);
    return out;
}

bool decodeZone(const json& value, ExternalZone& out) {
    if (!value.is_object()) return false;
    if (!value.contains("id") || !value["id"].is_number_integer()) return false;

    const int id = value["id"].get<int>();
    if (id < 0 || id >= kExternalLightIdCount) return false;

    out.lightId = static_cast<std::uint8_t>(id);
    out.name = readString(value, "name", defaultZoneName(out.lightId));
    out.color.ac = decodeColor(readMember(value, "color"), Rgb{});
    if (value.contains("batteryColor")) {
        out.color.battery = decodeColor(value["batteryColor"], out.color.ac);
        out.color.separateBatteryColor = true;
    } else {
        out.color.battery = out.color.ac;
        out.color.separateBatteryColor = false;
    }
    return true;
}

json encodeExternal(const ExternalSettings& external) {
    json zones = json::array();
    for (const ExternalZone& zone : external.zones) zones.push_back(encodeZone(zone));
    return json{{"brightness", external.brightness.percent()}, {"zones", std::move(zones)}};
}

ExternalSettings decodeExternal(const json& value) {
    ExternalSettings external;
    if (!value.is_object()) return defaultExternalSettings(Rgb{189, 147, 249});

    if (value.contains("brightness") && value["brightness"].is_number_integer())
        external.brightness = Brightness::fromPercent(value["brightness"].get<int>());

    // An absent "zones" key means "you decide" and gets the built-in zone list. An explicit
    // empty array means the user wants no external lighting, and is honoured as written.
    // Either way only the zones are defaulted - a brightness the file did carry stands.
    if (!value.contains("zones") || !value["zones"].is_array()) {
        external.zones = defaultExternalSettings(Rgb{189, 147, 249}).zones;
        return external;
    }

    // A light ID is a zone's identity, so a repeat is dropped and the first one kept. Done
    // on every load rather than as a migration: hand-edits happen at any schema, and a
    // duplicate zone paints twice for no benefit.
    std::set<std::uint8_t> seen;
    for (const json& entry : value["zones"]) {
        ExternalZone zone;
        if (decodeZone(entry, zone) && seen.insert(zone.lightId).second)
            external.zones.push_back(zone);
    }
    return external;
}

json encodeScheme(const Scheme& scheme) {
    return json{{"name", scheme.name},
                {"keyboard", encodeKeyboard(scheme.keyboard)},
                {"external", encodeExternal(scheme.external)}};
}

bool decodeScheme(const json& value, Scheme& out) {
    if (!value.is_object()) return false;
    if (!value.contains("name") || !value["name"].is_string()) return false;
    out.name = value["name"].get<std::string>();
    if (out.name.empty()) return false;
    out.keyboard = decodeKeyboard(readMember(value, "keyboard"));
    out.external = decodeExternal(readMember(value, "external"));
    return true;
}

}  // namespace

const Scheme* Config::findScheme(const std::string& name) const {
    for (const Scheme& scheme : schemes)
        if (scheme.name == name) return &scheme;
    return nullptr;
}

Scheme* Config::findScheme(const std::string& name) {
    return const_cast<Scheme*>(std::as_const(*this).findScheme(name));
}

Scheme Config::activeScheme() const {
    if (const Scheme* scheme = findScheme(activeSchemeName)) return *scheme;
    if (!schemes.empty()) return schemes.front();
    return defaultScheme();
}

LightingState Config::lightingState() const { return LightingState{activeScheme(), lightsOn}; }

bool Config::upsertScheme(const Scheme& scheme) {
    // Saving under a name that was deleted brings it back as a live scheme, so the old
    // deletion must stop suppressing the re-seed and the name becomes usable again.
    std::erase(removedBuiltins, scheme.name);

    if (Scheme* existing = findScheme(scheme.name)) {
        *existing = scheme;
        return true;
    }
    schemes.push_back(scheme);
    return false;
}

bool Config::removeScheme(const std::string& name) {
    for (auto it = schemes.begin(); it != schemes.end(); ++it) {
        if (it->name != name) continue;
        schemes.erase(it);
        if (activeSchemeName == name) activeSchemeName = schemes.empty() ? "" : schemes.front().name;
        // A deliberate delete has to outlive the restart, or the re-seed puts the shipped
        // preset straight back and Delete only appears to work.
        const bool alreadyRecorded =
            std::ranges::find(removedBuiltins, name) != removedBuiltins.end();
        if (isBuiltinSchemeName(name) && !alreadyRecorded) removedBuiltins.push_back(name);
        return true;
    }
    return false;
}

void Config::restoreMissingBuiltins() {
    for (const Scheme& builtin : builtinSchemes()) {
        if (findScheme(builtin.name)) continue;
        if (std::ranges::find(removedBuiltins, builtin.name) != removedBuiltins.end()) continue;
        schemes.push_back(builtin);
    }
}

Config Config::defaults() {
    Config config;
    config.schemes = builtinSchemes();
    config.activeSchemeName = config.schemes.front().name;
    config.lightsOn = true;
    return config;
}

namespace {

bool isOldDefaultName(const std::string& name) {
    constexpr const char* olds[] = {
        "Unidentified (ID 0)", "Rear ring (provisional)",
        "Unidentified (ID 4)", "Unidentified (ID 5)",
        "Rear ring (zone 0)", "Rear ring (zone 1)",
        "No visible light (zone 4)", "No visible light (zone 5)",
        "Rear ring",
    };
    return std::ranges::find(olds, name) != std::end(olds);
}

}  // namespace

bool migrateConfig(Config& config) {
    if (config.schemaVersion >= Config::kSchemaVersion) return false;

    for (Scheme& scheme : config.schemes) {
        auto& zones = scheme.external.zones;

        // v1 -> v2: remove zones 4 and 5, which light nothing visible.
        if (config.schemaVersion < 2) {
            std::erase_if(zones, [](const ExternalZone& z) {
                return z.lightId == 4 || z.lightId == 5;
            });
        }

        // v2 -> v3: the rear ring is two independent segments, IDs 0 and 1. A config that
        // carries only one of them gets the missing segment as a copy of the one it has.
        if (config.schemaVersion < 3) {
            const auto findZone = [&](std::uint8_t id) {
                return std::ranges::find_if(zones,
                    [id](const ExternalZone& z) { return z.lightId == id; });
            };

            const auto zone0 = findZone(0);
            const auto zone1 = findZone(1);
            if (zone0 != zones.end() && zone1 == zones.end()) {
                ExternalZone copy = *zone0;
                copy.lightId = 1;
                copy.name = defaultZoneName(1);
                zones.insert(zone0 + 1, copy);
            } else if (zone1 != zones.end() && zone0 == zones.end()) {
                ExternalZone copy = *zone1;
                copy.lightId = 0;
                copy.name = defaultZoneName(0);
                zones.insert(zone1, copy);
            }
        }

        // Rename zones whose names match an old default to the current default.
        for (ExternalZone& zone : zones) {
            if (isOldDefaultName(zone.name)) zone.name = defaultZoneName(zone.lightId);
        }

    }

    // v3 -> v4: the old editing model wrote every manual edit straight into the selected
    // scheme, so any built-in whose colours were ever touched no longer matches what it
    // shipped as. There was no explicit overwrite-save in that model - every change to a
    // built-in's contents came through the implicit path - so the shipped contents are
    // restored wholesale. Schemes with other names are the user's own and are left alone,
    // and the pre-migration file is kept as .bak before the rewrite, as with every bump.
    if (config.schemaVersion < 4) {
        for (const Scheme& builtin : builtinSchemes()) {
            if (Scheme* existing = config.findScheme(builtin.name)) *existing = builtin;
        }
    }

    config.schemaVersion = Config::kSchemaVersion;
    return true;
}

namespace {

bool writeFileContents(const std::filesystem::path& target, const std::string& contents) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << contents;
    out.flush();
    return static_cast<bool>(out);
}

std::string utcTimestamp(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(buf.data());
}

}  // namespace

bool backupConfigFile(const std::filesystem::path& source, const std::string& contents) {
    return writeFileContents(source.string() + ".bak", contents);
}

std::optional<std::filesystem::path> preserveCorruptConfig(
    const std::filesystem::path& source, const std::string& contents,
    std::chrono::system_clock::time_point when) {
    // Every login and resume loads the file, so the same broken text is kept once rather
    // than once per load.
    const std::string prefix = source.filename().string() + ".corrupt-";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(source.parent_path(), ec)) {
        if (!entry.path().filename().string().starts_with(prefix)) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string existing((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        if (existing == contents) return entry.path();
    }

    const std::filesystem::path target = source.string() + ".corrupt-" + utcTimestamp(when);
    if (!writeFileContents(target, contents)) return std::nullopt;
    return target;
}

std::filesystem::path configDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "alienfx-tray";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "alienfx-tray";
    return std::filesystem::path(".config") / "alienfx-tray";
}

std::filesystem::path configPath() { return configDir() / "config.json"; }

std::string serializeConfig(const Config& config) {
    json schemes = json::array();
    for (const Scheme& scheme : config.schemes) schemes.push_back(encodeScheme(scheme));

    json removed = json::array();
    for (const std::string& name : config.removedBuiltins) removed.push_back(name);

    json root{{"schemaVersion", Config::kSchemaVersion},
              {"activeScheme", config.activeSchemeName},
              {"lightsOn", config.lightsOn},
              {"removedBuiltins", std::move(removed)},
              {"schemes", std::move(schemes)}};
    return root.dump(2) + "\n";
}

bool deserializeConfig(const std::string& text, Config& out, std::string& error) {
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "config is not valid JSON";
        return false;
    }

    const int version = readInt(root, "schemaVersion", 0);
    if (version > Config::kSchemaVersion) {
        error = "config was written by a newer version of alienfx-tray";
        return false;
    }

    Config config;
    config.schemaVersion = version;
    if (root.contains("schemes") && root["schemes"].is_array()) {
        for (const json& entry : root["schemes"]) {
            Scheme scheme;
            if (decodeScheme(entry, scheme)) config.schemes.push_back(scheme);
        }
    }
    if (config.schemes.empty()) {
        error = "config contains no usable schemes";
        return false;
    }

    if (root.contains("removedBuiltins") && root["removedBuiltins"].is_array()) {
        for (const json& entry : root["removedBuiltins"])
            if (entry.is_string()) config.removedBuiltins.push_back(entry.get<std::string>());
    }

    config.activeSchemeName = readString(root, "activeScheme", config.schemes.front().name);
    if (!config.findScheme(config.activeSchemeName))
        config.activeSchemeName = config.schemes.front().name;
    config.lightsOn = readBool(root, "lightsOn", true);
    out = std::move(config);
    return true;
}

LoadResult loadConfig(const std::filesystem::path& path) {
    LoadResult result;
    result.config = Config::defaults();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return result;
    result.fileExisted = true;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "could not open " + path.string();
        return result;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    Config parsed;
    std::string error;
    if (!deserializeConfig(text, parsed, error)) {
        // The first save after this fallback replaces the file. Keep the unreadable
        // original beside it first: a file a human just broke is exactly the one they will
        // want back. Not as the .bak, which may already hold the pre-migration original.
        if (const auto kept = preserveCorruptConfig(path, text)) {
            result.error = error + "; the unreadable file was kept as " + kept->string();
        } else {
            result.error = error + "; keeping a copy of the unreadable file failed, and the "
                                   "next save will replace it";
        }
        return result;
    }
    result.config = std::move(parsed);

    const bool migrated = migrateConfig(result.config);

    // Built-ins are re-seeded rather than merged, so editing one in place can never destroy
    // it for good. Deleting one is a different intent and is recorded, so it stays deleted.
    result.config.restoreMissingBuiltins();

    // Rewriting the file stamps the new version, so the next load skips the migration and
    // leaves the backup of the original untouched.
    if (migrated && backupConfigFile(path, text)) {
        std::string saveError;
        saveConfig(result.config, path, saveError);
    }

    return result;
}

bool saveConfig(const Config& config, const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "could not create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }

    // Temp file in the same directory so the rename is atomic on the same filesystem.
    const std::filesystem::path temp = path.string() + "." + std::to_string(::getpid()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not write " + temp.string();
            return false;
        }
        out << serializeConfig(config);
        out.flush();
        if (!out) {
            error = "write failed for " + temp.string();
            // A failed write must not strand the temp file either; only the rename gets to
            // leave anything behind.
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        error = "could not replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace alienfx
