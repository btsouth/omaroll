#include "viewer/MprisService.h"

#include "app/AppSettings.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kRootInterface = QStringLiteral("org.mpris.MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kServiceName = QStringLiteral("org.mpris.MediaPlayer2.omaroll");
constexpr double kMinimumRate = 0.25;
constexpr double kMaximumRate = 4.0;

} // namespace

class MprisRootAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
  Q_PROPERTY(bool CanQuit READ canQuit)
  Q_PROPERTY(bool CanRaise READ canRaise)
  Q_PROPERTY(bool HasTrackList READ hasTrackList)
  Q_PROPERTY(QString Identity READ identity)
  Q_PROPERTY(QString DesktopEntry READ desktopEntry)
  Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
  Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
  MprisRootAdaptor(QObject* root, MprisService* service)
      : QDBusAbstractAdaptor(root), m_service(service) {}

  bool canQuit() const { return false; }
  bool canRaise() const { return m_service->canRaise(); }
  bool hasTrackList() const { return false; }
  QString identity() const { return QStringLiteral("Omaroll"); }
  QString desktopEntry() const { return QStringLiteral("io.github.tsouth89.omaroll"); }
  QStringList supportedUriSchemes() const { return {}; }
  QStringList supportedMimeTypes() const { return {}; }

public slots:
  void Raise() { m_service->raise(); }
  void Quit() {}

private:
  MprisService* m_service;
};

class MprisPlayerAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
  Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
  Q_PROPERTY(double Rate READ rate WRITE setRate)
  Q_PROPERTY(QVariantMap Metadata READ metadata)
  Q_PROPERTY(double Volume READ volume WRITE setVolume)
  Q_PROPERTY(qlonglong Position READ position)
  Q_PROPERTY(double MinimumRate READ minimumRate)
  Q_PROPERTY(double MaximumRate READ maximumRate)
  Q_PROPERTY(bool CanGoNext READ canGoNext)
  Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
  Q_PROPERTY(bool CanPlay READ canPlay)
  Q_PROPERTY(bool CanPause READ canPause)
  Q_PROPERTY(bool CanSeek READ canSeek)
  Q_PROPERTY(bool CanControl READ canControl)

public:
  MprisPlayerAdaptor(QObject* root, MprisService* service)
      : QDBusAbstractAdaptor(root), m_service(service) {
    connect(service, &MprisService::seeked, this, &MprisPlayerAdaptor::Seeked);
  }

  QString playbackStatus() const { return m_service->playbackStatus(); }
  double rate() const { return m_service->rate(); }
  void setRate(double rate) { m_service->setRate(rate); }
  QVariantMap metadata() const { return m_service->metadata(); }
  double volume() const { return m_service->volume(); }
  void setVolume(double volume) { m_service->setVolume(volume); }
  qlonglong position() const { return m_service->position(); }
  double minimumRate() const { return kMinimumRate; }
  double maximumRate() const { return kMaximumRate; }
  bool canGoNext() const { return false; }
  bool canGoPrevious() const { return false; }
  bool canPlay() const { return m_service->canPlay(); }
  bool canPause() const { return m_service->canPlay(); }
  bool canSeek() const { return m_service->canSeek(); }
  bool canControl() const { return true; }

public slots:
  void Next() {}
  void Previous() {}
  void Pause() { m_service->pause(); }
  void PlayPause() { m_service->playPause(); }
  void Stop() { m_service->stop(); }
  void Play() { m_service->play(); }
  void Seek(qlonglong offset) { m_service->seek(offset); }
  void SetPosition(const QDBusObjectPath& track, qlonglong position) {
    m_service->setPosition(track, position);
  }
  void OpenUri(const QString&) {}

signals:
  void Seeked(qlonglong position);

private:
  MprisService* m_service;
};

MprisService::MprisService(AppSettings* settings, std::optional<QDBusConnection> bus,
                           QObject* parent)
    : QObject(parent), m_settings(settings), m_bus(std::move(bus)), m_root(new QObject(this)) {
  new MprisRootAdaptor(m_root, this);
  new MprisPlayerAdaptor(m_root, this);
  if (m_settings) {
    connect(m_settings, &AppSettings::videoVolumeChanged, this,
            [this] { changed(kPlayerInterface, {QStringLiteral("Volume")}); });
    connect(m_settings, &AppSettings::videoMutedChanged, this,
            [this] { changed(kPlayerInterface, {QStringLiteral("Volume")}); });
  }
}

MprisService::~MprisService() {
  detach();
  unpublish();
}

