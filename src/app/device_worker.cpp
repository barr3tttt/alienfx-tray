#include "app/device_worker.h"

#include <QMetaObject>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

#include "app/apply_mode.h"
#include "device/device_factory.h"
#include "device/usb_context.h"

namespace alienfx {

DeviceWorker::DeviceWorker(QObject* parent) : QObject(parent) {}

DeviceWorker::~DeviceWorker() {
    // Last line of defence. If we get here with devices still open - a crash on the way out,
    // an exception, a missed shutdown() - this is what hands the keyboard interface back to
    // the kernel before the process disappears.
    if (engine_) engine_->releaseDevices();
}

DeviceSet DeviceWorker::createDevices() {
    silenceSdkLogging();
    context_ = std::make_unique<UsbContext>();
    return makeSdkDevices(*context_);
}

void DeviceWorker::initialise() {
    DeviceSet devices = createDevices();
    engine_ = std::make_unique<LightingEngine>(std::move(devices.keyboard),
                                               std::move(devices.external));

    keyboardIdleTimer_ = new QTimer(this);
    keyboardIdleTimer_->setSingleShot(true);
    keyboardIdleTimer_->setInterval(idleReleaseMs(Domain::Keyboard));
    connect(keyboardIdleTimer_, &QTimer::timeout, this, &DeviceWorker::releaseIdleKeyboard);

    externalIdleTimer_ = new QTimer(this);
    externalIdleTimer_->setSingleShot(true);
    externalIdleTimer_->setInterval(idleReleaseMs(Domain::External));
    connect(externalIdleTimer_, &QTimer::timeout, this, &DeviceWorker::releaseIdleExternal);

    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &DeviceWorker::processPending);

    // Something may already have been submitted while the thread was starting.
    QMetaObject::invokeMethod(this, &DeviceWorker::processPending, Qt::QueuedConnection);
}

int DeviceWorker::idleReleaseMs(Domain domain) const {
    return domain == Domain::Keyboard ? kKeyboardIdleReleaseMs : kExternalIdleReleaseMs;
}

void DeviceWorker::setExternalStateSurvivesIdleRelease(bool survives) {
    externalStateSurvivesIdleRelease_.store(survives);
}

void DeviceWorker::markResumed() { resumed_.store(true); }

quint64 DeviceWorker::submit(const ResolvedScheme& scheme, ApplyDepth depth) {
    quint64 generation = 0;
    {
        QMutexLocker locker(&mutex_);
        if (shuttingDown_) return generation_;
        pending_ = scheme;
        pendingFull_ = pendingFull_ || depth == ApplyDepth::Full;
        generation = ++generation_;
    }
    // One queued wake-up per burst. Everything that arrives before it runs is collapsed into
    // the single pending scheme above.
    if (!scheduled_.exchange(true))
        QMetaObject::invokeMethod(this, &DeviceWorker::processPending, Qt::QueuedConnection);
    return generation;
}

void DeviceWorker::processPending() {
    scheduled_.store(false);

    std::optional<ResolvedScheme> scheme;
    quint64 generation = 0;
    bool fullRepaint = false;
    {
        QMutexLocker locker(&mutex_);
        if (shuttingDown_ || !pending_) return;
        scheme = std::move(pending_);
        pending_.reset();
        fullRepaint = pendingFull_;
        pendingFull_ = false;
        generation = generation_;
    }

    if (!engine_) return;

    // Serialise against the `--apply` the resume hook and the login unit both run.
    if (!deviceLock_.held()) {
        if (!deviceLock_.acquire(std::chrono::milliseconds(kLockTimeoutMs))) {
            deferUntilLockFrees(std::move(*scheme), generation, fullRepaint);
            return;
        }
        // Whoever held it in between may have painted something else - on resume that is
        // the hook repainting the saved scheme - so nothing the engine remembers from before
        // can be trusted.
        if (deviceLock_.othersHeldSinceLastRelease()) engine_->forgetAppliedState();
    }
    retryDelayMs_ = kInitialRetryDelayMs;

    // A deliberate selection owes the user the whole scheme on the lights, not the delta
    // against a tracker that may have drifted from what the lights actually show.
    if (fullRepaint) engine_->forgetAppliedState();

    const ApplyResult result =
        resumed_.exchange(false) ? repaintAfterResume(*scheme) : engine_->apply(*scheme);

    publishStatus(Domain::Keyboard, result.keyboard);
    publishStatus(Domain::External, result.external);

    if (keyboardIdleTimer_) keyboardIdleTimer_->start();
    if (externalIdleTimer_) externalIdleTimer_->start();

    emit applyFinished(generation, result.allOk());
}

