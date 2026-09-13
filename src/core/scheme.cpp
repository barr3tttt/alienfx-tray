#include "core/scheme.h"

#include <array>
#include <cstdio>

namespace alienfx {

std::string_view toString(Domain domain) {
    switch (domain) {
        case Domain::Keyboard:
            return "keyboard";
        case Domain::External:
            return "external";
    }
    return "unknown";
}

const ZoneProfile* findZoneProfile(std::uint8_t lightId) {
    for (const ZoneProfile& profile : kExternalZoneProfiles)
        if (profile.lightId == lightId) return &profile;
    return nullptr;
}

std::string defaultZoneName(std::uint8_t lightId) {
    if (const ZoneProfile* profile = findZoneProfile(lightId)) {
        std::string name = profile->name;
        if (profile->confidence == ZoneConfidence::Provisional) name += " (provisional)";
        return name;
    }

    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "ext-%02u", static_cast<unsigned>(lightId));
    return std::string(buf.data());
}

ExternalSettings defaultExternalSettings(Rgb color) {
    ExternalSettings settings;
    settings.zones.reserve(std::size(kDefaultExternalLightIds));
    for (std::uint8_t id : kDefaultExternalLightIds)
        settings.zones.push_back(ExternalZone{id, defaultZoneName(id), uniform(color)});
    return settings;
}

Scheme defaultScheme() {
    constexpr Rgb draculaPurple{189, 147, 249};
    Scheme scheme;
    scheme.name = "Dracula purple";
    scheme.keyboard.effect = kDefaultKeyboardEffect;
    scheme.keyboard.color = draculaPurple;
    scheme.external = defaultExternalSettings(draculaPurple);
    return scheme;
}

std::vector<Scheme> builtinSchemes() {
    std::vector<Scheme> schemes;
    schemes.push_back(defaultScheme());

    Scheme white;
    white.name = "Cool white";
    white.keyboard.color = Rgb{255, 255, 255};
    white.keyboard.brightness = Brightness::fromPercent(70);
    white.external = defaultExternalSettings(Rgb{255, 255, 255});
    schemes.push_back(white);

    // Amber on battery, teal on AC - the split the power-light path gives us for free.
    Scheme battery;
    battery.name = "Amber on battery";
    battery.keyboard.color = Rgb{0, 200, 190};
    battery.external = defaultExternalSettings(Rgb{0, 200, 190});
    for (ExternalZone& zone : battery.external.zones) {
        zone.color.battery = Rgb{255, 150, 0};
        zone.color.separateBatteryColor = true;
    }
    schemes.push_back(battery);

    return schemes;
}

Scheme resolveOutput(const LightingState& state) {
    if (state.lightsOn) return state.scheme;

    constexpr Rgb black{0, 0, 0};
    Scheme off = state.scheme;
    // Effect stays a real, non-zero effect. Brightness stays where it is. Only the colour
    // goes to black, which is the one way to darken this hardware that it can come back from.
    off.keyboard.color = black;
    for (ExternalZone& zone : off.external.zones) zone.color = uniform(black);
    return off;
}

}  // namespace alienfx
