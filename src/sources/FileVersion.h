#pragma once

#include <QFile>
#include <QString>

#include <sys/stat.h>

namespace FileVersion {

inline QString key(const struct stat& info) {
  return QStringLiteral("%1-%2-%3-%4-%5")
      .arg(qulonglong(info.st_dev)).arg(qulonglong(info.st_ino)).arg(qlonglong(info.st_size))
      .arg(qlonglong(info.st_mtim.tv_sec)).arg(qlonglong(info.st_mtim.tv_nsec));
}

// Distinguish an atomic replacement or an in-place save, including writes
// within the same millisecond. This is a cache identity, not a content hash.
inline QString key(const QString& path, bool followSymlink = true) {
  struct stat info {};
  const QByteArray name = QFile::encodeName(path);
  if (path.isEmpty() || (followSymlink ? ::stat(name.constData(), &info)
                                     : ::lstat(name.constData(), &info)) != 0) {
    return {};
  }
  return key(info);
}

} // namespace FileVersion