void MprisService::track(QObject* object) {
  auto* player = qobject_cast<QMediaPlayer*>(object);
  if (!player || player->source().isEmpty()) {
    return;
  }
  if (player != m_player) {
    detach();
    attach(player);
  }
  publish();
}

void MprisService::release(QObject* object) {
  if (!object || object != m_player) {
    return;
  }
  detach();
  unpublish();
}

void MprisService::attach(QMediaPlayer* player) {
  m_player = player;
  ++m_track;
  m_lastPosition = player->position();
  m_sincePosition.start();
  const auto status = [this] {
    changed(kPlayerInterface, {QStringLiteral("PlaybackStatus"), QStringLiteral("CanPlay"),
                               QStringLiteral("CanPause")});
  };
  const auto data = [this] { changed(kPlayerInterface, {QStringLiteral("Metadata")}); };
  m_connections = {
      connect(player, &QMediaPlayer::playbackStateChanged, this, status),
      connect(player, &QMediaPlayer::durationChanged, this, data),
      connect(player, &QMediaPlayer::metaDataChanged, this, data),
      connect(player, &QMediaPlayer::seekableChanged, this,
              [this] { changed(kPlayerInterface, {QStringLiteral("CanSeek")}); }),
      connect(player, &QMediaPlayer::playbackRateChanged, this,
              [this] { changed(kPlayerInterface, {QStringLiteral("Rate")}); }),
      connect(player, &QMediaPlayer::positionChanged, this, &MprisService::positionMoved),
      // A new file in the same player is a new track; no file at all, or the
      // player going away, ends what there is to control.
      connect(player, &QMediaPlayer::sourceChanged, this,
              [this, player](const QUrl& source) {
                if (source.isEmpty()) {
                  release(player);
                  return;
                }
                ++m_track;
                m_lastPosition = 0;
                m_sincePosition.restart();
                changed(kPlayerInterface, {QStringLiteral("Metadata"),
                                           QStringLiteral("PlaybackStatus")});
              }),
      connect(player, &QObject::destroyed, this, [this] {
        m_connections.clear();
        unpublish();
      }),
  };
  changed(kPlayerInterface, {QStringLiteral("Metadata"), QStringLiteral("PlaybackStatus"),
                             QStringLiteral("CanPlay"), QStringLiteral("CanPause"),
                             QStringLiteral("CanSeek"), QStringLiteral("Rate")});
}

void MprisService::detach() {
  for (const QMetaObject::Connection& connection : std::as_const(m_connections)) {
    disconnect(connection);
  }
  m_connections.clear();
  m_player = nullptr;
}

void MprisService::publish() {
  if (isPublished()) {
    return;
  }
  if (!m_bus) {
    m_bus = QDBusConnection::sessionBus();
  }
  if (!m_bus->isConnected() ||
      !m_bus->registerObject(kObjectPath, m_root, QDBusConnection::ExportAdaptors)) {
    return;
  }
  // Another process already holding the name gets an instance of its own,
  // as the specification asks.
  for (const QString& name :
       {kServiceName,
        kServiceName + QStringLiteral(".instance") + QString::number(QCoreApplication::applicationPid())}) {
    if (m_bus->registerService(name)) {
      m_serviceName = name;
      return;
    }
  }
  m_bus->unregisterObject(kObjectPath);
}

void MprisService::unpublish() {
  if (!isPublished()) {
    return;
  }
  m_bus->unregisterService(m_serviceName);
  m_bus->unregisterObject(kObjectPath);
  m_serviceName.clear();
}

void MprisService::changed(const QString& interface, const QStringList& names) {
  if (!isPublished()) {
    return;
  }
  const QObject* adaptor = nullptr;
  for (const QObject* child : m_root->children()) {
    const QMetaObject* meta = child->metaObject();
    const int info = meta->indexOfClassInfo("D-Bus Interface");
    if (info >= 0 && interface == QLatin1String(meta->classInfo(info).value())) {
      adaptor = child;
    }
  }
  if (!adaptor) {
    return;
  }
  QVariantMap values;
  for (const QString& name : names) {
    values.insert(name, adaptor->property(name.toLatin1().constData()));
  }
  QDBusMessage signal = QDBusMessage::createSignal(
      kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
      QStringLiteral("PropertiesChanged"));
  signal << interface << values << QStringList();
  m_bus->send(signal);
}

