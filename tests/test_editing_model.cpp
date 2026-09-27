#include <QtTest>
#include <QTemporaryDir>

#include <memory>
#include <utility>

#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "core/config.h"
#include "recording_devices.h"

using namespace alienfx;

namespace {

constexpr Rgb kSeedColor{0x11, 0x22, 0x33};
constexpr Rgb kEditedColor{0x44, 0x55, 0x66};

}  // namespace

// The editing model the captain asked for, driven through the real LightingController the
// way the control panel drives it, against a device layer that records every command.
//
// Issue #14: a manual colour edit used to write straight into the selected scheme, in
// memory and on disk, with no save gesture. Now the preview paints the lights and nothing
// else; only Save and "Save current as..." persist.
//
// Issue #15: selecting a scheme used to be a delta apply, so re-selecting the active
// scheme - the only "make it so" gesture - commanded nothing and any drift between the
// tracked state and the physical lights stayed forever. Now selection is a full repaint:
// every zone, every time.
class TestEditingModel : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
    void cleanup();

    void manualEditsPreviewButNeverPersist();
    void selectingASchemeSetsEveryZoneEveryTime();
    void saveOverwritesTheSelectedScheme();
    void saveAsNewStoresAndActivates();
    void switchingSchemeDiscardsUnsavedEdits();
    void revertRestoresTheSelectedScheme();
    void unsavedChangesSignalTracksTheDraft();

   private:
    void seedConfig(std::initializer_list<Scheme> schemes, const std::string& active);
    void makeController();
    // Waits for the constructor's full paint of the active scheme, then clears the record
    // so each test asserts only what its own actions sent.
    void settle();
    [[nodiscard]] std::vector<test::DeviceRecord::ZoneWrite> zoneWrites() const {
        return record_->zoneSnapshot();
    }
    [[nodiscard]] std::vector<test::DeviceRecord::EffectWrite> effectWrites() const {
        return record_->effectSnapshot();
    }
    [[nodiscard]] Scheme savedScheme(const std::string& name) const {
        const LoadResult onDisk = loadConfig(configPath());
        const Scheme* scheme = onDisk.config.findScheme(name);
        return scheme ? *scheme : Scheme{};
    }

    std::shared_ptr<test::DeviceRecord> record_;
    std::unique_ptr<LightingController> controller_;
    std::unique_ptr<QTemporaryDir> home_;
    QByteArray previousConfigHome_;
    QByteArray previousRuntimeDir_;
};

void TestEditingModel::initTestCase() { registerMetaTypes(); }

void TestEditingModel::init() {
    home_ = std::make_unique<QTemporaryDir>();
    QVERIFY(home_->isValid());
    previousConfigHome_ = qgetenv("XDG_CONFIG_HOME");
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_CONFIG_HOME", home_->path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", home_->path().toUtf8());

    record_ = std::make_shared<test::DeviceRecord>();
}

void TestEditingModel::cleanup() {
    controller_.reset();
    record_.reset();
    home_.reset();

    if (previousConfigHome_.isEmpty())
        qunsetenv("XDG_CONFIG_HOME");
    else
        qputenv("XDG_CONFIG_HOME", previousConfigHome_);

    if (previousRuntimeDir_.isEmpty())
        qunsetenv("XDG_RUNTIME_DIR");
    else
        qputenv("XDG_RUNTIME_DIR", previousRuntimeDir_);
}

void TestEditingModel::seedConfig(std::initializer_list<Scheme> schemes,
                                  const std::string& active) {
    Config config;
    config.schemes.clear();
    for (const Scheme& scheme : schemes) config.schemes.push_back(scheme);
    config.activeSchemeName = active;
    config.lightsOn = true;

    std::string error;
    QVERIFY2(saveConfig(config, configPath(), error), error.c_str());
}

void TestEditingModel::makeController() {
    controller_ = std::make_unique<LightingController>(
        new test::RecordingWorker(record_), LightingController::kApplyWatchdogMs);
}

