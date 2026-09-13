#include "core/device_interface.h"

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
    }
    return "unknown";
}

std::string describe(Domain domain, const DomainStatus& status) {
    const std::string_view name =
        domain == Domain::Keyboard ? "Keyboard controller" : "External lighting controller";

    switch (status.state) {
        case DeviceState::Ready:
            return std::string(name) + " ready.";
        case DeviceState::Missing:
            return std::string(name) + " not found.";
        case DeviceState::Busy:
            return std::string(name) +
                   " is busy - another process is holding it. Will retry on the next change.";
        case DeviceState::Error:
            return std::string(name) + " refused the last command" +
                   (status.detail.empty() ? "." : ": " + status.detail);
        case DeviceState::Unknown:
            break;
    }
    return std::string(name) + " not checked yet.";
}

}  // namespace alienfx
