#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>

class SingleInstance final : public QObject {
  Q_OBJECT

public:
  explicit SingleInstance(const QString& serverName = {}, QObject* parent = nullptr);
  // |library| asks the running instance for the library window even when every
  // path could open in the viewer, as --library does on the command line.
  [[nodiscard]] bool claimOrNotify(const QStringList& paths = {}, bool library = false);

signals:
  void activationRequested(const QStringList& paths, bool library);

private:
  QString m_serverName;
  QLocalServer m_server;
};
