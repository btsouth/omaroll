#include "theme/OmarchyTheme.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace {
QString defaultStateHome() {
  const QString configured = qEnvironmentVariable("XDG_STATE_HOME");
  return configured.isEmpty()
             ? QDir::homePath() + QStringLiteral("/.local/state")
             : configured;
}

QString defaultConfigHome() {
  const QString configured = qEnvironmentVariable("XDG_CONFIG_HOME");
  return configured.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                              : configured;
}

QJsonObject hyprlandOption(const QString &option) {
  const QString executable =
      QStandardPaths::findExecutable(QStringLiteral("hyprctl"));
  if (executable.isEmpty() ||
      qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE").isEmpty()) {
    return {};
  }

  QProcess process;
  process.start(executable,
                {QStringLiteral("getoption"), option, QStringLiteral("-j")});
  if (!process.waitForFinished(300) || process.exitCode() != 0) {
    return {};
  }

  const QJsonDocument document =
      QJsonDocument::fromJson(process.readAllStandardOutput());
  return document.isObject() ? document.object() : QJsonObject{};
}

double linearChannel(double channel) {
  channel /= 255.0;
  return channel <= 0.04045 ? channel / 12.92
                            : std::pow((channel + 0.055) / 1.055, 2.4);
}
} // namespace

OmarchyTheme::OmarchyTheme(QObject *parent)
    : OmarchyTheme(defaultStateHome(), defaultConfigHome(), parent) {}

OmarchyTheme::OmarchyTheme(QString stateHome, QString configHome,
                           QObject *parent)
    : QObject(parent), m_stateHome(std::move(stateHome)),
      m_configHome(std::move(configHome)) {
  m_reloadTimer.setSingleShot(true);
  m_reloadTimer.setInterval(60);

  connect(&m_reloadTimer, &QTimer::timeout, this, &OmarchyTheme::reload);
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this,
          [this] { scheduleReload(); });
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          [this] { scheduleReload(); });

  reload();
}

bool OmarchyTheme::omarchyAvailable() const { return m_omarchyAvailable; }
QString OmarchyTheme::themeName() const { return m_themeName; }
QString OmarchyTheme::mode() const { return m_mode; }
QString OmarchyTheme::fontFamily() const { return m_fontFamily; }
int OmarchyTheme::cornerRadius() const { return m_cornerRadius; }
int OmarchyTheme::gapsOut() const { return m_gapsOut; }
qreal OmarchyTheme::surfaceAlpha() const { return m_surfaceAlpha; }
QColor OmarchyTheme::surfaceBackground() const { return m_surfaceBackground; }
QColor OmarchyTheme::accent() const { return m_accent; }
QColor OmarchyTheme::selection() const { return m_selection; }
QColor OmarchyTheme::muted() const { return m_muted; }
QColor OmarchyTheme::background() const { return m_background; }
QColor OmarchyTheme::darkBackground() const { return m_darkBackground; }
QColor OmarchyTheme::darkerBackground() const { return m_darkerBackground; }
QColor OmarchyTheme::lighterBackground() const { return m_lighterBackground; }
QColor OmarchyTheme::foreground() const { return m_foreground; }
QColor OmarchyTheme::darkForeground() const { return m_darkForeground; }
QColor OmarchyTheme::lightForeground() const { return m_lightForeground; }
QColor OmarchyTheme::brightForeground() const { return m_brightForeground; }
QColor OmarchyTheme::mutedText() const { return m_mutedText; }
QColor OmarchyTheme::red() const { return m_red; }
QColor OmarchyTheme::yellow() const { return m_yellow; }
QColor OmarchyTheme::green() const { return m_green; }
QColor OmarchyTheme::cyan() const { return m_cyan; }
QColor OmarchyTheme::blue() const { return m_blue; }
QColor OmarchyTheme::magenta() const { return m_magenta; }

void OmarchyTheme::reload() {
  applyFallback();

  const QString colorsPath = themeRoot() + QStringLiteral("/colors.toml");
  const Values values = readSimpleToml(colorsPath);
  m_omarchyAvailable = !values.isEmpty();
  if (m_omarchyAvailable) {
    applyValues(values);
    const QString shellPath = themeRoot() + QStringLiteral("/shell.toml");
    m_surfaceBackground =
        readSectionColor(shellPath, QStringLiteral("launcher"),
                         QStringLiteral("background"), m_surfaceBackground);
    m_surfaceAlpha =
        readSectionAlpha(shellPath, QStringLiteral("launcher"),
                         QStringLiteral("background-alpha"), m_surfaceAlpha);

    QFile nameFile(currentRoot() + QStringLiteral("/theme.name"));
    if (nameFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
      const QString slug = QString::fromUtf8(nameFile.readAll()).trimmed();
      if (!slug.isEmpty()) {
        m_themeName = slug;
        m_themeName.replace(QLatin1Char('-'), QLatin1Char(' '));
        const QStringList words =
            m_themeName.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QStringList titled;
        titled.reserve(words.size());
        for (const QString &word : words) {
          QString transformed = word;
          transformed[0] = transformed.at(0).toUpper();
          titled.push_back(transformed);
        }
        m_themeName = titled.join(QLatin1Char(' '));
      }
    }
  }

  m_fontFamily = resolvedMonospaceFamily();
  refreshHyprlandMetrics();
  refreshWatchPaths();
  emit themeChanged();
}

