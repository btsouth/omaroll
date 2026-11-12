#include "app/AppSettings.h"
#include "app/StartupTrace.h"
#include "viewer/ViewerSort.h"
#include "actions/TrashRestore.h"
#include "app/Diagnostics.h"
#include "app/HyprlandSetup.h"
#include "app/VideoPlayback.h"
#include "app/VideoPresentation.h"
#include "sources/FileVersion.h"

#include "library/CaptureRecord.h"
#include "library/TagNames.h"
#include "library/EmbeddedMetadata.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QFutureWatcher>
#include <QtConcurrent>
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
#include <tuple>
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
constexpr auto kViewerSort = "viewer/sort";
constexpr auto kViewerSortReversed = "viewer/sortReversed";
constexpr auto kTransparencyCheckerboard = "viewer/transparencyCheckerboard";
constexpr auto kTrashWithoutAsking = "actions/trashWithoutAsking";
constexpr auto kViewerFilmstrip = "viewer/filmstrip";
constexpr auto kAlbums = "library/albums";
constexpr auto kTags = "library/tags";
// Keywords from files hidden here, and file ratings or captions cleared here.
// Kept in Omaroll's settings, never written to the files.
constexpr auto kRemovedEmbeddedTags = "library/removedEmbeddedTags";
constexpr auto kClearedEmbedded = "library/clearedEmbedded";
constexpr auto kReadEmbedded = "library/readEmbedded";
const QString kClearedRating = QStringLiteral("rating");
const QString kClearedCaption = QStringLiteral("caption");
// Files outside the library whose values the viewer read.
constexpr int kMaximumTransientEmbedded = 32;
constexpr int kFingerprintBatch = 256;
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

QString normalizedTagName(const QString& name) { return TagNames::normalized(name); }
bool tagLessThan(const QString& first, const QString& second) { return TagNames::lessThan(first, second); }
bool tagIsUnder(const QString& candidate, const QString& parent) { return TagNames::isUnder(candidate, parent); }
// clearedEmbedded holds exactly these two keys.
QString clearedKey(const QString& name) {
  return name == kClearedRating || name == kClearedCaption ? name : QString();
}
// The same name, or one nested under it, compared case-folded.
bool foldedAtOrUnder(const QString& folded, const QString& parent) {
  return folded == parent || TagNames::isUnder(folded, parent);
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
  // `--install-window-rules` may write its one key before the first launch;
  // that alone is still a new profile.
  QStringList profileKeys = m_settings.allKeys();
  profileKeys.removeAll(QLatin1String(kWindowRulesOfferDismissedKey));
  const bool existingProfile = !profileKeys.isEmpty();
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
  m_viewerSort = m_settings.value(kViewerSort, QStringLiteral("name")).toString();
  if (!ViewerSort::isKnown(m_viewerSort)) m_viewerSort = QStringLiteral("name");
  m_viewerSortReversed = m_settings.value(kViewerSortReversed, false).toBool();
  m_transparencyCheckerboard = m_settings.value(kTransparencyCheckerboard, true).toBool();
  m_trashWithoutAsking = m_settings.value(kTrashWithoutAsking, false).toBool();
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
  restoreCollections(m_settings.value(kRemovedEmbeddedTags).toMap(), m_removedEmbeddedTags,
                     normalizedTagName);
  restoreCollections(m_settings.value(kClearedEmbedded).toMap(), m_clearedEmbedded, clearedKey);
  m_readEmbedded = m_settings.value(kReadEmbedded, true).toBool();
  rebuildRemovalIndex();

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
    noteSnapshotFailure();
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
  // A refresh that failed (a full disk, a folder that was not writable) is
  // still owed, so the exit tries once more.
  noteSnapshotFailure();
  if (m_snapshotDirty && !snapshotsHeld()) scheduleDailySnapshot(true);
}

