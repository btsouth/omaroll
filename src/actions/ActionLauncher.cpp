#include "actions/ActionLauncher.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

ActionLauncher::ActionLauncher(QObject* parent) : QObject(parent) {}

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

bool ActionLauncher::open(const QString& path) {
  if (!QFileInfo::exists(path)) {
    emit failed(QStringLiteral("That file is no longer there"));
    return false;
  }
  return run(QStringLiteral("xdg-open"), {path});
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
