#include "app/SingleInstance.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QJsonArray>
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
          QStringList paths;
          const QJsonObject object = message.object();
          if (object.value(QStringLiteral("paths")).isArray()) {
            for (const QJsonValue& value : object.value(QStringLiteral("paths")).toArray()) {
              if (value.isString()) {
                paths.append(value.toString());
              }
            }
          } else {
            const QString path = object.value(QStringLiteral("path")).toString();
            if (!path.isEmpty()) {
              paths.append(path);
            }
          }
          emit activationRequested(paths);
        }
        socket->disconnectFromServer();
      });
      connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
  });
}

bool SingleInstance::claimOrNotify(const QStringList& paths) {
  if (m_server.listen(m_serverName)) {
    return true;
  }

  QLocalSocket socket;
  socket.connectToServer(m_serverName, QIODevice::WriteOnly);
  if (socket.waitForConnected(120)) {
    QJsonObject message;
    message.insert(QStringLiteral("paths"), QJsonArray::fromStringList(paths));
    // An older running version can still open the first file after an upgrade.
    message.insert(QStringLiteral("path"), paths.value(0));
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    QDeadlineTimer deadline(5000);
    while (socket.bytesToWrite() > 0) {
      if (deadline.hasExpired() || !socket.waitForBytesWritten(deadline.remainingTime())) {
        qWarning() << "omaroll: could not finish forwarding the selection:" << socket.errorString();
        break;
      }
    }
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