void AppSettings::noteSnapshotFailure() {
  if (m_snapshotWriteFailed->exchange(false)) m_snapshotDirty = true;
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

void AppSettings::setTrashWithoutAsking(bool value) {
  if (m_trashWithoutAsking == value) return;
  m_trashWithoutAsking = value;
  setValue(kTrashWithoutAsking, value);
  emit trashWithoutAskingChanged();
}

void AppSettings::setViewerSort(const QString& value) {
  if (!ViewerSort::isKnown(value) || m_viewerSort == value) return;
  m_viewerSort = value;
  setValue(kViewerSort, value);
  emit viewerSortChanged();
}

void AppSettings::setViewerSortReversed(bool value) {
  if (m_viewerSortReversed == value) return;
  m_viewerSortReversed = value;
  setValue(kViewerSortReversed, value);
  emit viewerSortReversedChanged();
}

void AppSettings::setTransparencyCheckerboard(bool value) {
  if (m_transparencyCheckerboard == value) return;
  m_transparencyCheckerboard = value;
  setValue(kTransparencyCheckerboard, value);
  emit transparencyCheckerboardChanged();
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
  if (tagsChangedValue) persistTags();

  // Hidden keywords and cleared values follow the file too, and so do the
  // values read from it: its sidecar moved with it.
  bool removalsChanged = false;
  AlbumEntry moved;
  for (auto* rows : {&m_removedEmbeddedTags, &m_clearedEmbedded}) {
    for (auto it = rows->begin(); it != rows->end(); ++it) {
      for (AlbumEntry& entry : it.value()) {
        if (entry.path != oldPath) continue;
        if (moved.path.isEmpty()) moved = identityFor(newPath);
        entry = moved;
        removalsChanged = true;
      }
    }
  }
  bool embeddedMoved = false;
  if (auto values = m_embedded.find(oldPath); values != m_embedded.end()) {
    EmbeddedValues kept = values.value();
    m_embedded.erase(values);
    kept.bytes = -1; // Read again by the index; rows meanwhile read the file.
    m_embedded.insert(newPath, kept);
    m_embeddedLive.insert(newPath);
    embeddedMoved = true;
  }
  if (removalsChanged) {
    persistRemovals();
    rebuildRemovalIndex();
  }
  if (removalsChanged || embeddedMoved) {
    indexEmbedded(oldPath);
    indexEmbedded(newPath);
  }
  if (tagsChangedValue || removalsChanged || embeddedMoved) emit tagsChanged();
  if (removalsChanged || embeddedMoved) emit marksChanged();
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

QHash<QString, QString> AppSettings::localTagsByFold() const {
  QHash<QString, QString> byFold;
  byFold.reserve(m_tags.size());
  for (auto it = m_tags.cbegin(); it != m_tags.cend(); ++it) byFold.insert(TagNames::folded(it.key()), it.key());
  return byFold;
}

QString AppSettings::displayTag(const QString& keyword, const QHash<QString, QString>& localByFold) const {
  // Level by level: a local tag's spelling, else the spelling of a keyword
  // from files that names that level, else the keyword's own.
  QString result;
  for (const QString& segment : keyword.split(QLatin1Char('/'))) {
    const QString candidate = result.isEmpty() ? segment : result + QLatin1Char('/') + segment;
    const QString folded = TagNames::folded(candidate);
    if (const auto local = localByFold.constFind(folded); local != localByFold.cend()) {
      result = local.value();
    } else if (const auto spelled = m_embeddedSpelling.constFind(folded);
               spelled != m_embeddedSpelling.cend()) {
      result = result.isEmpty() ? spelled.value()
                                : result + QLatin1Char('/') + spelled->section(QLatin1Char('/'), -1);
    } else {
      result = candidate;
    }
  }
  return result;
}

QStringList AppSettings::tagNames() const {
  QStringList names = m_tags.keys();
  if (!m_embeddedTagIndex.isEmpty()) {
    const QHash<QString, QString> local = localTagsByFold();
    QSet<QString> present(local.keyBegin(), local.keyEnd());
    for (auto it = m_embeddedTagIndex.cbegin(); it != m_embeddedTagIndex.cend(); ++it) {
      if (present.contains(it.key())) continue;
      // Ancestors too, so the tree in Browse has no gaps.
      const QStringList segments = displayTag(m_embeddedSpelling.value(it.key()), local).split(QLatin1Char('/'));
      for (qsizetype depth = 1; depth <= segments.size(); ++depth) {
        const QString level = segments.first(depth).join(QLatin1Char('/'));
        const QString folded = TagNames::folded(level);
        if (!present.contains(folded)) {
          present.insert(folded);
          names.append(level);
        }
      }
    }
  }
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
  if (!m_embeddedTagIndex.isEmpty() && !name.isEmpty()) {
    const QString folded = TagNames::folded(name);
    for (auto it = m_embeddedTagIndex.cbegin(); it != m_embeddedTagIndex.cend(); ++it) {
      if (!foldedAtOrUnder(it.key(), folded)) continue;
      for (const QString& path : it.value()) {
        if (!seen.contains(path)) {
          paths.append(path);
          seen.insert(path);
        }
      }
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
  const EmbeddedValues* values = embeddedFor(path);
  if (!values) return tags;
  const QStringList kept = m_embedded.contains(path) ? m_embeddedEffective.value(path)
                                                     : effectiveKeywords(path, values->keywords);
  if (kept.isEmpty()) return tags;
  const QHash<QString, QString> local = localTagsByFold();
  QSet<QString> present;
  for (const QString& tag : std::as_const(tags)) present.insert(TagNames::folded(tag));
  for (const QString& keyword : kept) {
    const QString display = displayTag(keyword, local);
    if (!present.contains(TagNames::folded(display))) {
      present.insert(TagNames::folded(display));
      tags.append(display);
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
  if (m_embeddedEffective.isEmpty()) return byPath;
  const QHash<QString, QString> local = localTagsByFold();
  QHash<QString, QString> displayed;
  for (auto it = m_embeddedEffective.cbegin(); it != m_embeddedEffective.cend(); ++it) {
    QStringList& names = byPath[it.key()];
    for (const QString& keyword : it.value()) {
      const QString folded = TagNames::folded(keyword);
      auto display = displayed.constFind(folded);
      if (display == displayed.cend()) display = displayed.insert(folded, displayTag(keyword, local));
      bool present = false;
      for (const QString& name : std::as_const(names)) {
        if (name.compare(display.value(), Qt::CaseInsensitive) == 0) {
          present = true;
          break;
        }
      }
      if (!present) names.append(display.value());
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
  // parent can be chosen to see everything under it. A name that exists so
  // far only as a keyword from files gets its local key here.
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

QString AppSettings::ensureTagKeys(const QString& normalized) {
  const QStringList segments = normalized.split(QLatin1Char('/'));
  QString spelled;
  for (qsizetype depth = 1; depth <= segments.size(); ++depth) {
    const QString level = (spelled.isEmpty() ? QString() : spelled + QLatin1Char('/')) + segments.at(depth - 1);
    QString present;
    for (const QString& existing : m_tags.keys()) {
      if (existing.compare(level, Qt::CaseInsensitive) == 0) {
        present = existing;
        break;
      }
    }
    if (present.isEmpty()) {
      m_tags.insert(level, {});
      present = level;
    }
    spelled = present;
  }
  return spelled;
}

const AppSettings::EmbeddedValues* AppSettings::embeddedFor(const QString& path) const {
  if (const auto it = m_embedded.constFind(path); it != m_embedded.cend()) return &it.value();
  if (const auto it = m_transientEmbedded.constFind(path); it != m_transientEmbedded.cend())
    return &it.value();
  return nullptr;
}

QStringList AppSettings::effectiveKeywords(const QString& path, const QStringList& keywords) const {
  const auto removed = m_removedByPath.constFind(path);
  if (removed == m_removedByPath.cend()) return keywords;
  QStringList kept;
  for (const QString& keyword : keywords) {
    const QString folded = TagNames::folded(keyword);
    // A removal hides the keyword and everything nested under it.
    const bool hidden = std::any_of(removed->cbegin(), removed->cend(), [&](const QString& name) {
      return foldedAtOrUnder(folded, name);
    });
    if (!hidden) kept.append(keyword);
  }
  return kept;
}

void AppSettings::indexEmbedded(const QString& path) {
  for (const QString& keyword : m_embeddedEffective.take(path)) {
    const QString folded = TagNames::folded(keyword);
    if (auto it = m_embeddedTagIndex.find(folded); it != m_embeddedTagIndex.end()) {
      it->remove(path);
      if (it->isEmpty()) {
        m_embeddedTagIndex.erase(it);
        m_embeddedSpelling.remove(folded);
      }
    }
  }
  const auto values = m_embedded.constFind(path);
  if (values == m_embedded.cend()) return;
  const QStringList kept = effectiveKeywords(path, values->keywords);
  if (kept.isEmpty()) return;
  for (const QString& keyword : kept) {
    const QString folded = TagNames::folded(keyword);
    m_embeddedTagIndex[folded].insert(path);
    if (!m_embeddedSpelling.contains(folded)) m_embeddedSpelling.insert(folded, keyword);
  }
  m_embeddedEffective.insert(path, kept);
}

void AppSettings::rebuildEmbeddedIndex() {
  m_embeddedEffective.clear();
  m_embeddedTagIndex.clear();
  m_embeddedSpelling.clear();
  for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) indexEmbedded(it.key());
}

void AppSettings::rebuildRemovalIndex() {
  m_removedByPath.clear();
  for (auto it = m_removedEmbeddedTags.cbegin(); it != m_removedEmbeddedTags.cend(); ++it) {
    const QString folded = TagNames::folded(it.key());
    for (const AlbumEntry& entry : it.value()) {
      if (!entry.resolved) continue;
      QStringList& names = m_removedByPath[entry.path];
      if (!names.contains(folded)) names.append(folded);
    }
  }
  const auto paths = [this](const QString& key) {
    QSet<QString> set;
    for (const AlbumEntry& entry : m_clearedEmbedded.value(key))
      if (entry.resolved) set.insert(entry.path);
    return set;
  };
  m_clearedRatings = paths(kClearedRating);
  m_clearedCaptions = paths(kClearedCaption);
}

QStringList AppSettings::visibleKeywords(const QString& path) const {
  // A keyword already hidden on this file is not there for a rename, delete
  // or remove to act on; acting on it would bring it back.
  if (m_embedded.contains(path)) return m_embeddedEffective.value(path);
  const auto transient = m_transientEmbedded.constFind(path);
  if (transient == m_transientEmbedded.cend()) return {};
  return effectiveKeywords(path, transient->keywords);
}

QStringList AppSettings::embeddedKeywordsUnder(const QString& path, const QString& name) const {
  QStringList found;
  if (name.isEmpty()) return found;
  const QString folded = TagNames::folded(name);
  for (const QString& keyword : visibleKeywords(path)) {
    if (foldedAtOrUnder(TagNames::folded(keyword), folded)) found.append(keyword);
  }
  return found;
}

QStringList AppSettings::embeddedCarriers(const QString& name) const {
  QStringList carriers;
  if (name.isEmpty()) return carriers;
  const QString folded = TagNames::folded(name);
  const auto carries = [&](const QStringList& keywords) {
    return std::any_of(keywords.cbegin(), keywords.cend(), [&](const QString& keyword) {
      return foldedAtOrUnder(TagNames::folded(keyword), folded);
    });
  };
  for (auto it = m_embeddedEffective.cbegin(); it != m_embeddedEffective.cend(); ++it) {
    if (carries(it.value())) carriers.append(it.key());
  }
  for (auto it = m_transientEmbedded.cbegin(); it != m_transientEmbedded.cend(); ++it) {
    if (!m_embedded.contains(it.key()) && carries(effectiveKeywords(it.key(), it->keywords)))
      carriers.append(it.key());
  }
  return carriers;
}

AppSettings::AlbumEntry AppSettings::rowFor(const QString& path) const {
  // The identity the index read the file with: no file read here, so hiding
  // a keyword across thousands of files stays off the disk. The fingerprint
  // follows from a worker.
  if (const EmbeddedValues* values = embeddedFor(path); values && values->bytes >= 0) {
    AlbumEntry entry;
    entry.path = path;
    entry.bytes = values->bytes;
    entry.modified = values->modified;
    entry.device = values->device;
    entry.inode = values->inode;
    entry.resolved = true;
    return entry;
  }
  return identityFor(path);
}

bool AppSettings::addRows(QMap<QString, QList<AlbumEntry>>& rows, const QString& name,
                          const QList<AlbumEntry>& added) {
  if (name.isEmpty() || added.isEmpty()) return false;
  const QString folded = TagNames::folded(name);
  QSet<QString> present;
  QString key = name;
  for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
    if (TagNames::folded(it.key()) != folded) continue;
    key = it.key();
    for (const AlbumEntry& entry : it.value()) present.insert(entry.path);
  }
  bool changed = false;
  QList<AlbumEntry>& target = rows[key];
  for (const AlbumEntry& entry : added) {
    if (entry.path.isEmpty() || present.contains(entry.path)) continue;
    present.insert(entry.path);
    target.append(entry);
    changed = true;
  }
  if (target.isEmpty()) rows.remove(key);
  return changed;
}

bool AppSettings::dropRows(QMap<QString, QList<AlbumEntry>>& rows, const QString& name,
                           const QString& path) {
  const QString folded = TagNames::folded(name);
  bool changed = false;
  for (auto it = rows.begin(); it != rows.end();) {
    if (TagNames::folded(it.key()) == folded &&
        it->removeIf([&path](const AlbumEntry& entry) { return entry.path == path; }) > 0) {
      changed = true;
    }
    it = it->isEmpty() ? rows.erase(it) : std::next(it);
  }
  return changed;
}

int AppSettings::deleteTag(const QString& name) {
  int removed = m_tags.remove(name);
  for (const QString& existing : m_tags.keys()) {
    if (tagIsUnder(existing, name)) {
      removed += m_tags.remove(existing);
    }
  }
  // Every file carrying the keyword, or one nested under it, keeps it hidden
  // here. The files themselves are not changed.
  const QStringList carriers = embeddedCarriers(name);
  QList<AlbumEntry> rows;
  rows.reserve(carriers.size());
  for (const QString& path : carriers) rows.append(rowFor(path));
  const bool hidden = addRows(m_removedEmbeddedTags, name, rows);
  if (removed == 0 && !hidden) {
    return 0;
  }
  const int views = removeViewsNaming(QStringLiteral("tag"), [&name](const QString& tag) {
    return tag == name || tagIsUnder(tag, name);
  });
  m_deferOrganizationSync = true;
  if (removed > 0) persistTags();
  if (hidden) persistRemovals();
  if (views > 0) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
  if (hidden) {
    rebuildRemovalIndex();
    for (const QString& path : carriers) indexEmbedded(path);
    scheduleFingerprints();
  }
  emit tagsChanged();
  return views;
}

bool AppSettings::renameTag(const QString& oldName, const QString& newName) {
  const QString target = normalizedTagName(newName);
  if (target.isEmpty() || target == oldName) {
    return false;
  }
  // Renaming a tag into its own subtree would make the move ambiguous.
  if (tagIsUnder(target, oldName)) {
    return false;
  }
  // Files that carry the keyword, or one under it, get a removal row for the
  // old name and local tags under the new one, so the file is unchanged and
  // the rename still shows.
  const QStringList carriers = embeddedCarriers(oldName);
  const qsizetype oldDepth = oldName.split(QLatin1Char('/')).size();
  const auto renamed = [&](const QString& keyword) {
    const QStringList segments = keyword.split(QLatin1Char('/'));
    return segments.size() > oldDepth ? target + QLatin1Char('/') + segments.mid(oldDepth).join(QLatin1Char('/'))
                                      : target;
  };

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
  QHash<QString, QList<QString>> fileMoves; // new name to the files that get it
  for (const QString& path : carriers) {
    for (const QString& keyword : embeddedKeywordsUnder(path, oldName)) {
      fileMoves[renamed(keyword)].append(path);
    }
  }
  if (moves.isEmpty() && fileMoves.isEmpty()) {
    return false;
  }

  // A new name must not land on a tag that is staying where it is, or the
  // rename would silently swallow it. Keywords from files count too.
  QSet<QString> targetsLower;
  for (const auto& move : moves) targetsLower.insert(move.second.toLower());
  for (auto it = fileMoves.cbegin(); it != fileMoves.cend(); ++it) targetsLower.insert(it.key().toLower());
  const QString oldFolded = TagNames::folded(oldName);
  for (const QString& key : tagNames()) {
    if (!sources.contains(key) && !foldedAtOrUnder(TagNames::folded(key), oldFolded) &&
        targetsLower.contains(key.toLower())) {
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
  QList<AlbumEntry> hiddenRows;
  for (auto it = fileMoves.cbegin(); it != fileMoves.cend(); ++it) {
    const QString key = ensureTagKeys(it.key());
    QList<AlbumEntry>& entries = m_tags[key];
    QSet<QString> present;
    for (const AlbumEntry& entry : std::as_const(entries)) present.insert(entry.path);
    for (const QString& path : it.value()) {
      if (present.contains(path)) continue;
      present.insert(path);
      entries.append(rowFor(path));
    }
  }
  for (const QString& path : carriers) hiddenRows.append(rowFor(path));
  const bool hidden = addRows(m_removedEmbeddedTags, oldName, hiddenRows);
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
  if (hidden) persistRemovals();
  if (viewsChanged) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  m_deferOrganizationSync = false;
  syncOrganization();
  if (hidden) {
    rebuildRemovalIndex();
    for (const QString& path : carriers) indexEmbedded(path);
  }
  if (!carriers.isEmpty()) scheduleFingerprints();
  emit tagsChanged();
  return m_organizationError.isEmpty();
}

bool AppSettings::addTag(const QString& name, const QStringList& paths) {
  auto it = m_tags.find(name);
  bool created = false;
  if (it == m_tags.end()) {
    // A tag that exists so far only as a keyword from files gets its local
    // key first.
    if (!tagNames().contains(name)) return false;
    it = m_tags.find(ensureTagKeys(name));
    created = true;
  }
  bool changed = false;
  bool shown = false;
  for (const QString& path : paths) {
    // Adding a tag again undoes hiding it on that file.
    shown = dropRows(m_removedEmbeddedTags, name, path) || shown;
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
  if (changed || created || shown) {
    m_deferOrganizationSync = true;
    if (changed || created) persistTags();
    if (shown) persistRemovals();
    m_deferOrganizationSync = false;
    syncOrganization();
    if (shown) {
      rebuildRemovalIndex();
      for (const QString& path : paths) indexEmbedded(path);
    }
    emit tagsChanged();
  }
  return (changed || shown) && m_organizationError.isEmpty();
}

void AppSettings::removeTag(const QString& name, const QStringList& paths) {
  bool changed = false;
  if (auto it = m_tags.find(name); it != m_tags.end()) {
    for (qsizetype index = it->size() - 1; index >= 0; --index) {
      if (paths.contains(it->at(index).path)) {
        it->removeAt(index);
        changed = true;
      }
    }
  }
  // A file whose own keywords carry the tag, or one nested under it, keeps
  // it hidden here instead. Its file is not changed.
  QList<AlbumEntry> rows;
  QStringList carriers;
  for (const QString& path : paths) {
    if (!embeddedKeywordsUnder(path, name).isEmpty()) {
      rows.append(rowFor(path));
      carriers.append(path);
    }
  }
  const bool hidden = addRows(m_removedEmbeddedTags, name, rows);
  if (!changed && !hidden) return;
  m_deferOrganizationSync = true;
  if (changed) persistTags();
  if (hidden) persistRemovals();
  m_deferOrganizationSync = false;
  syncOrganization();
  if (hidden) {
    rebuildRemovalIndex();
    for (const QString& path : std::as_const(carriers)) {
      indexEmbedded(path);
    }
    scheduleFingerprints();
  }
  emit tagsChanged();
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

QList<bool> AppSettings::reconcileCollectionMaps(const QList<QMap<QString, QList<AlbumEntry>>*>& maps,
                                                const QList<CaptureRecord>& records) {
  QList<bool> changed(maps.size(), false);
  if (std::all_of(maps.cbegin(), maps.cend(), [](const auto* map) { return map->isEmpty(); })) return changed;
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

  for (qsizetype map = 0; map < maps.size(); ++map) {
  bool persistedChange = false;
  bool resolutionChange = false;
  for (auto collection = maps.at(map)->begin(); collection != maps.at(map)->end(); ++collection) {
    for (AlbumEntry& entry : collection.value()) {
      const bool wasResolved = entry.resolved;
      entry.resolved = false;

      const auto live = byPath.constFind(entry.path);
      const bool livePath = live != byPath.cend();
      if (livePath) {
        // The scanner's own identity for the file, gathered off the window
        // thread, settles an unchanged entry without another stat.
        const Candidate& scanned = candidates.at(live.value());
        if (scanned.device != 0 && scanned.inode != 0 && entry.device == scanned.device &&
            entry.inode == scanned.inode && entry.bytes == scanned.bytes &&
            entry.modified == scanned.modified) {
          entry.resolved = true;
          resolutionChange = resolutionChange || !wasResolved;
          continue;
        }
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
  changed[map] = persistedChange || resolutionChange;
  }
  return changed;
}

void AppSettings::reconcileAlbums(const QList<CaptureRecord>& records) {
  if (reconcileCollectionMap(m_albums, records)) {
    persistAlbums();
    emit albumsChanged();
  }
}

void AppSettings::reconcileTags(const QList<CaptureRecord>& records) {
  // Hidden keywords and cleared values follow files moved outside Omaroll the
  // way tags do, in the same pass over the scan.
  const QList<bool> changed =
      reconcileCollectionMaps({&m_tags, &m_removedEmbeddedTags, &m_clearedEmbedded}, records);
  const bool removals = changed.at(1) || changed.at(2);
  // Rows written from a scanner identity, in this session or an earlier one.
  scheduleFingerprints();
  if (!changed.at(0) && !removals) return;
  m_deferOrganizationSync = true;
  if (changed.at(0)) persistTags();
  if (removals) persistRemovals();
  m_deferOrganizationSync = false;
  syncOrganization();
  if (removals) {
    rebuildRemovalIndex();
    rebuildEmbeddedIndex();
  }
  if (changed.at(0) || changed.at(1)) emit tagsChanged();
  if (changed.at(2)) emit marksChanged();
}

namespace {
constexpr int kDailyBackups = 7;
constexpr int kWeeklyBackups = 4;
constexpr int kBeforeRestoreBackups = 4;
constexpr auto kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

// Empty, or under half of a snapshot that held at least three more items.
bool looksIncomplete(int loaded, int snapshot) {
  if (snapshot <= 0) return false;
  return loaded == 0 || (qint64(loaded) * 2 < snapshot && snapshot - loaded >= 3);
}

// Snapshots list the paths of organized files, so keep them private.
bool prepareBackupDirectory(const QString& path) {
  if (!QDir().mkpath(path)) return false;
  QFile::setPermissions(path, kOwnerOnly | QFileDevice::ExeOwner);
  return true;
}

bool writeBackupFile(const QString& path, const QByteArray& payload) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  // Owner-only from the start: the temporary file becomes the snapshot. The
  // mode is set again after the rename for file systems that ignore this.
  file.setPermissions(kOwnerOnly);
  if (file.write(payload) != payload.size() || !file.commit()) return false;
  QFile::setPermissions(path, kOwnerOnly);
  return true;
}

// daily-2026-11-08.json and weekly-2026-11-02.json carry their date.
// before-change-2026-11-08.json too.
constexpr QLatin1StringView kBeforeChange("before-change-");
QDate backupDate(const QString& name) {
  const qsizetype at = name.startsWith(kBeforeChange) ? kBeforeChange.size() : name.indexOf(QLatin1Char('-')) + 1;
  return QDate::fromString(name.mid(at, 10), Qt::ISODate);
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
  // Copies kept before a change live exactly as long as their day's daily,
  // checked on every write rather than only when another copy is kept.
  QSet<QDate> days;
  for (const QString& name : dir.entryList({QStringLiteral("daily-*.json")}, QDir::Files)) days.insert(backupDate(name));
  QDir copies(path);
  for (const QString& name : copies.entryList({QStringLiteral("before-change-*.json")}, QDir::Files))
    if (!days.contains(backupDate(name))) copies.remove(name);
  return true;
}
} // namespace

QString AppSettings::backupDirectory() const {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
         + QStringLiteral("/omaroll/backups");
}

QUrl AppSettings::backupFolderUrl() const {
  return QUrl::fromLocalFile(backupDirectory());
}

AppSettings::OrganizationState AppSettings::organizationState() const {
  return {m_albums, m_tags, m_smartCollections, m_favorites, m_hidden, m_ratings, m_captions,
          m_markIdentities, m_removedEmbeddedTags, m_clearedEmbedded};
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

// Albums and tags count once each and once per file in them, then every mark
// and saved view: a rough size to compare a load with a snapshot.
int AppSettings::organizationSize(const OrganizationState& state) {
  qsizetype size = 0;
  for (const auto* collections : {&state.albums, &state.tags})
    for (const auto& entries : *collections) size += 1 + entries.size();
  size += state.favorites.size() + state.hidden.size() + state.ratings.size() + state.captions.size() +
          state.smartCollections.size();
  // One per removal row. Values read from files never count: they are read
  // again, so a launch before the index runs, with a drive unplugged or with
  // reading turned off cannot look incomplete.
  for (const auto* rows : {&state.removedEmbeddedTags, &state.clearedEmbedded})
    for (const auto& entries : *rows) size += entries.size();
  return int(qMin<qsizetype>(size, std::numeric_limits<int>::max()));
}

int AppSettings::snapshotSize(const QByteArray& payload) {
  DecodedOrganization decoded;
  if (!decodeOrganization(payload, decoded).isEmpty()) return -1;
  return organizationSize({decoded.albums, decoded.tags, decoded.smartCollections, decoded.favorites,
                           decoded.hidden, decoded.ratings, decoded.captions, decoded.markIdentities,
                           decoded.removedEmbeddedTags, decoded.clearedEmbedded});
}

// A refresh follows the latest state, so the launch check compares with what
// was last saved and a deliberate clear-out is not warned about. When that
// state looks incomplete against the snapshot it replaces (a mass untag or
// album delete by mistake), that snapshot is kept beside it first. Each day
// keeps the fullest state it had, before-change-<day>.json, and, when it
// differs, the state just before the latest drop, before-change-<day>-latest.json,
// so work built after a deliberate clear-out is not lost to an older, larger
// copy. Both are kept exactly as long as their day's daily.
bool AppSettings::keepFullerSnapshot(const QString& path, const QDate& day, int replacing, bool replace) {
  const QDir dir(path);
  const auto daily = [&dir](const QDate& date) {
    return dir.filePath(QStringLiteral("daily-%1.json").arg(date.toString(Qt::ISODate)));
  };
  const auto read = [](const QString& file) -> std::pair<QByteArray, int> {
    QFile input(file);
    if (input.size() > kMaximumOrganizationBytes || !input.open(QIODevice::ReadOnly)) return {{}, -1};
    QByteArray bytes = input.read(kMaximumOrganizationBytes + 1);
    const int size = snapshotSize(bytes);
    return {size < 0 ? QByteArray() : bytes, size};
  };
  const bool exists = QFileInfo::exists(daily(day));
  if (exists && !replace) return true; // Nothing is replaced.
  auto [payload, held] = read(daily(day));
  // The day's first snapshot, in a session left open past midnight, is
  // checked against the newest earlier daily instead.
  for (int back = 1; !exists && held < 0 && back <= kDailyBackups; ++back)
    std::tie(payload, held) = read(daily(day.addDays(-back)));
  if (!looksIncomplete(replacing, held)) return true;
  const QString date = day.toString(Qt::ISODate);
  const QString fullest = QStringLiteral("before-change-%1.json").arg(date);
  const QString latest = QStringLiteral("before-change-%1-latest.json").arg(date);
  const auto [kept, keptSize] = read(dir.filePath(fullest));
  if (keptSize < held) {
    if (!writeBackupFile(dir.filePath(fullest), payload)) return false;
    QFile::remove(dir.filePath(latest)); // The fullest is also the latest now.
  } else if (kept != payload) {
    if (!writeBackupFile(dir.filePath(latest), payload)) return false;
  } else {
    QFile::remove(dir.filePath(latest));
  }
  // writeScheduledBackups prunes them with the dailies.
  return true;
}

bool AppSettings::writeDailySnapshot(const QString& path, const QDate& day, const QByteArray& payload, int size,
                                     bool replace) {
  // Without the copy of a fuller snapshot, it is not replaced.
  return prepareBackupDirectory(path) && keepFullerSnapshot(path, day, size, replace) &&
         writeScheduledBackups(path, day, payload, replace);
}

void AppSettings::checkOrganizationHealth(bool existingProfile) {
  // What loaded, before any edit made while the check runs can add to it.
  m_loadedSize = organizationSize(organizationState());
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
      // Checked as an import checks it: a header-only, cut or unreadable
      // snapshot is passed over for an older one that restores.
      const int size = snapshotSize(file.read(kMaximumOrganizationBytes + 1));
      if (size < 0) continue;
      std::lock_guard lock(report->mutex);
      report->snapshot = file.fileName();
      report->date = day;
      report->snapshotSize = size;
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
  // Compared with what loaded, not with edits made while the check ran, which
  // could hide a damaged load; one that looks incomplete is not snapshotted
  // over the backup.
  const int loaded = m_loadedSize;
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
  const OrganizationState state = organizationState();
  m_backupWriter->post([path = backupDirectory(), today, payload = encodeOrganization(state),
                        size = organizationSize(state), replace, failed = m_snapshotWriteFailed] {
    if (!writeDailySnapshot(path, today, payload, size, replace)) failed->store(true);
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
  const OrganizationState state = organizationState();
  return writeDailySnapshot(backupDirectory(), date, encodeOrganization(state), organizationSize(state), false);
}

bool AppSettings::canRestoreOrganization() const {
  return (settingsStatus() == QSettings::FormatError || m_organizationSuspect) &&
         !QDir(backupDirectory()).entryList({QStringLiteral("daily-*.json"), QStringLiteral("weekly-*.json"),
                 QStringLiteral("before-change-*.json"), QStringLiteral("before-restore-*.json")}, QDir::Files).isEmpty();
}

QVariantMap AppSettings::restoreLastGoodBackup() {
  flushBackups();
  const QDir dir(backupDirectory());
  // Newest date first. Within a day: its own snapshot, then the fuller one
  // kept before a change that day, then its week's copy. Dates after today
  // come from a clock that ran ahead and go last.
  const QDate today = QDate::currentDate();
  struct Candidate {
    QString name;
    QDate date;
    int order;
    bool current;
  };
  QList<Candidate> candidates;
  for (const QString& name : dir.entryList({QStringLiteral("daily-*.json"), QStringLiteral("before-change-*.json"),
                                            QStringLiteral("weekly-*.json")}, QDir::Files)) {
    const QDate date = backupDate(name);
    // The state just before the latest drop, then the fullest of the day.
    const int order = name.startsWith(QLatin1String("daily-")) ? 0
                      : name.startsWith(kBeforeChange)        ? (name.endsWith(QLatin1String("-latest.json")) ? 1 : 2)
                                                              : 3;
    candidates.append({name, date, order, date.isValid() && date <= today});
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.current != b.current) return a.current;
    if (a.date != b.date) return a.date > b.date;
    return a.order < b.order;
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
    const bool beforeChange = name.startsWith(kBeforeChange);
    const QDateTime taken = QDateTime::fromString(result.value(QStringLiteral("exportedAt")).toString(), Qt::ISODate);
    const QDate day = name.startsWith(QLatin1String("daily-")) || beforeChange || !taken.isValid()
        ? backupDate(name) : taken.toLocalTime().date();
    if (day.isValid()) {
      result.insert(QStringLiteral("backupDate"), day);
      result.insert(QStringLiteral("message"), result.value(QStringLiteral("message")).toString()
                        + (beforeChange ? QStringLiteral(" from the backup kept before a change on ")
                                        : QStringLiteral(" from the backup of "))
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

  // Optional, and only written with rows: 1.18 checks only its own members,
  // so it still imports this file and loses only what it cannot use. The
  // rows carry their own identities; markIdentities is left as it was.
  if (!state.removedEmbeddedTags.isEmpty())
    root.insert(QStringLiteral("removedEmbeddedTags"), encodeEntries(state.removedEmbeddedTags));
  if (!state.clearedEmbedded.isEmpty())
    root.insert(QStringLiteral("clearedEmbedded"), encodeEntries(state.clearedEmbedded));

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

QVariantMap AppSettings::decodeOrganization(const QByteArray& payload, DecodedOrganization& out) {
  const auto invalidBackup = []() -> QVariantMap {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("This backup contains invalid organization")}};
  };
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

  // Optional too, and as strict as tags when present: a bad member refuses
  // the whole file. clearedEmbedded holds only "rating" and "caption".
  QMap<QString, QList<AlbumEntry>> removedEmbeddedTags;
  if (root.contains(QStringLiteral("removedEmbeddedTags")) &&
      (!memberIs("removedEmbeddedTags", QJsonValue::Object) ||
       !decodeEntries(root.value(QStringLiteral("removedEmbeddedTags")).toObject(), normalizedTagName,
                      removedEmbeddedTags)))
    return invalidBackup();
  QMap<QString, QList<AlbumEntry>> clearedEmbedded;
  if (root.contains(QStringLiteral("clearedEmbedded"))) {
    if (!memberIs("clearedEmbedded", QJsonValue::Object)) return invalidBackup();
    const QJsonObject cleared = root.value(QStringLiteral("clearedEmbedded")).toObject();
    for (auto it = cleared.begin(); it != cleared.end(); ++it)
      if (clearedKey(it.key()).isEmpty()) return invalidBackup();
    if (!decodeEntries(cleared, clearedKey, clearedEmbedded)) return invalidBackup();
  }

  out = {exportedAt, std::move(albums), std::move(tags), std::move(favorites), std::move(hidden),
         std::move(ratings), std::move(captions), std::move(smartCollections), std::move(markIdentities),
         std::move(removedEmbeddedTags), std::move(clearedEmbedded)};
  return {};
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
  DecodedOrganization decoded;
  if (const QVariantMap error = decodeOrganization(payload, decoded); !error.isEmpty()) return error;
  const QString exportedAt = decoded.exportedAt;
  auto albums = std::move(decoded.albums);
  auto tags = std::move(decoded.tags);
  auto favorites = std::move(decoded.favorites);
  auto hidden = std::move(decoded.hidden);
  auto ratings = std::move(decoded.ratings);
  auto captions = std::move(decoded.captions);
  auto smartCollections = std::move(decoded.smartCollections);
  auto markIdentities = std::move(decoded.markIdentities);
  // A restore returns to the backup's own removal rows, so a backup made
  // before a keyword was hidden shows it again.
  auto removedEmbeddedTags = std::move(decoded.removedEmbeddedTags);
  auto clearedEmbedded = std::move(decoded.clearedEmbedded);

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
  resolvePresent(removedEmbeddedTags);
  resolvePresent(clearedEmbedded);

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
  const MarksSnapshot oldMarks{m_favorites, m_hidden, m_ratings, m_captions, m_markIdentities, m_clearedEmbedded};
  const auto oldRemoved = m_removedEmbeddedTags;
  qsizetype hiddenBefore = 0;
  for (const auto& rows : std::as_const(m_removedEmbeddedTags)) hiddenBefore += rows.size();
  m_albums = std::move(albums);
  m_tags = std::move(tags);
  m_smartCollections = std::move(smartCollections);
  m_favorites = std::move(favorites);
  m_hidden = std::move(hidden);
  m_ratings = std::move(ratings);
  m_captions = std::move(captions);
  m_markIdentities = std::move(markIdentities);
  m_removedEmbeddedTags = std::move(removedEmbeddedTags);
  m_clearedEmbedded = std::move(clearedEmbedded);

  m_restoring = true;
  const auto restoring = qScopeGuard([this] { m_restoring = false; });
  m_deferOrganizationSync = true;
  persistAlbums();
  persistTags();
  persistSmartCollections();
  persistMarks();
  persistRemovals();
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
    m_clearedEmbedded = oldMarks.clearedEmbedded;
    m_removedEmbeddedTags = oldRemoved;
    // Replace pending failed values too, so a later flush cannot restore them.
    m_deferOrganizationSync = true;
    persistAlbums();
    persistTags();
    persistSmartCollections();
    persistMarks();
    persistRemovals();
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
  rebuildRemovalIndex();
  rebuildEmbeddedIndex();
  emit albumsChanged();
  emit tagsChanged();
  emit smartCollectionsChanged();
  emit marksChanged();

  const int marks = m_favorites.size() + m_hidden.size() + m_ratings.size() + m_captions.size();
  qsizetype hiddenAfter = 0;
  for (const auto& rows : std::as_const(m_removedEmbeddedTags)) hiddenAfter += rows.size();
  QVariantMap result;
  result.insert(QStringLiteral("ok"), true);
  result.insert(QStringLiteral("message"),
                QStringLiteral("Restored %1 album%2, %3 tag%4 and %5 item%6")
                    .arg(m_albums.size())
                    .arg(m_albums.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(m_tags.size())
                    .arg(m_tags.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(marks)
                    .arg(marks == 1 ? QString() : QStringLiteral("s"))
                    // Keywords read from files are not in backups. Those
                    // hidden after this backup was made show again.
                    + (hiddenAfter < hiddenBefore
                           ? QStringLiteral(". Keywords from files hidden since this backup show again")
                           : QString()));
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

void AppSettings::persistRemovals() {
  const auto stored = [](const QMap<QString, QList<AlbumEntry>>& collections) {
    QVariantMap out;
    for (auto it = collections.cbegin(); it != collections.cend(); ++it) {
      QVariantList entries;
      for (const AlbumEntry& entry : it.value()) {
        QVariantMap row;
        row.insert(QStringLiteral("path"), entry.path);
        row.insert(QStringLiteral("bytes"), entry.bytes);
        row.insert(QStringLiteral("modified"), entry.modified);
        row.insert(QStringLiteral("fingerprint"), entry.fingerprint);
        row.insert(QStringLiteral("device"), QString::number(entry.device));
        row.insert(QStringLiteral("inode"), QString::number(entry.inode));
        entries.append(row);
      }
      out.insert(it.key(), entries);
    }
    return out;
  };
  // New keys: 1.18 ignores them, and QSettings keeps them when it saves.
  const bool deferred = m_deferOrganizationSync;
  m_deferOrganizationSync = true;
  for (const auto& [key, rows] : {std::pair{kRemovedEmbeddedTags, &m_removedEmbeddedTags},
                                  std::pair{kClearedEmbedded, &m_clearedEmbedded}}) {
    if (!rows->isEmpty()) {
      setValue(key, stored(*rows));
    } else if (settingsStatus() != QSettings::FormatError && m_settings.contains(key)) {
      m_settings.remove(key);
    }
  }
  m_deferOrganizationSync = deferred;
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

int AppSettings::embeddedRating(const QString& path) const {
  const EmbeddedValues* values = embeddedFor(path);
  return values ? values->rating : 0;
}

QString AppSettings::embeddedCaption(const QString& path) const {
  const EmbeddedValues* values = embeddedFor(path);
  return values ? values->caption : QString();
}

int AppSettings::rating(const QString& path) const {
  if (const auto local = m_ratings.constFind(path); local != m_ratings.cend()) return local.value();
  return m_clearedRatings.contains(path) ? 0 : embeddedRating(path);
}

int AppSettings::ratedCount() const {
  qsizetype count = m_ratings.size();
  for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) {
    if (it->rating > 0 && !m_ratings.contains(it.key()) && !m_clearedRatings.contains(it.key())) ++count;
  }
  return static_cast<int>(count);
}

void AppSettings::setRating(const QStringList& paths, int rating) {
  const int stars = qBound(0, rating, kMaximumRating);
  pushMarksUndo();
  bool changed = false;
  bool cleared = false;
  for (const QString& path : paths) {
    if (path.isEmpty()) {
      continue;
    }
    if (stars == 0) {
      changed = m_ratings.remove(path) > 0 || changed;
      // Clearing a rating the file states is kept here, not in the file.
      if (embeddedRating(path) > 0) cleared = addRows(m_clearedEmbedded, kClearedRating, {rowFor(path)}) || cleared;
    } else {
      if (m_ratings.value(path, 0) != stars) {
        m_ratings.insert(path, stars);
        changed = true;
      }
      cleared = dropRows(m_clearedEmbedded, kClearedRating, path) || cleared;
    }
  }
  if (!changed && !cleared) {
    return;
  }
  for (const QString& path : paths) {
    refreshMarkIdentity(path);
  }
  const bool deferred = m_deferOrganizationSync;
  m_deferOrganizationSync = true;
  persistMarks();
  if (cleared) persistRemovals();
  m_deferOrganizationSync = deferred;
  syncOrganization();
  if (cleared) {
    rebuildRemovalIndex();
    scheduleFingerprints();
  }
  emit marksChanged();
}

QString AppSettings::caption(const QString& path) const {
  if (const auto local = m_captions.constFind(path); local != m_captions.cend()) return local.value();
  return m_clearedCaptions.contains(path) ? QString() : embeddedCaption(path);
}

void AppSettings::setCaption(const QString& path, const QString& text) {
  if (path.isEmpty()) {
    return;
  }
  const QString clean = text.trimmed().left(kMaximumCaptionLength);
  if (caption(path) == clean) {
    return;
  }
  pushMarksUndo();
  bool cleared = false;
  if (clean.isEmpty()) {
    m_captions.remove(path);
    if (!embeddedCaption(path).isEmpty()) cleared = addRows(m_clearedEmbedded, kClearedCaption, {rowFor(path)});
  } else {
    m_captions.insert(path, clean);
    cleared = dropRows(m_clearedEmbedded, kClearedCaption, path);
  }
  refreshMarkIdentity(path);
  const bool deferred = m_deferOrganizationSync;
  m_deferOrganizationSync = true;
  persistMarks();
  if (cleared) persistRemovals();
  m_deferOrganizationSync = deferred;
  syncOrganization();
  if (cleared) {
    rebuildRemovalIndex();
    scheduleFingerprints();
  }
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
  step.marks.clearedEmbedded = m_clearedEmbedded;
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
  const bool clearedChanged = m_clearedEmbedded != snapshot.clearedEmbedded;
  m_clearedEmbedded = std::move(snapshot.clearedEmbedded);
  m_deferOrganizationSync = true;
  persistMarks();
  if (clearedChanged) persistRemovals();
  m_deferOrganizationSync = false;
  syncOrganization();
  if (clearedChanged) {
    rebuildRemovalIndex();
    scheduleFingerprints();
  }
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

void AppSettings::setReadEmbeddedMetadata(bool value) {
  if (m_readEmbedded == value) return;
  m_readEmbedded = value;
  setValue(kReadEmbedded, value);
  if (!value) {
    // What was hidden stays hidden; only the layer read from files goes.
    const bool had = !m_embedded.isEmpty() || !m_transientEmbedded.isEmpty();
    m_embedded.clear();
    m_transientEmbedded.clear();
    m_transientOrder.clear();
    rebuildEmbeddedIndex();
    if (had) {
      emit tagsChanged();
      emit marksChanged();
    }
  }
  emit readEmbeddedMetadataChanged();
}

namespace {
AppSettings::EmbeddedValues boundedValues(AppSettings::EmbeddedValues values) {
  if (values.keywords.size() > EmbeddedMetadata::kMaximumKeywords)
    values.keywords = values.keywords.first(EmbeddedMetadata::kMaximumKeywords);
  values.keywords.removeIf([](const QString& keyword) {
    return keyword.isEmpty() || keyword.size() > EmbeddedMetadata::kMaximumKeywordLength;
  });
  if (values.rating < 0 || values.rating > kMaximumRating) values.rating = 0;
  values.caption = values.caption.trimmed().left(kMaximumCaptionLength);
  return values;
}
} // namespace

void AppSettings::setEmbedded(const QList<EmbeddedUpdate>& batch) {
  if (!m_readEmbedded || batch.isEmpty()) return;
  bool tags = false;
  bool marks = false;
  for (const EmbeddedUpdate& update : batch) {
    if (update.path.isEmpty()) continue;
    const int ratingBefore = rating(update.path);
    const QString captionBefore = caption(update.path);
    const QStringList before = m_embeddedEffective.value(update.path);
    const bool wasTransient = m_transientEmbedded.remove(update.path) > 0;
    if (wasTransient) m_transientOrder.removeOne(update.path);
    const EmbeddedValues values = boundedValues(update.values);
    if (values.isEmpty()) {
      m_embedded.remove(update.path);
    } else {
      m_embedded.insert(update.path, values);
    }
    indexEmbedded(update.path);
    tags = tags || wasTransient || m_embeddedEffective.value(update.path) != before;
    marks = marks || rating(update.path) != ratingBefore || caption(update.path) != captionBefore;
  }
  if (tags) emit tagsChanged();
  if (marks) emit marksChanged();
}

void AppSettings::retainEmbedded(const QSet<QString>& livePaths) {
  m_embeddedLive = livePaths;
  bool tags = false;
  bool marks = false;
  for (auto it = m_embedded.begin(); it != m_embedded.end();) {
    if (livePaths.contains(it.key())) {
      ++it;
      continue;
    }
    const QString path = it.key();
    const bool shownValue = !m_ratings.contains(path) && !m_captions.contains(path) &&
                            (it->rating > 0 || !it->caption.isEmpty());
    it = m_embedded.erase(it);
    tags = tags || m_embeddedEffective.contains(path);
    marks = marks || shownValue;
    indexEmbedded(path);
  }
  if (tags) emit tagsChanged();
  if (marks) emit marksChanged();
}

void AppSettings::noteEmbeddedRead(const QString& path, const QStringList& keywords, int stars,
                                   const QString& text) {
  if (!m_readEmbedded || path.isEmpty()) return;
  EmbeddedValues values = boundedValues({keywords, stars, text});
  if (const auto known = m_embedded.constFind(path); known != m_embedded.cend()) {
    values.bytes = known->bytes;
    values.modified = known->modified;
    values.device = known->device;
    values.inode = known->inode;
  }
  if (m_embedded.contains(path) || m_embeddedLive.contains(path)) {
    setEmbedded({{path, values}});
    return;
  }
  const QStringList tagsBefore = tagsForPath(path);
  const int ratingBefore = rating(path);
  const QString captionBefore = caption(path);
  m_transientOrder.removeOne(path);
  if (values.isEmpty()) {
    m_transientEmbedded.remove(path);
  } else {
    m_transientEmbedded.insert(path, values);
    m_transientOrder.append(path);
    while (m_transientOrder.size() > kMaximumTransientEmbedded)
      m_transientEmbedded.remove(m_transientOrder.takeFirst());
  }
  if (tagsForPath(path) != tagsBefore) emit tagsChanged();
  if (rating(path) != ratingBefore || caption(path) != captionBefore) emit marksChanged();
}

namespace {
// A row's file identity. A template, since AlbumEntry is private.
template <typename Entry>
QString fingerprintIdentity(const Entry& entry) {
  return QStringLiteral("%1:%2:%3:%4:%5").arg(entry.path).arg(entry.bytes).arg(entry.modified)
      .arg(entry.device).arg(entry.inode);
}
} // namespace

void AppSettings::scheduleFingerprints() {
  const bool running = m_fingerprintWatcher && m_fingerprintWatcher->isRunning();
  // A row whose identity was read before takes that fingerprint now; the
  // rest go to the worker, a batch at a time.
  bool tagsFilled = false;
  bool removalsFilled = false;
  QList<AlbumEntry> batch;
  QSet<QString> queued;
  for (auto* rows : {&m_tags, &m_removedEmbeddedTags, &m_clearedEmbedded}) {
    for (auto& entries : *rows) {
      for (AlbumEntry& entry : entries) {
        if (!entry.fingerprint.isEmpty() || !entry.resolved || entry.bytes < 0) continue;
        const QString identity = fingerprintIdentity(entry);
        if (const auto known = m_fingerprints.constFind(identity); known != m_fingerprints.cend()) {
          if (!known->isEmpty()) {
            entry.fingerprint = *known;
            (rows == &m_tags ? tagsFilled : removalsFilled) = true;
          }
          continue;
        }
        if (running || batch.size() >= kFingerprintBatch || queued.contains(identity)) continue;
        queued.insert(identity);
        batch.append(entry);
      }
    }
  }
  if (tagsFilled || removalsFilled) {
    const bool deferred = m_deferOrganizationSync;
    m_deferOrganizationSync = true;
    if (tagsFilled) persistTags();
    if (removalsFilled) persistRemovals();
    m_deferOrganizationSync = deferred;
    syncOrganization();
  }
  if (running || batch.isEmpty()) return;
  if (!m_fingerprintWatcher) {
    m_fingerprintWatcher = new QFutureWatcher<QList<std::pair<QString, QByteArray>>>(this);
    connect(m_fingerprintWatcher, &QFutureWatcher<QList<std::pair<QString, QByteArray>>>::finished, this, [this] {
      for (const auto& [identity, fingerprint] : m_fingerprintWatcher->result())
        m_fingerprints.insert(identity, fingerprint);
      // Fills the rows from what was read, and starts the next batch.
      scheduleFingerprints();
    });
  }
  // The worker reads only its own copies. Only a file that still has the
  // identity its row was written with gives the row its fingerprint.
  m_fingerprintWatcher->setFuture(QtConcurrent::run([batch] {
    QList<std::pair<QString, QByteArray>> read;
    read.reserve(batch.size());
    for (const AlbumEntry& entry : batch) {
      const AlbumEntry current = identityFor(entry.path);
      const bool same = current.resolved && current.bytes == entry.bytes && current.modified == entry.modified &&
                        current.device == entry.device && current.inode == entry.inode;
      read.append({fingerprintIdentity(entry), same ? current.fingerprint : QByteArray()});
    }
    return read;
  }));
}

int AppSettings::pendingFingerprints() const {
  int pending = 0;
  for (const auto* rows : {&m_tags, &m_removedEmbeddedTags, &m_clearedEmbedded})
    for (const auto& entries : *rows)
      for (const AlbumEntry& entry : entries)
        pending += entry.fingerprint.isEmpty() && entry.resolved && entry.bytes >= 0 ? 1 : 0;
  return pending;
}

void AppSettings::waitForFingerprints() {
  if (!m_fingerprintWatcher) return;
  // Each finished batch schedules the next.
  do {
    m_fingerprintWatcher->waitForFinished();
    QCoreApplication::processEvents();
  } while (m_fingerprintWatcher->isRunning());
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
  // Installed once, never offered again, even if they are removed later.
  if (result.ok) setValue(kWindowRulesOfferDismissedKey, true);
  emit windowRulesChanged();
  return result.message;
}

const char* const AppSettings::kWindowRulesOfferDismissedKey = "hyprland/windowRulesOfferDismissed";

bool AppSettings::windowRulesOfferDismissed() const {
  return m_settings.value(kWindowRulesOfferDismissedKey, false).toBool();
}

bool AppSettings::windowRulesOfferDismissedOnDisk() {
  const QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                           QStringLiteral("omaroll"));
  return settings.value(kWindowRulesOfferDismissedKey, false).toBool();
}

bool AppSettings::endWindowRulesOfferOnDisk() {
  QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                     QStringLiteral("omaroll"));
  // Sections are parsed lazily; read them all, as the app does, so a format
  // error anywhere is known before anything is written.
  (void)settings.allKeys();
  if (settings.status() == QSettings::FormatError) return false;
  settings.setValue(kWindowRulesOfferDismissedKey, true);
  settings.sync();
  return settings.status() == QSettings::NoError;
}

void AppSettings::dismissWindowRulesOffer() {
  if (windowRulesOfferDismissed()) return;
  setValue(kWindowRulesOfferDismissedKey, true);
  emit windowRulesChanged();
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

void AppSettings::copyDiagnostics(const QString &theme, qreal scale) {
  if (m_copyingDiagnostics) return;
  m_copyingDiagnostics = true;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
    QGuiApplication::clipboard()->setText(watcher->result());
    watcher->deleteLater();
    m_copyingDiagnostics = false;
    emit diagnosticsCopied();
  });
  watcher->setFuture(QtConcurrent::run([platform = QGuiApplication::platformName(), theme, scale] {
    return Diagnostics::report(Diagnostics::Sources::system(), platform, theme, scale);
  }));
}
