#include <QDBusConnection>
#include <QtTest>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "app/device_thread.h"
#include "app/device_worker.h"
#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "app/process_lock.h"
#include "app/sleep_monitor.h"
#include "core/device_interface.h"
#include "core/scheme.h"
#include "fake_devices.h"

// QCOMPARE on a scoped enum would pick up alienfx::toString() by ADL, which returns a
// string_view where QTest wants a const char*. Compare the underlying values instead.
static int asInt(alienfx::DeviceState state) { return static_cast<int>(state); }

using namespace alienfx;

namespace {

// Counters rather than the recording fakes, because the worker thread writes them while the
// test thread reads them to decide whether anything reached the hardware yet.
struct Painted {
    std::atomic<int> zoneCalls{0};
    std::atomic<int> lastRed{-1};
    std::atomic<bool> sawFirstFrame{false};
    std::atomic<bool> open{false};
    std::atomic<int> opens{0};
    std::atomic<int> keyboardWrites{0};
    // How many more opens of the external controller report it not enumerated yet.
    std::atomic<int> missingOpens{0};
};

class CountingKeyboard final : public IKeyboardDevice {
   public:
    explicit CountingKeyboard(std::shared_ptr<Painted> painted) : painted_(std::move(painted)) {}

    bool open() override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness) override {
        painted_->keyboardWrites.fetch_add(1);
        return true;
    }
    bool applyEffect(KeyboardEffect, Rgb) override {
        painted_->keyboardWrites.fetch_add(1);
        return true;
    }

    [[nodiscard]] DomainStatus status() const override {
        return DomainStatus{DeviceState::Ready, {}};
    }

   private:
    std::shared_ptr<Painted> painted_;
    bool open_ = false;
};

class CountingExternal final : public IExternalDevice {
   public:
    explicit CountingExternal(std::shared_ptr<Painted> painted) : painted_(std::move(painted)) {}

    bool open() override {
        painted_->opens.fetch_add(1);
        if (painted_->missingOpens.load() > 0) {
            painted_->missingOpens.fetch_sub(1);
            status_ = DomainStatus{DeviceState::Missing, "fake: not enumerated yet"};
            return false;
        }
        open_ = true;
        painted_->open.store(true);
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }
    void close() override {
        open_ = false;
        painted_->open.store(false);
    }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override { return true; }

    bool applyZone(std::uint8_t, Rgb acColor, Rgb) override {
        painted_->zoneCalls.fetch_add(1);
        painted_->lastRed.store(acColor.r);
        if (acColor.r == kFirstFrameRed) painted_->sawFirstFrame.store(true);
        return true;
    }

    bool commit() override { return true; }

    [[nodiscard]] DomainStatus status() const override { return status_; }

    static constexpr int kFirstFrameRed = 11;

   private:
    std::shared_ptr<Painted> painted_;
    bool open_ = false;
    DomainStatus status_{DeviceState::Ready, {}};
};

class StubWorker final : public DeviceWorker {
   public:
    explicit StubWorker(DeviceSet devices, int idleMs = 0, int keyboardIdleMs = 0)
        : devices_(std::move(devices)),
          idleMs_(idleMs),
          keyboardIdleMs_(keyboardIdleMs > 0 ? keyboardIdleMs : idleMs) {}

   protected:
    DeviceSet createDevices() override { return std::move(devices_); }
    [[nodiscard]] int idleReleaseMs(Domain domain) const override {
        const int ms = domain == Domain::Keyboard ? keyboardIdleMs_ : idleMs_;
        return ms > 0 ? ms : DeviceWorker::idleReleaseMs(domain);
    }

   private:
    DeviceSet devices_;
    int idleMs_;
    int keyboardIdleMs_;
};

constexpr int kShortIdleMs = 50;

DeviceSet countingDevices(const std::shared_ptr<Painted>& painted) {
    DeviceSet devices;
    devices.keyboard = std::make_unique<test::FakeKeyboardDevice>();
    devices.external = std::make_unique<CountingExternal>(painted);
    return devices;
}

ResolvedScheme frameWithRed(int red) {
    Scheme scheme;
    scheme.external.zones.push_back(
        ExternalZone{0, "probe", uniform(Rgb{static_cast<std::uint8_t>(red), 0, 0})});
    return resolveOutput(LightingState{std::move(scheme), true});
}

}  // namespace

