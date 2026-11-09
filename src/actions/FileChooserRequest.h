#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

// The desktop's file chooser, through xdg-desktop-portal, for opening
// pictures, videos and PDFs that are not in the library. One request per
// chooser; it deletes nothing and reads nothing itself.
class FileChooserRequest final : public QObject {
  Q_OBJECT
public:
  explicit FileChooserRequest(QDBusConnection bus, QObject* parent = nullptr);
  ~FileChooserRequest() override;
  // |mimeTypes| become the chooser's filter. False, with failed() emitted,
  // when there is no portal to ask.
  bool start(const QString& title, const QStringList& mimeTypes);

signals:
  // Local paths, empty when the chooser was cancelled.
  void chosen(const QStringList& paths);
  void failed(const QString& message);
  void finished();

private slots:
  void response(uint result, const QVariantMap& results);

private:
  bool subscribe(const QString& handle);
  void unsubscribe();
  void finish(const QString& error = {});
  QDBusConnection m_bus;
  QString m_handle;
  bool m_finished = false;
};
