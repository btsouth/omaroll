#include "app/MimeDefaults.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>

namespace {
const QString desktop = QStringLiteral("io.github.tsouth89.omaroll.desktop");
bool xdgMime(const QStringList &args, QString *output = nullptr) {
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
MimeDefaults::MimeDefaults(QObject *parent) : QObject(parent) {
  connect(&m_change, &QFutureWatcher<Result>::finished, this, [this] {
    const Result result = m_change.result();
    m_applying = false;
    m_succeeded = result.ok;
    emit finished(result.ok, summarize(m_unset, result.report, result.ok),
                  result.report);
    refresh();
  });
  connect(&m_check, &QFutureWatcher<State>::finished, this, [this] {
    const State state = m_check.result();
    m_checking = false;
    // Asked again while this ran: what it read may be from before a change.
    if (m_recheck) {
      m_recheck = false;
      refresh();
      return;
    }
    m_known = true;
    m_imagesDefault = state.images;
    m_videosDefault = state.videos;
    emit stateChanged();
  });
}

// Workers only run xdg-mime and touch files, never this object.
MimeDefaults::~MimeDefaults() {
  m_change.waitForFinished();
  m_check.waitForFinished();
}

QString MimeDefaults::apply(bool unset, const QString &group) {
  const Result result = run(unset, group);
  m_succeeded = result.ok;
  return result.report;
}

void MimeDefaults::applyAsync(bool unset, const QString &group) {
  if (m_applying) return;
  m_applying = true;
  m_unset = unset;
  emit stateChanged();
  m_change.setFuture(QtConcurrent::run([unset, group] { return run(unset, group); }));
}

void MimeDefaults::refresh() {
  if (m_checking) {
    m_recheck = true;
    return;
  }
  m_checking = true;
  emit stateChanged();
  m_check.setFuture(QtConcurrent::run([] { return query(); }));
}

MimeDefaults::State MimeDefaults::query() {
  State state;
  QString handler;
  state.images = xdgMime({"query", "default", "image/jpeg"}, &handler) && handler == desktop;
  state.videos = xdgMime({"query", "default", "video/mp4"}, &handler) && handler == desktop;
  return state;
}

QString MimeDefaults::summarize(bool unset, const QString &report, bool ok) {
  int changed = 0;
  int failed = 0;
  int unchanged = 0;
  QStringList apps;
  const auto name = [](QString handler) {
    handler = handler.trimmed();
    if (handler.endsWith(u".desktop")) handler.chop(8);
    // org.gnome.Loupe reads better as Loupe.
    const auto dot = handler.lastIndexOf(u'.');
    return dot >= 0 && dot + 1 < handler.size() ? handler.mid(dot + 1) : handler;
  };
  for (const auto &line : report.split(u'\n', Qt::SkipEmptyParts)) {
    if (line.startsWith(u"Could not") || line.startsWith(u"No matching")) {
      ++failed;
      continue;
    }
    const auto colon = line.indexOf(u": ");
    if (colon < 0) continue;
    const QString outcome = line.mid(colon + 2);
    if (!unset && outcome.contains(u" -> ")) {
      ++changed;
      const QString previous = outcome.left(outcome.indexOf(u" -> "));
      if (previous != u"(none)" && !apps.contains(name(previous)))
        apps << name(previous);
    } else if (unset && outcome.startsWith(u"restored ")) {
      ++changed;
      const QString restored = name(outcome.mid(9));
      if (!apps.contains(restored)) apps << restored;
    } else if (unset && outcome == u"removed Omaroll") {
      ++changed;
    } else {
      ++unchanged;
    }
  }
  const auto list = [](QStringList names) {
    if (names.size() > 3) {
      const int more = int(names.size()) - 3;
      names = names.mid(0, 3);
      names << QStringLiteral("%1 more").arg(more);
    }
    if (names.size() < 2) return names.join(QString());
    const QString last = names.takeLast();
    return names.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last;
  };
  const auto types = [](int count) {
    return count == 1 ? QStringLiteral("1 type") : QStringLiteral("%1 types").arg(count);
  };
  QString summary;
  if (!unset) {
    if (changed == 0 && failed == 0)
      summary = QStringLiteral("Omaroll already opens your pictures and videos.");
    else if (changed > 0)
      summary = QStringLiteral("Omaroll now opens %1 of picture and video%2.")
                    .arg(types(changed),
                         apps.isEmpty() ? QString()
                                        : QStringLiteral(", replacing ") + list(apps));
  } else {
    if (changed == 0 && failed == 0)
      summary = QStringLiteral("Nothing to restore. Omaroll was not the default.");
    else if (changed > 0)
      summary = apps.isEmpty()
                    ? QStringLiteral("Omaroll is no longer the default for %1.").arg(types(changed))
                    : QStringLiteral("Restored %1 for %2.").arg(list(apps), types(changed));
  }
  if (failed > 0) {
    const QString failure = QStringLiteral("%1 could not be changed.").arg(types(failed));
    summary = summary.isEmpty() ? failure : summary + u' ' + failure;
  }
  if (summary.isEmpty())
    summary = ok ? QStringLiteral("Done.") : QStringLiteral("Could not change the defaults.");
  return summary;
}

MimeDefaults::Result MimeDefaults::run(bool unset, const QString &group) {
  const QString path =
      QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                             QStringLiteral("applications/") + desktop);
  const auto selected = types(path, group);
  if (selected.isEmpty())
    return {QStringLiteral("No matching installed desktop types (group: %1)")
                .arg(group),
            false};
  const QString config =
      QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  QSettings state(config + QStringLiteral("/Omaroll/mime-defaults.ini"),
                  QSettings::IniFormat);
  QStringList report;
  bool ok = true;
  for (const auto &type : selected) {
    QString previous;
    if (!xdgMime({"query", "default", type}, &previous)) {
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
          !xdgMime({"default", desktop, type})) {
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
        removed = xdgMime({"default", restore, type});
      // A system-wide default, or Omaroll being the only installed app for
      // the type, can still open it with Omaroll; say so instead of claiming
      // it was removed.
      QString now;
      if (removed && restore.isEmpty() && xdgMime({"query", "default", type}, &now) &&
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
  return {report.join(u'\n'), ok && state.status() == QSettings::NoError};
}
