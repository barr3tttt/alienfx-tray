#include <QtTest>

#include <memory>

#include "core/device_interface.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"
#include "fake_devices.h"

// QCOMPARE on a scoped enum would pick up alienfx::toString() by ADL, which returns a
// string_view where QTest wants a const char*. Compare the underlying values instead.
static int asInt(alienfx::DeviceState state) { return static_cast<int>(state); }

using namespace alienfx;
using namespace alienfx::test;

class TestLightingEngine : public QObject {
    Q_OBJECT

   private slots:
    void opensLazilyAndReleasesOnDemand() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        QCOMPARE(keyboardView->openCount, 0);

        engine.apply(defaultScheme());
        QCOMPARE(keyboardView->openCount, 1);
        QVERIFY(engine.anyDeviceOpen());

        // A second apply reuses the open handle rather than thrashing the claim.
        engine.apply(defaultScheme());
        QCOMPARE(keyboardView->openCount, 1);

        engine.releaseDevices();
        QCOMPARE(keyboardView->closeCount, 1);
        QCOMPARE(externalView->closeCount, 1);
        QVERIFY(!engine.anyDeviceOpen());
    }

    // Brightness has to come first: on APIv4 the brightness command is also what turns the
    // listed lights on, and on APIv5 it is what wakes the controller.
    void brightnessPrecedesColour() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(defaultScheme());

        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(1));
        const auto& ids = externalView->brightnessCalls.front().lightIds;
        QCOMPARE(ids.size(), std::size(kDefaultExternalLightIds));
        for (std::size_t i = 0; i < ids.size(); ++i) QCOMPARE(ids[i], kDefaultExternalLightIds[i]);

        QCOMPARE(externalView->commitCount, 1);
    }

    // The AC/battery split is a property of the APIv4 power lights, so both slots must
    // always be sent - matched when there is no separate battery colour, distinct when there is.
    void batteryColourMirrorsAcUnlessSeparate() {
        Scheme scheme = defaultScheme();
        scheme.external.zones.resize(2);
        scheme.external.zones[0].lightId = 1;
        scheme.external.zones[0].color = uniform(Rgb{10, 20, 30});
        scheme.external.zones[1].lightId = 3;
        scheme.external.zones[1].color = ColorPair{Rgb{1, 2, 3}, Rgb{4, 5, 6}, true};

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);

        QCOMPARE(externalView->zoneCalls.size(), std::size_t(2));
        QCOMPARE(externalView->zoneCalls[0].lightId, std::uint8_t(1));
        QCOMPARE(externalView->zoneCalls[0].acColor, (Rgb{10, 20, 30}));
        QCOMPARE(externalView->zoneCalls[0].batteryColor, (Rgb{10, 20, 30}));
        QCOMPARE(externalView->zoneCalls[1].lightId, std::uint8_t(3));
        QCOMPARE(externalView->zoneCalls[1].acColor, (Rgb{1, 2, 3}));
        QCOMPARE(externalView->zoneCalls[1].batteryColor, (Rgb{4, 5, 6}));
    }

    // There are two independent controllers. One of them failing must never stop the other
    // from lighting up - that is the whole reason they are modelled as separate domains.
    void oneDomainFailingDoesNotStopTheOther() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        keyboard->failOpen = true;
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(defaultScheme());

        QCOMPARE(asInt(result.keyboard.state), asInt(DeviceState::Missing));
        QVERIFY(result.external.ok());
        QVERIFY(!result.allOk());
        QVERIFY(result.anyOk());
        QVERIFY(!externalView->zoneCalls.empty());
    }

    void busyExternalControllerIsReportedNotSwallowed() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        external->failOpen = true;

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(defaultScheme());

        QCOMPARE(asInt(result.external.state), asInt(DeviceState::Busy));
        QVERIFY(result.keyboard.ok());
        QVERIFY(!describe(Domain::External, result.external).empty());
    }

    void aZoneRefusalStopsThatDomainAndIsReported() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        external->failZone = true;
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(defaultScheme());

        QCOMPARE(asInt(result.external.state), asInt(DeviceState::Error));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(1));
        QCOMPARE(externalView->commitCount, 0);
        QVERIFY(result.keyboard.ok());
    }

    // A scheme with no zones never opens the external controller, so releasing it has
    // nothing to report. Re-reading the device's status after the release used to overwrite
    // the success with the never-opened Unknown, which turned a supported configuration
    // into a reported failure and an --apply exit code of 1.
    void releasingAnUnopenedDomainDoesNotUndoItsSuccess() {
        Scheme scheme = defaultScheme();
        scheme.external.zones.clear();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(scheme);
        QVERIFY(result.external.ok());

        engine.releaseDevices();
        QCOMPARE(externalView->openCount, 0);

        DomainStatus reported = result.external;
        reported.interfaceLeftDetached = engine.interfaceLeftDetached(Domain::External);

        QVERIFY2(reported.ok(), "a domain that never had to be opened still succeeded");
        QVERIFY(!reported.interfaceLeftDetached);
        QCOMPARE(describe(Domain::External, reported),
                 describe(Domain::External, DomainStatus{DeviceState::Ready, {}}));
        QVERIFY2(describe(Domain::External, reported).find("Warning") == std::string::npos,
                 "a supported configuration must not be reported as a fault");
    }

    // The other half: a release that really did leave the interface driverless still has to
    // reach the user, and must not be mistaken for a lighting failure.
    void aFailedReleaseSurfacesWithoutDemotingTheDomain() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        keyboard->failReattachOnClose = true;
        auto external = std::make_unique<FakeExternalDevice>();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(defaultScheme());
        QVERIFY(result.keyboard.ok());
        QVERIFY2(!result.keyboard.interfaceLeftDetached, "nothing has been released yet");

        engine.releaseDevices();

        DomainStatus reported = result.keyboard;
        reported.interfaceLeftDetached = engine.interfaceLeftDetached(Domain::Keyboard);

        QVERIFY2(reported.ok(), "the lighting worked; only the release did not");
        QVERIFY(reported.interfaceLeftDetached);
        QVERIFY(describe(Domain::Keyboard, reported).find("Warning") != std::string::npos);
    }

    void aSchemeWithNoZonesIsNotAnError() {
        Scheme scheme = defaultScheme();
        scheme.external.zones.clear();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(scheme);

        QVERIFY(result.external.ok());
        QCOMPARE(externalView->openCount, 0);
    }

    // Live preview is only usable because an unchanged zone costs nothing. One external
    // zone is about 175 ms of USB traffic on this hardware; six of them per wheel frame
    // would put the lights a second behind the cursor.
    void onlyChangedZonesAreResent() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);
        const std::size_t firstPass = externalView->zoneCalls.size();
        QCOMPARE(firstPass, scheme.external.zones.size());

        // Nothing changed: nothing goes on the wire.
        engine.apply(scheme);
        QCOMPARE(externalView->zoneCalls.size(), firstPass);
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));

        // One zone changed: exactly one zone goes on the wire.
        scheme.external.zones[2].color = uniform(Rgb{7, 7, 7});
        engine.apply(scheme);
        QCOMPARE(externalView->zoneCalls.size(), firstPass + 1);
        QCOMPARE(externalView->zoneCalls.back().lightId, scheme.external.zones[2].lightId);
        QCOMPARE(externalView->zoneCalls.back().acColor, (Rgb{7, 7, 7}));
    }

    void onlyChangedKeyboardFieldsAreResent() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(1));

        // Colour only: the brightness register is left alone.
        scheme.keyboard.color = Rgb{1, 2, 3};
        engine.apply(scheme);
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));

        // Brightness only: the colour is not resent.
        scheme.keyboard.brightness = Brightness::fromPercent(20);
        engine.apply(scheme);
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(2));
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));
    }

    // Everything we think we know about the hardware's state is only true while we hold the
    // handle. After a release - the idle timeout, a suspend, a scheme applied by the resume
    // hook - the next apply has to send the lot.
    void releasingTheDevicesForcesAFullResend() {
        const Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);
        const std::size_t firstPass = externalView->zoneCalls.size();

        engine.releaseDevices();
        engine.apply(scheme);

        QCOMPARE(externalView->zoneCalls.size(), firstPass * 2);
        QCOMPARE(externalView->openCount, 2);
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(2));
    }

    // A refused command means the controller is in an unknown state. Carrying on with a
    // stale idea of what it is showing would leave a zone permanently the wrong colour.
    void aFailureForcesAFullResendNextTime() {
        const Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();
        externalView->failZone = true;

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(1));

        externalView->failZone = false;
        engine.apply(scheme);
        QCOMPARE(externalView->zoneCalls.size(), 1 + scheme.external.zones.size());
    }

    // A renamed zone is the same light. Renaming must not cost a USB round trip.
    void renamingAZoneSendsNothing() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);
        const std::size_t firstPass = externalView->zoneCalls.size();

        scheme.external.zones.front().name = "Rear ring";
        engine.apply(scheme);
        QCOMPARE(externalView->zoneCalls.size(), firstPass);
    }

    // Changing which lights are in the scheme has to resend the brightness command, because
    // on APIv4 that command is also what turns the listed lights on.
    void changingTheZoneSetResendsBrightness() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(scheme);
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(1));

        scheme.external.zones.pop_back();
        engine.apply(scheme);
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(2));
        QCOMPARE(externalView->brightnessCalls.back().lightIds.size(),
                 scheme.external.zones.size());
    }

    void missingBackendsDegradeInsteadOfCrashing() {
        LightingEngine engine(nullptr, nullptr);
        const ApplyResult result = engine.apply(defaultScheme());

        QCOMPARE(asInt(result.keyboard.state), asInt(DeviceState::Missing));
        QCOMPARE(asInt(result.external.state), asInt(DeviceState::Missing));
        QVERIFY(!engine.anyDeviceOpen());
        engine.releaseDevices();  // must not crash
    }
};

QTEST_GUILESS_MAIN(TestLightingEngine)
#include "test_lighting_engine.moc"
