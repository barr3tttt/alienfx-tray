#pragma once

#include <memory>
#include <optional>

#include "core/device_interface.h"
#include "core/scheme.h"

namespace alienfx {

struct ApplyResult {
    DomainStatus keyboard;
    DomainStatus external;

    [[nodiscard]] bool allOk() const { return keyboard.ok() && external.ok(); }
    [[nodiscard]] bool anyOk() const { return keyboard.ok() || external.ok(); }
    [[nodiscard]] const DomainStatus& forDomain(Domain domain) const {
        return domain == Domain::Keyboard ? keyboard : external;
    }
};

// Turns a Scheme into device commands. Knows nothing about Qt, threads or USB - it talks to
// two interfaces, and the whole of it is exercised in tests against fakes.
//
// Two behaviours are load-bearing:
//
//  * A failure on one domain never short-circuits the other. If the keyboard is wedged the
//    external lights still change, and the UI says which half is unhappy.
//
//  * Only what changed is sent. Measured on an Alienware m15 R7 AMD, one external zone costs
//    about 175 ms because the APIv4 power path walks control IDs 0x5b..0x60 per light, while
//    a keyboard colour costs about 20 ms. Re-sending all six zones on every frame of a colour
//    wheel drag would put the lights a full second behind the cursor; sending only the zone
//    being edited keeps it under 200 ms.
//
// The change tracking is discarded whenever the devices are released or a command fails,
// because at that point what the hardware is actually showing is no longer known.
class LightingEngine {
   public:
    LightingEngine(std::unique_ptr<IKeyboardDevice> keyboard,
                   std::unique_ptr<IExternalDevice> external);

    ApplyResult apply(const Scheme& scheme);

    // Release both USB claims. Called on shutdown and from the destructor.
    void releaseDevices();

    // Release one of them. The two domains want very different idle policies: claiming the
    // keyboard controller detaches usbhid from the interface the user types on, so it has to
    // go back quickly, while the external controller charges about 1.6 s for the first write
    // after every open and is worth keeping.
    void releaseDevice(Domain domain);

    [[nodiscard]] bool anyDeviceOpen() const;
    [[nodiscard]] bool isDeviceOpen(Domain domain) const;

    // What a domain's device says about itself right now. Worth re-reading after a release:
    // closing is where the kernel driver goes back, and that can fail on its own.
    [[nodiscard]] DomainStatus status(Domain domain) const;

   private:
    DomainStatus applyKeyboard(const KeyboardSettings& settings);
    DomainStatus applyExternal(const ExternalSettings& settings);

    std::unique_ptr<IKeyboardDevice> keyboard_;
    std::unique_ptr<IExternalDevice> external_;

    // What the hardware is believed to be showing, per domain. Empty means "unknown, send
    // everything".
    std::optional<KeyboardSettings> appliedKeyboard_;
    std::optional<ExternalSettings> appliedExternal_;
};

}  // namespace alienfx