void TestEditingModel::settle() {
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!effectWrites().empty(), 5000);
    record_->clear();
}

void TestEditingModel::manualEditsPreviewButNeverPersist() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makeController();
    settle();
    QVERIFY(!controller_->hasUnsavedChanges());

    // The panel's exact edit path: one working copy, pushed on every drag frame.
    Scheme edited = controller_->workingScheme();
    edited.keyboard.color = kEditedColor;
    edited.external.zones.front().color = uniform(kEditedColor);
    controller_->previewScheme(edited);

    // The preview is live on the lights: the edited zone and the keyboard were painted.
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 1, 5000);
    QCOMPARE(zoneWrites().front().ac, kEditedColor);
    QTRY_VERIFY_WITH_TIMEOUT(effectWrites().size() == 1, 5000);
    QCOMPARE(effectWrites().front().color, kEditedColor);
    QVERIFY(controller_->hasUnsavedChanges());

    // Outlast the debounce the old write-through used, then prove the saved scheme is
    // untouched in memory and on disk.
    QTest::qWait(1500);
    const Scheme* saved = controller_->config().findScheme("Repro");
    QVERIFY(saved != nullptr);
    QCOMPARE(saved->keyboard.color, kSeedColor);
    QCOMPARE(saved->external.zones.front().color.ac, kSeedColor);

    const Scheme onDisk = savedScheme("Repro");
    QCOMPARE(onDisk.keyboard.color, kSeedColor);
    QCOMPARE(onDisk.external.zones.front().color.ac, kSeedColor);
}

void TestEditingModel::selectingASchemeSetsEveryZoneEveryTime() {
    seedConfig({test::solidScheme("Alpha", Rgb{0x10, 0, 0}),
                test::solidScheme("Beta", Rgb{0x20, 0, 0})},
               "Alpha");
    makeController();
    settle();

    record_->clear();
    controller_->activateScheme(QStringLiteral("Beta"));
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, (Rgb{0x20, 0, 0}));

    // Re-selecting the scheme that is already active must still set every zone. It is the
    // one "make it so" gesture available when the lights have drifted from what the scheme
    // says, and it used to command nothing at all.
    record_->clear();
    controller_->activateScheme(QStringLiteral("Beta"));
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, (Rgb{0x20, 0, 0}));
    QTRY_VERIFY_WITH_TIMEOUT(effectWrites().size() == 1, 5000);
    QCOMPARE(effectWrites().front().color, (Rgb{0x20, 0, 0}));
}

void TestEditingModel::saveOverwritesTheSelectedScheme() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makeController();
    settle();

    Scheme edited = controller_->workingScheme();
    edited.keyboard.color = kEditedColor;
    edited.external.zones.front().color = uniform(kEditedColor);
    controller_->previewScheme(edited);
    QVERIFY(controller_->hasUnsavedChanges());

    controller_->saveWorkingScheme();
    QVERIFY(!controller_->hasUnsavedChanges());
    QCOMPARE(controller_->config().activeSchemeName, std::string("Repro"));

    const Scheme* saved = controller_->config().findScheme("Repro");
    QVERIFY(saved != nullptr);
    QCOMPARE(saved->keyboard.color, kEditedColor);
    QCOMPARE(saved->external.zones.front().color.ac, kEditedColor);

    const Scheme onDisk = savedScheme("Repro");
    QCOMPARE(onDisk.keyboard.color, kEditedColor);
    QCOMPARE(onDisk.external.zones.front().color.ac, kEditedColor);
}

