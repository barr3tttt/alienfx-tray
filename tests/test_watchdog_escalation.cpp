#include <QtTest>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>
#include <string>

#include "app/device_worker.h"
#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "core/device_interface.h"

// QCOMPARE on a scoped enum would pick up alienfx::toString() by ADL, which returns a
// string_view where QTest wants a const char*. Compare the underlying values instead.
static int asInt(alienfx::DeviceState state) { return static_cast<int>(state); }

using namespace alienfx;

namespace {

// Everything the test steers, in atomics, because the devices run on the worker thread while
// the test thread changes its mind between applies.
struct Script {
    // Below zero means open() succeeds; otherwise it fails with this DeviceState.
    std::atomic<int> keyboardOpenFails{-1};
    std::atomic<int> externalOpenFails{-1};
    std::atomic<bool> keyboardRefusesBrightness{false};
    std::atomic<bool> keyboardLeavesInterfaceDetached{false};

    // The wedge: applyZone takes the command and never answers, exactly as the SDK's APIv4
    // ready-wait does when a controller latches busy mid-call.
    std::atomic<bool> wedgeExternal{false};
    std::atomic<bool> externalWedged{false};
    std::atomic<bool> releaseWedge{false};
};

class ScriptedKeyboard final : public IKeyboardDevice {
   public:
    explicit ScriptedKeyboard(std::shared_ptr<Script> script) : script_(std::move(script)) {}

