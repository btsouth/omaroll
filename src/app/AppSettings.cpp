#include "app/AppSettings.h"

#include "library/CaptureRecord.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariantMap>

#include <sys/stat.h>

namespace {
constexpr auto kFavorites = "library/favorites";
constexpr auto kHidden = "library/hidden";
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

QString normalizedFolder(const QString& path, bool mustExist) {
  const QFileInfo info(path);
  if (mustExist && (!info.isDir() || !info.isReadable())) {
    return {};
  }
  const QString normalized = info.exists() ? info.canonicalFilePath()
                                           : QDir::cleanPath(info.absoluteFilePath());
  const QString home = QFileInfo(QDir::homePath()).canonicalFilePath();
  return normalized.isEmpty() || normalized == home || normalized == QDir::rootPath()
             ? QString()
             : normalized;
}

QString normalizedAlbumName(const QString& name) {
  const QString normalized = name.simplified().left(60);
  return normalized.contains(QLatin1Char('/')) ? QString() : normalized;
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
    : QObject(parent),
      m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                 QStringLiteral("omaroll")) {
  const QStringList favorites = m_settings.value(kFavorites).toStringList();
  m_favorites = QSet<QString>(favorites.begin(), favorites.end());

  const QStringList hidden = m_settings.value(kHidden).toStringList();
  m_hidden = QSet<QString>(hidden.begin(), hidden.end());

  m_showHidden = m_settings.value(kShowHidden, false).toBool();
  m_sortMode = qBound(0, m_settings.value(kSortMode, 0).toInt(), 4);
  m_kindFilter = qBound(-1, m_settings.value(kKindFilter, -1).toInt(), 4);
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
  if (QStringList{QStringLiteral("matte"), QStringLiteral("view"), QStringLiteral("edit")}
          .contains(imageAction)) {
    m_imagePrimaryAction = imageAction;
  }
  const QString videoAction =
      m_settings.value(kVideoPrimaryAction, QStringLiteral("trim")).toString();
  if (QStringList{QStringLiteral("trim"), QStringLiteral("play")}.contains(videoAction)) {
    m_videoPrimaryAction = videoAction;
  }
  m_thumbnailCacheMb =
      qBound(64, m_settings.value(kThumbnailCacheMb, 256).toInt(), 1024);
  m_tileWidth = qBound(kMinimumTileWidth, m_settings.value(kTileWidth, 240).toInt(),
                       kMaximumTileWidth);
  m_slideshowVideos = m_settings.value(kSlideshowVideos, false).toBool();
  const QVariantMap storedAlbums = m_settings.value(kAlbums).toMap();
  for (auto it = storedAlbums.cbegin(); it != storedAlbums.cend(); ++it) {
    const QString name = normalizedAlbumName(it.key());
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
        if (entry.device != 0 && entry.inode != 0 &&
            ::stat(QFile::encodeName(entry.path).constData(), &status) == 0 &&
            entry.device == status.st_dev && entry.inode == status.st_ino) {
          entry.resolved = true;
        }
        entries.append(entry);
      }
    }
    m_albums.insert(name, entries);
  }
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
  // Five modes; an ini edited by hand or by an older version must not leave
  // the sort menu reading "undefined".
  const int bounded = qBound(0, value, 4);
  if (m_sortMode == bounded) {
    return;
  }
  m_sortMode = bounded;
  m_settings.setValue(kSortMode, bounded);
  emit sortModeChanged();
}

void AppSettings::setKindFilter(int value) {
  const int bounded = qBound(-1, value, 4);
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

bool AppSettings::addToAlbum(const QString& name, const QStringList& paths) {
  auto it = m_albums.find(name);
  if (it == m_albums.end()) {
    return false;
  }
  bool changed = false;
  for (const QString& path : paths) {
    bool present = false;
    for (const AlbumEntry& entry : std::as_const(*it)) {
      if (entry.path == path) {
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

void AppSettings::reconcileAlbums(const QList<CaptureRecord>& records) {
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
    struct stat status {};
    quint64 device = 0;
    quint64 inode = 0;
    if (::stat(QFile::encodeName(record.path).constData(), &status) == 0) {
      device = status.st_dev;
      inode = status.st_ino;
    }
    candidates.append({record.path, record.bytes, record.modified, device, inode});
  }

  bool persistedChange = false;
  bool resolutionChange = false;
  for (auto album = m_albums.begin(); album != m_albums.end(); ++album) {
    for (AlbumEntry& entry : album.value()) {
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
          if (entry.device != 0 && entry.inode != 0 && candidate.device == entry.device &&
              candidate.inode == entry.inode) {
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
  if (persistedChange) {
    persistAlbums();
  }
  if (persistedChange || resolutionChange) {
    emit albumsChanged();
  }
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

bool AppSettings::isFavorite(const QString& path) const { return m_favorites.contains(path); }
bool AppSettings::isHidden(const QString& path) const { return m_hidden.contains(path); }

void AppSettings::toggleFavorite(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  if (!m_favorites.remove(path)) {
    m_favorites.insert(path);
  }
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
  persistMarks();
  emit marksChanged();
}

void AppSettings::setHidden(const QStringList& paths, bool on) {
  mark(m_hidden, paths, on);
  persistMarks();
  emit marksChanged();
}

QStringList AppSettings::markedPaths() const {
  QSet<QString> all = m_favorites;
  all.unite(m_hidden);
  return QStringList(all.begin(), all.end());
}

void AppSettings::forgetMarks(const QStringList& paths) {
  bool changed = false;
  for (const QString& path : paths) {
    changed = m_favorites.remove(path) || changed;
    changed = m_hidden.remove(path) || changed;
  }
  if (changed) {
    persistMarks();
  }
}

void AppSettings::persistMarks() {
  m_settings.setValue(kFavorites, QStringList(m_favorites.begin(), m_favorites.end()));
  m_settings.setValue(kHidden, QStringList(m_hidden.begin(), m_hidden.end()));
}
