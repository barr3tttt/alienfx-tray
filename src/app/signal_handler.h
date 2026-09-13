#pragma once

#include <QObject>

class QSocketNotifier;

namespace alienfx {

// SIGTERM and SIGINT have to reach the Qt event loop, because that is where the device
// worker's clean shutdown lives - and the clean shutdown is what releases the claim on the
// interface the user types on. A handler that only sets a flag is not enough; this is the
// standard self-pipe so the signal becomes an ordinary queued Qt signal.
class UnixSignalHandler : public QObject {
    Q_OBJECT

   public:
    explicit UnixSignalHandler(QObject* parent = nullptr);
    ~UnixSignalHandler() override;

    // Returns false if the pipe or the handlers could not be installed, in which case the
    // application simply runs without graceful signal handling.
    bool install();

   signals:
    void terminationRequested();

   private:
    void onActivated();

    QSocketNotifier* notifier_ = nullptr;
    int readFd_ = -1;
    int writeFd_ = -1;
};

}  // namespace alienfx
