#include "core/device_interface.h"

#include <utility>

namespace alienfx {

std::string_view toString(DeviceState state) {
    switch (state) {
        case DeviceState::Unknown:
            return "unknown";
        case DeviceState::Ready:
            return "ready";
        case DeviceState::Missing:
            return "missing";
        case DeviceState::Busy:
            return "busy";
        case DeviceState::Error:
            return "error";
        case DeviceState::Unresponsive:
            return "unresponsive";
    }
    std::unreachable();
}

std::string describe(Domain domain, const DomainStatus& status) {
    const std::string_view name =
        domain == Domain::Keyboard ? "Keyboard controller" : "External lighting controller";

    std::string text = std::string(name) + " not checked yet.";
    switch (status.state) {
        case DeviceState::Ready:
            text = std::string(name) + " ready.";
            break;
        case DeviceState::Missing:
            text = std::string(name) + " not found.";
            break;
        case DeviceState::Busy:
            text = std::string(name) +
                   " is busy - another process is holding it. Will retry on the next change.";
            break;
        case DeviceState::Error:
            text = std::string(name) + " refused the last command" +
                   (status.detail.empty() ? "." : ": " + status.detail);
            break;
        case DeviceState::Unresponsive:
            text = std::string(name) +
                   " has stopped responding - a command went out and never came back. This "
                   "is the controller hanging, not a permissions problem. Lighting will "
                   "catch up if it answers.";
            break;
        case DeviceState::Unknown:
            break;
    }

    // Orthogonal to the state: the lighting can have worked perfectly and the interface
    // still not have gone back, which on this hardware costs a real keyboard input device
    // until something reattaches it or the machine reboots.
    if (status.interfaceLeftDetached)
        text += " Warning: the USB interface may not have been handed back to its kernel "
                "driver.";
    return text;
}

}  // namespace alienfx
