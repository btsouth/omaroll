#pragma once

#include "library/CaptureRecord.h"

#include <QList>
#include <QString>
#include <QStringList>

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
    // Files here that match no capture pattern fall back to this kind.
    CaptureRecord::Kind fallbackKind = CaptureRecord::Picture;
  };

  [[nodiscard]] static QList<CaptureRecord> scan(const QList<Root>& roots);

  // Exposed for testing and reuse: decide a kind from the bare filename, or
  // return false when nothing matches and the caller should fall back.
  [[nodiscard]] static bool classifyByName(const QString& fileName, CaptureRecord::Kind& kind,
                                           QDateTime& captured);

  [[nodiscard]] static bool isImage(const QString& suffix);
  [[nodiscard]] static bool isVideo(const QString& suffix);
};
