#include "app/SingleInstance.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <unistd.h>

SingleInstance::SingleInstance(const QString& serverName, QObject* parent)
    : QObject(parent),
      // Per user and per session: two Wayland sessions of one account must
      // not activate each other's windows across seats.
      m_serverName(serverName.isEmpty()
                       ? QStringLiteral("omaroll-%1-%2")
                             .arg(getuid())
                             .arg(qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("x11")))
                       : serverName) {
  connect(&m_server, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket* socket = m_server.nextPendingConnection()) {
      connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        if (!socket->canReadLine()) {
          return;
        }
        const QJsonDocument message = QJsonDocument::fromJson(socket->readLine());
        if (message.isObject()) {
          emit activationRequested(message.object().value(QStringLiteral("path")).toString());
        }
        socket->disconnectFromServer();
      });
      connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
  });
}

bool SingleInstance::claimOrNotify(const QString& path) {
  if (m_server.listen(m_serverName)) {
    return true;
  }

  QLocalSocket socket;
  socket.connectToServer(m_serverName, QIODevice::WriteOnly);
  if (socket.waitForConnected(120)) {
    QJsonObject message;
    message.insert(QStringLiteral("path"), path);
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact));
    socket.write("\n");
    socket.flush();
    socket.waitForBytesWritten(120);
    return false;
  }

  if (m_server.serverError() != QAbstractSocket::AddressInUseError) {
    // The socket could not be made at all (an unwritable or missing TMPDIR,
    // say). Nobody else is running; run without single-instance rather than
    // exit with nothing on screen.
    return true;
  }
  QLocalServer::removeServer(m_serverName);
  return m_server.listen(m_serverName);
}
