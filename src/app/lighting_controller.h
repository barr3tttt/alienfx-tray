#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "app/metatypes.h"
#include "core/config.h"
#include "core/scheme.h"

class QTimer;

namespace alienfx {

class DeviceThread;
class DeviceWorker;

// The GUI thread's single view of the world: what scheme is active, what the devices think
// of us, and the one place edits turn into device work.
//
// Nothing here blocks. Every call returns immediately after handing the worker a scheme; the
// answer arrives later as a signal. A controller that is missing, busy or wedged shows up as
// a status banner, never as a frozen window.
class LightingController : public QObject {
    Q_OBJECT

   public:
    explicit LightingController(QObject* parent = nullptr);

    // Takes ownership of `worker` and uses `applyWatchdogMs` as the apply deadline. The seam
    // exists so the wedge this class is built to survive can actually be staged: a device
    // layer whose apply never returns, without hardware and without a six-second wait.
    LightingController(DeviceWorker* worker, int applyWatchdogMs, QObject* parent = nullptr);

    ~LightingController() override;

    // Config edits are written back after this much quiet. Applies are immediate; only the
    // disk write is debounced, so dragging a wheel does not rewrite the file 200 times.
    static constexpr int kConfigSaveDebounceMs = 1000;

    // If an apply has not come back in this long the controller is assumed wedged and the
    // UI is told so. The SDK's APIv4 ready-wait has no timeout of its own.
    static constexpr int kApplyWatchdogMs = 6000;

    [[nodiscard]] const Config& config() const { return config_; }
    [[nodiscard]] const Scheme& workingScheme() const { return working_; }
    [[nodiscard]] bool lightsOn() const { return config_.lightsOn; }
    [[nodiscard]] DomainStatus status(Domain domain) const;
    [[nodiscard]] QString configError() const { return configError_; }

    // Applies immediately and schedules a debounced save. This is the live-preview path.
    void applyWorkingScheme(const Scheme& scheme);

    void activateScheme(const QString& name);
    void setLightsOn(bool on);

    // Stores the working scheme under a name and makes it active.
    void saveWorkingSchemeAs(const QString& name);
    void deleteScheme(const QString& name);

    // Re-sends the current state. Used after a device comes back.
    void reapply();

    // Whether resumes reach onSystemResumed. Only then may what the external lights were
    // sent be trusted across the worker's idle release.
    void setResumeWatched(bool watched);

    // The machine has come back from suspend: whatever the lights show now is not known to
    // be what was sent, so the current state is repainted in full.
    void onSystemResumed();

    // Writes a debounced edit out now, rather than waiting for the quiet period. The colour
    // pickers call this when the user lets go, so a drag costs one write at the end instead
    // of one per settling period.
    void flushPendingSave();

    // Whether a domain last published in this state should be escalated when an apply misses
    // the watchdog deadline. Busy and Waiting are included deliberately: both read as a
    // situation that is about to resolve, and a wedged worker never delivers that, so leaving
    // either up hides the exact hang the watchdog exists to surface. Missing and Error
    // already name a specific fault and keep it.
    [[nodiscard]] static bool escalatesOnWedge(DeviceState state);

    // The one bounded wait in the process. Quitting cannot hang on a wedged controller:
    // DeviceThread gives up and walks away rather than joining. See DeviceThread::stop.
    void shutdown();

   signals:
    // A different scheme is now active, or the active one was replaced wholesale. Views
    // reload everything when this fires.
    //
    // Deliberately NOT emitted by applyWorkingScheme: that path's only caller is the editor
    // itself, and echoing an edit straight back at its author rewrites the widget the user
    // is still typing into and rebuilds the tray menu once per colour-wheel frame.
    void schemeChanged();
    void schemeListChanged();
    void lightsOnChanged(bool on);
    void statusChanged(alienfx::Domain domain, alienfx::DomainStatus status);
    // The config on disk could not be read, or could not be written back. Separate from
    // statusChanged because it outlives any one device state and has nothing to do with the
    // hardware: the user is running on defaults and their saved schemes are not in play.
    void configErrorChanged(const QString& error);

   private slots:
    void onWorkerStatus(alienfx::Domain domain, alienfx::DomainStatus status);
    void onApplyFinished(quint64 generation, bool allOk);
    void onApplyDeferred(quint64 generation);
    void saveConfigNow();
    void onApplyWatchdog();

   private:
    void submitCurrent();
    void setConfigError(QString error);
    [[nodiscard]] static DomainStatus watchdogStamp();

    Config config_;
    Scheme working_;
    QString configError_;

    DomainStatus keyboardStatus_;
    DomainStatus externalStatus_;
    // What the worker last told us, before the watchdog overwrote it. The worker only emits
    // on change, so an apply that finishes late leaves nothing to correct the stamp with.
    DomainStatus keyboardBeforeWatchdog_;
    DomainStatus externalBeforeWatchdog_;
    bool watchdogTripped_ = false;
    // The newest scheme handed to the worker. A slow apply that has already been superseded
    // must not disarm the watchdog the apply after it is relying on.
    quint64 pendingGeneration_ = 0;

    std::unique_ptr<DeviceThread> deviceThread_;
    QTimer* saveTimer_ = nullptr;
    QTimer* watchdog_ = nullptr;
    bool shutdownDone_ = false;
};

}  // namespace alienfx