// Playback moves the position steadily; anything else is a seek, which MPRIS
// clients hear about so their progress does not drift.
void MprisService::positionMoved(qint64 position) {
  const bool playing = m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
  const double rate = m_player ? m_player->playbackRate() : 1.0;
  const qint64 expected =
      m_lastPosition + (playing ? qint64(double(m_sincePosition.elapsed()) * rate) : 0);
  m_lastPosition = position;
  m_sincePosition.restart();
  if (std::abs(position - expected) > 1000) {
    emit seeked(position * 1000);
  }
}

void MprisService::raise() {
  if (!m_player) {
    return;
  }
  if (auto* item = qobject_cast<QQuickItem*>(m_player->videoOutput()); item && item->window()) {
    item->window()->show();
    item->window()->raise();
    item->window()->requestActivate();
  }
}

bool MprisService::canRaise() const {
  const auto* item = m_player ? qobject_cast<QQuickItem*>(m_player->videoOutput()) : nullptr;
  return item && item->window();
}

QString MprisService::playbackStatus() const {
  if (!m_player) {
    return QStringLiteral("Stopped");
  }
  switch (m_player->playbackState()) {
  case QMediaPlayer::PlayingState:
    return QStringLiteral("Playing");
  case QMediaPlayer::PausedState:
    return QStringLiteral("Paused");
  case QMediaPlayer::StoppedState:
    break;
  }
  return QStringLiteral("Stopped");
}

QDBusObjectPath MprisService::trackId() const {
  return QDBusObjectPath(QStringLiteral("/io/github/tsouth89/omaroll/track/%1").arg(m_track));
}

QVariantMap MprisService::metadata() const {
  if (!m_player) {
    return {};
  }
  const QUrl source = m_player->source();
  QString title = m_player->metaData().stringValue(QMediaMetaData::Title);
  if (title.isEmpty()) {
    title = QFileInfo(source.toLocalFile()).fileName();
  }
  QVariantMap data{
      {QStringLiteral("mpris:trackid"), QVariant::fromValue(trackId())},
      {QStringLiteral("xesam:title"), title},
      {QStringLiteral("xesam:url"), source.toString()},
  };
  if (m_player->duration() > 0) {
    data.insert(QStringLiteral("mpris:length"), qlonglong(m_player->duration()) * 1000);
  }
  return data;
}

qlonglong MprisService::position() const {
  return m_player ? qlonglong(m_player->position()) * 1000 : 0;
}

double MprisService::rate() const { return m_player ? m_player->playbackRate() : 1.0; }

void MprisService::setRate(double rate) {
  // A rate of zero means pause, as the specification allows.
  if (!m_player || !std::isfinite(rate)) {
    return;
  }
  if (rate <= 0) {
    m_player->pause();
    return;
  }
  m_player->setPlaybackRate(std::clamp(rate, kMinimumRate, kMaximumRate));
}

double MprisService::volume() const {
  if (!m_settings) {
    return 1.0;
  }
  return m_settings->videoMuted() ? 0.0 : m_settings->videoVolume();
}

void MprisService::setVolume(double volume) {
  if (!m_settings || !std::isfinite(volume)) {
    return;
  }
  m_settings->setVideoVolume(std::clamp(volume, 0.0, 1.0));
  if (volume > 0 && m_settings->videoMuted()) {
    m_settings->setVideoMuted(false);
  }
}

bool MprisService::canSeek() const { return m_player && m_player->isSeekable(); }

void MprisService::play() {
  if (m_player) {
    m_player->play();
  }
}

void MprisService::pause() {
  if (m_player) {
    m_player->pause();
  }
}

void MprisService::playPause() {
  if (!m_player) {
    return;
  }
  if (m_player->playbackState() == QMediaPlayer::PlayingState) {
    m_player->pause();
  } else {
    m_player->play();
  }
}

// Stopping keeps the first frame on screen, as the end of a video does.
void MprisService::stop() {
  if (m_player) {
    m_player->pause();
    m_player->setPosition(0);
  }
}

void MprisService::seek(qlonglong offsetUs) {
  if (!canSeek()) {
    return;
  }
  const qint64 target = m_player->position() + offsetUs / 1000;
  if (m_player->duration() > 0 && target >= m_player->duration()) {
    return;
  }
  m_player->setPosition(std::max<qint64>(0, target));
}

void MprisService::setPosition(const QDBusObjectPath& track, qlonglong positionUs) {
  if (!canSeek() || track != trackId() || positionUs < 0 ||
      (m_player->duration() > 0 && positionUs / 1000 > m_player->duration())) {
    return;
  }
  m_player->setPosition(positionUs / 1000);
}

#include "MprisService.moc"
