#pragma once

#include <QObject>

class QDBusConnection;

namespace alienfx {

// Tells the tray that the machine has just come back from suspend, so it can stop trusting
// what it believes the lights are showing and repaint them.
//
// The root sleep hook repaints on resume too, but it is optional and can stand down, and the
// tray keeps what it painted across its own idle release of the external controller. Without
// a resume signal of its own the tray would go on believing a scheme the firmware has since
// replaced. logind announces every suspend and resume as PrepareForSleep on the system bus.
class SleepMonitor : public QObject {
    Q_OBJECT

   public:
    // Subscribes on the system bus.
    explicit SleepMonitor(QObject* parent = nullptr);
    // Subscribes on `bus`. A disconnected bus leaves the monitor not watching.
    explicit SleepMonitor(const QDBusConnection& bus, QObject* parent = nullptr);

    // Whether the subscription took. When it did not, resume goes unseen, and callers must
    // not keep anything that only a resume would invalidate.
    [[nodiscard]] bool watching() const { return watching_; }

   public slots:
    // logind's PrepareForSleep: true on the way down, false on the way back up.
    void handlePrepareForSleep(bool goingToSleep);

   signals:
    void resumed();

   private:
    bool watching_ = false;
};

}  // namespace alienfx
