#pragma once

#include <memory>

#include "core/device_interface.h"
#include "device/usb_context.h"

namespace alienfx {

struct DeviceSet {
    std::unique_ptr<IKeyboardDevice> keyboard;
    std::unique_ptr<IExternalDevice> external;
};

[[nodiscard]] DeviceSet makeSdkDevices(UsbContext& context);

// The SDK links loguru and logs at INFO on every command. A tray application has no
// business writing to stderr every time a slider moves.
void silenceSdkLogging();

}  // namespace alienfx
