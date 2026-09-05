#pragma once

#include <QStringList>

// Validate the whole request before changing a window or forwarding it. Paths
// are canonical and deduplicated in the order supplied by the file manager.
struct OpenRequest {
  QStringList files;
  QString folder;
  QString error;

  static OpenRequest fromPaths(const QStringList& paths);
};