// The device lock is held by whichever process is mid-apply, and the resume hook holds it
// across its whole device poll - tens of seconds. A frame submitted in that window used to be
// dropped outright, so a colour-wheel drag during a resume left the hardware on the old
// colour while the config file and the UI both showed the new one.
class TestDeviceWorkerLock : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void init();
    void cleanup();

    void aFrameSubmittedWhileTheLockIsHeldIsPaintedWhenItFrees();
    void anOutstandingFrameIsNotReportedAsAFinishedApply();
    void theLockReportsWhetherAnotherProcessHeldItInBetween();
    void anUnopenableLockIsReportedOnceNotOnEveryAcquire();
    void anIdleReleaseAloneRepaintsNothingWhenResumesAreWatched();
    void anotherHolderInBetweenMakesTheWorkerRepaint();
    void withoutAResumeWatchEveryIdleReleaseRepaints();
    void aResumeRepaintsBothDomains();
    void aResumeWaitsForAControllerThatHasNotComeBackYet();
    void theSleepMonitorReportsOnlyTheWayBackUp();

   private:
    // Paints one frame, waits for the worker to release everything, and returns the zone
    // writes it cost.
    int paintAndWaitForRelease(DeviceThread& thread, int red);

    std::shared_ptr<Painted> painted_;
    std::unique_ptr<QTemporaryDir> runtimeDir_;
    QByteArray previousRuntimeDir_;
    QByteArray previousConfigHome_;
};

void TestDeviceWorkerLock::initTestCase() { registerMetaTypes(); }

void TestDeviceWorkerLock::init() {
    runtimeDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(runtimeDir_->isValid());
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    previousConfigHome_ = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_RUNTIME_DIR", runtimeDir_->path().toUtf8());
    qputenv("XDG_CONFIG_HOME", runtimeDir_->path().toUtf8());
    painted_ = std::make_shared<Painted>();
}

void TestDeviceWorkerLock::cleanup() {
    painted_.reset();
    runtimeDir_.reset();
    if (previousRuntimeDir_.isEmpty())
        qunsetenv("XDG_RUNTIME_DIR");
    else
        qputenv("XDG_RUNTIME_DIR", previousRuntimeDir_);
    if (previousConfigHome_.isEmpty())
        qunsetenv("XDG_CONFIG_HOME");
    else
        qputenv("XDG_CONFIG_HOME", previousConfigHome_);
}

void TestDeviceWorkerLock::aFrameSubmittedWhileTheLockIsHeldIsPaintedWhenItFrees() {
    ProcessLock contender;
    QVERIFY(contender.acquire(std::chrono::milliseconds(500)));

    DeviceSet devices;
    devices.keyboard = std::make_unique<test::FakeKeyboardDevice>();
    devices.external = std::make_unique<CountingExternal>(painted_);

    DeviceThread thread(new StubWorker(std::move(devices)));
    QSignalSpy deferred(&thread, &DeviceThread::applyDeferred);

    thread.submit(frameWithRed(CountingExternal::kFirstFrameRed));
    QVERIFY2(deferred.wait(5000), "a contended lock has to be reported, not silently dropped");
    QCOMPARE(painted_->zoneCalls.load(), 0);

    // The drag carries on while the lock is still held. Only the newest frame is owed.
    const int lastFrameRed = 200;
    thread.submit(frameWithRed(lastFrameRed));

    // Wait until that frame has been deferred too. Releasing the lock while an attempt is
    // still mid-acquire would let it through without the retry ever being involved, and the
    // retry is the whole point.
    QTRY_VERIFY_WITH_TIMEOUT(deferred.count() >= 2, 5000);
    QCOMPARE(painted_->zoneCalls.load(), 0);

    contender.release();

    QTRY_VERIFY_WITH_TIMEOUT(painted_->zoneCalls.load() > 0, 10000);
    QCOMPARE(painted_->lastRed.load(), lastFrameRed);
    QVERIFY2(!painted_->sawFirstFrame.load(),
             "a superseded frame must not be painted after the newer one");

    QVERIFY(thread.stop(DeviceThread::kStopTimeout));
}

// The apply watchdog is armed by a submit and disarmed by applyFinished. A frame still owed
// is not finished, so reporting it as such would let a controller that wedges immediately
// afterwards go unnoticed.
void TestDeviceWorkerLock::anOutstandingFrameIsNotReportedAsAFinishedApply() {
    ProcessLock contender;
    QVERIFY(contender.acquire(std::chrono::milliseconds(500)));

    DeviceSet devices;
    devices.keyboard = std::make_unique<test::FakeKeyboardDevice>();
    devices.external = std::make_unique<CountingExternal>(painted_);

    DeviceThread thread(new StubWorker(std::move(devices)));
    QSignalSpy finished(&thread, &DeviceThread::applyFinished);
    QSignalSpy deferred(&thread, &DeviceThread::applyDeferred);
    QSignalSpy statuses(&thread, &DeviceThread::statusChanged);

    const quint64 generation = thread.submit(frameWithRed(CountingExternal::kFirstFrameRed));
    QVERIFY(deferred.wait(5000));
    QCOMPARE(finished.count(), 0);

    // The UI still has to say what is going on while we wait.
    QVERIFY(statuses.count() >= 2);
    QCOMPARE(asInt(statuses.first().at(1).value<DomainStatus>().state),
             asInt(DeviceState::Waiting));

    contender.release();

    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 10000);
    QCOMPARE(finished.first().at(0).value<quint64>(), generation);
    QVERIFY(finished.first().at(1).toBool());

    QVERIFY(thread.stop(DeviceThread::kStopTimeout));
}

