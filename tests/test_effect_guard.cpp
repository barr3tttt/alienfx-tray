#include <QtTest>

#include <memory>

#include "core/effect.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"
#include "fake_devices.h"

using namespace alienfx;
using namespace alienfx::test;

// ---------------------------------------------------------------------------------------
// This file is the guard around the single worst failure mode this hardware has.
//
// AlienFX_SDK.cpp, SetGlobalEffects, API_V5:
//
//     if (!effType) PrepareAndSend(COMMV5_setEffect, {{2, {1, 0xfe}}});
//
// An effect type of 0 is a clear/disable. It leaves the keyboard dark in a state that no
// later colour set - `setall` included - can override. The external controller carries on
// working, so the machine looks half-broken, and the only recoveries are a non-zero effect
// type or a reboot.
//
// If any of these tests fail, the application can emit that command. Do not relax them.
// ---------------------------------------------------------------------------------------

class TestEffectGuard : public QObject {
    Q_OBJECT

   private slots:
    // The table the whole guard rests on.
    void noEnumeratorIsZero() {
        QVERIFY(!kAllKeyboardEffects.empty());
        for (KeyboardEffect effect : kAllKeyboardEffects)
            QVERIFY2(static_cast<std::uint8_t>(effect) != 0,
                     "a KeyboardEffect enumerator is zero");
    }

    // toWireValue is a lookup, not a cast, so its range is exactly the enumerator table.
    // Walk every value the underlying type can hold - including ones no enumerator names,
    // and including 0 itself - and prove none of them can produce a zero wire byte.
    void noByteValueCanProduceZero() {
        for (int raw = 0; raw <= 0xff; ++raw) {
            const auto effect = static_cast<KeyboardEffect>(static_cast<std::uint8_t>(raw));
            QVERIFY2(toWireValue(effect) != 0,
                     qPrintable(QStringLiteral("toWireValue produced 0 for raw value %1")
                                    .arg(raw)));
        }
    }

    // Config parsing is an input path from a file a human can edit. It must also be unable
    // to reach zero.
    void parsingUnknownNamesCannotProduceZero() {
        const char* names[] = {"", "off", "none", "0", "disable", "clear", "steady", "STEADY",
                               "\xff\xfe"};
        for (const char* name : names)
            QVERIFY2(toWireValue(parseEffect(name)) != 0, name);
    }

    void roundTripsThroughItsOwnName() {
        // Compared as wire bytes rather than as enum values: QCOMPARE would otherwise pick
        // up alienfx::toString(KeyboardEffect) by ADL and fail to build, and the byte is the
        // thing that actually matters here anyway.
        for (KeyboardEffect effect : kAllKeyboardEffects)
            QCOMPARE(toWireValue(parseEffect(toString(effect))), toWireValue(effect));
    }

    // The end-to-end proof: drive the real engine over every scheme shape the application
    // can produce, including "lights off" and pure black, and assert that nothing the
    // device would have received was ever a zero effect type.
    void engineNeverEmitsZeroEffectType() {
        std::vector<Scheme> schemes = builtinSchemes();

        Scheme black = defaultScheme();
        black.keyboard.color = Rgb{0, 0, 0};
        black.keyboard.brightness = Brightness(Brightness::kMin);
        for (ExternalZone& zone : black.external.zones) zone.color = uniform(Rgb{0, 0, 0});
        schemes.push_back(black);

        Scheme noZones = defaultScheme();
        noZones.external.zones.clear();
        schemes.push_back(noZones);

        int appliedEffects = 0;
        for (const Scheme& scheme : schemes) {
            for (bool lightsOn : {true, false}) {
                auto keyboard = std::make_unique<FakeKeyboardDevice>();
                auto external = std::make_unique<FakeExternalDevice>();
                FakeKeyboardDevice* keyboardView = keyboard.get();

                LightingEngine engine(std::move(keyboard), std::move(external));
                engine.apply(resolveOutput(LightingState{scheme, lightsOn}));

                QVERIFY(!keyboardView->effectCalls.empty());
                for (const auto& call : keyboardView->effectCalls) {
                    QVERIFY2(call.wireEffectType != 0,
                             "the engine emitted effect type 0 - this bricks the keyboard "
                             "until reboot");
                    ++appliedEffects;
                }
            }
        }
        QVERIFY(appliedEffects > 0);
    }

    // Turning the lights off must be a black colour, never a disable and never zero
    // brightness. Both of the alternatives are unrecoverable on this device.
    void lightsOffIsBlackNotDisabled() {
        Scheme scheme = defaultScheme();

        auto keyboard = std::make_unique<FakeKeyboardDevice>();
        auto external = std::make_unique<FakeExternalDevice>();
        FakeKeyboardDevice* keyboardView = keyboard.get();
        FakeExternalDevice* externalView = external.get();

        LightingEngine engine(std::move(keyboard), std::move(external));
        engine.apply(resolveOutput(LightingState{scheme, false}));

        QCOMPARE(keyboardView->effectCalls.size(), std::size_t(1));
        QCOMPARE(keyboardView->effectCalls.front().color, (Rgb{0, 0, 0}));
        QVERIFY(keyboardView->effectCalls.front().wireEffectType != 0);

        QCOMPARE(keyboardView->brightnessCalls.size(), std::size_t(1));
        QVERIFY2(keyboardView->brightnessCalls.front() >= Brightness::kMin,
                 "brightness 0 is the other way to strand this controller dark");

        for (const auto& zone : externalView->zoneCalls) {
            QCOMPARE(zone.acColor, (Rgb{0, 0, 0}));
            QCOMPARE(zone.batteryColor, (Rgb{0, 0, 0}));
        }
    }

    // Brightness is a type, not an int, for the same reason the effect is. The invariant is
    // that nothing can drive it to zero - not that any particular input maps to kMin.
    void brightnessCannotReachZero() {
        QCOMPARE(Brightness(0).value(), Brightness::kMin);
        QCOMPARE(Brightness(-100).value(), Brightness::kMin);
        QCOMPARE(Brightness(999).value(), Brightness::kMax);
        QCOMPARE(Brightness::fromPercent(100).value(), Brightness::kMax);

        for (int percent = -50; percent <= 150; ++percent) {
            QVERIFY2(Brightness::fromPercent(percent).value() >= Brightness::kMin,
                     qPrintable(QStringLiteral("brightness reached zero at %1%").arg(percent)));
        }
        for (int raw = -50; raw <= 400; ++raw)
            QVERIFY(Brightness(raw).value() >= Brightness::kMin);

        // The display side of the same floor: the slider's range starts at 1%, so the
        // minimum brightness must not read back as 0.
        for (int raw = 0; raw <= 0xff; ++raw) {
            const int percent = Brightness(raw).percent();
            QVERIFY2(percent >= 1 && percent <= 100,
                     qPrintable(QStringLiteral("percent %1 out of range for raw %2")
                                    .arg(percent)
                                    .arg(raw)));
        }
    }
};

QTEST_GUILESS_MAIN(TestEffectGuard)
#include "test_effect_guard.moc"
