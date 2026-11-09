#include "app/AppSettings.h"
#include "app/StartupTrace.h"
#include "actions/TrashRestore.h"
#include "app/Diagnostics.h"
#include "app/HyprlandSetup.h"
#include "app/VideoPlayback.h"
#include "app/VideoPresentation.h"
#include "sources/FileVersion.h"

#include "library/CaptureRecord.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QSysInfo>
#include <QRegularExpression>
#include <QCache>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <limits>
#include <memory>
#include <cmath>

#include <sys/stat.h>

namespace {
constexpr auto kFavorites = "library/favorites";
constexpr auto kHidden = "library/hidden";
// One "stars:path" entry per rated file, so the ini stays readable by hand.
constexpr auto kRatings = "library/ratings";
constexpr int kMaximumRating = 5;
constexpr auto kCaptions = "library/captions";
constexpr auto kMarkIdentities = "library/markIdentities";
constexpr int kMaximumCaptionLength = 500;
constexpr auto kShowHidden = "library/showHidden";
constexpr auto kSortMode = "library/sortMode";
constexpr auto kKindFilter = "library/kindFilter";
constexpr auto kScanDownloads = "sources/scanDownloads";
constexpr auto kRecursionDepth = "sources/recursionDepth";
constexpr auto kLibraryFolders = "sources/libraryFolders";
constexpr auto kPinnedFolders = "sources/pinnedFolders";
constexpr auto kPairRawJpeg = "library/pairRawJpeg";
constexpr auto kSimilarStrictness = "library/similarStrictness";
constexpr auto kBackgroundTextIndex = "search/backgroundTextIndex";
constexpr auto kTextIndexScreenshotsOnly = "search/textIndexScreenshotsOnly";
constexpr auto kImagePrimaryAction = "actions/imagePrimary";
constexpr auto kVideoPrimaryAction = "actions/videoPrimary";
constexpr auto kThumbnailCacheMb = "cache/maximumMb";
constexpr auto kTileWidth = "view/tileWidth";
constexpr auto kShowGridFilenames = "view/showGridFilenames";
constexpr auto kShowFolderSidebar = "view/showFolderSidebar";
constexpr auto kFolderSidebarWidth = "view/folderSidebarWidth";
constexpr auto kPreviewWidth = "view/previewWidth";
constexpr auto kPreviewHeight = "view/previewHeight";
constexpr int kMinimumTileWidth = 160;
constexpr int kMaximumTileWidth = 480;
constexpr auto kSlideshowVideos = "slideshow/includeVideos";
constexpr auto kConfirmPermanentDelete = "actions/confirmPermanentDelete";
constexpr auto kSlideshowInterval = "slideshow/intervalSeconds";
constexpr auto kSlideshowShuffle = "slideshow/shuffle";
constexpr auto kEnlargeSmallPictures = "viewer/enlargeSmallPictures";
constexpr auto kViewerFilmstrip = "viewer/filmstrip";
constexpr auto kAlbums = "library/albums";
constexpr auto kTags = "library/tags";
constexpr auto kSmartCollections = "library/smartCollections";
constexpr auto kLastVisit = "library/lastVisit";
constexpr auto kVideoVolume = "playback/volume";
constexpr auto kVideoMuted = "playback/muted";
constexpr auto kRememberPlaybackSpeed = "playback/rememberSpeed";
constexpr auto kVideoPlaybackRate = "playback/rate";
constexpr auto kVideoPositions = "playback/positionsV2";
constexpr auto kVideoPositionRecency = "playback/positionRecencyV2";
constexpr auto kLegacyVideoPositions = "playback/positions";
// Resume spots beyond a few minutes are the useful ones; too many entries and
// the file grows for no benefit.
constexpr int kMaximumResumeEntries = 500;
constexpr qint64 kMinimumResumeMs = 5000;
constexpr auto kOrganizationFormat = "omaroll.organization";
// Today's snapshot is taken again this long after the last organization change.
constexpr qint64 kMaximumOrganizationBytes = 16 * 1024 * 1024;

QString normalizedFolder(const QString& path, bool mustExist) {
  const QFileInfo info(path);
  if (mustExist && (!info.isDir() || !info.isReadable())) {
    return {};
  }
  const QString normalized =
      info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
  const QString home = QFileInfo(QDir::homePath()).canonicalFilePath();
  return normalized.isEmpty() || normalized == home || normalized == QDir::rootPath() ? QString()
                                                                                      : normalized;
}

QString normalizedAlbumName(const QString& name) {
  const QString normalized = name.simplified().left(60);
  return normalized.contains(QLatin1Char('/')) ? QString() : normalized;
}

// "travel / japan /" becomes "travel/japan". Empty segments are dropped, so
// a stray slash cannot create a nameless level.
QString normalizedTagName(const QString& name) {
  QStringList segments;
  for (const QString& part : name.split(QLatin1Char('/'))) {
    const QString segment = part.simplified();
    if (!segment.isEmpty()) {
      segments.append(segment);
    }
  }
  return segments.join(QLatin1Char('/')).left(80);
}

bool tagLessThan(const QString& first, const QString& second) {
  const QStringList a = first.split(QLatin1Char('/'));
  const QStringList b = second.split(QLatin1Char('/'));
  for (qsizetype index = 0; index < qMin(a.size(), b.size()); ++index) {
    if (const int order = a.at(index).compare(b.at(index), Qt::CaseInsensitive); order != 0) {
      return order < 0;
    }
  }
  return a.size() < b.size();
}

bool tagIsUnder(const QString& candidate, const QString& parent) {
  return candidate.size() > parent.size() && candidate.startsWith(parent) &&
         candidate.at(parent.size()) == QLatin1Char('/');
}

QString identityVersionFor(const QString& path) {
  struct stat status {};
  if (::stat(QFile::encodeName(path).constData(), &status) != 0) return {};
  // ctime also invalidates an edit whose size and mtime were preserved.
  return QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
      .arg(qulonglong(status.st_dev)).arg(qulonglong(status.st_ino))
      .arg(qlonglong(status.st_size)).arg(qlonglong(status.st_mtim.tv_sec))
      .arg(qlonglong(status.st_mtim.tv_nsec)).arg(qlonglong(status.st_ctim.tv_sec))
      .arg(qlonglong(status.st_ctim.tv_nsec));
}

} // namespace

#ifdef OMAROLL_TESTING
thread_local QStringList* AppSettings::s_fingerprintReadPaths = nullptr;
#endif

AppSettings::AlbumEntry AppSettings::identityFor(const QString& path) {
  AppSettings::AlbumEntry entry;
  entry.path = path;

  struct CachedIdentity {
    QString version;
    AlbumEntry identity;
  };
  static thread_local QCache<QString, CachedIdentity> cache(4096);
  const QString version = identityVersionFor(path);
  if (const auto* cached = cache.object(path); cached && cached->version == version) {
    return cached->identity;
  }
  const QFileInfo info(path);
  if (version.isEmpty() || !info.isFile() || !info.isReadable()) {
    return entry;
  }
  entry.bytes = info.size();
  entry.modified = info.lastModified().toMSecsSinceEpoch();

  struct stat status {};
  if (::stat(QFile::encodeName(path).constData(), &status) == 0) {
    entry.device = status.st_dev;
    entry.inode = status.st_ino;
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return entry;
  }
  constexpr qint64 chunkSize = 64 * 1024;
#ifdef OMAROLL_TESTING
  if (s_fingerprintReadPaths) s_fingerprintReadPaths->append(path);
#endif
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArray::number(entry.bytes));
  hash.addData(file.read(chunkSize));
  if (entry.bytes > chunkSize) {
    file.seek(qMax<qint64>(0, entry.bytes - chunkSize));
    hash.addData(file.read(chunkSize));
  }
  if (file.error() != QFile::NoError || identityVersionFor(path) != version) return entry;
  entry.fingerprint = hash.result();
  entry.resolved = true;
  cache.insert(path, new CachedIdentity{version, entry});
  return entry;
}

bool AppSettings::relocationAvailable(const AlbumEntry& entry) {
  const QFileInfo parent(QFileInfo(entry.path).absolutePath());
  if (QFileInfo::exists(entry.path) || !parent.isDir() || !parent.isReadable()) return false;
  struct stat status {};
  if (::stat(QFile::encodeName(parent.absoluteFilePath()).constData(), &status) != 0) return false;
  // An unmounted source can leave a readable empty mountpoint on the host
  // filesystem. Its device no longer agrees with the remembered member.
  return entry.device == 0 || entry.device == status.st_dev;
}

// Writes snapshot files one after another on its own thread, which ends when
// there is nothing left to write. Standard primitives, so ThreadSanitizer can
// follow every handoff, and each job owns all of its data.
class BackupWriter {
public:
  ~BackupWriter() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return !m_running; });
    if (m_thread.joinable()) m_thread.join();
  }
  void post(std::function<void()> job) {
    std::lock_guard lock(m_mutex);
    m_jobs.push_back(std::move(job));
    if (m_running) return;
    // The last thread has already let go of the lock for good.
    if (m_thread.joinable()) m_thread.join();
    m_running = true;
    m_thread = std::thread([this] { run(); });
  }
  void flush() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return !m_running; });
  }

private:
  void run() {
    std::unique_lock lock(m_mutex);
    while (!m_jobs.empty()) {
      std::function<void()> job = std::move(m_jobs.front());
      m_jobs.pop_front();
      lock.unlock();
      job();
      job = nullptr;
      lock.lock();
    }
    m_running = false;
    m_idle.notify_all();
  }

  std::mutex m_mutex;
  std::condition_variable m_idle;
  std::deque<std::function<void()>> m_jobs;
  bool m_running = false;
  std::thread m_thread;
};

