#include "device/sdk_devices.h"

#include <thread>
#include <utility>

// AlienFX_SDK.h does `using namespace std;` at file scope. It is included here and in no
// other translation unit in this project, which is the reason the IKeyboardDevice /
// IExternalDevice interfaces exist at all.
#include "AlienFX_SDK.h"
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

AlienFX_SDK::Afx_action powerAction(Rgb color) {
    return AlienFX_SDK::Afx_action{AlienFX_SDK::AlienFX_A_Power, kPowerActionTime,
                                   kPowerActionTempo, color.r, color.g, color.b};
}

}  // namespace

SdkDeviceBase::SdkDeviceBase(UsbContext& context, DeviceId id) : context_(context), id_(id) {}

SdkDeviceBase::~SdkDeviceBase() = default;

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

bool SdkExternalDevice::applyZone(std::uint8_t lightId, Rgb acColor, Rgb batteryColor) {
    PhaseTimer timer("external/applyZone");
    AlienFX_SDK::Functions* fx = device();
    if (!fx) {
        markError("device not open");
        return false;
    }

    if (!waitUntilReady()) {
        // Refusing here is the point: SetPowerAction's WaitForReady() has no timeout.
        status_ = DomainStatus{DeviceState::Busy, "controller did not become ready"};
        return false;
    }

    // These are power-type lights carrying two colours, AC and battery. A plain colour set
    // across all 23 mapped IDs does nothing whatsoever on this controller - it has to be
    // the power path.
    AlienFX_SDK::Afx_lightblock block{};
    block.index = lightId;
    block.act = {powerAction(acColor), powerAction(batteryColor)};

    if (!fx->SetPowerAction(&block)) {
        markError("power-light command refused for light " + std::to_string(lightId));
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
    fx->Reset();
    if (!fx->UpdateColors()) {
        markError("update refused");
        return false;
    }
    markReady();
    return true;
}

}  // namespace alienfx
