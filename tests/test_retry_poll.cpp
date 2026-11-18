#include <QtTest>
#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

#include "app/apply_mode.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"
#include "fake_devices.h"

// QCOMPARE on a scoped enum would pick up alienfx::toString() by ADL, which returns a
// string_view where QTest wants a const char*. Compare the underlying values instead.
static int asInt(alienfx::DeviceState state) { return static_cast<int>(state); }

using namespace alienfx;
using namespace alienfx::test;

namespace {

ResolvedScheme lit() { return resolveOutput(LightingState{defaultScheme(), true}); }

// A controller that is not enumerable yet and turns up part way through the poll, which is
// the case this whole wait exists for.
class LateExternalDevice final : public IExternalDevice {
   public:
    explicit LateExternalDevice(int attemptsBeforeItAppears)
        : attemptsLeft_(attemptsBeforeItAppears) {}

    bool open() override {
        if (attemptsLeft_ > 0) {
            --attemptsLeft_;
            status_ = DomainStatus{DeviceState::Missing, "fake: not enumerated yet"};
            return false;
        }
        open_ = true;
        status_ = DomainStatus{DeviceState::Ready, {}};
        return true;
    }

    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override { return true; }

    bool applyZones(const std::vector<ExternalZoneWrite>& zones) override {
        zoneCalls += static_cast<int>(zones.size());
        return true;
    }

    bool commit() override { return true; }
    [[nodiscard]] DomainStatus status() const override { return status_; }

    int zoneCalls = 0;

   private:
    int attemptsLeft_;
    bool open_ = false;
    DomainStatus status_;
};

}  // namespace

// The headless poll waits up to ten seconds for a controller that has not re-enumerated yet,
// which on the resume path is the unlock screen. The keyboard controller IS the interface the
// user types on, so whatever else this poll does, it must not sit on that claim while waiting
// for the other controller - and for anyone whose udev rule tags only the keyboard device,
// "waiting for the other controller" is every single run, for the full duration.
class TestRetryPoll : public QObject {
    Q_OBJECT

   private slots:
    void aSucceededKeyboardIsReleasedWhileTheExternalIsStillBeingWaitedFor();
    void aSucceededDomainIsNeverReapplied();
    void aControllerThatComesBackLateIsStillPainted();
    void aRefusedCommandIsNotRetried();
    void aCancelledPollStopsWaitingRatherThanRunningOutItsBudget();

   private:
    static constexpr auto kPollTimeout = std::chrono::milliseconds(300);
    static constexpr auto kRetryInterval = std::chrono::milliseconds(20);
};

void TestRetryPoll::aSucceededKeyboardIsReleasedWhileTheExternalIsStillBeingWaitedFor() {
    auto keyboard = std::make_unique<FakeKeyboardDevice>();
    auto external = std::make_unique<FakeExternalDevice>();
    FakeKeyboardDevice* keyboardView = keyboard.get();
    external->failOpen = true;

    LightingEngine engine(std::move(keyboard), std::move(external));
    std::ostringstream err;
    const ApplyResult result =
        applyDomainsWithRetry(engine, lit(), kPollTimeout, kRetryInterval, err);

    QVERIFY(result.keyboard.ok());
    QCOMPARE(asInt(result.external.state), asInt(DeviceState::Busy));

    QVERIFY2(!engine.isDeviceOpen(Domain::Keyboard),
             "the keyboard was left claimed while the poll waited for the other controller");
    QCOMPARE(keyboardView->closeCount, 1);
}

// Releasing a satisfied domain is only half of it. Re-opening and re-sending it on every one
// of the retries would hand the interface back and take it away again twenty times over,
// exercising the kernel-driver reattach each cycle for no benefit.
void TestRetryPoll::aSucceededDomainIsNeverReapplied() {
    auto keyboard = std::make_unique<FakeKeyboardDevice>();
    auto external = std::make_unique<FakeExternalDevice>();
    FakeKeyboardDevice* keyboardView = keyboard.get();
    FakeExternalDevice* externalView = external.get();
    external->failOpen = true;

    LightingEngine engine(std::move(keyboard), std::move(external));
    std::ostringstream err;
    static_cast<void>(applyDomainsWithRetry(engine, lit(), kPollTimeout, kRetryInterval, err));

    QCOMPARE(keyboardView->openCount, 1);
    QCOMPARE(keyboardView->effectCalls.size(), std::size_t(1));
    QVERIFY2(externalView->openCount > 1, "the domain that had not settled was not retried");
}

