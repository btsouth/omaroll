#pragma once

#include "library/CaptureRecord.h"

#include <QHash>
#include <QStringList>

// Read-only companionship, independent of marks, filters and presentation.
// Paths are already absolute/canonical. Directory names remain case sensitive;
// only the complete filename stem is folded. No headers or pixels are read.
namespace RawJpegPairs {
using Companions = QHash<QString, QString>;

[[nodiscard]] Companions find(const QStringList& paths);
void assign(QList<CaptureRecord>& records);
// One entry per unambiguous pair, preferring RAW except for the file on screen.
[[nodiscard]] QStringList grouped(const QStringList& paths, const Companions& companions,
                                 const QString& preferred = {});
} // namespace RawJpegPairs
