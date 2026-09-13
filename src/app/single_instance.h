#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace alienfx {

// One tray icon per session. A second launch does not start a rival process fighting over
// the same USB interfaces - it asks the running one to show its panel and exits.
class SingleInstance : public QObject {
    Q_OBJECT

   public:
    explicit SingleInstance(QObject* parent = nullptr);

    // True when this process claimed the name and should carry on running.
    [[nodiscard]] bool claim();

    // Asks an already-running instance to raise its panel. Returns false if nobody answered.
    [[nodiscard]] static bool signalExistingInstance();

    // True when a live process is listening on the name. Distinguishes a running instance
    // from a socket left behind by one that crashed.
    [[nodiscard]] static bool isAnotherInstanceListening();

   signals:
    void raiseRequested();

   private:
    [[nodiscard]] static QString serverName();

    QLocalServer* server_ = nullptr;
};

}  // namespace alienfx
