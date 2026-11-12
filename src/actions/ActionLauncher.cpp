#include "actions/ActionLauncher.h"
#include "actions/ClipboardServe.h"
#include "actions/FileDrag.h"
#include "actions/TrashRestore.h"
#include "edit/ClipboardText.h"
#include "actions/FileChooserRequest.h"
#include "actions/OpenWithRequest.h"
#include "app/VideoPlayback.h"
#include "edit/ClipboardText.h"
#include "library/EmbeddedMetadata.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"
#include "sources/FileVersion.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalBlocker>
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
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <functional>
#include <atomic>
#include <memory>

using namespace Qt::StringLiterals;

ActionLauncher::ActionLauncher(QObject* parent, std::optional<QDBusConnection> bus)
    : QObject(parent), m_bus(bus ? *bus : QDBusConnection::sessionBus()) {
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
          this, &ActionLauncher::stopTracked);
}

ActionLauncher::~ActionLauncher() {
  stopTracked();
}

void ActionLauncher::stopTracked() {
  const auto processes = m_trackedProcesses;
  QList<qint64> groups;
  QElapsedTimer starting;
  starting.start();
  for (auto* process : processes) {
    const QSignalBlocker blocked(process);
    if (process->state() == QProcess::Starting) process->waitForStarted(qMax(1, 1000 - int(starting.elapsed())));
    const qint64 group = process->processId();
    if (group > 0) {
      groups.append(group);
      ::kill(-group, SIGTERM);
    }
  }
  QElapsedTimer wait;
  wait.start();
  for (auto* process : processes) {
    const QSignalBlocker blocked(process);
    if (process->state() != QProcess::NotRunning) {
      process->waitForFinished(qMax(1, 500 - int(wait.elapsed())));
    }
  }
  // The helper may already have exited while an encoder ignored SIGTERM.
  for (const qint64 group : groups) ::kill(-group, SIGKILL);
  wait.restart();
  for (auto* process : processes) {
    const QSignalBlocker blocked(process);
    if (process->state() != QProcess::NotRunning) {
      process->waitForFinished(qMax(1, 1000 - int(wait.elapsed())));
    }
  }
  m_trackedProcesses.clear();
}

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

void ActionLauncher::openFiles(const QStringList& paths) {
  if (!paths.isEmpty()) emit openRequested(paths);
}

QVariantMap ActionLauncher::fileDragData(const QStringList& paths) const {
  return FileDrag::mimeData(paths);
}

QVariantMap ActionLauncher::sortDrop(const QList<QUrl>& urls) const {
  const FileDrag::Sorted sorted = FileDrag::sort(urls);
  QVariantList folderUrls;
  for (const QString& folder : sorted.folders) folderUrls.append(QUrl::fromLocalFile(folder));
  return {{u"files"_s, sorted.files},
          {u"folders"_s, sorted.folders},
          {u"folderUrls"_s, folderUrls},
          {u"skipped"_s, sorted.skipped},
          {u"note"_s, FileDrag::skippedNote(sorted.skipped)}};
}

bool ActionLauncher::chooseFilesToOpen() {
  // One chooser at a time; a second click while it is up does nothing.
  if (findChild<FileChooserRequest*>()) return true;
  auto* request = new FileChooserRequest(m_bus, this);
  connect(request, &FileChooserRequest::failed, this, &ActionLauncher::failed);
  connect(request, &FileChooserRequest::chosen, this, [this](const QStringList& paths) {
    if (!paths.isEmpty()) emit openRequested(paths);
  });
  connect(request, &FileChooserRequest::finished, request, &QObject::deleteLater);
  return request->start(u"Open in Omaroll"_s,
                        {u"image/*"_s, u"video/*"_s, u"application/pdf"_s});
}

