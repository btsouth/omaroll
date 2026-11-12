#include "viewer/ViewerSort.h"

#include "edit/JpegExif.h"
#include "library/CaptureRecord.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

#include <algorithm>
#include <utility>
#include <vector>

namespace ViewerSort {

std::atomic<quint64> readCount{0};

namespace {
// What the headers said, per path, until the file's size or modified time
// changes: a folder relisted after a move or a Trash is not read again.
struct Remembered {
  qint64 size = -1;
  qint64 modified = 0;
  QDateTime taken;
};
QMutex rememberedLock;
QHash<QString, Remembered> remembered;
constexpr qsizetype kRememberedLimit = 200000;
} // namespace

QDateTime dateTaken(const QString& path) {
  const QFileInfo info(path);
  CaptureRecord::Kind kind{};
  QDateTime named;
  if (CaptureScanner::classifyByName(info.fileName(), kind, named) && named.isValid()) return named;
  const qint64 size = info.size();
  const qint64 modified = info.lastModified().toMSecsSinceEpoch();
  {
    const QMutexLocker locker(&rememberedLock);
    const auto known = remembered.constFind(path);
    if (known != remembered.constEnd() && known->size == size && known->modified == modified) {
      return known->taken.isValid() ? known->taken : info.lastModified();
    }
  }
  const QDateTime read = readDateTaken(path);
  {
    const QMutexLocker locker(&rememberedLock);
    if (remembered.size() >= kRememberedLimit) remembered.clear();
    remembered.insert(path, {size, modified, read});
  }
  return read.isValid() ? read : info.lastModified();
}

QDateTime readDateTaken(const QString& path) {
  ++readCount;
  QDateTime taken;
  if (CameraRaw::isRawFile(path)) {
    taken = CameraRaw::readMetadata(path).taken;
  } else {
    const QString suffix = CaptureScanner::mediaSuffix(path);
    if (suffix == u"jpg" || suffix == u"jpeg") {
      QFile file(path);
      // EXIF sits in the first segment, within its first 64 KiB.
      if (file.open(QIODevice::ReadOnly)) taken = JpegExif::dateTaken(file.read(128 * 1024));
    }
  }
  return taken;
}

QStringList sorted(const QStringList& names, const QString& order, bool reversed) {
  QStringList result = names;
  if (order == u"taken" || order == u"modified" || order == u"size") {
    std::vector<std::pair<qint64, QString>> keyed;
    keyed.reserve(result.size());
    for (const QString& path : std::as_const(result)) {
      const QFileInfo info(path);
      qint64 key = 0;
      if (order == u"size") key = info.size();
      else if (order == u"modified") key = info.lastModified().toMSecsSinceEpoch();
      else key = dateTaken(path).toMSecsSinceEpoch();
      keyed.emplace_back(key, path);
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& left, const auto& right) { return left.first < right.first; });
    result.clear();
    for (auto& [key, path] : keyed) result.append(std::move(path));
  }
  if (reversed) std::reverse(result.begin(), result.end());
  return result;
}

} // namespace ViewerSort
