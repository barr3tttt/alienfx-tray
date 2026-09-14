#include "app/apply_mode.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <thread>
#include <utility>

#include "app/process_lock.h"
#include "core/config.h"
#include "core/lighting_engine.h"
#include "device/device_factory.h"
#include "device/usb_context.h"

namespace alienfx {
namespace {

// The controllers are not necessarily enumerable the instant a user session starts or a
// machine comes back from suspend. The script this replaces polled for ten seconds; so do we.
constexpr auto kDeviceWaitTimeout = std::chrono::seconds(10);
constexpr auto kDeviceRetryInterval = std::chrono::milliseconds(500);

// Long enough to outlast a running tray's external-controller idle window, which is itself
// long because re-opening that controller costs about 1.6 s. Short enough that a resume hook
// does not sit around forever if something is genuinely stuck.
constexpr auto kLockTimeout = std::chrono::seconds(25);

int applyWithRetry(const ResolvedScheme& scheme, std::ostream& out, std::ostream& err) {
    silenceSdkLogging();

    UsbContext context;
    ProcessLock lock;
    if (!lock.acquire(kLockTimeout))
        err << "alienfx-tray: timed out waiting for another instance to finish\n";

    DeviceSet devices = makeSdkDevices(context);
    LightingEngine engine(std::move(devices.keyboard), std::move(devices.external));

    const auto deadline = std::chrono::steady_clock::now() + kDeviceWaitTimeout;
    ApplyResult result;
    for (int attempt = 1;; ++attempt) {
        result = engine.apply(scheme);
        if (result.allOk()) break;

        // Only keep trying for conditions that plausibly clear on their own. A controller
        // that has not been re-enumerated yet reports Missing, and that is the exact case
        // this poll exists for: on resume, usbhid has not necessarily finished. A controller
        // that actively refused a command is not retried - it will refuse again.
        const bool worthRetrying = std::ranges::any_of(kAllDomains, [&](Domain domain) {
            const DeviceState state = result.forDomain(domain).state;
            return state == DeviceState::Busy || state == DeviceState::Missing ||
                   state == DeviceState::Unknown;
        });
        if (!worthRetrying || std::chrono::steady_clock::now() >= deadline) break;

        err << "alienfx-tray: attempt " << attempt << " incomplete, retrying\n";
        std::this_thread::sleep_for(kDeviceRetryInterval);
    }

    // Hand the interfaces back before we report anything. For the keyboard controller this
    // is the user's actual typing device.
    engine.releaseDevices();

    // Handing them back can itself fail, and that is only known once the close has run. Only
    // the warning crosses over: a domain that never had to be opened - a scheme with no
    // zones, say - has nothing to report from the release and keeps the result it earned.
    for (Domain domain : kAllDomains) {
        DomainStatus& status = domain == Domain::Keyboard ? result.keyboard : result.external;
        status.interfaceLeftDetached = engine.interfaceLeftDetached(domain);
    }

    for (Domain domain : kAllDomains) {
        const DomainStatus& status = result.forDomain(domain);
        if (status.ok())
            out << describe(domain, status) << "\n";
        else
            err << describe(domain, status) << "\n";
    }

    // Partial success is still success: one dead controller must not stop the other from
    // lighting up, and a login unit that fails on a missing device is noise.
    return result.anyOk() ? 0 : 1;
}

}  // namespace

int runApplyMode(std::ostream& out, std::ostream& err) {
    LoadResult loaded = loadConfig(configPath());
    if (!loaded.error.empty())
        err << "alienfx-tray: " << loaded.error << " - using defaults\n";
    return applyWithRetry(resolveOutput(loaded.config.lightingState()), out, err);
}

int runOffMode(std::ostream& out, std::ostream& err) {
    LoadResult loaded = loadConfig(configPath());
    LightingState state = loaded.config.lightingState();
    state.lightsOn = false;
    return applyWithRetry(resolveOutput(state), out, err);
}

int runProbeExternalMode(std::optional<int> singleLightId, std::ostream& out,
                         std::ostream& err) {
    // Six unmistakable, well-separated colours for the IDs known to drive visible lights,
    // and black everywhere else so anything that does light up stands out.
    static constexpr struct {
        std::uint8_t id;
        Rgb color;
        const char* name;
    } kProbeColors[] = {
        {0, Rgb{255, 0, 0}, "red"},        {1, Rgb{0, 255, 0}, "green"},
        {2, Rgb{0, 0, 255}, "blue"},       {3, Rgb{255, 255, 0}, "yellow"},
        {4, Rgb{255, 0, 255}, "magenta"},  {5, Rgb{0, 255, 255}, "cyan"},
    };

    if (singleLightId && (*singleLightId < 0 || *singleLightId >= kExternalLightIdCount)) {
        err << "alienfx-tray: external light ID must be between 0 and "
            << (kExternalLightIdCount - 1) << "\n";
        return 2;
    }

    LoadResult loaded = loadConfig(configPath());
    if (!loaded.error.empty())
        err << "alienfx-tray: " << loaded.error << " - using defaults\n";

    // The keyboard keeps whatever the active scheme gives it. This mode is run to identify
    // the external lights, so it must not repaint the lighting around the ones being read.
    Scheme scheme = loaded.config.activeScheme();
    scheme.name = "external probe";

    scheme.external.zones.clear();
    for (std::uint8_t id = 0; id < kExternalLightIdCount; ++id) {
        Rgb color{};
        if (singleLightId) {
            if (id == static_cast<std::uint8_t>(*singleLightId)) color = Rgb{255, 255, 255};
        } else {
            for (const auto& entry : kProbeColors)
                if (entry.id == id) color = entry.color;
        }
        scheme.external.zones.push_back(ExternalZone{id, defaultZoneName(id), uniform(color)});
    }

    if (singleLightId) {
        out << "Lighting external light ID " << *singleLightId
            << " white. Every other external light is off.\n\n"
               "Look at the laptop and note exactly which fixture is lit - and whether it is\n"
               "the whole of that fixture or only part of it.\n\n";
    } else {
        out << "Lighting each external ID a distinct colour. Look at the laptop and note "
               "which\nphysical zone shows which colour:\n\n";
        for (const auto& entry : kProbeColors)
            out << "  ID " << static_cast<int>(entry.id) << "  " << entry.name << "\n";
        out << "  IDs 6-22  off\n\n"
               "If two zones report the same colour, or one zone reports two colours, rerun\n"
               "with a single ID to remove the ambiguity:  --probe-external=1\n\n";
    }

    out << "Nothing is written to device memory, so this is undone by a reboot, a resume or\n"
           "the next scheme you apply.\n\n";

    if (!loaded.config.lightsOn)
        out << "The lights are currently switched off, so nothing will light up. Turn them "
               "back\non and run this again.\n\n";

    return applyWithRetry(resolveOutput(LightingState{std::move(scheme), loaded.config.lightsOn}),
                          out, err);
}

}  // namespace alienfx
