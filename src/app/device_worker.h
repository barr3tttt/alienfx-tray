#pragma once

#include <QMutex>
#include <QObject>

#include <atomic>
#include <memory>
#include <optional>

#include "app/metatypes.h"
#include "app/process_lock.h"
#include "core/lighting_engine.h"
#include "core/scheme.h"
#include "device/device_factory.h"

class QTimer;

namespace alienfx {

class UsbContext;

// Lives on its own thread and owns every USB handle in the process. The GUI thread never
// touches libusb, and this object never touches a widget.
//
// Two properties matter more than anything else here:
//
//  * Latest-wins coalescing. A full external apply is around 36 HID exchanges - the APIv4
//    power path walks cid 0x5b..0x60 for each light - so 50-100ms for six zones. A colour
//    wheel drag produces far more frames than that. Queuing them would leave the lights
//    seconds behind the cursor, so intermediate frames are dropped and only the newest
//    scheme is ever applied.
//
//  * Short claims. Claiming the keyboard controller detaches usbhid from the interface the
//    user types on. Handles are opened on demand and released after a short idle period, so
//    a claim never outlives an interaction.
class DeviceWorker : public QObject {
    Q_OBJECT

   public:
    explicit DeviceWorker(QObject* parent = nullptr);
    ~DeviceWorker() override;

    // The two controllers get very different idle policies, because the costs are not
    // symmetric.
    //
    // Claiming the keyboard controller detaches usbhid from the interface the user types on -
    // it is the built-in keyboard, and there is no separate LED interface - so the claim goes
    // back almost immediately. Re-opening it costs about 110-130 ms, and a colour set after
    // that is another 20 ms.
    //
    // The external controller costs about 45 ms to open and 110-140 ms per zone write. Nothing
    // else on the system wants it and no input device is affected, so releasing it eagerly
    // would buy nothing and charge the reopen for it.
    static constexpr int kKeyboardIdleReleaseMs = 1500;
    static constexpr int kExternalIdleReleaseMs = 20000;

    // How long to wait for the device lock before giving up on this attempt. Short, because
    // a frame that waits is a frame the preview is behind on - but giving up on the attempt
    // is not giving up on the frame, which is re-tried below.
    static constexpr int kLockTimeoutMs = 750;

    // A contended lock means another process is mid-apply: the resume hook holds it across
    // its whole device poll, which can be tens of seconds. A colour-wheel drag submits a
    // frame roughly every 16 ms, so retrying at that rate would be a hot loop against the
    // lock for the entire window. Back off instead, and keep only the newest frame.
    static constexpr int kInitialRetryDelayMs = 250;
    static constexpr int kMaxRetryDelayMs = 2000;

    // Callable from any thread. Replaces whatever was pending and returns the generation
    // the caller can expect back from applyFinished, so a superseded apply is recognisable.
    quint64 submit(const ResolvedScheme& scheme);

    // Callable from any thread. Whether what the external lights were last sent may be
    // trusted across the idle release. Releasing the handle changes nothing they show, but
    // a suspend can, so this is only safe while something reports resumes - otherwise every
    // idle release forgets it and the next apply repaints the lot. Off until told otherwise.
    void setExternalStateSurvivesIdleRelease(bool survives);

    // Callable from any thread. The external lights may no longer show what they were sent -
    // the machine has resumed - so the next apply repaints them in full. Takes effect before
    // whatever is applied next, including a scheme already pending.
    void markExternalStateUnknown();

   public slots:
    // Runs on the worker thread once it starts.
    void initialise();
    // Releases the devices and stops the idle timer. Safe to call twice.
    void shutdown();

   signals:
    void statusChanged(alienfx::Domain domain, alienfx::DomainStatus status);
    void applyFinished(quint64 generation, bool allOk);

    // The frame could not be painted yet because another process holds the devices, and is
    // still owed. Deliberately not applyFinished: nothing has been applied, and the apply
    // watchdog must not read a lock we are patiently waiting on as a wedged device thread.
    void applyDeferred(quint64 generation);

   protected:
    // Built on the worker thread rather than by the constructor, because opening USB handles
    // must not happen on the GUI thread. Overridden in tests to stand in for hardware.
    virtual DeviceSet createDevices();

    // The idle release delays. Overridden in tests so a release can be waited for.
    [[nodiscard]] virtual int idleReleaseMs(Domain domain) const;

   private slots:
    void processPending();
    void releaseIdleKeyboard();
    void releaseIdleExternal();

   private:
    void publishStatus(Domain domain, const DomainStatus& status);
    void deferUntilLockFrees(ResolvedScheme scheme, quint64 generation);
    void releaseIdle(Domain domain);
    // The lock is only meaningful while a handle is open, so it goes back with the last one.
    void releaseLockIfIdle();

    std::unique_ptr<UsbContext> context_;
    ProcessLock deviceLock_;
    std::unique_ptr<LightingEngine> engine_;
    QTimer* keyboardIdleTimer_ = nullptr;
    QTimer* externalIdleTimer_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    int retryDelayMs_ = kInitialRetryDelayMs;

    QMutex mutex_;
    std::optional<ResolvedScheme> pending_;
    quint64 generation_ = 0;
    std::atomic<bool> scheduled_{false};
    std::atomic<bool> externalStateSurvivesIdleRelease_{false};
    std::atomic<bool> externalStateUnknown_{false};
    bool shuttingDown_ = false;

    DomainStatus lastKeyboardStatus_;
    DomainStatus lastExternalStatus_;
};

}  // namespace alienfx
