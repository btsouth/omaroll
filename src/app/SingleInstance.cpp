#include "app/SingleInstance.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>

#include <unistd.h>

namespace {
constexpr qint64 maxFrameBytes = 1024 * 1024;
constexpr int maxPaths = 4096;
constexpr int maxConnections = 16;
constexpr int requestTimeout = 5000;
const QByteArray acknowledgement = "accepted\n";

QString digest(const QString& value) {
  return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool missingEndpoint(QLocalSocket::LocalSocketError error) {
  return error == QLocalSocket::ServerNotFoundError || error == QLocalSocket::ConnectionRefusedError;
}

SingleInstance::Result forward(QLocalSocket& socket, const QByteArray& frame) {
  QDeadlineTimer deadline(requestTimeout);
  if (socket.write(frame) != frame.size()) {
    qWarning() << "omaroll: could not write the open request:" << socket.errorString();
    return SingleInstance::Result::Error;
  }
  while (socket.bytesToWrite() > 0) {
    if (deadline.hasExpired() || !socket.waitForBytesWritten(deadline.remainingTime())) {
      qWarning() << "omaroll: could not finish forwarding the open request:" << socket.errorString();
      return SingleInstance::Result::Error;
    }
  }
  socket.setReadBufferSize(acknowledgement.size() + 1);
  while (!socket.canReadLine()) {
    if (deadline.hasExpired() || !socket.waitForReadyRead(deadline.remainingTime())) {
      // Older versions accept the legacy path field but do not acknowledge it.
      // Delivery is uncertain, so report failure instead of starting another writer.
      qWarning() << "omaroll: the running instance did not acknowledge the open request;"
                    " close it and retry if the file did not open";
      return SingleInstance::Result::Error;
    }
  }
  if (socket.readAll() != acknowledgement) {
    qWarning() << "omaroll: the running instance rejected the open request";
    return SingleInstance::Result::Error;
  }
  return SingleInstance::Result::Forwarded;
}
}  // namespace

SingleInstance::SingleInstance(const QString& serverName, QObject* parent)
    : QObject(parent) {
  if (serverName.isEmpty()) {
    const QString wayland = qEnvironmentVariable("WAYLAND_DISPLAY");
    const QString display = wayland.isEmpty() ? qEnvironmentVariable("DISPLAY") : wayland;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    // Hash the full session identity: Wayland display names may be absolute,
    // and different runtime directories can each contain a wayland-0 socket.
    m_serverName = QStringLiteral("omaroll-%1-%2").arg(getuid()).arg(
        digest(runtime + QLatin1Char('\n') + (wayland.isEmpty() ? "x11:" : "wayland:") + display));
    m_legacyServerName = QStringLiteral("omaroll-%1-%2").arg(getuid()).arg(
        qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("x11")));
  } else {
    m_serverName = serverName;
  }
  m_lock = std::make_unique<QLockFile>(QDir::temp().filePath(
      QStringLiteral("omaroll-%1-%2.lock").arg(getuid()).arg(digest(m_serverName))));
  m_lock->setStaleLockTime(0);
  m_server.setSocketOptions(QLocalServer::UserAccessOption);
  m_server.setMaxPendingConnections(maxConnections);
  connect(&m_server, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket* socket = m_server.nextPendingConnection()) {
      if (m_activeConnections >= maxConnections) {
        socket->abort();
        socket->deleteLater();
        continue;
      }
      ++m_activeConnections;
      socket->setReadBufferSize(maxFrameBytes + 1);
      auto* timer = new QTimer(socket);
      timer->setSingleShot(true);
      connect(timer, &QTimer::timeout, socket, &QLocalSocket::abort);
      timer->start(requestTimeout);
      connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
        --m_activeConnections;
        socket->deleteLater();
      });
      auto receive = [this, socket, accepted = false]() mutable {
        if (accepted) return;
        if (socket->bytesAvailable() > maxFrameBytes) {
          socket->abort();
          return;
        }
        if (!socket->canReadLine()) return;
        const QByteArray frame = socket->readLine(maxFrameBytes + 1);
        const QJsonDocument message = QJsonDocument::fromJson(frame);
        if (!message.isObject() || socket->bytesAvailable() != 0) {
          socket->abort();
          return;
        }
        const QJsonObject object = message.object();
        QStringList paths;
        if (object.contains(QStringLiteral("paths"))) {
          const QJsonValue value = object.value(QStringLiteral("paths"));
          if (!value.isArray() || value.toArray().size() > maxPaths) {
            socket->abort();
            return;
          }
          for (const QJsonValue& path : value.toArray()) {
            if (!path.isString()) {
              socket->abort();
              return;
            }
            paths.append(path.toString());
          }
        } else if (object.value(QStringLiteral("path")).isString()) {
          const QString path = object.value(QStringLiteral("path")).toString();
          if (!path.isEmpty()) paths.append(path);
        } else {
          socket->abort();
          return;
        }
        if (object.contains(QStringLiteral("library")) && !object.value(QStringLiteral("library")).isBool()) {
          socket->abort();
          return;
        }
        accepted = true;
        emit activationRequested(paths, object.value(QStringLiteral("library")).toBool());
        socket->write(acknowledgement);
        socket->disconnectFromServer();
      };
      connect(socket, &QLocalSocket::readyRead, this, receive);
      if (socket->bytesAvailable() > 0) emit socket->readyRead();
    }
  });
}

