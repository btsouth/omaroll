#include "app/OpenRequest.h"

#include "sources/CaptureScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

QString OpenRequest::skippedNote(const QStringList& names) {
  if (names.isEmpty()) return {};
  constexpr int kNamed = 5;
  if (names.size() <= kNamed + 1) return QStringLiteral("Skipped: %1").arg(names.join(QStringLiteral(", ")));
  return QStringLiteral("Skipped: %1 and %2 more")
      .arg(names.first(kNamed).join(QStringLiteral(", ")))
      .arg(names.size() - kNamed);
}

OpenRequest OpenRequest::fromPaths(const QStringList& paths) {
  QList<QFileInfo> infos;
  infos.reserve(paths.size());
  for (const QString& path : paths) infos.append(QFileInfo(path));
  return fromInfos(infos);
}

OpenRequest OpenRequest::fromInfos(const QList<QFileInfo>& infos) {
  OpenRequest result;
  QSet<QString> seen;
  QStringList failures;
  const qsizetype count = infos.size();
  for (const QFileInfo& info : infos) {
    const QString path = info.filePath();
    if (!info.exists()) {
      result.error = QStringLiteral("file or folder does not exist: %1").arg(path);
    } else if (!info.isReadable()) {
      result.error = QStringLiteral("cannot read: %1").arg(path);
    } else if (info.isDir()) {
      if (count != 1) {
        result.error = QStringLiteral("open one folder or a selection of files");
      } else {
        result.folder = info.canonicalFilePath();
        if (result.folder == QFileInfo(QDir::homePath()).canonicalFilePath() ||
            result.folder == QDir::rootPath()) {
          result.error = QStringLiteral("choose a media folder, not your home or filesystem root");
        }
      }
    } else if (!info.isFile() || !CaptureScanner::isSupported(CaptureScanner::mediaSuffix(path, true))) {
      result.error = QStringLiteral("unsupported media file: %1").arg(path);
    } else {
      const QString canonical = info.canonicalFilePath();
      if (!seen.contains(canonical)) {
        seen.insert(canonical);
        result.files.append(canonical);
        result.entryPaths.insert(canonical, info.absoluteFilePath());
      }
    }
    if (!result.error.isEmpty()) {
      // A protected folder remains an error; skip other invalid selection entries.
      if (info.isDir() && count == 1) {
        result.files.clear();
        result.entryPaths.clear();
        result.folder.clear();
        return result;
      }
      failures.append(result.error);
      result.skipped.append(path);
      result.error.clear();
    }
  }
  if (!result.skipped.isEmpty() && result.files.isEmpty() && result.folder.isEmpty())
    result.error = failures.join(QLatin1Char('\n'));
  return result;
}