    bool open() override {
        const int fails = script_->keyboardOpenFails.load();
        if (fails >= 0) {
            status_ = DomainStatus{static_cast<DeviceState>(fails), "fake: keyboard unavailable"};
            return false;
        }
        open_ = true;
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    void close() override {
        open_ = false;
        status_.interfaceLeftDetached = script_->keyboardLeavesInterfaceDetached.load();
    }

    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness) override {
        if (script_->keyboardRefusesBrightness.load()) {
            status_ = DomainStatus{DeviceState::Error, "fake: brightness refused"};
            return false;
        }
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    bool applyEffect(KeyboardEffect, Rgb) override {
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    [[nodiscard]] DomainStatus status() const override { return status_; }

   private:
    std::shared_ptr<Script> script_;
    bool open_ = false;
    DomainStatus status_;
};

class ScriptedExternal final : public IExternalDevice {
   public:
    explicit ScriptedExternal(std::shared_ptr<Script> script) : script_(std::move(script)) {}

    bool open() override {
        const int fails = script_->externalOpenFails.load();
        if (fails >= 0) {
            status_ = DomainStatus{static_cast<DeviceState>(fails), "fake: external unavailable"};
            return false;
        }
        open_ = true;
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override {
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    bool applyZone(std::uint8_t, Rgb, Rgb) override {
        if (script_->wedgeExternal.load()) {
            script_->externalWedged.store(true);
            while (script_->wedgeExternal.load() && !script_->releaseWedge.load())
                QThread::msleep(5);
        }
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    bool commit() override {
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    [[nodiscard]] DomainStatus status() const override { return status_; }

   private:
    std::shared_ptr<Script> script_;
    bool open_ = false;
    DomainStatus status_;
};

class ScriptedWorker final : public DeviceWorker {
   public:
    explicit ScriptedWorker(std::shared_ptr<Script> script) : script_(std::move(script)) {}

   protected:
    DeviceSet createDevices() override {
        DeviceSet devices;
        devices.keyboard = std::make_unique<ScriptedKeyboard>(script_);
        devices.external = std::make_unique<ScriptedExternal>(script_);
        return devices;
    }

   private:
    std::shared_ptr<Script> script_;
};

}  // namespace

// The apply watchdog is the whole visibility guarantee: when the device thread parks inside
// a command that never returns, the UI has to stop implying the lights are fine. These drive
// a real LightingController against a device layer that genuinely hangs, so the suite fails
// for the real reason rather than agreeing with a predicate about itself.
class TestWatchdogEscalation : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
    void cleanup();

    void bothDomainsBusyThenAWedgeStillReachesTheUser();
    void aMissingDomainKeepsItsOwnDiagnosis();
    void aRefusedCommandKeepsItsOwnDiagnosis();
    void theDetachedInterfaceWarningSurvivesTheStamp();
    void aReturningApplyRestoresWhatWasThereBefore();

    void theStateIsClassifiedWithoutADefaultArm();

   private:
    // Far shorter than the six seconds production uses, because the fake devices answer
    // instantly and only the deliberate wedge is ever slow.
    static constexpr int kWatchdogMs = 400;

    std::unique_ptr<LightingController> makeController();

    // The engine only sends what changed, so re-submitting the same scheme reaches no
    // device at all. Every apply these tests want to observe has to paint something new.
    void editAZoneColour();

    [[nodiscard]] int stateOf(Domain domain) const {
        return asInt(controller_->status(domain).state);
    }

    std::shared_ptr<Script> script_;
    std::unique_ptr<LightingController> controller_;
    std::uint8_t nextZoneColour_ = 1;
    std::unique_ptr<QTemporaryDir> home_;
    QByteArray previousConfigHome_;
    QByteArray previousRuntimeDir_;
};

void TestWatchdogEscalation::initTestCase() { registerMetaTypes(); }

void TestWatchdogEscalation::init() {
    // A private config directory so the developer's own schemes are neither read nor
    // rewritten, and a private runtime directory so a running tray's device lock cannot make
    // the worker report Busy for reasons this suite did not ask for.
    home_ = std::make_unique<QTemporaryDir>();
    QVERIFY(home_->isValid());
    previousConfigHome_ = qgetenv("XDG_CONFIG_HOME");
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_CONFIG_HOME", home_->path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", home_->path().toUtf8());

    script_ = std::make_shared<Script>();
}

void TestWatchdogEscalation::cleanup() {
    // Let a wedged worker finish before the controller goes away, so the thread is joined
    // rather than abandoned and the next test starts from a quiet process.
    if (script_) {
        script_->wedgeExternal.store(false);
        script_->releaseWedge.store(true);
    }
    controller_.reset();
    script_.reset();
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

std::unique_ptr<LightingController> TestWatchdogEscalation::makeController() {
    return std::make_unique<LightingController>(new ScriptedWorker(script_), kWatchdogMs);
}

void TestWatchdogEscalation::editAZoneColour() {
    Scheme scheme = controller_->workingScheme();
    QVERIFY(!scheme.external.zones.empty());
    scheme.external.zones.front().color = uniform(Rgb{nextZoneColour_, 0, 0});
    nextZoneColour_ = static_cast<std::uint8_t>(nextZoneColour_ + 17);
    controller_->previewScheme(scheme);
}

// The case the escalation exists for. Both controllers last reported Busy - the interface
// could not be claimed - and the next apply then wedges, so nothing will ever publish a
// status again. A Busy left standing reads as a controller that is merely occupied for the
// moment, which is not what a wedged worker is.
void TestWatchdogEscalation::bothDomainsBusyThenAWedgeStillReachesTheUser() {
    script_->keyboardOpenFails.store(asInt(DeviceState::Busy));
    script_->externalOpenFails.store(asInt(DeviceState::Busy));

    controller_ = makeController();
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::Keyboard), asInt(DeviceState::Busy), 5000);
    QTRY_COMPARE(stateOf(Domain::External), asInt(DeviceState::Busy));

    QSignalSpy changes(controller_.get(), &LightingController::statusChanged);

    script_->keyboardOpenFails.store(-1);
    script_->externalOpenFails.store(-1);
    script_->wedgeExternal.store(true);
    editAZoneColour();

    QTRY_VERIFY(script_->externalWedged.load());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::External), asInt(DeviceState::Unresponsive),
                              5000);
    QCOMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Unresponsive));
    QVERIFY2(changes.count() >= 2, "the wedge has to be announced, not only recorded");
}

void TestWatchdogEscalation::aMissingDomainKeepsItsOwnDiagnosis() {
    script_->keyboardOpenFails.store(asInt(DeviceState::Missing));

    controller_ = makeController();
    QTRY_COMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Missing));
    QTRY_COMPARE(stateOf(Domain::External), asInt(DeviceState::Ready));

    script_->wedgeExternal.store(true);
    editAZoneColour();

    QTRY_VERIFY(script_->externalWedged.load());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::External), asInt(DeviceState::Unresponsive),
                              5000);
    QCOMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Missing));
    QCOMPARE(controller_->status(Domain::Keyboard).detail,
             std::string("fake: keyboard unavailable"));
}

