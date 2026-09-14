#include <QtTest>

#include "app/lighting_controller.h"
#include "core/device_interface.h"

using namespace alienfx;

// When an apply misses the watchdog deadline the device thread is wedged and no further
// apply will ever complete, so the UI must stop implying otherwise. Which domain is stuck is
// unknowable from the controller, so the decision is made per domain from what it last said.
class TestWatchdogEscalation : public QObject {
    Q_OBJECT

   private slots:
    // The case this exists for. Two processes racing for the device lock publish Busy to
    // both domains; if the next apply then wedges, a Busy left standing keeps promising a
    // retry that is never coming and the hang stays invisible for the life of the process.
    void busyIsEscalatedRatherThanShieldingAWedge() {
        QVERIFY(LightingController::escalatesOnWedge(DeviceState::Busy));
    }

    void aDomainThatLookedHealthyIsEscalated() {
        QVERIFY(LightingController::escalatesOnWedge(DeviceState::Ready));
        QVERIFY(LightingController::escalatesOnWedge(DeviceState::Unknown));
    }

    // These already name a specific fault, which beats the generic stamp.
    void aSpecificFaultKeepsItsOwnDiagnosis() {
        QVERIFY(!LightingController::escalatesOnWedge(DeviceState::Missing));
        QVERIFY(!LightingController::escalatesOnWedge(DeviceState::Error));
    }
};

QTEST_GUILESS_MAIN(TestWatchdogEscalation)
#include "test_watchdog_escalation.moc"
