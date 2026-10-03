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

    // The first save after a fallback replaces the file, so the unreadable original has to
    // be kept before that can happen - a file a human just broke is the one they want back.
    // It is kept beside the .bak, never over it: that may hold the pre-migration original.
    void anUnparseableFileIsKeptBeforeDefaultsTakeOver() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path root(dir.path().toStdString());
        const std::filesystem::path path = root / "config.json";
        const std::string original = "the pre-migration original";
        writeAll(path.string() + ".bak", original);
        const std::string broken = "{ this is not json";
        writeAll(path, broken);

        const LoadResult result = loadConfig(path);
        QVERIFY(result.fileExisted);
        QVERIFY(!result.error.empty());
        QCOMPARE(readAll(path.string() + ".bak"), original);

        std::vector<std::filesystem::path> kept;
        for (const auto& entry : std::filesystem::directory_iterator(root))
            if (entry.path().filename().string().starts_with("config.json.corrupt-"))
                kept.push_back(entry.path());
        QCOMPARE(kept.size(), std::size_t(1));
        QCOMPARE(readAll(kept.front()), broken);
        QVERIFY2(result.error.find(kept.front().string()) != std::string::npos,
                 result.error.c_str());

        // Login and resume load the file again; the same broken text is not kept twice.
        (void)loadConfig(path);
        std::size_t copies = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root))
            if (entry.path().filename().string().starts_with("config.json.corrupt-")) ++copies;
        QCOMPARE(copies, std::size_t(1));
    }

    void theCorruptCopyIsNamedForTheUtcTimeOfTheLoad() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";

        // 2026-09-26 12:34:56 UTC.
        const auto when = std::chrono::system_clock::from_time_t(1790426096);
        const auto kept = preserveCorruptConfig(path, "{", when);
        QVERIFY(kept);
        QCOMPARE(kept->filename().string(), std::string("config.json.corrupt-20260926T123456Z"));
        QCOMPARE(readAll(*kept), std::string("{"));
    }

    // If the copy cannot be written, the next save will replace the only copy of the file,
    // so the load has to say so rather than fall back quietly.
    void aFailedCorruptCopyIsReported() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path root(dir.path().toStdString());
        const std::filesystem::path path = root / "config.json";
        writeAll(path, "{ this is not json");

        std::filesystem::permissions(root, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_exec);
        const bool writable = static_cast<bool>(std::ofstream(root / "probe"));
        if (writable) {
            std::filesystem::permissions(root, std::filesystem::perms::owner_all);
            QSKIP("directory permissions are not enforced for this user");
        }
        const LoadResult result = loadConfig(path);
        std::filesystem::permissions(root, std::filesystem::perms::owner_all);

        QVERIFY(result.fileExisted);
        QVERIFY2(result.error.find("failed") != std::string::npos, result.error.c_str());
        QVERIFY2(result.error.find("next save will replace") != std::string::npos,
                 result.error.c_str());
    }

    // A light ID is a zone's identity, so a repeat is dropped at load at every schema - a
    // current-schema file is never migrated, and hand-edits happen at any version.
    void duplicateZonesAreDroppedAtEverySchema() {
        const std::string head = R"({"schemaVersion": )";
        const std::string tail = R"(,
          "activeScheme": "test",
          "schemes": [{"name": "test", "external": {"zones": [
            {"id": 0, "name": "Rear ring 1", "color": "#0a141e"},
            {"id": 1, "name": "Rear ring 2", "color": "#28323c"},
            {"id": 0, "name": "hand-added duplicate", "color": "#010203"}
          ]}}]
        })";
        for (const char* version : {"2", "3"}) {
            const std::string text = head + version + tail;
            Config parsed;
            std::string error;
            QVERIFY2(deserializeConfig(text, parsed, error), error.c_str());
            const auto& zones = parsed.findScheme("test")->external.zones;
            QCOMPARE(zones.size(), std::size_t(2));
            QCOMPARE(zones[0].lightId, std::uint8_t(0));
            QCOMPARE(zones[0].color.ac, (Rgb{10, 20, 30}));
            QCOMPARE(zones[1].lightId, std::uint8_t(1));
        }
    }

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
        QCOMPARE(result.config.activeSchemeName, std::string(kDefaultSchemeName));

        const Scheme* scheme = result.config.findScheme("x");
        QVERIFY(scheme != nullptr);
        QCOMPARE(scheme->keyboard.color, (Rgb{1, 2, 3}));
        QCOMPARE(scheme->external.zones.size(), std::size_t(2));
        QCOMPARE(scheme->external.zones[0].lightId, std::uint8_t(0));
        QCOMPARE(scheme->external.zones[1].lightId, std::uint8_t(1));
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
        const auto entries = std::distance(std::filesystem::directory_iterator(path.parent_path()),
                                           std::filesystem::directory_iterator());
        QVERIFY2(entries == 1, "the temporary file must not be left behind");

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

    // The standing rule for Delete: the selection returns to the default scheme - Dracula -
    // rather than to whatever happens to sort first in the file.
    void removingTheActiveSchemeFallsBackToTheDefault() {
        Config config = Config::defaults();
        // Put a user scheme ahead of the default so "first" and "default" are different
        // schemes and the assertion can tell them apart.
        Scheme mine = defaultScheme();
        mine.name = "Mine";
        config.schemes.insert(config.schemes.begin(), mine);
        config.activeSchemeName = "Mine";

        QVERIFY(config.removeScheme("Mine"));
        QCOMPARE(config.activeSchemeName, std::string(kDefaultSchemeName));
    }

    // The one exception: when the default itself is the scheme being deleted, it cannot be
    // the fallback, and the first remaining scheme takes over.
    void removingTheActiveDefaultItselfFallsBackToTheFirstRemaining() {
        Config config = Config::defaults();
        config.activeSchemeName = std::string(kDefaultSchemeName);

        QVERIFY(config.removeScheme(std::string(kDefaultSchemeName)));
        QVERIFY(!config.activeSchemeName.empty());
        QCOMPARE(config.activeSchemeName, config.schemes.front().name);
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

    // Each solid preset paints the keyboard and every default external zone one colour.
    void solidPresetsCoverKeyboardAndEveryZone() {
        const Config config = Config::defaults();
        for (const char* name : {"Steady blue", "Solid red", "Solid green", "Solid orange",
                                 "Solid cyan", "Solid pink", "Solid white"}) {
            const Scheme* scheme = config.findScheme(name);
            QVERIFY2(scheme != nullptr, name);
            QCOMPARE(scheme->external.zones.size(), std::size(kDefaultExternalLightIds));
            for (const ExternalZone& zone : scheme->external.zones)
                QCOMPARE(zone.color, uniform(scheme->keyboard.color));
        }
        QCOMPARE(config.findScheme("Solid white")->keyboard.color, (Rgb{255, 255, 255}));
        QCOMPARE(config.findScheme("Solid white")->keyboard.brightness,
                 config.findScheme("Solid red")->keyboard.brightness);
    }

    // An active name that matches no scheme falls back to the default scheme and is
    // recorded as active, so the tray checkmark and the scheme list agree with what is
    // painted.
    void aStaleActiveSchemeNameIsRepairedOnLoad() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        Config config = Config::defaults();
        config.activeSchemeName = "Gone";
        std::string error;
        QVERIFY2(saveConfig(config, path, error), error.c_str());

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());
        QCOMPARE(result.config.activeSchemeName, std::string(kDefaultSchemeName));
        QCOMPARE(result.config.activeScheme().name, result.config.activeSchemeName);
    }

    // A file that lacks the default without having deleted it gets the default re-seeded on
    // load, and a missing or stale selection lands on it rather than on the first scheme.
    void aStaleSelectionLandsOnTheReseededDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        for (const char* active : {"Gone", ""}) {
            Config config;
            Scheme mine = defaultScheme();
            mine.name = "Mine";
            config.schemes.push_back(mine);
            config.activeSchemeName = active;
            std::string error;
            QVERIFY2(saveConfig(config, path, error), error.c_str());

            const LoadResult result = loadConfig(path);
            QVERIFY(result.error.empty());
            QVERIFY(result.config.findScheme(std::string(kDefaultSchemeName)) != nullptr);
            QCOMPARE(result.config.activeSchemeName, std::string(kDefaultSchemeName));
        }
    }

    // A deliberately deleted default stays deleted, so a stale selection falls to the first
    // remaining scheme.
    void aStaleSelectionWithTheDefaultDeletedLandsOnTheFirstScheme() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        Config config = Config::defaults();
        QVERIFY(config.removeScheme(std::string(kDefaultSchemeName)));
        config.activeSchemeName = "Gone";
        std::string error;
        QVERIFY2(saveConfig(config, path, error), error.c_str());

        const LoadResult result = loadConfig(path);
        QVERIFY(result.config.findScheme(std::string(kDefaultSchemeName)) == nullptr);
        QCOMPARE(result.config.activeSchemeName, result.config.schemes.front().name);
    }

    // The stale-name fallback prefers the default scheme even when it is not the first
    // scheme in the file.
    void aStaleActiveSchemeNamePrefersTheDefaultOverTheFirstScheme() {
        Config config;
        Scheme mine = defaultScheme();
        mine.name = "Mine";
        config.schemes.push_back(mine);
        config.schemes.push_back(defaultScheme());
        config.activeSchemeName = "Gone";

        QCOMPARE(config.activeScheme().name, std::string(kDefaultSchemeName));
        QCOMPARE(config.fallbackSchemeName(), std::string(kDefaultSchemeName));
    }

    // --- migration tests ---

    // A v1 config with separate zone 0 and 1 entries keeps them as separate zones after
    // migration, each with its own colour; both are renamed to current defaults.
    void migrationKeepsSeparateRingSegments() {
        Config config;
        config.schemaVersion = 1;
        Scheme s;
        s.name = "test";
        s.external.zones.push_back(
            ExternalZone{0, "Rear ring (zone 0)", uniform(Rgb{10, 20, 30})});
        s.external.zones.push_back(
            ExternalZone{1, "Rear ring (zone 1)", uniform(Rgb{40, 50, 60})});
        s.external.zones.push_back(ExternalZone{2, "Interior logo", uniform(Rgb{0, 0, 255})});
        s.external.zones.push_back(ExternalZone{3, "Lid logo", uniform(Rgb{255, 255, 0})});
        config.schemes.push_back(s);
        config.activeSchemeName = "test";

        QVERIFY(migrateConfig(config));

        const ExternalSettings& ext = config.schemes.front().external;
        QCOMPARE(ext.zones.size(), std::size_t(4));
        // Zone 0's identity preserved, renamed to current default.
        QCOMPARE(ext.zones[0].lightId, std::uint8_t(0));
        QCOMPARE(ext.zones[0].name, std::string("Rear ring 1"));
        QCOMPARE(ext.zones[0].color.ac, (Rgb{10, 20, 30}));
        QCOMPARE(ext.zones[1].lightId, std::uint8_t(1));
        QCOMPARE(ext.zones[1].name, std::string("Rear ring 2"));
        QCOMPARE(ext.zones[1].color.ac, (Rgb{40, 50, 60}));
        QCOMPARE(ext.zones[2].lightId, std::uint8_t(2));
        QCOMPARE(ext.zones[3].lightId, std::uint8_t(3));
    }

    // Zones 4 and 5 light nothing visible: migration removes them.
    void migrationRemovesInvisibleZones() {
        Config config;
        config.schemaVersion = 1;
        Scheme s;
        s.name = "test";
        s.external.zones.push_back(
            ExternalZone{0, "Rear ring (zone 0)", uniform(Rgb{128, 0, 0})});
        s.external.zones.push_back(
            ExternalZone{1, "Rear ring (zone 1)", uniform(Rgb{0, 128, 0})});
        s.external.zones.push_back(ExternalZone{2, "Interior logo", uniform(Rgb{0, 0, 255})});
        s.external.zones.push_back(ExternalZone{3, "Lid logo", uniform(Rgb{255, 255, 0})});
        s.external.zones.push_back(
            ExternalZone{4, "No visible light (zone 4)", uniform(Rgb{128, 64, 0})});
        s.external.zones.push_back(
            ExternalZone{5, "No visible light (zone 5)", uniform(Rgb{64, 128, 0})});
        config.schemes.push_back(s);
        config.activeSchemeName = "test";

        QVERIFY(migrateConfig(config));

        const ExternalSettings& ext = config.schemes.front().external;
        QCOMPARE(ext.zones.size(), std::size_t(4));
        for (const ExternalZone& z : ext.zones) {
            QVERIFY2(z.lightId != 4 && z.lightId != 5, "zones 4 and 5 must be removed");
        }
    }

    // Old default names are replaced with current names. Custom names survive.
    void migrationRenamesOldDefaultsButNotCustomNames() {
        Config config;
        config.schemaVersion = 1;
        Scheme s;
        s.name = "test";
        s.external.zones.push_back(
            ExternalZone{0, "Rear ring (zone 0)", uniform(Rgb{1, 1, 1})});
        s.external.zones.push_back(
            ExternalZone{1, "Rear ring (zone 1)", uniform(Rgb{2, 2, 2})});
        s.external.zones.push_back(ExternalZone{2, "The glowy thing", uniform(Rgb{3, 3, 3})});
        config.schemes.push_back(s);
        config.activeSchemeName = "test";

        QVERIFY(migrateConfig(config));

        const ExternalSettings& ext = config.schemes.front().external;
        QCOMPARE(ext.zones.size(), std::size_t(3));
        QCOMPARE(ext.zones[0].lightId, std::uint8_t(0));
        QCOMPARE(ext.zones[0].name, std::string("Rear ring 1"));
        QCOMPARE(ext.zones[1].lightId, std::uint8_t(1));
        QCOMPARE(ext.zones[1].name, std::string("Rear ring 2"));
        QCOMPARE(ext.zones[2].lightId, std::uint8_t(2));
        QCOMPARE(ext.zones[2].name, std::string("The glowy thing"));
    }

    // Running migration twice is safe - the second pass makes no changes.
    void migrationIsIdempotent() {
        Config config;
        config.schemaVersion = 1;
        Scheme s;
        s.name = "test";
        s.external.zones.push_back(
            ExternalZone{0, "Rear ring (zone 0)", uniform(Rgb{10, 20, 30})});
        s.external.zones.push_back(
            ExternalZone{1, "Rear ring (zone 1)", uniform(Rgb{40, 50, 60})});
        s.external.zones.push_back(ExternalZone{4, "should go away", uniform(Rgb{0, 0, 0})});
        config.schemes.push_back(s);
        config.activeSchemeName = "test";

        QVERIFY(migrateConfig(config));
        QVERIFY(!migrateConfig(config));

        const ExternalSettings& ext = config.schemes.front().external;
        QCOMPARE(ext.zones.size(), std::size_t(2));
        QCOMPARE(ext.zones[0].lightId, std::uint8_t(0));
        QCOMPARE(ext.zones[0].name, std::string("Rear ring 1"));
        QCOMPARE(ext.zones[1].lightId, std::uint8_t(1));
        QCOMPARE(ext.zones[1].name, std::string("Rear ring 2"));
    }

    void migrationCreatesConfigBackup() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";

        // Write a config with the old 6-zone layout.
        const std::string oldText = R"CONF({
          "schemaVersion": 1,
          "activeScheme": "x",
          "schemes": [{"name": "x",
                       "keyboard": {"color": "#010203"},
                       "external": {"zones": [
                         {"id": 0, "name": "Rear ring (zone 0)", "color": "#ff0000"},
                         {"id": 1, "name": "Rear ring (zone 1)", "color": "#00ff00"}
                       ]}}]
        })CONF";
        writeAll(path, oldText);

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());
        QVERIFY(result.fileExisted);

        // The migrated config should have two separate ring segment entries.
        const ExternalSettings& ext = result.config.schemes.front().external;
        QCOMPARE(ext.zones.size(), std::size_t(2));
        QCOMPARE(ext.zones[0].lightId, std::uint8_t(0));
        QCOMPARE(ext.zones[1].lightId, std::uint8_t(1));

        // A backup of the original config should exist.
        const auto backupPath = path.string() + ".bak";
        QVERIFY2(std::filesystem::exists(backupPath),
                 "backup file must be created on first migrating load");
        QCOMPARE(readAll(backupPath), oldText);
    }
    // The first release named the zones after its guesses. Those names reached saved configs
    // too, and must not survive as the label on the one Rear ring entry.
    void migrationRenamesFirstReleaseDefaults() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        const std::string text = R"CONF({
          "schemaVersion": 1,
          "activeScheme": "x",
          "schemes": [{"name": "x",
                       "external": {"zones": [
                         {"id": 0, "name": "Unidentified (ID 0)", "color": "#ff0000"},
                         {"id": 1, "name": "Rear ring (provisional)", "color": "#00ff00"},
                         {"id": 2, "name": "Interior logo", "color": "#0000ff"},
                         {"id": 3, "name": "Lid logo", "color": "#ffff00"},
                         {"id": 4, "name": "Unidentified (ID 4)", "color": "#010101"},
                         {"id": 5, "name": "Unidentified (ID 5)", "color": "#020202"}
                       ]}}]
        })CONF";
        writeAll(path, text);

        const LoadResult result = loadConfig(path);
        QVERIFY(result.error.empty());

        const Scheme* scheme = result.config.findScheme("x");
        QVERIFY(scheme != nullptr);
        const auto& zones = scheme->external.zones;
        QCOMPARE(zones.size(), std::size_t(4));
        QCOMPARE(zones[0].lightId, std::uint8_t(0));
        QCOMPARE(zones[0].name, std::string("Rear ring 1"));
        QCOMPARE(zones[0].color.ac, (Rgb{255, 0, 0}));
        QCOMPARE(zones[1].lightId, std::uint8_t(1));
        QCOMPARE(zones[1].name, std::string("Rear ring 2"));
        QCOMPARE(zones[2].name, std::string("Interior logo"));
        QCOMPARE(zones[3].name, std::string("Lid logo"));
    }

    // The pre-v4 editing model wrote every manual edit straight into the selected scheme,
    // so a v3 config can hold a built-in whose colours no longer match what it shipped as.
    // There was no explicit overwrite-save then, so every built-in whose contents differ is
    // damage, and the migration restores the shipped contents. Schemes with any other name
    // are the user's own and keep exactly what they say.
    void migrationRestoresBuiltInContentsButNotUserSchemes() {
        Config config;
        config.schemaVersion = 3;

        Scheme mangled;
        mangled.name = "Cool white";
        mangled.keyboard.color = Rgb{0, 0, 255};
        mangled.keyboard.brightness = Brightness::fromPercent(100);
        mangled.external = defaultExternalSettings(Rgb{1, 2, 3});

        Scheme mine;
        mine.name = "Mine";
        mine.keyboard.color = Rgb{9, 9, 9};
        mine.external = defaultExternalSettings(Rgb{9, 9, 9});

        config.schemes.push_back(mangled);
        config.schemes.push_back(mine);
        config.activeSchemeName = "Mine";

        QVERIFY(migrateConfig(config));

        const Scheme* restored = config.findScheme("Cool white");
        QVERIFY(restored != nullptr);
        Scheme shipped;
        bool foundShipped = false;
        for (const Scheme& builtin : builtinSchemes()) {
            if (builtin.name == "Cool white") {
                shipped = builtin;
                foundShipped = true;
            }
        }
        QVERIFY(foundShipped);
        QCOMPARE(*restored, shipped);

        const Scheme* userScheme = config.findScheme("Mine");
        QVERIFY(userScheme != nullptr);
        QCOMPARE(userScheme->keyboard.color, (Rgb{9, 9, 9}));
        QCOMPARE(userScheme->external.zones.front().color.ac, (Rgb{9, 9, 9}));
    }

    // Deleting a shipped preset is recorded so it stays deleted. Upgrading the layout of an
    // old config is not a reason to bring it back.
    void aDeletedBuiltInStaysDeletedThroughMigration() {        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        const std::string deleted = builtinSchemes().back().name;
        const std::string text = R"CONF({
          "schemaVersion": 1,
          "activeScheme": "x",
          "removedBuiltins": ["DELETED"],
          "schemes": [{"name": "x",
                       "external": {"zones": [
                         {"id": 0, "name": "Rear ring (zone 0)", "color": "#ff0000"},
                         {"id": 1, "name": "Rear ring (zone 1)", "color": "#00ff00"}
                       ]}}]
        })CONF";
        std::string withName = text;
        withName.replace(withName.find("DELETED"), 7, deleted);
        writeAll(path, withName);

        QVERIFY(loadConfig(path).config.findScheme(deleted) == nullptr);
        QVERIFY2(loadConfig(path).config.findScheme(deleted) == nullptr,
                 "a deleted built-in must not come back on a later load either");
    }

    // Migration is one-shot: once a config is on the current schema, IDs that a pre-v2
    // config would have lost are deliberate and stay.
    void aCurrentSchemaConfigIsNotMigrated() {
        Config config;
        Scheme s;
        s.name = "probe";
        s.external.zones.push_back(ExternalZone{1, "Rear ring (zone 1)", uniform(Rgb{1, 1, 1})});
        s.external.zones.push_back(ExternalZone{4, "Probe 4", uniform(Rgb{2, 2, 2})});
        s.external.zones.push_back(ExternalZone{5, "Probe 5", uniform(Rgb{3, 3, 3})});
        config.schemes.push_back(s);
        config.activeSchemeName = "probe";

        Config parsed;
        std::string error;
        QVERIFY2(deserializeConfig(serializeConfig(config), parsed, error), error.c_str());
        QVERIFY(!migrateConfig(parsed));

        const auto& zones = parsed.schemes.front().external.zones;
        QCOMPARE(zones.size(), std::size_t(3));
        QCOMPARE(zones[0].lightId, std::uint8_t(1));
        QCOMPARE(zones[0].name, std::string("Rear ring (zone 1)"));
        QCOMPARE(zones[1].lightId, std::uint8_t(4));
        QCOMPARE(zones[2].lightId, std::uint8_t(5));
    }

    // The first migrating load keeps the original as the backup and stamps the file with the
    // current schema, so a later load neither migrates again nor overwrites that backup.
    void migrationRunsOnceAndKeepsTheOriginalBackup() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::filesystem::path path =
            std::filesystem::path(dir.path().toStdString()) / "config.json";
        const std::string oldText = R"CONF({
          "schemaVersion": 1,
          "activeScheme": "x",
          "schemes": [{"name": "x",
                       "external": {"zones": [
                         {"id": 0, "name": "Rear ring (zone 0)", "color": "#ff0000"},
                         {"id": 1, "name": "Rear ring (zone 1)", "color": "#00ff00"}
                       ]}}]
        })CONF";
        writeAll(path, oldText);
        const auto backupPath = path.string() + ".bak";

        QVERIFY(loadConfig(path).error.empty());
        QCOMPARE(readAll(backupPath), oldText);

        Config onDisk;
        std::string error;
        QVERIFY2(deserializeConfig(readAll(path), onDisk, error), error.c_str());
        QCOMPARE(onDisk.schemaVersion, Config::kSchemaVersion);
        QCOMPARE(onDisk.findScheme("x")->external.zones.size(), std::size_t(2));

        std::filesystem::remove(backupPath);
        QVERIFY(loadConfig(path).error.empty());
        QVERIFY2(!std::filesystem::exists(backupPath),
                 "a config already on the current schema must not be backed up again");
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
