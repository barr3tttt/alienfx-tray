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
    [[nodiscard]] DomainStatus& forDomain(Domain domain) {
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
//  * Only what changed is sent. Measured 2026-09-27 on an Alienware m15 R7 AMD, one
//    external zone costs about 110-140 ms because the APIv4 power path walks control IDs
//    0x5b..0x60 per light, while a keyboard colour costs about 20 ms. Re-sending all four
//    zones on every frame of a colour wheel drag would put the lights half a second behind
//    the cursor; sending only the zone being edited keeps it under 150 ms.
//
// The change tracking is discarded whenever what the hardware is showing is no longer known:
// a command failed, a controller turned up closed without being released, or the caller
// says someone else may have painted it. Every known external zone the next scheme does not
// carry is then assumed lit and driven off. Releasing the external handle on purpose is not
// one of those - closing it changes nothing the lights show - so its tracking survives.
class LightingEngine {
   public:
    LightingEngine(std::unique_ptr<IKeyboardDevice> keyboard,
                   std::unique_ptr<IExternalDevice> external);
    // Releases both USB claims. The explicit releaseDevices() calls at shutdown stay the
    // primary path; this is the backstop that makes the property structural rather than
    // conventional, so a future caller cannot leak a claimed interface by forgetting one.
    ~LightingEngine();

    ApplyResult apply(const ResolvedScheme& resolved);

    // Apply one domain on its own. The headless retry poll needs this: when it is waiting for
    // a controller that has not re-enumerated yet, the one that already succeeded must not be
    // re-sent, and above all must not stay claimed, for the rest of the wait.
    DomainStatus applyDomain(Domain domain, const ResolvedScheme& resolved);

    // Release both USB claims. Called on shutdown and from the destructor.
    void releaseDevices();

    // Release one of them. The two domains want very different idle policies: claiming the
    // keyboard controller detaches usbhid from the interface the user types on, so it has to
    // go back quickly, while the external controller charges a real enumeration round for
    // every open and is worth keeping.
    //
    // The keyboard starts from scratch after a release, because on APIv5 the brightness
    // command is what wakes it. The external controller keeps showing what it was sent, so
    // an apply after its release sends only what changed, and nothing at all - not even an
    // open - when nothing did.
    void releaseDevice(Domain domain);

    // Discard everything believed about both controllers, so the next apply sends the lot and
    // drives off every known external zone the scheme does not carry. For callers that know
    // something else may have painted the hardware while it was released.
    void forgetAppliedState();

    [[nodiscard]] bool anyDeviceOpen() const;
    [[nodiscard]] bool isDeviceOpen(Domain domain) const;

    // Whether the last close of this domain left its USB interface without a kernel driver.
    //
    // Deliberately a bool and not the device's whole DomainStatus. Releasing says nothing
    // about what the lighting did, and handing callers a status invited them to adopt it
    // wholesale - which demoted a domain that succeeded without ever needing to be opened,
    // because a device that was never opened reports Unknown. A bool cannot demote anything.
    [[nodiscard]] bool interfaceLeftDetached(Domain domain) const;

   private:
    DomainStatus applyKeyboard(const KeyboardSettings& settings);
    DomainStatus applyExternal(const ExternalSettings& settings);

    std::unique_ptr<IKeyboardDevice> keyboard_;
    std::unique_ptr<IExternalDevice> external_;

    // What the hardware is believed to be showing, per domain. Empty means "unknown, send
    // everything".
    std::optional<KeyboardSettings> appliedKeyboard_;
    std::optional<ExternalSettings> appliedExternal_;
    // Set by a deliberate release, so the reopen that follows can tell it apart from finding
    // the controller closed under it.
    bool externalReleased_ = false;
};

}  // namespace alienfx
