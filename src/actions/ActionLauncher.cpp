#include "actions/ActionLauncher.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

using namespace Qt::StringLiterals;

ActionLauncher::ActionLauncher(QObject* parent) : QObject(parent) {}

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

  // The house helper first, which is exactly "wl-copy --type <mime> < file";
  // the same call directly on a system without Omarchy.
  const QString helper = QStandardPaths::findExecutable(u"omarchy-clipboard-paste-file"_s);
  if (!helper.isEmpty()) {
    return runDetached(helper, {u"--copy-only"_s, mimeTypeFor(path), path}, {},
                       u"Copied to clipboard"_s);
  }
  const QString wlCopy = QStandardPaths::findExecutable(u"wl-copy"_s);
  if (wlCopy.isEmpty()) {
    emit failed(u"wl-copy is not installed. Try: sudo pacman -S wl-clipboard"_s);
    return false;
  }
  QProcess process;
  process.setStandardInputFile(path);
  process.start(wlCopy, {u"--type"_s, mimeTypeFor(path)});
  if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0) {
    emit failed(u"Could not copy %1"_s.arg(info.fileName()));
    return false;
  }
  emit reported(u"Copied to clipboard"_s);
  return true;
}

bool ActionLauncher::runDetached(const QString& program, const QStringList& arguments,
                                 const QString& packageHint, const QString& confirmation) {
  const QString executable = locate(program, packageHint);
  if (executable.isEmpty()) {
    return false;
  }

  if (!QProcess::startDetached(executable, arguments)) {
    emit failed(u"Could not start %1"_s.arg(program));
    return false;
  }
  if (!confirmation.isEmpty()) {
    emit reported(confirmation);
  }
  return true;
}

bool ActionLauncher::runTracked(const QString& program, const QStringList& arguments,
                                const QString& packageHint, const QString& outputPath) {
  const QString executable = locate(program, packageHint);
  if (executable.isEmpty()) {
    return false;
  }

  auto* process = new QProcess(this);
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
              // The run was refused while this path existed, so whatever sits
              // there now is this run's partial write, not a user's file.
              QFile::remove(outputPath);
              const QStringList lines = QString::fromUtf8(process->readAllStandardError())
                                            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
              emit failed(lines.isEmpty() ? u"%1 did not finish"_s.arg(program)
                                          : u"%1: %2"_s.arg(program, lines.last().trimmed()));
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

void ActionLauncher::settleExisting(const QString& path) {
  emit reported(u"Already done: %1 is beside the original"_s.arg(QFileInfo(path).fileName()));
  emit outputSettled(path, true);
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
  process->setProgram(executable);
  process->setArguments(arguments);

  connect(process, &QProcess::errorOccurred, this, [this, process, program] {
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