ApplyResult DeviceWorker::repaintAfterResume(const ResolvedScheme& scheme) {
    engine_->releaseDevices();
    engine_->forgetAppliedState();

    const ApplyResult result = applyDomainsWithRetry(*engine_, scheme, kDeviceWaitTimeout,
                                                     kDeviceRetryInterval, std::cerr);
    for (Domain domain : kAllDomains) {
        const DomainStatus& status = result.forDomain(domain);
        if (!status.ok())
            qWarning("alienfx-tray: not repainted after resume: %s",
                     describe(domain, status).c_str());
    }
    return result;
}

void DeviceWorker::deferUntilLockFrees(ResolvedScheme scheme, quint64 generation,
                                       bool fullRepaint) {
    const DomainStatus waiting{DeviceState::Waiting,
                               "another alienfx-tray process is applying a scheme"};
    publishStatus(Domain::Keyboard, waiting);
    publishStatus(Domain::External, waiting);

    {
        QMutexLocker locker(&mutex_);
        if (shuttingDown_) return;
        // A frame submitted while we were waiting on the lock is newer than this one, and a
        // live preview only ever owes the newest frame. Putting this one back over it would
        // paint a colour the user has already dragged past. The Full flag is not part of
        // that trade: it survives into whatever frame does get painted.
        if (!pending_) pending_ = std::move(scheme);
        pendingFull_ = pendingFull_ || fullRepaint;
    }

    if (retryTimer_) retryTimer_->start(retryDelayMs_);
    retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);

    emit applyDeferred(generation);
}

void DeviceWorker::publishStatus(Domain domain, const DomainStatus& status) {
    DomainStatus& last =
        domain == Domain::Keyboard ? lastKeyboardStatus_ : lastExternalStatus_;
    if (last == status) return;
    last = status;
    emit statusChanged(domain, status);
}

void DeviceWorker::releaseIdleKeyboard() { releaseIdle(Domain::Keyboard); }

void DeviceWorker::releaseIdleExternal() { releaseIdle(Domain::External); }

void DeviceWorker::releaseIdle(Domain domain) {
    if (engine_) {
        engine_->releaseDevice(domain);
        if (domain == Domain::External && !externalStateSurvivesIdleRelease_.load())
            engine_->forgetAppliedState(Domain::External);

        // Closing is where the kernel driver goes back, and that can fail on its own. No
        // lighting command ran, so the last published state stands and only the warning
        // crosses over. publishStatus emits only if that actually changed anything.
        DomainStatus status =
            domain == Domain::Keyboard ? lastKeyboardStatus_ : lastExternalStatus_;
        status.interfaceLeftDetached = engine_->interfaceLeftDetached(domain);
        publishStatus(domain, status);
    }
    releaseLockIfIdle();
}

void DeviceWorker::releaseLockIfIdle() {
    if (engine_ && engine_->anyDeviceOpen()) return;
    deviceLock_.release();
}

void DeviceWorker::shutdown() {
    {
        QMutexLocker locker(&mutex_);
        shuttingDown_ = true;
        pending_.reset();
        pendingFull_ = false;
    }
    if (keyboardIdleTimer_) keyboardIdleTimer_->stop();
    if (externalIdleTimer_) externalIdleTimer_->stop();
    if (retryTimer_) retryTimer_->stop();
    if (engine_) engine_->releaseDevices();
    deviceLock_.release();
}

}  // namespace alienfx
