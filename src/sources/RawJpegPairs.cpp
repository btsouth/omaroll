#include "sources/RawJpegPairs.h"

#include "sources/CameraRaw.h"

#include <QFileInfo>
#include <QSet>

namespace RawJpegPairs {
Companions find(const QStringList& paths) {
  struct Candidates {
    QString raw;
    QString jpeg;
    int raws = 0;
    int jpegs = 0;
  };
  QHash<QString, Candidates> candidates;
  QSet<QString> seen;
  for (const QString& path : paths) {
    if (seen.contains(path)) continue;
    seen.insert(path);
    const QFileInfo file(path);
    const QString suffix = file.suffix().toLower();
    const bool raw = CameraRaw::isRaw(suffix);
    if (!raw && suffix != u"jpg" && suffix != u"jpeg") continue;
    const QString key = file.absolutePath() + QLatin1Char('/') + file.completeBaseName().toCaseFolded();
    Candidates& group = candidates[key];
    if (raw) {
      group.raw = path;
      ++group.raws;
    } else {
      group.jpeg = path;
      ++group.jpegs;
    }
  }
  Companions result;
  for (const Candidates& group : candidates) {
    if (group.raws == 1 && group.jpegs == 1) {
      result.insert(group.raw, group.jpeg);
      result.insert(group.jpeg, group.raw);
    }
  }
  return result;
}

void assign(QList<CaptureRecord>& records) {
  QStringList paths;
  paths.reserve(records.size());
  for (const CaptureRecord& record : records) paths.append(record.path);
  const Companions companions = find(paths);
  for (CaptureRecord& record : records) record.companionPath = companions.value(record.path);
}

QStringList grouped(const QStringList& paths, const Companions& companions, const QString& preferred) {
  QStringList result;
  QSet<QString> seen;
  const QSet<QString> available(paths.begin(), paths.end());
  result.reserve(paths.size());
  for (const QString& path : paths) {
    if (seen.contains(path)) continue;
    const QString companion = companions.value(path);
    if (companion.isEmpty() || !available.contains(companion)) {
      result.append(path);
      seen.insert(path);
      continue;
    }
    const QString representative = (path == preferred || companion == preferred)
                                       ? preferred
                                       : (CameraRaw::isRawFile(path) ? path : companion);
    result.append(representative);
    seen.insert(path);
    seen.insert(companion);
  }
  return result;
}
} // namespace RawJpegPairs
