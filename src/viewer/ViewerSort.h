#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

// The orders the quick viewer can step through a folder in. Every order
// starts from the natural name order, so files that tie (the same size, no
// date) stay in the order a file manager shows them.
namespace ViewerSort {

// "name", "taken", "modified" and "size".
[[nodiscard]] inline bool isKnown(const QString& order) {
  return order == u"name" || order == u"taken" || order == u"modified" || order == u"size";
}

// When the file was taken: an Omarchy capture's timestamped name, a JPEG's
// or camera raw's EXIF date, or else the file's modified time. Reads at most
// the start of the file; never decodes.
[[nodiscard]] QDateTime dateTaken(const QString& path);

// |names| (already in name order) in |order|, oldest or smallest first, or
// the other way round when |reversed|. Reads file metadata, so call it on a
// worker for a large folder.
[[nodiscard]] QStringList sorted(const QStringList& names, const QString& order, bool reversed);

} // namespace ViewerSort
