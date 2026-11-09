#include "app/MimeDefaults.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {
const QString desktop = QStringLiteral("io.github.tsouth89.omaroll.desktop");
bool run(const QStringList &args, QString *output = nullptr) {
  QProcess process;
  process.start(QStringLiteral("xdg-mime"), args);
  if (!process.waitForFinished(5000)) {
    process.kill();
    process.waitForFinished();
    return false;
  }
  if (output)
    *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
  return process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}
// Remove only our tokens, preserving other handlers, groups and comments.
bool removeEntries(const QString &path, const QStringList &types) {
  QFile input(path);
  if (!input.exists())
    return true;
  if (!input.open(QIODevice::ReadOnly))
    return false;
  auto lines = QString::fromUtf8(input.readAll()).split(u'\n');
  input.close();
  bool changed = false;
  for (auto &line : lines) {
    const auto at = line.indexOf(u'=');
    if (at < 0 || !types.contains(line.left(at).trimmed()))
      continue;
    auto handlers = line.mid(at + 1).split(u';', Qt::SkipEmptyParts);
    if (!handlers.removeAll(desktop))
      continue;
    line = handlers.isEmpty() ? QString()
                              : line.left(at + 1) + handlers.join(u';') + u';';
    changed = true;
  }
  if (!changed)
    return true;
  QSaveFile output(path);
  const auto bytes = lines.join(u'\n').toUtf8();
  return output.open(QIODevice::WriteOnly) &&
         output.write(bytes) == bytes.size() && output.commit();
}
} // namespace
QStringList MimeDefaults::types(const QString &path, const QString &group) {
  if (!QStringList{"images", "videos", "pdf", "all"}.contains(group))
    return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  QStringList result;
  for (const auto &line : QString::fromUtf8(file.readAll()).split(u'\n')) {
    if (!line.startsWith(u"MimeType="))
      continue;
    for (const auto &type :
         line.mid(9).trimmed().split(u';', Qt::SkipEmptyParts)) {
      const bool image = type.startsWith(u"image/");
      const bool video =
          type.startsWith(u"video/") || type == u"application/vnd.ms-asf";
      if ((image && (group == u"images" || group == u"all")) ||
          (video && (group == u"videos" || group == u"all")) ||
          (type == u"application/pdf" && group == u"pdf"))
        result.append(type);
    }
  }
  result.removeDuplicates();
  return result;
}
QString MimeDefaults::apply(bool unset, const QString &group) {
  m_succeeded = false;
  const QString path =
      QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                             QStringLiteral("applications/") + desktop);
  const auto selected = types(path, group);
  if (selected.isEmpty())
    return QStringLiteral("No matching installed desktop types (group: %1)")
        .arg(group);
  const QString config =
      QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  QSettings state(config + QStringLiteral("/Omaroll/mime-defaults.ini"),
                  QSettings::IniFormat);
  QStringList report;
  bool ok = true;
  for (const auto &type : selected) {
    QString previous;
    if (!run({"query", "default", type}, &previous)) {
      ok = false;
      report << "Could not query " + type;
      continue;
    }
    if (!unset) {
      if (previous == desktop) {
        report << type + ": already Omaroll";
        continue;
      }
      // The handler being replaced now, which may be newer than one recorded
      // by an earlier --set-default.
      state.setValue(type, previous);
      state.sync();
      if (state.status() != QSettings::NoError ||
          !run({"default", desktop, type})) {
        ok = false;
        report << "Could not set " + type;
        continue;
      }
      report << type + ": " + (previous.isEmpty() ? "(none)" : previous) +
                    " -> " + desktop;
    } else {
      const QString restore = state.value(type).toString();
      bool removed = true;
      // Only write user-owned configuration, never system defaults.
      const QStringList dirs = {config,
                                QStandardPaths::writableLocation(
                                    QStandardPaths::GenericDataLocation) +
                                    "/applications"};
      const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                                .toLower()
                                .split(u':', Qt::SkipEmptyParts);
      for (const auto &dir : dirs) {
        removed &= removeEntries(dir + "/mimeapps.list", {type});
        removed &= removeEntries(dir + "/defaults.list", {type});
        for (const auto &name : desktops)
          removed &= removeEntries(dir + "/" + name + "-mimeapps.list", {type});
      }
      if (removed && previous == desktop && !restore.isEmpty())
        removed = run({"default", restore, type});
      // A system-wide default, or Omaroll being the only installed app for
      // the type, can still open it with Omaroll; say so instead of claiming
      // it was removed.
      QString now;
      if (removed && restore.isEmpty() && run({"query", "default", type}, &now) &&
          now == desktop) {
        state.remove(type);
        report << type + ": removed your setting; it still opens with Omaroll "
                         "(a system default, or no other app handles it)";
        continue;
      }
      if (!removed) {
        ok = false;
        report << "Could not unset " + type;
        continue;
      }
      state.remove(type);
      report << type + (previous == desktop
                            ? (restore.isEmpty() ? ": removed Omaroll"
                                                 : ": restored " + restore)
                            : ": kept " + previous);
    }
  }
  state.sync();
  m_succeeded = ok && state.status() == QSettings::NoError;
  return report.join(u'\n');
}
