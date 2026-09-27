#include <QtTest>

#include <algorithm>
#include <memory>
#include <vector>

#include "core/device_interface.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"
#include "fake_devices.h"

// QCOMPARE on a scoped enum would pick up alienfx::toString() by ADL, which returns a
// string_view where QTest wants a const char*. Compare the underlying values instead.
static int asInt(alienfx::DeviceState state) { return static_cast<int>(state); }

using namespace alienfx;
using namespace alienfx::test;

// The engine only accepts a resolved scheme. These cases are about what reaches the wire
// rather than about the lights-off path, so they resolve with the lights on.
static ResolvedScheme lit(const Scheme& scheme) {
    return resolveOutput(LightingState{scheme, true});
}

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

        engine.apply(lit(defaultScheme()));
        QCOMPARE(keyboardView->openCount, 1);
        QVERIFY(engine.anyDeviceOpen());

        // A second apply reuses the open handle rather than thrashing the claim.
        engine.apply(lit(defaultScheme()));
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
        engine.apply(lit(defaultScheme()));

        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(1));
        const auto& ids = externalView->brightnessCalls.front().lightIds;
        const std::vector<std::uint8_t> expected = collectHardwareIds(defaultScheme().external);
        QCOMPARE(ids.size(), expected.size());
        for (std::size_t i = 0; i < ids.size(); ++i) QCOMPARE(ids[i], expected[i]);

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
        engine.apply(lit(scheme));

        // A fresh engine first drives off the visible lights the scheme leaves out (0 and 2).
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(4));
        QCOMPARE(externalView->zoneCalls[2].lightId, std::uint8_t(1));
        QCOMPARE(externalView->zoneCalls[2].acColor, (Rgb{10, 20, 30}));
        QCOMPARE(externalView->zoneCalls[2].batteryColor, (Rgb{10, 20, 30}));
        QCOMPARE(externalView->zoneCalls[3].lightId, std::uint8_t(3));
        QCOMPARE(externalView->zoneCalls[3].acColor, (Rgb{1, 2, 3}));
        QCOMPARE(externalView->zoneCalls[3].batteryColor, (Rgb{4, 5, 6}));
    }

    // There are two independent controllers. One of them failing must never stop the other
    // from lighting up - that is the whole reason they are modelled as separate domains.
    void oneDomainFailingDoesNotStopTheOther() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        keyboard->failOpen = true;
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(lit(defaultScheme()));

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
        const ApplyResult result = engine.apply(lit(defaultScheme()));

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
        const ApplyResult result = engine.apply(lit(defaultScheme()));

        QCOMPARE(asInt(result.external.state), asInt(DeviceState::Error));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(1));
        QCOMPARE(externalView->commitCount, 0);
        QVERIFY(result.keyboard.ok());
    }

    // Releasing says nothing about what the lighting did. Re-reading the device's status
    // after the release used to overwrite a success with the closed device's own status,
    // which turned a supported configuration into a reported failure and an --apply exit
    // code of 1.
    void releasingDoesNotUndoADomainsSuccess() {
        Scheme scheme = defaultScheme();
        scheme.external.zones.clear();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        const ApplyResult result = engine.apply(lit(scheme));
        QVERIFY(result.external.ok());

        engine.releaseDevices();
        QCOMPARE(externalView->openCount, 1);

        DomainStatus reported = result.external;
        reported.interfaceLeftDetached = engine.interfaceLeftDetached(Domain::External);

        QVERIFY2(reported.ok(), "a domain that succeeded stays succeeded after its release");
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
        const ApplyResult result = engine.apply(lit(defaultScheme()));
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
        QVERIFY(engine.apply(lit(scheme)).external.ok());

        // Once the lights are known to be off, applying it again is free.
        const std::size_t firstPass = externalView->zoneCalls.size();
        QVERIFY(engine.apply(lit(scheme)).external.ok());
        QCOMPARE(externalView->zoneCalls.size(), firstPass);
        QCOMPARE(externalView->openCount, 1);
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
        engine.apply(lit(scheme));
        const std::size_t firstPass = externalView->zoneCalls.size();
        QCOMPARE(firstPass, collectHardwareIds(scheme.external).size());

        // Nothing changed: nothing goes on the wire.
        engine.apply(lit(scheme));
        QCOMPARE(externalView->zoneCalls.size(), firstPass);
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));

        // One zone changed: exactly one hardware ID goes on the wire.
        scheme.external.zones[2].color = uniform(Rgb{7, 7, 7});
        engine.apply(lit(scheme));
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
        engine.apply(lit(scheme));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(1));

        // Colour only: the brightness register is left alone.
        scheme.keyboard.color = Rgb{1, 2, 3};
        engine.apply(lit(scheme));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));

        // Brightness only: the colour is not resent.
        scheme.keyboard.brightness = Brightness::fromPercent(20);
        engine.apply(lit(scheme));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(2));
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));
    }

    // The keyboard is woken by its brightness command, so it starts from scratch after every
    // release. The external controller keeps showing what it was sent: releasing its handle
    // changes nothing the lights show, so an unchanged scheme does not even reopen it.
    void releasingTheDevicesResendsTheKeyboardButNotTheExternalLights() {
        const Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        const std::size_t firstPass = externalView->zoneCalls.size();

        engine.releaseDevices();
        QVERIFY(engine.apply(lit(scheme)).allOk());

        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(2));
        QCOMPARE(externalView->openCount, 1);
        QCOMPARE(externalView->zoneCalls.size(), firstPass);
        QCOMPARE(externalView->commitCount, 1);
    }

    // When something else may have painted the hardware - the resume hook, the login unit -
    // the caller says so, and everything goes back on the wire.
    void forgettingTheAppliedStateForcesAFullResend() {
        const Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        const std::size_t firstPass = externalView->zoneCalls.size();

        engine.releaseDevices();
        engine.forgetAppliedState();
        engine.apply(lit(scheme));

        QCOMPARE(externalView->zoneCalls.size(), firstPass * 2);
        QCOMPARE(externalView->openCount, 2);
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));
        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(2));
    }

    // A controller found closed without having been released is not known to be showing
    // anything in particular, so the next apply sends the lot.
    void aControllerClosedUnderTheEngineForcesAFullResend() {
        const Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        const std::size_t firstPass = externalView->zoneCalls.size();

        externalView->close();
        Scheme changed = scheme;
        changed.external.zones.front().color = uniform(Rgb{9, 9, 9});
        engine.apply(lit(changed));

        QCOMPARE(externalView->openCount, 2);
        QCOMPARE(externalView->zoneCalls.size(), firstPass * 2);
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
        engine.apply(lit(scheme));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(1));

        externalView->failZone = false;
        engine.apply(lit(scheme));
        QCOMPARE(externalView->zoneCalls.size(),
                 1 + collectHardwareIds(scheme.external).size());
    }

    // A renamed zone is the same light. Renaming must not cost a USB round trip.
    void renamingAZoneSendsNothing() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        const std::size_t firstPass = externalView->zoneCalls.size();

        scheme.external.zones.front().name = "Custom name";
        engine.apply(lit(scheme));
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
        engine.apply(lit(scheme));
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(1));

        scheme.external.zones.pop_back();
        engine.apply(lit(scheme));
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(2));
        QCOMPARE(externalView->brightnessCalls.back().lightIds.size(),
                 collectHardwareIds(scheme.external).size());
    }

    void missingBackendsDegradeInsteadOfCrashing() {
        LightingEngine engine(nullptr, nullptr);
        const ApplyResult result = engine.apply(lit(defaultScheme()));

        QCOMPARE(asInt(result.keyboard.state), asInt(DeviceState::Missing));
        QCOMPARE(asInt(result.external.state), asInt(DeviceState::Missing));
        QVERIFY(!engine.anyDeviceOpen());
        engine.releaseDevices();  // must not crash
    }

    // Each logical zone maps to exactly one hardware light ID.
    void eachZoneDrivesOneHardwareId() {
        Scheme scheme;
        scheme.name = "test";
        scheme.external.zones.push_back(
            ExternalZone{2, "Interior logo", uniform(Rgb{255, 128, 0})});

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));

        // The three visible lights the scheme leaves out go black first, then the one it has.
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(4));
        for (std::size_t i = 0; i < 3; ++i) {
            QVERIFY(externalView->zoneCalls[i].lightId != 2);
            QCOMPARE(externalView->zoneCalls[i].acColor, (Rgb{0, 0, 0}));
        }
        QCOMPARE(externalView->zoneCalls.back().lightId, std::uint8_t(2));
        QCOMPARE(externalView->zoneCalls.back().acColor, (Rgb{255, 128, 0}));
    }

    // Zones 4 and 5 are not in the default scheme because they light nothing.
    void defaultSchemeHasNoInvisibleZones() {
        const Scheme scheme = defaultScheme();
        for (const ExternalZone& zone : scheme.external.zones) {
            QVERIFY2(zone.lightId != 4 && zone.lightId != 5,
                     "zones 4 and 5 must not appear in the default scheme");
        }
        QCOMPARE(scheme.external.zones.size(), std::size_t(4));
    }

    // Switching schemes with completely different per-zone colours must update every
    // hardware light and must not be skipped by the delta-optimisation.
    void switchingSchemesSendsAllChangedZones() {
        Scheme schemeA;
        schemeA.name = "A";
        schemeA.external.zones.push_back(
            ExternalZone{0, "Rear ring 1", uniform(Rgb{255, 0, 0})});
        schemeA.external.zones.push_back(
            ExternalZone{1, "Rear ring 2", uniform(Rgb{0, 255, 0})});
        schemeA.external.zones.push_back(
            ExternalZone{2, "Interior logo", uniform(Rgb{0, 0, 255})});

        Scheme schemeB;
        schemeB.name = "B";
        schemeB.external.zones.push_back(
            ExternalZone{0, "Rear ring 1", uniform(Rgb{128, 128, 128})});
        schemeB.external.zones.push_back(
            ExternalZone{1, "Rear ring 2", uniform(Rgb{64, 64, 64})});
        schemeB.external.zones.push_back(
            ExternalZone{2, "Interior logo", uniform(Rgb{255, 255, 255})});

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(schemeA));
        const std::size_t afterA = externalView->zoneCalls.size();
        // Scheme A's three lights, plus light 3 driven off because a fresh engine cannot
        // know it is dark.
        QCOMPARE(afterA, collectHardwareIds(schemeA.external).size() + 1);

        // All three zones have different colours between A and B -- every one should be sent.
        engine.apply(lit(schemeB));
        QCOMPARE(externalView->zoneCalls.size(), afterA + collectHardwareIds(schemeB.external).size());
    }

    // Scheme switching where a zone happens to carry the same colour as before must
    // skip that zone, not re-send it.
    void unchangedZonesAcrossSchemesAreSkipped() {
        Scheme schemeA;
        schemeA.name = "A";
        schemeA.external.zones.push_back(
            ExternalZone{0, "Rear ring 1", uniform(Rgb{100, 100, 100})});
        schemeA.external.zones.push_back(
            ExternalZone{2, "Interior logo", uniform(Rgb{0, 0, 255})});

        Scheme schemeB;
        schemeB.name = "B";
        // Same colour as scheme A for this zone -- should be skipped.
        schemeB.external.zones.push_back(
            ExternalZone{0, "Rear ring 1", uniform(Rgb{100, 100, 100})});
        // Different colour -- should be sent.
        schemeB.external.zones.push_back(
            ExternalZone{2, "Interior logo", uniform(Rgb{255, 0, 0})});

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(schemeA));
        const std::size_t afterA = externalView->zoneCalls.size();

        engine.apply(lit(schemeB));
        // Only one changed zone (lightId 2) goes on the wire.
        QCOMPARE(externalView->zoneCalls.size(), afterA + 1);
        QCOMPARE(externalView->zoneCalls.back().lightId, std::uint8_t(2));
        QCOMPARE(externalView->zoneCalls.back().acColor, (Rgb{255, 0, 0}));
    }

    // The destructor is the backstop for the releaseDevices() calls every shutdown path
    // already makes: a destroyed engine must not leak a claimed interface. On this hardware
    // a leaked claim is a keyboard with no kernel driver until reboot.
    void destructionReleasesTheDevices() {
        // The engine owns the devices, so the counters have to live outside both.
        auto closes = std::make_shared<std::pair<int, int>>(0, 0);

        class CountingKeyboard final : public IKeyboardDevice {
           public:
            explicit CountingKeyboard(std::shared_ptr<std::pair<int, int>> closes)
                : closes_(std::move(closes)) {}
            bool open() override {
                open_ = true;
                return true;
            }
            void close() override {
                if (open_) ++closes_->first;
                open_ = false;
            }
            [[nodiscard]] bool isOpen() const override { return open_; }
            bool setBrightness(Brightness) override { return true; }
            bool applyEffect(KeyboardEffect, Rgb) override { return true; }
            [[nodiscard]] DomainStatus status() const override {
                return DomainStatus{DeviceState::Ready, {}};
            }

           private:
            std::shared_ptr<std::pair<int, int>> closes_;
            bool open_ = false;
        };
        class CountingExternal final : public IExternalDevice {
           public:
            explicit CountingExternal(std::shared_ptr<std::pair<int, int>> closes)
                : closes_(std::move(closes)) {}
            bool open() override {
                open_ = true;
                return true;
            }
            void close() override {
                if (open_) ++closes_->second;
                open_ = false;
            }
            [[nodiscard]] bool isOpen() const override { return open_; }
            bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override {
                return true;
            }
            bool applyZone(std::uint8_t, Rgb, Rgb) override { return true; }
            bool commit() override { return true; }
            [[nodiscard]] DomainStatus status() const override {
                return DomainStatus{DeviceState::Ready, {}};
            }

           private:
            std::shared_ptr<std::pair<int, int>> closes_;
            bool open_ = false;
        };

        {
            LightingEngine engine(std::make_unique<CountingKeyboard>(closes),
                                  std::make_unique<CountingExternal>(closes));
            engine.apply(lit(defaultScheme()));
            QVERIFY(engine.anyDeviceOpen());
        }

        QCOMPARE(closes->first, 1);
        QCOMPARE(closes->second, 1);
    }

    // An explicit empty zone list means "no external lighting". Zones a previous scheme lit
    // have to be driven off before they are forgotten - a black pair, because there is no
    // off command for these lights, only colours.
    void removingAllZonesDrivesThemOff() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(defaultScheme()));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(4));

        Scheme empty = defaultScheme();
        empty.external.zones.clear();
        engine.apply(lit(empty));

        // Every previously-lit zone got a black pair, followed by one commit.
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(8));
        for (std::size_t i = 4; i < 8; ++i) {
            QCOMPARE(externalView->zoneCalls[i].acColor, (Rgb{0, 0, 0}));
            QCOMPARE(externalView->zoneCalls[i].batteryColor, (Rgb{0, 0, 0}));
        }
        QCOMPARE(externalView->commitCount, 2);
    }

    // The same rule for a set that shrinks without emptying: zones the new scheme no longer
    // carries go off, zones it keeps are not touched again.
    void shrinkingTheZoneSetDrivesOnlyRemovedZonesOff() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(4));

        const std::uint8_t removedId = scheme.external.zones.back().lightId;
        scheme.external.zones.pop_back();  // colours of the survivors unchanged
        engine.apply(lit(scheme));

        QCOMPARE(externalView->zoneCalls.size(), std::size_t(5));
        QCOMPARE(externalView->zoneCalls.back().lightId, removedId);
        QCOMPARE(externalView->zoneCalls.back().acColor, (Rgb{0, 0, 0}));
        // The zone set changed, so the brightness/on command is resent with the smaller list.
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(2));
        QCOMPARE(externalView->brightnessCalls.back().lightIds.size(), std::size_t(3));
        QCOMPARE(externalView->commitCount, 2);
    }

    // A fresh engine cannot know what an earlier session left lit, so a first apply with no
    // zones drives every visible light off rather than trusting the controller to be dark.
    void emptyZonesOnAFreshEngineDriveEveryVisibleLightOff() {
        Scheme noZones = defaultScheme();
        noZones.external.zones.clear();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        QVERIFY(engine.apply(lit(noZones)).external.ok());
        QCOMPARE(externalView->zoneCalls.size(), std::size(kVisibleHardwareIds));
        for (std::size_t i = 0; i < std::size(kVisibleHardwareIds); ++i) {
            QCOMPARE(externalView->zoneCalls[i].lightId, kVisibleHardwareIds[i]);
            QCOMPARE(externalView->zoneCalls[i].acColor, (Rgb{0, 0, 0}));
            QCOMPARE(externalView->zoneCalls[i].batteryColor, (Rgb{0, 0, 0}));
        }
        QCOMPARE(externalView->commitCount, 1);

        // Switching to a zoned scheme paints it...
        engine.apply(lit(defaultScheme()));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(8));
        // ...switching back to none drives those zones off...
        engine.apply(lit(noZones));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(12));
        // ...and staying on none sends nothing further.
        engine.apply(lit(noZones));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(12));
        QCOMPARE(externalView->openCount, 1);
    }

    // The tray releases the external controller after an idle timeout. What the lights show
    // does not change with the handle, so a scheme switch after that clears exactly the zones
    // the new scheme drops and leaves the survivors alone.
    void shrinkingTheZoneSetAfterAnIdleReleaseDrivesOnlyTheRemovedZonesOff() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(4));

        engine.releaseDevice(Domain::External);

        const std::uint8_t removedId = scheme.external.zones.back().lightId;
        scheme.external.zones.pop_back();
        QVERIFY(engine.apply(lit(scheme)).external.ok());
        QCOMPARE(externalView->openCount, 2);

        QCOMPARE(externalView->zoneCalls.size(), std::size_t(5));
        QCOMPARE(externalView->zoneCalls[4].lightId, removedId);
        QCOMPARE(externalView->zoneCalls[4].acColor, (Rgb{0, 0, 0}));
        QCOMPARE(externalView->zoneCalls[4].batteryColor, (Rgb{0, 0, 0}));
        QCOMPARE(externalView->brightnessCalls.back().lightIds.size(), std::size_t(3));
        QCOMPARE(externalView->commitCount, 2);
    }

    // The same after an idle release when the new scheme has no zones at all: the early
    // "nothing to do" exit must not fire while the lights are still showing the old scheme.
    void emptyingTheZoneSetAfterAnIdleReleaseDrivesEveryLitZoneOff() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(defaultScheme()));
        QCOMPARE(externalView->zoneCalls.size(), std::size_t(4));

        engine.releaseDevice(Domain::External);

        Scheme empty = defaultScheme();
        empty.external.zones.clear();
        QVERIFY(engine.apply(lit(empty)).external.ok());
        QCOMPARE(externalView->openCount, 2);

        const std::vector<std::uint8_t> litIds = collectHardwareIds(defaultScheme().external);
        QCOMPARE(externalView->zoneCalls.size(), 4 + litIds.size());
        for (std::size_t i = 0; i < litIds.size(); ++i) {
            QCOMPARE(externalView->zoneCalls[4 + i].lightId, litIds[i]);
            QCOMPARE(externalView->zoneCalls[4 + i].acColor, (Rgb{0, 0, 0}));
            QCOMPARE(externalView->zoneCalls[4 + i].batteryColor, (Rgb{0, 0, 0}));
        }
        QCOMPARE(externalView->commitCount, 2);
    }

    // Every keyboard colour drag applies both domains. Once the external lights are known to
    // be dark, an idle release must not turn each later drag into a reopen and a rewrite of
    // the same blacks.
    void anIdleReleaseThenAKeyboardEditSendsNothingExternal() {
        Scheme scheme = defaultScheme();
        scheme.external.zones.clear();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        QVERIFY(engine.apply(lit(scheme)).external.ok());
        const std::size_t blacks = externalView->zoneCalls.size();

        engine.releaseDevice(Domain::External);

        scheme.keyboard.color = Rgb{40, 50, 60};
        const ApplyResult result = engine.apply(lit(scheme));
        QVERIFY(result.allOk());
        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(2));
        QCOMPARE(externalView->openCount, 1);
        QCOMPARE(externalView->zoneCalls.size(), blacks);
        QCOMPARE(externalView->commitCount, 1);
        QVERIFY(!engine.isDeviceOpen(Domain::External));

        // The same holds for a zoned scheme: an unchanged external half is left alone.
        Scheme zoned = defaultScheme();
        engine.apply(lit(zoned));
        const std::size_t painted = externalView->zoneCalls.size();
        const int opens = externalView->openCount;
        engine.releaseDevice(Domain::External);
        zoned.keyboard.color = Rgb{70, 80, 90};
        QVERIFY(engine.apply(lit(zoned)).allOk());
        QCOMPARE(externalView->openCount, opens);
        QCOMPARE(externalView->zoneCalls.size(), painted);
    }

    // A refused command leaves what the lights show unknown, so a later scheme that drops
    // zones drives off every visible light it does not carry, not only the ones last sent.
    void aFailedCommandThenAShrunkSchemeDrivesEveryMissingVisibleZoneOff() {
        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(defaultScheme()));
        engine.releaseDevice(Domain::External);

        externalView->failZone = true;
        Scheme changed = defaultScheme();
        changed.external.zones.front().color = uniform(Rgb{5, 5, 5});
        QVERIFY(!engine.apply(lit(changed)).external.ok());
        externalView->failZone = false;

        Scheme single = defaultScheme();
        single.external.zones.resize(1);
        const std::size_t before = externalView->zoneCalls.size();
        QVERIFY(engine.apply(lit(single)).external.ok());

        const std::vector<std::uint8_t> kept = collectHardwareIds(single.external);
        std::vector<std::uint8_t> blacked;
        for (std::size_t i = before; i < externalView->zoneCalls.size(); ++i)
            if (externalView->zoneCalls[i].acColor == Rgb{0, 0, 0} &&
                externalView->zoneCalls[i].batteryColor == Rgb{0, 0, 0})
                blacked.push_back(externalView->zoneCalls[i].lightId);
        std::vector<std::uint8_t> expected;
        for (std::uint8_t id : kVisibleHardwareIds)
            if (std::find(kept.begin(), kept.end(), id) == kept.end()) expected.push_back(id);
        QCOMPARE(blacked, expected);
        QCOMPARE(externalView->zoneCalls.size(), before + expected.size() + kept.size());
    }

    // Zone identity is the light ID. Reordering the list paints the same lights in a
    // different order, so nothing at all goes back on the wire.
    void reorderingZonesResendsNothing() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(lit(scheme));
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(1));
        const std::size_t firstPass = externalView->zoneCalls.size();

        std::reverse(scheme.external.zones.begin(), scheme.external.zones.end());
        engine.apply(lit(scheme));
        QCOMPARE(externalView->zoneCalls.size(), firstPass);
        QCOMPARE(externalView->brightnessCalls.size(), std::size_t(1));
        QCOMPARE(externalView->commitCount, 1);
    }
};

QTEST_GUILESS_MAIN(TestLightingEngine)
#include "test_lighting_engine.moc"
