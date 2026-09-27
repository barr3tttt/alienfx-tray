#pragma once

#include <QMutex>
#include <QMutexLocker>

#include <memory>
#include <utility>
#include <vector>

#include "app/device_worker.h"
#include "core/device_interface.h"
#include "core/scheme.h"

namespace alienfx::test {

// Everything the worker thread is told, recorded under a lock because the engine runs on
// the device thread while the test thread asserts. The editing-model tests share this
// harness: one drives the controller directly, the other drives the real control panel,
// and both need to know exactly what would have gone on the wire.
struct DeviceRecord {
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

    [[nodiscard]] std::vector<ZoneWrite> zoneSnapshot() const {
        QMutexLocker lock(&mutex);
        return zones;
    }

    [[nodiscard]] std::vector<EffectWrite> effectSnapshot() const {
        QMutexLocker lock(&mutex);
        return effects;
    }
};

class RecordingKeyboard final : public IKeyboardDevice {
   public:
    explicit RecordingKeyboard(std::shared_ptr<DeviceRecord> record)
        : record_(std::move(record)) {}

    bool open() override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness) override { return true; }

    bool applyEffect(KeyboardEffect effect, Rgb color) override {
        QMutexLocker lock(&record_->mutex);
        // toWireValue is the production conversion, deliberately: the record holds the byte
        // the device would have been sent.
        record_->effects.push_back(DeviceRecord::EffectWrite{toWireValue(effect), color});
        return true;
    }

    [[nodiscard]] DomainStatus status() const override {
        return DomainStatus{DeviceState::Ready, {}};
    }

   private:
    std::shared_ptr<DeviceRecord> record_;
    bool open_ = false;
};

class RecordingExternal final : public IExternalDevice {
   public:
    explicit RecordingExternal(std::shared_ptr<DeviceRecord> record)
        : record_(std::move(record)) {}

    bool open() override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    bool setBrightness(Brightness, const std::vector<std::uint8_t>&) override { return true; }

    bool applyZone(std::uint8_t lightId, Rgb acColor, Rgb batteryColor) override {
        QMutexLocker lock(&record_->mutex);
        record_->zones.push_back(DeviceRecord::ZoneWrite{lightId, acColor, batteryColor});
        return true;
    }

    bool commit() override {
        QMutexLocker lock(&record_->mutex);
        ++record_->commits;
        return true;
    }

    [[nodiscard]] DomainStatus status() const override {
        return DomainStatus{DeviceState::Ready, {}};
    }

   private:
    std::shared_ptr<DeviceRecord> record_;
    bool open_ = false;
};

class RecordingWorker final : public DeviceWorker {
   public:
    explicit RecordingWorker(std::shared_ptr<DeviceRecord> record)
        : record_(std::move(record)) {}

   protected:
    DeviceSet createDevices() override {
        DeviceSet devices;
        devices.keyboard = std::make_unique<RecordingKeyboard>(record_);
        devices.external = std::make_unique<RecordingExternal>(record_);
        return devices;
    }

   private:
    std::shared_ptr<DeviceRecord> record_;
};

// A scheme with one colour on the keyboard and every default external zone.
[[nodiscard]] inline Scheme solidScheme(const std::string& name, Rgb color) {
    Scheme scheme;
    scheme.name = name;
    scheme.keyboard.color = color;
    scheme.external = defaultExternalSettings(color);
    return scheme;
}

}  // namespace alienfx::test
