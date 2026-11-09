#include "app/MimeDefaults.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>

#include <optional>

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
// Rewrites one list file through MimeDefaults::withoutOmaroll.
bool removeEntries(const QString &path, const QString &type,
                   const QString &original = {}, bool *restored = nullptr) {
  QFile input(path);
  if (!input.exists())
    return true;
  if (!input.open(QIODevice::ReadOnly))
    return false;
  const QByteArray bytes = input.readAll();
  if (input.error() != QFileDevice::NoError || bytes.size() != input.size())
    return false;
  const QString before = QString::fromUtf8(bytes);
  input.close();
  const QString after = MimeDefaults::withoutOmaroll(before, type, original, restored);
  if (after == before)
    return true;
  QSaveFile output(path);
  const auto written = after.toUtf8();
  return output.open(QIODevice::WriteOnly) &&
         output.write(written) == written.size() && output.commit();
}
// A list that is not there reads as empty. One that is there but cannot be
// read whole reads as nothing at all, so no decision is made from it.
std::optional<QString> readText(const QString &path) {
  QFile file(path);
  if (!file.exists())
    return QString();
  if (!file.open(QIODevice::ReadOnly))
    return std::nullopt;
  const QByteArray bytes = file.readAll();
  if (file.error() != QFileDevice::NoError || bytes.size() != file.size())
    return std::nullopt;
  return QString::fromUtf8(bytes);
}
const QString kFileExisted = QStringLiteral("file/existed");
const QString kFileSection = QStringLiteral("file/section");
const QString kFileNewline = QStringLiteral("file/newline");
bool hasDefaultsSection(const QString &text) {
  for (const auto &line : text.split(u'\n'))
    if (line.trimmed() == u"[Default Applications]")
      return true;
  return false;
}
bool writeText(const QString &path, const QString &text) {
  QSaveFile output(path);
  const auto bytes = text.toUtf8();
  return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() &&
         output.commit();
}
// Once no type is left to restore: what xdg-mime added around Omaroll's
// lines goes too, and the record of the file before is dropped.
bool restoreStructure(QSettings &state, const QString &userList) {
  if (!state.contains(kFileExisted))
    return true;
  for (const auto &key : state.allKeys())
    if (!key.startsWith(u"lines/") && !key.startsWith(u"file/"))
      return true;
  const bool existed = state.value(kFileExisted).toBool();
  const bool section = state.value(kFileSection).toBool();
  const bool newline = state.value(kFileNewline).toBool();
  bool ok = true;
  if (QFile::exists(userList)) {
    // Unreadable now: leave its structure as it is and keep the record for
    // the next try.
    const std::optional<QString> text = readText(userList);
    if (!text)
      return false;
    const QString after = MimeDefaults::withoutAddedStructure(*text, section, newline);
    if (!existed && after.isEmpty())
      ok = QFile::remove(userList);
    else if (after != *text)
      ok = writeText(userList, after);
  }
  if (ok)
    state.remove(QStringLiteral("file"));
  return ok;
}
QString recordKey(const QString &type) { return QStringLiteral("lines/") + type; }
QStringList handlersOf(const QString &line) {
  const auto at = line.indexOf(u'=');
  return at < 0 ? QStringList() : line.mid(at + 1).split(u';', Qt::SkipEmptyParts);
}
} // namespace
QString MimeDefaults::defaultLine(const QString &text, const QString &type) {
  bool inDefaults = false;
  for (const auto &line : text.split(u'\n')) {
    if (line.startsWith(u'[')) {
      inDefaults = line.trimmed() == u"[Default Applications]";
      continue;
    }
    // The line xdg-mime replaces in place: exactly "type=" at its start.
    if (inDefaults && line.startsWith(type + u'='))
      return line;
  }
  return {};
}
QString MimeDefaults::withoutOmaroll(const QString &text, const QString &type,
                                     const QString &original, bool *restored) {
  auto lines = text.split(u'\n');
  bool inDefaults = false;
  bool putBack = false;
  // The saved line itself may list Omaroll behind another app; it comes
  // back without it.
  QString saved = original;
  if (const QString savedEntry = original.chopped(original.endsWith(u'\r') ? 1 : 0);
      handlersOf(savedEntry).contains(desktop)) {
    auto handlers = handlersOf(savedEntry);
    handlers.removeAll(desktop);
    saved = handlers.isEmpty()
                ? QString()
                : savedEntry.left(savedEntry.indexOf(u'=') + 1) + handlers.join(u';') + u';' +
                      (original.endsWith(u'\r') ? QStringLiteral("\r") : QString());
  }
  bool inRemoved = false;
  for (qsizetype index = 0; index < lines.size();) {
    QString &line = lines[index];
    if (line.startsWith(u'[')) {
      inDefaults = line.trimmed() == u"[Default Applications]";
      // The user's exclusions keep Omaroll out of lower lists; they stay.
      inRemoved = line.trimmed() == u"[Removed Associations]";
      ++index;
      continue;
    }
    if (inRemoved) {
      ++index;
      continue;
    }
    // Put back by an earlier unset whose other lists failed: a retry finds it
    // in place and leaves it, rather than writing a single handler over it.
    if (inDefaults && !putBack && !saved.isEmpty() && line == saved) {
      putBack = true;
      ++index;
      continue;
    }
    // A CRLF list: parse without the carriage return, and keep it.
    const QString ending = line.endsWith(u'\r') ? QStringLiteral("\r") : QString();
    const QString entry = line.chopped(ending.size());
    const auto at = entry.indexOf(u'=');
    if (at < 0 || entry.left(at).trimmed() != type) {
      ++index;
      continue;
    }
    auto handlers = handlersOf(entry);
    if (!handlers.contains(desktop)) {
      ++index;
      continue;
    }
    // The line xdg-mime wrote over goes back exactly as it was.
    if (inDefaults && !putBack && !saved.isEmpty() && handlers == QStringList{desktop}) {
      line = saved;
      putBack = true;
      ++index;
      continue;
    }
    handlers.removeAll(desktop);
    if (handlers.isEmpty()) {
      // Deleted, not blanked, so cycles leave no empty lines behind.
      lines.removeAt(index);
      continue;
    }
    line = entry.left(at + 1) + handlers.join(u';') + u';' + ending;
    ++index;
  }
  if (restored)
    *restored = putBack;
  return lines.join(u'\n');
}
QString MimeDefaults::withoutAddedStructure(const QString &text, bool hadSection,
                                            bool endedWithNewline) {
  QString result = text;
  // xdg-mime appends a blank line and the header to a list without one.
  // Only an empty section is taken out; anything the user put in it stays.
  if (!hadSection) {
    static const QRegularExpression appended(
        QStringLiteral("\n\\[Default Applications\\]\n(\n*)\\z"));
    result.replace(appended, QStringLiteral("\\1"));
  }
  // And it ends every line with a newline, the last one included.
  if (!endedWithNewline && result.endsWith(u'\n'))
    result.chop(1);
  return result;
}
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
    emit finished(result.ok, summarize(m_unset, result.report, result.ok, m_group),
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
  m_group = group;
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

QString MimeDefaults::summarize(bool unset, const QString &report, bool ok,
                                const QString &group) {
  int changed = 0;
  int failed = 0;
  int unchanged = 0;
  // Types whose setting was removed but still open with Omaroll, from a system
  // default or no other app.
  int still = 0;
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
    } else if (unset && outcome.contains(u"still opens with Omaroll")) {
      ++still;
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
  const QString kind = group == u"images" ? QStringLiteral("picture")
                       : group == u"videos" ? QStringLiteral("video")
                       : group == u"pdf"    ? QStringLiteral("PDF")
                                            : QStringLiteral("picture and video");
  const QString plural = group == u"images" ? QStringLiteral("pictures")
                         : group == u"videos" ? QStringLiteral("videos")
                         : group == u"pdf"    ? QStringLiteral("PDFs")
                                              : QStringLiteral("pictures and videos");
  QString summary;
  if (!unset) {
    if (changed == 0 && failed == 0)
      summary = QStringLiteral("Omaroll already opens your %1.").arg(plural);
    else if (changed > 0)
      summary = QStringLiteral("Omaroll now opens %1 of %2%3.")
                    .arg(types(changed), kind,
                         apps.isEmpty() ? QString()
                                        : QStringLiteral(", replacing ") + list(apps));
  } else {
    if (changed == 0 && failed == 0 && still == 0)
      summary = QStringLiteral("Nothing to restore. Omaroll was not the default.");
    else if (changed > 0)
      summary = apps.isEmpty()
                    ? QStringLiteral("Omaroll is no longer the default for %1.").arg(types(changed))
                    : QStringLiteral("Restored %1 for %2.").arg(list(apps), types(changed));
  }
  if (still > 0) {
    const QString remains =
        QStringLiteral("%1 still %2 with Omaroll, from a system default or because no other app "
                       "handles %3.")
            .arg(types(still), still == 1 ? QStringLiteral("opens") : QStringLiteral("open"),
                 still == 1 ? QStringLiteral("it") : QStringLiteral("them"));
    summary = summary.isEmpty() ? remains : summary + u' ' + remains;
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
  const QString userList = config + QStringLiteral("/mimeapps.list");
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
      // A list that cannot be read is left alone: nothing could be put back.
      const std::optional<QString> read = readText(userList);
      if (!read) {
        ok = false;
        report << "Could not set " + type + " (could not read " + userList + ")";
        continue;
      }
      const QString &before = *read;
      // The handler being replaced now, which may be newer than one recorded
      // by an earlier --set-default.
      state.setValue(type, previous);
      // How the user's list looked before Omaroll's first change, so that
      // once every type is restored the file is back byte for byte.
      if (!state.contains(kFileExisted)) {
        state.setValue(kFileExisted, QFile::exists(userList));
        state.setValue(kFileSection, hasDefaultsSection(before));
        state.setValue(kFileNewline, before.isEmpty() || before.endsWith(u'\n'));
      }
      // And the exact line xdg-mime is about to overwrite, whichever of its
      // apps answers now, so --unset-default puts it back as it was. A line
      // that is only Omaroll's, from an earlier --set-default, never replaces
      // the user's line saved then.
      const QString line = defaultLine(before, type);
      const bool omarollsOwn = handlersOf(line.chopped(line.endsWith(u'\r') ? 1 : 0)) ==
                               QStringList{desktop};
      if (omarollsOwn && state.contains(recordKey(type))) {
        // Keep it.
      } else if (!line.isEmpty() && !omarollsOwn) {
        state.setValue(recordKey(type), line);
      } else {
        state.remove(recordKey(type));
      }
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
      // Put back whenever the line is still exactly the one xdg-mime wrote,
      // even if another app answers now through a list that ranks higher.
      const QString original = state.value(recordKey(type)).toString();
      bool removed = true;
      // Only write user-owned configuration, never system defaults. Each
      // list is handled on its own: one that cannot be read is left as it is
      // (and the type reported as not unset), while Omaroll is still taken
      // out of the others, which keeps the saved record for a retry.
      const QStringList dirs = {config,
                                QStandardPaths::writableLocation(
                                    QStandardPaths::GenericDataLocation) +
                                    "/applications"};
      const auto desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                                .toLower()
                                .split(u':', Qt::SkipEmptyParts);
      bool restoredLine = false;
      for (const auto &dir : dirs) {
        const QString list = dir + "/mimeapps.list";
        removed &= removeEntries(list, type, list == userList ? original : QString(),
                                 list == userList ? &restoredLine : nullptr);
        removed &= removeEntries(dir + "/defaults.list", type);
        for (const auto &name : desktops)
          removed &= removeEntries(dir + "/" + name + "-mimeapps.list", type);
      }
      // Written only when the user's own line did not come back and the
      // files do not already give that answer: a handler that came from a
      // system default needs no line of its own, and a restored list may
      // rightly answer with an app ahead of the one recorded.
      QString restoredNow;
      if (removed && previous == desktop && !restore.isEmpty() && !restoredLine &&
          !(xdgMime({"query", "default", type}, &restoredNow) && restoredNow == restore))
        removed = xdgMime({"default", restore, type});
      // A system-wide default, or Omaroll being the only installed app for
      // the type, can still open it with Omaroll; say so instead of claiming
      // it was removed.
      QString now;
      if (removed && restore.isEmpty() && xdgMime({"query", "default", type}, &now) &&
          now == desktop) {
        state.remove(type);
        state.remove(recordKey(type));
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
      state.remove(recordKey(type));
      report << type + (previous == desktop
                            ? (restore.isEmpty() ? ": removed Omaroll"
                                                 : ": restored " + restore)
                            : ": kept " + previous);
    }
  }
  if (unset)
    ok &= restoreStructure(state, userList);
  state.sync();
  return {report.join(u'\n'), ok && state.status() == QSettings::NoError};
}
