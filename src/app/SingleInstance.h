#pragma once

#include <QLocalServer>
#include <QObject>

class SingleInstance final : public QObject {
  Q_OBJECT

public:
  explicit SingleInstance(const QString& serverName = {}, QObject* parent = nullptr);
  [[nodiscard]] bool claimOrNotify(const QString& path = {});

signals:
  void activationRequested(const QString& path);

private:
  QString m_serverName;
  QLocalServer m_server;
};
