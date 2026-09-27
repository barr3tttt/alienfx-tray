#include "app/lighting_controller.h"

#include <QTimer>

#include <utility>

#include "app/device_thread.h"
#include "app/device_worker.h"

namespace alienfx {

LightingController::LightingController(QObject* parent)
    : LightingController(new DeviceWorker, kApplyWatchdogMs, parent) {}

LightingController::LightingController(DeviceWorker* worker, int applyWatchdogMs,
                                       QObject* parent)
    : QObject(parent) {
    LoadResult loaded = loadConfig(configPath());
    config_ = std::move(loaded.config);
    if (!loaded.error.empty()) {
        // A config we cannot read must never mean no lighting. Fall back to the defaults and
        // say so in the UI rather than starting dark.
        setConfigError(QString::fromStdString(loaded.error));
    }
    working_ = config_.activeScheme();

    // First run: write the defaults out straight away so there is a file to read and edit,
    // and so `--apply` at login has something concrete rather than falling back every time.
    if (!loaded.fileExisted) {
        std::string error;
        if (!saveConfig(config_, configPath(), error))
            setConfigError(QString::fromStdString(error));
    }

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kConfigSaveDebounceMs);
    connect(saveTimer_, &QTimer::timeout, this, &LightingController::saveConfigNow);

    watchdog_ = new QTimer(this);
    watchdog_->setSingleShot(true);
    watchdog_->setInterval(applyWatchdogMs);
    connect(watchdog_, &QTimer::timeout, this, &LightingController::onApplyWatchdog);

    deviceThread_ = std::make_unique<DeviceThread>(worker);
    connect(deviceThread_.get(), &DeviceThread::statusChanged, this,
            &LightingController::onWorkerStatus);
    connect(deviceThread_.get(), &DeviceThread::applyFinished, this,
            &LightingController::onApplyFinished);
    connect(deviceThread_.get(), &DeviceThread::applyDeferred, this,
            &LightingController::onApplyDeferred);

    // Paint what the config says as soon as we are up.
    submitCurrent();
}

LightingController::~LightingController() { shutdown(); }

DomainStatus LightingController::status(Domain domain) const {
    return domain == Domain::Keyboard ? keyboardStatus_ : externalStatus_;
}

void LightingController::submitCurrent() {
    if (!deviceThread_) return;
    pendingGeneration_ =
        deviceThread_->submit(resolveOutput(LightingState{working_, config_.lightsOn}));
    if (watchdog_) watchdog_->start();
}

void LightingController::applyWorkingScheme(const Scheme& scheme) {
    working_ = scheme;
    config_.upsertScheme(working_);
    // Keeping the user's last explicit activation intact: the active scheme only moves when
    // they deliberately pick it from the tray menu, double-click it in the scheme list, or
    // click "Activate".  Live-previewing an edit must not silently change which scheme the
    // tray checkmark and login hook consider active.
    submitCurrent();
    if (saveTimer_) saveTimer_->start();
}

void LightingController::activateScheme(const QString& name) {
    const std::string key = name.toStdString();
    const Scheme* scheme = config_.findScheme(key);
    if (!scheme) return;

    config_.activeSchemeName = key;
    working_ = *scheme;
    submitCurrent();
    saveConfigNow();
    emit schemeChanged();
}

void LightingController::setLightsOn(bool on) {
    if (config_.lightsOn == on) return;
    config_.lightsOn = on;
    submitCurrent();
    saveConfigNow();
    emit lightsOnChanged(on);
}

void LightingController::saveWorkingSchemeAs(const QString& name) {
    const std::string key = name.toStdString();
    if (key.empty()) return;

    Scheme scheme = working_;
    scheme.name = key;
    config_.upsertScheme(scheme);
    config_.activeSchemeName = key;
    working_ = scheme;
    saveConfigNow();
    emit schemeListChanged();
    emit schemeChanged();
}

void LightingController::deleteScheme(const QString& name) {
    if (config_.schemes.size() <= 1) return;  // never leave the user with nothing to pick
    if (!config_.removeScheme(name.toStdString())) return;

    working_ = config_.activeScheme();
    submitCurrent();
    saveConfigNow();
    emit schemeListChanged();
    emit schemeChanged();
}

void LightingController::reapply() { submitCurrent(); }

