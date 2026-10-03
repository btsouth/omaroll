#pragma once

#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QVariantMap>

// One portal request per captured file. The desktop discovers compatible apps
// and handles their Desktop Entry launch rules, including Flatpak applications.
class OpenWithRequest final : public QObject {
  Q_OBJECT
public:
  explicit OpenWithRequest(QDBusConnection bus, QObject* parent = nullptr);
  ~OpenWithRequest() override;
  bool start(const QString& path);

signals:
  void failed(const QString& message);
  void finished();

private slots:
  void response(uint result, const QVariantMap& results);

private:
  void requestFile(const QDBusUnixFileDescriptor& descriptor, const QString& token);
  bool subscribe(const QString& handle);
  void unsubscribe();
  void finish(const QString& error = {});
  QDBusConnection m_bus;
  QString m_handle;
  bool m_finished = false;
};
