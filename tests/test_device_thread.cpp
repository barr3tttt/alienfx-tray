#include <QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>

#include "app/device_thread.h"
#include "app/device_worker.h"
#include "app/metatypes.h"
#include "core/device_interface.h"
#include "core/scheme.h"
#include "fake_devices.h"

using namespace alienfx;

namespace {

// Owned by the test rather than by the device, so it stays readable after the worker - and
// with it the device - has been deleted on the thread we walked away from.
struct WedgeState {
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    std::atomic<bool> returned{false};
};

// Stands in for the failure the SDK cannot protect us from: a controller that answers ready
// and then latches busy inside SetPowerAction, where WaitForReady()'s APIv4 branch spins
// with no timeout. The call comes back when the test says so and not before.
class WedgingExternalDevice final : public IExternalDevice {
   public:
    explicit WedgingExternalDevice(std::shared_ptr<WedgeState> state)
        : state_(std::move(state)) {}

    bool open() override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override { return true; }

    bool applyZone(std::uint8_t, Rgb, Rgb) override {
        state_->entered.store(true);
        while (!state_->released.load()) QThread::msleep(5);
        state_->returned.store(true);
        return true;
    }

    bool commit() override { return true; }

    [[nodiscard]] DomainStatus status() const override {
        return DomainStatus{DeviceState::Ready, {}};
    }

   private:
    std::shared_ptr<WedgeState> state_;
    bool open_ = false;
};

// The devices are built on the test thread and handed over, so the test never races the
// worker thread to learn what it is driving.
class StubWorker final : public DeviceWorker {
   public:
    explicit StubWorker(DeviceSet devices) : devices_(std::move(devices)) {}

   protected:
    DeviceSet createDevices() override { return std::move(devices_); }

   private:
    DeviceSet devices_;
};

}  // namespace

// The device thread is a thread that may never come back: the SDK's APIv4 ready-wait is an
// unbounded spin, and nothing in this process is allowed to join it. This is the structural
// guard for that. It parks a real DeviceWorker inside a device call and then stops it, so a
// future change that turns DeviceThread::stop into an unbounded join - or adds a join
// anywhere on the shutdown path - does not fail late here, it never returns at all and ctest
// kills the test.
class TestDeviceThread : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase();
    void cleanupTestCase();
    void stopAbandonsAWedgedWorkerRatherThanJoiningIt();
    void stopReturnsTrueWhenTheWorkerIsHealthy();

   private:
    std::unique_ptr<QTemporaryDir> runtimeDir_;
    QByteArray previousRuntimeDir_;
};

void TestDeviceThread::initTestCase() {
    registerMetaTypes();

    // A private lock directory. A tray running on the developer's own desktop holds
    // $XDG_RUNTIME_DIR/alienfx-tray.lock, which would make the worker report Waiting and
    // defer the device call this test needs it to reach.
    runtimeDir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(runtimeDir_->isValid());
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_RUNTIME_DIR", runtimeDir_->path().toUtf8());
}

void TestDeviceThread::cleanupTestCase() {
    if (previousRuntimeDir_.isEmpty())
        qunsetenv("XDG_RUNTIME_DIR");
    else
        qputenv("XDG_RUNTIME_DIR", previousRuntimeDir_);
}

void TestDeviceThread::stopAbandonsAWedgedWorkerRatherThanJoiningIt() {
    auto wedge = std::make_shared<WedgeState>();

    DeviceSet devices;
    devices.keyboard = std::make_unique<test::FakeKeyboardDevice>();
    devices.external = std::make_unique<WedgingExternalDevice>(wedge);

    auto thread = std::make_unique<DeviceThread>(new StubWorker(std::move(devices)));

    Scheme scheme;
    scheme.external.zones.push_back(ExternalZone{0, "probe", uniform(Rgb{255, 0, 0})});
    QVERIFY(thread->submit(resolveOutput(LightingState{scheme, true})) > 0);

    QTRY_VERIFY_WITH_TIMEOUT(wedge->entered.load(), 5000);

    QElapsedTimer elapsed;
    elapsed.start();
    const bool stopped = thread->stop(std::chrono::milliseconds(200));
    const qint64 waited = elapsed.elapsed();

    QVERIFY(!stopped);
    QVERIFY2(waited < 2000,
             qPrintable(QStringLiteral("stop() waited %1 ms on a wedged worker").arg(waited)));

    // Let the abandoned thread unwind before the process ends.
    wedge->released.store(true);
    QTRY_VERIFY_WITH_TIMEOUT(wedge->returned.load(), 5000);
    QTest::qWait(250);
    thread.reset();
}

void TestDeviceThread::stopReturnsTrueWhenTheWorkerIsHealthy() {
    DeviceSet devices;
    devices.keyboard = std::make_unique<test::FakeKeyboardDevice>();
    devices.external = std::make_unique<test::FakeExternalDevice>();

    DeviceThread thread(new StubWorker(std::move(devices)));

    QSignalSpy finished(&thread, &DeviceThread::applyFinished);
    Scheme scheme;
    scheme.external.zones.push_back(ExternalZone{0, "probe", uniform(Rgb{0, 255, 0})});
    const quint64 generation = thread.submit(resolveOutput(LightingState{scheme, true}));

    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.first().at(0).value<quint64>(), generation);
    QVERIFY(thread.stop(DeviceThread::kStopTimeout));
}

QTEST_GUILESS_MAIN(TestDeviceThread)
#include "test_device_thread.moc"
