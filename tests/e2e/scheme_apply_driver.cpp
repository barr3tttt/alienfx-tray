// The hardware check for the editing model: drives a real LightingController through every
// scheme in the config against the real controllers, and lists every zone's commanded
// colour. This is the end-to-end proof for "selecting a scheme sets every zone, every
// time": each scheme is activated twice, and both activations must command all four
// external zones and the keyboard.
//
// Not a ctest test - it needs the laptop. tests/e2e/scheme-apply-all-zones.sh runs it and
// restores the user's lighting afterwards. Exit status is 0 only when every scheme verified.

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/device_worker.h"
#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "core/color.h"
#include "core/config.h"
#include "device/device_factory.h"
#include "device/usb_context.h"

using namespace alienfx;

namespace {

// What the engine put on the wire, recorded by decorating the real SDK devices: every call
// is forwarded to the hardware and logged, so the listing below is what the lights were
// actually told, on this run.
struct PaintLog {
    struct ZoneWrite {
        std::uint8_t lightId;
        Rgb ac;
        Rgb battery;
    };
    struct EffectWrite {
        std::uint8_t wireEffectType;
        Rgb color;
    };

    mutable QMutex mutex;
    std::vector<ZoneWrite> zones;
    std::vector<EffectWrite> effects;
    int commits = 0;

    void clear() {
        QMutexLocker lock(&mutex);
        zones.clear();
        effects.clear();
        commits = 0;
    }

    void snapshot(std::vector<ZoneWrite>& zonesOut, std::vector<EffectWrite>& effectsOut,
                  int& commitsOut) const {
        QMutexLocker lock(&mutex);
        zonesOut = zones;
        effectsOut = effects;
        commitsOut = commits;
    }

    [[nodiscard]] int zoneCount() const {
        QMutexLocker lock(&mutex);
        return static_cast<int>(zones.size());
    }
    [[nodiscard]] int effectCount() const {
        QMutexLocker lock(&mutex);
        return static_cast<int>(effects.size());
    }
    [[nodiscard]] int commitCount() const {
        QMutexLocker lock(&mutex);
        return commits;
    }
};

class LoggingKeyboard final : public IKeyboardDevice {
   public:
    LoggingKeyboard(std::unique_ptr<IKeyboardDevice> inner, std::shared_ptr<PaintLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}

    bool open() override { return inner_->open(); }
    void close() override { inner_->close(); }
    [[nodiscard]] bool isOpen() const override { return inner_->isOpen(); }

    bool setBrightness(Brightness brightness) override {
        return inner_->setBrightness(brightness);
    }

    bool applyEffect(KeyboardEffect effect, Rgb color) override {
        const bool ok = inner_->applyEffect(effect, color);
        if (ok) {
            QMutexLocker lock(&log_->mutex);
            log_->effects.push_back(PaintLog::EffectWrite{toWireValue(effect), color});
        }
        return ok;
    }

    [[nodiscard]] DomainStatus status() const override { return inner_->status(); }

   private:
    std::unique_ptr<IKeyboardDevice> inner_;
    std::shared_ptr<PaintLog> log_;
};

class LoggingExternal final : public IExternalDevice {
   public:
    LoggingExternal(std::unique_ptr<IExternalDevice> inner, std::shared_ptr<PaintLog> log)
        : inner_(std::move(inner)), log_(std::move(log)) {}

    bool open() override { return inner_->open(); }
    void close() override { inner_->close(); }
    [[nodiscard]] bool isOpen() const override { return inner_->isOpen(); }

    bool setBrightness(Brightness brightness,
                       const std::vector<std::uint8_t>& lightIds) override {
        return inner_->setBrightness(brightness, lightIds);
    }

    bool applyZones(const std::vector<ExternalZoneWrite>& zones) override {
        const bool ok = inner_->applyZones(zones);
        if (ok) {
            QMutexLocker lock(&log_->mutex);
            for (const ExternalZoneWrite& zone : zones)
                log_->zones.push_back(
                    PaintLog::ZoneWrite{zone.lightId, zone.acColor, zone.batteryColor});
        }
        return ok;
    }

    bool commit() override {
        const bool ok = inner_->commit();
        if (ok) {
            QMutexLocker lock(&log_->mutex);
            ++log_->commits;
        }
        return ok;
    }

    [[nodiscard]] DomainStatus status() const override { return inner_->status(); }

   private:
    std::unique_ptr<IExternalDevice> inner_;
    std::shared_ptr<PaintLog> log_;
};

class LoggingWorker final : public DeviceWorker {
   public:
    explicit LoggingWorker(std::shared_ptr<PaintLog> log) : log_(std::move(log)) {}

   protected:
    DeviceSet createDevices() override {
        silenceSdkLogging();
        usbContext_ = std::make_unique<UsbContext>();
        DeviceSet devices = makeSdkDevices(*usbContext_);
        devices.keyboard =
            std::make_unique<LoggingKeyboard>(std::move(devices.keyboard), log_);
        devices.external =
            std::make_unique<LoggingExternal>(std::move(devices.external), log_);
        return devices;
    }

