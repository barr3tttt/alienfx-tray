#pragma once

#include <QObject>
#include <QString>

#include "app/metatypes.h"
#include "core/config.h"
#include "core/scheme.h"

class QThread;
class QTimer;

namespace alienfx {

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
    ~LightingController() override;

    // Config edits are written back after this much quiet. Applies are immediate; only the
    // disk write is debounced, so dragging a wheel does not rewrite the file 200 times.
    static constexpr int kConfigSaveDebounceMs = 1000;

    // If an apply has not come back in this long the controller is assumed wedged and the
    // UI is told so. The SDK's APIv4 ready-wait has no timeout of its own.
    static constexpr int kApplyWatchdogMs = 6000;

    // Longest the application will wait for the device thread when quitting.
    static constexpr int kShutdownWaitMs = 3000;

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

    // Blocks only on the worker thread joining, with a bounded wait, so quitting cannot
    // hang on a wedged controller.
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

   private slots:
    void onWorkerStatus(alienfx::Domain domain, alienfx::DomainStatus status);
    void onApplyFinished(quint64 generation, bool allOk);
    void saveConfigNow();
    void onApplyWatchdog();

   private:
    void submitCurrent();
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

    QThread* workerThread_ = nullptr;
    DeviceWorker* worker_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QTimer* watchdog_ = nullptr;
    bool shutdownDone_ = false;
};

}  // namespace alienfx