void TestWatchdogEscalation::aRefusedCommandKeepsItsOwnDiagnosis() {
    script_->keyboardRefusesBrightness.store(true);

    controller_ = makeController();
    QTRY_COMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Error));

    script_->wedgeExternal.store(true);
    editAZoneColour();

    QTRY_VERIFY(script_->externalWedged.load());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::External), asInt(DeviceState::Unresponsive),
                              5000);
    QCOMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Error));
    QCOMPARE(controller_->status(Domain::Keyboard).detail,
             std::string("fake: brightness refused"));
}

// A driverless interface costs a real keyboard input device until reboot. The watchdog stamp
// is about the lighting and says nothing about the kernel driver, so it must not take the
// warning down with it - especially here, where the wedge is permanent and nothing will ever
// publish the warning again.
void TestWatchdogEscalation::theDetachedInterfaceWarningSurvivesTheStamp() {
    script_->keyboardLeavesInterfaceDetached.store(true);

    controller_ = makeController();
    QTRY_COMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Ready));

    // The keyboard claim is released after a short idle period, and that release is where
    // the failed reattach is noticed.
    QTRY_VERIFY_WITH_TIMEOUT(controller_->status(Domain::Keyboard).interfaceLeftDetached,
                             5000);

    script_->wedgeExternal.store(true);
    editAZoneColour();

    QTRY_VERIFY(script_->externalWedged.load());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::Keyboard), asInt(DeviceState::Unresponsive),
                              5000);
    QVERIFY2(controller_->status(Domain::Keyboard).interfaceLeftDetached,
             "the watchdog stamp erased the detached-interface warning");
    QVERIFY(describe(Domain::Keyboard, controller_->status(Domain::Keyboard))
                .find("kernel driver") != std::string::npos);
}

// A wedge is not always permanent. When the apply finally comes back the worker re-publishes
// nothing it considers unchanged, so the pre-watchdog status captured at the stamp is the
// only thing that can put a healthy controller back.
void TestWatchdogEscalation::aReturningApplyRestoresWhatWasThereBefore() {
    controller_ = makeController();
    QTRY_COMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Ready));
    QTRY_COMPARE(stateOf(Domain::External), asInt(DeviceState::Ready));

    script_->wedgeExternal.store(true);
    editAZoneColour();

    QTRY_VERIFY(script_->externalWedged.load());
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::External), asInt(DeviceState::Unresponsive),
                              5000);

    // A second edit while still wedged trips the watchdog again. It must not capture the
    // stamp as the thing to restore, or the domain never recovers.
    editAZoneColour();
    QTest::qWait(kWatchdogMs * 3);
    QCOMPARE(stateOf(Domain::External), asInt(DeviceState::Unresponsive));

    script_->wedgeExternal.store(false);
    script_->releaseWedge.store(true);

    QTRY_COMPARE_WITH_TIMEOUT(stateOf(Domain::External), asInt(DeviceState::Ready), 8000);
    QTRY_COMPARE(stateOf(Domain::Keyboard), asInt(DeviceState::Ready));
}

// The predicate the loop above consults, kept under test in its own right so the intent of
// each arm is written down. It is not the guarantee - the cases above are.
void TestWatchdogEscalation::theStateIsClassifiedWithoutADefaultArm() {
    QVERIFY(LightingController::escalatesOnWedge(DeviceState::Busy));
    QVERIFY(LightingController::escalatesOnWedge(DeviceState::Ready));
    QVERIFY(LightingController::escalatesOnWedge(DeviceState::Unknown));
    // The one that reads most like "nothing to worry about": a worker that deferred for the
    // lock and then wedged would otherwise promise an automatic retry forever.
    QVERIFY(LightingController::escalatesOnWedge(DeviceState::Waiting));
    QVERIFY(!LightingController::escalatesOnWedge(DeviceState::Missing));
    QVERIFY(!LightingController::escalatesOnWedge(DeviceState::Error));
    QVERIFY(!LightingController::escalatesOnWedge(DeviceState::Unresponsive));
}

QTEST_GUILESS_MAIN(TestWatchdogEscalation)
#include "test_watchdog_escalation.moc"
