#include "actions/ActionLauncher.h"
#include "actions/OpenWithRequest.h"
#include "app/VideoPlayback.h"
#include "sources/FileVersion.h"

#include <QClipboard>
#include <QDir>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSettings>
#include <QUrl>
#include <QTimer>

#include <QScopeGuard>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>

using namespace Qt::StringLiterals;

ActionLauncher::ActionLauncher(QObject* parent, std::optional<QDBusConnection> bus)
    : QObject(parent), m_bus(bus ? *bus : QDBusConnection::sessionBus()) {}

bool ActionLauncher::showInFolder(const QString& path) {
  const QFileInfo file(path);
  if (!file.isFile()) {
    emit failed(u"That file is no longer there"_s);
    return false;
  }
  const QString folder = file.absolutePath();
  if (!m_bus.isConnected()) {
    return runDetached(u"xdg-open"_s, {folder}, u"xdg-utils"_s);
  }
  // The generic FileManager1 activation service can belong to a different
  // installed file manager. Resolve the folder default before attempting to
  // select a file, and only activate that application's own D-Bus service.
  auto* query = new QProcess(this);
  query->setProcessEnvironment(externalProcessEnvironment());
  query->setProgram(u"xdg-mime"_s);
  query->setArguments({u"query"_s, u"default"_s, u"inode/directory"_s});
  query->setStandardInputFile(QProcess::nullDevice());
  auto* timer = new QTimer(query);
  timer->setSingleShot(true);
  auto settled = std::make_shared<bool>(false);
  const auto finish = [this, query, timer, settled, folder, path = file.absoluteFilePath()](bool success) {
    if (*settled) return;
    *settled = true;
    timer->stop();
    const QString desktopId = QString::fromUtf8(query->readAllStandardOutput().left(512)).trimmed();
    query->deleteLater();
    QString service;
    if (success && desktopId.endsWith(u".desktop"_s) && !desktopId.contains(QLatin1Char('/'))) {
      const QString entry = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                   u"applications/"_s + desktopId);
      if (!entry.isEmpty()) {
        QSettings desktop(entry, QSettings::IniFormat);
        desktop.beginGroup(u"Desktop Entry"_s);
        if (desktop.value(u"DBusActivatable"_s, false).toBool()) {
          service = desktopId.chopped(8);
        }
      }
    }
    if (service.isEmpty()) {
      runDetached(u"xdg-open"_s, {folder}, u"xdg-utils"_s);
      return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        service, u"/org/freedesktop/FileManager1"_s,
        u"org.freedesktop.FileManager1"_s, u"ShowItems"_s);
    message.setArguments({QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)}, QString()});
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, folder](QDBusPendingCallWatcher* completed) {
      const QDBusPendingReply<> reply = *completed;
      completed->deleteLater();
      if (reply.isError()) runDetached(u"xdg-open"_s, {folder}, u"xdg-utils"_s);
    });
  };
  connect(query, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
    finish(status == QProcess::NormalExit && code == 0);
  });
  connect(query, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) finish(false);
  });
  connect(timer, &QTimer::timeout, query, [query] { query->kill(); });
  query->start();
  timer->start(2000);
  return true;
}

bool ActionLauncher::openWith(const QString& path) {
  if (m_pendingOpenWith.contains(path)) return true;
  auto* request = new OpenWithRequest(m_bus, this);
  connect(request, &OpenWithRequest::failed, this, &ActionLauncher::failed);
  connect(request, &OpenWithRequest::finished, this, [this, path, request] {
    m_pendingOpenWith.remove(path);
    request->deleteLater();
  });
  m_pendingOpenWith.insert(path);
  return request->start(path);
}

QString ActionLauncher::mimeTypeFor(const QString& path) {
  static QMimeDatabase database;
  const QString name = database.mimeTypeForFile(path).name();
  return name.isEmpty() ? u"application/octet-stream"_s : name;
}

bool ActionLauncher::handlerAvailable(const QString& program) const {
  return !QStandardPaths::findExecutable(program).isEmpty();
}

