#include "app/HyprlandSetup.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

namespace {

// The first line of a rules file Omaroll wrote, so it can be replaced or
// removed later without touching one the user wrote.
constexpr char kMarker[] = "-- Installed by Omaroll.";
constexpr char kRequireComment[] = "-- Omaroll window rules. `omaroll --remove-window-rules` removes them.";
// Omarchy's optional require: a missing file is skipped instead of breaking
// the whole config, while an error inside it still shows.
constexpr char kRequire[] = "require(\"default.hypr.require_optional\").module(\"hypr.omaroll\")";

QString configPath(const QString& directory) { return directory + QStringLiteral("/hyprland.lua"); }
QString rulesPath(const QString& directory) { return directory + QStringLiteral("/omaroll.lua"); }

QByteArray readAll(const QString& path, bool* ok = nullptr) {
  QFile file(path);
  const bool opened = file.open(QIODevice::ReadOnly);
  if (ok) *ok = opened;
  return opened ? file.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& bytes) {
  // QSaveFile writes through a symlinked config to its target and keeps the
  // old file whole if anything fails.
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

// A line that loads hypr/omaroll.lua, ours or one the README suggested.
bool loadsRules(const QString& line) {
  const QString trimmed = line.trimmed();
  if (trimmed.startsWith(QStringLiteral("--"))) return false;
  return trimmed.contains(QStringLiteral("require")) &&
         (trimmed.contains(QStringLiteral("\"hypr.omaroll\"")) ||
          trimmed.contains(QStringLiteral("'hypr.omaroll'")));
}

bool omarchyConfig(const QByteArray& config) {
  return config.contains("default.hypr.omarchy");
}

bool ours(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  return file.readLine().startsWith(kMarker);
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

} // namespace

namespace HyprlandSetup {

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
  for (const QString& line : QString::fromUtf8(config).split(QLatin1Char('\n'))) {
    if (loadsRules(line)) return RulesState::Installed;
  }
  return RulesState::NotInstalled;
}

Result installRules(const QString& hyprDirectory) {
  bool ok = false;
  QByteArray config = readAll(configPath(hyprDirectory), &ok);
  if (!ok || !omarchyConfig(config)) {
    return {false, QStringLiteral("Window rules need Omarchy's Hyprland config in ~/.config/hypr/hyprland.lua.")};
  }
  const QByteArray rules = bundledRules();
  if (rules.isEmpty()) return {false, QStringLiteral("The window rules are missing from this build.")};

  const QString file = rulesPath(hyprDirectory);
  bool keptTheirs = false;
  if (QFile::exists(file) && !ours(file)) {
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

  const bool already = rulesState(hyprDirectory) == RulesState::Installed;
  if (!already) {
    if (!config.isEmpty() && !config.endsWith('\n')) config += '\n';
    config += QByteArray("\n") + kRequireComment + '\n' + kRequire + '\n';
    if (!writeAll(configPath(hyprDirectory), config)) {
      return {false, QStringLiteral("Could not add the rules to ~/.config/hypr/hyprland.lua.")};
    }
  }
  QString message = already ? QStringLiteral("Window rules are already installed.")
                            : QStringLiteral("Window rules installed. New Omaroll windows open at "
                                             "full opacity and the viewer floats.");
  if (keptTheirs) message += QStringLiteral(" Your own ~/.config/hypr/omaroll.lua was kept.");
  return {true, message};
}

Result removeRules(const QString& hyprDirectory) {
  bool ok = false;
  const QByteArray config = readAll(configPath(hyprDirectory), &ok);
  bool changed = false;
  if (ok) {
    QStringList lines = QString::fromUtf8(config).split(QLatin1Char('\n'));
    for (qsizetype index = lines.size() - 1; index >= 0; --index) {
      const QString line = lines.at(index);
      if (!loadsRules(line) && line != QLatin1String(kRequireComment)) continue;
      lines.removeAt(index);
      changed = true;
      // The blank line install put before its comment.
      if (line == QLatin1String(kRequireComment) && index > 0 && lines.at(index - 1).isEmpty()) {
        lines.removeAt(--index);
      }
    }
    if (changed && !writeAll(configPath(hyprDirectory), lines.join(QLatin1Char('\n')).toUtf8())) {
      return {false, QStringLiteral("Could not edit ~/.config/hypr/hyprland.lua.")};
    }
  }
  const QString file = rulesPath(hyprDirectory);
  bool keptTheirs = false;
  if (QFile::exists(file)) {
    if (ours(file)) {
      if (!QFile::remove(file)) return {false, QStringLiteral("Could not remove ~/.config/hypr/omaroll.lua.")};
      changed = true;
    } else {
      keptTheirs = true;
    }
  }
  QString message = changed ? QStringLiteral("Window rules removed.")
                            : QStringLiteral("Window rules were not installed.");
  if (keptTheirs) message += QStringLiteral(" Your own ~/.config/hypr/omaroll.lua was kept.");
  return {true, message};
}

QString screenshotKey(const QString& hyprDirectory, const QString& omarchyPath) {
  static const QRegularExpression bind(QStringLiteral(
      R"re(^\s*o\.bind\(\s*"([^"]+)"\s*,\s*"[^"]*"\s*,\s*"omarchy-capture-screenshot(?:\s[^"]*)?"\s*[,)])re"));
  const QString root = omarchyPath.isEmpty() ? QStringLiteral("/usr/share/omarchy") : omarchyPath;
  for (const QString& path : {hyprDirectory + QStringLiteral("/bindings.lua"),
                              root + QStringLiteral("/default/hypr/bindings/utilities.lua")}) {
    const QString text = QString::fromUtf8(readAll(path).left(256 * 1024));
    for (const QString& line : text.split(QLatin1Char('\n'))) {
      const QRegularExpressionMatch match = bind.match(line);
      if (!match.hasMatch()) continue;
      QStringList keys;
      for (const QString& key : match.captured(1).split(QLatin1Char('+'), Qt::SkipEmptyParts)) {
        keys << keyName(key);
      }
      if (!keys.isEmpty()) return keys.join(QStringLiteral(" + "));
    }
  }
  return QStringLiteral("Print Screen");
}

} // namespace HyprlandSetup
