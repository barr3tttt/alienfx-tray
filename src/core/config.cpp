#include "core/config.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

namespace alienfx {
namespace {

using nlohmann::json;

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
    keyboard.color = decodeColor(value.value("color", json{}), keyboard.color);
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
    out.name = value.value("name", defaultZoneName(out.lightId));
    out.color.ac = decodeColor(value.value("color", json{}), Rgb{});
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
    if (!value.contains("zones") || !value["zones"].is_array())
        return defaultExternalSettings(Rgb{189, 147, 249});

    for (const json& entry : value["zones"]) {
        ExternalZone zone;
        if (decodeZone(entry, zone)) external.zones.push_back(zone);
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
    out.keyboard = decodeKeyboard(value.value("keyboard", json{}));
    out.external = decodeExternal(value.value("external", json{}));
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
        return true;
    }
    return false;
}

Config Config::defaults() {
    Config config;
    config.schemes = builtinSchemes();
    config.activeSchemeName = config.schemes.front().name;
    config.lightsOn = true;
    return config;
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

    json root{{"schemaVersion", Config::kSchemaVersion},
              {"activeScheme", config.activeSchemeName},
              {"lightsOn", config.lightsOn},
              {"schemes", std::move(schemes)}};
    return root.dump(2) + "\n";
}

bool deserializeConfig(const std::string& text, Config& out, std::string& error) {
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "config is not valid JSON";
        return false;
    }

    const int version = root.value("schemaVersion", 0);
    if (version > Config::kSchemaVersion) {
        error = "config was written by a newer version of alienfx-tray";
        return false;
    }

    Config config;
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

    config.activeSchemeName = root.value("activeScheme", config.schemes.front().name);
    config.lightsOn = root.value("lightsOn", true);
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
        result.error = error;
        return result;
    }
    result.config = std::move(parsed);
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
    const std::filesystem::path temp = path.string() + ".tmp";
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