QString ActionLauncher::locate(const QString& program, const QString& packageHint) {
  const QString executable = QStandardPaths::findExecutable(program);
  if (executable.isEmpty()) {
    if (packageHint == u"Omarchy"_s) {
      emit failed(u"%1 comes with Omarchy and is not on this system"_s.arg(program));
    } else if (packageHint.isEmpty()) {
      emit failed(u"%1 is not installed"_s.arg(program));
    } else {
      emit failed(u"%1 is not installed. Try: sudo pacman -S %2"_s.arg(program, packageHint));
    }
  }
  return executable;
}

bool ActionLauncher::copyFile(const QString& path) {
  const QFileInfo info(path);
  if (!info.isFile() || !info.isReadable()) {
    emit failed(info.exists() ? u"Could not read %1"_s.arg(info.fileName())
                              : u"That file is no longer there"_s);
    return false;
  }

  // Only PNG and JPEG paste as picture data anywhere that matters, and the
  // shell's clipboard history records only text and image/png. A video, a
  // GIF or a WebP goes on as a file reference instead, which Nautilus and
  // browser uploads take.
  const QString mime = mimeTypeFor(path);
  if (mime != u"image/png"_s && mime != u"image/jpeg"_s) {
    return copyUris({path});
  }

  // The house helper's --copy-only branch can exit zero after wl-copy fails.
  // Its copy operation is exactly this invocation, so track wl-copy directly
  // to establish whether the selection was accepted.
  return startSubmission(u"wl-copy"_s, {u"--type"_s, mime}, u"wl-clipboard"_s,
                         u"Copied to clipboard"_s, path, 5000);
}

bool ActionLauncher::runDetached(const QString& program, const QStringList& arguments,
                                 const QString& packageHint, const QString& confirmation) {
  const QString executable = locate(program, packageHint);
  if (executable.isEmpty()) {
    return false;
  }

  QProcess process;
  process.setProgram(executable);
  process.setArguments(arguments);
  process.setProcessEnvironment(externalProcessEnvironment());
  if (!process.startDetached()) {
    emit failed(u"Could not start %1"_s.arg(program));
    return false;
  }
  if (!confirmation.isEmpty()) {
    emit reported(confirmation);
  }
  return true;
}

bool ActionLauncher::runSubmission(const QString& program, const QStringList& arguments,
                                   const QString& packageHint, const QString& confirmation) {
  return startSubmission(program, arguments, packageHint, confirmation, {}, 30000);
}

bool ActionLauncher::startSubmission(const QString& program, const QStringList& arguments,
                                     const QString& packageHint, const QString& confirmation,
                                     const QString& inputPath, int timeoutMs) {
  const QString executable = locate(program, packageHint);
  if (executable.isEmpty()) return false;

  auto* process = new QProcess(this);
  process->setProcessEnvironment(externalProcessEnvironment());
  process->setProgram(executable);
  process->setArguments(arguments);
  // No interactive stdin, and no shell interpolation of paths or arguments.
  process->setStandardInputFile(inputPath.isEmpty() ? QProcess::nullDevice() : inputPath);
  auto* timer = new QTimer(process);
  timer->setSingleShot(true);
  struct State {
    QByteArray error;
    QByteArray output;
    bool timedOut = false;
    bool settled = false;
  };
  auto state = std::make_shared<State>();
  // Drain as data arrives, retaining only a bounded tail of diagnostics.
  const auto drain = [process, state] {
    state->error = (state->error + process->readAllStandardError()).right(4096);
    state->output = (state->output + process->readAllStandardOutput()).right(4096);
  };
  connect(process, &QProcess::readyReadStandardError, this, drain);
  connect(process, &QProcess::readyReadStandardOutput, this, drain);
  const auto finish = [this, process, timer, state, drain, program, confirmation](bool success,
                                                                              QString detail) {
    if (state->settled) return;
    state->settled = true;
    timer->stop();
    drain();
    if (detail.isEmpty()) {
      const QString text = QString::fromUtf8(success ? state->output : state->error).trimmed();
      const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
      if (!lines.isEmpty()) detail = lines.last().trimmed();
    }
    if (!success && detail.isEmpty()) detail = u"The tool did not finish successfully"_s;
    process->deleteLater();
    if (success) {
      if (!confirmation.isEmpty()) emit reported(confirmation);
    } else {
      emit failed(u"%1: %2"_s.arg(QFileInfo(program).fileName(), detail));
    }
    emit submissionFinished(program, success, detail);
  };
  connect(process, &QProcess::errorOccurred, this,
          [process, finish](QProcess::ProcessError error) {
    // A crash emits finished too. A failed start does not.
    if (error == QProcess::FailedToStart) finish(false, process->errorString());
  });
  connect(process, &QProcess::finished, this,
          [state, finish](int code, QProcess::ExitStatus status) {
    finish(!state->timedOut && status == QProcess::NormalExit && code == 0,
           state->timedOut ? u"Timed out before completion"_s : QString());
  });
  connect(timer, &QTimer::timeout, process, [process, state] {
    state->timedOut = true;
    process->kill(); // finished owns the single terminal notification
  });
  process->start();
  timer->start(timeoutMs);
  return true;
}