// The tray keeps what it knows about the external lights across its own idle release, which
// is only safe while nobody else touched them. The lock is the one thing every process that
// paints has to take, so it is what tells the tray that someone did.
void TestDeviceWorkerLock::theLockReportsWhetherAnotherProcessHeldItInBetween() {
    ProcessLock tray;
    QVERIFY(tray.acquire(std::chrono::milliseconds(500)));
    QVERIFY2(tray.othersHeldSinceLastRelease(), "a first acquire knows nothing about before");
    tray.release();

    QVERIFY(tray.acquire(std::chrono::milliseconds(500)));
    QVERIFY2(!tray.othersHeldSinceLastRelease(), "nobody else took it");
    tray.release();

    {
        ProcessLock resumeHook;
        QVERIFY(resumeHook.acquire(std::chrono::milliseconds(500)));
    }

    QVERIFY(tray.acquire(std::chrono::milliseconds(500)));
    QVERIFY2(tray.othersHeldSinceLastRelease(), "the resume hook held it in between");
    tray.release();

    QVERIFY(tray.acquire(std::chrono::milliseconds(500)));
    QVERIFY(!tray.othersHeldSinceLastRelease());
}

// With no lock file there is no serialisation, and so no way to tell whether anyone else
// painted. That is reported once, on the way in: reporting it on every acquire would make
// the worker repaint everything on every frame of a colour drag.
void TestDeviceWorkerLock::anUnopenableLockIsReportedOnceNotOnEveryAcquire() {
    qputenv("XDG_RUNTIME_DIR", (runtimeDir_->path() + "/missing").toUtf8());

    ProcessLock tray;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("cannot open the device lock"));
    QVERIFY(tray.acquire(std::chrono::milliseconds(100)));
    QVERIFY(!tray.held());
    QVERIFY(tray.othersHeldSinceLastRelease());

    QTest::failOnWarning(QRegularExpression("cannot open the device lock"));
    for (int i = 0; i < 3; ++i) {
        QVERIFY(tray.acquire(std::chrono::milliseconds(100)));
        QVERIFY2(!tray.othersHeldSinceLastRelease(), "reported on every acquire");
    }

    // Once the lock is back, what happened while it was missing is unknown.
    qputenv("XDG_RUNTIME_DIR", runtimeDir_->path().toUtf8());
    QVERIFY(tray.acquire(std::chrono::milliseconds(100)));
    QVERIFY(tray.held());
    QVERIFY(tray.othersHeldSinceLastRelease());
}

int TestDeviceWorkerLock::paintAndWaitForRelease(DeviceThread& thread, int red) {
    QSignalSpy finished(&thread, &DeviceThread::applyFinished);
    const int before = painted_->zoneCalls.load();
    thread.submit(frameWithRed(red));
    if (!finished.wait(5000)) return -1;
    if (!QTest::qWaitFor([&] { return !painted_->open.load(); }, 5000)) return -1;
    return painted_->zoneCalls.load() - before;
}

// Releasing the handle changes nothing the lights show. With resumes watched, the same frame
// after an idle release neither reopens the controller nor writes anything to it.
void TestDeviceWorkerLock::anIdleReleaseAloneRepaintsNothingWhenResumesAreWatched() {
    auto* worker = new StubWorker(countingDevices(painted_), kShortIdleMs);
    worker->setExternalStateSurvivesIdleRelease(true);
    DeviceThread thread(worker);

    QVERIFY(paintAndWaitForRelease(thread, 40) > 0);
    QCOMPARE(paintAndWaitForRelease(thread, 40), 0);
    QCOMPARE(painted_->opens.load(), 1);

    QVERIFY(thread.stop(DeviceThread::kStopTimeout));
}

// Anyone else taking the lock - the resume hook, the login unit - may have painted, so the
// worker repaints what it had kept.
void TestDeviceWorkerLock::anotherHolderInBetweenMakesTheWorkerRepaint() {
    auto* worker = new StubWorker(countingDevices(painted_), kShortIdleMs);
    worker->setExternalStateSurvivesIdleRelease(true);
    DeviceThread thread(worker);

    const int full = paintAndWaitForRelease(thread, 40);
    QVERIFY(full > 0);
    {
        ProcessLock resumeHook;
        QTRY_VERIFY_WITH_TIMEOUT(resumeHook.acquire(std::chrono::milliseconds(100)), 5000);
    }
    QCOMPARE(paintAndWaitForRelease(thread, 40), full);

    QVERIFY(thread.stop(DeviceThread::kStopTimeout));
}

