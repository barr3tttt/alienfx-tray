#include "core/lighting_engine.h"

#include <algorithm>
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

bool sameLightIds(const ExternalSettings& a, const ExternalSettings& b) {
    if (a.zones.size() != b.zones.size()) return false;
    for (std::size_t i = 0; i < a.zones.size(); ++i)
        if (a.zones[i].lightId != b.zones[i].lightId) return false;
    return true;
}

}  // namespace

LightingEngine::LightingEngine(std::unique_ptr<IKeyboardDevice> keyboard,
                               std::unique_ptr<IExternalDevice> external)
    : keyboard_(std::move(keyboard)), external_(std::move(external)) {}

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
    if (settings.zones.empty()) {
        appliedExternal_ = settings;
        return DomainStatus{DeviceState::Ready, "no zones configured"};
    }

    const bool wasOpen = external_->isOpen();
    if (!wasOpen) {
        appliedExternal_.reset();
        if (!external_->open()) {
            appliedExternal_.reset();
            return external_->status();
        }
    }

    const ExternalSettings* applied = appliedExternal_ ? &*appliedExternal_ : nullptr;

    // On APIv4 the brightness command is also what turns the listed lights on, so it has to
    // be resent whenever the set of lights changes, not only when the level does.
    if (!applied || applied->brightness != settings.brightness ||
        !sameLightIds(*applied, settings)) {
        std::vector<std::uint8_t> lightIds = collectHardwareIds(settings);

        if (!external_->setBrightness(settings.brightness, lightIds)) {
            appliedExternal_.reset();
            return external_->status();
        }
    }

    bool sentAnything = false;
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
    // Once a handle is gone, what that controller is showing is no longer something we can
    // claim to know: its next apply starts from scratch.
    if (domain == Domain::Keyboard) {
        if (keyboard_) keyboard_->close();
        appliedKeyboard_.reset();
        return;
    }
    if (external_) external_->close();
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
