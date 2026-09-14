#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdlib>
#include <fstream>

#include "app/startup_integration.h"

static int asInt(alienfx::ShimState state) { return static_cast<int>(state); }

using namespace alienfx;

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeExecutable(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

// A stand-in for a real program, which records that it ran and with which arguments.
std::string recordingScript(const std::filesystem::path& log) {
    return "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + log.string() + "'\nexit 0\n";
}

constexpr const char* kOriginalScript = "#!/usr/bin/env bash\n# the original CLI script\n";

}  // namespace

// The whole point of this component is that it adds no startup mechanism of its own: it
// makes the script that the already-installed login unit and resume hook both call delegate
// here instead. These tests run against a throwaway HOME.
//
// The shim is a generated executable, so the tests that care what it does run it, with a
// recording stub standing in for the tray binary. Reading the script and looking for words
// in it would pass just as happily on a script that never executes them.
class TestStartupIntegration : public QObject {
    Q_OBJECT

   private slots:
    void init() {
        home_ = std::make_unique<QTemporaryDir>();
        QVERIFY(home_->isValid());
        previousHome_ = qgetenv("HOME");
        qputenv("HOME", home_->path().toUtf8());
    }

    void cleanup() {
        qputenv("HOME", previousHome_);
        home_.reset();
    }

    void reportsMissingWhenNothingIsInstalled() {
        const IntegrationStatus status = checkStartupIntegration();
        QCOMPARE(asInt(status.shim), asInt(ShimState::Missing));
        QVERIFY(!status.isManaged());
        QVERIFY(!status.summary().empty());
    }

    void recognisesAForeignScript() {
        writeExecutable(applyShimPath(), kOriginalScript);
        QCOMPARE(asInt(checkStartupIntegration().shim), asInt(ShimState::Foreign));
    }

    void installingBacksUpTheOriginalAndDelegates() {
        writeExecutable(applyShimPath(), kOriginalScript);

        std::string error;
        QVERIFY2(installStartupIntegration(error), error.c_str());

        const IntegrationStatus status = checkStartupIntegration();
        QCOMPARE(asInt(status.shim), asInt(ShimState::Managed));
        QVERIFY(status.backupExists);
        QCOMPARE(readAll(status.backupPath), std::string(kOriginalScript));

        // Executable, or neither the login unit nor the sleep hook can run it.
        const auto permissions = std::filesystem::status(status.shimPath).permissions();
        QVERIFY((permissions & std::filesystem::perms::owner_exec) !=
                std::filesystem::perms::none);

        const std::filesystem::path log = home() / "tray-argv";
        const std::filesystem::path trayBin = home() / "stub-alienfx-tray";
        writeExecutable(trayBin, recordingScript(log));

        QCOMPARE(runInstalledShim(trayBin), 0);
        QCOMPARE(readAll(log), std::string("--apply\n"));
    }

    // The fallback matters: if the binary is ever removed, login must still light up.
    void theShimRunsTheBackupWhenTheBinaryHasGone() {
        const std::filesystem::path log = home() / "original-ran";
        writeExecutable(applyShimPath(), recordingScript(log));

        std::string error;
        QVERIFY2(installStartupIntegration(error), error.c_str());

        QCOMPARE(runInstalledShim(home() / "not-installed"), 0);
        QCOMPARE(readAll(log), std::string("\n"));
    }

    void theShimFailsLoudlyWhenThereIsNothingToRun() {
        std::string error;
        QVERIFY2(installStartupIntegration(error), error.c_str());
        QCOMPARE(runInstalledShim(home() / "not-installed"), 1);
    }

    void installingIsIdempotentAndNeverOverwritesTheFirstBackup() {
        writeExecutable(applyShimPath(), kOriginalScript);

        std::string error;
        QVERIFY(installStartupIntegration(error));
        QVERIFY(installStartupIntegration(error));

        const IntegrationStatus status = checkStartupIntegration();
        QCOMPARE(asInt(status.shim), asInt(ShimState::Managed));
        QVERIFY2(readAll(status.backupPath) == std::string(kOriginalScript),
                 "a second install overwrote the backup with our own shim");
    }

    void installingWithNothingThereCreatesTheScript() {
        std::string error;
        QVERIFY2(installStartupIntegration(error), error.c_str());

        const IntegrationStatus status = checkStartupIntegration();
        QCOMPARE(asInt(status.shim), asInt(ShimState::Managed));
        QVERIFY2(!status.backupExists, "there was nothing to back up");
    }

    // A file we could not read is the one case where losing it is unrecoverable: nobody ever
    // saw what was in it, and copying it would fail for the same reason the read did. The
    // install has to refuse and say so rather than replace it.
    void anUnreadableScriptIsNeverDestroyed() {
        writeExecutable(applyShimPath(), kOriginalScript);
        std::filesystem::permissions(applyShimPath(), std::filesystem::perms::owner_write |
                                                          std::filesystem::perms::owner_exec);
        if (checkStartupIntegration().shim != ShimState::Unreadable)
            QSKIP("this user can read the file regardless of its mode");

        std::string error;
        const bool installed = installStartupIntegration(error);
        std::filesystem::permissions(applyShimPath(), std::filesystem::perms::owner_all);

        QVERIFY2(!installed, "a file whose contents could not be read must not be replaced");
        QVERIFY2(error.find(applyShimPath().string()) != std::string::npos,
                 "the refusal has to name the file standing in the way");
        QCOMPARE(readAll(applyShimPath()), std::string(kOriginalScript));
    }

