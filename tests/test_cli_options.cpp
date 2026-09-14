#include <QtTest>

#include "app/cli_options.h"

using namespace alienfx;

namespace {

CliRequest parse(std::initializer_list<std::string_view> arguments) {
    return parseCommandLine(std::vector<std::string_view>(arguments));
}

int asInt(CliMode mode) { return static_cast<int>(mode); }

}  // namespace

// The scan in front of QApplication. Its job is to route the headless modes the login unit
// and the resume hook depend on, hand Qt the options Qt owns, and still report a typo.
class TestCliOptions : public QObject {
    Q_OBJECT

   private slots:
    void noArgumentsRunsTheTray() { QCOMPARE(asInt(parse({}).mode), asInt(CliMode::Tray)); }

    void headlessModesAreRouted() {
        QCOMPARE(asInt(parse({"--apply"}).mode), asInt(CliMode::Apply));
        QCOMPARE(asInt(parse({"--off"}).mode), asInt(CliMode::Off));
        QCOMPARE(asInt(parse({"--version"}).mode), asInt(CliMode::Version));
        QCOMPARE(asInt(parse({"--help"}).mode), asInt(CliMode::Help));
        QCOMPARE(asInt(parse({"-h"}).mode), asInt(CliMode::Help));
    }

    void probeTakesAnOptionalLightId() {
        const CliRequest whole = parse({"--probe-external"});
        QCOMPARE(asInt(whole.mode), asInt(CliMode::ProbeExternal));
        QVERIFY(!whole.probeLightId.has_value());

        const CliRequest single = parse({"--probe-external=3"});
        QCOMPARE(asInt(single.mode), asInt(CliMode::ProbeExternal));
        QCOMPARE(single.probeLightId.value_or(-1), 3);
    }

    void aNonNumericProbeIdIsRejected() {
        const CliRequest request = parse({"--probe-external=abc"});
        QCOMPARE(asInt(request.mode), asInt(CliMode::Error));
        QVERIFY(!request.message.empty());
    }

    // The reason the pass-through exists at all: an X11 session manager restores this
    // process with `-session <id>`, and a tray that refuses to start then is useless.
    void qtStandardOptionsStartTheTray() {
        QCOMPARE(asInt(parse({"-session", "abc123"}).mode), asInt(CliMode::Tray));
        QCOMPARE(asInt(parse({"-style", "Fusion"}).mode), asInt(CliMode::Tray));
        QCOMPARE(asInt(parse({"--style", "Fusion"}).mode), asInt(CliMode::Tray));
        QCOMPARE(asInt(parse({"-style=Fusion"}).mode), asInt(CliMode::Tray));
        QCOMPARE(asInt(parse({"-widgetcount"}).mode), asInt(CliMode::Tray));
        QCOMPARE(asInt(parse({"-platform", "wayland"}).mode), asInt(CliMode::Tray));
    }

    // The value of `-style` is a bare token that happens to look like nothing we know.
    // Consuming exactly one of them must not hide a flag that follows.
    void anOptionValueIsConsumedButNothingBeyondIt() {
        QCOMPARE(asInt(parse({"-style", "Fusion", "--apply"}).mode), asInt(CliMode::Apply));
        QCOMPARE(asInt(parse({"-widgetcount", "--apply"}).mode), asInt(CliMode::Apply));
    }

    void aMistypedHeadlessFlagIsReportedRatherThanStartingTheTray() {
        for (std::string_view typo : {"--aply", "--of", "-styl", "---style"}) {
            const CliRequest request = parseCommandLine({typo});
            QVERIFY2(request.mode == CliMode::Error, std::string(typo).c_str());
            QVERIFY2(request.message.find(typo) != std::string::npos,
                     "the message has to name the option the user actually typed");
        }
    }

    // Nothing dash-prefixed, so there is no typo to diagnose - Qt decides what it means.
    void bareTokensAreLeftToQt() {
        QCOMPARE(asInt(parse({"somefile"}).mode), asInt(CliMode::Tray));
    }
};

QTEST_GUILESS_MAIN(TestCliOptions)
#include "test_cli_options.moc"