AppSettings::AppSettings(QObject* parent)
    : QObject(parent), m_settings(QSettings::IniFormat, QSettings::UserScope,
                                  QStringLiteral("omaroll"), QStringLiteral("omaroll")),
      m_backupWriter(std::make_unique<BackupWriter>()) {
  const bool existingProfile = !m_settings.allKeys().isEmpty();
  // Saves made while loading wait for the launch check below.
  m_healthPending = m_settings.status() == QSettings::NoError;
  const QStringList favorites = m_settings.value(kFavorites).toStringList();
  m_favorites = QSet<QString>(favorites.begin(), favorites.end());

  const QStringList hidden = m_settings.value(kHidden).toStringList();
  m_hidden = QSet<QString>(hidden.begin(), hidden.end());

  const QStringList ratings = m_settings.value(kRatings).toStringList();
  for (const QString& entry : ratings) {
    const qsizetype colon = entry.indexOf(QLatin1Char(':'));
    bool okay = false;
    const int stars = colon > 0 ? entry.first(colon).toInt(&okay) : 0;
    const QString path = colon > 0 ? entry.sliced(colon + 1) : QString();
    if (okay && stars >= 1 && stars <= kMaximumRating && !path.isEmpty()) {
      m_ratings.insert(path, stars);
    }
  }

  const QVariantMap captions = m_settings.value(kCaptions).toMap();
  for (auto it = captions.cbegin(); it != captions.cend(); ++it) {
    const QString text = it.value().toString().trimmed().left(kMaximumCaptionLength);
    if (!it.key().isEmpty() && !text.isEmpty()) {
      m_captions.insert(it.key(), text);
    }
  }

  m_showHidden = m_settings.value(kShowHidden, false).toBool();
  m_sortMode = qBound(0, m_settings.value(kSortMode, 0).toInt(), 5);
  m_kindFilter = qBound(-1, m_settings.value(kKindFilter, -1).toInt(), 5);
  m_scanDownloads = m_settings.value(kScanDownloads, true).toBool();
  m_recursionDepth = qBound(1, m_settings.value(kRecursionDepth, 4).toInt(), 8);
  const QStringList folders = m_settings.value(kLibraryFolders).toStringList();
  for (const QString& folder : folders) {
    const QString normalized = normalizedFolder(folder, false);
    if (!normalized.isEmpty() && !m_libraryFolders.contains(normalized)) {
      m_libraryFolders.append(normalized);
    }
  }
  const QStringList pinnedFolders = m_settings.value(kPinnedFolders).toStringList();
  for (const QString& folder : pinnedFolders) {
    const QString normalized = normalizedFolder(folder, false);
    if (!normalized.isEmpty() && !m_pinnedFolders.contains(normalized)) {
      m_pinnedFolders.append(normalized);
    }
  }
  m_pairRawJpeg = m_settings.value(kPairRawJpeg, true).toBool();
  m_similarStrictness = qBound(0, m_settings.value(kSimilarStrictness, 1).toInt(), 2);
  m_backgroundTextIndex = m_settings.value(kBackgroundTextIndex, false).toBool();
  m_textIndexScreenshotsOnly = m_settings.value(kTextIndexScreenshotsOnly, false).toBool();
  const QString imageAction =
      m_settings.value(kImagePrimaryAction, existingProfile ? QStringLiteral("matte")
                                                          : QStringLiteral("preview")).toString();
  if (QStringList {QStringLiteral("preview"), QStringLiteral("omaframe"), QStringLiteral("corrections"), QStringLiteral("matte"), QStringLiteral("view"), QStringLiteral("edit")}
          .contains(imageAction)) {
    m_imagePrimaryAction = imageAction;
  }
  const QString videoAction =
      m_settings.value(kVideoPrimaryAction, existingProfile ? QStringLiteral("trim")
                                                          : QStringLiteral("preview")).toString();
  if (QStringList {QStringLiteral("preview"), QStringLiteral("trim"), QStringLiteral("play")}.contains(videoAction)) {
    m_videoPrimaryAction = videoAction;
  }
  // Persist resolved defaults so a fresh profile stays on native viewing after restart.
  if (!m_settings.contains(kImagePrimaryAction)) setValue(kImagePrimaryAction, m_imagePrimaryAction);
  if (!m_settings.contains(kVideoPrimaryAction)) setValue(kVideoPrimaryAction, m_videoPrimaryAction);
  m_thumbnailCacheMb = qBound(64, m_settings.value(kThumbnailCacheMb, 256).toInt(), 1024);
  m_tileWidth =
      qBound(kMinimumTileWidth, m_settings.value(kTileWidth, 240).toInt(), kMaximumTileWidth);
  m_showGridFilenames = m_settings.value(kShowGridFilenames, false).toBool();
  m_showFolderSidebar = m_settings.value(kShowFolderSidebar, false).toBool();
  m_folderSidebarWidth = qBound(180, m_settings.value(kFolderSidebarWidth, 240).toInt(), 400);
  m_previewWidth = qBound(500, m_settings.value(kPreviewWidth, 1000).toInt(), 8192);
  m_previewHeight = qBound(360, m_settings.value(kPreviewHeight, 700).toInt(), 8192);
  m_slideshowVideos = m_settings.value(kSlideshowVideos, false).toBool();
  m_confirmPermanentDelete = m_settings.value(kConfirmPermanentDelete, true).toBool();
  m_slideshowIntervalSeconds = qBound(2, m_settings.value(kSlideshowInterval, 4).toInt(), 20);
  m_slideshowShuffle = m_settings.value(kSlideshowShuffle, false).toBool();
  m_enlargeSmallPictures = m_settings.value(kEnlargeSmallPictures, true).toBool();
  m_viewerFilmstrip = m_settings.value(kViewerFilmstrip, false).toBool();
  m_videoVolume = qBound(0.0, m_settings.value(kVideoVolume, 0.8).toDouble(), 1.0);
  m_videoMuted = m_settings.value(kVideoMuted, false).toBool();
  m_rememberPlaybackSpeed = m_settings.value(kRememberPlaybackSpeed, false).toBool();
  const qreal savedRate = m_settings.value(kVideoPlaybackRate, 1.0).toDouble();
  m_videoPlaybackRate = std::isfinite(savedRate) ? qBound(0.25, savedRate, 4.0) : 1.0;
  const QVariantMap storedAlbums = m_settings.value(kAlbums).toMap();
  const auto restoreCollections = [](const QVariantMap& storedCollections,
                                         QMap<QString, QList<AlbumEntry>>& target,
                                         QString (*normalize)(const QString&)) {
    for (auto it = storedCollections.cbegin(); it != storedCollections.cend(); ++it) {
      const QString name = normalize(it.key());
      if (name.isEmpty()) {
        continue;
      }
      QList<AlbumEntry> entries;
      const QVariantList storedEntries = it.value().toList();
      for (const QVariant& value : storedEntries) {
        const QVariantMap stored = value.toMap();
        AlbumEntry entry;
        entry.path = stored.value(QStringLiteral("path")).toString();
        entry.bytes = stored.value(QStringLiteral("bytes"), -1).toLongLong();
        entry.modified = stored.value(QStringLiteral("modified")).toLongLong();
        entry.fingerprint = stored.value(QStringLiteral("fingerprint")).toByteArray();
        entry.device = stored.value(QStringLiteral("device")).toString().toULongLong();
        entry.inode = stored.value(QStringLiteral("inode")).toString().toULongLong();
        if (!entry.path.isEmpty()) {
          struct stat status {};
          // Inode and size together. A freed inode number is handed to the
          // next file created on ext4 and xfs, so the number alone would
          // repoint an entry to whatever was saved after a delete.
          // On btrfs a subvolume's device number is anonymous and can change
          // between boots, so the device alone must not unresolve an entry:
          // the mtime stands in for it then.
          if (entry.device != 0 && entry.inode != 0 &&
              ::stat(QFile::encodeName(entry.path).constData(), &status) == 0 &&
              entry.inode == status.st_ino && entry.bytes == static_cast<qint64>(status.st_size) &&
              (entry.device == status.st_dev ||
               entry.modified == QFileInfo(entry.path).lastModified().toMSecsSinceEpoch())) {
            entry.resolved = true;
          }
          entries.append(entry);
        }
      }
      target.insert(name, entries);
    }
  };
  restoreCollections(storedAlbums, m_albums, normalizedAlbumName);
  restoreCollections(m_settings.value(kTags).toMap(), m_tags, normalizedTagName);

  const QVariantMap storedSmart = m_settings.value(kSmartCollections).toMap();
  for (auto it = storedSmart.cbegin(); it != storedSmart.cend(); ++it) {
    const QString name = normalizedAlbumName(it.key());
    if (!name.isEmpty()) {
      m_smartCollections.insert(name, it.value().toMap());
    }
  }

  // Identities behind marks, so a file moved outside Omaroll keeps its
  // favourite, rating and caption on the next scan.
  const QVariantMap storedMarkIdentities = m_settings.value(kMarkIdentities).toMap();
  for (auto it = storedMarkIdentities.cbegin(); it != storedMarkIdentities.cend(); ++it) {
    if (it.key().isEmpty()) {
      continue;
    }
    const QVariantMap stored = it.value().toMap();
    AlbumEntry entry;
    entry.path = it.key();
    entry.bytes = stored.value(QStringLiteral("bytes"), -1).toLongLong();
    entry.modified = stored.value(QStringLiteral("modified")).toLongLong();
    entry.fingerprint = stored.value(QStringLiteral("fingerprint")).toByteArray();
    entry.device = stored.value(QStringLiteral("device")).toString().toULongLong();
    entry.inode = stored.value(QStringLiteral("inode")).toString().toULongLong();
    m_markIdentities.insert(it.key(), entry);
  }

  m_videoRecency = m_settings.value(kVideoPositionRecency).toStringList();
  QVariantMap storedPositions = m_settings.value(kVideoPositions).toMap();
  // Path-keyed resume positions move to file identities. The migration drops
  // entries for files that are missing or unmounted right now, so the raw
  // settings file is copied into the backups folder first. Without that copy
  // the legacy key stays for a later start; it is removed only once migrated.
  bool migrated = false;
  if (settingsStatus() != QSettings::FormatError && m_settings.contains(kLegacyVideoPositions) &&
      backupLegacySettings()) {
    if (storedPositions.isEmpty()) m_videoRecency.clear();
    const QVariantMap legacy = m_settings.value(kLegacyVideoPositions).toMap();
    // The old map loads in key order too. Preserve that order while changing
    // identities, rather than sorting by the new device/inode keys.
    for (auto it = legacy.cbegin(); it != legacy.cend(); ++it) {
      const QString key = FileVersion::key(it.key());
      const qint64 position = it.value().toLongLong();
      if (!key.isEmpty() && QFileInfo(it.key()).isFile() && position >= kMinimumResumeMs &&
          !storedPositions.contains(key) && m_videoRecency.size() < kMaximumResumeEntries) {
        storedPositions.insert(key, position);
        m_videoRecency.append(key);
      }
    }
    setValue(kVideoPositions, storedPositions);
    setValue(kVideoPositionRecency, m_videoRecency);
    migrated = true;
  }
  for (auto it = storedPositions.cbegin(); it != storedPositions.cend(); ++it) {
    const qint64 position = it.value().toLongLong();
    if (!it.key().isEmpty() && position >= kMinimumResumeMs) {
      m_videoPositions.insert(it.key(), position);
      if (!m_videoRecency.contains(it.key())) m_videoRecency.append(it.key());
    }
  }
  m_videoRecency.removeIf([this](const QString& key) { return !m_videoPositions.contains(key); });
  if (migrated) {
    m_settings.remove(kLegacyVideoPositions);
    m_settings.sync();
  }
  m_healthPoll.setInterval(25);
  connect(&m_healthPoll, &QTimer::timeout, this, &AppSettings::applyOrganizationHealth);
  m_snapshotRefresh.setSingleShot(true);
  connect(&m_snapshotRefresh, &QTimer::timeout, this, [this] {
    if (!m_snapshotDirty || snapshotsHeld()) return;
    m_snapshotDirty = false;
    scheduleDailySnapshot(true);
  });
  // A fresh profile has nothing to keep; its first organization save takes the
  // snapshot. Either way the launch is checked against recent snapshots first.
  if (settingsStatus() == QSettings::NoError) checkOrganizationHealth(existingProfile);
  else if (canRestoreOrganization())
    m_launchWarning = QStringLiteral("Your settings file is damaged, so albums, tags and marks may be missing and cannot be saved. Restore the last good backup now? What can still be read is kept in a backup first.");
  m_previousVisit = m_settings.value(kLastVisit).toString();
  setValue(kLastVisit, QDateTime::currentDateTime().toString(Qt::ISODate));
}

AppSettings::~AppSettings() {
  // A clean exit takes the refresh still waiting; the backup writer then
  // finishes its queue before it is destroyed.
  m_snapshotRefresh.stop();
  m_healthPoll.stop();
  flushBackups();
  if (m_snapshotDirty && !snapshotsHeld()) scheduleDailySnapshot(true);
}

void AppSettings::setShowHidden(bool value) {
  if (m_showHidden == value) {
    return;
  }
  m_showHidden = value;
  setValue(kShowHidden, value);
  emit showHiddenChanged();
}

void AppSettings::setSortMode(int value) {
  // Six modes; an ini edited by hand or by an older version must not leave
  // the sort menu reading "undefined".
  const int bounded = qBound(0, value, 5);
  if (m_sortMode == bounded) {
    return;
  }
  m_sortMode = bounded;
  setValue(kSortMode, bounded);
  emit sortModeChanged();
}

void AppSettings::setKindFilter(int value) {
  const int bounded = qBound(-1, value, 5);
  if (m_kindFilter == bounded) {
    return;
  }
  m_kindFilter = bounded;
  setValue(kKindFilter, bounded);
  emit kindFilterChanged();
}

void AppSettings::setScanDownloads(bool value) {
  if (m_scanDownloads == value) {
    return;
  }
  m_scanDownloads = value;
  setValue(kScanDownloads, value);
  emit scanDownloadsChanged();
}

void AppSettings::setRecursionDepth(int value) {
  const int bounded = qBound(1, value, 8);
  if (m_recursionDepth == bounded) {
    return;
  }
  m_recursionDepth = bounded;
  setValue(kRecursionDepth, bounded);
  emit recursionDepthChanged();
}

bool AppSettings::addLibraryFolder(const QUrl& folder) {
  const QString normalized = normalizedFolder(folder.toLocalFile(), true);
  if (normalized.isEmpty() || m_libraryFolders.contains(normalized)) {
    return false;
  }
  m_libraryFolders.append(normalized);
  setValue(kLibraryFolders, m_libraryFolders);
  emit libraryFoldersChanged();
  return true;
}

void AppSettings::removeLibraryFolder(const QString& folder) {
  const QString normalized = normalizedFolder(folder, false);
  if (normalized.isEmpty() || !m_libraryFolders.removeOne(normalized)) {
    return;
  }
  setValue(kLibraryFolders, m_libraryFolders);
  emit libraryFoldersChanged();
}

bool AppSettings::pinFolder(const QUrl& folder) {
  const QString normalized = normalizedFolder(folder.toLocalFile(), true);
  if (normalized.isEmpty() || m_pinnedFolders.contains(normalized)) {
    return false;
  }

  m_pinnedFolders.append(normalized);
  setValue(kPinnedFolders, m_pinnedFolders);
  emit pinnedFoldersChanged();
  addLibraryFolder(QUrl::fromLocalFile(normalized));
  return true;
}

void AppSettings::unpinFolder(const QString& folder) {
  const QString normalized = normalizedFolder(folder, false);
  if (normalized.isEmpty() || !m_pinnedFolders.removeOne(normalized)) {
    return;
  }

  setValue(kPinnedFolders, m_pinnedFolders);
  emit pinnedFoldersChanged();
}

bool AppSettings::isFolderPinned(const QString& folder) const {
  const QString normalized = normalizedFolder(folder, false);
  return !normalized.isEmpty() && m_pinnedFolders.contains(normalized);
}

void AppSettings::setPairRawJpeg(bool value) {
  if (m_pairRawJpeg == value) {
    return;
  }
  m_pairRawJpeg = value;
  setValue(kPairRawJpeg, value);
  emit pairRawJpegChanged();
}

void AppSettings::setSimilarStrictness(int value) {
  value = qBound(0, value, 2);
  if (m_similarStrictness == value) {
    return;
  }
  m_similarStrictness = value;
  setValue(kSimilarStrictness, value);
  emit similarStrictnessChanged();
}

void AppSettings::setBackgroundTextIndex(bool value) {
  if (m_backgroundTextIndex == value) {
    return;
  }
  m_backgroundTextIndex = value;
  setValue(kBackgroundTextIndex, value);
  emit backgroundTextIndexChanged();
}

void AppSettings::setTextIndexScreenshotsOnly(bool value) {
  if (m_textIndexScreenshotsOnly == value) {
    return;
  }
  m_textIndexScreenshotsOnly = value;
  setValue(kTextIndexScreenshotsOnly, value);
  emit textIndexScreenshotsOnlyChanged();
}

void AppSettings::setImagePrimaryAction(const QString& action) {
  static const QStringList allowed = {QStringLiteral("preview"), QStringLiteral("omaframe"), QStringLiteral("corrections"), QStringLiteral("matte"), QStringLiteral("view"),
                                      QStringLiteral("edit")};
  if (!allowed.contains(action) || m_imagePrimaryAction == action) {
    return;
  }
  m_imagePrimaryAction = action;
  setValue(kImagePrimaryAction, action);
  emit imagePrimaryActionChanged();
}

void AppSettings::setVideoPrimaryAction(const QString& action) {
  static const QStringList allowed = {QStringLiteral("preview"), QStringLiteral("trim"), QStringLiteral("play")};
  if (!allowed.contains(action) || m_videoPrimaryAction == action) {
    return;
  }
  m_videoPrimaryAction = action;
  setValue(kVideoPrimaryAction, action);
  emit videoPrimaryActionChanged();
}

void AppSettings::setTileWidth(int width) {
  const int bounded = qBound(kMinimumTileWidth, width, kMaximumTileWidth);
  if (m_tileWidth == bounded) {
    return;
  }
  m_tileWidth = bounded;
  setValue(kTileWidth, bounded);
  emit tileWidthChanged();
}

