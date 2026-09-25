#include <QtTest>

#include <optional>

#include "app/apply_mode.h"
#include "core/config.h"
#include "core/scheme.h"

using namespace alienfx;

namespace {

// An active scheme with every property the probe must ignore set to something that would
// wreck the probe if it leaked through: the external brightness at its dimmest legal value,
// and zone colours that are neither the probe's nor black.
Config awkwardConfig() {
    Config config;

    Scheme scheme;
    scheme.name = "dim";
    scheme.keyboard.color = Rgb{12, 34, 56};
    scheme.keyboard.brightness = Brightness::fromPercent(42);
    scheme.external.brightness = Brightness::fromPercent(1);
    for (std::uint8_t id = 0; id < 6; ++id)
        scheme.external.zones.push_back(
            ExternalZone{id, defaultZoneName(id), uniform(Rgb{7, 7, 7})});

    config.schemes.push_back(scheme);
    config.activeSchemeName = scheme.name;
    config.lightsOn = true;
    return config;
}

const ExternalZone* zone(const Scheme& scheme, std::uint8_t lightId) {
    for (const ExternalZone& candidate : scheme.external.zones)
        if (candidate.lightId == lightId) return &candidate;
    return nullptr;
}

}  // namespace

// The probe is how the external light IDs get mapped to physical fixtures at all, so a
// property leaking in from the active scheme does not just look wrong - it produces a wrong
// answer the user then writes down. These pin what the mode paints.
class TestProbeMode : public QObject {
    Q_OBJECT

   private slots:
    void externalBrightnessIsAlwaysFull() {
        const Scheme painted = buildProbeOutput(awkwardConfig(), std::nullopt).scheme();
        QCOMPARE(painted.external.brightness.value(), Brightness::kMax);
    }

    void everyExternalIdIsPaintedWithTheProbesOwnColours() {
        const Scheme painted = buildProbeOutput(awkwardConfig(), std::nullopt).scheme();

        QCOMPARE(painted.external.zones.size(), std::size(kDefaultExternalLightIds));
        QCOMPARE(zone(painted, 0)->color.ac, (Rgb{255, 0, 0}));
        QCOMPARE(zone(painted, 1)->color.ac, (Rgb{0, 255, 0}));
        QCOMPARE(zone(painted, 2)->color.ac, (Rgb{0, 0, 255}));
        QCOMPARE(zone(painted, 3)->color.ac, (Rgb{255, 255, 0}));
        QCOMPARE(zone(painted, 4)->color.ac, (Rgb{255, 0, 255}));
        QCOMPARE(zone(painted, 5)->color.ac, (Rgb{0, 255, 255}));
    }

    void aSingleIdProbeLightsOnlyThatId() {
        const Scheme painted = buildProbeOutput(awkwardConfig(), 3).scheme();

        QCOMPARE(painted.external.brightness.value(), Brightness::kMax);
        for (std::uint8_t id : kDefaultExternalLightIds)
            QCOMPARE(zone(painted, id)->color.ac,
                     id == 3 ? (Rgb{255, 255, 255}) : (Rgb{0, 0, 0}));
    }

    void aSingleIdOutsideTheDefaultsIsStillSentLit() {
        const Scheme painted = buildProbeOutput(awkwardConfig(), 10).scheme();

        QVERIFY(zone(painted, 10));
        QCOMPARE(zone(painted, 10)->color.ac, (Rgb{255, 255, 255}));
        for (std::uint8_t id : kDefaultExternalLightIds)
            QCOMPARE(zone(painted, id)->color.ac, (Rgb{0, 0, 0}));
    }

    // The deliberate exception. Repainting the keyboard while the user is staring at the
    // external lights changes the thing they are being asked to describe.
    void theKeyboardIsInheritedFromTheActiveScheme() {
        const Config config = awkwardConfig();
        const Scheme painted = buildProbeOutput(config, std::nullopt).scheme();
        QCOMPARE(painted.keyboard, config.activeScheme().keyboard);
    }

    void lightsOffIsStillHonoured() {
        Config config = awkwardConfig();
        config.lightsOn = false;

        const Scheme painted = buildProbeOutput(config, 3).scheme();
        QCOMPARE(painted.keyboard.color, (Rgb{0, 0, 0}));
        for (std::uint8_t id : kDefaultExternalLightIds)
            QCOMPARE(zone(painted, id)->color.ac, (Rgb{0, 0, 0}));
    }
};

QTEST_GUILESS_MAIN(TestProbeMode)
#include "test_probe_mode.moc"