bool ActionLauncher::runTracked(const QString& program, const QStringList& arguments,
                                const QString& packageHint, const QString& outputPath) {
  if (isPending(outputPath)) {
    emit reported(u"Still working on %1"_s.arg(QFileInfo(outputPath).fileName()));
    return true;
  }
  if (QFileInfo(outputPath).exists() || QFileInfo(outputPath).isSymLink()) {
    emit failed(u"Output %1 already exists. Move or rename it before exporting again"_s
                    .arg(QFileInfo(outputPath).fileName()));
    return false;
  }
  const QString executable = locate(program, packageHint);
  if (executable.isEmpty()) {
    return false;
  }

  auto* process = new QProcess(this);
  process->setProcessEnvironment(externalProcessEnvironment());
  process->setProgram(executable);
  process->setArguments(arguments);

  m_pendingOutputs.insert(outputPath);
  emit outputPending(outputPath);
  emit pendingOutputsChanged();
  emit reported(u"Making %1"_s.arg(QFileInfo(outputPath).fileName()));

  connect(process, &QProcess::errorOccurred, this,
          [this, process, program, outputPath](QProcess::ProcessError error) {
            // Anything after a successful start also emits finished, which
            // owns the cleanup; only a start that never happened ends here.
            if (error != QProcess::FailedToStart) {
              return;
            }
            process->deleteLater();
            m_pendingOutputs.remove(outputPath);
            emit pendingOutputsChanged();
            emit failed(u"Could not start %1"_s.arg(program));
            emit outputSettled(outputPath, false);
          });

  connect(process, &QProcess::finished, this,
          [this, process, program, outputPath](int exitCode, QProcess::ExitStatus status) {
            process->deleteLater();
            m_pendingOutputs.remove(outputPath);
            emit pendingOutputsChanged();

            const QFileInfo output(outputPath);
            const bool saved =
                status == QProcess::NormalExit && exitCode == 0 && output.size() > 0;
            if (saved) {
              emit reported(u"Saved %1 beside the original"_s.arg(output.fileName()));
            } else {
              // The helper may have completed conversion before a clipboard
              // or notification step failed. Keep its output for review.
              // A path alone cannot prove ownership, including for empty output.
              const QStringList lines = QString::fromUtf8(process->readAllStandardError())
                                            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
              QString message = lines.isEmpty() ? u"%1 did not finish"_s.arg(program)
                                                : u"%1: %2"_s.arg(program, lines.last().trimmed());
              if (output.size() > 0) {
                message += u". Kept %1 beside the original for review"_s.arg(output.fileName());
              }
              emit failed(message);
            }
            emit outputSettled(outputPath, saved);
          });

  process->start();
  // ffmpeg reads stdin for interactive commands; an EOF up front means any
  // unexpected prompt gets an answer instead of waiting on a pipe forever.
  process->closeWriteChannel();
  return true;
}