// Without a resume signal, a suspend could go unseen, so nothing is kept across the release.
void TestDeviceWorkerLock::withoutAResumeWatchEveryIdleReleaseRepaints() {
    DeviceThread thread(new StubWorker(countingDevices(painted_), kShortIdleMs));

    const int full = paintAndWaitForRelease(thread, 40);
    QVERIFY(full > 0);
    QCOMPARE(paintAndWaitForRelease(thread, 40), full);
    QCOMPARE(painted_->opens.load(), 2);

    QVERIFY(thread.stop(DeviceThread::kStopTimeout));
}

// logind's resume signal reaches the controller, which repaints both controllers in full
// even though nothing about the scheme changed and nobody else held the lock. The keyboard is
// kept inside its idle window across the "suspend", as it is when the lid closes within a
// second or so of an edit, so its own release has not already forgotten what it was sent.
void TestDeviceWorkerLock::aResumeRepaintsBothDomains() {
    DeviceSet devices;
    devices.keyboard = std::make_unique<CountingKeyboard>(painted_);
    devices.external = std::make_unique<CountingExternal>(painted_);
    LightingController controller(new StubWorker(std::move(devices), kShortIdleMs, 60000),
                                  LightingController::kApplyWatchdogMs);
    controller.setResumeWatched(true);

    QVERIFY(!controller.workingScheme().external.zones.empty());
    QTRY_VERIFY_WITH_TIMEOUT(painted_->zoneCalls.load() > 0 && !painted_->open.load(), 5000);
    const int firstPaint = painted_->zoneCalls.load();

    // A plain reapply after the idle release has nothing to send.
    controller.reapply();
    QTest::qWait(10 * kShortIdleMs);
    QCOMPARE(painted_->zoneCalls.load(), firstPaint);
    QCOMPARE(painted_->opens.load(), 1);
    const int keyboardBefore = painted_->keyboardWrites.load();
    QVERIFY(keyboardBefore > 0);

    SleepMonitor monitor(QDBusConnection(QStringLiteral("alienfx-test-no-bus")));
    connect(&monitor, &SleepMonitor::resumed, &controller, &LightingController::onSystemResumed);
    QMetaObject::invokeMethod(&monitor, "handlePrepareForSleep", Q_ARG(bool, false));

    QTRY_VERIFY_WITH_TIMEOUT(painted_->zoneCalls.load() >= firstPaint * 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(painted_->keyboardWrites.load() >= keyboardBefore * 2, 5000);

    controller.shutdown();
}

// On resume the controllers are not necessarily enumerable yet. Without the sleep hook the
// tray's own repaint is the only one, so it waits for them the way --apply does rather than
// making one attempt and leaving the lights on firmware defaults.
void TestDeviceWorkerLock::aResumeWaitsForAControllerThatHasNotComeBackYet() {
    DeviceSet devices;
    devices.keyboard = std::make_unique<CountingKeyboard>(painted_);
    devices.external = std::make_unique<CountingExternal>(painted_);
    LightingController controller(new StubWorker(std::move(devices), kShortIdleMs),
                                  LightingController::kApplyWatchdogMs);
    controller.setResumeWatched(true);

    QTRY_VERIFY_WITH_TIMEOUT(painted_->zoneCalls.load() > 0 && !painted_->open.load(), 5000);
    const int firstPaint = painted_->zoneCalls.load();
    const int opensBefore = painted_->opens.load();

    constexpr int kMissingPolls = 3;
    painted_->missingOpens.store(kMissingPolls);
    controller.onSystemResumed();

    QTRY_VERIFY_WITH_TIMEOUT(painted_->zoneCalls.load() >= firstPaint * 2, 10000);
    QCOMPARE(painted_->opens.load(), opensBefore + kMissingPolls + 1);
    QTRY_VERIFY_WITH_TIMEOUT(controller.status(Domain::External).ok(), 5000);

    controller.shutdown();
}

void TestDeviceWorkerLock::theSleepMonitorReportsOnlyTheWayBackUp() {
    SleepMonitor monitor(QDBusConnection(QStringLiteral("alienfx-test-no-bus")));
    QVERIFY2(!monitor.watching(), "a bus that is not there cannot be watched");

    QSignalSpy resumed(&monitor, &SleepMonitor::resumed);
    QMetaObject::invokeMethod(&monitor, "handlePrepareForSleep", Q_ARG(bool, true));
    QCOMPARE(resumed.count(), 0);
    QMetaObject::invokeMethod(&monitor, "handlePrepareForSleep", Q_ARG(bool, false));
    QCOMPARE(resumed.count(), 1);
}

QTEST_GUILESS_MAIN(TestDeviceWorkerLock)
#include "test_device_worker_lock.moc"
