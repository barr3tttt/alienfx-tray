#include "app/apply_mode.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string_view>
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

// See app/apply_mode.h - these are read by the login unit and the resume hook.
constexpr int kExitApplied = 0;
constexpr int kExitNothingApplied = 1;
constexpr int kExitBadArguments = 2;

// What a mode wants to happen when another alienfx-tray process holds the device lock.
//
// Required at every call site and deliberately without a default, because the honest answer
// differs per mode and a mode that inherited apply's answer by accident is exactly how this
// path went wrong: --off was told it had succeeded while the lights were still on.
//
// Note the asymmetry and do not collapse it back together. REFUSING to write because
// somebody else holds the lock is always the safe outcome, and for --apply it is also a
// success - the holder is already painting the active scheme, which is what was asked for.
// PROCEEDING without the lock is the opposite: two writers on a controller where a bad write
// costs the owner a working keyboard until reboot, and it must never happen quietly. Same
// lock, opposite answers.
struct LockStandDown {
    int exitCode;
    // Goes to stdout when standing down is a success and to stderr when it is not, so a mode
    // that did nothing cannot read like one that did its job.
    std::string_view explanation;
};

constexpr LockStandDown kApplyStandDown{
    kExitApplied,
    "alienfx-tray: another alienfx-tray process holds the controllers and is applying the "
    "lighting; standing down rather than writing without the device lock\n"};

constexpr LockStandDown kOffStandDown{
    kExitNothingApplied,
    "alienfx-tray: another alienfx-tray process holds the controllers; standing down rather "
    "than writing without the device lock, so the lights were NOT turned off\n"};

constexpr LockStandDown kProbeStandDown{
    kExitNothingApplied,
    "alienfx-tray: another alienfx-tray process holds the controllers; standing down rather "
    "than writing without the device lock, so nothing was painted and no zone was probed\n"};

struct RunOutcome {
    int exitCode = kExitNothingApplied;
    // False when the lock was held elsewhere and no device was touched at all.
    bool applied = false;
    ApplyResult result{};
};

// Four unmistakable, well-separated colours for the hardware IDs known to drive visible
// lights. IDs 0 and 1 both drive the rear ring; each gets its own colour so the probe makes
// the double-drive visible. IDs 4 and 5 are not probed - they light nothing.
constexpr struct {
    std::uint8_t id;
    Rgb color;
    const char* name;
} kProbeColors[] = {
    {0, Rgb{255, 0, 0}, "red"},        {1, Rgb{0, 255, 0}, "green"},
    {2, Rgb{0, 0, 255}, "blue"},       {3, Rgb{255, 255, 0}, "yellow"},
};

// A domain is worth another attempt only while its failure is one that plausibly clears on
// its own. A switch rather than a comparison chain so a new state has to be classified here
// rather than silently falling into "give up".
bool mightStillComeGood(DeviceState state) {
    switch (state) {
        case DeviceState::Unknown:
        case DeviceState::Busy:
        case DeviceState::Missing:
            return true;
        case DeviceState::Ready:
        case DeviceState::Error:
            return false;
        case DeviceState::Waiting:
        case DeviceState::Unresponsive:
            // Neither is reachable here: both are produced by the tray, and this poll runs
            // only in the headless modes, which hold the device lock for their whole run.
            return false;
    }
    std::unreachable();
}

Rgb probeColor(std::uint8_t id, std::optional<int> singleLightId) {
    if (singleLightId)
        return id == static_cast<std::uint8_t>(*singleLightId) ? Rgb{255, 255, 255} : Rgb{};
    for (const auto& entry : kProbeColors)
        if (entry.id == id) return entry.color;
    return Rgb{};
}