bool ActionLauncher::isPending(const QString& outputPath) const {
  return m_pendingOutputs.contains(outputPath);
}

QStringList ActionLauncher::pendingOutputs() const {
  return QStringList(m_pendingOutputs.begin(), m_pendingOutputs.end());
}

void ActionLauncher::revealExisting(const QString& path) {
  emit reported(u"Already made earlier: %1"_s.arg(QFileInfo(path).fileName()));
  emit outputAlreadyDone(path);
}

bool ActionLauncher::copyText(const QString& text, bool sensitive, const QString& mimeType) {
  // wl-copy rather than QClipboard: the house tools use it, it keeps the
  // selection alive after omaroll closes, and only it can flag a paste as
  // sensitive so the clipboard history leaves it out.
  const QString wlCopy = QStandardPaths::findExecutable(u"wl-copy"_s);
  if (!wlCopy.isEmpty()) {
    QStringList arguments;
    if (sensitive) {
      arguments << u"--sensitive"_s;
    }
    if (!mimeType.isEmpty()) {
      arguments << u"--type"_s << mimeType;
    }
    QProcess process;
    process.setProcessEnvironment(externalProcessEnvironment());
    process.start(wlCopy, arguments);
    process.write(text.toUtf8());
    process.closeWriteChannel();
    // wl-copy hands the selection to a forked child and returns at once.
    if (process.waitForFinished(2000) && process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0) {
      return true;
    }
  }
  // A secret must never fall back to an ordinary clipboard offer, where a
  // history manager could retain it.
  if (sensitive) {
    return false;
  }
  if (!mimeType.isEmpty()) {
    return false;
  }
  QGuiApplication::clipboard()->setText(text);
  return true;
}

bool ActionLauncher::copyUris(const QStringList& paths) {
  if (paths.isEmpty()) {
    return false;
  }
  QString list;
  for (const QString& path : paths) {
    if (!QFileInfo::exists(path)) {
      emit failed(u"That file is no longer there"_s);
      return false;
    }
    list += QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) + u"\r\n"_s;
  }
  if (!copyText(list, false, u"text/uri-list"_s)) {
    emit failed(u"wl-copy is not installed"_s);
    return false;
  }
  emit reported(paths.size() == 1 ? u"Copied as a file"_s : u"Copied %1 files"_s.arg(paths.size()));
  return true;
}

bool ActionLauncher::captureTextToClipboard(const QString& program, const QStringList& arguments,
                                            const QString& packageHint, bool sensitive,
                                            const QString& confirmation,
                                            const QString& nothingFound) {
  const QString executable = locate(program, packageHint);
  if (executable.isEmpty()) {
    return false;
  }

  // Asynchronous: tesseract on a full-screen capture can take a few seconds,
  // and the window has to stay live while it works.
  auto* process = new QProcess(this);
  process->setProcessEnvironment(externalProcessEnvironment());
  process->setProgram(executable);
  process->setArguments(arguments);

  connect(process, &QProcess::errorOccurred, this,
          [this, process, program](QProcess::ProcessError error) {
            // A crash after a successful start also emits finished, which
            // reports the tool's own last line; only a start that never
            // happened ends here.
            if (error != QProcess::FailedToStart) {
              return;
            }
            emit failed(u"Could not start %1"_s.arg(program));
            process->deleteLater();
          });

  connect(process, &QProcess::finished, this,
          [this, process, program, sensitive, confirmation, nothingFound](
              int exitCode, QProcess::ExitStatus status) {
            process->deleteLater();
            const QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();

            if (text.isEmpty()) {
              // zbarimg exits 4 for "no symbol found"; tesseract exits 0 with
              // nothing to say. Both are ordinary, not failures. A real
              // failure (missing language data, say) is explained by the
              // tool itself, so pass its last line on.
              if (status != QProcess::NormalExit || (exitCode != 0 && exitCode != 4)) {
                const QStringList lines = QString::fromUtf8(process->readAllStandardError())
                                              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                emit failed(lines.isEmpty() ? u"%1 did not finish"_s.arg(program)
                                            : u"%1: %2"_s.arg(program, lines.last().trimmed()));
              } else {
                emit reported(nothingFound.isEmpty() ? u"Nothing found"_s : nothingFound);
              }
              return;
            }

            if (!copyText(text, sensitive)) {
              emit failed(sensitive ? u"Could not copy this result securely"_s
                                    : u"Could not copy the result"_s);
              return;
            }

            if (!confirmation.isEmpty()) {
              emit reported(confirmation);
              return;
            }
            const qsizetype words =
                text.split(QRegularExpression(u"\\s+"_s), Qt::SkipEmptyParts).size();
            emit reported(u"Copied %1 %2"_s.arg(words).arg(words == 1 ? u"word"_s : u"words"_s));
          });

  process->start();
  return true;
}