void AppSettings::setShowGridFilenames(bool value) {
  if (m_showGridFilenames == value) {
    return;
  }
  m_showGridFilenames = value;
  m_settings.setValue(kShowGridFilenames, value);
  emit showGridFilenamesChanged();
}

void AppSettings::setShowFolderSidebar(bool value) {
  if (m_showFolderSidebar == value) return;
  m_showFolderSidebar = value;
  m_settings.setValue(kShowFolderSidebar, value);
  emit showFolderSidebarChanged();
}

void AppSettings::setFolderSidebarWidth(int width) {
  const int bounded = qBound(180, width, 400);
  if (m_folderSidebarWidth == bounded) return;
  m_folderSidebarWidth = bounded;
  m_settings.setValue(kFolderSidebarWidth, bounded);
  emit folderSidebarWidthChanged();
}

void AppSettings::setPreviewWidth(int width) {
  const int bounded = qBound(500, width, 8192);
  if (m_previewWidth == bounded) {
    return;
  }
  m_previewWidth = bounded;
  m_settings.setValue(kPreviewWidth, bounded);
  emit previewWidthChanged();
}

void AppSettings::setPreviewHeight(int height) {
  const int bounded = qBound(360, height, 8192);
  if (m_previewHeight == bounded) {
    return;
  }
  m_previewHeight = bounded;
  m_settings.setValue(kPreviewHeight, bounded);
  emit previewHeightChanged();
}

void AppSettings::setThumbnailCacheMb(int megabytes) {
  const int bounded = qBound(64, megabytes, 1024);
  if (m_thumbnailCacheMb == bounded) {
    return;
  }
  m_thumbnailCacheMb = bounded;
  setValue(kThumbnailCacheMb, bounded);
  emit thumbnailCacheMbChanged();
}

void AppSettings::setSlideshowVideos(bool value) {
  if (m_slideshowVideos == value) {
    return;
  }
  m_slideshowVideos = value;
  setValue(kSlideshowVideos, value);
  emit slideshowVideosChanged();
}

void AppSettings::setConfirmPermanentDelete(bool value) {
  if (m_confirmPermanentDelete == value) return;
  m_confirmPermanentDelete = value;
  setValue(kConfirmPermanentDelete, value);
  emit confirmPermanentDeleteChanged();
}

void AppSettings::setSlideshowIntervalSeconds(int seconds) {
  const int bounded = qBound(2, seconds, 20);
  if (m_slideshowIntervalSeconds == bounded) {
    return;
  }
  m_slideshowIntervalSeconds = bounded;
  setValue(kSlideshowInterval, bounded);
  emit slideshowIntervalSecondsChanged();
}

void AppSettings::setEnlargeSmallPictures(bool value) {
  if (m_enlargeSmallPictures == value) return;
  m_enlargeSmallPictures = value;
  m_settings.setValue(kEnlargeSmallPictures, value);
  emit enlargeSmallPicturesChanged();
}

void AppSettings::setViewerFilmstrip(bool value) {
  if (m_viewerFilmstrip == value) {
    return;
  }
  m_viewerFilmstrip = value;
  setValue(kViewerFilmstrip, value);
  emit viewerFilmstripChanged();
}

void AppSettings::setSlideshowShuffle(bool value) {
  if (m_slideshowShuffle == value) {
    return;
  }
  m_slideshowShuffle = value;
  setValue(kSlideshowShuffle, value);
  emit slideshowShuffleChanged();
}

qint64 AppSettings::videoPosition(const QString& path) const {
  return m_videoPositions.value(FileVersion::key(path), 0);
}

QString AppSettings::videoIdentity(const QString& path) const {
  return FileVersion::key(path);
}

void AppSettings::setVideoPosition(const QString& path, qint64 milliseconds,
                                   const QString& identity) {
  const QString key = FileVersion::key(path);
  if (key.isEmpty() || (!identity.isEmpty() && identity != key)) {
    return;
  }
  if (milliseconds < kMinimumResumeMs) {
    clearVideoPosition(path);
    return;
  }
  m_videoPositions.insert(key, milliseconds);
  m_videoRecency.removeAll(key);
  m_videoRecency.prepend(key);
  while (m_videoRecency.size() > kMaximumResumeEntries) {
    m_videoPositions.remove(m_videoRecency.takeLast());
  }
  QVariantMap stored;
  for (auto it = m_videoPositions.cbegin(); it != m_videoPositions.cend(); ++it) {
    stored.insert(it.key(), it.value());
  }
  setValue(kVideoPositions, stored);
  setValue(kVideoPositionRecency, m_videoRecency);
}

void AppSettings::clearVideoPosition(const QString& path) {
  const QString key = FileVersion::key(path);
  if (m_videoPositions.remove(key) == 0 && !m_videoRecency.contains(key)) {
    return;
  }
  m_videoRecency.removeAll(key);
  QVariantMap stored;
  for (auto it = m_videoPositions.cbegin(); it != m_videoPositions.cend(); ++it) {
    stored.insert(it.key(), it.value());
  }
  setValue(kVideoPositions, stored);
  setValue(kVideoPositionRecency, m_videoRecency);
}

void AppSettings::setVideoVolume(qreal value) {
  const qreal bounded = qBound(0.0, value, 1.0);
  if (qFuzzyCompare(m_videoVolume, bounded)) {
    return;
  }
  m_videoVolume = bounded;
  setValue(kVideoVolume, bounded);
  emit videoVolumeChanged();
}

void AppSettings::prepareVideoPlayback() const {
  StartupTrace::note("player");
  configureVideoPlayback();
}

void AppSettings::setVideoMuted(bool value) {
  if (m_videoMuted == value) {
    return;
  }
  m_videoMuted = value;
  setValue(kVideoMuted, value);
  emit videoMutedChanged();
}

void AppSettings::setRememberPlaybackSpeed(bool value) {
  if (m_rememberPlaybackSpeed == value) {
    return;
  }
  m_rememberPlaybackSpeed = value;
  m_settings.setValue(kRememberPlaybackSpeed, value);
  emit rememberPlaybackSpeedChanged();
}

void AppSettings::setVideoPlaybackRate(qreal value) {
  if (!std::isfinite(value)) {
    return;
  }
  const qreal bounded = qBound(0.25, value, 4.0);
  if (qFuzzyCompare(m_videoPlaybackRate, bounded)) {
    return;
  }
  m_videoPlaybackRate = bounded;
  m_settings.setValue(kVideoPlaybackRate, bounded);
  emit videoPlaybackRateChanged();
}

