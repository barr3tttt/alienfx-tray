#include <QtTest>
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

constexpr const char* kOriginalScript = "#!/usr/bin/env bash\n# the original CLI script\n";

}  // namespace

// The whole point of this component is that it adds no startup mechanism of its own: it
// makes the script that the already-installed login unit and resume hook both call delegate
// here instead. These tests run against a throwaway HOME.
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

        const std::string installed = readAll(status.shimPath);
        QVERIFY(looksManaged(installed));
        QVERIFY(installed.find("--apply") != std::string::npos);
        // The fallback matters: if the binary is ever removed, login must still light up.
        QVERIFY(installed.find("alienfx-apply.pre-tray.bak") != std::string::npos);

        // Executable, or neither the login unit nor the sleep hook can run it.
        const auto permissions = std::filesystem::status(status.shimPath).permissions();
        QVERIFY((permissions & std::filesystem::perms::owner_exec) !=
                std::filesystem::perms::none);
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
    // writing a systemd unit, that is a second startup mechanism and the design says no.
    void theShimInstallsNoUnitAndNoHook() {
        std::string error;
        QVERIFY(installStartupIntegration(error));

        const std::filesystem::path home(qgetenv("HOME").toStdString());
        QVERIFY(!std::filesystem::exists(home / ".config" / "systemd" / "user" /
                                         "alienfx-tray.service"));
        QVERIFY(!std::filesystem::exists(home / ".config" / "autostart"));

        const std::string installed = readAll(applyShimPath());
        QVERIFY2(installed.find("systemctl") == std::string::npos,
                 "the shim must not enable anything of its own");
    }

   private:
    std::unique_ptr<QTemporaryDir> home_;
    QByteArray previousHome_;
};

QTEST_GUILESS_MAIN(TestStartupIntegration)
#include "test_startup_integration.moc"