// The reason the poll exists at all: on resume or at login a controller may simply not be
// enumerable yet, and that is worth waiting for.
void TestRetryPoll::aControllerThatComesBackLateIsStillPainted() {
    auto keyboard = std::make_unique<FakeKeyboardDevice>();
    auto external = std::make_unique<LateExternalDevice>(3);
    FakeKeyboardDevice* keyboardView = keyboard.get();
    LateExternalDevice* externalView = external.get();

    LightingEngine engine(std::move(keyboard), std::move(external));
    std::ostringstream err;
    const ApplyResult result =
        applyDomainsWithRetry(engine, lit(), kPollTimeout, kRetryInterval, err);

    QVERIFY2(result.external.ok(),
             "a controller that turned up during the poll was not painted");
    QVERIFY(externalView->zoneCalls > 0);

    // And the keyboard sat out those attempts closed, having succeeded on the first one.
    QCOMPARE(keyboardView->openCount, 1);
    QVERIFY(!engine.isDeviceOpen(Domain::Keyboard));
}

void TestRetryPoll::aRefusedCommandIsNotRetried() {
    auto keyboard = std::make_unique<FakeKeyboardDevice>();
    auto external = std::make_unique<FakeExternalDevice>();
    FakeKeyboardDevice* keyboardView = keyboard.get();
    keyboard->failBrightness = true;

    LightingEngine engine(std::move(keyboard), std::move(external));
    std::ostringstream err;
    const ApplyResult result =
        applyDomainsWithRetry(engine, lit(), kPollTimeout, kRetryInterval, err);

    QCOMPARE(asInt(result.keyboard.state), asInt(DeviceState::Error));
    QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));
    QVERIFY2(!engine.isDeviceOpen(Domain::Keyboard),
             "a domain that failed for good still held its interface");
}

// The tray's shutdown needs the worker thread back, not on time but promptly: the resume
// repaint can sit in this poll for its whole ten-second device-wait budget, and without a
// way to break the wait the stop would expire its own timeout and abandon the worker
// mid-poll (issue #12). The cancellation is checked between attempts and throughout the
// wait, so a poll told to stop returns almost at once and makes no further attempt.
void TestRetryPoll::aCancelledPollStopsWaitingRatherThanRunningOutItsBudget() {
    auto keyboard = std::make_unique<FakeKeyboardDevice>();
    auto external = std::make_unique<FakeExternalDevice>();
    FakeExternalDevice* externalView = external.get();
    external->failOpen = true;  // Busy: a state the poll keeps waiting on

    LightingEngine engine(std::move(keyboard), std::move(external));

    std::atomic<bool> cancelled{false};

    // The poll runs on its own thread the way the worker's does, and the cancellation
    // arrives from outside it the way DeviceThread::stop delivers one.
    std::jthread poll([&] {
        std::ostringstream err;
        static_cast<void>(applyDomainsWithRetry(engine, lit(), kPollTimeout, kRetryInterval,
                                                 err, [&] { return cancelled.load(); }));
    });

    // Wait until the poll is genuinely mid-wait - one attempt made, another due - then
    // cancel well inside its budget. An uncancellable poll would still owe most of that
    // budget when the cancellation lands, which is what the bound below catches.
    const auto sawRetries = std::chrono::steady_clock::now() + kPollTimeout * 4;
    while (externalView->openCount < 2 && std::chrono::steady_clock::now() < sawRetries)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    QVERIFY2(externalView->openCount >= 2, "the poll never started retrying");

    QElapsedTimer elapsed;
    elapsed.start();
    cancelled.store(true);
    poll.join();
    QVERIFY2(elapsed.elapsed() < kPollTimeout.count() / 2,
             qPrintable(QStringLiteral("the cancelled poll spent %1 ms coming back")
                            .arg(elapsed.elapsed())));

    // And nothing more reached the device after the cancellation.
    const int attempts = externalView->openCount;
    std::this_thread::sleep_for(kRetryInterval * 4);
    QCOMPARE(externalView->openCount, attempts);
}

QTEST_GUILESS_MAIN(TestRetryPoll)
#include "test_retry_poll.moc"
