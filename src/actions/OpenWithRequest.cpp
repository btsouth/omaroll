#include "actions/OpenWithRequest.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace {
const QString service = u"org.freedesktop.portal.Desktop"_s;
const QString requestInterface = u"org.freedesktop.portal.Request"_s;
}

OpenWithRequest::OpenWithRequest(QDBusConnection bus, QObject* parent)
    : QObject(parent), m_bus(std::move(bus)) {
  auto* owner = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(owner, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString&, const QString& before, const QString& after) {
    if (!before.isEmpty() && before != after) {
      finish(u"The desktop application chooser stopped. Please try again"_s);
    }
  });
}

OpenWithRequest::~OpenWithRequest() {
  unsubscribe();
  if (!m_finished && !m_handle.isEmpty()) {
    m_bus.asyncCall(QDBusMessage::createMethodCall(service, m_handle, requestInterface, u"Close"_s));
  }
}

bool OpenWithRequest::subscribe(const QString& handle) {
  unsubscribe();
  m_handle = handle;
  return m_bus.connect(service, handle, requestInterface, u"Response"_s,
                       this, SLOT(response(uint,QVariantMap)));
}

void OpenWithRequest::unsubscribe() {
  if (!m_handle.isEmpty()) {
    m_bus.disconnect(service, m_handle, requestInterface, u"Response"_s,
                     this, SLOT(response(uint,QVariantMap)));
  }
}

bool OpenWithRequest::start(const QString& path) {
  QFile file(path);
  if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
    finish(u"That file is unavailable or cannot be read"_s);
    return false;
  }
  if (!m_bus.isConnected()) {
    finish(u"Open with needs the desktop application chooser (xdg-desktop-portal)"_s);
    return false;
  }
  const QString token = u"omaroll_"_s + QUuid::createUuid().toString(QUuid::Id128);
  QString sender = m_bus.baseService().mid(1);
  sender.replace(QLatin1Char('.'), QLatin1Char('_'));
  // Subscribe before the method call: a fast chooser can respond before its
  // method reply reaches us. Modern portals derive the handle from this token.
  if (!subscribe(u"/org/freedesktop/portal/desktop/request/"_s + sender + QLatin1Char('/') + token)) {
    finish(u"Could not listen to the desktop application chooser"_s);
    return false;
  }
  const QDBusUnixFileDescriptor descriptor(file.handle());
  QDBusMessage version = QDBusMessage::createMethodCall(
      service, u"/org/freedesktop/portal/desktop"_s, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
  version.setArguments({u"org.freedesktop.portal.OpenURI"_s, u"version"_s});
  auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(version, 2000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, descriptor, token](QDBusPendingCallWatcher* completed) {
    const QDBusPendingReply<QDBusVariant> reply = *completed;
    completed->deleteLater();
    if (m_finished) return;
    if (reply.isError() || reply.value().variant().toUInt() < 3) {
      // Older portals ignore ask=true and may launch the default app instead.
      finish(u"Open with needs a desktop application chooser with support for choosing an app"_s);
      return;
    }
    requestFile(descriptor, token);
  });
  return true;
}

void OpenWithRequest::requestFile(const QDBusUnixFileDescriptor& descriptor, const QString& token) {
  QDBusMessage message = QDBusMessage::createMethodCall(
      service, u"/org/freedesktop/portal/desktop"_s, u"org.freedesktop.portal.OpenURI"_s, u"OpenFile"_s);
  message.setArguments({QString(), QVariant::fromValue(descriptor),
                        QVariantMap{{u"handle_token"_s, token}, {u"ask"_s, true}, {u"writable"_s, true}}});
  auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 5000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this](QDBusPendingCallWatcher* completed) {
    const QDBusPendingReply<QDBusObjectPath> reply = *completed;
    completed->deleteLater();
    if (m_finished) return;
    if (reply.isError()) {
      finish(u"Could not open the desktop application chooser: %1"_s.arg(reply.error().message()));
    } else if (reply.value().path().isEmpty()) {
      finish(u"The desktop application chooser returned no request"_s);
    } else if (reply.value().path() != m_handle && !subscribe(reply.value().path())) {
      finish(u"Could not listen to the desktop application chooser"_s);
    }
  });
}

void OpenWithRequest::response(uint result, const QVariantMap&) {
  // 1 is user cancellation. It must not silently open a default application.
  finish(result > 1 ? u"The desktop could not open that file in another application"_s : QString());
}

void OpenWithRequest::finish(const QString& error) {
  if (m_finished) return;
  m_finished = true;
  unsubscribe();
  if (!error.isEmpty()) emit failed(error);
  emit finished();
}
