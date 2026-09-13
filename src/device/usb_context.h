#pragma once

#include <cstdint>
#include <string>

struct libusb_context;

namespace alienfx {

// Owns the hidapi and libusb global state for the process.
//
// Note what this class does NOT do: it never calls the SDK's Mappings::AlienFXEnumDevices().
// That helper runs hid_enumerate(0, 0) and opens *every* path matching a known vendor ID
// before discarding the duplicates. On this laptop the AlienFX keyboard controller is the
// built-in keyboard's only USB interface, and hidapi's libusb backend detaches usbhid to
// claim it - so a full enumeration momentarily kills typing. We look up exactly the one
// path we intend to claim instead.
class UsbContext {
   public:
    UsbContext();
    ~UsbContext();

    UsbContext(const UsbContext&) = delete;
    UsbContext& operator=(const UsbContext&) = delete;

    [[nodiscard]] bool valid() const { return context_ != nullptr; }
    [[nodiscard]] libusb_context* raw() const { return context_; }
    [[nodiscard]] const std::string& lastError() const { return lastError_; }

    // Reads descriptors only - no interface is claimed, so the kernel driver stays attached.
    // Returns an empty string when the device is not present.
    [[nodiscard]] std::string findDevicePath(std::uint16_t vendorId, std::uint16_t productId);

    // Hands every interface of a device back to its kernel driver.
    //
    // This is not belt and braces, it is load-bearing. The SDK's hidapi is built for the
    // libusb backend, and hidapi only re-attaches the kernel driver inside
    // `#ifdef DETACH_KERNEL_DRIVER` - a macro its CMake never defines. So closing the device
    // releases the interface but leaves usbhid unbound, and it stays unbound until something
    // explicitly reattaches it or the machine reboots. Nothing in the library will do it, so
    // we do it here, after every close.
    //
    // Needs no root: the udev rule tags both controllers with uaccess, and USBDEVFS_CONNECT
    // goes through the same descriptor we already have permission to open.
    bool reattachKernelDriver(std::uint16_t vendorId, std::uint16_t productId);

   private:
    libusb_context* context_ = nullptr;
    bool hidInitialised_ = false;
    std::string lastError_;
};

}  // namespace alienfx
