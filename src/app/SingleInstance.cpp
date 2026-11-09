#include "app/SingleInstance.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>

namespace {
constexpr qint64 maxFrameBytes = 1024 * 1024;
constexpr int maxPaths = 4096;
constexpr int maxConnections = 16;
constexpr int requestTimeout = 5000;
const QByteArray acknowledgement = "accepted\n";

QString digest(const QString& value) {
  return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}

// XDG activation tokens are short printable ASCII. Bounded so a stray value
// can't reach the environment.
bool validToken(const QString& token) {
  if (token.isEmpty() || token.size() > 512) return false;
  for (const QChar c : token) {
    if (c.unicode() <= 0x20 || c.unicode() >= 0x7f) return false;
  }
  return true;
}

bool missingEndpoint(QLocalSocket::LocalSocketError error) {
  return error == QLocalSocket::ServerNotFoundError || error == QLocalSocket::ConnectionRefusedError;
}

SingleInstance::Result forward(QLocalSocket& socket, const QByteArray& frame, bool legacy = false) {
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
      // Versions before the acknowledgement read one line and hang up, so a
      // clean close from the old endpoint is how they accept a request.
      if (legacy && !deadline.hasExpired() && socket.error() == QLocalSocket::PeerClosedError) {
        return SingleInstance::Result::Forwarded;
      }
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

bool SingleInstance::parseFrame(const QByteArray& frame, QStringList& paths, bool& library,
                                qint64& startMs, QStringList& skipped) {
  QString token;
  return parseFrame(frame, paths, library, startMs, skipped, token);
}

bool SingleInstance::parseFrame(const QByteArray& frame, QStringList& paths, bool& library,
                                qint64& startMs, QStringList& skipped, QString& activationToken) {
  if (frame.size() > maxFrameBytes) return false;
  const QJsonDocument message = QJsonDocument::fromJson(frame);
  if (!message.isObject()) {
    return false;
  }
  const QJsonObject object = message.object();
  paths.clear();
  if (object.contains(QStringLiteral("paths"))) {
    const QJsonValue value = object.value(QStringLiteral("paths"));
    if (!value.isArray() || value.toArray().size() > maxPaths) {
      return false;
    }
    for (const QJsonValue& path : value.toArray()) {
      if (!path.isString()) {
        return false;
      }
      paths.append(path.toString());
    }
  } else if (object.value(QStringLiteral("path")).isString()) {
    const QString path = object.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) paths.append(path);
  } else {
    return false;
  }
  if (object.contains(QStringLiteral("library")) && !object.value(QStringLiteral("library")).isBool()) {
    return false;
  }
  const auto start = object.value(QStringLiteral("startMs"));
  if (!start.isUndefined() && (!start.isDouble() || start.toDouble() < -1 ||
      start.toDouble() > 9007199254740991.0 || std::floor(start.toDouble()) != start.toDouble())) { return false; }
  skipped.clear();
  const auto skippedValue = object.value(QStringLiteral("skipped"));
  if (!skippedValue.isUndefined()) {
    if (!skippedValue.isArray() || skippedValue.toArray().size() > maxPaths) { return false; }
    for (const auto& name : skippedValue.toArray()) {
      if (!name.isString()) { return false; }
      skipped.append(name.toString());
    }
  }
  // An opaque compositor string; anything else is dropped, not refused, so a
  // launch still opens its files without the focus hand-over.
  const QJsonValue token = object.value(QStringLiteral("activationToken"));
  if (!token.isUndefined() && !token.isString()) return false;
  activationToken = validToken(token.toString()) ? token.toString() : QString();
  library = object.value(QStringLiteral("library")).toBool();
  startMs = start.isUndefined() ? -1 : qint64(start.toDouble());
  return true;
}

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
        QStringList paths, skipped;
        bool library = false;
        qint64 startMs = -1;
        QString token;
        if (socket->bytesAvailable() != 0 ||
            !parseFrame(frame, paths, library, startMs, skipped, token)) {
          socket->abort();
          return;
        }
        accepted = true;
        // The launcher's token lets the window that takes the request come
        // to the front: Qt's Wayland plugin reads it in requestActivate(),
        // which the handlers call before they return. It is there only for
        // that call, so a request that opened nothing, or a session without
        // Wayland, can't hand it to a later child process. The environment is
        // touched only from the GUI thread, where the windows are.
        const bool guiThread = QCoreApplication::instance() &&
                               QThread::currentThread() == QCoreApplication::instance()->thread();
        if (guiThread && !token.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", token.toLatin1());
        else if (guiThread) qunsetenv("XDG_ACTIVATION_TOKEN");
        emit activationRequested(paths, library, startMs, skipped);
        if (guiThread) qunsetenv("XDG_ACTIVATION_TOKEN");
        socket->write(acknowledgement);
        socket->disconnectFromServer();
      };
      connect(socket, &QLocalSocket::readyRead, this, receive);
      if (socket->bytesAvailable() > 0) emit socket->readyRead();
    }
  });
}