    // The collision case: a backup already exists, so the canonical path is taken, and the
    // file about to be replaced is one nobody could read. Skipping the copy destroyed it.
    void anUnreadableScriptSurvivesEvenWhenABackupAlreadyExists() {
        writeExecutable(applyShimPath(), kOriginalScript);

        std::string error;
        QVERIFY2(installStartupIntegration(error), error.c_str());
        const IntegrationStatus backed = checkStartupIntegration();
        QVERIFY(backed.backupExists);

        const std::string replacement = "#!/bin/sh\n# a script nobody got to read\n";
        writeExecutable(applyShimPath(), replacement);
        std::filesystem::permissions(applyShimPath(), std::filesystem::perms::owner_write |
                                                          std::filesystem::perms::owner_exec);
        if (checkStartupIntegration().shim != ShimState::Unreadable)
            QSKIP("this user can read the file regardless of its mode");

        const bool installed = installStartupIntegration(error);
        std::filesystem::permissions(applyShimPath(), std::filesystem::perms::owner_all);

        QVERIFY2(readAll(backed.backupPath) == std::string(kOriginalScript),
                 "the first backup is the original and must never be overwritten");
        QVERIFY2(!installed, "a file whose contents could not be read must not be replaced");
        QVERIFY2(error.find(applyShimPath().string()) != std::string::npos,
                 "the refusal has to name the file standing in the way");
        QCOMPARE(readAll(applyShimPath()), replacement);
    }

    void removingRestoresWhatWasThereBefore() {
        writeExecutable(applyShimPath(), kOriginalScript);

        std::string error;
        QVERIFY(installStartupIntegration(error));
        QVERIFY2(removeStartupIntegration(error), error.c_str());

        QCOMPARE(readAll(applyShimPath()), std::string(kOriginalScript));
        QCOMPARE(asInt(checkStartupIntegration().shim), asInt(ShimState::Foreign));
    }

    void removingWhenWeCreatedItLeavesNothingBehind() {
        std::string error;
        QVERIFY(installStartupIntegration(error));
        QVERIFY(removeStartupIntegration(error));
        QCOMPARE(asInt(checkStartupIntegration().shim), asInt(ShimState::Missing));
    }

    void removingSomebodyElsesScriptIsRefused() {
        writeExecutable(applyShimPath(), kOriginalScript);

        std::string error;
        QVERIFY2(!removeStartupIntegration(error),
                 "we must never delete a script we did not write");
        QVERIFY(!error.empty());
        QCOMPARE(readAll(applyShimPath()), std::string(kOriginalScript));
    }

    // The shim must not reach outside the two hooks that already exist. If this ever starts
    // enabling a systemd unit, that is a second startup mechanism and the design says no.
    void theShimInstallsNoUnitAndNoHook() {
        std::string error;
        QVERIFY(installStartupIntegration(error));

        QVERIFY(!std::filesystem::exists(home() / ".config" / "systemd" / "user" /
                                         "alienfx-tray.service"));
        QVERIFY(!std::filesystem::exists(home() / ".config" / "autostart"));

        const std::filesystem::path trayBin = home() / "stub-alienfx-tray";
        writeExecutable(trayBin, recordingScript(home() / "tray-argv"));

        // Anything the shim shells out to would be found here first.
        const std::filesystem::path systemctlLog = home() / "systemctl-called";
        writeExecutable(home() / "intercept" / "systemctl", recordingScript(systemctlLog));

        QCOMPARE(runInstalledShim(trayBin, home() / "intercept"), 0);
        QVERIFY2(!std::filesystem::exists(systemctlLog),
                 "the shim must not enable anything of its own");
    }

   private:
    [[nodiscard]] std::filesystem::path home() const {
        return std::filesystem::path(home_->path().toStdString());
    }

    // Runs the installed shim exactly as the login unit and the sleep hook do, with
    // ALIENFX_TRAY_BIN standing in for the installed binary.
    int runInstalledShim(const std::filesystem::path& trayBin,
                         const std::filesystem::path& pathPrefix = {}) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home_->path());
        env.insert(QStringLiteral("ALIENFX_TRAY_BIN"),
                   QString::fromStdString(trayBin.string()));
        if (!pathPrefix.empty()) {
            env.insert(QStringLiteral("PATH"),
                       QString::fromStdString(pathPrefix.string()) + QLatin1Char(':') +
                           env.value(QStringLiteral("PATH")));
        }

        QProcess shim;
        shim.setProcessEnvironment(env);
        shim.start(QString::fromStdString(applyShimPath().string()), {});
        if (!shim.waitForFinished(15000)) {
            shim.kill();
            return -1;
        }
        return shim.exitStatus() == QProcess::NormalExit ? shim.exitCode() : -1;
    }

    std::unique_ptr<QTemporaryDir> home_;
    QByteArray previousHome_;
};

QTEST_GUILESS_MAIN(TestStartupIntegration)
#include "test_startup_integration.moc"
