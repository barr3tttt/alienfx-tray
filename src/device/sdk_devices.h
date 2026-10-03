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
    bool applyZones(const std::vector<ExternalZoneWrite>& zones) override;
    bool commit() override;

   private:
    // Every command this controller answers to can leave it programming for a while, and
    // the SDK's own wait for that - WaitForReady(), reachable from SetPowerAction and
    // Reset() - is `while (!IsDeviceReady()) usleep(20);` on APIv4: an unbounded spin.
    // Nothing here calls either of those SDK functions; the power transaction and the
    // finish-and-play are hand-rolled over PrepareAndSend precisely so this deadline is
    // the only wait on the path. A controller that does not settle in time is reported
    // busy rather than allowed to wedge the device thread.
    [[nodiscard]] bool waitUntilReady();

    static constexpr std::chrono::milliseconds kReadyTimeout{250};
};

}  // namespace alienfx