RunOutcome applyWithRetry(const ResolvedScheme& scheme, const LockStandDown& standDown,
                          std::chrono::milliseconds lockTimeout, std::ostream& out,
                          std::ostream& err) {
    silenceSdkLogging();

    UsbContext context;
    ProcessLock lock;
    if (!lock.acquire(lockTimeout)) {
        (standDown.exitCode == kExitApplied ? out : err) << standDown.explanation;
        return RunOutcome{standDown.exitCode, false, {}};
    }

    DeviceSet devices = makeSdkDevices(context);
    LightingEngine engine(std::move(devices.keyboard), std::move(devices.external));

    ApplyResult result =
        applyDomainsWithRetry(engine, scheme, kDeviceWaitTimeout, kDeviceRetryInterval, err);

    // Hand the interfaces back before we report anything. For the keyboard controller this
    // is the user's actual typing device.
    engine.releaseDevices();

    // Handing them back can itself fail, and that is only known once the close has run. Only
    // the warning crosses over: a domain that never had to be opened - a scheme with no
    // zones, say - has nothing to report from the release and keeps the result it earned.
    for (Domain domain : kAllDomains)
        result.forDomain(domain).interfaceLeftDetached = engine.interfaceLeftDetached(domain);

    for (Domain domain : kAllDomains) {
        const DomainStatus& status = result.forDomain(domain);
        if (status.ok())
            out << describe(domain, status) << "\n";
        else
            err << describe(domain, status) << "\n";
    }

    // Partial success is still success: one dead controller must not stop the other from
    // lighting up, and a login unit that fails on a missing device is noise.
    return RunOutcome{result.anyOk() ? kExitApplied : kExitNothingApplied, true, result};
}

}  // namespace

int runApplyMode(std::ostream& out, std::ostream& err,
                 std::chrono::milliseconds lockTimeout) {
    LoadResult loaded = loadConfig(configPath());
    if (!loaded.error.empty())
        err << "alienfx-tray: " << loaded.error << " - using defaults\n";
    return applyWithRetry(resolveOutput(loaded.config.lightingState()), kApplyStandDown,
                          lockTimeout, out, err)
        .exitCode;
}

int runOffMode(std::ostream& out, std::ostream& err, std::chrono::milliseconds lockTimeout) {
    LoadResult loaded = loadConfig(configPath());
    if (!loaded.error.empty())
        err << "alienfx-tray: " << loaded.error << " - using defaults\n";
    LightingState state = loaded.config.lightingState();
    state.lightsOn = false;
    return applyWithRetry(resolveOutput(state), kOffStandDown, lockTimeout, out, err).exitCode;
}

int runProbeExternalMode(std::optional<int> singleLightId, std::ostream& out,
                         std::ostream& err, std::chrono::milliseconds lockTimeout) {
    if (singleLightId && (*singleLightId < 0 || *singleLightId >= kExternalLightIdCount)) {
        err << "alienfx-tray: external light ID must be between 0 and "
            << (kExternalLightIdCount - 1) << "\n";
        return kExitBadArguments;
    }

    LoadResult loaded = loadConfig(configPath());
    if (!loaded.error.empty())
        err << "alienfx-tray: " << loaded.error << " - using defaults\n";

    // Paint first, then say what to look at. The guidance below is the whole output of this
    // mode, and printing it over lighting this run did not set is worse than printing
    // nothing: the user reads a zone mapping off whatever happened to be lit already and
    // writes it down as measured fact.
    const RunOutcome outcome = applyWithRetry(buildProbeOutput(loaded.config, singleLightId),
                                              kProbeStandDown, lockTimeout, out, err);
    if (!outcome.applied) return outcome.exitCode;
    if (!outcome.result.external.ok()) {
        err << "alienfx-tray: the external controller was not painted, so there is nothing "
               "to look at\n";
        return kExitNothingApplied;
    }

    if (singleLightId) {
        out << "\nExternal light ID " << *singleLightId
            << " is lit white. IDs 0-3 are driven black; any other ID keeps whatever it "
               "was\nalready showing.\n\n"
               "Look at the laptop and note exactly which fixture is lit - and whether it is\n"
               "the whole of that fixture or only part of it.\n\n";
    } else {
        out << "\nEach external ID is lit a distinct colour. Look at the laptop and note "
               "which\nphysical zone shows which colour:\n\n";
        for (const auto& entry : kProbeColors)
            out << "  ID " << static_cast<int>(entry.id) << "  " << entry.name << "\n";
        out << "\nIDs 0 and 1 both drive the rear ring - you should see two colours on the "
               "ring.\n"
               "IDs 4 and up are not sent, so they keep whatever they were already showing\n\n"
               "If two zones report the same colour, or one zone reports two colours, rerun\n"
               "with a single ID to remove the ambiguity:  --probe-external=1\n\n";
    }

    out << "Nothing is written to device memory, so this is undone by a reboot, a resume or\n"
           "the next scheme you apply.\n\n";

    if (!loaded.config.lightsOn)
        out << "The lights are switched off, so every zone was painted black and nothing "
               "will\nlight up. Turn them back on and run this again.\n\n";

    return outcome.exitCode;
}

