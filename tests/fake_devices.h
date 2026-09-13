#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/device_interface.h"

namespace alienfx::test {

// Records every byte the engine would put on the wire, so the tests can assert on what the
// hardware would have been told rather than on what the code looks like.
class FakeKeyboardDevice final : public IKeyboardDevice {
   public:
    struct EffectCall {
        std::uint8_t wireEffectType;
        Rgb color;
    };

    bool open() override {
        ++openCount;
        if (failOpen) {
            status_ = DomainStatus{DeviceState::Missing, "fake: not present"};
            return false;
        }
        open_ = true;
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    void close() override {
        if (open_) ++closeCount;
        open_ = false;
    }

    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness brightness) override {
        brightnessCalls.push_back(brightness.value());
        if (failBrightness) {
            status_ = DomainStatus{DeviceState::Error, "fake: brightness refused"};
            return false;
        }
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    bool applyEffect(KeyboardEffect effect, Rgb color) override {
        // toWireValue is the production conversion, deliberately - the test must exercise
        // the real path from a KeyboardEffect to the byte the device would receive.
        effectCalls.push_back(EffectCall{toWireValue(effect), color});
        if (failEffect) {
            status_ = DomainStatus{DeviceState::Error, "fake: effect refused"};
            return false;
        }
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    [[nodiscard]] DomainStatus status() const override { return status_; }

    std::vector<EffectCall> effectCalls;
    std::vector<std::uint8_t> brightnessCalls;
    int openCount = 0;
    int closeCount = 0;
    bool failOpen = false;
    bool failBrightness = false;
    bool failEffect = false;

   private:
    bool open_ = false;
    DomainStatus status_;
};

class FakeExternalDevice final : public IExternalDevice {
   public:
    struct ZoneCall {
        std::uint8_t lightId;
        Rgb acColor;
        Rgb batteryColor;
    };
    struct BrightnessCall {
        std::uint8_t value;
        std::vector<std::uint8_t> lightIds;
    };

    bool open() override {
        ++openCount;
        if (failOpen) {
            status_ = DomainStatus{DeviceState::Busy, "fake: busy"};
            return false;
        }
        open_ = true;
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    void close() override {
        if (open_) ++closeCount;
        open_ = false;
    }

    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness brightness,
                       const std::vector<std::uint8_t>& lightIds) override {
        brightnessCalls.push_back(BrightnessCall{brightness.value(), lightIds});
        if (failBrightness) {
            status_ = DomainStatus{DeviceState::Error, "fake: brightness refused"};
            return false;
        }
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    bool applyZone(std::uint8_t lightId, Rgb acColor, Rgb batteryColor) override {
        zoneCalls.push_back(ZoneCall{lightId, acColor, batteryColor});
        if (failZone) {
            status_ = DomainStatus{DeviceState::Error, "fake: zone refused"};
            return false;
        }
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    bool commit() override {
        ++commitCount;
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    [[nodiscard]] DomainStatus status() const override { return status_; }

    std::vector<ZoneCall> zoneCalls;
    std::vector<BrightnessCall> brightnessCalls;
    int openCount = 0;
    int closeCount = 0;
    int commitCount = 0;
    bool failOpen = false;
    bool failBrightness = false;
    bool failZone = false;

   private:
    bool open_ = false;
    DomainStatus status_;
};

}  // namespace alienfx::test