bool ActionLauncher::open(const QString& path) {
  if (!QFileInfo::exists(path)) {
    emit failed(u"That file is no longer there"_s);
    return false;
  }
  return runDetached(u"xdg-open"_s, {path});
}

bool ActionLauncher::moveToTrash(const QString& path) {
  if (!QFileInfo::exists(path)) {
    emit failed(u"That file is no longer there"_s);
    return false;
  }

  // Qt creates Trash/files and Trash/info, but expects the XDG data-home
  // parent to exist. A genuinely fresh account may not have used it yet.
  QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
  if (!QFile::moveToTrash(path)) {
    // Most often a trash directory that does not exist on the volume the file
    // lives on, which is worth saying rather than hiding behind "failed".
    emit failed(u"Could not move this file to Trash. Its volume may not support Trash"_s);
    return false;
  }
  return true;
}

QVariantMap ActionLauncher::captureFileAction(const QString& path, const QString& expectedMediaPath) {
  QVariantMap target = capturePermanentDelete(path, expectedMediaPath);
  if (target.isEmpty()) return {};
  target.insert(u"path"_s, QFileInfo(path).absoluteFilePath());
  target.insert(u"entryVersion"_s, FileVersion::key(path, false));
  target.insert(u"mediaVersion"_s, FileVersion::key(path));
  if (!capturedFileMatches(target)) {
    emit failed(u"That file changed. Open it again before acting on it"_s);
    return {};
  }
  return target;
}

bool ActionLauncher::capturedFileMatches(const QVariantMap& target) {
  const QString path = target.value(u"path"_s).toString();
  const QString entryVersion = target.value(u"entryVersion"_s).toString();
  const QString mediaVersion = target.value(u"mediaVersion"_s).toString();
  struct stat parent {}, entry {};
  const QFileInfo info(path);
  return !path.isEmpty() && !entryVersion.isEmpty() && !mediaVersion.isEmpty() &&
      info.fileName() == target.value(u"name"_s).toString() &&
      info.dir().canonicalPath() == target.value(u"parent"_s).toString() &&
      ::stat(QFile::encodeName(info.absolutePath()).constData(), &parent) == 0 &&
      ::lstat(QFile::encodeName(path).constData(), &entry) == 0 &&
      (S_ISREG(entry.st_mode) || S_ISLNK(entry.st_mode)) &&
      QString::number(qulonglong(parent.st_dev)) == target.value(u"parentDevice"_s).toString() &&
      QString::number(qulonglong(parent.st_ino)) == target.value(u"parentInode"_s).toString() &&
      QString::number(qulonglong(entry.st_dev)) == target.value(u"device"_s).toString() &&
      QString::number(qulonglong(entry.st_ino)) == target.value(u"inode"_s).toString() &&
      FileVersion::key(path, false) == entryVersion && FileVersion::key(path) == mediaVersion;
}

bool ActionLauncher::moveCapturedToTrash(const QVariantMap& target) {
  if (!capturedFileMatches(target)) {
    emit failed(u"%1 changed or disappeared. Open the Trash prompt again"_s
                    .arg(target.value(u"path"_s).toString()));
    return false;
  }
  return moveToTrash(target.value(u"path"_s).toString());
}

