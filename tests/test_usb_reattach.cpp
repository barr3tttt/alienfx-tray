#include <QtTest>

#include "core/device_interface.h"
#include "device/usb_context.h"

using namespace alienfx;

namespace {

int asInt(KernelDriverState state) { return static_cast<int>(state); }

// libusb's own values, so the cases below are the ones the real call can produce.
constexpr int kLibusbErrorIo = -1;
constexpr int kLibusbErrorNoDevice = -4;
constexpr int kLibusbErrorNotSupported = -12;
constexpr int kLibusbErrorOther = -99;

}  // namespace

// Handing a USB interface back to its kernel driver is load-bearing on this hardware: the
// keyboard controller is the built-in keyboard's only interface, so one left driverless
// costs the user their typing until something reattaches it or the machine reboots.
// Reporting that outcome as a success is the worst answer available, so the three things
// libusb_kernel_driver_active can say have to stay three things.
class TestUsbReattach : public QObject {
    Q_OBJECT

   private slots:
    void zeroMeansTheDriverHasToBePutBack() {
        QCOMPARE(asInt(classifyKernelDriverActive(0)), asInt(KernelDriverState::Detached));
    }

    void oneMeansThereIsNothingToDo() {
        QCOMPARE(asInt(classifyKernelDriverActive(1)), asInt(KernelDriverState::Attached));
    }

    // The regression: `!= 0` read every one of these as "a driver is already attached", so
    // the reattach was skipped and the whole call still reported success.
    void aNegativeErrorIsNeverMistakenForAnAttachedDriver() {
        for (int result : {kLibusbErrorIo, kLibusbErrorNoDevice, kLibusbErrorNotSupported,
                           kLibusbErrorOther}) {
            QVERIFY2(classifyKernelDriverActive(result) != KernelDriverState::Attached,
                     QByteArray::number(result).constData());
            QCOMPARE(asInt(classifyKernelDriverActive(result)),
                     asInt(KernelDriverState::Unknown));
        }
    }

    // Nothing we can act on, but equally nothing we may claim we handed back.
    void anUnknownProbeIsNotTreatedAsDetachedEither() {
        QVERIFY(classifyKernelDriverActive(kLibusbErrorNoDevice) !=
                KernelDriverState::Detached);
    }

    // The other half of "must not be swallowed": a domain whose lighting worked but whose
    // interface did not go back is still ready, and has to say what went wrong anyway.
    void aReadyDomainStillReportsAReleaseWarning() {
        const DomainStatus clean{DeviceState::Ready, {}};
        const std::string quiet = describe(Domain::Keyboard, clean);
        QVERIFY(!quiet.empty());

        const DomainStatus warned{DeviceState::Ready, "the interface is still detached"};
        const std::string noisy = describe(Domain::Keyboard, warned);
        QVERIFY2(noisy != quiet, "a warning on a ready domain must not read the same as none");
        QVERIFY(noisy.find("the interface is still detached") != std::string::npos);
        QVERIFY2(warned.ok(), "failing to release is not a lighting failure");
    }
};

QTEST_GUILESS_MAIN(TestUsbReattach)
#include "test_usb_reattach.moc"
