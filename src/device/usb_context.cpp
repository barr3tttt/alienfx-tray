#include "device/usb_context.h"

#include <hidapi.h>
#include <libusb.h>

#include "device/phase_timer.h"

namespace alienfx {

UsbContext::UsbContext() {
    PhaseTimer timer("usb/init");

    if (const int result = libusb_init(&context_); result != 0) {
        context_ = nullptr;
        lastError_ = std::string("libusb_init failed: ") + libusb_error_name(result);
        return;
    }
    if (hid_init() != 0) {
        lastError_ = "hid_init failed";
        libusb_exit(context_);
        context_ = nullptr;
        return;
    }
    hidInitialised_ = true;
}

UsbContext::~UsbContext() {
    PhaseTimer timer("usb/shutdown");

    if (hidInitialised_) hid_exit();
    if (context_) libusb_exit(context_);
}

bool UsbContext::reattachKernelDriver(std::uint16_t vendorId, std::uint16_t productId) {
    if (!context_) return false;
    PhaseTimer timer("usb/reattachKernelDriver");

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &devices);
    if (count < 0) return false;

    bool reattachedAll = true;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0) continue;
        if (descriptor.idVendor != vendorId || descriptor.idProduct != productId) continue;

        libusb_config_descriptor* config = nullptr;
        if (libusb_get_config_descriptor(devices[i], 0, &config) != 0) continue;

        libusb_device_handle* handle = nullptr;
        if (libusb_open(devices[i], &handle) == 0) {
            for (int interface = 0; interface < config->bNumInterfaces; ++interface) {
                // 0 means "no kernel driver attached", which is exactly the state we are
                // here to undo. 1 means one is already attached and there is nothing to do.
                if (libusb_kernel_driver_active(handle, interface) != 0) continue;
                if (libusb_attach_kernel_driver(handle, interface) != 0) reattachedAll = false;
            }
            libusb_close(handle);
        } else {
            reattachedAll = false;
        }
        libusb_free_config_descriptor(config);
    }

    libusb_free_device_list(devices, 1);
    return reattachedAll;
}

std::string UsbContext::findDevicePath(std::uint16_t vendorId, std::uint16_t productId) {
    hid_device_info* devices = hid_enumerate(vendorId, productId);
    std::string path;
    for (hid_device_info* current = devices; current; current = current->next) {
        if (current->path && *current->path) {
            path = current->path;
            break;
        }
    }
    hid_free_enumeration(devices);
    return path;
}

}  // namespace alienfx