SingleInstance::Result SingleInstance::claimOrNotify(const QStringList& paths, bool library) {
  if (paths.size() > maxPaths) {
    qWarning() << "omaroll: the open request exceeds the single-instance limit";
    return Result::Error;
  }
  QJsonObject message{{QStringLiteral("paths"), QJsonArray::fromStringList(paths)},
                      {QStringLiteral("path"), paths.value(0)},
                      {QStringLiteral("library"), library}};
  const QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  if (frame.size() > maxFrameBytes) {
    qWarning() << "omaroll: the open request exceeds the single-instance limit";
    return Result::Error;
  }
  const bool ownsLock = m_lock->tryLock();
  if (ownsLock) {
    // An app running before an upgrade has no ownership lock or ACK protocol.
    // Never remove its endpoint or start a second settings-writing instance.
    if (!m_legacyServerName.isEmpty()) {
      QLocalSocket legacy;
      legacy.connectToServer(m_legacyServerName);
      if (legacy.waitForConnected(1000)) {
        const Result result = forward(legacy, frame);
        m_lock->unlock();
        return result;
      }
      if (!missingEndpoint(legacy.error())) {
        qWarning() << "omaroll: could not contact the older running instance:" << legacy.errorString();
        m_lock->unlock();
        return Result::Error;
      }
    }
    if (m_server.listen(m_serverName)) return Result::Primary;
  }

  QLocalSocket socket;
  QDeadlineTimer connectDeadline(1000);
  bool connected = false;
  do {
    socket.connectToServer(m_serverName);
    connected = socket.waitForConnected(connectDeadline.remainingTime());
    if (connected || ownsLock || !missingEndpoint(socket.error()) || connectDeadline.hasExpired()) break;
    // The owner may hold the lock just before publishing its endpoint.
    socket.abort();
    QThread::msleep(10);
  } while (!connectDeadline.hasExpired());
  if (connected) {
    const Result result = forward(socket, frame);
    if (ownsLock) m_lock->unlock();
    return result;
  }
  // A timeout, permissions failure or lock held by a starting/live owner is
  // not evidence of a stale socket. Only the lock holder can recover a refusal.
  if (ownsLock && m_server.serverError() == QAbstractSocket::AddressInUseError &&
      missingEndpoint(socket.error()) && QLocalServer::removeServer(m_serverName) &&
      m_server.listen(m_serverName)) {
    return Result::Primary;
  }
  qWarning() << "omaroll: could not claim or contact the running instance:" << socket.errorString();
  if (ownsLock) m_lock->unlock();
  return Result::Error;
}
