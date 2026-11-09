#pragma once

#include <QStringList>
#include <QHash>

// Validate paths, retaining supported files and reporting skipped entries. Paths
// are canonical and deduplicated in the order supplied by the file manager.
struct OpenRequest {
  QStringList files;
  QHash<QString, QString> entryPaths;
  QString folder;
  QString error;
  QStringList skipped;
  QString note() const { return skipped.isEmpty() ? QString() : QStringLiteral("Skipped: %1").arg(skipped.join(QStringLiteral(", "))); }

  static OpenRequest fromPaths(const QStringList& paths);
};
