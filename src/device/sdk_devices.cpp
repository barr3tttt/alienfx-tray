#include "device/sdk_devices.h"

#include <thread>
#include <utility>

// AlienFX_SDK.h does `using namespace std;` at file scope. It is included here and in no
// other translation unit in this project, which is the reason the IKeyboardDevice /
// IExternalDevice interfaces exist at all. alienfx_control.h carries the APIv4 command
// templates the hand-rolled power transaction below is built from.
#include "AlienFX_SDK.h"
#include "alienfx_control.h"
#include "device/phase_timer.h"

namespace alienfx {
namespace {

// Matches the known-good invocation on this hardware:
//   alienfx setglobal 0 1 0 <b> <g> <r>
// which reaches SetGlobalEffects(effType=1, mode=0, nc=1, tempo=5, colour, {}).
constexpr std::uint8_t kGlobalEffectMode = 0;
constexpr std::uint8_t kGlobalEffectColorCount = 1;
constexpr std::uint8_t kGlobalEffectTempo = 5;

// APIv4 power-light action parameters, as the CLI sends them.
constexpr std::uint8_t kPowerActionTime = 3;
constexpr std::uint8_t kPowerActionTempo = 0x64;

// Afx_colorcode's positional constructor takes (b, g, r), not (r, g, b). That single fact
// is why half the notes about this hardware on the internet have red and blue swapped.
// Build it from named fields so the order cannot be misread or accidentally reordered.
AlienFX_SDK::Afx_colorcode toSdkColor(Rgb color) {
    AlienFX_SDK::Afx_colorcode out{0ul};
    out.r = color.r;
    out.g = color.g;
    out.b = color.b;
    out.br = 0;
    return out;
}

}  // namespace

SdkDeviceBase::SdkDeviceBase(UsbContext& context, DeviceId id) : context_(context), id_(id) {}

// Closes on destruction, not just on an explicit close(). The kernel-driver reattach
// below is too expensive to leave to convention: a destroyed-still-open device would keep
// the interface driverless until reboot, and on this hardware that interface is the
// built-in keyboard. close() early-returns when already closed, so the explicit release
// paths that run first cost nothing extra.
SdkDeviceBase::~SdkDeviceBase() { close(); }

bool SdkDeviceBase::open() {
    if (device_) return true;
    PhaseTimer timer("open");

    if (!context_.valid()) {
        status_ = DomainStatus{DeviceState::Error, context_.lastError()};
        return false;
    }

    {
        PhaseTimer findTimer("open/findDevicePath");
        devicePath_ = context_.findDevicePath(id_.vendorId, id_.productId);
    }
    if (devicePath_.empty()) {
        status_ = DomainStatus{DeviceState::Missing, "device not present"};
        return false;
    }

    auto device = std::make_unique<AlienFX_SDK::Functions>();
    PhaseTimer probeTimer("open/probe");
    if (!device->AlienFXProbeDevice(context_.raw(), id_.vendorId, id_.productId,
                                    devicePath_.data())) {
        // The path exists, so the device is plugged in. Failing to open it means either
        // another process holds the interface or the udev rule is not in place.
        status_ = DomainStatus{DeviceState::Busy,
                               "could not claim the interface (held by another process, or "
                               "the udev rule is missing)"};
        return false;
    }

    device_ = std::move(device);
    markReady();
    return true;
}

void SdkDeviceBase::close() {
    if (!device_) return;
    PhaseTimer timer("close");

    // ~Functions calls hid_close(), which releases the interface - but does NOT give it back
    // to the kernel driver, because hidapi's re-attach lives inside #ifdef
    // DETACH_KERNEL_DRIVER and that macro is never defined by its build. Left alone, the
    // interface stays driverless until a reboot. Reattaching is therefore ours to do, and it
    // has to happen every single time a handle closes.
    device_.reset();
    const bool reattached = context_.reattachKernelDriver(id_.vendorId, id_.productId);

    // Releasing the handle always happens, so close() never fails - but a driverless
    // interface costs the user a working keyboard until it is reattached or the machine
    // reboots, which is far too expensive to close over in silence. The state is left alone
    // because the lighting itself is not what went wrong.
    status_.interfaceLeftDetached = !reattached;
}

void SdkDeviceBase::markReady() { status_ = DomainStatus{DeviceState::Ready, {}}; }

void SdkDeviceBase::markError(std::string detail) {
    status_ = DomainStatus{DeviceState::Error, std::move(detail)};
}

// --------------------------------------------------------------------------- keyboard ---

SdkKeyboardDevice::SdkKeyboardDevice(UsbContext& context)
    : SdkDeviceBase(context, kKeyboardDeviceId) {}

bool SdkKeyboardDevice::setBrightness(Brightness brightness) {
    PhaseTimer timer("keyboard/setBrightness");
    AlienFX_SDK::Functions* fx = device();
    if (!fx) {
        markError("device not open");
        return false;
    }

    // APIv5 ignores the mappings list; an empty vector keeps the call well-defined anyway.
    std::vector<AlienFX_SDK::Afx_light> noMappings;
    if (!fx->SetBrightness(brightness.value(), Brightness::kMax, &noMappings, true)) {
        markError("brightness command refused");
        return false;
    }
    markReady();
    return true;
}

bool SdkKeyboardDevice::applyEffect(KeyboardEffect effect, Rgb color) {
    PhaseTimer timer("keyboard/applyEffect");
    AlienFX_SDK::Functions* fx = device();
    if (!fx) {
        markError("device not open");
        return false;
    }

    // toWireValue is a lookup over the enumerator table, every entry of which is
    // static_asserted non-zero. effType 0 sends the APIv5 clear/disable that leaves this
    // keyboard dark in a state no later command can recover. It cannot be produced here.
    const std::uint8_t effectType = toWireValue(effect);

    if (!fx->SetGlobalEffects(effectType, kGlobalEffectMode, kGlobalEffectColorCount,
                              kGlobalEffectTempo, toSdkColor(color), toSdkColor(Rgb{}))) {
        markError("global effect command refused");
        return false;
    }
    markReady();
    return true;
}

// --------------------------------------------------------------------------- external ---

SdkExternalDevice::SdkExternalDevice(UsbContext& context)
    : SdkDeviceBase(context, kExternalDeviceId) {}

bool SdkExternalDevice::waitUntilReady() {
    PhaseTimer timer("external/waitUntilReady");
    AlienFX_SDK::Functions* fx = device();
    if (!fx) return false;

    const auto deadline = std::chrono::steady_clock::now() + kReadyTimeout;
    do {
        if (fx->IsDeviceReady()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
}

bool SdkExternalDevice::setBrightness(Brightness brightness,
                                      const std::vector<std::uint8_t>& lightIds) {
    PhaseTimer timer("external/setBrightness");
    AlienFX_SDK::Functions* fx = device();
    if (!fx) {
        markError("device not open");
        return false;
    }

    // On APIv4 this command is also what turns the listed lights on, so the ID list is not
    // optional. Built in memory - the CLI's mappings.json is not this application's to read.
    std::vector<AlienFX_SDK::Afx_light> lights;
    lights.reserve(lightIds.size());
    for (std::uint8_t id : lightIds) {
        AlienFX_SDK::Afx_light light{};
        light.lightid = id;
        light.data = 0;  // no POWER or INDICATOR flags: these are ordinary zones
        lights.push_back(light);
    }

    if (!fx->SetBrightness(brightness.value(), Brightness::kMax, &lights, true)) {
        markError("brightness command refused");
        return false;
    }
    markReady();
    return true;
}

namespace {

using AlienFX_SDK::Afx_action;
using AlienFX_SDK::Afx_icommand;

// One report on the wire, or false. PrepareAndSend takes the mod list by pointer and
// empties it; the by-value parameter hands it a copy it can drain.
bool sendReport(AlienFX_SDK::Functions* fx, const std::uint8_t* command,
                std::vector<Afx_icommand> mods) {
    return fx->PrepareAndSend(command, &mods);
}

// The action list one power block carries for one light, mirroring the construction the
// SDK's SetPowerAction uses, byte for byte. Each of the six control IDs is a power state;
// the state decides whether the AC colour, the battery colour, or black fills each slot.
// front/back are the caller's AC/battery actions.
std::vector<Afx_action> powerBlockActions(std::uint8_t controlId,
                                          const ExternalZoneWrite& zone) {
    const Afx_action front{AlienFX_SDK::AlienFX_A_Power, kPowerActionTime, kPowerActionTempo,
                           zone.acColor.r, zone.acColor.g, zone.acColor.b};
    const Afx_action back{AlienFX_SDK::AlienFX_A_Power, kPowerActionTime, kPowerActionTempo,
                          zone.batteryColor.r, zone.batteryColor.g, zone.batteryColor.b};
    const Afx_action black{AlienFX_SDK::AlienFX_A_Power, kPowerActionTime, kPowerActionTempo,
                           0, 0, 0};
    Afx_action frontAsColor = front;
    frontAsColor.type = AlienFX_SDK::AlienFX_A_Color;
    Afx_action frontAsPulse = front;
    frontAsPulse.type = AlienFX_SDK::AlienFX_A_Pulse;

    switch (controlId) {
        case 0x5b:  // AC sleep
            return {front, back, front, black};
        case 0x5c:  // AC power
            return {frontAsColor, back, front};
        case 0x5d:  // charge
            return {front, back, front, back};
        case 0x5e:  // battery sleep
            return {front, back, back, black};
        case 0x5f:  // battery power
            return {frontAsColor, back, back};
        case 0x60:  // battery critical
            return {frontAsPulse, back, back};
        default:
            return {};
    }
}

// One action's 8-byte entry in a colour-set report - the packing the SDK's SetV4Action
// produces, v4OpCodes and all. Reports carry at most three entries, at offsets 3, 11, 19.
Afx_icommand packAction(const Afx_action& action, int offset) {
    const std::uint8_t type = action.type < AlienFX_SDK::AlienFX_A_Breathing
                                  ? action.type
                                  : static_cast<std::uint8_t>(AlienFX_SDK::AlienFX_A_Morph);
    return {offset,
            {type, action.time, AlienFX_SDK::v4OpCodes[action.type], 0,
             static_cast<std::uint8_t>(action.type == AlienFX_SDK::AlienFX_A_Color
                                           ? 0xfa
                                           : action.tempo),
             action.r, action.g, action.b}};
}

}  // namespace

bool SdkExternalDevice::applyZones(const std::vector<ExternalZoneWrite>& zones) {
    // The light count rides in the label: the timing breakdown is the wire-level evidence
    // that a scheme apply staged every zone in one transaction, and a bare label cannot
    // show that.
    const std::string timerLabel =
        "external/applyZones (" + std::to_string(zones.size()) + " lights)";
    PhaseTimer timer(timerLabel.c_str());
    AlienFX_SDK::Functions* fx = device();
    if (!fx) {
        markError("device not open");
        return false;
    }
    if (zones.empty()) {
        markReady();
        return true;
    }

    if (!waitUntilReady()) {
        status_ = DomainStatus{DeviceState::Busy, "controller did not become ready"};
        return false;
    }

    // The power transaction, hand-rolled over PrepareAndSend rather than built from the
    // SDK's SetPowerAction. SetPowerAction stages ONE light into the six power blocks
    // (control IDs 0x5b..0x60), and the blocks are shared slots: calling it once per
    // light re-stages every block for the new light, so the play that follows applies
    // only the last light written and the rest keep whatever they already showed. Every
    // zone is therefore staged into each block before the play. The per-block shape -
    // remove, start, colour-select plus colour-set per zone, finish-and-save - is the
    // one the SDK produces for a single light, so a one-zone batch is byte-identical to
    // the call it replaces.
    for (std::uint8_t cid = 0x5b; cid < 0x61; ++cid) {
        if (!sendReport(fx, AlienFX_SDK::COMMV4_setPower, {{4, {4, 0, cid}}}) ||
            !sendReport(fx, AlienFX_SDK::COMMV4_setPower, {{4, {1, 0, cid}}})) {
            markError("power-block open refused");
            return false;
        }

        for (const ExternalZoneWrite& zone : zones) {
            if (!sendReport(fx, AlienFX_SDK::COMMV4_colorSel, {{6, {zone.lightId}}})) {
                markError("light-select refused for light " + std::to_string(zone.lightId));
                return false;
            }
            const std::vector<Afx_action> actions = powerBlockActions(cid, zone);
            for (std::size_t first = 0; first < actions.size(); first += 3) {
                std::vector<Afx_icommand> mods;
                for (std::size_t slot = 0; slot < 3 && first + slot < actions.size();
                     ++slot)
                    mods.push_back(
                        packAction(actions[first + slot], 3 + static_cast<int>(slot) * 8));
                if (!sendReport(fx, AlienFX_SDK::COMMV4_colorSet, std::move(mods))) {
                    markError("colour-set refused for light " +
                              std::to_string(zone.lightId));
                    return false;
                }
            }
        }

        if (!sendReport(fx, AlienFX_SDK::COMMV4_setPower, {{4, {2, 0, cid}}})) {
            markError("power-block finish refused");
            return false;
        }
    }

    if (!sendReport(fx, AlienFX_SDK::COMMV4_control, {{4, {5}}})) {  // play what was staged
        markError("play command refused");
        return false;
    }

    // The play is acknowledged before it is processed: the controller latches busy while
    // it applies the blocks. Watch for the latch for a short bounded window - the same
    // 10 ms the SDK's WaitForBusy allows - so the ready-wait cannot slip past before the
    // busy starts, then wait for it to settle.
    const auto busyDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    while (std::chrono::steady_clock::now() < busyDeadline) {
        if (!fx->IsDeviceReady()) break;
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }

    if (!waitUntilReady()) {
        status_ = DomainStatus{DeviceState::Busy, "controller did not become ready"};
        return false;
    }
    markReady();
    return true;
}

bool SdkExternalDevice::commit() {
    PhaseTimer timer("external/commit");
    AlienFX_SDK::Functions* fx = device();
    if (!fx) {
        markError("device not open");
        return false;
    }
    if (!waitUntilReady()) {
        status_ = DomainStatus{DeviceState::Busy, "controller did not become ready"};
        return false;
    }
    // The finish-and-play that makes the staged power blocks the running program.
    // Hand-rolled rather than Reset() + UpdateColors() because Reset() reaches the SDK's
    // unbounded WaitForReady() on this API version; the three reports are exactly the ones
    // those calls send - remove the running program, start a new one from the staged
    // blocks, finish and play it - with only the bounded wait above on the path.
    if (!sendReport(fx, AlienFX_SDK::COMMV4_control, {{4, {4}}}) ||
        !sendReport(fx, AlienFX_SDK::COMMV4_control, {{4, {1}}})) {
        markError("transaction open refused");
        return false;
    }
    // No mod list: the bare control template is the finish-and-play (type 3, common ID).
    if (!fx->PrepareAndSend(AlienFX_SDK::COMMV4_control)) {
        markError("update refused");
        return false;
    }
    markReady();
    return true;
}

}  // namespace alienfx
