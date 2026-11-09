#pragma once

#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QStringList>

// Validate paths, retaining supported files and reporting skipped entries. Paths
// are canonical and deduplicated in the order supplied by the file manager.
struct OpenRequest {
  QStringList files;
  QHash<QString, QString> entryPaths;
  QString folder;
  QString error;
  QStringList skipped;
  QString note() const { return skippedNote(skipped); }

  // "Skipped: a, b", naming the first few and counting the rest.
  static QString skippedNote(const QStringList& names);
  static OpenRequest fromPaths(const QStringList& paths);
  // The same, from entries already looked at, so a large drop is not read
  // from the disk twice.
  static OpenRequest fromInfos(const QList<QFileInfo>& infos);
};
