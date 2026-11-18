#pragma once

#include <QObject>

#include <chrono>

#include "app/metatypes.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"

class QThread;

namespace alienfx {

class DeviceWorker;

// Owns the device thread and the worker running on it, and is the only way to reach either.
//
// The ownership is the whole point of the type. The SDK's APIv4 ready-wait is an unbounded
// spin (see device/sdk_devices.h), so the device thread is a thread that may never come
// back, and anything that joins it inherits that. Nothing here hands out the QThread or the
// worker, so there is no handle for a future caller to wait on: submit() posts and returns,
// and stop() is the only wait in the process - bounded by its argument, and honest about
// walking away when the bound expires.
class DeviceThread : public QObject {
    Q_OBJECT

   public:
    // Longest the application will wait for the device thread to come back. It may not.
    static constexpr std::chrono::milliseconds kStopTimeout{3000};

    // Takes ownership of `worker` and moves it onto the thread it starts.
    explicit DeviceThread(DeviceWorker* worker, QObject* parent = nullptr);
    ~DeviceThread() override;

    DeviceThread(const DeviceThread&) = delete;
    DeviceThread& operator=(const DeviceThread&) = delete;

    // Returns the generation the caller can expect back from applyFinished. Never blocks.
    // The depth follows DeviceWorker::submit: Changed for live preview, Full for a
    // deliberate selection that must repaint every zone.
    quint64 submit(const ResolvedScheme& scheme, ApplyDepth depth = ApplyDepth::Changed);

    // See DeviceWorker. Never block.
    void setExternalStateSurvivesIdleRelease(bool survives);
    void markResumed();

    // Asks the worker to release its devices, then waits at most `limit` for the thread to
    // finish. Returns false if it was still running when the limit expired, in which case it
    // is deliberately abandoned rather than joined: the kernel reattaches usbhid when the
    // usbfs descriptor closes on process exit. Safe to call more than once.
    bool stop(std::chrono::milliseconds limit);

   signals:
    void statusChanged(alienfx::Domain domain, alienfx::DomainStatus status);
    void applyFinished(quint64 generation, bool allOk);
    void applyDeferred(quint64 generation);
    void resumeRepaintStarted();

   private:
    QThread* thread_ = nullptr;
    DeviceWorker* worker_ = nullptr;
};

}  // namespace alienfx