bool ActionLauncher::openDemo() {
  return runDetached(QCoreApplication::applicationFilePath(), {u"--demo"_s}, {},
                     u"Opening the demo library in a new window"_s);
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
  // shell's clipboard history records only text and image/png. Any other
  // picture Qt reads (WebP, HEIC, AVIF, JPEG XL, a GIF's first frame) goes on
  // as a file reference, which Nautilus and browser uploads take, with a PNG
  // beside it for chat apps, editors and the history. A video, or a picture
  // without a Wayland session, is a file reference alone.
  const QString mime = mimeTypeFor(path);
  if (mime != u"image/png"_s && mime != u"image/jpeg"_s) {
    if (!ClipboardServe::mayOfferPng(mime) || qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
      return copyUris({path});
    }
    newCopy();
    return copyWithPng(path, QString::fromLatin1(ClipboardServe::kUriList));
  }
  newCopy();

  // A JPEG goes on as itself and as PNG, so the shell's history keeps it.
  if (ClipboardServe::mayOfferPng(mime) && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
    return copyWithPng(path, mime);
  }

  // The house helper's --copy-only branch can exit zero after wl-copy fails.
  // Its copy operation is exactly this invocation, so track wl-copy directly
  // to establish whether the selection was accepted.
  return startSubmission(u"wl-copy"_s, {u"--type"_s, mime}, u"wl-clipboard"_s,
                         u"Copied to clipboard"_s, path, 5000, &m_copyProcess);
}

void ActionLauncher::newCopy() {
  ++m_copyGeneration;
  // A copy still reading a big file must not land on top of this one.
  for (QProcess* older : {m_copyHelper.data(), m_copyProcess.data()}) {
    if (older && older->state() != QProcess::NotRunning) {
      older->setProperty("superseded", true);
      older->kill();
    }
  }
}

