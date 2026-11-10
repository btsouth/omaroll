#include "viewer/ViewerSort.h"

#include "edit/JpegExif.h"
#include "library/CaptureRecord.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <utility>
#include <vector>

namespace ViewerSort {

QDateTime dateTaken(const QString& path) {
  const QFileInfo info(path);
  CaptureRecord::Kind kind{};
  QDateTime named;
  if (CaptureScanner::classifyByName(info.fileName(), kind, named) && named.isValid()) return named;
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
  return taken.isValid() ? taken : info.lastModified();
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