void AppSettings::relocatePath(const QString& oldPath, const QString& newPath) {
  if (oldPath.isEmpty() || newPath.isEmpty() || oldPath == newPath) {
    return;
  }

  clearMarksUndo();
  m_deferOrganizationSync = true;
  bool marksChangedValue = false;
  if (m_favorites.remove(oldPath)) {
    m_favorites.insert(newPath);
    marksChangedValue = true;
  }
  if (m_hidden.remove(oldPath)) {
    m_hidden.insert(newPath);
    marksChangedValue = true;
  }
  if (const int stars = m_ratings.take(oldPath); stars > 0) {
    m_ratings.insert(newPath, stars);
    marksChangedValue = true;
  }
  if (const QString text = m_captions.take(oldPath); !text.isEmpty()) {
    m_captions.insert(newPath, text);
    marksChangedValue = true;
  }
  // The move recovery identity follows the mark to its new path.
  if (const auto identity = m_markIdentities.take(oldPath); identity.resolved) {
    const AlbumEntry moved = identityFor(newPath);
    if (moved.resolved) {
      m_markIdentities.insert(newPath, moved);
    }
  }
  if (marksChangedValue) {
    persistMarks();
    emit marksChanged();
  }

  bool albumsChangedValue = false;
  for (auto album = m_albums.begin(); album != m_albums.end(); ++album) {
    for (AlbumEntry& entry : album.value()) {
      if (entry.path == oldPath) {
        entry = identityFor(newPath);
        albumsChangedValue = true;
      }
    }
  }
  if (albumsChangedValue) {
    persistAlbums();
    emit albumsChanged();
  }

  bool tagsChangedValue = false;
  for (auto tag = m_tags.begin(); tag != m_tags.end(); ++tag) {
    for (AlbumEntry& entry : tag.value()) {
      if (entry.path == oldPath) {
        entry = identityFor(newPath);
        tagsChangedValue = true;
      }
    }
  }
  if (tagsChangedValue) {
    persistTags();
    emit tagsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
}

QStringList AppSettings::albumNames() const {
  QStringList names = m_albums.keys();
  names.sort(Qt::CaseInsensitive);
  return names;
}

QStringList AppSettings::albumPaths(const QString& name) const {
  QStringList paths;
  for (const AlbumEntry& entry : m_albums.value(name)) {
    if (entry.resolved) {
      paths.append(entry.path);
    }
  }
  return paths;
}

int AppSettings::albumItemCount(const QString& name) const {
  return static_cast<int>(m_albums.value(name).size());
}

int AppSettings::unavailableAlbumItemCount(const QString& name) const {
  int count = 0;
  for (const AlbumEntry& entry : m_albums.value(name)) {
    count += entry.resolved ? 0 : 1;
  }
  return count;
}

bool AppSettings::createAlbum(const QString& name) {
  const QString normalized = normalizedAlbumName(name);
  if (normalized.isEmpty()) {
    return false;
  }
  for (const QString& existing : m_albums.keys()) {
    if (existing.compare(normalized, Qt::CaseInsensitive) == 0) {
      return false;
    }
  }
  m_albums.insert(normalized, {});
  persistAlbums();
  emit albumsChanged();
  return m_organizationError.isEmpty();
}

int AppSettings::deleteAlbum(const QString& name) {
  if (m_albums.remove(name) == 0) {
    return 0;
  }
  const int views = removeViewsNaming(QStringLiteral("album"),
                                      [&name](const QString& album) { return album == name; });
  // One sync for both, so a saved view never outlives its album on disk.
  m_deferOrganizationSync = true;
  persistAlbums();
  if (views > 0) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
  emit albumsChanged();
  return views;
}

int AppSettings::removeViewsNaming(const QString& key,
                                   const std::function<bool(const QString&)>& matches) {
  return static_cast<int>(m_smartCollections.removeIf([&](QMap<QString, QVariantMap>::iterator view) {
    const QString value = view.value().value(key).toString();
    return !value.isEmpty() && matches(value);
  }));
}

bool AppSettings::renameAlbum(const QString& oldName, const QString& newName) {
  const QString target = normalizedAlbumName(newName);
  if (target.isEmpty() || !m_albums.contains(oldName)) {
    return false;
  }
  if (target != oldName) {
    for (const QString& existing : m_albums.keys()) {
      if (existing != oldName && existing.compare(target, Qt::CaseInsensitive) == 0) {
        return false;
      }
    }
  }
  // Membership moves with the name; nothing is re-resolved, so a rename cannot
  // lose an entry the way a delete-and-recreate would.
  m_albums.insert(target, m_albums.take(oldName));
  // Saved views name their album too.
  bool viewsChanged = false;
  for (auto view = m_smartCollections.begin(); view != m_smartCollections.end(); ++view) {
    if (view.value().value(QStringLiteral("album")).toString() == oldName) {
      view.value().insert(QStringLiteral("album"), target);
      viewsChanged = true;
    }
  }
  m_deferOrganizationSync = true;
  persistAlbums();
  if (viewsChanged) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
  emit albumsChanged();
  return m_organizationError.isEmpty();
}

bool AppSettings::addToAlbum(const QString& name, const QStringList& paths) {
  auto it = m_albums.find(name);
  if (it == m_albums.end()) {
    return false;
  }
  bool changed = false;
  for (const QString& path : paths) {
    // An entry already here for this path stands, unless its file is gone:
    // then a new file at the same name is a new member and takes its place.
    qsizetype stale = -1;
    bool present = false;
    for (qsizetype row = 0; row < it->size(); ++row) {
      if (it->at(row).path == path) {
        if (it->at(row).resolved) {
          present = true;
        } else {
          stale = row;
        }
        break;
      }
    }
    if (present) {
      continue;
    }
    AlbumEntry entry = identityFor(path);
    if (!entry.resolved) {
      continue;
    }
    if (stale >= 0) {
      (*it)[stale] = std::move(entry);
    } else {
      it->append(std::move(entry));
    }
    changed = true;
  }
  if (changed) {
    persistAlbums();
    emit albumsChanged();
  }
  return changed && m_organizationError.isEmpty();
}

void AppSettings::removeFromAlbum(const QString& name, const QStringList& paths) {
  auto it = m_albums.find(name);
  if (it == m_albums.end()) {
    return;
  }
  bool changed = false;
  for (qsizetype index = it->size() - 1; index >= 0; --index) {
    if (paths.contains(it->at(index).path)) {
      it->removeAt(index);
      changed = true;
    }
  }
  if (changed) {
    persistAlbums();
    emit albumsChanged();
  }
}

void AppSettings::removeUnavailableFromAlbum(const QString& name) {
  auto it = m_albums.find(name);
  if (it == m_albums.end()) {
    return;
  }
  bool changed = false;
  for (qsizetype index = it->size() - 1; index >= 0; --index) {
    if (!it->at(index).resolved) {
      it->removeAt(index);
      changed = true;
    }
  }
  if (changed) {
    persistAlbums();
    emit albumsChanged();
  }
}

QStringList AppSettings::tagNames() const {
  QStringList names = m_tags.keys();
  std::sort(names.begin(), names.end(), tagLessThan);
  return names;
}

QStringList AppSettings::tagPaths(const QString& name) const {
  QStringList paths;
  QSet<QString> seen;
  const auto collect = [&](const QList<AlbumEntry>& entries) {
    for (const AlbumEntry& entry : entries) {
      if (entry.resolved && !seen.contains(entry.path)) {
        paths.append(entry.path);
        seen.insert(entry.path);
      }
    }
  };
  collect(m_tags.value(name));
  for (auto it = m_tags.cbegin(); it != m_tags.cend(); ++it) {
    if (tagIsUnder(it.key(), name)) {
      collect(it.value());
    }
  }
  return paths;
}

QStringList AppSettings::tagsForPath(const QString& path) const {
  QStringList tags;
  for (auto it = m_tags.cbegin(); it != m_tags.cend(); ++it) {
    for (const AlbumEntry& entry : it.value()) {
      if (entry.resolved && entry.path == path) {
        tags.append(it.key());
        break;
      }
    }
  }
  return tags;
}

QHash<QString, QStringList> AppSettings::tagsByPath() const {
  QHash<QString, QStringList> byPath;
  for (auto it = m_tags.cbegin(); it != m_tags.cend(); ++it) {
    for (const AlbumEntry& entry : it.value()) {
      if (entry.resolved) byPath[entry.path].append(it.key());
    }
  }
  return byPath;
}

QHash<QString, QStringList> AppSettings::albumsByPath() const {
  QHash<QString, QStringList> byPath;
  for (auto it = m_albums.cbegin(); it != m_albums.cend(); ++it) {
    for (const AlbumEntry& entry : it.value()) {
      if (entry.resolved) byPath[entry.path].append(it.key());
    }
  }
  return byPath;
}

int AppSettings::tagItemCount(const QString& name) const {
  return static_cast<int>(tagPaths(name).size());
}

bool AppSettings::createTag(const QString& name) {
  const QString normalized = normalizedTagName(name);
  if (normalized.isEmpty()) {
    return false;
  }
  for (const QString& existing : m_tags.keys()) {
    if (existing.compare(normalized, Qt::CaseInsensitive) == 0) {
      return false;
    }
  }
  // Every ancestor exists too, so the tree in Browse has no gaps and a
  // parent can be chosen to see everything under it.
  const QStringList segments = normalized.split(QLatin1Char('/'));
  for (qsizetype depth = 1; depth <= segments.size(); ++depth) {
    const QString level = segments.first(depth).join(QLatin1Char('/'));
    bool present = false;
    for (const QString& existing : m_tags.keys()) {
      if (existing.compare(level, Qt::CaseInsensitive) == 0) {
        present = true;
        break;
      }
    }
    if (!present) {
      m_tags.insert(level, {});
    }
  }
  persistTags();
  emit tagsChanged();
  return m_organizationError.isEmpty();
}

int AppSettings::deleteTag(const QString& name) {
  int removed = m_tags.remove(name);
  for (const QString& existing : m_tags.keys()) {
    if (tagIsUnder(existing, name)) {
      removed += m_tags.remove(existing);
    }
  }
  if (removed == 0) {
    return 0;
  }
  const int views = removeViewsNaming(QStringLiteral("tag"), [&name](const QString& tag) {
    return tag == name || tagIsUnder(tag, name);
  });
  m_deferOrganizationSync = true;
  persistTags();
  if (views > 0) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
  emit tagsChanged();
  return views;
}

bool AppSettings::renameTag(const QString& oldName, const QString& newName) {
  const QString target = normalizedTagName(newName);
  if (target.isEmpty() || target == oldName || !m_tags.contains(oldName)) {
    return false;
  }
  // Renaming a tag into its own subtree would make the move ambiguous.
  if (tagIsUnder(target, oldName)) {
    return false;
  }

  QList<QPair<QString, QString>> moves;
  QSet<QString> sources;
  for (const QString& key : m_tags.keys()) {
    if (key == oldName) {
      moves.append({key, target});
      sources.insert(key);
    } else if (tagIsUnder(key, oldName)) {
      moves.append({key, target + key.mid(oldName.size())});
      sources.insert(key);
    }
  }
  if (moves.isEmpty()) {
    return false;
  }

  // A new name must not land on a tag that is staying where it is, or the
  // rename would silently swallow it.
  QSet<QString> targetsLower;
  for (const auto& move : moves) {
    targetsLower.insert(move.second.toLower());
  }
  for (const QString& key : m_tags.keys()) {
    if (!sources.contains(key) && targetsLower.contains(key.toLower())) {
      return false;
    }
  }

  QMap<QString, QList<AlbumEntry>> moved;
  for (const auto& move : moves) {
    moved.insert(move.second, m_tags.take(move.first));
  }
  // Keep the tree gapless: every ancestor of the new name exists, as creating
  // a tag guarantees.
  const QStringList segments = target.split(QLatin1Char('/'));
  for (qsizetype depth = 1; depth <= segments.size(); ++depth) {
    const QString level = segments.first(depth).join(QLatin1Char('/'));
    if (!m_tags.contains(level) && !moved.contains(level)) {
      m_tags.insert(level, {});
    }
  }
  for (auto it = moved.cbegin(); it != moved.cend(); ++it) {
    m_tags.insert(it.key(), it.value());
  }
  // Saved views hold the tag by name, so a rename has to follow it or the
  // view reopens onto an empty tag.
  bool viewsChanged = false;
  for (auto view = m_smartCollections.begin(); view != m_smartCollections.end(); ++view) {
    const QString viewTag = view.value().value(QStringLiteral("tag")).toString();
    if (viewTag == oldName) {
      view.value().insert(QStringLiteral("tag"), target);
      viewsChanged = true;
    } else if (tagIsUnder(viewTag, oldName)) {
      view.value().insert(QStringLiteral("tag"), target + viewTag.mid(oldName.size()));
      viewsChanged = true;
    }
  }
  m_deferOrganizationSync = true;
  persistTags();
  if (viewsChanged) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
  emit tagsChanged();
  return m_organizationError.isEmpty();
}

bool AppSettings::addTag(const QString& name, const QStringList& paths) {
  auto it = m_tags.find(name);
  if (it == m_tags.end()) {
    return false;
  }
  bool changed = false;
  for (const QString& path : paths) {
    bool present = false;
    for (const AlbumEntry& entry : std::as_const(*it)) {
      if (entry.path == path && entry.resolved) {
        present = true;
        break;
      }
    }
    if (!present) {
      AlbumEntry entry = identityFor(path);
      if (entry.resolved) {
        it->append(std::move(entry));
        changed = true;
      }
    }
  }
  if (changed) {
    persistTags();
    emit tagsChanged();
  }
  return changed && m_organizationError.isEmpty();
}

void AppSettings::removeTag(const QString& name, const QStringList& paths) {
  auto it = m_tags.find(name);
  if (it == m_tags.end()) {
    return;
  }
  bool changed = false;
  for (qsizetype index = it->size() - 1; index >= 0; --index) {
    if (paths.contains(it->at(index).path)) {
      it->removeAt(index);
      changed = true;
    }
  }
  if (changed) {
    persistTags();
    emit tagsChanged();
  }
}

QStringList AppSettings::smartCollectionNames() const {
  QStringList names = m_smartCollections.keys();
  names.sort(Qt::CaseInsensitive);
  return names;
}

QVariantMap AppSettings::smartCollection(const QString& name) const {
  return m_smartCollections.value(name);
}

bool AppSettings::saveSmartCollection(const QString& name, const QVariantMap& view) {
  const QString normalized = normalizedAlbumName(name);
  if (normalized.isEmpty() || view.isEmpty()) {
    return false;
  }
  for (const QString& existing : m_smartCollections.keys()) {
    if (existing.compare(normalized, Qt::CaseInsensitive) == 0 && existing != normalized) {
      return false;
    }
  }
  m_smartCollections.insert(normalized, view);
  persistSmartCollections();
  emit smartCollectionsChanged();
  return m_organizationError.isEmpty();
}

void AppSettings::deleteSmartCollection(const QString& name) {
  if (m_smartCollections.remove(name) == 0) {
    return;
  }
  persistSmartCollections();
  emit smartCollectionsChanged();
}

bool AppSettings::reconcileCollectionMap(QMap<QString, QList<AlbumEntry>>& collections,
                                         const QList<CaptureRecord>& records) {
  if (collections.isEmpty()) return false;
  struct Candidate {
    QString path;
    qint64 bytes = 0;
    qint64 modified = 0;
    quint64 device = 0;
    quint64 inode = 0;
  };
  QHash<QString, int> byPath;
  QMultiHash<quint64, int> byInode;
  QMultiHash<qint64, int> bySize;
  QMultiHash<QPair<qint64, qint64>, int> bySizeAndTime;
  QList<Candidate> candidates;
  candidates.reserve(records.size());
  for (const CaptureRecord& record : records) {
    quint64 device = record.device;
    quint64 inode = record.inode;
    // The scanner fills these in off the GUI thread; a record built any other
    // way is looked up here.
    if (device == 0 && inode == 0) {
      struct stat status {};
      if (::stat(QFile::encodeName(record.path).constData(), &status) == 0) {
        device = status.st_dev;
        inode = status.st_ino;
      }
    }
    const int index = candidates.size();
    byPath.insert(record.path, index);
    if (inode != 0) byInode.insert(inode, index);
    bySize.insert(record.bytes, index);
    bySizeAndTime.insert({record.bytes, record.modified}, index);
    candidates.append({record.path, record.bytes, record.modified, device, inode});
  }

  bool persistedChange = false;
  bool resolutionChange = false;
  for (auto collection = collections.begin(); collection != collections.end(); ++collection) {
    for (AlbumEntry& entry : collection.value()) {
      const bool wasResolved = entry.resolved;
      entry.resolved = false;

      const bool livePath = byPath.contains(entry.path);
      if (livePath) {
        const QFileInfo info(entry.path);
        struct stat status {};
        if (info.isFile() && info.isReadable() &&
            ::stat(QFile::encodeName(entry.path).constData(), &status) == 0 &&
            entry.device == status.st_dev && entry.inode == status.st_ino &&
            entry.bytes == static_cast<qint64>(status.st_size) &&
            entry.modified == info.lastModified().toMSecsSinceEpoch()) {
          entry.resolved = true;
          resolutionChange = resolutionChange || !wasResolved;
          continue;
        }
        const AlbumEntry current = identityFor(entry.path);
        // Continuous same-path, same-device/inode edits keep membership.
        // A replacement or a previously unavailable entry must still match
        // content; an inode alone cannot establish that it is the old file.
        if (current.resolved &&
            (entry.fingerprint.isEmpty() || current.fingerprint == entry.fingerprint ||
             (wasResolved && entry.device != 0 && entry.inode != 0 &&
              current.device == entry.device && current.inode == entry.inode))) {
          persistedChange = persistedChange || entry.bytes != current.bytes ||
              entry.modified != current.modified || entry.fingerprint != current.fingerprint ||
              entry.device != current.device || entry.inode != current.inode;
          entry = current;
        }
      }

      // A missing scan row is not evidence of a move when the old path still
      // exists, or its containing directory is disconnected/unreadable.
      const bool mayRelocate = !livePath && relocationAvailable(entry);
      if (mayRelocate && !entry.fingerprint.isEmpty()) {
        QString match;
        int matches = 0;
        const auto sameInode = byInode.values(entry.inode);
        for (int index : sameInode) {
          const Candidate& candidate = candidates.at(index);
          if (entry.device != 0 && entry.inode != 0 && candidate.inode == entry.inode &&
              candidate.bytes == entry.bytes &&
              (candidate.device == entry.device || candidate.modified == entry.modified)) {
            const AlbumEntry current = identityFor(candidate.path);
            if (current.resolved && current.fingerprint == entry.fingerprint) {
              match = candidate.path;
              if (++matches > 1) break;
            }
          }
        }
        if (matches == 1) {
          const AlbumEntry current = identityFor(match);
          if (current.resolved && current.fingerprint == entry.fingerprint) {
            entry = current;
            persistedChange = true;
          }
        }
      }

      if (!entry.resolved && mayRelocate && !entry.fingerprint.isEmpty()) {
        QList<int> possible = bySizeAndTime.values({entry.bytes, entry.modified});
        if (possible.isEmpty()) possible = bySize.values(entry.bytes);
        // A pathological directory full of equally sized files should not
        // stall the UI to repair one uncertain album entry.
        if (possible.size() > 128) {
          resolutionChange = resolutionChange || wasResolved;
          continue;
        }
        QString matchedPath;
        int matches = 0;
        for (int index : std::as_const(possible)) {
          const Candidate& candidate = candidates.at(index);
          if (identityFor(candidate.path).fingerprint == entry.fingerprint) {
            matchedPath = candidate.path;
            ++matches;
            if (matches > 1) {
              break;
            }
          }
        }
        if (matches == 1) {
          const AlbumEntry repaired = identityFor(matchedPath);
          if (repaired.resolved && repaired.fingerprint == entry.fingerprint) {
            entry = repaired;
            persistedChange = true;
          }
        }
      }
      resolutionChange = resolutionChange || wasResolved != entry.resolved;
    }
  }
  return persistedChange || resolutionChange;
}

void AppSettings::reconcileAlbums(const QList<CaptureRecord>& records) {
  if (reconcileCollectionMap(m_albums, records)) {
    persistAlbums();
    emit albumsChanged();
  }
}

void AppSettings::reconcileTags(const QList<CaptureRecord>& records) {
  if (reconcileCollectionMap(m_tags, records)) {
    persistTags();
    emit tagsChanged();
  }
}

namespace {
constexpr int kDailyBackups = 7;
constexpr int kWeeklyBackups = 4;
constexpr int kBeforeRestoreBackups = 4;
constexpr auto kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

// Snapshots list the paths of organized files, so keep them private.
bool prepareBackupDirectory(const QString& path) {
  if (!QDir().mkpath(path)) return false;
  QFile::setPermissions(path, kOwnerOnly | QFileDevice::ExeOwner);
  return true;
}

bool writeBackupFile(const QString& path, const QByteArray& payload) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit())
    return false;
  QFile::setPermissions(path, kOwnerOnly);
  return true;
}

// daily-2026-11-08.json and weekly-2026-11-02.json carry their date.
QDate backupDate(const QString& name) {
  return QDate::fromString(name.mid(name.indexOf(QLatin1Char('-')) + 1, 10), Qt::ISODate);
}

