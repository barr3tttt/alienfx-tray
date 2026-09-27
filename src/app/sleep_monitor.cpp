#include "app/sleep_monitor.h"

#include <QDBusConnection>

namespace alienfx {

SleepMonitor::SleepMonitor(QObject* parent) : SleepMonitor(QDBusConnection::systemBus(), parent) {}

SleepMonitor::SleepMonitor(const QDBusConnection& bus, QObject* parent) : QObject(parent) {
    QDBusConnection connection = bus;
    watching_ = connection.isConnected() &&
                connection.connect(QStringLiteral("org.freedesktop.login1"),
                                   QStringLiteral("/org/freedesktop/login1"),
                                   QStringLiteral("org.freedesktop.login1.Manager"),
                                   QStringLiteral("PrepareForSleep"), this,
                                   SLOT(handlePrepareForSleep(bool)));
}

void SleepMonitor::handlePrepareForSleep(bool goingToSleep) {
    if (!goingToSleep) emit resumed();
}

}  // namespace alienfx
