#pragma once

#include <QDeadlineTimer>
#include <QObject>
#include <QString>

#include <memory>

#include "app/metatypes.h"
#include "core/config.h"
#include "core/lighting_engine.h"
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
//
// The editing model has exactly one source of truth: the schemes in config_. What the
// lights show is working_, a draft. previewScheme() repaints the draft and nothing else -
// it never writes a scheme back, so a colour-wheel drag cannot silently overwrite the
// selected scheme. The draft becomes permanent only through an explicit gesture:
// saveWorkingScheme() overwrites the selected scheme with it, saveWorkingSchemeAs() stores
// it under a new name. Anything that moves the selection - activateScheme(), or
// deleteScheme() on the selected scheme - or revertWorkingScheme() discards the draft and
// repaints the saved scheme in full. Deleting a scheme that is not selected touches only
// the scheme list: the draft and the lights stay exactly as they were.
class LightingController : public QObject {
    Q_OBJECT

   public:
    explicit LightingController(QObject* parent = nullptr);

    // Takes ownership of `worker` and uses `applyWatchdogMs` as the apply deadline. The seam
    // exists so the wedge this class is built to survive can actually be staged: a device
    // layer whose apply never returns, without hardware and without a six-second wait.
    LightingController(DeviceWorker* worker, int applyWatchdogMs, QObject* parent = nullptr);

    ~LightingController() override;

    // If an apply has not come back in this long the controller is assumed wedged and the
    // UI is told so. The SDK's APIv4 ready-wait has no timeout of its own.
    static constexpr int kApplyWatchdogMs = 6000;

    [[nodiscard]] const Config& config() const { return config_; }
    [[nodiscard]] const Scheme& workingScheme() const { return working_; }
    [[nodiscard]] bool lightsOn() const { return config_.lightsOn; }
    [[nodiscard]] DomainStatus status(Domain domain) const;
    [[nodiscard]] QString configError() const { return configError_; }

    // Whether the draft on the lights differs from the selected scheme as saved. While this
    // is true the UI shows the unsaved-changes indicator, and switching scheme or closing
    // the panel throws the draft away.
    [[nodiscard]] bool hasUnsavedChanges() const {
        return working_ != config_.activeScheme();
    }

    // Live preview: paints the draft and marks it unsaved. Never touches config_ - the
    // debounced write-through this used to do is what overwrote the captain's schemes.
    void previewScheme(const Scheme& scheme);

    // The explicit saves. saveWorkingScheme overwrites the selected scheme with the draft;
    // saveWorkingSchemeAs stores the draft under a new name and makes that active.
    void saveWorkingScheme();
    void saveWorkingSchemeAs(const QString& name);

    // Throws the draft away and repaints the selected scheme. Called when the panel closes
    // with unsaved changes; switching scheme discards them the same way.
    void revertWorkingScheme();

    void activateScheme(const QString& name);
    void setLightsOn(bool on);

    void deleteScheme(const QString& name);

    // Re-sends the current state. Used after a device comes back. Deliberately a Changed
    // apply rather than a Full one: the engine already forgets what it painted whenever a
    // device could have been touched elsewhere, so what is left to send here is genuinely
    // nothing in the steady state, and tests assert that.
    void reapply();

    // Whether resumes reach onSystemResumed. Only then may what the external lights were
    // sent be trusted across the worker's idle release.
    void setResumeWatched(bool watched);

    // The machine has come back from suspend: whatever the lights show now is not known to
    // be what was sent, so the current state is repainted in full.
    void onSystemResumed();

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
    // Deliberately NOT emitted by previewScheme: that path's only caller is the editor
    // itself, and echoing an edit straight back at its author rewrites the widget the user
    // is still typing into and rebuilds the tray menu once per colour-wheel frame.
    void schemeChanged();
    void schemeListChanged();
    void lightsOnChanged(bool on);
    void statusChanged(alienfx::Domain domain, alienfx::DomainStatus status);
    // The draft started or stopped differing from the selected scheme as saved.
    void unsavedChangesChanged(bool unsaved);
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
    void submitCurrent(ApplyDepth depth);
    void armWatchdog();
    void setConfigError(QString error);
    void updateUnsavedChanges();
    [[nodiscard]] static DomainStatus watchdogStamp();

    Config config_;
    Scheme working_;
    QString configError_;
    bool unsavedChanges_ = false;

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
    int applyWatchdogMs_ = kApplyWatchdogMs;
    // Until this expires the worker may be waiting for the controllers to come back from a
    // suspend, and every apply deadline is stretched by what is left of that wait.
    QDeadlineTimer resumeWait_{0};

    std::unique_ptr<DeviceThread> deviceThread_;
    QTimer* watchdog_ = nullptr;
    bool shutdownDone_ = false;
};

}  // namespace alienfx