QVariantMap ActionLauncher::renameCapturedFile(const QVariantMap& target, const QString& baseName) {
  if (!capturedFileMatches(target)) {
    return {{u"ok"_s, false}, {u"error"_s, u"That file changed or disappeared. Open Rename again"_s}};
  }
  QVariantMap result = renameFile(target.value(u"path"_s).toString(), baseName, target);
  if (result.value(u"ok"_s).toBool()) {
    result.insert(u"mediaPath"_s, QFileInfo(result.value(u"path"_s).toString()).canonicalFilePath());
  }
  return result;
}

void ActionLauncher::moveDuplicateCopiesToTrash(const QVariantMap& keep, const QVariantList& copies) {
  // Re-read the bytes on a worker. Cached duplicate groups only nominate
  // candidates; they never authorize moving a file.
  QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
  auto* watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher] {
    const QVariantMap result = watcher->result();
    watcher->deleteLater();
    const int moved = result.value(u"moved"_s).toInt();
    const QStringList skipped = result.value(u"skipped"_s).toStringList();
    if (!skipped.isEmpty()) emit failed(u"Skipped changed, unreadable or nonidentical copies:\n%1"_s
                                          .arg(skipped.join(QLatin1Char('\n'))));
    emit duplicateCleanupFinished(moved, skipped);
  });
  watcher->setFuture(QtConcurrent::run([keep, copies] {
    int moved = 0;
    QStringList skipped;
    const QString keepPath = keep.value(u"path"_s).toString();
    for (const QVariant& value : copies) {
      const QVariantMap target = value.toMap();
      const QString path = target.value(u"path"_s).toString();
      QFile kept(keepPath), copy(path);
      bool identical = path != keepPath && capturedFileMatches(keep) && capturedFileMatches(target) &&
          kept.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly) && kept.size() == copy.size();
      while (identical && !kept.atEnd()) {
        const QByteArray first = kept.read(1024 * 1024);
        const QByteArray second = copy.read(1024 * 1024);
        identical = !first.isEmpty() && first == second &&
                    kept.error() == QFileDevice::NoError && copy.error() == QFileDevice::NoError;
      }
      // Check versions again after reading, immediately before the move.
      identical = identical && copy.atEnd() && capturedFileMatches(keep) && capturedFileMatches(target);
      if (identical && QFile::moveToTrash(path)) ++moved;
      else skipped.append(path);
    }
    return QVariantMap{{u"moved"_s, moved}, {u"skipped"_s, skipped}};
  }));
}

