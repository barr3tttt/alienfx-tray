#include "app/metatypes.h"

namespace alienfx {

void registerMetaTypes() {
    qRegisterMetaType<Domain>("alienfx::Domain");
    qRegisterMetaType<DeviceState>("alienfx::DeviceState");
    qRegisterMetaType<DomainStatus>("alienfx::DomainStatus");
}

}  // namespace alienfx
