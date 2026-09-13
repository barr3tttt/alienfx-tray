#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace alienfx {

// A plain 24-bit colour. The SDK's own Afx_colorcode stores its channels in b,g,r order,
// which is the single most common source of "why is my red blue" on this hardware. Core
// code never touches that struct; the conversion happens once, in the SDK transport, using
// named fields rather than positional initialisation.
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    constexpr Rgb() = default;
    constexpr Rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
        : r(red), g(green), b(blue) {}

    [[nodiscard]] constexpr bool isBlack() const { return r == 0 && g == 0 && b == 0; }

    friend constexpr bool operator==(const Rgb&, const Rgb&) = default;
};

[[nodiscard]] std::string toHex(Rgb c);

// Parses "#rrggbb" or "rrggbb". Returns std::nullopt-equivalent via the bool out-param
// style used across the config layer: an unparseable value falls back to a caller default
// rather than throwing, because a hand-edited config should never stop the lights working.
[[nodiscard]] bool parseHex(const std::string& text, Rgb& out);

// Hardware brightness.
//
// This is a type, not an int, for the same reason KeyboardEffect is a type: the APIv5
// brightness path bottoms out in the same family of "turn the controller off" commands
// that leave the keyboard in a state a later colour set cannot recover. The minimum
// representable value is 1. "Off" in this application always means a steady effect painted
// black - never a disable, never zero brightness.
class Brightness {
   public:
    static constexpr std::uint8_t kMin = 1;
    static constexpr std::uint8_t kMax = 255;

    constexpr Brightness() = default;
    constexpr explicit Brightness(int value)
        : value_(static_cast<std::uint8_t>(std::clamp(value, static_cast<int>(kMin),
                                                      static_cast<int>(kMax)))) {}

    [[nodiscard]] constexpr std::uint8_t value() const { return value_; }

    // 1..100, the unit the UI slider works in.
    [[nodiscard]] constexpr int percent() const { return (value_ * 100 + kMax / 2) / kMax; }
    [[nodiscard]] static constexpr Brightness fromPercent(int percent) {
        return Brightness((std::clamp(percent, 1, 100) * kMax + 50) / 100);
    }

    friend constexpr bool operator==(const Brightness&, const Brightness&) = default;

   private:
    std::uint8_t value_ = kMax;
};

}  // namespace alienfx
