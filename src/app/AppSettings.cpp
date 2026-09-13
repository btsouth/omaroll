#include "app/AppSettings.h"

#include "library/CaptureRecord.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVariantMap>

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
constexpr auto kImagePrimaryAction = "actions/imagePrimary";
constexpr auto kVideoPrimaryAction = "actions/videoPrimary";
constexpr auto kThumbnailCacheMb = "cache/maximumMb";
constexpr auto kTileWidth = "view/tileWidth";
constexpr int kMinimumTileWidth = 160;
constexpr int kMaximumTileWidth = 480;
constexpr auto kSlideshowVideos = "slideshow/includeVideos";
constexpr auto kAlbums = "library/albums";
constexpr auto kTags = "library/tags";
constexpr auto kSmartCollections = "library/smartCollections";
constexpr auto kLastVisit = "library/lastVisit";
constexpr auto kVideoVolume = "playback/volume";
constexpr auto kVideoMuted = "playback/muted";
constexpr auto kVideoPositions = "playback/positions";
// Resume spots beyond a few minutes are the useful ones; too many entries and
// the file grows for no benefit.
constexpr int kMaximumResumeEntries = 500;
constexpr qint64 kMinimumResumeMs = 5000;
constexpr auto kOrganizationFormat = "omaroll.organization";

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

} // namespace

AppSettings::AlbumEntry AppSettings::identityFor(const QString& path) {
  AppSettings::AlbumEntry entry;
  entry.path = path;

  const QFileInfo info(path);
  if (!info.isFile() || !info.isReadable()) {
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
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArray::number(entry.bytes));
  hash.addData(file.read(chunkSize));
  if (entry.bytes > chunkSize) {
    file.seek(qMax<qint64>(0, entry.bytes - chunkSize));
    hash.addData(file.read(chunkSize));
  }
  entry.fingerprint = hash.result();
  entry.resolved = true;
  return entry;
}