SingleInstance::Result SingleInstance::claimOrNotify(const QStringList& paths, bool library, qint64 startMs, const QStringList& skipped) {
  if (paths.size() > maxPaths || skipped.size() > maxPaths || startMs < -1 || startMs > 9007199254740991LL) {
    qWarning() << "omaroll: the open request exceeds the single-instance limit";
    return Result::Error;
  }
  QJsonObject message{{QStringLiteral("paths"), QJsonArray::fromStringList(paths)},
                      {QStringLiteral("path"), paths.value(0)},
                      {QStringLiteral("library"), library},
                      {QStringLiteral("startMs"), double(startMs)},
                      {QStringLiteral("skipped"), QJsonArray::fromStringList(skipped)}};
  // Focus belongs to whichever window opens the request, which is in the
  // running instance; the launcher's token has to go there with it.
  if (const QString token = qEnvironmentVariable("XDG_ACTIVATION_TOKEN"); validToken(token)) {
    message.insert(QStringLiteral("activationToken"), token);
  }
  const QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  if (frame.size() > maxFrameBytes) {
    qWarning() << "omaroll: the open request exceeds the single-instance limit";
    return Result::Error;
  }
  const bool ownsLock = m_lock->tryLock();
  // No lock file at all (an unwritable TMPDIR, say) means the endpoint cannot
  // be made either. Run without single-instance rather than exit with
  // nothing on screen, unless an owner can still be reached.
  const bool lockUnavailable = !ownsLock && m_lock->error() != QLockFile::LockFailedError;
  if (ownsLock) {
    // An app running before an upgrade has no ownership lock or ACK protocol.
    // Never remove its endpoint or start a second settings-writing instance.
    if (!m_legacyServerName.isEmpty()) {
      QLocalSocket legacy;
      legacy.connectToServer(m_legacyServerName);
      if (legacy.waitForConnected(1000)) {
        const Result result = forward(legacy, frame, true);
        m_lock->unlock();
        return result;
      }
      if (!missingEndpoint(legacy.error())) {
        qWarning() << "omaroll: could not contact the older running instance:" << legacy.errorString();
        m_lock->unlock();
        return Result::Error;
      }
    }
  }

  QLocalSocket socket;
  QDeadlineTimer connectDeadline(1000);
  bool connected = false;
  do {
    socket.connectToServer(m_serverName);
    connected = socket.waitForConnected(connectDeadline.remainingTime());
    if (connected || ownsLock || lockUnavailable || !missingEndpoint(socket.error()) ||
        connectDeadline.hasExpired()) {
      break;
    }
    // The owner may hold the lock just before publishing its endpoint.
    socket.abort();
    QThread::msleep(10);
  } while (!connectDeadline.hasExpired());
  if (connected) {
    const Result result = forward(socket, frame);
    if (ownsLock) m_lock->unlock();
    return result;
  }
  // Probe before listen: Qt's UserAccessOption publishes with rename(),
  // which can replace an existing endpoint instead of reporting AddressInUse.
  // A timeout or a live owner's lock never permits removal or a second writer.
  if (ownsLock && missingEndpoint(socket.error())) {
    const QByteArray endpoint = QFile::encodeName(QDir::temp().filePath(m_serverName));
    struct stat info{};
    const int found = ::lstat(endpoint.constData(), &info);
    const bool absent = found != 0 && errno == ENOENT;
    const bool stale = found == 0 && S_ISSOCK(info.st_mode) && info.st_uid == getuid() &&
                       socket.error() == QLocalSocket::ConnectionRefusedError;
    if (absent || (stale && QLocalServer::removeServer(m_serverName))) {
      if (!m_server.listen(m_serverName)) {
        qWarning() << "omaroll: running without single-instance:" << m_server.errorString();
        // Without an endpoint the lock would only make later launches fail.
        m_lock->unlock();
      }
      return Result::Primary;
    }
  }
  if (lockUnavailable && missingEndpoint(socket.error())) {
    qWarning() << "omaroll: running without single-instance:" << m_lock->error();
    return Result::Primary;
  }
  qWarning() << "omaroll: could not claim or contact the running instance:" << socket.errorString();
  if (ownsLock) m_lock->unlock();
  return Result::Error;
}