   private:
    std::unique_ptr<UsbContext> usbContext_;
    std::shared_ptr<PaintLog> log_;
};

// Waits until a full apply's worth of commands has been logged - four zone writes, one
// keyboard effect, one commit - plus a short quiet period for stragglers. Returns false on
// timeout, which is how a missing or wedged controller shows up here. The timeout is long
// because a running tray holds the device lock for up to twenty seconds after its own last
// apply, and this driver politely waits its turn.
bool waitForFullPaint(const PaintLog& log, int timeoutMs = 45000) {
    const int stepMs = 25;
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        if (log.zoneCount() >= 4 && log.effectCount() >= 1 && log.commitCount() >= 1) {
            QThread::msleep(250);
            return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(stepMs);
    }
    return false;
}

std::string hex(Rgb color) { return toHex(color); }

// Verifies one logged paint against the scheme that was selected. Every visible zone must
// have been commanded exactly once, with the colours the scheme carries for it.
int verifyPaint(const Scheme& scheme, const PaintLog& log) {
    std::vector<PaintLog::ZoneWrite> zones;
    std::vector<PaintLog::EffectWrite> effects;
    int commits = 0;
    log.snapshot(zones, effects, commits);

    int failures = 0;
    std::printf("  keyboard        %s\n", effects.empty() ? "NOTHING COMMANDED"
                                                         : hex(effects.back().color).c_str());
    if (effects.size() != 1 || effects.back().color != scheme.keyboard.color) {
        std::printf("  FAIL: keyboard commanded %zu time(s), want 1 at %s\n", effects.size(),
                    hex(scheme.keyboard.color).c_str());
        ++failures;
    }

    for (const ExternalZone& zone : scheme.external.zones) {
        int writes = 0;
        PaintLog::ZoneWrite last{};
        for (const PaintLog::ZoneWrite& write : zones) {
            if (write.lightId == zone.lightId) {
                ++writes;
                last = write;
            }
        }
        std::printf("  zone %u %-14s ac %s  battery %s\n", zone.lightId, zone.name.c_str(),
                    writes ? hex(last.ac).c_str() : "NOTHING COMMANDED",
                    writes ? hex(last.battery).c_str() : "");
        if (writes != 1 || last.ac != zone.color.ac ||
            last.battery != zone.color.effectiveBattery()) {
            std::printf("  FAIL: zone %u commanded %d time(s), want 1 at ac %s battery %s\n",
                        zone.lightId, writes, hex(zone.color.ac).c_str(),
                        hex(zone.color.effectiveBattery()).c_str());
            ++failures;
        }
    }

    if (commits != 1) {
        std::printf("  FAIL: %d commits, want 1\n", commits);
        ++failures;
    }
    return failures;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    registerMetaTypes();

    // Seed the scratch config the wrapper points at us with the shipped schemes, so the
    // listing below covers each built-in exactly as it ships.
    Config seed = Config::defaults();
    std::string error;
    if (!saveConfig(seed, configPath(), error)) {
        std::fprintf(stderr, "scheme-apply-driver: could not seed config: %s\n",
                     error.c_str());
        return 2;
    }

    auto log = std::make_shared<PaintLog>();
    LightingController controller(new LoggingWorker(log), LightingController::kApplyWatchdogMs);

    // The constructor paints the active scheme; wait for that to finish before measuring.
    if (!waitForFullPaint(*log)) {
        std::fprintf(stderr,
                     "scheme-apply-driver: the initial paint never completed - are both "
                     "controllers attached and free?\n");
        return 2;
    }

    int failures = 0;
    const std::vector<Scheme> schemes = controller.config().schemes;
    for (const Scheme& scheme : schemes) {
        const QString name = QString::fromStdString(scheme.name);
        std::printf("scheme \"%s\"\n", scheme.name.c_str());

        log->clear();
        controller.activateScheme(name);
        if (!waitForFullPaint(*log)) {
            std::printf("  FAIL: activation never painted a full scheme\n");
            ++failures;
            continue;
        }
        std::printf("  activation:\n");
        failures += verifyPaint(scheme, *log);

        // The gesture that used to command nothing at all: select the scheme that is
        // already active. It must set every zone again.
        log->clear();
        controller.activateScheme(name);
        if (!waitForFullPaint(*log)) {
            std::printf("  FAIL: re-selecting the active scheme commanded nothing\n");
            ++failures;
            continue;
        }
        std::printf("  re-select:\n");
        failures += verifyPaint(scheme, *log);
    }

    std::printf("\nscheme-apply-driver: %zu schemes, %d failure(s)\n", schemes.size(),
                failures);

    controller.shutdown();
    return failures == 0 ? 0 : 1;
}
