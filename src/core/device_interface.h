#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/color.h"
#include "core/effect.h"
#include "core/scheme.h"

namespace alienfx {

enum class DeviceState : std::uint8_t {
    Unknown,  // not looked for yet
    Ready,    // present and last operation succeeded
    Missing,  // not plugged in / not enumerable
    Busy,     // present but another process holds the interface
    Error,    // present, opened, but refused a command
};

[[nodiscard]] std::string_view toString(DeviceState state);

struct DomainStatus {
    DeviceState state = DeviceState::Unknown;
    std::string detail;

    [[nodiscard]] bool ok() const { return state == DeviceState::Ready; }

    friend bool operator==(const DomainStatus&, const DomainStatus&) = default;
};

// Human-readable, already phrased for a UI banner.
[[nodiscard]] std::string describe(Domain domain, const DomainStatus& status);

// ---------------------------------------------------------------------------------------
// Device interfaces.
//
// Two of them, not one, because the two controllers genuinely do not share a command
// vocabulary: the keyboard takes a single global effect, the external controller takes a
// per-light power block. Folding them into one interface would mean half the methods
// throwing "not supported" on each implementation.
//
// These also exist so core logic can be tested without a laptop attached, and so
// AlienFX_SDK.h - which does `using namespace std;` at file scope - is included in exactly
// two .cpp files and never leaks into the rest of the build.
// ---------------------------------------------------------------------------------------

class IKeyboardDevice {
   public:
    virtual ~IKeyboardDevice() = default;

    // Claiming this device detaches usbhid from the interface the user types on - it is the
    // built-in keyboard, and there is no separate LED interface. Callers must hold it for
    // the shortest time that gets the job done.
    virtual bool open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    virtual bool setBrightness(Brightness brightness) = 0;

    // Note the parameter type. There is no std::uint8_t overload, and there never will be:
    // effType 0 is the command that leaves this device unrecoverably dark. See core/effect.h.
    virtual bool applyEffect(KeyboardEffect effect, Rgb color) = 0;

    [[nodiscard]] virtual DomainStatus status() const = 0;
};

class IExternalDevice {
   public:
    virtual ~IExternalDevice() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    // APIv4 brightness is also what turns the listed lights on, so the ID list matters.
    virtual bool setBrightness(Brightness brightness,
                               const std::vector<std::uint8_t>& lightIds) = 0;

    // These are power-type lights: each carries an AC colour and a battery colour. Plain
    // colour sets across all 23 IDs do nothing at all on this controller.
    virtual bool applyZone(std::uint8_t lightId, Rgb acColor, Rgb batteryColor) = 0;

    virtual bool commit() = 0;

    [[nodiscard]] virtual DomainStatus status() const = 0;
};

}  // namespace alienfx
