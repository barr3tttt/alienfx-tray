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
        DomainStatus warned{DeviceState::Ready, {}};
        warned.interfaceLeftDetached = true;

        const std::string noisy = describe(Domain::Keyboard, warned);
        QVERIFY2(warned.ok(), "failing to release is not a lighting failure");
        QVERIFY(noisy.find("Warning") != std::string::npos);
        QVERIFY(noisy.find("kernel driver") != std::string::npos);
    }

    // The warning says nothing about the lighting, so it has to survive alongside whatever
    // the lighting itself did rather than replacing it.
    void aReleaseWarningSurvivesOnANonReadyDomain() {
        DomainStatus warned{DeviceState::Error, "brightness command refused"};
        warned.interfaceLeftDetached = true;

        const std::string text = describe(Domain::External, warned);
        QVERIFY(text.find("brightness command refused") != std::string::npos);
        QVERIFY(text.find("kernel driver") != std::string::npos);
    }

    // An ordinary detail is information, not evidence of a fault. Reading a warning into
    // its presence told the user something had gone wrong when nothing had.
    void anOrdinaryDetailOnAReadyDomainIsNotAWarning() {
        const DomainStatus plain{DeviceState::Ready, {}};
        const DomainStatus informative{DeviceState::Ready, "no zones configured"};

        QCOMPARE(describe(Domain::External, informative), describe(Domain::External, plain));
        QVERIFY(describe(Domain::External, informative).find("Warning") == std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestUsbReattach)
#include "test_usb_reattach.moc"
