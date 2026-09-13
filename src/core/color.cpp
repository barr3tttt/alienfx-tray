#include "core/color.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <string_view>

namespace alienfx {

std::string toHex(Rgb c) {
    std::array<char, 8> buf{};
    std::snprintf(buf.data(), buf.size(), "#%02x%02x%02x", c.r, c.g, c.b);
    return std::string(buf.data());
}

bool parseHex(const std::string& text, Rgb& out) {
    std::string_view sv(text);
    if (!sv.empty() && sv.front() == '#') sv.remove_prefix(1);
    if (sv.size() != 6) return false;

    std::uint8_t channels[3]{};
    for (int i = 0; i < 3; ++i) {
        const char* first = sv.data() + i * 2;
        unsigned value = 0;
        auto [ptr, ec] = std::from_chars(first, first + 2, value, 16);
        if (ec != std::errc{} || ptr != first + 2) return false;
        channels[i] = static_cast<std::uint8_t>(value);
    }
    out = Rgb{channels[0], channels[1], channels[2]};
    return true;
}

}  // namespace alienfx
