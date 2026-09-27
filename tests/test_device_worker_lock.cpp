#include <QtTest>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "app/device_thread.h"
#include "app/device_worker.h"
#include "app/metatypes.h"
#include "app/process_lock.h"
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
};

class CountingExternal final : public IExternalDevice {
   public:
    explicit CountingExternal(std::shared_ptr<Painted> painted) : painted_(std::move(painted)) {}

    bool open() override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override { return true; }

    bool applyZone(std::uint8_t, Rgb acColor, Rgb) override {
        painted_->zoneCalls.fetch_add(1);
        painted_->lastRed.store(acColor.r);
        if (acColor.r == kFirstFrameRed) painted_->sawFirstFrame.store(true);
        return true;
    }

    bool commit() override { return true; }

    [[nodiscard]] DomainStatus status() const override {
        return DomainStatus{DeviceState::Ready, {}};
    }

    static constexpr int kFirstFrameRed = 11;

   private:
    std::shared_ptr<Painted> painted_;
    bool open_ = false;
};

class StubWorker final : public DeviceWorker {
   public:
    explicit StubWorker(DeviceSet devices) : devices_(std::move(devices)) {}

   protected:
    DeviceSet createDevices() override { return std::move(devices_); }

   private:
    DeviceSet devices_;
};

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

   private:
    std::shared_ptr<Painted> painted_;
    std::unique_ptr<QTemporaryDir> runtimeDir_;
    QByteArray previousRuntimeDir_;
};

void TestDeviceWorkerLock::initTestCase() { registerMetaTypes(); }

void TestDeviceWorkerLock::init() {
    runtimeDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(runtimeDir_->isValid());
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_RUNTIME_DIR", runtimeDir_->path().toUtf8());
    painted_ = std::make_shared<Painted>();
}

void TestDeviceWorkerLock::cleanup() {
    painted_.reset();
    runtimeDir_.reset();
    if (previousRuntimeDir_.isEmpty())
        qunsetenv("XDG_RUNTIME_DIR");
    else
        qputenv("XDG_RUNTIME_DIR", previousRuntimeDir_);
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

QTEST_GUILESS_MAIN(TestDeviceWorkerLock)
#include "test_device_worker_lock.moc"
