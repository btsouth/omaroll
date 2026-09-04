#pragma once

#include "library/CaptureRecord.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>

// Walks the watched roots and turns files into CaptureRecords.
//
// Classification is filename-pattern first, then extension. That order is the
// whole reason sections are possible: Omarchy writes screenshots into the same
// directory as every other image, so only the name separates "a screenshot I
// took" from "a photo I downloaded".
class CaptureScanner {
public:
  struct Root {
    QString path;
    // Captures land flat, so depth 1. General media roots recurse.
    int maxDepth = 1;
    // Files here that match no producer pattern fall back to these, chosen by
    // medium. Two fields rather than one so a root like Downloads can call
    // everything a Download while Pictures still splits image from video.
    CaptureRecord::Kind imageFallback = CaptureRecord::Picture;
    CaptureRecord::Kind videoFallback = CaptureRecord::Video;
  };

  // Checked once per directory; a set flag ends the walk with a partial list
  // the caller is expected to discard. When supplied, traversedDirectories is
  // filled with the canonical directories the same walk visited.
  [[nodiscard]] static QList<CaptureRecord> scan(const QList<Root>& roots,
                                                 const std::atomic_bool* cancel = nullptr,
                                                 QStringList* traversedDirectories = nullptr);

  // Exposed for testing and reuse: decide a kind from the bare filename, or
  // return false when nothing matches and the caller should fall back.
  [[nodiscard]] static bool classifyByName(const QString& fileName, CaptureRecord::Kind& kind,
                                           QDateTime& captured);

  [[nodiscard]] static bool isImage(const QString& suffix);
  [[nodiscard]] static bool isVideo(const QString& suffix);
  [[nodiscard]] static bool isDocument(const QString& suffix);
  [[nodiscard]] static bool isSupported(const QString& suffix);
};
