#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QStringList>

#include <memory>

class SingleInstance final : public QObject {
  Q_OBJECT

public:
  enum class Result { Primary, Forwarded, Error };

  explicit SingleInstance(const QString& serverName = {}, QObject* parent = nullptr);
  // |library| asks the running instance for the library window even when every
  // path could open in the viewer, as --library does on the command line.
  [[nodiscard]] Result claimOrNotify(const QStringList& paths = {}, bool library = false);

signals:
  void activationRequested(const QStringList& paths, bool library);

private:
  QString m_serverName;
  QString m_legacyServerName;
  std::unique_ptr<QLockFile> m_lock;
  QLocalServer m_server;
  int m_activeConnections = 0;
};
