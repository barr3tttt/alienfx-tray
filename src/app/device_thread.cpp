#include "app/device_thread.h"

#include <QDeadlineTimer>
#include <QMetaObject>
#include <QThread>

#include "app/device_worker.h"

namespace alienfx {

DeviceThread::DeviceThread(DeviceWorker* worker, QObject* parent)
    : QObject(parent), worker_(worker) {
    // Deliberately unparented and never owned by a smart pointer: a thread we walk away from
    // is leaked on purpose, because ~QThread aborts the process if the thread is still
    // running and that is the one outcome worse than a wedged controller.
    thread_ = new QThread;
    thread_->setObjectName("alienfx-device");
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::started, worker_, &DeviceWorker::initialise);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &DeviceWorker::statusChanged, this, &DeviceThread::statusChanged);
    connect(worker_, &DeviceWorker::applyFinished, this, &DeviceThread::applyFinished);
    connect(worker_, &DeviceWorker::applyDeferred, this, &DeviceThread::applyDeferred);

    thread_->start();
}

DeviceThread::~DeviceThread() { stop(kStopTimeout); }

quint64 DeviceThread::submit(const ResolvedScheme& scheme) {
    if (!worker_) return 0;
    return worker_->submit(scheme);
}

void DeviceThread::setExternalStateSurvivesIdleRelease(bool survives) {
    if (worker_) worker_->setExternalStateSurvivesIdleRelease(survives);
}

void DeviceThread::markResumed() {
    if (worker_) worker_->markResumed();
}

bool DeviceThread::stop(std::chrono::milliseconds limit) {
    if (!thread_) return true;

    if (worker_) {
        // An abandoned worker outlives us, so its signals have to stop pointing here first.
        worker_->disconnect(this);
        // Queued, never blocking: a blocking invoke would wait on exactly the wedge this
        // type exists to survive.
        QMetaObject::invokeMethod(worker_, &DeviceWorker::shutdown, Qt::QueuedConnection);
        worker_ = nullptr;  // the thread's finished -> deleteLater owns it from here on
    }

    QThread* thread = thread_;
    thread_ = nullptr;
    thread->quit();
    if (!thread->wait(QDeadlineTimer(limit))) {
        qWarning("alienfx-tray: device worker did not stop in time; leaving it behind");
        return false;
    }
    delete thread;
    return true;
}

}  // namespace alienfx
