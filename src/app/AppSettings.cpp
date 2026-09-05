#include "app/AppSettings.h"

#include "library/CaptureRecord.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariantMap>

#include <sys/stat.h>

namespace {
constexpr auto kFavorites = "library/favorites";
constexpr auto kHidden = "library/hidden";
// One "stars:path" entry per rated file, so the ini stays readable by hand.
constexpr auto kRatings = "library/ratings";
constexpr int kMaximumRating = 5;
constexpr auto kCaptions = "library/captions";
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
  }
  if (changed) {
    persistMarks();
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
