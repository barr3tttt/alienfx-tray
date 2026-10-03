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
    Busy,     // present but the interface could not be claimed, or refused to become ready

    // Not attempted yet, because another alienfx-tray process holds the device lock. Distinct
    // from Busy because only this one is coming back on its own: the worker has the frame and
    // a timer, and will paint it the moment the lock frees. Busy has nobody re-trying it.
    Waiting,
    Error,    // present, opened, but refused a command

    // Took a command and never answered. Distinct from Error on purpose: a refusal came
    // back and a hang did not, and the two send someone looking in opposite directions.
    // Only the apply watchdog produces this - no device reports it about itself, because a
    // device that could would not be wedged.
    Unresponsive,
};

// Every switch over DeviceState in this project is exhaustive with no default arm, so adding
// an enumerator above is a compile error at each site rather than a silent fallthrough. Keep
// it that way: the states drive what the user is told has gone wrong.

[[nodiscard]] std::string_view toString(DeviceState state);

struct DomainStatus {
    DeviceState state = DeviceState::Unknown;
    std::string detail;

    // Set when closing could not hand the USB interface back to its kernel driver.
    //
    // A field of its own rather than a detail string, because a detail on a Ready status is
    // ordinary information - "no zones configured" is a supported setup, not a fault - and
    // reading meaning into its mere presence tells the user something broke when nothing
    // did. Not a DeviceState either: the lighting worked, and only the release did not.
    bool interfaceLeftDetached = false;

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
// one .cpp file and never leaks into the rest of the build.
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

// One light's worth of an external write: the light ID plus the two colours the APIv4
// power path makes every light carry, AC and battery.
struct ExternalZoneWrite {
    std::uint8_t lightId;
    Rgb acColor;
    Rgb batteryColor;
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
    //
    // The whole write set arrives in one call because that is the only shape the hardware
    // honours: the six power blocks are shared staging slots, and a per-light write
    // rewrites all of them, so setting lights one at a time leaves only the last light
    // standing once the transaction commits. Batching is what makes every zone of a
    // multi-zone scheme take. See docs/hardware-notes.md.
    virtual bool applyZones(const std::vector<ExternalZoneWrite>& zones) = 0;

    virtual bool commit() = 0;

    [[nodiscard]] virtual DomainStatus status() const = 0;
};

}  // namespace alienfx