QString OmarchyTheme::currentRoot() const {
  return m_stateHome + QStringLiteral("/omarchy/current");
}

QString OmarchyTheme::themeRoot() const {
  return currentRoot() + QStringLiteral("/theme");
}

OmarchyTheme::Values OmarchyTheme::readSimpleToml(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }

  static const QRegularExpression assignment(QStringLiteral(
      R"(^\s*([A-Za-z0-9_]+)\s*=\s*["']([^"']+)["']\s*(?:#.*)?$)"));
  Values values;
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QRegularExpressionMatch match = assignment.match(stream.readLine());
    if (match.hasMatch()) {
      values.insert(match.captured(1), match.captured(2));
    }
  }
  return values;
}

qreal OmarchyTheme::readSectionAlpha(const QString &path,
                                     const QString &section, const QString &key,
                                     qreal fallback) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return fallback;
  }
  const QRegularExpression sectionPattern(
      QStringLiteral(R"(^\s*\[([^]]+)\]\s*(?:#.*)?$)"));
  const QRegularExpression assignment(QStringLiteral(
      R"(^\s*([A-Za-z0-9_-]+)\s*=\s*([0-9]*\.?[0-9]+)\s*(?:#.*)?$)"));
  QString currentSection;
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QString line = stream.readLine();
    const QRegularExpressionMatch sectionMatch = sectionPattern.match(line);
    if (sectionMatch.hasMatch()) {
      currentSection = sectionMatch.captured(1);
      continue;
    }
    if (currentSection != section) {
      continue;
    }
    const QRegularExpressionMatch valueMatch = assignment.match(line);
    if (valueMatch.hasMatch() && valueMatch.captured(1) == key) {
      bool okay = false;
      const qreal value = valueMatch.captured(2).toDouble(&okay);
      return okay ? std::clamp(value, 0.0, 1.0) : fallback;
    }
  }
  return fallback;
}

QColor OmarchyTheme::readSectionColor(const QString &path,
                                      const QString &section,
                                      const QString &key,
                                      const QColor &fallback) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return fallback;
  }
  const QRegularExpression sectionPattern(
      QStringLiteral(R"(^\s*\[([^]]+)\]\s*(?:#.*)?$)"));
  const QRegularExpression assignment(QStringLiteral(
      R"(^\s*([A-Za-z0-9_-]+)\s*=\s*["']([^"']+)["']\s*(?:#.*)?$)"));
  QString currentSection;
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QString line = stream.readLine();
    const QRegularExpressionMatch sectionMatch = sectionPattern.match(line);
    if (sectionMatch.hasMatch()) {
      currentSection = sectionMatch.captured(1);
      continue;
    }
    if (currentSection != section) {
      continue;
    }
    const QRegularExpressionMatch valueMatch = assignment.match(line);
    if (valueMatch.hasMatch() && valueMatch.captured(1) == key) {
      const QColor color(valueMatch.captured(2));
      return color.isValid() ? color : fallback;
    }
  }
  return fallback;
}

QColor OmarchyTheme::parsedColor(const Values &values, const QString &key,
                                 const QColor &fallback) {
  const QColor candidate(values.value(key));
  return candidate.isValid() ? candidate : fallback;
}

QString OmarchyTheme::resolvedMonospaceFamily() {
  const QFontInfo resolved(QFont(QStringLiteral("monospace")));
  return resolved.family().isEmpty() ? QStringLiteral("monospace")
                                     : resolved.family();
}

double OmarchyTheme::contrastRatio(const QColor &first, const QColor &second) {
  const auto luminance = [](const QColor &color) {
    return 0.2126 * linearChannel(color.red()) +
           0.7152 * linearChannel(color.green()) +
           0.0722 * linearChannel(color.blue());
  };
  const double firstLuminance = luminance(first);
  const double secondLuminance = luminance(second);
  const double brightest = std::max(firstLuminance, secondLuminance);
  const double darkest = std::min(firstLuminance, secondLuminance);
  return (brightest + 0.05) / (darkest + 0.05);
}