bool ActionLauncher::copyWithPng(const QString& path, const QString& mime) {
  auto* process = new QProcess(this);
  m_copyHelper = process;
  const quint64 generation = m_copyGeneration;
  process->setProcessEnvironment(externalProcessEnvironment());
  process->setProgram(m_clipboardHelper.isEmpty() ? QCoreApplication::applicationFilePath()
                                                  : m_clipboardHelper);
  process->setArguments({QString::fromLatin1(ClipboardServe::kOption), mime, path});
  process->setStandardInputFile(QProcess::nullDevice());
  process->setStandardOutputFile(QProcess::nullDevice());
  auto* timer = new QTimer(process);
  timer->setSingleShot(true);
  auto settled = std::make_shared<bool>(false);
  // copied, or the helper's exit code when it did not copy (-1: no exit).
  const auto finish = [this, process, timer, settled, path, mime, generation](bool copied,
                                                                              int code) {
    if (*settled) return;
    *settled = true;
    timer->stop();
    process->deleteLater();
    // Something newer went on the clipboard meanwhile. A fallback now would
    // put this older picture back over it.
    if (generation != m_copyGeneration || code == ClipboardServe::kSuperseded) {
      emit submissionFinished(u"omaroll"_s, false, u"A newer copy replaced this one"_s);
      return;
    }
    if (copied) {
      // A file reference says so, as it did before the PNG came with it.
      emit reported(mime == QLatin1StringView(ClipboardServe::kUriList) ? u"Copied as a file"_s
                                                                          : u"Copied to clipboard"_s);
      emit submissionFinished(u"omaroll"_s, true, {});
      return;
    }
    // No ext-data-control here, or it failed: the file reference, or the
    // file's own type, through wl-copy as before.
    if (mime == QLatin1StringView(ClipboardServe::kUriList)) {
      const bool ok = copyUris({path});
      emit submissionFinished(u"wl-copy"_s, ok, {});
      return;
    }
    startSubmission(u"wl-copy"_s, {u"--type"_s, mime}, u"wl-clipboard"_s,
                    u"Copied to clipboard"_s, path, 5000, &m_copyProcess);
  };
  connect(process, &QProcess::finished, this,
          [finish](int code, QProcess::ExitStatus status) {
            const bool exited = status == QProcess::NormalExit;
            finish(exited && code == 0, exited ? code : -1);
          });
  connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) finish(false, -1);
  });
  connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
  process->start();
  timer->start(3000);
  return true;
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
                                     const QString& inputPath, int timeoutMs,
                                     QPointer<QProcess>* track) {
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
    if (process->property("superseded").toBool()) {
      process->deleteLater();
      emit submissionFinished(program, false, u"A newer copy replaced this one"_s);
      return;
    }
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
  if (track) *track = process;
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
  if (QFileInfo(outputPath).isSymLink()) {
    emit failed(u"Output %1 is a symbolic link. Move or rename it before exporting again"_s
                    .arg(QFileInfo(outputPath).fileName()));
    return false;
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
  process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
  m_trackedProcesses.insert(process);

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
            m_trackedProcesses.remove(process);
            process->deleteLater();
            m_pendingOutputs.remove(outputPath);
            emit pendingOutputsChanged();
            emit failed(u"Could not start %1"_s.arg(program));
            emit outputSettled(outputPath, false);
          });

  connect(process, &QProcess::finished, this,
          [this, process, program, outputPath](int exitCode, QProcess::ExitStatus status) {
            const QString finishedVersion = FileVersion::key(outputPath);
            const QFileInfo output(outputPath);
            const bool empty = output.exists() && !output.isSymLink() && output.isFile() && output.size() == 0;
            m_trackedProcesses.remove(process);
            process->deleteLater();
            m_pendingOutputs.remove(outputPath);
            emit pendingOutputsChanged();

            const bool saved =
                status == QProcess::NormalExit && exitCode == 0 && output.size() > 0;
            if (saved) {
              emit reported(u"Saved %1 beside the original"_s.arg(output.fileName()));
            } else {
              // The helper may have completed conversion before a clipboard
              // or notification step failed. Keep its output for review.
              if (empty && !finishedVersion.isEmpty() && !QFileInfo(outputPath).isSymLink() &&
                  FileVersion::key(outputPath) == finishedVersion) {
                QFile::remove(outputPath);
              }
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

bool ActionLauncher::copyPlainText(const QString& text) {
  newCopy();
  return ClipboardText::offer(text);
}

bool ActionLauncher::copyText(const QString& text, bool sensitive, const QString& mimeType) {
  newCopy();
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
  // An incomplete export output: only the file itself goes.
  return !trashFile(path, false).isEmpty();
}

namespace {
// Moves |path| to the Trash, and then its .xmp |sidecars|, found on the
// window thread before the file moves. They count as the file's: the record
// holds theirs under "sidecars", so undo puts them back together. Safe off
// the window thread.
QVariantMap trashWithSidecars(const QString& path, const QStringList& sidecars, QString* error = nullptr) {
  QVariantMap record = TrashRestore::moveToTrash(path, error);
  if (record.isEmpty() || sidecars.isEmpty()) return record;
  QVariantList moved;
  for (const QString& sidecar : sidecars) {
    const QVariantMap trashed = TrashRestore::moveToTrash(sidecar);
    if (!trashed.isEmpty()) moved.append(trashed);
  }
  if (!moved.isEmpty()) record.insert(u"sidecars"_s, moved);
  return record;
}
} // namespace

void ActionLauncher::findSidecarLater(const QString& path) {
  if (path.isEmpty()) return;
  QtConcurrent::run([path] { return EmbeddedMetadata::findSidecar(path); })
      .then(this, [this, path](const QString& sidecar) { noteSidecar(path, sidecar); });
}

void ActionLauncher::noteSidecar(const QString& path, const QString& sidecar) {
  if (path.isEmpty()) return;
  m_knownSidecarOrder.removeOne(path);
  if (sidecar.isEmpty()) {
    m_knownSidecars.remove(path);
    return;
  }
  m_knownSidecars.insert(path, sidecar);
  m_knownSidecarOrder.append(path);
  while (m_knownSidecarOrder.size() > 32) m_knownSidecars.remove(m_knownSidecarOrder.takeFirst());
}

QStringList ActionLauncher::sidecarFiles(const QString& path) const {
  const QFileInfo info(path);
  const QString name = info.fileName();
  if (name.isEmpty() || name.endsWith(u".xmp"_s, Qt::CaseInsensitive)) return {};
  const QString folder = info.absolutePath() + QLatin1Char('/');
  QStringList found;
  QSet<QString> seen;
  const auto take = [&](const QString& candidate) {
    const QFileInfo sidecar(candidate);
    if (!sidecar.isFile()) return;
    // One file under two spellings on a case-insensitive filesystem.
    const QString canonical = sidecar.canonicalFilePath();
    if (seen.contains(canonical)) return;
    seen.insert(canonical);
    found.append(sidecar.absoluteFilePath());
  };
  if (m_sidecarLookup) {
    if (const QString recorded = m_sidecarLookup(path); !recorded.isEmpty()) take(recorded);
  }
  if (const QString noted = m_knownSidecars.value(path); !noted.isEmpty()) take(noted);
  take(folder + name + u".xmp"_s);
  take(folder + name + u".XMP"_s);
  if (CameraRaw::isRaw(info.suffix())) {
    const QString base = info.completeBaseName();
    take(folder + base + u".xmp"_s);
    take(folder + base + u".XMP"_s);
  }
  return found;
}

QVariantMap ActionLauncher::trashFile(const QString& path, bool withSidecars) {
  if (!QFileInfo::exists(path)) {
    emit failed(u"That file is no longer there"_s);
    return {};
  }
  QString error;
  QVariantMap record = withSidecars ? trashWithSidecars(path, sidecarFiles(path), &error)
                                     : TrashRestore::moveToTrash(path, &error);
  if (record.isEmpty()) emit failed(error);
  return record;
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

QVariantMap ActionLauncher::trashCaptured(const QVariantMap& target) {
  if (!capturedFileMatches(target)) {
    emit failed(u"%1 changed or disappeared. Open the Trash prompt again"_s
                    .arg(target.value(u"path"_s).toString()));
    return {};
  }
  QVariantMap record = trashFile(target.value(u"path"_s).toString());
  if (!record.isEmpty() && target.contains(u"viewedPath"_s)) {
    record.insert(u"viewedPath"_s, target.value(u"viewedPath"_s));
  }
  return record;
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

namespace {
// Faults the tests inject into relocateEntry, as ActionLauncher::RelocationFault bits.
std::atomic_int relocationFaults{0};

// Copies |from| to a new |to| (O_EXCL: never an existing file), checks every
// byte arrived, and gives the copy the original's permissions and, when
// |keepTime|, its modified time. A failed copy is removed.
bool exclusiveCopy(const QString& from, const QString& to, bool keepTime) {
  struct stat original {};
  if (::stat(QFile::encodeName(from).constData(), &original) != 0 || !S_ISREG(original.st_mode)) {
    return false;
  }
  QFile in(from);
  QFile out(to);
  if (!in.open(QIODevice::ReadOnly)) return false;
  if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
  bool okay = true;
  qint64 written = 0;
  while (okay && !in.atEnd()) {
    const QByteArray chunk = in.read(1024 * 1024);
    okay = in.error() == QFileDevice::NoError &&
           (chunk.isEmpty() || out.write(chunk) == chunk.size());
    written += chunk.size();
    if (chunk.isEmpty()) break;
  }
  // On the device before a move removes the original, which may be the
  // only other copy (a USB drive pulled right after).
  okay = okay && out.flush() && ::fsync(out.handle()) == 0 && written == original.st_size;
  out.close();
  okay = okay && QFileInfo(to).size() == original.st_size;
  if (okay) {
    // Permissions last, so a read-only original's copy could still be
    // written; the time with utimensat, which needs no write access.
    okay = ::chmod(QFile::encodeName(to).constData(), original.st_mode & 07777) == 0;
    if (okay && keepTime) {
      const timespec times[2] = {{0, UTIME_OMIT}, original.st_mtim};
      okay = ::utimensat(AT_FDCWD, QFile::encodeName(to).constData(), times, 0) == 0;
    }
  }
  if (!okay) QFile::remove(to);
  return okay;
}

// The last way to move a file where the filesystem neither renames without
// replacing nor links (another device, or a FUSE or gvfs mount): an
// exclusive copy, then the original removed, or the copy if that fails.
// Puts the copy's name in |to|'s folder on the device, so a crash right
// after the original is removed still finds the file by one of its names.
// A filesystem that cannot sync a folder (some FUSE mounts) says so with
// EINVAL or EOPNOTSUPP, which is taken as done.
bool syncParentFolder(const QString& to) {
  const QByteArray folder = QFile::encodeName(QFileInfo(to).absolutePath());
  const int fd = ::open(folder.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0 || errno == EINVAL || errno == EOPNOTSUPP;
  ::close(fd);
  return synced;
}

bool copyThenRemove(const QString& from, const QString& to) {
  struct stat entry {};
  if (::lstat(QFile::encodeName(from).constData(), &entry) != 0 || !S_ISREG(entry.st_mode)) {
    return false;
  }
  if (!exclusiveCopy(from, to, true)) return false;
  if ((relocationFaults.load() & ActionLauncher::FaultNoFolderSync) || !syncParentFolder(to)) {
    QFile::remove(to);
    return false;
  }
  const bool removed = !(relocationFaults.load() & ActionLauncher::FaultKeepOriginal) && QFile::remove(from);
  if (removed) return true;
  QFile::remove(to);
  return false;
}
} // namespace

void ActionLauncher::setRelocationFaultsForTesting(int faults) { relocationFaults.store(faults); }

bool ActionLauncher::relocateEntry(const QString& from, const QString& to, bool allowCopy) {
  const QByteArray source = QFile::encodeName(from);
  const QByteArray target = QFile::encodeName(to);
  const int faults = relocationFaults.load();
  // The kernel refuses to replace an existing |to|, with no gap between a
  // check and the move.
  int error = 0;
  if (faults & FaultCrossDevice) {
    error = EXDEV;
  } else if (faults & FaultNoReplace) {
    error = EINVAL;
  } else if (::renameat2(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), RENAME_NOREPLACE) == 0) {
    return true;
  } else {
    error = errno;
  }
  if (error == EINVAL || error == ENOSYS || error == EOPNOTSUPP) {
    // No RENAME_NOREPLACE here: a hard link fails on an existing name just
    // the same.
    int linkError = EPERM;
    if (!(faults & FaultNoLink)) {
      if (::link(source.constData(), target.constData()) == 0) {
        if (::unlink(source.constData()) == 0) return true;
        ::unlink(target.constData());
        return false;
      }
      linkError = errno;
    }
    if (linkError == EEXIST) return false;
    if (!allowCopy) {
      // A rename in the same folder: a copy here would hold the window for
      // a large video. The gap between this check and the rename is the
      // same one QFile::rename has.
      if (QFileInfo::exists(to)) return false;
      return ::rename(source.constData(), target.constData()) == 0;
    }
    // Nor hard links (FUSE, gvfs): a copy that never replaces a file.
    return copyThenRemove(from, to);
  }
  if (error == EXDEV) return copyThenRemove(from, to);
  return false;
}

QVariantMap ActionLauncher::checkTransfer(const QVariantMap& target, const QString& folder, bool copy) {
  const auto failure = [](const QString& message) {
    return QVariantMap{{u"ok"_s, false}, {u"error"_s, message}};
  };
  if (!capturedFileMatches(target)) {
    return failure(u"That file changed or disappeared. Open it again before moving it"_s);
  }
  const QFileInfo source(target.value(u"path"_s).toString());
  // The folder picker hands over a URL.
  const QString local = folder.startsWith(u"file:"_s) ? QUrl(folder).toLocalFile() : folder;
  const QFileInfo destination(local);
  if (local.isEmpty() || !destination.isDir()) return failure(u"That folder is not there"_s);
  if (!destination.isWritable()) return failure(u"That folder cannot be written to"_s);
  if (!copy && QDir(destination.absoluteFilePath()).canonicalPath() == source.dir().canonicalPath()) {
    return failure(u"The file is already in that folder"_s);
  }
  // A moved link could stop pointing at its file; the file it points at can
  // be moved from its own folder instead. A copy copies the picture itself.
  if (!copy && source.isSymLink()) {
    return failure(u"This is a link to a file elsewhere. Move that file instead"_s);
  }
  return {{u"ok"_s, true}, {u"folder"_s, destination.absoluteFilePath()}};
}

QVariantMap ActionLauncher::transferCapturedFile(const QVariantMap& target, const QString& folder,
                                                 bool copy, const QStringList& sidecars) {
  const QVariantMap checked = checkTransfer(target, folder, copy);
  if (!checked.value(u"ok"_s).toBool()) return checked;
  const auto failure = [](const QString& message) {
    return QVariantMap{{u"ok"_s, false}, {u"error"_s, message}};
  };
  const QFileInfo source(target.value(u"path"_s).toString());
  const QDir directory(checked.value(u"folder"_s).toString());
  const QString base = source.completeBaseName();
  const QString suffix = source.suffix().isEmpty() ? QString() : u"."_s + source.suffix();
  // The file's own name first, then "name (2)", "name (3)" and so on. Each
  // attempt refuses to replace a file that appeared in the meantime.
  for (int attempt = 1; attempt <= 999; ++attempt) {
    const QString fileName = attempt == 1 ? source.fileName()
                                          : u"%1 (%2)%3"_s.arg(base).arg(attempt).arg(suffix);
    const QString path = directory.filePath(fileName);
    if (QFileInfo(path).exists() || QFileInfo(path).isSymLink()) continue;
    if (!capturedFileMatches(target)) {
      return failure(u"That file changed or disappeared. Open it again before moving it"_s);
    }
    const bool done = copy ? exclusiveCopy(source.absoluteFilePath(), path, true)
                           : relocateEntry(source.absoluteFilePath(), path);
    if (!done) {
      if (QFileInfo(path).exists() || QFileInfo(path).isSymLink()) continue; // taken just now
      return failure(copy ? u"Could not copy this file there"_s : u"Could not move this file there"_s);
    }
    // Its sidecars follow under the name the file took: "<name>.<ext>.xmp"
    // the file name, "<name>.xmp" the base name. One that cannot go stays
    // where it is; the file has already moved and is not put back for it.
    int carried = 0;
    QString stayed;
    for (const QString& sidecar : sidecars) {
      const QString sidecarName = QFileInfo(sidecar).fileName();
      const bool fullName = sidecarName.chopped(4).compare(source.fileName(), Qt::CaseInsensitive) == 0;
      const QString stem = fullName ? fileName : QFileInfo(fileName).completeBaseName();
      const QString destination = directory.filePath(stem + sidecarName.right(4));
      const QFileInfo existing(destination);
      bool moved = false;
      if (!existing.exists() && !existing.isSymLink()) {
        moved = copy ? exclusiveCopy(sidecar, destination, true) : relocateEntry(sidecar, destination);
      }
      if (moved) {
        ++carried;
      } else if (stayed.isEmpty()) {
        const QString verb = copy ? u"was not copied"_s : u"stayed"_s;
        stayed = existing.exists() || existing.isSymLink()
                     ? u"Its sidecar %1 because %2 is already there"_s.arg(verb, existing.fileName())
                     : u"Its sidecar %1: could not %2 it"_s.arg(verb, copy ? u"copy"_s : u"move"_s);
      }
    }
    return {{u"ok"_s, true},
            {u"path"_s, path},
            {u"fileName"_s, fileName},
            {u"folder"_s, directory.absolutePath()},
            {u"mediaPath"_s, QFileInfo(path).canonicalFilePath()},
            {u"renamed"_s, attempt > 1},
            {u"sidecars"_s, carried},
            {u"sidecarNote"_s, stayed}};
  }
  return failure(u"Too many files with that name are already there"_s);
}

QVariantMap ActionLauncher::startTransfer(const QVariantMap& target, const QString& folder, bool copy) {
  const QString source = target.value(u"path"_s).toString();
  if (m_transferring.contains(source)) {
    return {{u"ok"_s, false}, {u"error"_s, u"That file is already being moved or copied"_s}};
  }
  // Refusals that need no more than a look are said at once.
  const QVariantMap checked = checkTransfer(target, folder, copy);
  if (!checked.value(u"ok"_s).toBool()) return checked;
  const int job = ++m_transferJob;
  m_transferring.insert(source);
  emit transferringChanged();
  auto* watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, job, source, target, copy] {
    QVariantMap result = watcher->result();
    watcher->deleteLater();
    m_transferring.remove(source);
    result.insert(u"copy"_s, copy);
    result.insert(u"source"_s, source);
    result.insert(u"viewedPath"_s, target.value(u"viewedPath"_s, source));
    emit transferringChanged();
    emit transferFinished(job, result);
  });
  // A large file on a slow or network disk takes a while; the window stays
  // live. Its sidecars are found here, without listing the folder.
  watcher->setFuture(QtConcurrent::run([target, folder, copy, sidecars = sidecarFiles(source)] {
    return transferCapturedFile(target, folder, copy, sidecars);
  }));
  return {{u"ok"_s, true}, {u"job"_s, job}};
}

bool ActionLauncher::capturedStillMatches(const QVariantMap& target) const {
  return !target.value(u"path"_s).toString().isEmpty() && capturedFileMatches(target);
}

bool ActionLauncher::sameCapturedEntry(const QVariantMap& left, const QVariantMap& right) {
  // One directory entry, however it was reached: a bind mount or a symlinked
  // folder gives the same entry another path, which a path or even a
  // canonical path comparison does not see.
  const auto value = [](const QVariantMap& map, QStringView key) {
    return map.value(key.toString()).toString();
  };
  return !value(left, u"name").isEmpty() && value(left, u"name") == value(right, u"name") &&
         !value(left, u"parentDevice").isEmpty() &&
         value(left, u"parentDevice") == value(right, u"parentDevice") &&
         value(left, u"parentInode") == value(right, u"parentInode");
}

void ActionLauncher::moveDuplicateCopiesToTrash(const QVariantMap& keep, const QVariantList& copies) {
  // Re-read the bytes on a worker. Cached duplicate groups only nominate
  // candidates; they never authorize moving a file.
  trashOthers(keep, copies, true, [this](int moved, const QStringList& skipped,
                                         const QVariantList& trashed) {
    if (!skipped.isEmpty()) emit failed(u"Skipped changed, unreadable or nonidentical copies:\n%1"_s
                                          .arg(skipped.join(QLatin1Char('\n'))));
    emit duplicateCleanupFinished(moved, skipped, trashed);
  });
}

void ActionLauncher::keepOneMoveOthersToTrash(const QVariantMap& keep, const QVariantList& others) {
  trashOthers(keep, others, false, [this](int moved, const QStringList& skipped,
                                          const QVariantList& trashed) {
    emit keepOneFinished(moved, skipped, trashed);
  });
}

void ActionLauncher::trashOthers(
    const QVariantMap& keep, const QVariantList& others, bool identicalOnly,
    std::function<void(int, const QStringList&, const QVariantList&)> done) {
  // Moving many files can take a while on a slow disk, so it runs on a
  // worker and reports back once.
  QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
  auto* watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [watcher, done] {
    const QVariantMap result = watcher->result();
    watcher->deleteLater();
    done(result.value(u"moved"_s).toInt(), result.value(u"skipped"_s).toStringList(),
         result.value(u"trashed"_s).toList());
  });
  QHash<QString, QStringList> sidecars;
  for (const QVariant& value : others) {
    const QString path = value.toMap().value(u"path"_s).toString();
    if (const QStringList found = sidecarFiles(path); !found.isEmpty()) sidecars.insert(path, found);
  }
  watcher->setFuture(QtConcurrent::run([keep, others, identicalOnly, sidecars] {
    int moved = 0;
    QStringList skipped;
    QVariantList trashed;
    const QString keepPath = keep.value(u"path"_s).toString();
    for (const QVariant& value : others) {
      const QVariantMap target = value.toMap();
      const QString path = target.value(u"path"_s).toString();
      // Never the kept file itself, by any path, and only while both are
      // still the files the prompt showed.
      bool movable = path != keepPath && !sameCapturedEntry(keep, target) &&
                     capturedFileMatches(keep) && capturedFileMatches(target);
      if (movable && identicalOnly) {
        QFile kept(keepPath), copy(path);
        movable = kept.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly) &&
                  kept.size() == copy.size();
        while (movable && !kept.atEnd()) {
          const QByteArray first = kept.read(1024 * 1024);
          const QByteArray second = copy.read(1024 * 1024);
          movable = !first.isEmpty() && first == second &&
                    kept.error() == QFileDevice::NoError && copy.error() == QFileDevice::NoError;
        }
        // Check versions again after reading, immediately before the move.
        movable = movable && copy.atEnd() && capturedFileMatches(keep) &&
                  capturedFileMatches(target);
      }
      QVariantMap record = movable ? trashWithSidecars(path, sidecars.value(path)) : QVariantMap();
      if (!record.isEmpty()) {
        ++moved;
        if (target.contains(u"viewedPath"_s)) record.insert(u"viewedPath"_s, target.value(u"viewedPath"_s));
        trashed.append(record);
      } else {
        skipped.append(path);
      }
    }
    return QVariantMap{{u"moved"_s, moved}, {u"skipped"_s, skipped}, {u"trashed"_s, trashed}};
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
  // Its .xmp sidecars take the new name in the same step: "<name>.<ext>.xmp"
  // follows the file name, "<name>.xmp" the base name. Their contents are not
  // touched.
  QList<QPair<QString, QString>> sidecars;
  for (const QString& sidecar : sidecarFiles(source.absoluteFilePath())) {
    const QString sidecarName = QFileInfo(sidecar).fileName();
    const bool fullName = sidecarName.chopped(4).compare(source.fileName(), Qt::CaseInsensitive) == 0;
    const QString renamed = source.dir().filePath((fullName ? fileName : name) + sidecarName.right(4));
    if (QFileInfo::exists(renamed)) {
      return failure(u"A sidecar named %1 already exists"_s.arg(QFileInfo(renamed).fileName()));
    }
    sidecars.append({sidecar, renamed});
  }
  if (!relocateEntry(source.absoluteFilePath(), target, false)) {
    return failure(u"Could not rename this file"_s);
  }
  for (qsizetype at = 0; at < sidecars.size(); ++at) {
    if (relocateEntry(sidecars.at(at).first, sidecars.at(at).second, false)) continue;
    // Put everything back rather than leave a sidecar behind its file.
    for (qsizetype back = at - 1; back >= 0; --back) {
      (void)relocateEntry(sidecars.at(back).second, sidecars.at(back).first, false);
    }
    (void)relocateEntry(target, source.absoluteFilePath(), false);
    return failure(u"Could not rename this file's sidecar"_s);
  }

  return {{QStringLiteral("ok"), true},
          {QStringLiteral("path"), target},
          {QStringLiteral("fileName"), fileName},
          {QStringLiteral("sidecars"), static_cast<int>(sidecars.size())}};
}