AppSettings::AppSettings(QObject* parent)
    : QObject(parent), m_settings(QSettings::IniFormat, QSettings::UserScope,
                                  QStringLiteral("omaroll"), QStringLiteral("omaroll")) {
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
  const QString imageAction =
      m_settings.value(kImagePrimaryAction, QStringLiteral("matte")).toString();
  if (QStringList {QStringLiteral("matte"), QStringLiteral("view"), QStringLiteral("edit")}
          .contains(imageAction)) {
    m_imagePrimaryAction = imageAction;
  }
  const QString videoAction =
      m_settings.value(kVideoPrimaryAction, QStringLiteral("trim")).toString();
  if (QStringList {QStringLiteral("trim"), QStringLiteral("play")}.contains(videoAction)) {
    m_videoPrimaryAction = videoAction;
  }
  m_thumbnailCacheMb = qBound(64, m_settings.value(kThumbnailCacheMb, 256).toInt(), 1024);
  m_tileWidth =
      qBound(kMinimumTileWidth, m_settings.value(kTileWidth, 240).toInt(), kMaximumTileWidth);
  m_slideshowVideos = m_settings.value(kSlideshowVideos, false).toBool();
  m_videoVolume = qBound(0.0, m_settings.value(kVideoVolume, 0.8).toDouble(), 1.0);
  m_videoMuted = m_settings.value(kVideoMuted, false).toBool();
  const QVariantMap storedPositions = m_settings.value(kVideoPositions).toMap();
  for (auto it = storedPositions.cbegin(); it != storedPositions.cend(); ++it) {
    const qint64 position = it.value().toLongLong();
    if (!it.key().isEmpty() && position >= kMinimumResumeMs) {
      m_videoPositions.insert(it.key(), position);
      m_videoRecency.append(it.key());
    }
  }
  const QVariantMap storedAlbums = m_settings.value(kAlbums).toMap();
  const auto restoreCollections = [this](const QVariantMap& storedCollections,
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

  m_previousVisit = m_settings.value(kLastVisit).toString();
  m_settings.setValue(kLastVisit, QDateTime::currentDateTime().toString(Qt::ISODate));
}

void AppSettings::setShowHidden(bool value) {
  if (m_showHidden == value) {
    return;
  }
  m_showHidden = value;
  m_settings.setValue(kShowHidden, value);
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
  m_settings.setValue(kSortMode, bounded);
  emit sortModeChanged();
}

void AppSettings::setKindFilter(int value) {
  const int bounded = qBound(-1, value, 5);
  if (m_kindFilter == bounded) {
    return;
  }
  m_kindFilter = bounded;
  m_settings.setValue(kKindFilter, bounded);
  emit kindFilterChanged();
}

void AppSettings::setScanDownloads(bool value) {
  if (m_scanDownloads == value) {
    return;
  }
  m_scanDownloads = value;
  m_settings.setValue(kScanDownloads, value);
  emit scanDownloadsChanged();
}

void AppSettings::setRecursionDepth(int value) {
  const int bounded = qBound(1, value, 8);
  if (m_recursionDepth == bounded) {
    return;
  }
  m_recursionDepth = bounded;
  m_settings.setValue(kRecursionDepth, bounded);
  emit recursionDepthChanged();
}

bool AppSettings::addLibraryFolder(const QUrl& folder) {
  const QString normalized = normalizedFolder(folder.toLocalFile(), true);
  if (normalized.isEmpty() || m_libraryFolders.contains(normalized)) {
    return false;
  }
  m_libraryFolders.append(normalized);
  m_settings.setValue(kLibraryFolders, m_libraryFolders);
  emit libraryFoldersChanged();
  return true;
}

void AppSettings::removeLibraryFolder(const QString& folder) {
  const QString normalized = normalizedFolder(folder, false);
  if (normalized.isEmpty() || !m_libraryFolders.removeOne(normalized)) {
    return;
  }
  m_settings.setValue(kLibraryFolders, m_libraryFolders);
  emit libraryFoldersChanged();
}

void AppSettings::setImagePrimaryAction(const QString& action) {
  static const QStringList allowed = {QStringLiteral("matte"), QStringLiteral("view"),
                                      QStringLiteral("edit")};
  if (!allowed.contains(action) || m_imagePrimaryAction == action) {
    return;
  }
  m_imagePrimaryAction = action;
  m_settings.setValue(kImagePrimaryAction, action);
  emit imagePrimaryActionChanged();
}

void AppSettings::setVideoPrimaryAction(const QString& action) {
  static const QStringList allowed = {QStringLiteral("trim"), QStringLiteral("play")};
  if (!allowed.contains(action) || m_videoPrimaryAction == action) {
    return;
  }
  m_videoPrimaryAction = action;
  m_settings.setValue(kVideoPrimaryAction, action);
  emit videoPrimaryActionChanged();
}

void AppSettings::setTileWidth(int width) {
  const int bounded = qBound(kMinimumTileWidth, width, kMaximumTileWidth);
  if (m_tileWidth == bounded) {
    return;
  }
  m_tileWidth = bounded;
  m_settings.setValue(kTileWidth, bounded);
  emit tileWidthChanged();
}

void AppSettings::setThumbnailCacheMb(int megabytes) {
  const int bounded = qBound(64, megabytes, 1024);
  if (m_thumbnailCacheMb == bounded) {
    return;
  }
  m_thumbnailCacheMb = bounded;
  m_settings.setValue(kThumbnailCacheMb, bounded);
  emit thumbnailCacheMbChanged();
}

void AppSettings::setSlideshowVideos(bool value) {
  if (m_slideshowVideos == value) {
    return;
  }
  m_slideshowVideos = value;
  m_settings.setValue(kSlideshowVideos, value);
  emit slideshowVideosChanged();
}

qint64 AppSettings::videoPosition(const QString& path) const {
  return m_videoPositions.value(path, 0);
}

void AppSettings::setVideoPosition(const QString& path, qint64 milliseconds) {
  if (path.isEmpty()) {
    return;
  }
  if (milliseconds < kMinimumResumeMs) {
    clearVideoPosition(path);
    return;
  }
  m_videoPositions.insert(path, milliseconds);
  m_videoRecency.removeAll(path);
  m_videoRecency.prepend(path);
  while (m_videoRecency.size() > kMaximumResumeEntries) {
    m_videoPositions.remove(m_videoRecency.takeLast());
  }
  QVariantMap stored;
  for (auto it = m_videoPositions.cbegin(); it != m_videoPositions.cend(); ++it) {
    stored.insert(it.key(), it.value());
  }
  m_settings.setValue(kVideoPositions, stored);
}

void AppSettings::clearVideoPosition(const QString& path) {
  if (m_videoPositions.remove(path) == 0 && !m_videoRecency.contains(path)) {
    return;
  }
  m_videoRecency.removeAll(path);
  QVariantMap stored;
  for (auto it = m_videoPositions.cbegin(); it != m_videoPositions.cend(); ++it) {
    stored.insert(it.key(), it.value());
  }
  m_settings.setValue(kVideoPositions, stored);
}

void AppSettings::setVideoVolume(qreal value) {
  const qreal bounded = qBound(0.0, value, 1.0);
  if (qFuzzyCompare(m_videoVolume, bounded)) {
    return;
  }
  m_videoVolume = bounded;
  m_settings.setValue(kVideoVolume, bounded);
  emit videoVolumeChanged();
}

void AppSettings::setVideoMuted(bool value) {
  if (m_videoMuted == value) {
    return;
  }
  m_videoMuted = value;
  m_settings.setValue(kVideoMuted, value);
  emit videoMutedChanged();
}

void AppSettings::relocatePath(const QString& oldPath, const QString& newPath) {
  if (oldPath.isEmpty() || newPath.isEmpty() || oldPath == newPath) {
    return;
  }

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
  return true;
}

void AppSettings::deleteAlbum(const QString& name) {
  if (m_albums.remove(name) == 0) {
    return;
  }
  persistAlbums();
  emit albumsChanged();
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
  persistAlbums();
  emit albumsChanged();
  return true;
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
  return changed;
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
  return true;
}

void AppSettings::deleteTag(const QString& name) {
  int removed = m_tags.remove(name);
  for (const QString& existing : m_tags.keys()) {
    if (tagIsUnder(existing, name)) {
      removed += m_tags.remove(existing);
    }
  }
  if (removed == 0) {
    return;
  }
  persistTags();
  emit tagsChanged();
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
  persistTags();
  if (viewsChanged) {
    persistSmartCollections();
    emit smartCollectionsChanged();
  }
  emit tagsChanged();
  return true;
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
  return changed;
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
  return true;
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
  struct Candidate {
    QString path;
    qint64 bytes = 0;
    qint64 modified = 0;
    quint64 device = 0;
    quint64 inode = 0;
  };
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
    candidates.append({record.path, record.bytes, record.modified, device, inode});
  }

  bool persistedChange = false;
  bool resolutionChange = false;
  for (auto collection = collections.begin(); collection != collections.end(); ++collection) {
    for (AlbumEntry& entry : collection.value()) {
      const bool wasResolved = entry.resolved;
      entry.resolved = false;

      if (entry.fingerprint.isEmpty()) {
        const AlbumEntry upgraded = identityFor(entry.path);
        if (upgraded.resolved) {
          entry = upgraded;
          persistedChange = true;
        }
      } else {
        for (const Candidate& candidate : std::as_const(candidates)) {
          if (entry.device != 0 && entry.inode != 0 && candidate.inode == entry.inode &&
              candidate.bytes == entry.bytes &&
              (candidate.device == entry.device || candidate.modified == entry.modified)) {
            if (entry.path != candidate.path) {
              entry.path = candidate.path;
              persistedChange = true;
            }
            entry.resolved = true;
            break;
          }
        }
      }

      if (!entry.resolved && !entry.fingerprint.isEmpty()) {
        QList<Candidate> possible;
        for (const Candidate& candidate : std::as_const(candidates)) {
          if (candidate.bytes == entry.bytes && candidate.modified == entry.modified) {
            possible.append(candidate);
          }
        }
        if (possible.isEmpty()) {
          for (const Candidate& candidate : std::as_const(candidates)) {
            if (candidate.bytes == entry.bytes) {
              possible.append(candidate);
            }
          }
        }
        // A pathological directory full of equally sized files should not
        // stall the UI to repair one uncertain album entry.
        if (possible.size() > 128) {
          resolutionChange = resolutionChange || wasResolved;
          continue;
        }
        QString matchedPath;
        int matches = 0;
        for (const Candidate& candidate : std::as_const(possible)) {
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
          entry = repaired;
          persistedChange = true;
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

QVariantMap AppSettings::exportOrganization(const QString& path) const {
  if (path.isEmpty()) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("Choose a file to write the backup to")}};
  }

  const auto encodeEntries = [](const QMap<QString, QList<AlbumEntry>>& collections) {
    QJsonObject out;
    for (auto it = collections.cbegin(); it != collections.cend(); ++it) {
      QJsonArray rows;
      for (const AlbumEntry& entry : it.value()) {
        QJsonObject row;
        row.insert(QStringLiteral("path"), entry.path);
        row.insert(QStringLiteral("bytes"), entry.bytes);
        row.insert(QStringLiteral("modified"), entry.modified);
        // SHA-256 is binary; base64 keeps it exact through JSON.
        row.insert(QStringLiteral("fingerprint"),
                   QString::fromLatin1(entry.fingerprint.toBase64()));
        row.insert(QStringLiteral("device"), QString::number(entry.device));
        row.insert(QStringLiteral("inode"), QString::number(entry.inode));
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
  root.insert(QStringLiteral("albums"), encodeEntries(m_albums));
  root.insert(QStringLiteral("tags"), encodeEntries(m_tags));

  QStringList favorites(m_favorites.cbegin(), m_favorites.cend());
  favorites.sort();
  QJsonArray favoriteArray;
  for (const QString& value : std::as_const(favorites)) {
    favoriteArray.append(value);
  }
  root.insert(QStringLiteral("favorites"), favoriteArray);

  QStringList hidden(m_hidden.cbegin(), m_hidden.cend());
  hidden.sort();
  QJsonArray hiddenArray;
  for (const QString& value : std::as_const(hidden)) {
    hiddenArray.append(value);
  }
  root.insert(QStringLiteral("hidden"), hiddenArray);

  QJsonObject ratings;
  for (auto it = m_ratings.cbegin(); it != m_ratings.cend(); ++it) {
    ratings.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("ratings"), ratings);

  QJsonObject captions;
  for (auto it = m_captions.cbegin(); it != m_captions.cend(); ++it) {
    captions.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("captions"), captions);

  QVariantMap smart;
  for (auto it = m_smartCollections.cbegin(); it != m_smartCollections.cend(); ++it) {
    smart.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("smartCollections"), QJsonObject::fromVariantMap(smart));

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"),
             QStringLiteral("Could not write %1").arg(QFileInfo(path).fileName())}};
  }
  const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
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
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("message"), QStringLiteral("That is not a valid backup file")}};
  }
  const QJsonObject root = document.object();
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
  const auto decodeEntries = [](const QJsonObject& collections,
                                QString (*normalize)(const QString&)) {
    QMap<QString, QList<AlbumEntry>> decoded;
    for (auto it = collections.begin(); it != collections.end(); ++it) {
      const QString name = normalize(it.key());
      if (name.isEmpty() || !it.value().isArray()) {
        continue;
      }
      QList<AlbumEntry> entries;
      for (const QJsonValue& value : it.value().toArray()) {
        const QJsonObject row = value.toObject();
        AlbumEntry entry;
        entry.path = row.value(QStringLiteral("path")).toString();
        if (entry.path.isEmpty()) {
          continue;
        }
        entry.bytes = row.value(QStringLiteral("bytes")).toVariant().toLongLong();
        entry.modified = row.value(QStringLiteral("modified")).toVariant().toLongLong();
        entry.fingerprint =
            QByteArray::fromBase64(row.value(QStringLiteral("fingerprint")).toString().toLatin1());
        entry.device = row.value(QStringLiteral("device")).toString().toULongLong();
        entry.inode = row.value(QStringLiteral("inode")).toString().toULongLong();
        entries.append(entry);
      }
      if (decoded.contains(name)) {
        decoded[name].append(entries);
      } else {
        decoded.insert(name, entries);
      }
    }
    return decoded;
  };

  QMap<QString, QList<AlbumEntry>> albums =
      decodeEntries(root.value(QStringLiteral("albums")).toObject(), normalizedAlbumName);
  QMap<QString, QList<AlbumEntry>> tags =
      decodeEntries(root.value(QStringLiteral("tags")).toObject(), normalizedTagName);
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
    if (!value.toString().isEmpty()) {
      favorites.insert(value.toString());
    }
  }
  QSet<QString> hidden;
  for (const QJsonValue& value : root.value(QStringLiteral("hidden")).toArray()) {
    if (!value.toString().isEmpty()) {
      hidden.insert(value.toString());
    }
  }
  QHash<QString, int> ratings;
  const QJsonObject ratingObject = root.value(QStringLiteral("ratings")).toObject();
  for (auto it = ratingObject.begin(); it != ratingObject.end(); ++it) {
    const int stars = it.value().toInt();
    if (!it.key().isEmpty() && stars >= 1 && stars <= kMaximumRating) {
      ratings.insert(it.key(), stars);
    }
  }
  QHash<QString, QString> captions;
  const QJsonObject captionObject = root.value(QStringLiteral("captions")).toObject();
  for (auto it = captionObject.begin(); it != captionObject.end(); ++it) {
    const QString text = it.value().toString().left(kMaximumCaptionLength);
    if (!it.key().isEmpty() && !text.isEmpty()) {
      captions.insert(it.key(), text);
    }
  }
  QMap<QString, QVariantMap> smartCollections;
  const QJsonObject smartObject = root.value(QStringLiteral("smartCollections")).toObject();
  for (auto it = smartObject.begin(); it != smartObject.end(); ++it) {
    if (it.value().isObject()) {
      smartCollections.insert(it.key(), it.value().toObject().toVariantMap());
    }
  }

  // An entry whose file is present right now shows up immediately; the next
  // scan repairs anything that moved. The stored identity is kept so a move
  // can still be matched by inode or fingerprint.
  const auto resolvePresent = [](QMap<QString, QList<AlbumEntry>>& collections) {
    for (auto it = collections.begin(); it != collections.end(); ++it) {
      for (AlbumEntry& entry : *it) {
        const QFileInfo info(entry.path);
        entry.resolved = info.isFile() && info.isReadable();
      }
    }
  };
  resolvePresent(albums);
  resolvePresent(tags);

  m_albums = std::move(albums);
  m_tags = std::move(tags);
  m_smartCollections = std::move(smartCollections);
  m_favorites = std::move(favorites);
  m_hidden = std::move(hidden);
  m_ratings = std::move(ratings);
  m_captions = std::move(captions);

  persistAlbums();
  persistTags();
  persistSmartCollections();
  persistMarks();
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
  m_settings.setValue(kAlbums, albums);
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
  m_settings.setValue(kTags, tags);
}