// Times are UTC: local time takes glibc's time zone lock on the backup thread.
QDateTime dayStart(const QDate& date) { return QDateTime(date, QTime(0, 0), QTimeZone::UTC); }

QDateTime datedStamp(const QDir&, const QString& name) {
  const QDate date = backupDate(name);
  return date.isValid() ? dayStart(date) : QDateTime();
}

QDateTime modifiedStamp(const QDir& dir, const QString& name) {
  return QFileInfo(dir.filePath(name)).lastModified(QTimeZone::UTC);
}

// Newest first. A stamp after `now` comes from a clock that ran ahead; such
// files sort as the oldest, so they cannot push out a current snapshot.
QStringList newestFirst(const QDir& dir, const QString& pattern, const QDateTime& now,
                        const std::function<QDateTime(const QDir&, const QString&)>& stamp) {
  struct Entry {
    QString name;
    QDateTime at;
    bool current;
  };
  QList<Entry> entries;
  for (const QString& name : dir.entryList({pattern}, QDir::Files)) {
    const QDateTime at = stamp(dir, name);
    entries.append({name, at, at.isValid() && at <= now});
  }
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.current != b.current) return a.current;
    return a.at > b.at;
  });
  QStringList names;
  for (const Entry& entry : std::as_const(entries)) names.append(entry.name);
  return names;
}

// Keeps the file just written and the newest others, up to `keep` in all.
void pruneBackups(QDir dir, const QString& pattern, int keep, const QDateTime& now,
                  const QString& written,
                  const std::function<QDateTime(const QDir&, const QString&)>& stamp) {
  QStringList names = newestFirst(dir, pattern, now, stamp);
  if (names.removeAll(written) > 0) --keep;
  for (qsizetype i = qMax(0, keep); i < names.size(); ++i) dir.remove(names.at(i));
}

// Today's daily snapshot, and this week's from the first daily of the week.
bool writeScheduledBackups(const QString& path, const QDate& date, const QByteArray& payload,
                           bool replace) {
  if (!date.isValid() || !prepareBackupDirectory(path)) return false;
  const QDir dir(path);
  const QString daily = QStringLiteral("daily-%1.json").arg(date.toString(Qt::ISODate));
  if ((replace || !dir.exists(daily)) && !writeBackupFile(dir.filePath(daily), payload)) return false;
  const QDate monday = date.addDays(1 - date.dayOfWeek());
  const QString weekly = QStringLiteral("weekly-%1.json").arg(monday.toString(Qt::ISODate));
  if (!dir.exists(weekly)) {
    if (!QFile::copy(dir.filePath(daily), dir.filePath(weekly))) return false;
    QFile::setPermissions(dir.filePath(weekly), kOwnerOnly);
  }
  pruneBackups(dir, QStringLiteral("daily-*.json"), kDailyBackups, dayStart(date), daily, datedStamp);
  pruneBackups(dir, QStringLiteral("weekly-*.json"), kWeeklyBackups, dayStart(monday), weekly, datedStamp);
  return true;
}
} // namespace

QString AppSettings::backupDirectory() const {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
         + QStringLiteral("/omaroll/backups");
}

AppSettings::OrganizationState AppSettings::organizationState() const {
  return {m_albums, m_tags, m_smartCollections, m_favorites, m_hidden, m_ratings, m_captions,
          m_markIdentities};
}

void AppSettings::flushBackups() {
  m_backupWriter->flush();
  applyOrganizationHealth();
  // Applying it may have queued today's first snapshot.
  m_backupWriter->flush();
}

struct AppSettings::HealthReport {
  std::mutex mutex;
  bool done = false;
  bool applied = false;
  QString snapshot;
  QDate date;
  int snapshotSize = 0;
};

namespace {
// Albums and tags count once each and once per file in them, then every mark
// and saved view: a rough size to compare a load with a snapshot.
int snapshotSize(const QJsonObject& root) {
  qsizetype size = 0;
  for (const auto key : {QLatin1String("albums"), QLatin1String("tags")}) {
    const QJsonObject collections = root.value(key).toObject();
    for (auto it = collections.constBegin(); it != collections.constEnd(); ++it)
      size += 1 + it.value().toArray().size();
  }
  size += root.value(QLatin1String("favorites")).toArray().size();
  size += root.value(QLatin1String("hidden")).toArray().size();
  size += root.value(QLatin1String("ratings")).toObject().size();
  size += root.value(QLatin1String("captions")).toObject().size();
  size += root.value(QLatin1String("smartCollections")).toObject().size();
  return int(qMin<qsizetype>(size, std::numeric_limits<int>::max()));
}

// Empty, or under half of a snapshot that held at least three more items.
bool looksIncomplete(int loaded, int snapshot) {
  if (snapshot <= 0) return false;
  return loaded == 0 || (qint64(loaded) * 2 < snapshot && snapshot - loaded >= 3);
}
} // namespace

int AppSettings::organizationSize(const OrganizationState& state) {
  qsizetype size = 0;
  for (const auto* collections : {&state.albums, &state.tags})
    for (const auto& entries : *collections) size += 1 + entries.size();
  size += state.favorites.size() + state.hidden.size() + state.ratings.size() + state.captions.size() +
          state.smartCollections.size();
  return int(qMin<qsizetype>(size, std::numeric_limits<int>::max()));
}

void AppSettings::checkOrganizationHealth(bool existingProfile) {
  m_healthPending = true;
  m_snapshotAfterHealth = m_snapshotAfterHealth || existingProfile;
  m_health = std::make_shared<HealthReport>();
  const QDate today = QDate::currentDate();
  // Polled from the window thread rather than posted back, so the handoff is
  // the report's own mutex.
  m_healthPoll.start();
  m_backupWriter->post([report = m_health, path = backupDirectory(), today] {
    // The newest daily snapshot within the week the dailies are kept, so a
    // profile damaged after a few days away is still caught.
    for (int back = 0; back < 7; ++back) {
      const QDate day = today.addDays(-back);
      QFile file(QDir(path).filePath(QStringLiteral("daily-%1.json").arg(day.toString(Qt::ISODate))));
      if (file.size() > kMaximumOrganizationBytes || !file.open(QIODevice::ReadOnly)) continue;
      const QJsonDocument document = QJsonDocument::fromJson(file.read(kMaximumOrganizationBytes));
      const QJsonObject root = document.object();
      if (root.value(QLatin1String("format")).toString() != QLatin1String(kOrganizationFormat)) continue;
      std::lock_guard lock(report->mutex);
      report->snapshot = file.fileName();
      report->date = day;
      report->snapshotSize = snapshotSize(root);
      break;
    }
    std::lock_guard lock(report->mutex);
    report->done = true;
  });
}

void AppSettings::applyOrganizationHealth() {
  if (!m_health) return;
  QString snapshot;
  QDate date;
  int size = 0;
  {
    std::lock_guard lock(m_health->mutex);
    if (!m_health->done || m_health->applied) return;
    m_health->applied = true;
    m_healthPoll.stop();
    snapshot = m_health->snapshot;
    date = m_health->date;
    size = m_health->snapshotSize;
  }
  m_healthPending = false;
  const int loaded = organizationSize(organizationState());
  // Changes made since the launch only add to what loaded; a load that still
  // looks incomplete next to the snapshot is not snapshotted over it.
  if (settingsStatus() == QSettings::NoError && looksIncomplete(loaded, size)) {
    m_organizationSuspect = true;
    m_suspectSnapshot = snapshot;
    const QDate today = QDate::currentDate();
    const QString day = date == today ? QStringLiteral("today's backup")
                        : date == today.addDays(-1)
                            ? QStringLiteral("yesterday's backup")
                            : QStringLiteral("the backup from %1").arg(QLocale().toString(date, QStringLiteral("d MMMM")));
    m_launchWarning = QStringLiteral("Your albums, tags and marks look incomplete: %1 has %2 %3, and only %4 loaded. "
                                     "Restore the backup? What is here now is kept in a backup first.")
                          .arg(day)
                          .arg(size)
                          .arg(size == 1 ? QStringLiteral("item") : QStringLiteral("items"))
                          .arg(loaded);
    emit launchWarningChanged();
    emit organizationErrorChanged(); // canRestoreOrganization changed.
    return;
  }
  if (m_snapshotAfterHealth) scheduleDailySnapshot();
  if (m_snapshotDirty) startSnapshotRefresh();
}

void AppSettings::startSnapshotRefresh() {
  const qint64 left = m_snapshotDirtySince.isValid()
                          ? m_snapshotMaxWait - m_snapshotDirtySince.elapsed()
                          : m_snapshotMaxWait;
  m_snapshotRefresh.start(static_cast<int>(std::clamp<qint64>(left, 0, m_snapshotRefreshDelay)));
}

void AppSettings::noteOrganizationSaved() {
  if (!m_snapshotDirty) m_snapshotDirtySince.start();
  m_snapshotDirty = true;
  if (snapshotsHeld()) {
    m_snapshotAfterHealth = true;
    return;
  }
  // Today's first snapshot is taken now; later changes refresh it once they settle.
  if (m_snapshotDate != QDate::currentDate()) scheduleDailySnapshot();
  startSnapshotRefresh();
}

QString AppSettings::keepCurrentOrganization() {
  const bool suspect = m_organizationSuspect;
  if (suspect) {
    flushBackups();
    // The fuller snapshot stays available to Import and as the last fallback.
    // Without that copy, replacing today's snapshot would lose it, so nothing
    // changes and the warning stays.
    const QDir dir(backupDirectory());
    const QString name = QStringLiteral("before-restore-%1.json")
                             .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QFile::copy(m_suspectSnapshot, dir.filePath(name))) {
      QFile::remove(dir.filePath(name));
      return QStringLiteral("Could not keep a copy of the fuller backup, so nothing was changed. "
                            "Check the free space in %1.").arg(dir.path());
    }
    QFile::setPermissions(dir.filePath(name), kOwnerOnly);
    pruneBackups(dir, QStringLiteral("before-restore-*.json"), kBeforeRestoreBackups,
                 QDateTime::currentDateTimeUtc(), name, modifiedStamp);
    m_organizationSuspect = false;
    m_suspectSnapshot.clear();
    m_snapshotDirty = false;
    scheduleDailySnapshot(true);
  }
  if (!m_launchWarning.isEmpty()) {
    m_launchWarning.clear();
    emit launchWarningChanged();
  }
  if (suspect) emit organizationErrorChanged();
  return {};
}

void AppSettings::scheduleDailySnapshot(bool replace) {
  const QDate today = QDate::currentDate();
  if (settingsStatus() != QSettings::NoError || (!replace && m_snapshotDate == today)) return;
  // Retried on the next start if the write fails, not on every save.
  m_snapshotDate = today;
  // Encoding is quick; the directory work and the synced writes are not.
  m_backupWriter->post([path = backupDirectory(), today, payload = encodeOrganization(organizationState()), replace] {
    writeScheduledBackups(path, today, payload, replace);
  });
}

bool AppSettings::saveBeforeRestore(const OrganizationState& state) {
  flushBackups();
  const QString path = backupDirectory();
  if (!prepareBackupDirectory(path)) return false;
  const QString name = QStringLiteral("before-restore-%1.json")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  const QDir dir(path);
  if (!writeBackupFile(dir.filePath(name), encodeOrganization(state))) return false;
  pruneBackups(dir, QStringLiteral("before-restore-*.json"), kBeforeRestoreBackups,
               QDateTime::currentDateTimeUtc(), name, modifiedStamp);
  return true;
}

bool AppSettings::backupLegacySettings() {
  flushBackups();
  const QString path = backupDirectory();
  if (!prepareBackupDirectory(path)) return false;
  const QString copy = QDir(path).filePath(QStringLiteral("settings-before-resume-migration-%1.ini")
                                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (!QFile::copy(m_settings.fileName(), copy)) return false;
  QFile::setPermissions(copy, kOwnerOnly);
  return true;
}

bool AppSettings::snapshotOrganization(const QDate& date, bool beforeRestore) {
  flushBackups();
  if (settingsStatus() != QSettings::NoError) return false;
  if (beforeRestore) return saveBeforeRestore(organizationState());
  return writeScheduledBackups(backupDirectory(), date, encodeOrganization(organizationState()), false);
}

bool AppSettings::canRestoreOrganization() const {
  return (settingsStatus() == QSettings::FormatError || m_organizationSuspect) &&
         !QDir(backupDirectory()).entryList({QStringLiteral("daily-*.json"),
                 QStringLiteral("weekly-*.json"), QStringLiteral("before-restore-*.json")}, QDir::Files).isEmpty();
}

QVariantMap AppSettings::restoreLastGoodBackup() {
  flushBackups();
  const QDir dir(backupDirectory());
  // Newest date first, and a day's own snapshot before its week's copy. Dates
  // after today come from a clock that ran ahead and go last.
  const QDate today = QDate::currentDate();
  struct Candidate {
    QString name;
    QDate date;
    bool daily;
    bool current;
  };
  QList<Candidate> candidates;
  for (const QString& name : dir.entryList({QStringLiteral("daily-*.json"), QStringLiteral("weekly-*.json")}, QDir::Files)) {
    const QDate date = backupDate(name);
    candidates.append({name, date, name.startsWith(QLatin1String("daily-")), date.isValid() && date <= today});
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.current != b.current) return a.current;
    if (a.date != b.date) return a.date > b.date;
    return a.daily && !b.daily;
  });
  QStringList scheduled;
  for (const Candidate& candidate : std::as_const(candidates)) scheduled.append(candidate.name);
  const QStringList fallback = newestFirst(dir, QStringLiteral("before-restore-*.json"),
                                           QDateTime::currentDateTimeUtc(), modifiedStamp);
  for (const QString& name : scheduled + fallback) {
    QVariantMap result = importOrganization(dir.filePath(name));
    // An older snapshot cannot help when the restore itself could not be saved.
    if (result.value(QStringLiteral("saveFailed")).toBool()) return result;
    if (!result.value(QStringLiteral("ok")).toBool()) continue;
    // A daily snapshot is named for its day. A weekly copy is named for its
    // Monday, so its own export time says when it was taken.
    const QDateTime taken = QDateTime::fromString(result.value(QStringLiteral("exportedAt")).toString(), Qt::ISODate);
    const QDate day = name.startsWith(QLatin1String("daily-")) || !taken.isValid()
        ? backupDate(name) : taken.toLocalTime().date();
    if (day.isValid()) {
      result.insert(QStringLiteral("backupDate"), day);
      result.insert(QStringLiteral("message"), result.value(QStringLiteral("message")).toString()
                        + QStringLiteral(" from the backup of ")
                        + QLocale().toString(day, QStringLiteral("d MMMM yyyy")));
    }
    return result;
  }
  return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), QStringLiteral("No readable organization backup was found")}};
}

