#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "core/device_interface.h"
#include "device/usb_context.h"

namespace AlienFX_SDK {
class Functions;
}

namespace alienfx {

// USB identities, established empirically on an Alienware m15 R7 AMD. There are two
// independent controllers, which is why every piece of advice on the internet about this
// laptop contradicts every other piece.
struct DeviceId {
    std::uint16_t vendorId;
    std::uint16_t productId;
};

// Darfon. APIv5, 64-byte feature reports, report ID 0xCC. Drives the RGB keyboard.
// This is also the interface the user types on - there is no separate LED interface.
inline constexpr DeviceId kKeyboardDeviceId{0x0d62, 0xcabc};

// Alienware AW-ELC. APIv4, 33-byte reports, vendor page 0xFF00. Drives the rear ring
// light, the lid logo and the interior logo.
inline constexpr DeviceId kExternalDeviceId{0x187c, 0x0550};

// Shared plumbing: opening, closing and status bookkeeping are identical for both
// controllers; only the command vocabulary differs.
class SdkDeviceBase {
   public:
    SdkDeviceBase(UsbContext& context, DeviceId id);
    virtual ~SdkDeviceBase();

    bool open();
    void close();
    [[nodiscard]] bool isOpen() const { return device_ != nullptr; }
    [[nodiscard]] DomainStatus status() const { return status_; }

   protected:
    [[nodiscard]] AlienFX_SDK::Functions* device() const { return device_.get(); }
    void markReady();
    void markError(std::string detail);

    UsbContext& context_;
    DeviceId id_;
    DomainStatus status_;

   private:
    // AlienFXProbeDevice stores the char* it is given, so the string has to outlive the
    // Functions object rather than being a temporary.
    std::string devicePath_;
    std::unique_ptr<AlienFX_SDK::Functions> device_;
};

class SdkKeyboardDevice final : public SdkDeviceBase, public IKeyboardDevice {
   public:
    explicit SdkKeyboardDevice(UsbContext& context);

    bool open() override { return SdkDeviceBase::open(); }
    void close() override { SdkDeviceBase::close(); }
    [[nodiscard]] bool isOpen() const override { return SdkDeviceBase::isOpen(); }
    [[nodiscard]] DomainStatus status() const override { return SdkDeviceBase::status(); }

    bool setBrightness(Brightness brightness) override;
    bool applyEffect(KeyboardEffect effect, Rgb color) override;
};

class SdkExternalDevice final : public SdkDeviceBase, public IExternalDevice {
   public:
    explicit SdkExternalDevice(UsbContext& context);

    bool open() override { return SdkDeviceBase::open(); }
    void close() override { SdkDeviceBase::close(); }
    [[nodiscard]] bool isOpen() const override { return SdkDeviceBase::isOpen(); }
    [[nodiscard]] DomainStatus status() const override { return SdkDeviceBase::status(); }

    bool setBrightness(Brightness brightness,
                       const std::vector<std::uint8_t>& lightIds) override;
    bool applyZone(std::uint8_t lightId, Rgb acColor, Rgb batteryColor) override;
    bool commit() override;

   private:
    // AlienFX_SDK::Functions::SetPowerAction reaches WaitForReady(), whose APIv4 branch is
    // `while (!IsDeviceReady()) usleep(20);` - an unbounded spin with no timeout.
    //
    // This pre-check cannot bound that. The spin is not at the entry to the path: the SDK
    // runs its whole 0x5b..0x60 write loop, then PrepareAndSend(COMMV4_control), then
    // WaitForBusy(), and only then reaches the spin. A controller that answers ready here
    // and latches busy mid-call still wedges the calling thread indefinitely. All the
    // pre-check buys is that an already-unresponsive controller is reported busy instead of
    // being called into, which makes entering the path rarer, not impossible.
    //
    // The residual risk is contained outside this class: the device thread is the only
    // thread that may be wedged, nothing is allowed to wait on it without a bound (see
    // DeviceThread), and LightingController's watchdog reports the domain as unresponsive
    // so a wedge is visible rather than silent.
    [[nodiscard]] bool waitUntilReady();

    static constexpr std::chrono::milliseconds kReadyTimeout{250};
};

}  // namespace alienfx