QVariantMap ActionLauncher::capturePermanentDelete(const QString& path,
                                                   const QString& expectedMediaPath) {
  const QFileInfo info(path);
  if (!expectedMediaPath.isEmpty() && info.canonicalFilePath() != expectedMediaPath) {
    emit failed(u"That file changed. Try opening it again before deleting it"_s);
    return {};
  }
  const QString parent = info.dir().canonicalPath();
  const QByteArray name = QFile::encodeName(info.fileName());
  const int directory = ::open(QFile::encodeName(parent).constData(),
                                O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    emit failed(u"Could not access this file's folder"_s);
    return {};
  }
  const auto closeDirectory = qScopeGuard([directory] { ::close(directory); });
  struct stat parentInfo {}, entry {};
  if (::fstat(directory, &parentInfo) != 0 ||
      ::fstatat(directory, name.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
    emit failed(u"That file is no longer there"_s);
    return {};
  }
  if (!S_ISREG(entry.st_mode) && !S_ISLNK(entry.st_mode)) {
    emit failed(u"Only files can be permanently deleted"_s);
    return {};
  }
  // Strings preserve 64-bit filesystem identities across QML's JS numbers.
  return {{u"parent"_s, parent}, {u"name"_s, info.fileName()},
          {u"parentDevice"_s, QString::number(qulonglong(parentInfo.st_dev))},
          {u"parentInode"_s, QString::number(qulonglong(parentInfo.st_ino))},
          {u"device"_s, QString::number(qulonglong(entry.st_dev))},
          {u"inode"_s, QString::number(qulonglong(entry.st_ino))}};
}

bool ActionLauncher::deleteCapturedPermanently(const QVariantMap& target) {
  if (target.isEmpty()) return false;
  const QString parent = target.value(u"parent"_s).toString();
  const QString name = target.value(u"name"_s).toString();
  if (parent.isEmpty() || name.isEmpty() || name.contains(QLatin1Char('/')) ||
      name == u"." || name == u"..") return false;
  const int directory = ::open(QFile::encodeName(parent).constData(),
                                O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    emit failed(u"That file's folder changed. Try again before deleting it"_s);
    return false;
  }
  const auto closeDirectory = qScopeGuard([directory] { ::close(directory); });
  struct stat parentInfo {}, entry {};
  const QByteArray encodedName = QFile::encodeName(name);
  if (::fstat(directory, &parentInfo) != 0 ||
      QString::number(qulonglong(parentInfo.st_dev)) != target.value(u"parentDevice"_s).toString() ||
      QString::number(qulonglong(parentInfo.st_ino)) != target.value(u"parentInode"_s).toString() ||
      ::fstatat(directory, encodedName.constData(), &entry, AT_SYMLINK_NOFOLLOW) != 0 ||
      QString::number(qulonglong(entry.st_dev)) != target.value(u"device"_s).toString() ||
      QString::number(qulonglong(entry.st_ino)) != target.value(u"inode"_s).toString()) {
    emit failed(u"That file or its folder changed. Try again before deleting it"_s);
    return false;
  }
  if (!S_ISREG(entry.st_mode) && !S_ISLNK(entry.st_mode)) {
    emit failed(u"Only files can be permanently deleted"_s);
    return false;
  }
  // Resolve only the basename through the validated directory descriptor.
  // Changing an ancestor symlink after open cannot redirect this removal.
  if (::unlinkat(directory, encodedName.constData(), 0) != 0) {
    emit failed(u"Could not permanently delete this file: %1"_s.arg(QString::fromLocal8Bit(std::strerror(errno))));
    return false;
  }
  return true;
}

bool ActionLauncher::deletePermanently(const QString& path) {
  return deleteCapturedPermanently(capturePermanentDelete(path));
}

QVariantMap ActionLauncher::renameFile(const QString& path, const QString& baseName) {
  return renameFile(path, baseName, {});
}

QVariantMap ActionLauncher::renameFile(const QString& path, const QString& baseName, const QVariantMap& captured) {
  const auto failure = [](const QString& message) {
    return QVariantMap{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), message}};
  };

  const QFileInfo source(path);
  if (!source.isFile()) {
    return failure(u"That file is no longer there"_s);
  }

  const QString name = baseName.trimmed();
  if (name.isEmpty() || name == u"."_s || name == u".."_s ||
      name.contains(QLatin1Char('/')) || name.contains(QChar::Null)) {
    return failure(u"Use a name without a slash"_s);
  }

  const QString suffix = source.suffix();
  const QString fileName = suffix.isEmpty() ? name : name + QLatin1Char('.') + suffix;
  if (QFile::encodeName(fileName).size() > 255) {
    return failure(u"That name is too long"_s);
  }

  const QString target = source.dir().filePath(fileName);
  if (QDir::cleanPath(target) == QDir::cleanPath(source.absoluteFilePath())) {
    return failure(u"That is already the file's name"_s);
  }
  if (QFileInfo::exists(target)) {
    return failure(u"A file with that name already exists"_s);
  }
  if (!captured.isEmpty() && !capturedFileMatches(captured)) {
    return failure(u"That file changed or disappeared. Open Rename again"_s);
  }
  if (!QFile::rename(source.absoluteFilePath(), target)) {
    return failure(u"Could not rename this file"_s);
  }

  return {{QStringLiteral("ok"), true},
          {QStringLiteral("path"), target},
          {QStringLiteral("fileName"), fileName}};
}
