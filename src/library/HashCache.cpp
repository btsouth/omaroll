#include "library/HashCache.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

namespace {

constexpr quint32 kMaximumEntries = 500'000;
constexpr int kMaximumEntryData = 64 * 1024;

QString cacheHome() {
  const QString configured = qEnvironmentVariable("XDG_CACHE_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : configured;
}

qint64 entrySize(const QString& path, const HashCache::Entry& entry) {
  // QString is stored as a length and UTF-16; the data as a length and bytes.
  return 4 + qint64(path.size()) * 2 + 8 + 8 + 4 + entry.data.size();
}

} // namespace

namespace HashCache {

QString path(const QString& name) {
  return cacheHome() + QStringLiteral("/omaroll/") + name;
}

Entries load(const QString& file, quint32 magic, quint16 version, qint64 maximumBytes,
             const std::atomic_bool* cancel) {
  QFile input(file);
  if (!input.open(QIODevice::ReadOnly) || input.size() > maximumBytes) {
    return {};
  }
  QDataStream stream(&input);
  stream.setVersion(QDataStream::Qt_6_0);
  quint32 storedMagic = 0;
  quint16 storedVersion = 0;
  quint32 count = 0;
  stream >> storedMagic >> storedVersion >> count;
  if (stream.status() != QDataStream::Ok || storedMagic != magic || storedVersion != version ||
      count > kMaximumEntries) {
    return {};
  }
  Entries entries;
  entries.reserve(int(count));
  for (quint32 index = 0; index < count; ++index) {
    if (cancel && (index % 256) == 0 && cancel->load()) {
      return {};
    }
    QString key;
    Entry entry;
    stream >> key >> entry.bytes >> entry.modified >> entry.data;
    if (stream.status() != QDataStream::Ok || key.isEmpty() ||
        entry.data.size() > kMaximumEntryData) {
      return {};
    }
    entries.insert(key, std::move(entry));
  }
  return entries;
}

bool save(const QString& file, quint32 magic, quint16 version, const Entries& entries,
          const QList<QString>& preferred, qint64 maximumBytes) {
  QList<QString> order;
  order.reserve(entries.size());
  QSet<QString> seen;
  for (const QString& key : preferred) {
    if (entries.contains(key) && !seen.contains(key)) {
      order.append(key);
      seen.insert(key);
    }
  }
  for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
    if (!seen.contains(it.key())) {
      order.append(it.key());
    }
  }

  QList<QString> retained;
  qint64 total = 16;
  for (const QString& key : std::as_const(order)) {
    const qint64 size = entrySize(key, entries[key]);
    if (retained.size() >= int(kMaximumEntries) || total + size > maximumBytes) {
      break;
    }
    retained.append(key);
    total += size;
  }

  const QString directory = QFileInfo(file).absolutePath();
  if (!QDir().mkpath(directory)) {
    return false;
  }
  QFile::setPermissions(directory,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QSaveFile output(file);
  if (!output.open(QIODevice::WriteOnly)) {
    return false;
  }
  QDataStream stream(&output);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << magic << version << quint32(retained.size());
  for (const QString& key : std::as_const(retained)) {
    const Entry& entry = entries[key];
    stream << key << entry.bytes << entry.modified << entry.data;
  }
  if (stream.status() != QDataStream::Ok || output.size() > maximumBytes || !output.commit()) {
    output.cancelWriting();
    return false;
  }
  QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

} // namespace HashCache
