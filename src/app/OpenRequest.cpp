#include "app/OpenRequest.h"

#include "sources/CaptureScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

OpenRequest OpenRequest::fromPaths(const QStringList& paths) {
  OpenRequest result;
  QSet<QString> seen;
  for (const QString& path : paths) {
    const QFileInfo info(path);
    if (!info.exists()) {
      result.error = QStringLiteral("file or folder does not exist: %1").arg(path);
    } else if (!info.isReadable()) {
      result.error = QStringLiteral("cannot read: %1").arg(path);
    } else if (info.isDir()) {
      if (paths.size() != 1) {
        result.error = QStringLiteral("open one folder or a selection of files");
      } else {
        result.folder = info.canonicalFilePath();
        if (result.folder == QFileInfo(QDir::homePath()).canonicalFilePath() ||
            result.folder == QDir::rootPath()) {
          result.error = QStringLiteral("choose a media folder, not your home or filesystem root");
        }
      }
    } else if (!info.isFile() || !CaptureScanner::isSupported(info.suffix())) {
      result.error = QStringLiteral("unsupported media file: %1").arg(path);
    } else {
      const QString canonical = info.canonicalFilePath();
      if (!seen.contains(canonical)) {
        seen.insert(canonical);
        result.files.append(canonical);
      }
    }
    if (!result.error.isEmpty()) {
      result.files.clear();
      result.folder.clear();
      break;
    }
  }
  return result;
}
