#include "app/signal_handler.h"

#include <QSocketNotifier>

#include <csignal>
#include <cstring>
#include <unistd.h>

namespace alienfx {
namespace {

int g_signalWriteFd = -1;

extern "C" void forwardSignal(int signalNumber) {
    if (g_signalWriteFd < 0) return;
    const char byte = static_cast<char>(signalNumber);
    // Only async-signal-safe calls in here. The real work happens in the event loop.
    const ssize_t written = ::write(g_signalWriteFd, &byte, 1);
    static_cast<void>(written);
}

}  // namespace

UnixSignalHandler::UnixSignalHandler(QObject* parent) : QObject(parent) {}

UnixSignalHandler::~UnixSignalHandler() {
    g_signalWriteFd = -1;
    if (readFd_ >= 0) ::close(readFd_);
    if (writeFd_ >= 0) ::close(writeFd_);
}

bool UnixSignalHandler::install() {
    int fds[2]{-1, -1};
    if (::pipe(fds) != 0) return false;

    readFd_ = fds[0];
    writeFd_ = fds[1];
    g_signalWriteFd = writeFd_;

    notifier_ = new QSocketNotifier(readFd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &UnixSignalHandler::onActivated);

    struct sigaction action{};
    action.sa_handler = forwardSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    for (int signalNumber : {SIGINT, SIGTERM, SIGHUP}) {
        if (::sigaction(signalNumber, &action, nullptr) != 0) return false;
    }
    return true;
}

void UnixSignalHandler::onActivated() {
    char byte = 0;
    const ssize_t read = ::read(readFd_, &byte, 1);
    if (read <= 0) return;
    emit terminationRequested();
}

}  // namespace alienfx
