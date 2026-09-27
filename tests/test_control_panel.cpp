#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QtTest>
#include <QTemporaryDir>

#include <memory>

#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "core/config.h"
#include "recording_devices.h"
#include "ui/color_picker.h"
#include "ui/control_panel.h"

using namespace alienfx;

namespace {

constexpr Rgb kSeedColor{0x11, 0x22, 0x33};

}  // namespace

// The panel half of the editing model. The controller tests prove the model; these prove
// the wiring the captain actually touches: typing a colour previews it on the lights and
// shows the unsaved strip, and only the Save button makes it permanent. Everything runs
// offscreen - the widgets are real, the display is not.
class TestControlPanel : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
    void cleanup();

    void typingAColourPreviewsWithoutSaving();
    void saveButtonPersistsTheDraft();
    void revertButtonRestoresTheScheme();
    void closingThePanelRevertsUnsavedEdits();
    void activatingFromTheSchemeListDiscardsTheDraft();

   private:
    void seedConfig(std::initializer_list<Scheme> schemes, const std::string& active);
    void makePanel();
    void settle();
    // Types a hex colour into the keyboard picker's hex field, the way the captain does.
    void typeKeyboardColour(const char* hexDigits);
    [[nodiscard]] QLabel* unsavedLabel() const {
        return panel_->findChild<QLabel*>(QStringLiteral("unsavedLabel"));
    }
    [[nodiscard]] QPushButton* saveButton() const {
        return panel_->findChild<QPushButton*>(QStringLiteral("saveScheme"));
    }
    [[nodiscard]] QPushButton* revertButton() const {
        return panel_->findChild<QPushButton*>(QStringLiteral("revertScheme"));
    }
    [[nodiscard]] bool stripVisible() const;
    [[nodiscard]] std::vector<test::DeviceRecord::ZoneWrite> zoneWrites() const {
        return record_->zoneSnapshot();
    }
    [[nodiscard]] std::vector<test::DeviceRecord::EffectWrite> effectWrites() const {
        return record_->effectSnapshot();
    }

    std::shared_ptr<test::DeviceRecord> record_;
    std::unique_ptr<LightingController> controller_;
    std::unique_ptr<ui::ControlPanel> panel_;
    std::unique_ptr<QTemporaryDir> home_;
    QByteArray previousConfigHome_;
    QByteArray previousRuntimeDir_;
};

void TestControlPanel::initTestCase() { registerMetaTypes(); }

void TestControlPanel::init() {
    home_ = std::make_unique<QTemporaryDir>();
    QVERIFY(home_->isValid());
    previousConfigHome_ = qgetenv("XDG_CONFIG_HOME");
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_CONFIG_HOME", home_->path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", home_->path().toUtf8());

    record_ = std::make_shared<test::DeviceRecord>();
}

void TestControlPanel::cleanup() {
    panel_.reset();
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

void TestControlPanel::seedConfig(std::initializer_list<Scheme> schemes,
                                  const std::string& active) {
    Config config;
    config.schemes.clear();
    for (const Scheme& scheme : schemes) config.schemes.push_back(scheme);
    config.activeSchemeName = active;
    config.lightsOn = true;

    std::string error;
    QVERIFY2(saveConfig(config, configPath(), error), error.c_str());
}

void TestControlPanel::makePanel() {
    controller_ = std::make_unique<LightingController>(
        new test::RecordingWorker(record_), LightingController::kApplyWatchdogMs);
    panel_ = std::make_unique<ui::ControlPanel>(*controller_);
    panel_->show();
}

void TestControlPanel::settle() {
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!effectWrites().empty(), 5000);
    record_->clear();
    QVERIFY(!stripVisible());
}

bool TestControlPanel::stripVisible() const {
    return unsavedLabel()->isVisible() && saveButton()->isVisible() &&
           revertButton()->isVisible();
}

void TestControlPanel::typeKeyboardColour(const char* hexDigits) {
    auto* picker = panel_->findChild<ui::ColorPicker*>(QStringLiteral("keyboardColor"));
    QVERIFY(picker != nullptr);
    auto* hex = picker->findChild<QLineEdit*>();
    QVERIFY(hex != nullptr);
    hex->clear();
    QTest::keyClicks(hex, QString::fromLatin1(hexDigits));
}

