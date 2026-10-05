#include "edit/CopyOutput.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QUuid>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool sameEntry(const struct stat& first, const struct stat& second) {
  return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

} // namespace

CopyOutput::CopyOutput(const QString& preferredPath) : m_path(preferredPath) {
  const QByteArray directory = QFile::encodeName(QFileInfo(m_path).absolutePath());
  m_directory = ::open(directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (m_directory < 0) return;
  m_temporaryName = ".omaroll-copy-" + QUuid::createUuid().toByteArray(QUuid::Id128);
  if (::mkdirat(m_directory, m_temporaryName.constData(), 0700) != 0) {
    m_temporaryName.clear();
    return;
  }
  m_temporaryDirectory = ::openat(m_directory, m_temporaryName.constData(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (m_temporaryDirectory < 0) return;
  const int file = ::openat(m_temporaryDirectory, "output", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (file >= 0 && !m_file.open(file, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
    ::close(file);
  }
}

CopyOutput::~CopyOutput() {
  // Only unlink the file and directory we created, through their pinned parents.
  struct stat owned {}, current {};
  if (m_file.isOpen() && ::fstat(m_file.handle(), &owned) == 0 &&
      ::fstatat(m_temporaryDirectory, "output", &current, AT_SYMLINK_NOFOLLOW) == 0 &&
      sameEntry(owned, current)) {
    ::unlinkat(m_temporaryDirectory, "output", 0);
  }
  if (m_temporaryDirectory >= 0) {
    if (::fstat(m_temporaryDirectory, &owned) == 0 &&
        ::fstatat(m_directory, m_temporaryName.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        sameEntry(owned, current)) {
      ::unlinkat(m_directory, m_temporaryName.constData(), AT_REMOVEDIR);
    }
    ::close(m_temporaryDirectory);
  }
  if (m_directory >= 0) ::close(m_directory);
}

QString CopyOutput::encoderPath() const {
  // jpegtran opens this descriptor path, never a publicly replaceable name.
  return m_file.isOpen() ? QStringLiteral("/proc/%1/fd/%2")
                              .arg(QCoreApplication::applicationPid()).arg(m_file.handle())
                        : QString();
}

QString CopyOutput::publish() {
  if (!m_file.isOpen() || !m_file.flush() || m_file.size() <= 0) return {};
  struct stat directory {}, current {}, owned {}, temporary {};
  const QFileInfo info(m_path);
  if (::fstat(m_directory, &directory) != 0 ||
      ::stat(QFile::encodeName(info.absolutePath()).constData(), &current) != 0 ||
      !sameEntry(directory, current) || ::fstat(m_file.handle(), &owned) != 0 ||
      ::fstatat(m_temporaryDirectory, "output", &temporary, AT_SYMLINK_NOFOLLOW) != 0 ||
      !sameEntry(owned, temporary)) return {};

  for (int number = 1; number <= 10000; ++number) {
    const QString name = number == 1 ? info.fileName()
        : info.completeBaseName() + QStringLiteral("-%1.").arg(number) + info.suffix();
    const QByteArray encodedName = QFile::encodeName(name);
    if (::linkat(m_temporaryDirectory, "output", m_directory, encodedName.constData(), 0) == 0) {
      return info.absolutePath() + QLatin1Char('/') + name;
    }
    if (errno != EEXIST) return {};
  }
  return {};
}
