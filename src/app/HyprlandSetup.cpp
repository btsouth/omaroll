#include "app/HyprlandSetup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <cctype>

namespace {

// The first line of a rules file Omaroll wrote, so it can be replaced or
// removed later without touching one the user wrote.
constexpr char kMarker[] = "-- Installed by Omaroll.";
// The comment above the require. The second form also says that install ended
// the file's last line, so removal can take that line break out again.
constexpr char kRequireComment[] = "-- Omaroll window rules. `omaroll --remove-window-rules` removes them.";
constexpr char kRequireCommentJoined[] =
    "-- Omaroll window rules, after a line break Omaroll added. "
    "`omaroll --remove-window-rules` removes both.";
// An optional require: a missing file is skipped instead of breaking the
// whole config, while an error inside it still shows. It does what Omarchy's
// default.hypr.require_optional does, without needing an Omarchy new enough to
// have it.
constexpr char kRequire[] =
    "if package.searchpath(\"hypr.omaroll\", package.path) then require(\"hypr.omaroll\") end";
// The same through Omarchy's helper, as earlier builds of this change wrote.
constexpr char kOmarchyRequire[] = "require(\"default.hypr.require_optional\").module(\"hypr.omaroll\")";

QString configPath(const QString& directory) { return directory + QStringLiteral("/hyprland.lua"); }
QString rulesPath(const QString& directory) { return directory + QStringLiteral("/omaroll.lua"); }

QByteArray readAll(const QString& path, bool* ok = nullptr) {
  QFile file(path);
  const bool opened = file.open(QIODevice::ReadOnly);
  if (ok) *ok = opened;
  return opened ? file.readAll() : QByteArray();
}

enum class Write { Done, Failed, Changed };

// Replaces the file only if it still holds `expected`, so an editor save made
// while Omaroll was working is never overwritten. QSaveFile writes through a
// symlinked config to its target and keeps the old file whole if anything
// fails.
Write writeIfUnchanged(const QString& path, const QByteArray& expected, const QByteArray& bytes) {
#ifdef OMAROLL_TESTING
  if (HyprlandSetup::beforeConfigWrite) HyprlandSetup::beforeConfigWrite();
#endif
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return Write::Failed;
  bool ok = false;
  if (readAll(path, &ok) != expected || !ok) {
    file.cancelWriting();
    return Write::Changed;
  }
  return file.commit() ? Write::Done : Write::Failed;
}

bool writeAll(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

// The level of a Lua long bracket opening at `at` ("[[" is 0, "[==[" is 2),
// or -1.
int longBracket(const QByteArray& text, qsizetype at) {
  if (at >= text.size() || text.at(at) != '[') return -1;
  qsizetype index = at + 1;
  while (index < text.size() && text.at(index) == '=') ++index;
  return index < text.size() && text.at(index) == '[' ? int(index - at - 1) : -1;
}

// The text with every Lua comment and long string blanked to spaces and its
// line breaks kept, so a line can be read for what it runs. Quoted strings
// stay as they are, since a require names its module in one.
QByteArray code(const QByteArray& text) {
  QByteArray out = text;
  qsizetype index = 0;
  const auto blank = [&](qsizetype from, qsizetype to) {
    for (qsizetype at = from; at < to && at < out.size(); ++at) {
      if (out.at(at) != '\n' && out.at(at) != '\r') out[at] = ' ';
    }
  };
  const auto closeLong = [&](qsizetype from, int level) {
    const QByteArray close = ']' + QByteArray(level, '=') + ']';
    const qsizetype end = text.indexOf(close, from);
    return end < 0 ? text.size() : end + close.size();
  };
  while (index < text.size()) {
    const char c = text.at(index);
    if (c == '-' && index + 1 < text.size() && text.at(index + 1) == '-') {
      const int level = longBracket(text, index + 2);
      qsizetype end;
      if (level >= 0) {
        end = closeLong(index + 4 + level, level);
      } else {
        end = text.indexOf('\n', index);
        if (end < 0) end = text.size();
      }
      blank(index, end);
      index = end;
    } else if (c == '"' || c == '\'') {
      qsizetype at = index + 1;
      while (at < text.size() && text.at(at) != c && text.at(at) != '\n') {
        at += text.at(at) == '\\' ? 2 : 1;
      }
      index = at + 1;
    } else if (const int level = longBracket(text, index); level >= 0) {
      const qsizetype end = closeLong(index + 2 + level, level);
      blank(index, end);
      index = end;
    } else {
      ++index;
    }
  }
  return out;
}

// Lines with their terminators, so joining them gives the bytes back.
QList<QByteArray> lines(const QByteArray& text) {
  QList<QByteArray> out;
  qsizetype start = 0;
  while (start < text.size()) {
    qsizetype end = text.indexOf('\n', start);
    end = end < 0 ? text.size() : end + 1;
    out << text.mid(start, end - start);
    start = end;
  }
  return out;
}

// What a line runs loads hypr/omaroll.lua.
bool loadsRules(const QByteArray& codeLine) {
  return codeLine.contains("require") &&
         (codeLine.contains("\"hypr.omaroll\"") || codeLine.contains("'hypr.omaroll'"));
}

// A whole line that does nothing but load the rules: Omaroll's own require or
// the one the README used to suggest. Only these are ever removed.
bool requireLine(const QByteArray& line, const QByteArray& codeLine) {
  const QByteArray trimmed = line.trimmed();
  if (codeLine.trimmed() != trimmed) return false;  // a comment is involved
  return trimmed == kRequire || trimmed == kOmarchyRequire || trimmed == "require(\"hypr.omaroll\")" ||
         trimmed == "require('hypr.omaroll')";
}

bool commentLine(const QByteArray& line) {
  const QByteArray trimmed = line.trimmed();
  return trimmed == kRequireComment || trimmed == kRequireCommentJoined;
}

// 1-based numbers of the lines still loading the rules.
QList<int> loadingLines(const QByteArray& config) {
  QList<int> found;
  const QList<QByteArray> codeLines = lines(code(config));
  for (qsizetype index = 0; index < codeLines.size(); ++index) {
    if (loadsRules(codeLines.at(index))) found << int(index + 1);
  }
  return found;
}

bool omarchyConfig(const QByteArray& config) {
  return code(config).contains("default.hypr.omarchy");
}

// A line whose code starts with the return keyword.
bool returnLine(const QByteArray& codeLine) {
  return codeLine.startsWith("return") &&
         (codeLine.size() == 6 || !(std::isalnum(static_cast<unsigned char>(codeLine.at(6))) ||
                                    codeLine.at(6) == '_'));
}

constexpr qsizetype kNoPlace = -1;

// Where install's block goes: the end, or the start of a closing one-line
// top-level return, since Lua allows nothing after one. kNoPlace for any
// other top-level return, which Omaroll can't safely place a line around.
qsizetype insertionPoint(const QByteArray& config) {
  const QList<QByteArray> rawLines = lines(config);
  const QList<QByteArray> codeLines = lines(code(config));
  qsizetype last = codeLines.size() - 1;
  while (last >= 0 && codeLines.at(last).trimmed().isEmpty()) --last;
  if (last >= 0 && returnLine(codeLines.at(last).trimmed())) {
    for (qsizetype index = 0; index < last; ++index) {
      if (returnLine(codeLines.at(index))) return kNoPlace;
    }
    qsizetype at = 0;
    for (qsizetype index = 0; index < last; ++index) at += rawLines.at(index).size();
    return at;
  }
  // A return written flush left that doesn't end the file: a multi-line
  // return, or one in a function Omaroll can't tell from the chunk.
  for (const QByteArray& line : codeLines) {
    if (returnLine(line)) return kNoPlace;
  }
  return config.size();
}

bool ours(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  return file.readLine().startsWith(kMarker);
}

QByteArray block(bool joined, const QByteArray& eol) {
  return (joined ? eol : QByteArray()) + eol +
         (joined ? kRequireCommentJoined : kRequireComment) + eol + kRequire + eol;
}

// The config without what install added: its exact block first, then any
// lone require or comment line moved elsewhere.
QByteArray withoutRules(QByteArray config) {
  for (const bool joined : {true, false}) {
    for (const QByteArray eol : {QByteArrayLiteral("\r\n"), QByteArrayLiteral("\n")}) {
      const QByteArray added = block(joined, eol);
      const qsizetype at = config.lastIndexOf(added);
      if (at < 0) continue;
      // The added line break only goes if nothing was written after the block;
      // otherwise the last line and the next would run together.
      const bool atEnd = at + added.size() == config.size();
      config.remove(at, added.size());
      if (joined && !atEnd) config.insert(at, eol);
    }
  }
  QList<QByteArray> kept = lines(config);
  const QList<QByteArray> codeLines = lines(code(config));
  for (qsizetype index = kept.size() - 1; index >= 0; --index) {
    if (commentLine(kept.at(index)) || requireLine(kept.at(index), codeLines.at(index))) {
      kept.removeAt(index);
    }
  }
  return kept.join();
}

QString keyName(QString key) {
  key = key.trimmed();
  const QString upper = key.toUpper();
  if (upper == u"PRINT") return QStringLiteral("Print Screen");
  if (upper == u"SUPER") return QStringLiteral("Super");
  if (upper == u"CTRL" || upper == u"CONTROL") return QStringLiteral("Ctrl");
  if (upper == u"SHIFT") return QStringLiteral("Shift");
  if (upper == u"ALT") return QStringLiteral("Alt");
  if (key.size() == 1) return upper;
  return key.left(1).toUpper() + key.mid(1).toLower();
}

QString chord(const QString& keys) {
  QStringList names;
  for (const QString& key : keys.split(QLatin1Char('+'), Qt::SkipEmptyParts)) names << keyName(key);
  return names.join(QStringLiteral(" + "));
}

QStringList codeLinesOf(const QString& path) {
  return QString::fromUtf8(code(readAll(path).left(256 * 1024))).split(QLatin1Char('\n'));
}

} // namespace

namespace HyprlandSetup {

#ifdef OMAROLL_TESTING
std::function<void()> beforeConfigWrite;
#endif

QString defaultDirectory() {
  // Omarchy's bootstrap adds ~/.config to Lua's path, whatever
  // XDG_CONFIG_HOME says, so the module has to live there.
  return QDir::homePath() + QStringLiteral("/.config/hypr");
}

QByteArray bundledRules() { return readAll(QStringLiteral(":/omaroll/hypr/omaroll.lua")); }

RulesState rulesState(const QString& hyprDirectory) {
  bool ok = false;
  const QByteArray config = readAll(configPath(hyprDirectory), &ok);
  if (!ok || !omarchyConfig(config)) return RulesState::Unsupported;
  // A require whose rules file has gone loads nothing; installing again
  // writes the file without a second require.
  return loadingLines(config).isEmpty() || !QFile::exists(rulesPath(hyprDirectory))
             ? RulesState::NotInstalled
             : RulesState::Installed;
}

Result installRules(const QString& hyprDirectory) {
  bool ok = false;
  const QByteArray config = readAll(configPath(hyprDirectory), &ok);
  if (!ok || !omarchyConfig(config)) {
    return {false, QStringLiteral("Window rules need Omarchy's Hyprland config in ~/.config/hypr/hyprland.lua.")};
  }
  const QByteArray rules = bundledRules();
  if (rules.isEmpty()) return {false, QStringLiteral("The window rules are missing from this build.")};
  const bool already = !loadingLines(config).isEmpty();
  const qsizetype at = already ? config.size() : insertionPoint(config);
  if (at == kNoPlace) {
    return {false, QStringLiteral("~/.config/hypr/hyprland.lua has a return Omaroll can't add a line "
                                  "around, so nothing was changed. Add require(\"hypr.omaroll\") "
                                  "before it, then install again.")};
  }
  // Install adds one block in the file's own line endings, at the end or
  // before a closing return, and says in its comment whether it had to end
  // the last line first.
  const QByteArray eol = config.contains("\r\n") ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n");
  const bool joined = at == config.size() && !config.isEmpty() && !config.endsWith('\n');
  QByteArray edited = config;
  if (!already) edited.insert(at, block(joined, eol));
  // A block comment or long string left open runs to the end of the file, so
  // a line added there would load nothing.
  if (loadingLines(edited).isEmpty()) {
    return {false, QStringLiteral("~/.config/hypr/hyprland.lua ends inside a comment or string that "
                                  "is never closed, so nothing was changed. Close it, then install again.")};
  }

  const QString file = rulesPath(hyprDirectory);
  // A link to nothing: writing would create its target, which removal could
  // not tell apart from the user's own.
  if (const QFileInfo info(file); info.isSymLink() && !info.exists()) {
    return {false, QStringLiteral("~/.config/hypr/omaroll.lua is a link to a file that isn't there, "
                                  "so nothing was changed. Fix or remove the link, then try again.")};
  }
  const bool existed = QFile::exists(file);
  bool keptTheirs = false;
  if (existed && !ours(file)) {
    keptTheirs = true;
  } else {
    const QByteArray contents =
        QByteArray(kMarker) +
        " Remove it with `omaroll --remove-window-rules`;\n"
        "-- a later install replaces it, so keep your own rules in another file.\n\n" +
        rules;
    if (!writeAll(file, contents)) {
      return {false, QStringLiteral("Could not write ~/.config/hypr/omaroll.lua.")};
    }
  }

  if (!already) {
    const Write written = writeIfUnchanged(configPath(hyprDirectory), config, edited);
    if (written != Write::Done) {
      // Nothing loads a rules file written just now, so it goes again.
      if (!existed) QFile::remove(file);
      return {false, written == Write::Changed
                         ? QStringLiteral("~/.config/hypr/hyprland.lua changed while Omaroll was "
                                          "editing it, so it was left alone. Try again.")
                         : QStringLiteral("Could not add the rules to ~/.config/hypr/hyprland.lua.")};
    }
  }
  QString message = !already ? QStringLiteral("Window rules installed. New Omaroll windows open at "
                                              "full opacity and the viewer floats.")
                    : existed ? QStringLiteral("Window rules are already installed.")
                              : QStringLiteral("Window rules restored: ~/.config/hypr/omaroll.lua "
                                               "was missing.");
  if (keptTheirs) message += QStringLiteral(" Your own ~/.config/hypr/omaroll.lua was kept.");
  return {true, message};
}

Result removeRules(const QString& hyprDirectory) {
  bool ok = false;
  const QByteArray config = readAll(configPath(hyprDirectory), &ok);
  // An unreadable config may still load the rules; deleting them then would
  // break it.
  if (!ok && QFileInfo::exists(configPath(hyprDirectory))) {
    return {false, QStringLiteral("Could not read ~/.config/hypr/hyprland.lua, so nothing was removed.")};
  }
  bool changed = false;
  QList<int> remaining;
  if (ok) {
    const QByteArray edited = withoutRules(config);
    remaining = loadingLines(edited);
    if (edited != config) {
      const Write written = writeIfUnchanged(configPath(hyprDirectory), config, edited);
      if (written == Write::Changed) {
        return {false, QStringLiteral("~/.config/hypr/hyprland.lua changed while Omaroll was "
                                      "editing it, so it was left alone. Try again.")};
      }
      if (written == Write::Failed) return {false, QStringLiteral("Could not edit ~/.config/hypr/hyprland.lua.")};
      changed = true;
    }
  }
  const QString file = rulesPath(hyprDirectory);
  bool keptTheirs = false;
  bool keptOurs = false;
  if (QFile::exists(file)) {
    if (!ours(file)) {
      keptTheirs = true;
    } else if (!remaining.isEmpty()) {
      // A line Omaroll won't edit still loads it; a plain require would fail
      // without the file.
      keptOurs = true;
    } else {
      if (!QFile::remove(file)) return {false, QStringLiteral("Could not remove ~/.config/hypr/omaroll.lua.")};
      changed = true;
    }
  }
  QString message = changed ? QStringLiteral("Window rules removed.")
                            : QStringLiteral("Window rules were not installed.");
  if (!remaining.isEmpty()) {
    QStringList numbers;
    for (const int line : std::as_const(remaining)) numbers << QString::number(line);
    const QString left =
        remaining.size() == 1
            ? QStringLiteral("Line %1 of ~/.config/hypr/hyprland.lua still loads them, so it was left "
                             "for you to edit.")
            : QStringLiteral("Lines %1 of ~/.config/hypr/hyprland.lua still load them, so they were "
                             "left for you to edit.");
    message = (changed ? message + u' ' : QString()) + left.arg(numbers.join(QStringLiteral(", ")));
    if (keptOurs) message += QStringLiteral(" ~/.config/hypr/omaroll.lua stays until then.");
  }
  if (keptTheirs) message += QStringLiteral(" Your own ~/.config/hypr/omaroll.lua was kept.");
  return {true, message};
}

QString screenshotKey(const QString& hyprDirectory, const QString& omarchyPath) {
  // o.bind or o.rebind of Omarchy's screenshot command, with a description or
  // nil, in either kind of quotes.
  static const QRegularExpression bind(QStringLiteral(
      R"re(^\s*o\.(?:re)?bind\(\s*(["'])(.+?)\1\s*,\s*(?:"[^"]*"|'[^']*'|nil)\s*,\s*(["'])omarchy-capture-screenshot(?:\s[^"']*)?\3\s*[,)])re"));
  // Anything that drops a default chord: an unbind, or a rebind to something
  // else.
  static const QRegularExpression unbind(
      QStringLiteral(R"re(^\s*(?:(?:hl|o)\.unbind|o\.rebind)\(\s*(["'])(.+?)\1)re"));
  static const QRegularExpression noDefaults(
      QStringLiteral(R"re(^\s*(?:_G\.)?omarchy_default_bindings\s*=\s*false\b)re"));

  // The user's bindings win over Omarchy's defaults, and a later unbind or
  // rebind wins over an earlier binding. hyprland.lua loads bindings.lua
  // after the defaults, so what it drops counts after bindings.lua too.
  QSet<QString> dropped;
  QStringList bound;
  const QStringList config = codeLinesOf(configPath(hyprDirectory));
  for (const QString& line : codeLinesOf(hyprDirectory + QStringLiteral("/bindings.lua")) + config) {
    if (const auto match = unbind.match(line); match.hasMatch()) {
      const QString keys = chord(match.captured(2));
      dropped << keys;
      bound.removeAll(keys);
    }
    if (const auto match = bind.match(line); match.hasMatch()) {
      const QString keys = chord(match.captured(2));
      if (!keys.isEmpty() && !bound.contains(keys)) bound << keys;
    }
  }
  if (!bound.isEmpty()) return bound.first();
  for (const QString& line : config) {
    if (noDefaults.match(line).hasMatch()) return {};
  }
  const QString root = omarchyPath.isEmpty() ? QStringLiteral("/usr/share/omarchy") : omarchyPath;
  const QString defaults = root + QStringLiteral("/default/hypr/bindings/utilities.lua");
  if (!QFile::exists(defaults)) return QStringLiteral("Print Screen");
  for (const QString& line : codeLinesOf(defaults)) {
    const auto match = bind.match(line);
    if (!match.hasMatch()) continue;
    const QString keys = chord(match.captured(2));
    if (!keys.isEmpty() && !dropped.contains(keys)) return keys;
  }
  return {};
}

} // namespace HyprlandSetup