void TestEditingModel::saveAsNewStoresAndActivates() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makeController();
    settle();

    Scheme edited = controller_->workingScheme();
    edited.keyboard.color = kEditedColor;
    controller_->previewScheme(edited);

    controller_->saveWorkingSchemeAs(QStringLiteral("New scheme"));
    QVERIFY(!controller_->hasUnsavedChanges());
    QCOMPARE(controller_->config().activeSchemeName, std::string("New scheme"));
    QCOMPARE(controller_->workingScheme().name, std::string("New scheme"));

    // The original is untouched, and both are on disk.
    const Scheme* original = controller_->config().findScheme("Repro");
    QVERIFY(original != nullptr);
    QCOMPARE(original->keyboard.color, kSeedColor);
    const Scheme* stored = controller_->config().findScheme("New scheme");
    QVERIFY(stored != nullptr);
    QCOMPARE(stored->keyboard.color, kEditedColor);

    QCOMPARE(savedScheme("Repro").keyboard.color, kSeedColor);
    QCOMPARE(savedScheme("New scheme").keyboard.color, kEditedColor);
}

void TestEditingModel::switchingSchemeDiscardsUnsavedEdits() {
    seedConfig({test::solidScheme("Alpha", Rgb{0x10, 0, 0}),
                test::solidScheme("Beta", Rgb{0x20, 0, 0})},
               "Alpha");
    makeController();
    settle();

    // Preview an edit on Alpha and prove it reached the lights.
    Scheme edited = controller_->workingScheme();
    edited.external.zones.front().color = uniform(Rgb{0x99, 0, 0});
    controller_->previewScheme(edited);
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 1, 5000);
    QCOMPARE(zoneWrites().front().ac, (Rgb{0x99, 0, 0}));
    QVERIFY(controller_->hasUnsavedChanges());

    // Switching away throws the draft away: the lights are repainted with Beta in full,
    // the draft state clears, and Alpha's saved contents never moved.
    record_->clear();
    controller_->activateScheme(QStringLiteral("Beta"));
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, (Rgb{0x20, 0, 0}));

    QVERIFY(!controller_->hasUnsavedChanges());
    QCOMPARE(controller_->workingScheme().name, std::string("Beta"));
    const Scheme* alpha = controller_->config().findScheme("Alpha");
    QVERIFY(alpha != nullptr);
    QCOMPARE(alpha->external.zones.front().color.ac, (Rgb{0x10, 0, 0}));
    QCOMPARE(savedScheme("Alpha").external.zones.front().color.ac, (Rgb{0x10, 0, 0}));
}

void TestEditingModel::revertRestoresTheSelectedScheme() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makeController();
    settle();

    Scheme edited = controller_->workingScheme();
    edited.keyboard.color = kEditedColor;
    edited.external.zones.front().color = uniform(kEditedColor);
    controller_->previewScheme(edited);
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(effectWrites().size() == 1, 5000);
    QVERIFY(controller_->hasUnsavedChanges());

    record_->clear();
    controller_->revertWorkingScheme();
    QVERIFY(!controller_->hasUnsavedChanges());
    QCOMPARE(controller_->workingScheme().keyboard.color, kSeedColor);

    // The revert repaints the saved scheme in full, not just the zones the preview touched.
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, kSeedColor);
    QTRY_VERIFY_WITH_TIMEOUT(effectWrites().size() == 1, 5000);
    QCOMPARE(effectWrites().front().color, kSeedColor);
}

void TestEditingModel::unsavedChangesSignalTracksTheDraft() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makeController();
    settle();

    QSignalSpy spy(controller_.get(), &LightingController::unsavedChangesChanged);
    QCOMPARE(spy.count(), 0);

    Scheme edited = controller_->workingScheme();
    edited.keyboard.color = kEditedColor;
    controller_->previewScheme(edited);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    // Still dirty after another preview: no repeated signal.
    controller_->previewScheme(edited);
    QCOMPARE(spy.count(), 0);

    controller_->saveWorkingScheme();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);

    // A new edit after the save is a new draft: the signal fires again. (Previewing the
    // same colours that were just saved would not be a change at all.)
    edited.keyboard.color = Rgb{0x77, 0x11, 0x22};
    controller_->previewScheme(edited);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    controller_->revertWorkingScheme();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

QTEST_GUILESS_MAIN(TestEditingModel)
#include "test_editing_model.moc"
