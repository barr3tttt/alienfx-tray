#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/color.h"
#include "core/effect.h"

namespace alienfx {

// The two controllers on this machine are independent in every way that matters - different
// USB device, different API version, different command path, different failure mode. They
// are modelled as separate domains all the way down so one can be missing or wedged without
// taking the other with it.
enum class Domain : std::uint8_t { Keyboard, External };

inline constexpr std::array kAllDomains{Domain::Keyboard, Domain::External};

[[nodiscard]] std::string_view toString(Domain domain);

// External lights are power-type lights: each one carries two colours, one for AC and one
// for battery. That is not a feature we invented, it is how the APIv4 power path works, so
// exposing it costs nothing.
struct ColorPair {
    Rgb ac;
    Rgb battery;
    bool separateBatteryColor = false;

    [[nodiscard]] Rgb effectiveBattery() const { return separateBatteryColor ? battery : ac; }

    friend bool operator==(const ColorPair&, const ColorPair&) = default;
};

[[nodiscard]] inline ColorPair uniform(Rgb c) { return ColorPair{c, c, false}; }

// The keyboard deliberately carries a single colour, not a ColorPair. APIv5's global-effect
// command does take two colour slots, but the second one is a per-effect parameter, not an
// AC/battery pair - only the APIv4 power lights have that split in hardware. Shipping a
// battery colour here would mean claiming a behaviour nobody has seen this device perform.
struct KeyboardSettings {
    // Never an integer. See core/effect.h.
    KeyboardEffect effect = kDefaultKeyboardEffect;
    Rgb color{189, 147, 249};
    Brightness brightness{Brightness::kMax};

    friend bool operator==(const KeyboardSettings&, const KeyboardSettings&) = default;
};

struct ExternalZone {
    std::uint8_t lightId = 0;
    // Placeholder names until the physical zones are identified. The IDs are what the
    // hardware cares about; the name is only ever a label in the UI.
    std::string name;
    ColorPair color = uniform(Rgb{189, 147, 249});

    friend bool operator==(const ExternalZone&, const ExternalZone&) = default;
};

struct ExternalSettings {
    std::vector<ExternalZone> zones;
    Brightness brightness{Brightness::kMax};

    friend bool operator==(const ExternalSettings&, const ExternalSettings&) = default;
};

struct Scheme {
    std::string name;
    KeyboardSettings keyboard;
    ExternalSettings external;

    friend bool operator==(const Scheme&, const Scheme&) = default;
};

// The AW-ELC maps 23 light IDs. Only 0-5 have ever been observed driving a visible light;
// 6-22 accept commands and do nothing. We ship the six so the UI is not a wall of dead
// sliders, and keep the full range reachable through the config file for anyone probing a
// different chassis.
inline constexpr std::uint8_t kExternalLightIdCount = 23;
inline constexpr std::uint8_t kDefaultExternalLightIds[] = {0, 1, 2, 3, 4, 5};

// How sure we are that a light ID drives the zone we have named it after. These IDs carry
// no self-description - the only way to learn what one does is to light it and look, so this
// is recorded observation, not a lookup table from a datasheet.
enum class ZoneConfidence : std::uint8_t {
    Confirmed,     // observed directly, one colour, unambiguously reported
    Provisional,   // observed, but the report was ambiguous
    Unidentified,  // lit during a probe and nothing was reported as changing
};

struct ZoneProfile {
    std::uint8_t lightId;
    const char* name;
    ZoneConfidence confidence;
};

// Established by lighting each ID on an Alienware m15 R7 AMD and asking the owner what
// changed. The observations behind each entry are in docs/hardware-notes.md.
inline constexpr ZoneProfile kExternalZoneProfiles[] = {
    {0, "Rear ring (zone 0)", ZoneConfidence::Confirmed},
    {1, "Rear ring (zone 1)", ZoneConfidence::Confirmed},
    {2, "Interior logo", ZoneConfidence::Confirmed},
    {3, "Lid logo", ZoneConfidence::Confirmed},
    {4, "No visible light (zone 4)", ZoneConfidence::Confirmed},
    {5, "No visible light (zone 5)", ZoneConfidence::Confirmed},
};

[[nodiscard]] const ZoneProfile* findZoneProfile(std::uint8_t lightId);

// The name shown in the UI. Provisional zones say so, so nobody mistakes an open question
// for a settled fact.
[[nodiscard]] std::string defaultZoneName(std::uint8_t lightId);

[[nodiscard]] ExternalSettings defaultExternalSettings(Rgb color);

// Dracula purple on both domains, which is where this machine started.
[[nodiscard]] Scheme defaultScheme();

[[nodiscard]] std::vector<Scheme> builtinSchemes();

// Everything the app is currently asking the hardware to show.
struct LightingState {
    Scheme scheme;
    bool lightsOn = true;

    friend bool operator==(const LightingState&, const LightingState&) = default;
};

// The scheme as it should actually be painted, and the only thing the engine will accept.
//
// It exists as a type rather than as a convention because forgetting to resolve is silent
// and expensive: the lights come back on after the user switched them off, and nothing about
// the call site looks wrong. Only resolveOutput can make one, so a caller that hands the
// engine a raw Scheme fails to compile instead - the same standard as KeyboardEffect, whose
// range cannot express the byte that leaves the keyboard dark.
class ResolvedScheme {
   public:
    [[nodiscard]] const Scheme& scheme() const { return scheme_; }

   private:
    friend ResolvedScheme resolveOutput(const LightingState& state);
    explicit ResolvedScheme(Scheme scheme) : scheme_(std::move(scheme)) {}

    Scheme scheme_;
};

// "Off" is a steady effect painted black - never effType 0, never zero brightness, because
// both of those are how the keyboard gets stuck dark.
[[nodiscard]] ResolvedScheme resolveOutput(const LightingState& state);

}  // namespace alienfx