void AppSettings::persistSmartCollections() {
  QVariantMap stored;
  for (auto it = m_smartCollections.cbegin(); it != m_smartCollections.cend(); ++it) {
    stored.insert(it.key(), it.value());
  }
  m_settings.setValue(kSmartCollections, stored);
}

bool AppSettings::isFavorite(const QString& path) const { return m_favorites.contains(path); }
bool AppSettings::isHidden(const QString& path) const { return m_hidden.contains(path); }

void AppSettings::toggleFavorite(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
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
  mark(m_favorites, paths, on);
  for (const QString& path : paths) {
    refreshMarkIdentity(path);
  }
  persistMarks();
  emit marksChanged();
}

void AppSettings::setHidden(const QStringList& paths, bool on) {
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

void AppSettings::forgetMarks(const QStringList& paths) {
  bool changed = false;
  for (const QString& path : paths) {
    changed = m_favorites.remove(path) || changed;
    changed = m_hidden.remove(path) || changed;
    changed = m_ratings.remove(path) > 0 || changed;
    changed = m_captions.remove(path) > 0 || changed;
    changed = m_markIdentities.remove(path) > 0 || changed;
  }
  if (changed) {
    persistMarks();
    persistMarkIdentities();
  }
}

void AppSettings::persistMarks() {
  m_settings.setValue(kFavorites, QStringList(m_favorites.begin(), m_favorites.end()));
  m_settings.setValue(kHidden, QStringList(m_hidden.begin(), m_hidden.end()));
  QStringList ratings;
  ratings.reserve(m_ratings.size());
  for (auto it = m_ratings.cbegin(); it != m_ratings.cend(); ++it) {
    ratings.append(QString::number(it.value()) + QLatin1Char(':') + it.key());
  }
  ratings.sort();
  m_settings.setValue(kRatings, ratings);
  QVariantMap captions;
  for (auto it = m_captions.cbegin(); it != m_captions.cend(); ++it) {
    captions.insert(it.key(), it.value());
  }
  m_settings.setValue(kCaptions, captions);
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
  m_settings.setValue(kMarkIdentities, stored);
}

void AppSettings::refreshMarkIdentity(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  if (!pathHasMark(path)) {
    if (m_markIdentities.remove(path) > 0) {
      persistMarkIdentities();
    }
    return;
  }
  if (m_markIdentities.contains(path)) {
    return;
  }
  const AlbumEntry identity = identityFor(path);
  if (identity.resolved) {
    m_markIdentities.insert(path, identity);
    persistMarkIdentities();
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
    candidates.append({record.path, record.bytes, record.modified, device, inode});
    live.insert(record.path);
  }

  // Decide every move before applying any, so two walking marks cannot chase
  // each other's path mid-loop.
  QList<QPair<QString, QString>> moves;
  for (auto it = m_markIdentities.cbegin(); it != m_markIdentities.cend(); ++it) {
    const QString& oldPath = it.key();
    if (live.contains(oldPath)) {
      continue;
    }
    const AlbumEntry& identity = it.value();

    QString match;
    int found = 0;
    if (identity.device != 0 && identity.inode != 0) {
      for (const Candidate& candidate : std::as_const(candidates)) {
        if (candidate.inode == identity.inode && candidate.bytes == identity.bytes &&
            (candidate.device == identity.device || candidate.modified == identity.modified)) {
          match = candidate.path;
          if (++found > 1) {
            break;
          }
        }
      }
    }
    if (found != 1 && !identity.fingerprint.isEmpty()) {
      QList<Candidate> possible;
      for (const Candidate& candidate : std::as_const(candidates)) {
        if (candidate.bytes == identity.bytes && candidate.modified == identity.modified) {
          possible.append(candidate);
        }
      }
      if (possible.isEmpty()) {
        for (const Candidate& candidate : std::as_const(candidates)) {
          if (candidate.bytes == identity.bytes) {
            possible.append(candidate);
          }
        }
      }
      // Same guard as albums and tags: a directory full of equal-sized files
      // must not stall the UI fingerprinting everything.
      if (possible.size() <= 128) {
        match.clear();
        found = 0;
        for (const Candidate& candidate : std::as_const(possible)) {
          if (identityFor(candidate.path).fingerprint == identity.fingerprint) {
            match = candidate.path;
            if (++found > 1) {
              break;
            }
          }
        }
      }
    }

    if (found == 1 && !match.isEmpty()) {
      moves.append({oldPath, match});
    }
  }

  if (moves.isEmpty()) {
    return;
  }

  for (const auto& move : std::as_const(moves)) {
    const QString& from = move.first;
    const QString& to = move.second;
    if (m_favorites.remove(from)) {
      m_favorites.insert(to);
    }
    if (m_hidden.remove(from)) {
      m_hidden.insert(to);
    }
    const auto rating = m_ratings.find(from);
    if (rating != m_ratings.end()) {
      m_ratings[to] = qMax(m_ratings.value(to, 0), rating.value());
      m_ratings.erase(rating);
    }
    const auto caption = m_captions.find(from);
    if (caption != m_captions.end()) {
      if (!m_captions.contains(to)) {
        m_captions.insert(to, caption.value());
      }
      m_captions.erase(caption);
    }
    m_markIdentities.remove(from);
    const AlbumEntry identity = identityFor(to);
    if (identity.resolved) {
      m_markIdentities.insert(to, identity);
    }
  }

  persistMarks();
  persistMarkIdentities();
  emit marksChanged();
}