void LightingController::flushPendingSave() {
    if (saveTimer_ && saveTimer_->isActive()) saveConfigNow();
}

void LightingController::onWorkerStatus(Domain domain, DomainStatus status) {
    (domain == Domain::Keyboard ? keyboardStatus_ : externalStatus_) = status;
    emit statusChanged(domain, status);
}

void LightingController::onApplyFinished(quint64 generation, bool) {
    if (generation < pendingGeneration_) return;

    if (watchdog_) watchdog_->stop();
    if (!watchdogTripped_) return;

    // The apply came back after all, so the stamp below was wrong. Any status the worker
    // does not consider changed it will never re-send - the slow case is a scheme with many
    // external zones, where 175 ms each outruns the watchdog and then reports Ready again -
    // so put back what it last told us rather than leaving a healthy controller labelled dead.
    watchdogTripped_ = false;
    for (Domain domain : kAllDomains) {
        DomainStatus& slot = domain == Domain::Keyboard ? keyboardStatus_ : externalStatus_;
        if (slot.state != DeviceState::Unresponsive) continue;
        slot = domain == Domain::Keyboard ? keyboardBeforeWatchdog_ : externalBeforeWatchdog_;
        emit statusChanged(domain, slot);
    }
}

void LightingController::onApplyDeferred(quint64) {
    // Proof of life. The worker is waiting on another process rather than stuck in a device
    // call, so the deadline starts again: without this the watchdog would eventually call a
    // perfectly responsive controller unresponsive for being patient.
    if (watchdog_) watchdog_->start();
}

DomainStatus LightingController::watchdogStamp() {
    return DomainStatus{DeviceState::Unresponsive, {}};
}

bool LightingController::escalatesOnWedge(DeviceState state) {
    switch (state) {
        case DeviceState::Ready:
        case DeviceState::Unknown:
        case DeviceState::Busy:
        case DeviceState::Waiting:
            return true;
        case DeviceState::Missing:
        case DeviceState::Error:
            return false;
        case DeviceState::Unresponsive:
            // Already said. Escalating it again would overwrite the pre-watchdog status
            // with the stamp itself, and the restore would then put the stamp back.
            return false;
    }
    std::unreachable();
}

void LightingController::onApplyWatchdog() {
    // The apply never came back. Rather than let the UI imply everything is fine, say what
    // is actually true: the worker is stuck on a device that is not answering. Which of the
    // two it is stuck on is unknowable from here, so every domain still promising progress
    // is escalated rather than guessing one.
    for (Domain domain : kAllDomains) {
        DomainStatus& slot = domain == Domain::Keyboard ? keyboardStatus_ : externalStatus_;
        if (!escalatesOnWedge(slot.state)) continue;

        // The stamp says nothing about the kernel driver, and a driverless interface costs
        // the user a working keyboard until reboot. It outlives whatever the lighting is
        // doing, so it crosses over rather than being reset by a fresh DomainStatus.
        DomainStatus stuck = watchdogStamp();
        stuck.interfaceLeftDetached = slot.interfaceLeftDetached;

        (domain == Domain::Keyboard ? keyboardBeforeWatchdog_ : externalBeforeWatchdog_) = slot;
        slot = stuck;
        watchdogTripped_ = true;
        emit statusChanged(domain, slot);
    }
}

void LightingController::saveConfigNow() {
    if (saveTimer_) saveTimer_->stop();
    std::string error;
    // A successful write also retires an unreadable-config complaint: the file on disk is
    // now ours and parseable, so the warning would be describing a state that is over.
    setConfigError(saveConfig(config_, configPath(), error) ? QString()
                                                            : QString::fromStdString(error));
}

void LightingController::setConfigError(QString error) {
    if (configError_ == error) return;
    configError_ = std::move(error);
    emit configErrorChanged(configError_);
}

void LightingController::shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    if (saveTimer_ && saveTimer_->isActive()) saveConfigNow();

    // The SDK's APIv4 ready-wait can spin without a timeout of its own, and quitting must
    // not depend on a wedged controller changing its mind. DeviceThread::stop bounds the
    // wait and abandons the thread if it expires; the kernel reattaches usbhid when the
    // usbfs descriptor closes on exit, so the keyboard comes back either way.
    if (deviceThread_) deviceThread_->stop(DeviceThread::kStopTimeout);
}

}  // namespace alienfx