void TestControlPanel::typingAColourPreviewsWithoutSaving() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makePanel();
    settle();

    typeKeyboardColour("445566");

    // The preview is live on the lights and the strip names the scheme it would overwrite.
    // Wait on the content, not the count: the drag frames in between are coalesced by the
    // worker, and the newest one is the only one owed.
    QTRY_VERIFY_WITH_TIMEOUT(!effectWrites().empty() &&
                                 effectWrites().back().color == Rgb(0x44, 0x55, 0x66),
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(stripVisible(), 5000);
    QCOMPARE(unsavedLabel()->text(), QStringLiteral("Unsaved changes to \"Repro\""));

    // Nothing was saved: not in memory, not on disk.
    const Scheme* saved = controller_->config().findScheme("Repro");
    QVERIFY(saved != nullptr);
    QCOMPARE(saved->keyboard.color, kSeedColor);
    const LoadResult onDisk = loadConfig(configPath());
    QCOMPARE(onDisk.config.findScheme("Repro")->keyboard.color, kSeedColor);
}

void TestControlPanel::saveButtonPersistsTheDraft() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makePanel();
    settle();

    typeKeyboardColour("445566");
    QTRY_VERIFY_WITH_TIMEOUT(stripVisible(), 5000);

    QTest::mouseClick(saveButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!stripVisible(), 5000);
    QVERIFY(!controller_->hasUnsavedChanges());

    QCOMPARE(controller_->config().findScheme("Repro")->keyboard.color, (Rgb{0x44, 0x55, 0x66}));
    const LoadResult onDisk = loadConfig(configPath());
    QCOMPARE(onDisk.config.findScheme("Repro")->keyboard.color, (Rgb{0x44, 0x55, 0x66}));
}

void TestControlPanel::revertButtonRestoresTheScheme() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makePanel();
    settle();

    typeKeyboardColour("445566");
    QTRY_VERIFY_WITH_TIMEOUT(stripVisible(), 5000);

    record_->clear();
    QTest::mouseClick(revertButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!stripVisible(), 5000);
    QCOMPARE(controller_->workingScheme().keyboard.color, kSeedColor);

    // The revert repaints the saved scheme in full.
    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, kSeedColor);

    const LoadResult onDisk = loadConfig(configPath());
    QCOMPARE(onDisk.config.findScheme("Repro")->keyboard.color, kSeedColor);
}

void TestControlPanel::closingThePanelRevertsUnsavedEdits() {
    seedConfig({test::solidScheme("Repro", kSeedColor)}, "Repro");
    makePanel();
    settle();

    typeKeyboardColour("445566");
    QTRY_VERIFY_WITH_TIMEOUT(stripVisible(), 5000);

    record_->clear();
    panel_->hide();
    QVERIFY(!controller_->hasUnsavedChanges());
    QCOMPARE(controller_->workingScheme().keyboard.color, kSeedColor);

    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, kSeedColor);

    const LoadResult onDisk = loadConfig(configPath());
    QCOMPARE(onDisk.config.findScheme("Repro")->keyboard.color, kSeedColor);
}

void TestControlPanel::activatingFromTheSchemeListDiscardsTheDraft() {
    seedConfig({test::solidScheme("Alpha", Rgb{0x10, 0, 0}),
                test::solidScheme("Beta", Rgb{0x20, 0, 0})},
               "Alpha");
    makePanel();
    settle();

    typeKeyboardColour("445566");
    QTRY_VERIFY_WITH_TIMEOUT(stripVisible(), 5000);

    // Select "Beta" in the scheme list and press Activate: the panel's own activation path.
    auto* list = panel_->findChild<QListWidget*>(QStringLiteral("schemeList"));
    QVERIFY(list != nullptr);
    QListWidgetItem* beta = nullptr;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole).toString() == QStringLiteral("Beta"))
            beta = list->item(row);
    }
    QVERIFY(beta != nullptr);
    auto* activate = panel_->findChild<QPushButton*>(QStringLiteral("activateScheme"));
    QVERIFY(activate != nullptr);

    record_->clear();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      list->visualItemRect(beta).center());
    QCOMPARE(list->currentItem(), beta);
    QTest::mouseClick(activate, Qt::LeftButton);

    QTRY_VERIFY_WITH_TIMEOUT(zoneWrites().size() == 4, 5000);
    for (const test::DeviceRecord::ZoneWrite& write : zoneWrites())
        QCOMPARE(write.ac, (Rgb{0x20, 0, 0}));
    QTRY_VERIFY_WITH_TIMEOUT(!stripVisible(), 5000);
    QCOMPARE(controller_->workingScheme().name, std::string("Beta"));

    // Alpha's saved contents never moved.
    const LoadResult onDisk = loadConfig(configPath());
    QCOMPARE(onDisk.config.findScheme("Alpha")->keyboard.color, (Rgb{0x10, 0, 0}));
}

int main(int argc, char* argv[]) {
    // The widgets are real; the display is not. Set before the QApplication exists.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestControlPanel test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_control_panel.moc"
