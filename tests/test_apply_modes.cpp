#include <QtTest>
#include <QTemporaryDir>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>

#include "app/apply_mode.h"
#include "app/process_lock.h"

using namespace alienfx;

// The exit codes here are read by the login systemd unit and by the system-sleep hook, so
// they are a contract rather than an implementation detail.
//
// Every case below holds the device lock for the whole run, which is also what keeps the
// suite safe to run on the developer's own laptop: standing down happens before any device
// is opened, so nothing here can repaint the hardware.
class TestApplyModes : public QObject {
    Q_OBJECT

   private slots:
    void init();
    void cleanup();

    void applyStandsDownAsASuccess();
    void offStandsDownAsAFailure();
    void offReportsABrokenConfigBeforeStandingDown();
    void probeStandsDownAsAFailure();
    void probePrintsNoGuidanceWhenItPaintedNothing();

   private:
    // Long enough to exercise the wait, short enough that the suite does not sit through the
    // 25 seconds production allows.
    static constexpr auto kShortWait = std::chrono::milliseconds(200);

    std::unique_ptr<QTemporaryDir> home_;
    QByteArray previousConfigHome_;
    QByteArray previousRuntimeDir_;
};

void TestApplyModes::init() {
    home_ = std::make_unique<QTemporaryDir>();
    QVERIFY(home_->isValid());
    previousConfigHome_ = qgetenv("XDG_CONFIG_HOME");
    previousRuntimeDir_ = qgetenv("XDG_RUNTIME_DIR");
    qputenv("XDG_CONFIG_HOME", home_->path().toUtf8());
    qputenv("XDG_RUNTIME_DIR", home_->path().toUtf8());
}

void TestApplyModes::cleanup() {
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

// The holder is already painting the active scheme, which is exactly what --apply asked for,
// so losing the race is not a failure and the login unit must not be marked failed for it.
void TestApplyModes::applyStandsDownAsASuccess() {
    ProcessLock contender;
    QVERIFY(contender.acquire(kShortWait));

    std::ostringstream out;
    std::ostringstream err;
    QCOMPARE(runApplyMode(out, err, kShortWait), 0);
    QVERIFY2(!out.str().empty(), "standing down as a success has to say so on stdout");
}

// Nobody else is turning the lights off. Reporting success here would tell the user the
// lights are out while they are still on.
void TestApplyModes::offStandsDownAsAFailure() {
    ProcessLock contender;
    QVERIFY(contender.acquire(kShortWait));

    std::ostringstream out;
    std::ostringstream err;
    QVERIFY(runOffMode(out, err, kShortWait) != 0);
    QVERIFY2(!err.str().empty(), "a command that did not happen has to say so on stderr");
}

// --off reads the same config as --apply, so it reports an unreadable one the same way -
// even when it then stands down for the lock before touching a device.
void TestApplyModes::offReportsABrokenConfigBeforeStandingDown() {
    const std::filesystem::path configFile =
        std::filesystem::path(home_->path().toStdString()) / "alienfx-tray" / "config.json";
    std::filesystem::create_directories(configFile.parent_path());
    {
        std::ofstream out(configFile, std::ios::binary | std::ios::trunc);
        out << "{ not json";
    }

    ProcessLock contender;
    QVERIFY(contender.acquire(kShortWait));

    std::ostringstream out;
    std::ostringstream err;
    QVERIFY(runOffMode(out, err, kShortWait) != 0);
    QVERIFY2(err.str().find("not valid JSON") != std::string::npos,
             "a broken config must be named on stderr even when nothing was applied");
}

void TestApplyModes::probeStandsDownAsAFailure() {
    ProcessLock contender;
    QVERIFY(contender.acquire(kShortWait));

    std::ostringstream out;
    std::ostringstream err;
    QVERIFY(runProbeExternalMode(std::nullopt, out, err, kShortWait) != 0);
    QVERIFY(!err.str().empty());
}

// The worst outcome this mode has available is a confident wrong answer: guidance telling the
// user to read a zone mapping off lighting that this run never set.
void TestApplyModes::probePrintsNoGuidanceWhenItPaintedNothing() {
    ProcessLock contender;
    QVERIFY(contender.acquire(kShortWait));

    std::ostringstream out;
    std::ostringstream err;
    runProbeExternalMode(1, out, err, kShortWait);
    QVERIFY2(out.str().empty(),
             qPrintable(QStringLiteral("the probe printed guidance having painted nothing: %1")
                            .arg(QString::fromStdString(out.str()))));
}

QTEST_GUILESS_MAIN(TestApplyModes)
#include "test_apply_modes.moc"
