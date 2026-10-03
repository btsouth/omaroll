#include "actions/ActionLauncher.h"
#include "app/VideoPlayback.h"

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
#include <QTimer>

#include <memory>

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
  if (QFileInfo::exists(outputPath)) {
    revealExisting(outputPath);
    return true;
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
              // or notification step failed. Keep nonempty output for review;
              // the next request probes it before deciding whether to retry.
              if (output.size() == 0) QFile::remove(outputPath);
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

QVariantMap ActionLauncher::renameFile(const QString& path, const QString& baseName) {
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
  if (!QFile::rename(source.absoluteFilePath(), target)) {
    return failure(u"Could not rename this file"_s);
  }

  return {{QStringLiteral("ok"), true},
          {QStringLiteral("path"), target},
          {QStringLiteral("fileName"), fileName}};
}
