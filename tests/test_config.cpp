#include <QtTest>
#include <QTemporaryDir>

#include <fstream>

#include "core/config.h"

using namespace alienfx;

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeAll(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

class TestConfig : public QObject {
    Q_OBJECT

   private slots:
    void roundTripsEveryField() {
        Config config = Config::defaults();
        config.lightsOn = false;
        config.activeSchemeName = config.schemes.back().name;
        config.schemes.front().keyboard.color = Rgb{1, 2, 3};
        config.schemes.front().keyboard.brightness = Brightness::fromPercent(42);
        config.schemes.front().external.zones.front().name = "Rear ring";
        config.schemes.front().external.zones.front().color =
            ColorPair{Rgb{9, 8, 7}, Rgb{6, 5, 4}, true};

        Config parsed;
        std::string error;
        QVERIFY2(deserializeConfig(serializeConfig(config), parsed, error), error.c_str());

        QCOMPARE(parsed.lightsOn, false);
        QCOMPARE(parsed.activeSchemeName, config.activeSchemeName);
        QCOMPARE(parsed.schemes.size(), config.schemes.size());
        QCOMPARE(parsed.schemes.front().keyboard.color, (Rgb{1, 2, 3}));
        QCOMPARE(parsed.schemes.front().keyboard.brightness.percent(), 42);
        QCOMPARE(parsed.schemes.front().external.zones.front().name, std::string("Rear ring"));
        QCOMPARE(parsed.schemes.front().external.zones.front().color.ac, (Rgb{9, 8, 7}));
        QCOMPARE(parsed.schemes.front().external.zones.front().color.battery, (Rgb{6, 5, 4}));
        QVERIFY(parsed.schemes.front().external.zones.front().color.separateBatteryColor);
    }

    void aColourWithNoBatterySlotMirrorsAc() {
        Config config = Config::defaults();
        config.schemes.front().external.zones.front().color = uniform(Rgb{200, 100, 50});

        Config parsed;
        std::string error;
        QVERIFY(deserializeConfig(serializeConfig(config), parsed, error));

        const ColorPair& pair = parsed.schemes.front().external.zones.front().color;
        QVERIFY(!pair.separateBatteryColor);
        QCOMPARE(pair.effectiveBattery(), (Rgb{200, 100, 50}));
    }

    // A config nobody can parse must still leave the user with working lights.
    void anUnparseableFileFallsBackToDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        writeAll(path, "{ this is not json");

        const LoadResult result = loadConfig(path);
        QVERIFY(result.fileExisted);
        QVERIFY(!result.error.empty());
        QVERIFY(!result.config.schemes.empty());
        QVERIFY(!result.config.activeScheme().name.empty());
    }

    // Hand-editing is advertised, so a field of the wrong type is a shape a real user will
    // produce. It has to land on the fallback like every other unusable value: the invariant
    // is that a bad config degrades to working lights, never to a process that dies on the
    // login and resume path.
    void aFieldOfTheWrongTypeFallsBackInsteadOfThrowing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        const std::string text = R"({
          "schemaVersion": "1",
          "activeScheme": 3,
          "lightsOn": "yes",
          "schemes": [{"name": "x",
                       "keyboard": {"color": "#010203", "brightness": "loud"},
                       "external": {"zones": [{"id": 1, "name": 5, "color": "#040506"}]}}]
        })";
        writeAll(path, text);

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());
        QVERIFY(result.config.lightsOn);
        QCOMPARE(result.config.activeSchemeName, std::string("x"));

        const Scheme* scheme = result.config.findScheme("x");
        QVERIFY(scheme != nullptr);
        QCOMPARE(scheme->keyboard.color, (Rgb{1, 2, 3}));
        QCOMPARE(scheme->external.zones.size(), std::size_t(1));
        QCOMPARE(scheme->external.zones.front().name, defaultZoneName(1));
        QCOMPARE(scheme->external.zones.front().color.ac, (Rgb{4, 5, 6}));
    }

    // The file the app writes always carries both keys. This is the hand-edit path the
    // README advertises as safe: an "external" block that sets a brightness and leaves the
    // zone list to us must still come back at the brightness it asked for.
    void externalBrightnessSurvivesAnAbsentZonesKey() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        const std::string text = R"({
          "schemaVersion": 1,
          "activeScheme": "x",
          "schemes": [{"name": "x", "external": {"brightness": 30}}]
        })";
        writeAll(path, text);

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());

        const Scheme* scheme = result.config.findScheme("x");
        QVERIFY(scheme != nullptr);
        QCOMPARE(scheme->external.brightness.percent(), 30);
        QCOMPARE(scheme->external.zones.size(),
                 defaultExternalSettings(Rgb{0, 0, 0}).zones.size());
    }

    // Editing a scheme rewrites it in place, so the only route back to a shipped preset is
    // to delete the edited copy and have it reappear.
    void deletedBuiltInsComeBackOnTheNextLoad() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";

        Config config;
        Scheme mine = defaultScheme();
        mine.name = "Mine";
        config.schemes.push_back(mine);
        config.activeSchemeName = "Mine";

        std::string error;
        QVERIFY2(saveConfig(config, path, error), error.c_str());

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());
        QCOMPARE(result.config.activeSchemeName, std::string("Mine"));
        for (const Scheme& builtin : builtinSchemes())
            QVERIFY2(result.config.findScheme(builtin.name) != nullptr, builtin.name.c_str());
    }

    // Delete has to mean delete. The re-seed exists so an accidental colour-wheel drag can
    // never destroy a shipped preset; it must not also undo a deliberate deletion.
    void aDeletedBuiltInStaysDeletedAcrossALoad() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";

        Config config = Config::defaults();
        const std::string deleted = builtinSchemes().back().name;
        QVERIFY(config.removeScheme(deleted));

        std::string error;
        QVERIFY2(saveConfig(config, path, error), error.c_str());

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());
        QVERIFY2(result.config.findScheme(deleted) == nullptr,
                 "a scheme the user deleted came back on the next load");
        for (const Scheme& builtin : builtinSchemes())
            if (builtin.name != deleted)
                QVERIFY2(result.config.findScheme(builtin.name) != nullptr,
                         builtin.name.c_str());
    }

    // The other half of the same invariant: editing in place still cannot destroy a preset.
    void aBuiltInEditedInPlaceStillComesBackWhenItsNameIsGone() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";

        Config config = Config::defaults();
        const std::string edited = builtinSchemes().front().name;
        config.findScheme(edited)->keyboard.color = Rgb{1, 1, 1};
        QVERIFY(config.removeScheme(edited));
        config.removedBuiltins.clear();  // renamed away from, not deliberately deleted

        std::string error;
        QVERIFY2(saveConfig(config, path, error), error.c_str());
        QVERIFY(loadConfig(path).config.findScheme(edited) != nullptr);
    }

    void savingOverADeletedBuiltInNameMakesItUsableAgain() {
        Config config = Config::defaults();
        const std::string name = builtinSchemes().back().name;
        QVERIFY(config.removeScheme(name));
        QVERIFY(!config.removedBuiltins.empty());

        Scheme mine = defaultScheme();
        mine.name = name;
        mine.keyboard.color = Rgb{4, 4, 4};
        config.upsertScheme(mine);
        QVERIFY2(config.removedBuiltins.empty(),
                 "saving under the name should clear the record of its deletion");

        config.restoreMissingBuiltins();
        QCOMPARE(config.findScheme(name)->keyboard.color, (Rgb{4, 4, 4}));
    }

    void aUserSchemeSharingABuiltInNameIsNeverOverwritten() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";

        const std::string shared = builtinSchemes().back().name;
        Config config;
        Scheme mine = defaultScheme();
        mine.name = shared;
        mine.keyboard.color = Rgb{7, 7, 7};
        config.schemes.push_back(mine);
        config.activeSchemeName = shared;

        std::string error;
        QVERIFY2(saveConfig(config, path, error), error.c_str());

        const LoadResult result = loadConfig(path);
        int matches = 0;
        for (const Scheme& scheme : result.config.schemes)
            if (scheme.name == shared) ++matches;
        QCOMPARE(matches, 1);
        QCOMPARE(result.config.findScheme(shared)->keyboard.color, (Rgb{7, 7, 7}));
    }

    void aMissingFileIsNotAnError() {
        QTemporaryDir dir;
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "absent.json";

        const LoadResult result = loadConfig(path);
        QVERIFY(!result.fileExisted);
        QVERIFY(result.error.empty());
        QCOMPARE(result.config.schemes.size(), builtinSchemes().size());
    }

    // A hand-edited effect name is an input path to the keyboard's effect type. It must land
    // on a real effect, never on zero. See tests/test_effect_guard.cpp for the whole story.
    void anUnknownEffectNameFallsBackToAUsableEffect() {
        Config parsed;
        std::string error;
        const std::string text = R"({
          "schemaVersion": 1,
          "activeScheme": "x",
          "schemes": [{"name": "x", "keyboard": {"effect": "off", "color": "#ffffff"}}]
        })";
        QVERIFY(deserializeConfig(text, parsed, error));
        QCOMPARE(toWireValue(parsed.schemes.front().keyboard.effect),
                 toWireValue(kDefaultKeyboardEffect));
        QVERIFY(toWireValue(parsed.schemes.front().keyboard.effect) != 0);
    }

    void outOfRangeLightIdsAreDropped() {
        Config parsed;
        std::string error;
        const std::string text = R"({
          "schemaVersion": 1,
          "activeScheme": "x",
          "schemes": [{"name": "x", "external": {"zones": [
            {"id": 2, "color": "#112233"},
            {"id": 99, "color": "#445566"},
            {"id": -1, "color": "#778899"}
          ]}}]
        })";
        QVERIFY(deserializeConfig(text, parsed, error));
        const auto& zones = parsed.schemes.front().external.zones;
        QCOMPARE(zones.size(), std::size_t(1));
        QCOMPARE(zones.front().lightId, std::uint8_t(2));
    }

    void aNewerSchemaIsRefusedRatherThanMisread() {
        Config parsed;
        std::string error;
        const std::string text = R"({"schemaVersion": 9999, "schemes": []})";
        QVERIFY(!deserializeConfig(text, parsed, error));
        QVERIFY(!error.empty());
    }

    void savingIsAtomicAndCreatesItsDirectory() {
        QTemporaryDir dir;
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "nested" / "config.json";

        std::string error;
        QVERIFY2(saveConfig(Config::defaults(), path, error), error.c_str());
        QVERIFY(std::filesystem::exists(path));
        QVERIFY2(!std::filesystem::exists(path.string() + ".tmp"),
                 "the temporary file must not be left behind");

        Config parsed;
        QVERIFY(deserializeConfig(readAll(path), parsed, error));
        QCOMPARE(parsed.schemes.size(), builtinSchemes().size());
    }

    void aStaleActiveNameStillYieldsAPaintableScheme() {
        Config config = Config::defaults();
        config.activeSchemeName = "a scheme that was deleted";
        QVERIFY(!config.activeScheme().name.empty());
        QVERIFY(!config.activeScheme().external.zones.empty());
    }

    void removingTheActiveSchemeMovesTheSelection() {
        Config config = Config::defaults();
        const std::string first = config.schemes.front().name;
        config.activeSchemeName = first;

        QVERIFY(config.removeScheme(first));
        QVERIFY(config.activeSchemeName != first);
        QVERIFY(config.findScheme(config.activeSchemeName) != nullptr);
    }

    void colourHexRoundTrips() {
        Rgb parsed;
        QVERIFY(parseHex("#bd93f9", parsed));
        QCOMPARE(parsed, (Rgb{0xbd, 0x93, 0xf9}));
        QCOMPARE(toHex(parsed), std::string("#bd93f9"));

        QVERIFY(parseHex("00ff7f", parsed));
        QCOMPARE(parsed, (Rgb{0x00, 0xff, 0x7f}));

        QVERIFY(!parseHex("#abc", parsed));
        QVERIFY(!parseHex("#gggggg", parsed));
        QVERIFY(!parseHex("", parsed));
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
