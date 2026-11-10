#include "actions/TrashRestore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

using namespace Qt::StringLiterals;

namespace TrashRestore {

namespace {

QString number(auto value) { return QString::number(qulonglong(value)); }

// Device and inode alone are not enough: a filesystem may hand a freed
// inode straight to the next new file. Size and modification time tell a
// later file apart, and so does the change time the move into the Trash set,
// except for a file with other hard links, whose change time moves whenever
// one of its other names is trashed or restored.
QString stamp(const struct stat& info) {
  QString result = u"%1:%2.%3"_s.arg(qlonglong(info.st_size))
                       .arg(qlonglong(info.st_mtim.tv_sec)).arg(qlonglong(info.st_mtim.tv_nsec));
  if (info.st_nlink <= 1) {
    result += u":%1.%2"_s.arg(qlonglong(info.st_ctim.tv_sec)).arg(qlonglong(info.st_ctim.tv_nsec));
  }
  return result;
}

bool sameStamp(const struct stat& info, const QString& recorded) {
  // A record made while the file had other links has no change time; one made
  // with a single link is compared in full even if links were added since.
  const QString now = stamp(info);
  if (recorded.count(u':') == 1) return now.section(u':', 0, 1) == recorded;
  struct stat single = info;
  single.st_nlink = 1;
  return stamp(single) == recorded;
}

QString errorText(int code) { return QString::fromLocal8Bit(std::strerror(code)); }

// Renames |from| in |fromDir| to |to| in |toDir| only if |to| is free. Some
// filesystems do not take RENAME_NOREPLACE; a hard link followed by removing
// the old name is just as unable to replace an existing file.
int renameNoReplace(int fromDir, const char* from, int toDir, const char* to) {
  if (::renameat2(fromDir, from, toDir, to, RENAME_NOREPLACE) == 0) return 0;
  if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) return errno;
  struct stat source {};
  if (::fstatat(fromDir, from, &source, AT_SYMLINK_NOFOLLOW) != 0) return errno;
  if (::linkat(fromDir, from, toDir, to, 0) != 0) return errno;
  if (::unlinkat(fromDir, from, 0) != 0) {
    const int code = errno;
    // Take back only our own link, never a file renamed over it meanwhile.
    struct stat placed {};
    if (::fstatat(toDir, to, &placed, AT_SYMLINK_NOFOLLOW) == 0 &&
        placed.st_dev == source.st_dev && placed.st_ino == source.st_ino) {
      ::unlinkat(toDir, to, 0);
    }
    return code;
  }
  return 0;
}

} // namespace

QVariantMap moveToTrash(const QString& path, QString* error) {
  const auto fail = [error](const QString& message) {
    if (error) *error = message;
    return QVariantMap();
  };
  const QString absolute = QFileInfo(path).absoluteFilePath();
  struct stat before {};
  if (path.isEmpty() || ::lstat(QFile::encodeName(absolute).constData(), &before) != 0) {
    return fail(u"That file is no longer there"_s);
  }

  // Qt creates Trash/files and Trash/info, but expects the XDG data-home
  // parent to exist. A genuinely fresh account may not have used it yet.
  QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
  QString trashPath;
  if (!QFile::moveToTrash(absolute, &trashPath)) {
    // Most often a trash directory that does not exist on the volume the file
    // lives on, which is worth saying rather than hiding behind "failed".
    return fail(u"Could not move this file to Trash. Its volume may not support Trash"_s);
  }

  QVariantMap record{{u"path"_s, absolute}, {u"trashPath"_s, QString()}};
  struct stat after {};
  if (!trashPath.isEmpty() && ::lstat(QFile::encodeName(trashPath).constData(), &after) == 0) {
    record.insert(u"trashPath"_s, trashPath);
    // Strings keep 64-bit identities intact through QML's numbers.
    record.insert(u"device"_s, number(after.st_dev));
    record.insert(u"inode"_s, number(after.st_ino));
    record.insert(u"stamp"_s, stamp(after));
  }
  return record;
}

Result restore(const QVariantMap& record) {
  const QString original = record.value(u"path"_s).toString();
  const QString trashPath = record.value(u"trashPath"_s).toString();
  const QString name = QFileInfo(original).fileName();
  if (original.isEmpty() || trashPath.isEmpty() || name.isEmpty()) {
    return {false, u"Could not undo that move to Trash. Restore it from the file manager"_s};
  }

  // A trashed file sits at <trash>/files/<name>, described by
  // <trash>/info/<name>.trashinfo, in the home Trash and a volume's alike.
  const QFileInfo trashed(trashPath);
  const QString filesDir = trashed.absolutePath();
  const QString trashRoot = QFileInfo(filesDir).absolutePath();
  const QString trashName = trashed.fileName();
  if (QFileInfo(filesDir).fileName() != u"files" || trashName.isEmpty()) {
    return {false, u"Could not undo that move to Trash. Restore %1 from the file manager"_s.arg(name)};
  }

  const int files = ::open(QFile::encodeName(filesDir).constData(),
                           O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (files < 0) return {false, u"%1 is no longer in the Trash"_s.arg(name)};
  const auto closeFiles = qScopeGuard([files] { ::close(files); });
  const QByteArray encodedTrashName = QFile::encodeName(trashName);
  struct stat entry {};
  if (::fstatat(files, encodedTrashName.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
    return {false, u"%1 is no longer in the Trash"_s.arg(name)};
  }
  if (number(entry.st_dev) != record.value(u"device"_s).toString() ||
      number(entry.st_ino) != record.value(u"inode"_s).toString() ||
      !sameStamp(entry, record.value(u"stamp"_s).toString())) {
    return {false, u"%1 in the Trash is a different file now. Restore it from the file manager"_s.arg(name)};
  }

  const QString parent = QFileInfo(original).absolutePath();
  const int folder = ::open(QFile::encodeName(parent).constData(),
                            O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (folder < 0) {
    return {false, u"%1's folder is gone. It is still in the Trash"_s.arg(name), true};
  }
  const auto closeFolder = qScopeGuard([folder] { ::close(folder); });
  const int code = renameNoReplace(files, encodedTrashName.constData(), folder,
                                   QFile::encodeName(name).constData());
  if (code == EEXIST) {
    return {false, u"Another file is already named %1. The trashed one is still in the Trash"_s.arg(name), true};
  }
  if (code != 0) {
    return {false, u"Could not restore %1: %2. It is still in the Trash"_s.arg(name, errorText(code))};
  }

  // The file is back; a stale .trashinfo would leave a ghost entry in the
  // file manager's Trash.
  QFile::remove(trashRoot + u"/info/"_s + trashName + u".trashinfo"_s);
  // Its .xmp sidecars went to the Trash with it and come back with it. One
  // that cannot is left in the Trash.
  for (const QVariant& sidecar : record.value(u"sidecars"_s).toList()) (void)restore(sidecar.toMap());
  return {true, {}};
}

} // namespace TrashRestore