void OmarchyTheme::applyFallback() {
  m_omarchyAvailable = false;
  m_themeName = QStringLiteral("Omaroll Dark");
  m_mode = QStringLiteral("dark");
  m_surfaceAlpha = 0.82;
  m_surfaceBackground = QColor(QStringLiteral("#090f0d"));
  m_accent = QColor(QStringLiteral("#78a98f"));
  m_selection = QColor(QStringLiteral("#31453b"));
  m_muted = QColor(QStringLiteral("#53685b"));
  m_background = QColor(QStringLiteral("#111c18"));
  m_darkBackground = QColor(QStringLiteral("#0c1512"));
  m_darkerBackground = QColor(QStringLiteral("#090f0d"));
  m_lighterBackground = QColor(QStringLiteral("#23372b"));
  m_foreground = QColor(QStringLiteral("#c1c497"));
  m_darkForeground = QColor(QStringLiteral("#81b8a8"));
  m_lightForeground = QColor(QStringLiteral("#d6d5bc"));
  m_brightForeground = QColor(QStringLiteral("#f7e8b2"));
  m_mutedText = m_darkForeground;
  m_red = QColor(QStringLiteral("#ff7166"));
  m_yellow = QColor(QStringLiteral("#e5c736"));
  m_green = QColor(QStringLiteral("#63b07a"));
  m_cyan = QColor(QStringLiteral("#8cd3cb"));
  m_blue = QColor(QStringLiteral("#7da6ff"));
  m_magenta = QColor(QStringLiteral("#d2689c"));
}

void OmarchyTheme::applyValues(const Values &values) {
  m_mode = values.value(QStringLiteral("mode"), m_mode);
  m_accent = parsedColor(values, QStringLiteral("accent"), m_accent);
  m_selection = parsedColor(values, QStringLiteral("selection"), m_selection);
  m_muted = parsedColor(values, QStringLiteral("muted"), m_muted);
  m_background =
      parsedColor(values, QStringLiteral("background"), m_background);
  m_darkBackground =
      parsedColor(values, QStringLiteral("dark_background"), m_darkBackground);
  m_darkerBackground = parsedColor(values, QStringLiteral("darker_background"),
                                   m_darkerBackground);
  m_surfaceBackground = m_darkerBackground;
  m_lighterBackground = parsedColor(
      values, QStringLiteral("lighter_background"), m_lighterBackground);
  m_foreground =
      parsedColor(values, QStringLiteral("foreground"), m_foreground);
  m_darkForeground =
      parsedColor(values, QStringLiteral("dark_foreground"), m_darkForeground);
  m_lightForeground = parsedColor(values, QStringLiteral("light_foreground"),
                                  m_lightForeground);
  m_brightForeground = parsedColor(values, QStringLiteral("bright_foreground"),
                                   m_brightForeground);
  m_red = parsedColor(values, QStringLiteral("red"), m_red);
  m_yellow = parsedColor(values, QStringLiteral("yellow"), m_yellow);
  m_green = parsedColor(values, QStringLiteral("green"), m_green);
  m_cyan = parsedColor(values, QStringLiteral("cyan"), m_cyan);
  m_blue = parsedColor(values, QStringLiteral("blue"), m_blue);
  m_magenta = parsedColor(values, QStringLiteral("magenta"), m_magenta);

  m_mutedText = contrastRatio(m_darkForeground, m_background) >= 3.0
                    ? m_darkForeground
                    : m_lightForeground;
}

void OmarchyTheme::refreshHyprlandMetrics() {
  const QJsonObject rounding =
      hyprlandOption(QStringLiteral("decoration:rounding"));
  if (rounding.contains(QStringLiteral("int"))) {
    m_cornerRadius = std::max(0, rounding.value(QStringLiteral("int")).toInt());
  }

  const QJsonObject gaps = hyprlandOption(QStringLiteral("general:gaps_out"));
  const QString css = gaps.value(QStringLiteral("css")).toString();
  static const QRegularExpression number(
      QStringLiteral(R"((-?\d+(?:\.\d+)?))"));
  const QRegularExpressionMatch match = number.match(css);
  if (match.hasMatch()) {
    m_gapsOut = std::max(0, qRound(match.captured(1).toDouble() / 2.0));
  }
}

void OmarchyTheme::refreshWatchPaths() {
  if (!m_watcher.files().isEmpty()) {
    m_watcher.removePaths(m_watcher.files());
  }
  if (!m_watcher.directories().isEmpty()) {
    m_watcher.removePaths(m_watcher.directories());
  }

  const QStringList candidates = {
      m_stateHome,
      m_stateHome + QStringLiteral("/omarchy"),
      currentRoot(),
      themeRoot(),
      themeRoot() + QStringLiteral("/colors.toml"),
      themeRoot() + QStringLiteral("/shell.toml"),
      currentRoot() + QStringLiteral("/theme.name"),
      m_configHome + QStringLiteral("/fontconfig"),
      m_configHome + QStringLiteral("/fontconfig/fonts.conf"),
  };

  QStringList existing;
  for (const QString &path : candidates) {
    if (QFileInfo::exists(path)) {
      existing.push_back(path);
    }
  }
  if (!existing.isEmpty()) {
    m_watcher.addPaths(existing);
  }
}

void OmarchyTheme::scheduleReload() { m_reloadTimer.start(); }
