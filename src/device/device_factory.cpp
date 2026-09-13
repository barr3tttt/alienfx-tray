#include "device/device_factory.h"

#include <loguru.hpp>

#include "device/sdk_devices.h"

namespace alienfx {

DeviceSet makeSdkDevices(UsbContext& context) {
    DeviceSet set;
    set.keyboard = std::make_unique<SdkKeyboardDevice>(context);
    set.external = std::make_unique<SdkExternalDevice>(context);
    return set;
}

void silenceSdkLogging() {
    // Not loguru::init(): that installs signal handlers and takes over the process's fatal
    // handling, which is not a third-party library's decision to make inside a GUI app.
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
    loguru::g_preamble = false;
}

}  // namespace alienfx
