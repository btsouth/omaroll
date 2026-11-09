#include "actions/FileChooserRequest.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QUrl>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace {
const QString service = u"org.freedesktop.portal.Desktop"_s;
const QString requestInterface = u"org.freedesktop.portal.Request"_s;

// The portal's filter type: a(sa(us)), a name and its (kind, pattern)
// pairs, where kind 1 is a MIME type.
struct FilterRule {
  uint kind = 0;
  QString pattern;
};
struct Filter {
  QString name;
  QList<FilterRule> rules;
};
QDBusArgument& operator<<(QDBusArgument& argument, const FilterRule& rule) {
  argument.beginStructure();
  argument << rule.kind << rule.pattern;
  argument.endStructure();
  return argument;
}
const QDBusArgument& operator>>(const QDBusArgument& argument, FilterRule& rule) {
  argument.beginStructure();
  argument >> rule.kind >> rule.pattern;
  argument.endStructure();
  return argument;
}
QDBusArgument& operator<<(QDBusArgument& argument, const Filter& filter) {
  argument.beginStructure();
  argument << filter.name << filter.rules;
  argument.endStructure();
  return argument;
}
const QDBusArgument& operator>>(const QDBusArgument& argument, Filter& filter) {
  argument.beginStructure();
  argument >> filter.name >> filter.rules;
  argument.endStructure();
  return argument;
}
} // namespace

Q_DECLARE_METATYPE(FilterRule)
Q_DECLARE_METATYPE(Filter)

FileChooserRequest::FileChooserRequest(QDBusConnection bus, QObject* parent)
    : QObject(parent), m_bus(std::move(bus)) {
  qDBusRegisterMetaType<FilterRule>();
  qDBusRegisterMetaType<QList<FilterRule>>();
  qDBusRegisterMetaType<Filter>();
  qDBusRegisterMetaType<QList<Filter>>();
  auto* owner = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(owner, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString&, const QString& before, const QString& after) {
    if (!before.isEmpty() && before != after) {
      finish(u"The desktop file chooser stopped. Please try again"_s);
    }
  });
}

FileChooserRequest::~FileChooserRequest() {
  unsubscribe();
  if (!m_finished && !m_handle.isEmpty()) {
    m_bus.asyncCall(QDBusMessage::createMethodCall(service, m_handle, requestInterface, u"Close"_s));
  }
}

bool FileChooserRequest::subscribe(const QString& handle) {
  unsubscribe();
  m_handle = handle;
  return m_bus.connect(service, handle, requestInterface, u"Response"_s,
                       this, SLOT(response(uint,QVariantMap)));
}

void FileChooserRequest::unsubscribe() {
  if (!m_handle.isEmpty()) {
    m_bus.disconnect(service, m_handle, requestInterface, u"Response"_s,
                     this, SLOT(response(uint,QVariantMap)));
  }
}

bool FileChooserRequest::start(const QString& title, const QStringList& mimeTypes) {
  if (!m_bus.isConnected()) {
    finish(u"Opening a file needs the desktop file chooser (xdg-desktop-portal)"_s);
    return false;
  }
  const QString token = u"omaroll_"_s + QUuid::createUuid().toString(QUuid::Id128);
  QString sender = m_bus.baseService().mid(1);
  sender.replace(QLatin1Char('.'), QLatin1Char('_'));
  // Subscribe before the call: a fast chooser can answer before its reply.
  if (!subscribe(u"/org/freedesktop/portal/desktop/request/"_s + sender + QLatin1Char('/') + token)) {
    finish(u"Could not listen to the desktop file chooser"_s);
    return false;
  }
  Filter media{u"Pictures, videos and PDFs"_s, {}};
  for (const QString& type : mimeTypes) media.rules.append({1, type});
  QVariantMap options{{u"handle_token"_s, token}, {u"multiple"_s, true}, {u"modal"_s, true}};
  if (!media.rules.isEmpty()) {
    options.insert(u"filters"_s, QVariant::fromValue(QList<Filter>{media}));
    options.insert(u"current_filter"_s, QVariant::fromValue(media));
  }
  QDBusMessage message = QDBusMessage::createMethodCall(
      service, u"/org/freedesktop/portal/desktop"_s, u"org.freedesktop.portal.FileChooser"_s,
      u"OpenFile"_s);
  message.setArguments({QString(), title, options});
  // The call returns once the chooser is up, which can take a while when
  // the portal and its backend have to start first.
  auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 30000), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this](QDBusPendingCallWatcher* completed) {
    const QDBusPendingReply<QDBusObjectPath> reply = *completed;
    completed->deleteLater();
    if (m_finished) return;
    if (reply.isError()) {
      finish(u"Could not open the desktop file chooser: %1"_s.arg(reply.error().message()));
    } else if (reply.value().path().isEmpty()) {
      finish(u"The desktop file chooser returned no request"_s);
    } else if (reply.value().path() != m_handle && !subscribe(reply.value().path())) {
      finish(u"Could not listen to the desktop file chooser"_s);
    }
  });
  return true;
}

void FileChooserRequest::response(uint result, const QVariantMap& results) {
  if (m_finished) return;
  // 1 is a cancellation, which says nothing.
  if (result > 1) {
    finish(u"The desktop file chooser could not open that"_s);
    return;
  }
  QStringList paths;
  if (result == 0) {
    const QStringList uris = results.value(u"uris"_s).toStringList();
    for (const QString& uri : uris) {
      const QUrl url(uri);
      if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    // Only files on this computer open; say so rather than doing nothing.
    if (paths.isEmpty() && !uris.isEmpty()) {
      finish(uris.size() == 1 ? u"Only files on this computer can be opened"_s
                              : u"None of those are files on this computer"_s);
      return;
    }
  }
  m_finished = true;
  unsubscribe();
  emit chosen(paths);
  emit finished();
}

void FileChooserRequest::finish(const QString& error) {
  if (m_finished) return;
  m_finished = true;
  unsubscribe();
  if (!error.isEmpty()) emit failed(error);
  emit finished();
}