QByteArray AppSettings::encodeOrganization(const OrganizationState& state) {
  const auto encodeIdentity = [](const AlbumEntry& entry) {
    QJsonObject row;
    row.insert(QStringLiteral("bytes"), entry.bytes);
    row.insert(QStringLiteral("modified"), entry.modified);
    // SHA-256 is binary; base64 keeps it exact through JSON.
    row.insert(QStringLiteral("fingerprint"), QString::fromLatin1(entry.fingerprint.toBase64()));
    row.insert(QStringLiteral("device"), QString::number(entry.device));
    row.insert(QStringLiteral("inode"), QString::number(entry.inode));
    return row;
  };
  const auto encodeEntries = [&encodeIdentity](const QMap<QString, QList<AlbumEntry>>& collections) {
    QJsonObject out;
    for (auto it = collections.cbegin(); it != collections.cend(); ++it) {
      QJsonArray rows;
      for (const AlbumEntry& entry : it.value()) {
        QJsonObject row = encodeIdentity(entry);
        row.insert(QStringLiteral("path"), entry.path);
        rows.append(row);
      }
      out.insert(it.key(), rows);
    }
    return out;
  };

  QJsonObject root;
  root.insert(QStringLiteral("format"), QString::fromLatin1(kOrganizationFormat));
  root.insert(QStringLiteral("version"), kOrganizationVersion);
  root.insert(QStringLiteral("exportedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  root.insert(QStringLiteral("albums"), encodeEntries(state.albums));
  root.insert(QStringLiteral("tags"), encodeEntries(state.tags));

  QStringList favorites(state.favorites.cbegin(), state.favorites.cend());
  favorites.sort();
  QJsonArray favoriteArray;
  for (const QString& value : std::as_const(favorites)) {
    favoriteArray.append(value);
  }
  root.insert(QStringLiteral("favorites"), favoriteArray);

  QStringList hidden(state.hidden.cbegin(), state.hidden.cend());
  hidden.sort();
  QJsonArray hiddenArray;
  for (const QString& value : std::as_const(hidden)) {
    hiddenArray.append(value);
  }
  root.insert(QStringLiteral("hidden"), hiddenArray);

  QJsonObject ratings;
  for (auto it = state.ratings.cbegin(); it != state.ratings.cend(); ++it) {
    ratings.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("ratings"), ratings);

  QJsonObject captions;
  for (auto it = state.captions.cbegin(); it != state.captions.cend(); ++it) {
    captions.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("captions"), captions);

  QJsonObject markIdentities;
  for (auto it = state.markIdentities.cbegin(); it != state.markIdentities.cend(); ++it) {
    const QString& path = it.key();
    const bool marked = state.favorites.contains(path) || state.hidden.contains(path) ||
                        state.ratings.contains(path) || state.captions.contains(path);
    if (marked && it.value().bytes >= 0) {
      markIdentities.insert(it.key(), encodeIdentity(it.value()));
    }
  }
  root.insert(QStringLiteral("markIdentities"), markIdentities);

  QVariantMap smart;
  for (auto it = state.smartCollections.cbegin(); it != state.smartCollections.cend(); ++it) {
    smart.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("smartCollections"), QJsonObject::fromVariantMap(smart));

  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QVariantMap AppSettings::exportOrganization(const QString& path) const {
  if (path.isEmpty()) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("Choose a file to write the backup to")}};
  }
  const QByteArray payload = encodeOrganization(organizationState());
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"),
             QStringLiteral("Could not write %1").arg(QFileInfo(path).fileName())}};
  }
  if (file.write(payload) != payload.size() || !file.commit()) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"),
             QStringLiteral("Could not write %1").arg(QFileInfo(path).fileName())}};
  }

  const int marks = m_favorites.size() + m_hidden.size() + m_ratings.size() + m_captions.size();
  QVariantMap result;
  result.insert(QStringLiteral("ok"), true);
  result.insert(QStringLiteral("message"),
                QStringLiteral("Backed up %1 album%2, %3 tag%4 and %5 item%6")
                    .arg(m_albums.size())
                    .arg(m_albums.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(m_tags.size())
                    .arg(m_tags.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(marks)
                    .arg(marks == 1 ? QString() : QStringLiteral("s")));
  result.insert(QStringLiteral("albums"), m_albums.size());
  result.insert(QStringLiteral("tags"), m_tags.size());
  result.insert(QStringLiteral("marks"), marks);
  return result;
}

QVariantMap AppSettings::importOrganization(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"),
             QStringLiteral("Could not open %1").arg(QFileInfo(path).fileName())}};
  }
  const auto invalidBackup = []() -> QVariantMap {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("This backup contains invalid organization")}};
  };
  if (file.size() > kMaximumOrganizationBytes) return invalidBackup();
  const QByteArray payload = file.read(kMaximumOrganizationBytes + 1);
  if (payload.size() > kMaximumOrganizationBytes || file.error() != QFile::NoError)
    return invalidBackup();
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("That is not a valid backup file")}};
  }
  const QJsonObject root = document.object();
  const QString exportedAt = root.value(QStringLiteral("exportedAt")).toString();
  if (root.value(QStringLiteral("format")).toString() != QLatin1String(kOrganizationFormat)) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("That is not an Omaroll backup")}};
  }
  const int version = root.value(QStringLiteral("version")).toInt();
  if (version <= 0 || version > kOrganizationVersion) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"),
             QStringLiteral("This backup needs a newer version of Omaroll")}};
  }

  // Every required member must be present and the right shape. A header-only
  // or partly damaged file is refused outright rather than read as empty
  // collections, which would wipe the profile and still report success.
  const auto memberIs = [&root](const char* key, QJsonValue::Type type) {
    return root.value(QString::fromLatin1(key)).type() == type;
  };
  if (!memberIs("albums", QJsonValue::Object) || !memberIs("tags", QJsonValue::Object) ||
      !memberIs("favorites", QJsonValue::Array) || !memberIs("hidden", QJsonValue::Array) ||
      !memberIs("ratings", QJsonValue::Object) || !memberIs("captions", QJsonValue::Object) ||
      !memberIs("smartCollections", QJsonValue::Object)) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("This backup is incomplete")}};
  }

  // Decode into temporaries. Nothing here mutates state, so a malformed file
  // cannot leave a half-restored profile behind. Keys are normalized the way
  // startup normalizes them, so an imported name survives a restart.
  const auto decodeInteger = [](const QJsonValue& value, qint64& decoded) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    const double minimum = static_cast<double>(std::numeric_limits<qint64>::min());
    if (!(number >= minimum && number < -minimum)) return false;
    decoded = static_cast<qint64>(number);
    return static_cast<double>(decoded) == number;
  };
  const auto decodeIdentity = [&decodeInteger](const QJsonObject& row, AlbumEntry& entry) {
    if (!decodeInteger(row.value(QStringLiteral("bytes")), entry.bytes) || entry.bytes < -1 ||
        !decodeInteger(row.value(QStringLiteral("modified")), entry.modified) ||
        !row.value(QStringLiteral("fingerprint")).isString() ||
        !row.value(QStringLiteral("device")).isString() ||
        !row.value(QStringLiteral("inode")).isString()) return false;
    const auto fingerprint = QByteArray::fromBase64Encoding(
        row.value(QStringLiteral("fingerprint")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    bool deviceOkay = false;
    bool inodeOkay = false;
    entry.device = row.value(QStringLiteral("device")).toString().toULongLong(&deviceOkay);
    entry.inode = row.value(QStringLiteral("inode")).toString().toULongLong(&inodeOkay);
    if (!fingerprint || (!fingerprint.decoded.isEmpty() && fingerprint.decoded.size() != 32) ||
        !deviceOkay || !inodeOkay) return false;
    entry.fingerprint = fingerprint.decoded;
    return true;
  };
  const auto decodeEntries = [&decodeIdentity](const QJsonObject& collections,
                                QString (*normalize)(const QString&),
                                QMap<QString, QList<AlbumEntry>>& decoded) {
    for (auto it = collections.begin(); it != collections.end(); ++it) {
      if (!it.value().isArray()) return false;
      const QString name = normalize(it.key());
      QList<AlbumEntry> entries;
      for (const QJsonValue& value : it.value().toArray()) {
        if (!value.isObject()) return false;
        const QJsonObject row = value.toObject();
        AlbumEntry entry;
        if (!row.value(QStringLiteral("path")).isString() ||
            row.value(QStringLiteral("path")).toString().isEmpty() ||
            !decodeIdentity(row, entry)) return false;
        entry.path = row.value(QStringLiteral("path")).toString();
        entries.append(entry);
      }
      // Preserve the existing name normalization, after validating its rows.
      if (!name.isEmpty()) decoded[name].append(entries);
    }
    return true;
  };
  QMap<QString, QList<AlbumEntry>> albums;
  QMap<QString, QList<AlbumEntry>> tags;
  if (!decodeEntries(root.value(QStringLiteral("albums")).toObject(), normalizedAlbumName, albums) ||
      !decodeEntries(root.value(QStringLiteral("tags")).toObject(), normalizedTagName, tags))
    return invalidBackup();
  // createTag always fills in a tag's ancestors; an imported tree keeps the
  // same invariant, or a child would be unreachable in Browse.
  const QStringList importedTagNames = tags.keys();
  for (const QString& name : importedTagNames) {
    const QStringList segments = name.split(QLatin1Char('/'));
    for (qsizetype depth = 1; depth < segments.size(); ++depth) {
      const QString ancestor = segments.first(depth).join(QLatin1Char('/'));
      if (!tags.contains(ancestor)) {
        tags.insert(ancestor, {});
      }
    }
  }

  QSet<QString> favorites;
  for (const QJsonValue& value : root.value(QStringLiteral("favorites")).toArray()) {
    if (!value.isString() || value.toString().isEmpty()) return invalidBackup();
    favorites.insert(value.toString());
  }
  QSet<QString> hidden;
  for (const QJsonValue& value : root.value(QStringLiteral("hidden")).toArray()) {
    if (!value.isString() || value.toString().isEmpty()) return invalidBackup();
    hidden.insert(value.toString());
  }
  QHash<QString, int> ratings;
  const QJsonObject ratingObject = root.value(QStringLiteral("ratings")).toObject();
  for (auto it = ratingObject.begin(); it != ratingObject.end(); ++it) {
    qint64 decodedStars = 0;
    if (it.key().isEmpty() || !decodeInteger(it.value(), decodedStars) ||
        decodedStars < 1 || decodedStars > kMaximumRating) return invalidBackup();
    const int stars = static_cast<int>(decodedStars);
    ratings.insert(it.key(), stars);
  }
  QHash<QString, QString> captions;
  const QJsonObject captionObject = root.value(QStringLiteral("captions")).toObject();
  for (auto it = captionObject.begin(); it != captionObject.end(); ++it) {
    if (it.key().isEmpty() || !it.value().isString() || it.value().toString().isEmpty())
      return invalidBackup();
    const QString text = it.value().toString().left(kMaximumCaptionLength);
    captions.insert(it.key(), text);
  }
  QMap<QString, QVariantMap> smartCollections;
  const QJsonObject smartObject = root.value(QStringLiteral("smartCollections")).toObject();
  for (auto it = smartObject.begin(); it != smartObject.end(); ++it) {
    if (!it.value().isObject() || it.value().toObject().isEmpty()) return invalidBackup();
    const QJsonObject view = it.value().toObject();
    for (auto field = view.begin(); field != view.end(); ++field) {
      const QString& key = field.key();
      const QJsonValue value = field.value();
      if (QStringList{QStringLiteral("favorites"), QStringLiteral("showHidden")}.contains(key)) {
        if (!value.isBool()) return invalidBackup();
      } else if (QStringList{QStringLiteral("kind"), QStringLiteral("dateField"),
                            QStringLiteral("minRating"), QStringLiteral("sort")}.contains(key)) {
        qint64 number = 0;
        const qint64 minimum = key == QLatin1String("kind") ? -1 : 0;
        const qint64 maximum = key == QLatin1String("dateField") ? 1 : 5;
        if (!decodeInteger(value, number) || number < minimum || number > maximum)
          return invalidBackup();
      } else if (!value.isString()) {
        return invalidBackup();
      }
    }
    const QString name = normalizedAlbumName(it.key());
    if (!name.isEmpty()) smartCollections.insert(name, it.value().toObject().toVariantMap());
  }

  // Optional in version 1 for backups written before mark move recovery was
  // exported. Reject malformed identities before replacing any organization.
  QHash<QString, AlbumEntry> markIdentities;
  if (root.contains(QStringLiteral("markIdentities"))) {
    const auto invalidIdentities = []() -> QVariantMap {
      return {{QStringLiteral("ok"), false},
              {QStringLiteral("message"), QStringLiteral("This backup has invalid mark identities")}};
    };
    if (!memberIs("markIdentities", QJsonValue::Object)) {
      return invalidIdentities();
    }
    const QJsonObject identities = root.value(QStringLiteral("markIdentities")).toObject();
    for (auto it = identities.begin(); it != identities.end(); ++it) {
      if (it.key().isEmpty() || !it.value().isObject() ||
          !(favorites.contains(it.key()) || hidden.contains(it.key()) ||
            ratings.contains(it.key()) || captions.contains(it.key()))) {
        return invalidIdentities();
      }
      const QJsonObject row = it.value().toObject();
      AlbumEntry entry;
      entry.path = it.key();
      if (!decodeIdentity(row, entry) || entry.bytes < 0) return invalidIdentities();
      markIdentities.insert(entry.path, entry);
    }
  }

  // An entry whose file is present right now shows up immediately; the next
  // scan repairs anything that moved. The stored identity is kept so a move
  // can still be matched by inode or fingerprint.
  const auto resolvePresent = [](QMap<QString, QList<AlbumEntry>>& collections) {
    for (auto it = collections.begin(); it != collections.end(); ++it) {
      for (AlbumEntry& entry : *it) {
        const QFileInfo info(entry.path);
        const AlbumEntry current = identityFor(entry.path);
        entry.resolved = info.isFile() && info.isReadable() && current.resolved &&
                         (entry.fingerprint.isEmpty() || current.fingerprint == entry.fingerprint);
      }
    }
  };
  resolvePresent(albums);
  resolvePresent(tags);

  // The backup was valid; saving it failed.
  const auto failed = [](const QString& message) -> QVariantMap {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), message},
            {QStringLiteral("saveFailed"), true}};
  };
  // QSettings' format error is sticky, so a damaged file is repaired by
  // rebuilding it: the new file is written beside it and replaces it only once
  // it is complete, so a full disk leaves the damaged file as it was.
  const QString damagedIni = settingsStatus() == QSettings::FormatError ? m_settings.fileName() : QString();
  const auto reopen = [this, &damagedIni](const QString& path) {
    std::destroy_at(&m_settings);
    std::construct_at(&m_settings, path, QSettings::IniFormat);
    m_damaged = path == damagedIni;
  };
  if (!damagedIni.isEmpty()) {
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QFile::copy(damagedIni, damagedIni + QStringLiteral(".damaged-") + uuid))
      return failed(QStringLiteral("Could not preserve damaged settings"));
  }
  // Keep the organization being replaced, including whatever could still be
  // read from a damaged file.
  if (!saveBeforeRestore(organizationState()))
    return failed(QStringLiteral("Could not save a backup before restoring"));
  if (!damagedIni.isEmpty()) {
    // Keep whatever preferences and folders could still be read; the
    // organization keys are replaced by the import below.
    QVariantMap readable;
    for (const QString& key : m_settings.allKeys()) readable.insert(key, m_settings.value(key));
    reopen(damagedIni + QStringLiteral(".restore-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    for (auto it = readable.cbegin(); it != readable.cend(); ++it) m_settings.setValue(it.key(), it.value());
  }
  // Replaces the damaged file with the complete rebuilt one, in one rename.
  const auto commitRepair = [&]() {
    if (damagedIni.isEmpty()) return true;
    const QString rebuilt = m_settings.fileName();
    std::destroy_at(&m_settings);
    const bool replaced = std::rename(QFile::encodeName(rebuilt).constData(),
                                      QFile::encodeName(damagedIni).constData()) == 0;
    if (!replaced) QFile::remove(rebuilt);
    std::construct_at(&m_settings, damagedIni, QSettings::IniFormat);
    m_damaged = !replaced;
    return replaced;
  };

  const auto oldAlbums = m_albums;
  const auto oldTags = m_tags;
  const auto oldViews = m_smartCollections;
  const MarksSnapshot oldMarks{m_favorites, m_hidden, m_ratings, m_captions, m_markIdentities};
  m_albums = std::move(albums);
  m_tags = std::move(tags);
  m_smartCollections = std::move(smartCollections);
  m_favorites = std::move(favorites);
  m_hidden = std::move(hidden);
  m_ratings = std::move(ratings);
  m_captions = std::move(captions);
  m_markIdentities = std::move(markIdentities);

  m_restoring = true;
  const auto restoring = qScopeGuard([this] { m_restoring = false; });
  m_deferOrganizationSync = true;
  persistAlbums();
  persistTags();
  persistSmartCollections();
  persistMarks();
  m_deferOrganizationSync = false;
  const bool saved = syncOrganization();
  if (!saved || !commitRepair()) {
    // A rebuilt file that did not take its place is discarded; the damaged
    // one stays, still in place, for the next try.
    if (!damagedIni.isEmpty() && m_settings.fileName() != damagedIni) {
      const QString rebuilt = m_settings.fileName();
      reopen(damagedIni);
      QFile::remove(rebuilt);
    }
    const QString error = saved ? QStringLiteral("Could not replace the damaged settings file") : m_organizationError;
    m_albums = oldAlbums;
    m_tags = oldTags;
    m_smartCollections = oldViews;
    m_favorites = oldMarks.favorites;
    m_hidden = oldMarks.hidden;
    m_ratings = oldMarks.ratings;
    m_captions = oldMarks.captions;
    m_markIdentities = oldMarks.identities;
    // Replace pending failed values too, so a later flush cannot restore them.
    m_deferOrganizationSync = true;
    persistAlbums();
    persistTags();
    persistSmartCollections();
    persistMarks();
    m_deferOrganizationSync = false;
    syncOrganization();
    return failed(error);
  }
  const bool wasSuspect = m_organizationSuspect;
  m_organizationSuspect = false;
  m_suspectSnapshot.clear();
  if (!damagedIni.isEmpty() || wasSuspect) emit organizationErrorChanged(); // canRestoreOrganization changed.
  if (!m_launchWarning.isEmpty()) {
    m_launchWarning.clear();
    emit launchWarningChanged();
  }
  // Today's snapshot now holds the import, which the user chose; the state it
  // replaced is in the before-restore copy.
  m_snapshotDirty = false;
  m_snapshotRefresh.stop();
  scheduleDailySnapshot(true);
  clearMarksUndo();
  emit albumsChanged();
  emit tagsChanged();
  emit smartCollectionsChanged();
  emit marksChanged();

  const int marks = m_favorites.size() + m_hidden.size() + m_ratings.size() + m_captions.size();
  QVariantMap result;
  result.insert(QStringLiteral("ok"), true);
  result.insert(QStringLiteral("message"),
                QStringLiteral("Restored %1 album%2, %3 tag%4 and %5 item%6")
                    .arg(m_albums.size())
                    .arg(m_albums.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(m_tags.size())
                    .arg(m_tags.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(marks)
                    .arg(marks == 1 ? QString() : QStringLiteral("s")));
  result.insert(QStringLiteral("albums"), m_albums.size());
  result.insert(QStringLiteral("tags"), m_tags.size());
  result.insert(QStringLiteral("marks"), marks);
  result.insert(QStringLiteral("exportedAt"), exportedAt);
  return result;
}

void AppSettings::persistAlbums() {
  QVariantMap albums;
  for (auto album = m_albums.cbegin(); album != m_albums.cend(); ++album) {
    QVariantList entries;
    for (const AlbumEntry& entry : album.value()) {
      QVariantMap stored;
      stored.insert(QStringLiteral("path"), entry.path);
      stored.insert(QStringLiteral("bytes"), entry.bytes);
      stored.insert(QStringLiteral("modified"), entry.modified);
      stored.insert(QStringLiteral("fingerprint"), entry.fingerprint);
      stored.insert(QStringLiteral("device"), QString::number(entry.device));
      stored.insert(QStringLiteral("inode"), QString::number(entry.inode));
      entries.append(stored);
    }
    albums.insert(album.key(), entries);
  }
  setValue(kAlbums, albums);
  syncOrganization();
}

void AppSettings::persistTags() {
  QVariantMap tags;
  for (auto tag = m_tags.cbegin(); tag != m_tags.cend(); ++tag) {
    QVariantList entries;
    for (const AlbumEntry& entry : tag.value()) {
      QVariantMap stored;
      stored.insert(QStringLiteral("path"), entry.path);
      stored.insert(QStringLiteral("bytes"), entry.bytes);
      stored.insert(QStringLiteral("modified"), entry.modified);
      stored.insert(QStringLiteral("fingerprint"), entry.fingerprint);
      stored.insert(QStringLiteral("device"), QString::number(entry.device));
      stored.insert(QStringLiteral("inode"), QString::number(entry.inode));
      entries.append(stored);
    }
    tags.insert(tag.key(), entries);
  }
  setValue(kTags, tags);
  syncOrganization();
}

void AppSettings::persistSmartCollections() {
  QVariantMap stored;
  for (auto it = m_smartCollections.cbegin(); it != m_smartCollections.cend(); ++it) {
    stored.insert(it.key(), it.value());
  }
  setValue(kSmartCollections, stored);
  syncOrganization();
}

bool AppSettings::isFavorite(const QString& path) const { return m_favorites.contains(path); }
bool AppSettings::isHidden(const QString& path) const { return m_hidden.contains(path); }

void AppSettings::toggleFavorite(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  pushMarksUndo();
  if (!m_favorites.remove(path)) {
    m_favorites.insert(path);
  }
  refreshMarkIdentity(path);
  persistMarks();
  emit marksChanged();
}

int AppSettings::rating(const QString& path) const { return m_ratings.value(path, 0); }

void AppSettings::setRating(const QStringList& paths, int rating) {
  const int stars = qBound(0, rating, kMaximumRating);
  pushMarksUndo();
  bool changed = false;
  for (const QString& path : paths) {
    if (path.isEmpty()) {
      continue;
    }
    if (stars == 0) {
      changed = m_ratings.remove(path) > 0 || changed;
    } else if (m_ratings.value(path, 0) != stars) {
      m_ratings.insert(path, stars);
      changed = true;
    }
  }
  if (!changed) {
    return;
  }
  for (const QString& path : paths) {
    refreshMarkIdentity(path);
  }
  persistMarks();
  emit marksChanged();
}

QString AppSettings::caption(const QString& path) const { return m_captions.value(path); }

void AppSettings::setCaption(const QString& path, const QString& text) {
  if (path.isEmpty()) {
    return;
  }
  const QString clean = text.trimmed().left(kMaximumCaptionLength);
  if (m_captions.value(path) == clean) {
    return;
  }
  pushMarksUndo();
  if (clean.isEmpty()) {
    m_captions.remove(path);
  } else {
    m_captions.insert(path, clean);
  }
  refreshMarkIdentity(path);
  persistMarks();
  emit marksChanged();
}

void AppSettings::toggleHidden(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  pushMarksUndo();
  if (!m_hidden.remove(path)) {
    m_hidden.insert(path);
  }
  refreshMarkIdentity(path);
  persistMarks();
  emit marksChanged();
}

namespace {
void mark(QSet<QString>& marks, const QStringList& paths, bool on) {
  for (const QString& path : paths) {
    if (path.isEmpty()) {
      continue;
    }
    if (on) {
      marks.insert(path);
    } else {
      marks.remove(path);
    }
  }
}
} // namespace

void AppSettings::setFavorite(const QStringList& paths, bool on) {
  pushMarksUndo();
  mark(m_favorites, paths, on);
  for (const QString& path : paths) {
    refreshMarkIdentity(path);
  }
  persistMarks();
  emit marksChanged();
}

void AppSettings::setHidden(const QStringList& paths, bool on) {
  pushMarksUndo();
  mark(m_hidden, paths, on);
  for (const QString& path : paths) {
    refreshMarkIdentity(path);
  }
  persistMarks();
  emit marksChanged();
}

QStringList AppSettings::markedPaths() const {
  QSet<QString> all = m_favorites;
  all.unite(m_hidden);
  for (auto it = m_ratings.cbegin(); it != m_ratings.cend(); ++it) {
    all.insert(it.key());
  }
  for (auto it = m_captions.cbegin(); it != m_captions.cend(); ++it) {
    all.insert(it.key());
  }
  return QStringList(all.begin(), all.end());
}

void AppSettings::persistMarks() {
  const bool deferred = m_deferOrganizationSync;
  m_deferOrganizationSync = true;
  persistMarkIdentities();
  setValue(kFavorites, QStringList(m_favorites.begin(), m_favorites.end()));
  setValue(kHidden, QStringList(m_hidden.begin(), m_hidden.end()));
  QStringList ratings;
  ratings.reserve(m_ratings.size());
  for (auto it = m_ratings.cbegin(); it != m_ratings.cend(); ++it) {
    ratings.append(QString::number(it.value()) + QLatin1Char(':') + it.key());
  }
  ratings.sort();
  setValue(kRatings, ratings);
  QVariantMap captions;
  for (auto it = m_captions.cbegin(); it != m_captions.cend(); ++it) {
    captions.insert(it.key(), it.value());
  }
  setValue(kCaptions, captions);
  m_deferOrganizationSync = deferred;
  syncOrganization();
}

bool AppSettings::pathHasMark(const QString& path) const {
  return m_favorites.contains(path) || m_hidden.contains(path) || m_ratings.contains(path) ||
         m_captions.contains(path);
}

void AppSettings::persistMarkIdentities() {
  QVariantMap stored;
  for (auto it = m_markIdentities.cbegin(); it != m_markIdentities.cend(); ++it) {
    const AlbumEntry& entry = it.value();
    QVariantMap row;
    row.insert(QStringLiteral("bytes"), entry.bytes);
    row.insert(QStringLiteral("modified"), entry.modified);
    row.insert(QStringLiteral("fingerprint"), entry.fingerprint);
    row.insert(QStringLiteral("device"), QString::number(entry.device));
    row.insert(QStringLiteral("inode"), QString::number(entry.inode));
    stored.insert(it.key(), row);
  }
  setValue(kMarkIdentities, stored);
  syncOrganization();
}

void AppSettings::setValue(const char* key, const QVariant& value) {
  // A damaged ini must remain available for recovery, not be overwritten
  // with whichever subset QSettings managed to read.
  if (settingsStatus() == QSettings::FormatError) {
    syncOrganization();
    return;
  }
  m_settings.setValue(key, value);
}

bool AppSettings::syncOrganization() {
  if (m_deferOrganizationSync) return true;
  m_settings.sync();
  const QString error = settingsStatus() == QSettings::NoError ? QString()
      : settingsStatus() == QSettings::FormatError
          ? QStringLiteral("Your settings file is damaged. Restore the last good backup in Settings, or keep a copy and repair it before saving organization.")
          : QStringLiteral("Could not save organization. Check that your settings folder is writable and has free space.");
  if (m_organizationError != error) {
    m_organizationError = error;
    emit organizationErrorChanged();
  }
  // Written on a worker. An import snapshots its own result.
  if (error.isEmpty() && !m_restoring) noteOrganizationSaved();
  return error.isEmpty();
}

void AppSettings::clearMarksUndo() {
  // A restored backup or a relocation makes old marks snapshots wrong. Moves
  // to Trash are about files on disk and stay undoable.
  const qsizetype removed = m_undoStack.removeIf([](const UndoStep& step) { return !step.isTrash(); });
  if (removed > 0) emit undoChanged();
}

void AppSettings::pushUndoStep(UndoStep step) {
  step.id = m_nextUndoId++;
  if (m_nextUndoId <= 0) m_nextUndoId = 1;
  m_undoStack.append(std::move(step));
  constexpr int kMaximumUndo = 64;
  while (m_undoStack.size() > kMaximumUndo) {
    m_undoStack.removeFirst();
  }
  emit undoChanged();
}

void AppSettings::pushMarksUndo() {
  if (m_undoing) {
    return;
  }
  UndoStep step;
  step.marks.favorites = m_favorites;
  step.marks.hidden = m_hidden;
  step.marks.ratings = m_ratings;
  step.marks.captions = m_captions;
  step.marks.identities = m_markIdentities;
  pushUndoStep(std::move(step));
}

int AppSettings::pushTrashUndo(const QVariantList& records, QObject* origin) {
  UndoStep step;
  for (const QVariant& value : records) {
    const QVariantMap record = value.toMap();
    if (!record.value(QStringLiteral("trashPath")).toString().isEmpty()) step.trashed.append(record);
  }
  if (step.trashed.isEmpty()) return 0;
  step.origin = origin;
  pushUndoStep(std::move(step));
  return m_undoStack.constLast().id;
}

QString AppSettings::undo() {
  if (m_undoStack.isEmpty()) {
    return {};
  }
  UndoStep step = m_undoStack.takeLast();
  if (step.isTrash()) {
    QVariantList restored;
    QVariantList retry;
    QStringList problems;
    for (const QVariant& value : std::as_const(step.trashed)) {
      const TrashRestore::Result result = TrashRestore::restore(value.toMap());
      if (result.ok) {
        restored.append(value);
      } else {
        problems.append(result.message);
        if (result.retryable) retry.append(value);
      }
    }
    // Files refused for a reason the user can fix stay undoable, as the
    // newest step, so Ctrl+Z can try them again.
    if (!retry.isEmpty()) {
      UndoStep again;
      again.trashed = retry;
      again.origin = step.origin;
      pushUndoStep(std::move(again));
    } else {
      emit undoChanged();
    }
    if (!restored.isEmpty()) emit trashRestored(restored, step.origin.data());
    const qsizetype total = step.trashed.size();
    if (problems.isEmpty()) {
      return total == 1 ? QStringLiteral("Restored %1").arg(QFileInfo(
                              restored.first().toMap().value(QStringLiteral("path")).toString()).fileName())
                        : QStringLiteral("Restored %1 items").arg(total);
    }
    if (total == 1) return problems.first();
    if (restored.isEmpty()) {
      return QStringLiteral("Could not restore %1 items. %2").arg(total).arg(problems.first());
    }
    return QStringLiteral("Restored %1 of %2 items. %3")
        .arg(restored.size()).arg(total).arg(problems.first());
  }
  MarksSnapshot snapshot = std::move(step.marks);
  m_undoing = true;
  m_favorites = std::move(snapshot.favorites);
  m_hidden = std::move(snapshot.hidden);
  m_ratings = std::move(snapshot.ratings);
  m_captions = std::move(snapshot.captions);
  m_markIdentities = std::move(snapshot.identities);
  persistMarks();
  emit marksChanged();
  m_undoing = false;
  emit undoChanged();
  return QStringLiteral("Undone");
}

void AppSettings::refreshMarkIdentity(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  if (!pathHasMark(path)) {
    m_markIdentities.remove(path);
    return;
  }
  if (m_markIdentities.contains(path)) {
    return;
  }
  const AlbumEntry identity = identityFor(path);
  if (identity.resolved) {
    m_markIdentities.insert(path, identity);
  }
}

void AppSettings::reconcileMarks(const QList<CaptureRecord>& records) {
  // Marks saved before move recovery existed have no stored identity yet.
  // Record one for every live marked file, so a later move can be recovered.
  bool backfilled = false;
  for (const CaptureRecord& record : records) {
    if (pathHasMark(record.path) && !m_markIdentities.contains(record.path)) {
      const AlbumEntry identity = identityFor(record.path);
      if (identity.resolved) {
        m_markIdentities.insert(record.path, identity);
        backfilled = true;
      }
    }
  }
  if (backfilled) {
    persistMarkIdentities();
  }
  if (m_markIdentities.isEmpty()) {
    return;
  }

  struct Candidate {
    QString path;
    qint64 bytes = 0;
    qint64 modified = 0;
    quint64 device = 0;
    quint64 inode = 0;
  };
  QList<Candidate> candidates;
  candidates.reserve(records.size());
  QHash<QPair<quint64, qint64>, QList<qsizetype>> byInodeAndSize;
  QHash<QPair<qint64, qint64>, QList<qsizetype>> bySizeAndModified;
  QHash<qint64, QList<qsizetype>> bySize;
  QSet<QString> live;
  live.reserve(records.size());
  for (const CaptureRecord& record : records) {
    quint64 device = record.device;
    quint64 inode = record.inode;
    if (device == 0 && inode == 0) {
      struct stat status {};
      if (::stat(QFile::encodeName(record.path).constData(), &status) == 0) {
        device = status.st_dev;
        inode = status.st_ino;
      }
    }
    const qsizetype index = candidates.size();
    candidates.append({record.path, record.bytes, record.modified, device, inode});
    byInodeAndSize[{inode, record.bytes}].append(index);
    bySizeAndModified[{record.bytes, record.modified}].append(index);
    bySize[record.bytes].append(index);
    live.insert(record.path);
  }

  struct CachedFingerprint {
    qint64 bytes;
    qint64 modified;
    QString version;
    QByteArray fingerprint;
  };
  // Bound memory across scans and share only within the calling thread. Cache
  // the content hash, not a match decision tied to a particular saved mark.
  static thread_local QCache<QString, CachedFingerprint> fingerprintCache(4096);
  const auto versionFor = identityVersionFor;
  QHash<QString, CachedFingerprint> fingerprints;
  const auto fingerprintFor = [&](const Candidate& candidate) -> QByteArray {
    const QString version = versionFor(candidate.path);
    if (version.isEmpty()) return {};
    if (const auto cached = fingerprints.constFind(candidate.path);
        cached != fingerprints.cend() && cached->version == version) {
      return cached->fingerprint;
    }
    if (const auto* cached = fingerprintCache.object(candidate.path);
        cached && cached->bytes == candidate.bytes && cached->modified == candidate.modified &&
        cached->version == version) {
      fingerprints.insert(candidate.path, *cached);
      return cached->fingerprint;
    }
    const QByteArray fingerprint = identityFor(candidate.path).fingerprint;
    if (versionFor(candidate.path) != version) return {};
    const CachedFingerprint cached{candidate.bytes, candidate.modified, version, fingerprint};
    if (!fingerprint.isEmpty()) {
      fingerprintCache.insert(candidate.path, new CachedFingerprint(cached));
    }
    fingerprints.insert(candidate.path, cached);
    return fingerprint;
  };

  // Decide every move before applying any, so two walking marks cannot chase
  // each other's path mid-loop.
  struct Move {
    QString from;
    QString to;
    QString version;
    QByteArray fingerprint;
  };
  QList<Move> moves;
  for (auto it = m_markIdentities.cbegin(); it != m_markIdentities.cend(); ++it) {
    const QString& oldPath = it.key();
    if (live.contains(oldPath) || !relocationAvailable(it.value())) {
      continue;
    }
    const AlbumEntry& identity = it.value();

    QString match;
    int found = 0;
    if (identity.device != 0 && identity.inode != 0) {
      for (qsizetype index : byInodeAndSize.value({identity.inode, identity.bytes})) {
        const Candidate& candidate = candidates.at(index);
        if ((candidate.device == identity.device || candidate.modified == identity.modified) &&
            (identity.fingerprint.isEmpty() || fingerprintFor(candidate) == identity.fingerprint)) {
          match = candidate.path;
          if (++found > 1) {
            break;
          }
        }
      }
    }
    if (found != 1 && !identity.fingerprint.isEmpty()) {
      QList<qsizetype> possible = bySizeAndModified.value({identity.bytes, identity.modified});
      if (possible.isEmpty()) {
        possible = bySize.value(identity.bytes);
      }
      // Same guard as albums and tags: a directory full of equal-sized files
      // must not stall the UI fingerprinting everything.
      if (possible.size() <= 128) {
        match.clear();
        found = 0;
        for (qsizetype index : std::as_const(possible)) {
          const Candidate& candidate = candidates.at(index);
          if (fingerprintFor(candidate) == identity.fingerprint) {
            match = candidate.path;
            if (++found > 1) {
              break;
            }
          }
        }
      }
    }

    if (found == 1 && !match.isEmpty()) {
      const QString version = identity.fingerprint.isEmpty() ? versionFor(match)
                                                             : fingerprints.value(match).version;
      moves.append({oldPath, match, version, identity.fingerprint});
    }
  }

  if (moves.isEmpty()) {
    return;
  }

  bool moved = false;
  for (const auto& move : std::as_const(moves)) {
    const QString& from = move.from;
    const QString& to = move.to;
    if (!move.version.isEmpty() && versionFor(to) != move.version) continue;
    const AlbumEntry identity = identityFor(to);
    // Verify before removing source marks and persist only this checked
    // identity. A target may have changed since the move was proposed.
    if ((!move.fingerprint.isEmpty() && identity.fingerprint != move.fingerprint) ||
        (!move.version.isEmpty() && versionFor(to) != move.version)) continue;
    if (m_favorites.remove(from)) {
      m_favorites.insert(to);
    }
    if (m_hidden.remove(from)) {
      m_hidden.insert(to);
    }
    const auto rating = m_ratings.find(from);
    if (rating != m_ratings.end()) {
      const int stars = rating.value();
      m_ratings.erase(rating);
      m_ratings.insert(to, qMax(m_ratings.value(to, 0), stars));
    }
    const auto caption = m_captions.find(from);
    if (caption != m_captions.end()) {
      const QString text = caption.value();
      m_captions.erase(caption);
      if (!m_captions.contains(to)) {
        m_captions.insert(to, text);
      }
    }
    m_markIdentities.remove(from);
    if (identity.resolved) {
      m_markIdentities.insert(to, identity);
    }
    moved = true;
  }

  if (!moved) return;
  clearMarksUndo();
  persistMarks();
  emit marksChanged();
}

QObject* AppSettings::videoPresentation(QObject* object) const {
  auto* surface = qobject_cast<QQuickItem*>(object);
  auto* output = surface ? surface->property("videoSink").value<QVideoSink*>() : nullptr;
  return output ? new VideoPresentation(output, surface) : nullptr;
}

qint64 AppSettings::videoSeekTarget(double milliseconds, double frameRate) const {
  // Qt's FFmpeg seek can select the preceding frame at its exact start.
  // Enter the requested frame by one millisecond, only at a boundary.
  const double frames = milliseconds * frameRate / 1000.0;
  const bool boundary = frameRate > 0 && milliseconds > 0
      && std::abs(frames - std::round(frames)) < 0.0001;
  return std::max<qint64>(0, std::llround(milliseconds) + (boundary ? 1 : 0));
}

QString AppSettings::diagnostics(const QString &theme, qreal scale) const {
  return Diagnostics::report(Diagnostics::Sources::system(), QGuiApplication::platformName(), theme,
                             scale);
}

QVariantList AppSettings::missingTools() const {
  return Diagnostics::missing(Diagnostics::Sources::system());
}

QString AppSettings::screenshotKey() const {
  if (m_screenshotKey.isEmpty()) {
    m_screenshotKey = HyprlandSetup::screenshotKey(
        m_hyprDirectory.isEmpty() ? HyprlandSetup::defaultDirectory() : m_hyprDirectory,
        qEnvironmentVariable("OMARCHY_PATH"));
  }
  return m_screenshotKey;
}

QString AppSettings::windowRules() const {
  switch (HyprlandSetup::rulesState(m_hyprDirectory.isEmpty() ? HyprlandSetup::defaultDirectory()
                                                              : m_hyprDirectory)) {
  case HyprlandSetup::RulesState::Installed: return QStringLiteral("installed");
  case HyprlandSetup::RulesState::NotInstalled: return QStringLiteral("available");
  case HyprlandSetup::RulesState::Unsupported: break;
  }
  return QStringLiteral("unsupported");
}

QString AppSettings::installWindowRules() {
  const auto result = HyprlandSetup::installRules(
      m_hyprDirectory.isEmpty() ? HyprlandSetup::defaultDirectory() : m_hyprDirectory);
  emit windowRulesChanged();
  return result.message;
}

QString AppSettings::removeWindowRules() {
  const auto result = HyprlandSetup::removeRules(
      m_hyprDirectory.isEmpty() ? HyprlandSetup::defaultDirectory() : m_hyprDirectory);
  emit windowRulesChanged();
  return result.message;
}

void AppSettings::setHyprDirectory(const QString &directory) {
  m_hyprDirectory = directory;
  m_screenshotKey.clear();
  emit windowRulesChanged();
}

void AppSettings::copyDiagnostics(const QString &theme, qreal scale) const {
  QGuiApplication::clipboard()->setText(diagnostics(theme, scale));
}
