#include "app/single_instance.h"

#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>

#include <unistd.h>

#include <cstdlib>

namespace alienfx {
namespace {
constexpr int kConnectTimeoutMs = 300;
constexpr const char* kRaiseMessage = "raise";
}  // namespace

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

QString SingleInstance::serverName() {
    // An explicit absolute path, deliberately, rather than a bare name.
    //
    // QLocalServer turns a bare name into a socket somewhere of its own choosing, and with
    // UserAccessOption it puts that socket in a fresh private directory and points a symlink
    // at it - replacing the symlink on every listen(). The name is then never actually
    // exclusive: a second process "succeeds" in listening and the two of them end up running
    // side by side, each claiming the same USB interfaces.
    //
    // XDG_RUNTIME_DIR is already mode 0700 and per-user, so binding a path inside it gives
    // the same access restriction with none of that behaviour, and a second bind fails the
    // way a second bind should.
    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir)
        return QString::fromLocal8Bit(runtimeDir) + QStringLiteral("/alienfx-tray.socket");

    return QDir::tempPath() +
           QStringLiteral("/alienfx-tray-%1.socket").arg(static_cast<uint>(::getuid()));
}

bool SingleInstance::claim() {
    server_ = new QLocalServer(this);

    if (!server_->listen(serverName())) {
        // The name is taken. It matters enormously which of the two reasons it is: removing
        // the socket unconditionally would evict a perfectly healthy instance and leave two
        // processes fighting over the same USB interfaces, which is exactly the state that
        // strands a keyboard claimed.
        if (isAnotherInstanceListening()) return false;

        // Nobody is answering, so this is a socket left behind by a process that died
        // without cleaning up. That one is safe to clear.
        QLocalServer::removeServer(serverName());
        if (!server_->listen(serverName())) return false;
    }

    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                socket->readAll();
                socket->disconnectFromServer();
                emit raiseRequested();
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    return true;
}

bool SingleInstance::isAnotherInstanceListening() {
    QLocalSocket socket;
    socket.connectToServer(serverName());
    const bool connected = socket.waitForConnected(kConnectTimeoutMs);
    if (connected) socket.abort();
    return connected;
}

bool SingleInstance::signalExistingInstance() {
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(kConnectTimeoutMs)) return false;

    socket.write(kRaiseMessage);
    socket.flush();
    socket.waitForBytesWritten(kConnectTimeoutMs);
    socket.disconnectFromServer();
    return true;
}

}  // namespace alienfx
