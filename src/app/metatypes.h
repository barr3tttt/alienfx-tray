#pragma once

#include <QMetaType>

#include "core/device_interface.h"
#include "core/scheme.h"

// Queued signal/slot connections between the GUI thread and the device worker need these
// registered. Registration itself happens once in registerMetaTypes().
Q_DECLARE_METATYPE(alienfx::Domain)
Q_DECLARE_METATYPE(alienfx::DeviceState)
Q_DECLARE_METATYPE(alienfx::DomainStatus)

namespace alienfx {
void registerMetaTypes();
}
