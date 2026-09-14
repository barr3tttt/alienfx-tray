#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace alienfx {

// ---------------------------------------------------------------------------------------
// THE GUARD.
//
// AlienFX_SDK.cpp, Functions::SetGlobalEffects, case API_V5:
//
//     if (!effType) PrepareAndSend(COMMV5_setEffect, {{2, {1, 0xfe}}});
//
// effType 0 is not "no effect". It is a clear/disable that leaves the keyboard dark in a
// state no later colour set can override - `setall` included. The external controller
// carries on responding, so the machine looks half-broken. Recovery is a non-zero effType
// or a reboot, and "reboot to fix the lights" is the exact failure this project exists to
// delete.
//
// So zero is not guarded against at runtime. It is made unrepresentable:
//
//   1. KeyboardEffect is a scoped enum with no zero enumerator.
//   2. kAllKeyboardEffects is the complete enumerator table, static_asserted non-zero.
//   3. toWireValue() is a table LOOKUP, not a cast. Its range is therefore exactly the
//      table, which contains no zero - even for a value someone casts in from outside.
//   4. The device interface takes KeyboardEffect. There is no std::uint8_t overload
//      anywhere, so "pass 0" is not a sentence a call site can say.
//   5. Config parsing maps unrecognised text onto a listed enumerator, so a hand-edited
//      scheme file cannot inject one either.
//
// tests/test_effect_guard.cpp walks all 256 underlying byte values through toWireValue and
// fails if any of them comes out zero. Removing any part of the above breaks that test.
// ---------------------------------------------------------------------------------------

enum class KeyboardEffect : std::uint8_t {
    // Confirmed working on an Alienware m15 R7 AMD (Darfon 0d62:cabc, APIv5).
    Steady = 1,
};

inline constexpr std::array kAllKeyboardEffects{
    KeyboardEffect::Steady,
};

inline constexpr KeyboardEffect kDefaultKeyboardEffect = KeyboardEffect::Steady;

namespace detail {
constexpr bool noEffectIsZero() {
    for (KeyboardEffect e : kAllKeyboardEffects)
        if (static_cast<std::uint8_t>(e) == 0) return false;
    return true;
}
constexpr bool defaultIsListed() {
    for (KeyboardEffect e : kAllKeyboardEffects)
        if (e == kDefaultKeyboardEffect) return true;
    return false;
}
}  // namespace detail

static_assert(detail::noEffectIsZero(),
              "A KeyboardEffect enumerator is zero. effType 0 sends the APIv5 clear/disable "
              "command and leaves the keyboard dark in an unrecoverable state. Do not add it.");
static_assert(detail::defaultIsListed(),
              "kDefaultKeyboardEffect must appear in kAllKeyboardEffects - it is the fallback "
              "toWireValue() returns for unlisted input.");
static_assert(!kAllKeyboardEffects.empty(), "There must be at least one usable effect.");

// The ONLY place a KeyboardEffect becomes a wire byte. A lookup rather than a cast, so the
// function's range is the enumerator table and nothing else.
[[nodiscard]] constexpr std::uint8_t toWireValue(KeyboardEffect effect) {
    for (KeyboardEffect known : kAllKeyboardEffects)
        if (known == effect) return static_cast<std::uint8_t>(known);
    return static_cast<std::uint8_t>(kDefaultKeyboardEffect);
}

[[nodiscard]] constexpr std::string_view toString(KeyboardEffect effect) {
    switch (effect) {
        case KeyboardEffect::Steady:
            return "steady";
    }
    return toString(kDefaultKeyboardEffect);
}

// Unrecognised input resolves to the default rather than failing: a scheme file someone
// hand-edited should still light the keyboard.
[[nodiscard]] constexpr KeyboardEffect parseEffect(std::string_view name) {
    for (KeyboardEffect known : kAllKeyboardEffects)
        if (toString(known) == name) return known;
    return kDefaultKeyboardEffect;
}

}  // namespace alienfx
