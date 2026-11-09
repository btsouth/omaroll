#pragma once

#include <QFileInfo>
#include <QList>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

// What a dragged file carries and what a drop brings in. Library tiles and
// the quick viewer drag the same data, so a file manager, browser or chat app
// takes either the same way.
namespace FileDrag {

// text/uri-list: one fully encoded file: URL per line, CRLF terminated as RFC
// 2483 asks, which is what Nautilus, browsers and Electron apps parse on drop.
inline QString uriList(const QStringList& paths) {
  QString list;
  for (const QString& path : paths) {
    list += QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) + QStringLiteral("\r\n");
  }
  return list;
}

inline QVariantMap mimeData(const QStringList& paths) {
  return {{QStringLiteral("text/uri-list"), uriList(paths)},
          {QStringLiteral("text/plain"), paths.join(QLatin1Char('\n'))}};
}

// Dropped URLs, in the order dropped: local files, local folders, and the
// names of what cannot be opened here (a web address, or something gone).
struct Sorted {
  QStringList files;
  QStringList folders;
  QStringList skipped;
};

inline Sorted sort(const QList<QUrl>& urls) {
  Sorted sorted;
  for (const QUrl& url : urls) {
    if (!url.isLocalFile()) {
      const QString name = url.fileName();
      sorted.skipped.append(name.isEmpty() ? url.toDisplayString() : name);
      continue;
    }
    const QFileInfo info(url.toLocalFile());
    if (info.isDir()) {
      sorted.folders.append(info.absoluteFilePath());
    } else if (info.exists()) {
      sorted.files.append(info.absoluteFilePath());
    } else {
      sorted.skipped.append(info.fileName());
    }
  }
  return sorted;
}

inline QString skippedNote(const QStringList& skipped) {
  return skipped.isEmpty() ? QString()
                           : QStringLiteral("Skipped: %1").arg(skipped.join(QStringLiteral(", ")));
}

} // namespace FileDrag