ApplyResult applyDomainsWithRetry(LightingEngine& engine, const ResolvedScheme& scheme,
                                  std::chrono::milliseconds pollTimeout,
                                  std::chrono::milliseconds retryInterval, std::ostream& err) {
    const auto deadline = std::chrono::steady_clock::now() + pollTimeout;

    ApplyResult result;
    for (int attempt = 1;; ++attempt) {
        for (Domain domain : kAllDomains) {
            DomainStatus& status = result.forDomain(domain);
            if (!mightStillComeGood(status.state)) continue;

            status = engine.applyDomain(domain, scheme);

            // The moment a domain is finished with - applied, or failed in a way waiting will
            // not mend - its interface goes back. Anything else leaves the keyboard claimed
            // while we wait on the other controller, and a claimed keyboard is one the user
            // cannot type on.
            if (!mightStillComeGood(status.state)) engine.releaseDevice(domain);
        }

        // Only keep waiting for conditions that plausibly clear on their own. A controller
        // that has not been re-enumerated yet reports Missing, and that is the exact case
        // this poll exists for: on resume, usbhid has not necessarily finished. A controller
        // that actively refused a command is not retried - it will refuse again.
        const bool anyPending = std::ranges::any_of(kAllDomains, [&](Domain domain) {
            return mightStillComeGood(result.forDomain(domain).state);
        });
        if (!anyPending || std::chrono::steady_clock::now() >= deadline) break;

        err << "alienfx-tray: attempt " << attempt << " incomplete, retrying\n";
        std::this_thread::sleep_for(retryInterval);
    }
    return result;
}

ResolvedScheme buildProbeOutput(const Config& config, std::optional<int> singleLightId) {
    Scheme scheme;
    scheme.name = "external probe";

    // The one deliberate inheritance, and the only one. The keyboard keeps what the active
    // scheme gives it so the mode does not repaint the lighting around the lights being read.
    scheme.keyboard = config.activeScheme().keyboard;

    // Every external property is stated here rather than carried over from the active scheme.
    // Anything inherited can make the light the user is being asked to look at invisible, and
    // a diagnostic that identifies lights by eye must not be dimmable by unrelated config.
    scheme.external.brightness = Brightness{Brightness::kMax};
    for (std::uint8_t id : kVisibleHardwareIds)
        scheme.external.zones.push_back(
            ExternalZone{id, defaultZoneName(id), uniform(probeColor(id, singleLightId))});
    if (singleLightId && !std::ranges::contains(kVisibleHardwareIds,
                                                static_cast<std::uint8_t>(*singleLightId))) {
        const auto id = static_cast<std::uint8_t>(*singleLightId);
        scheme.external.zones.push_back(
            ExternalZone{id, defaultZoneName(id), uniform(probeColor(id, singleLightId))});
    }

    return resolveOutput(LightingState{std::move(scheme), config.lightsOn});
}

}  // namespace alienfx
