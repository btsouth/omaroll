#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <optional>

class AppSettings;
class QMediaPlayer;

// Publishes the video playing most recently as an MPRIS player, so media keys
// and the Omarchy shell's player controls reach it. The bus name exists only
// while a video is loaded; pictures and the library alone never show up.
class MprisService : public QObject {
  Q_OBJECT

public:
  // Without a connection, the session bus is joined when a video first plays,
  // so starting up never waits on it.
  explicit MprisService(AppSettings* settings, std::optional<QDBusConnection> bus = std::nullopt,
                        QObject* parent = nullptr);
  ~MprisService() override;

  // QML calls these with its MediaPlayer. The last one played is controlled.
  Q_INVOKABLE void track(QObject* player);
  Q_INVOKABLE void release(QObject* player);

  [[nodiscard]] QString serviceName() const { return m_serviceName; }
  [[nodiscard]] bool isPublished() const { return !m_serviceName.isEmpty(); }

  // org.mpris.MediaPlayer2
  void raise();

  // org.mpris.MediaPlayer2.Player
  [[nodiscard]] QString playbackStatus() const;
  [[nodiscard]] QVariantMap metadata() const;
  [[nodiscard]] qlonglong position() const;
  [[nodiscard]] double rate() const;
  void setRate(double rate);
  [[nodiscard]] double volume() const;
  void setVolume(double volume);
  [[nodiscard]] bool canPlay() const { return m_player != nullptr; }
  [[nodiscard]] bool canSeek() const;
  [[nodiscard]] bool canRaise() const;

  void play();
  void pause();
  void playPause();
  void stop();
  void seek(qlonglong offsetUs);
  void setPosition(const QDBusObjectPath& track, qlonglong positionUs);

signals:
  void seeked(qlonglong positionUs);

private:
  void publish();
  void unpublish();
  void attach(QMediaPlayer* player);
  void detach();
  void changed(const QString& interface, const QStringList& names);
  void positionMoved(qint64 position);
  [[nodiscard]] QDBusObjectPath trackId() const;

  AppSettings* m_settings;
  std::optional<QDBusConnection> m_bus;
  QObject* m_root = nullptr;
  QPointer<QMediaPlayer> m_player;
  QList<QMetaObject::Connection> m_connections;
  QString m_serviceName;
  quint64 m_track = 0;
  qint64 m_lastPosition = 0;
  QElapsedTimer m_sincePosition;
};
