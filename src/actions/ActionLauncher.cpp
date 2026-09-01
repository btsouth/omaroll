#include "actions/ActionLauncher.h"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

ActionLauncher::ActionLauncher(QObject* parent) : QObject(parent) {}

QString ActionLauncher::mimeTypeFor(const QString& path) {
  static QMimeDatabase database;
  const QString name = database.mimeTypeForFile(path).name();
  return name.isEmpty() ? QStringLiteral("application/octet-stream") : name;
}

bool ActionLauncher::handlerAvailable(const QString& program) const {
  return !QStandardPaths::findExecutable(program).isEmpty();
}

bool ActionLauncher::run(const QString& program, const QStringList& arguments) {
  const QString executable = QStandardPaths::findExecutable(program);
  if (executable.isEmpty()) {
    emit failed(QStringLiteral("%1 is not installed").arg(program));
    return false;
  }

  if (!QProcess::startDetached(executable, arguments)) {
    emit failed(QStringLiteral("Could not start %1").arg(program));
    return false;
  }
  return true;
}

bool ActionLauncher::runDetached(const QString& program, const QStringList& arguments,
                                 const QString& packageHint) {
  const QString executable = QStandardPaths::findExecutable(program);
  if (executable.isEmpty()) {
    emit failed(packageHint.isEmpty()
                    ? QStringLiteral("%1 is not installed").arg(program)
                    : QStringLiteral("%1 is not installed. Try: sudo pacman -S %2")
                          .arg(program, packageHint));
    return false;
  }

  if (!QProcess::startDetached(executable, arguments)) {
    emit failed(QStringLiteral("Could not start %1").arg(program));
    return false;
  }
  return true;
}

bool ActionLauncher::captureTextTo(const QString& program, const QStringList& arguments) {
  const QString executable = QStandardPaths::findExecutable(program);
  if (executable.isEmpty()) {
    emit failed(QStringLiteral("%1 is not installed").arg(program));
    return false;
  }

  QProcess process;
  process.start(executable, arguments);
  if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit) {
    emit failed(QStringLiteral("%1 did not finish").arg(program));
    return false;
  }

  const QString text = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
  if (text.isEmpty()) {
    // Not an error: plenty of screenshots have no text in them, and saying so
    // is more useful than an empty clipboard and silence.
    emit reported(QStringLiteral("No text found"));
    return true;
  }

  QGuiApplication::clipboard()->setText(text);
  const int words = static_cast<int>(text.split(QRegularExpression(QStringLiteral("\\s+")),
                                                Qt::SkipEmptyParts)
                                         .size());
  emit reported(QStringLiteral("Copied %1 %2").arg(words).arg(words == 1 ? "word" : "words"));
  return true;
}

bool ActionLauncher::open(const QString& path) {
  if (!QFileInfo::exists(path)) {
    emit failed(QStringLiteral("That file is no longer there"));
    return false;
  }
  return run(QStringLiteral("xdg-open"), {path});
}

bool ActionLauncher::moveToTrash(const QString& path) {
  if (!QFileInfo::exists(path)) {
    emit failed(QStringLiteral("That file is no longer there"));
    return false;
  }

  QFile file(path);
  if (!file.moveToTrash()) {
    // Most often a trash directory that does not exist on the volume the file
    // lives on, which is worth saying rather than hiding behind "failed".
    emit failed(QStringLiteral("Could not move to trash: %1").arg(file.errorString()));
    return false;
  }
  return true;
}

bool ActionLauncher::showInFiles(const QString& path) {
  const QFileInfo info(path);
  if (!info.exists()) {
    emit failed(QStringLiteral("That file is no longer there"));
    return false;
  }

  // Nautilus is Omarchy's file manager and can select the file itself, which is
  // the behaviour people expect from "show in files". Opening the containing
  // directory is the honest fallback everywhere else.
  if (handlerAvailable(QStringLiteral("nautilus"))) {
    return run(QStringLiteral("nautilus"), {QStringLiteral("--select"), info.absoluteFilePath()});
  }
  return run(QStringLiteral("xdg-open"), {info.absolutePath()});
}
