#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <atomic>

// A small on-disk store for per-file hashes, so a relaunch does not read or
// decode every file again. Entries are keyed by path and are only valid while
// the file's size and modification time match. The file is rewritten
// atomically and bounded in size; it is a cache, so anything unreadable,
// from another format version, or over the limit is simply ignored.
namespace HashCache {

struct Entry {
  qint64 bytes = 0;
  qint64 modified = 0;
  QByteArray data;
};

using Entries = QHash<QString, Entry>;

// `~/.cache/omaroll/<name>`, following XDG_CACHE_HOME.
[[nodiscard]] QString path(const QString& name);

// Reads every entry, or nothing when the file is missing, damaged, too large
// or written by a different format. A set cancel flag stops early and returns
// nothing.
[[nodiscard]] Entries load(const QString& file, quint32 magic, quint16 version,
                           qint64 maximumBytes, const std::atomic_bool* cancel = nullptr);

// Writes `preferred` paths first and then any others, stopping before the
// file would exceed `maximumBytes`. Returns false when nothing was written.
bool save(const QString& file, quint32 magic, quint16 version, const Entries& entries,
          const QList<QString>& preferred, qint64 maximumBytes);

} // namespace HashCache
