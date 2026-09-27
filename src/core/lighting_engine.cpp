#include "core/lighting_engine.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace alienfx {
namespace {

// Zone identity is the light ID, not the position in the list: a zone being renamed or the
// list being reordered must not make us re-send colours that have not changed.
const ExternalZone* findZone(const ExternalSettings& settings, std::uint8_t lightId) {
    for (const ExternalZone& zone : settings.zones)
        if (zone.lightId == lightId) return &zone;
    return nullptr;
}

// Compared as sets, so reordering the zone list re-sends nothing at all: zone identity is
// the light ID, not the position in the list.
bool sameLightIds(const ExternalSettings& a, const ExternalSettings& b) {
    // collectHardwareIds returns sorted, de-duplicated ID lists, so vector equality is set
    // equality here.
    return collectHardwareIds(a) == collectHardwareIds(b);
}

}  // namespace

LightingEngine::LightingEngine(std::unique_ptr<IKeyboardDevice> keyboard,
                               std::unique_ptr<IExternalDevice> external)
    : keyboard_(std::move(keyboard)), external_(std::move(external)) {}

LightingEngine::~LightingEngine() { releaseDevices(); }

ApplyResult LightingEngine::apply(const ResolvedScheme& resolved) {
    ApplyResult result;
    result.keyboard = applyKeyboard(resolved.scheme().keyboard);
    result.external = applyExternal(resolved.scheme().external);
    return result;
}

DomainStatus LightingEngine::applyDomain(Domain domain, const ResolvedScheme& resolved) {
    if (domain == Domain::Keyboard) return applyKeyboard(resolved.scheme().keyboard);
    return applyExternal(resolved.scheme().external);
}

DomainStatus LightingEngine::applyKeyboard(const KeyboardSettings& settings) {
    if (!keyboard_) return DomainStatus{DeviceState::Missing, "no keyboard backend"};

    const bool wasOpen = keyboard_->isOpen();
    if (!wasOpen) {
        // A device we have just opened is in whatever state the firmware left it in, so
        // nothing about the previous session can be assumed.
        appliedKeyboard_.reset();
        if (!keyboard_->open()) {
            appliedKeyboard_.reset();
            return keyboard_->status();
        }
    }

    const KeyboardSettings* applied = appliedKeyboard_ ? &*appliedKeyboard_ : nullptr;

    // Brightness first: on APIv5 this is also the command that wakes the controller, and
    // the CLI does the same thing at startup for the same reason.
    if (!applied || applied->brightness != settings.brightness) {
        if (!keyboard_->setBrightness(settings.brightness)) {
            appliedKeyboard_.reset();
            return keyboard_->status();
        }
    }

    // settings.effect is a KeyboardEffect, so this call cannot carry effType 0.
    if (!applied || applied->effect != settings.effect || applied->color != settings.color) {
        if (!keyboard_->applyEffect(settings.effect, settings.color)) {
            appliedKeyboard_.reset();
            return keyboard_->status();
        }
    }

    appliedKeyboard_ = settings;
    return keyboard_->status();
}

DomainStatus LightingEngine::applyExternal(const ExternalSettings& settings) {
    if (!external_) return DomainStatus{DeviceState::Missing, "no external backend"};

    // Known to be showing exactly this already: never open the controller.
    if (appliedExternal_ && *appliedExternal_ == settings) {
        if (settings.zones.empty()) return DomainStatus{DeviceState::Ready, "no zones configured"};
        return external_->status();
    }

    const bool wasOpen = external_->isOpen();
    if (!wasOpen) {
        if (!externalReleased_) appliedExternal_.reset();
        externalReleased_ = false;
        if (!external_->open()) {
            appliedExternal_.reset();
            return external_->status();
        }
    }

    const ExternalSettings* applied = appliedExternal_ ? &*appliedExternal_ : nullptr;

    // Lights that may still be lit and that this scheme no longer carries have to be driven
    // off, or they stay lit forever - an explicit empty zone list in a hand-edited config
    // means "no external lighting", not "stop updating the hardware". When the previous
    // apply is known that is exactly its lights. When it is not - a fresh engine, a failed
    // command, another process painting while it was released - the controller can be showing anything an earlier session
    // painted, so every light known to be visible on this chassis is assumed lit.
    // A black pair, not a removal: there is no off command for these lights, only colours.
    const std::vector<std::uint8_t> wanted = collectHardwareIds(settings);
    const std::vector<std::uint8_t> possiblyLit =
        applied ? collectHardwareIds(*applied)
                : std::vector<std::uint8_t>(std::begin(kVisibleHardwareIds),
                                            std::end(kVisibleHardwareIds));
    std::vector<std::uint8_t> stale;
    std::ranges::set_difference(possiblyLit, wanted, std::back_inserter(stale));

    for (std::uint8_t hwId : stale) {
        if (!external_->applyZone(hwId, Rgb{}, Rgb{})) {
            appliedExternal_.reset();
            return external_->status();
        }
    }
    bool sentAnything = !stale.empty();

    if (settings.zones.empty()) {
        if (sentAnything && !external_->commit()) {
            appliedExternal_.reset();
            return external_->status();
        }
        appliedExternal_ = settings;
        return DomainStatus{DeviceState::Ready,
                            sentAnything ? "external zones turned off" : "no zones configured"};
    }

    // On APIv4 the brightness command is also what turns the listed lights on, so it has to
    // be resent whenever the set of lights changes, not only when the level does.
    if (!applied || applied->brightness != settings.brightness ||
        !sameLightIds(*applied, settings)) {
        if (!external_->setBrightness(settings.brightness, wanted)) {
            appliedExternal_.reset();
            return external_->status();
        }
    }

    for (const ExternalZone& zone : settings.zones) {
        const ExternalZone* previous = applied ? findZone(*applied, zone.lightId) : nullptr;
        if (previous && previous->color == zone.color) continue;

        for (std::uint8_t hwId : zoneHardwareIds(zone)) {
            if (!external_->applyZone(hwId, zone.color.ac, zone.color.effectiveBattery())) {
                appliedExternal_.reset();
                return external_->status();
            }
        }
        sentAnything = true;
    }

    if (sentAnything && !external_->commit()) {
        appliedExternal_.reset();
        return external_->status();
    }

    appliedExternal_ = settings;
    return external_->status();
}

void LightingEngine::releaseDevices() {
    releaseDevice(Domain::Keyboard);
    releaseDevice(Domain::External);
}

void LightingEngine::releaseDevice(Domain domain) {
    if (domain == Domain::Keyboard) {
        if (keyboard_) keyboard_->close();
        appliedKeyboard_.reset();
        return;
    }
    if (!external_ || !external_->isOpen()) return;
    external_->close();
    externalReleased_ = true;
}

void LightingEngine::forgetAppliedState() {
    appliedKeyboard_.reset();
    appliedExternal_.reset();
}

bool LightingEngine::anyDeviceOpen() const {
    return isDeviceOpen(Domain::Keyboard) || isDeviceOpen(Domain::External);
}

bool LightingEngine::interfaceLeftDetached(Domain domain) const {
    if (domain == Domain::Keyboard)
        return keyboard_ && keyboard_->status().interfaceLeftDetached;
    return external_ && external_->status().interfaceLeftDetached;
}

bool LightingEngine::isDeviceOpen(Domain domain) const {
    if (domain == Domain::Keyboard) return keyboard_ && keyboard_->isOpen();
    return external_ && external_->isOpen();
}

}  // namespace alienfx
