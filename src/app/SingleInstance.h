#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>

class SingleInstance final : public QObject {
  Q_OBJECT

public:
  explicit SingleInstance(const QString& serverName = {}, QObject* parent = nullptr);
  [[nodiscard]] bool claimOrNotify(const QStringList& paths = {});

signals:
  void activationRequested(const QStringList& paths);

private:
  QString m_serverName;
  QLocalServer m_server;
};
