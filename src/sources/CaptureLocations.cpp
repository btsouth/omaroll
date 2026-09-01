#include "sources/CaptureLocations.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString home() { return QDir::homePath(); }

QString normalizedUserDir(QString value) {
  if (value.startsWith(QStringLiteral("$HOME"))) {
    value.replace(0, 5, home());
  } else if (QDir::isRelativePath(value)) {
    value = home() + QLatin1Char('/') + value;
  }
  // xdg-user-dirs uses $HOME to mean "disabled". Scanning it recursively
  // would pull media from repositories, caches and unrelated applications.
  return QDir::cleanPath(value) == QDir::cleanPath(home()) ? QString() : value;
}

// XDG user dirs live in a shell-fragment file rather than anywhere Qt reads, and
// QStandardPaths does not honour a relocated XDG_PICTURES_DIR on every setup, so
// parse user-dirs.dirs directly. Values look like:
//   XDG_PICTURES_DIR="$HOME/Pictures"
QString xdgUserDir(const QString& key) {
  const QString configured = qEnvironmentVariable(key.toUtf8().constData());
  if (!configured.isEmpty()) {
    return normalizedUserDir(configured);
  }

  const QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME").isEmpty()
                                 ? home() + QStringLiteral("/.config")
                                 : qEnvironmentVariable("XDG_CONFIG_HOME");

  QFile file(configHome + QStringLiteral("/user-dirs.dirs"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }

  const QRegularExpression assignment(
      QStringLiteral(R"RX(^\s*%1\s*=\s*"([^"]*)"\s*$)RX").arg(QRegularExpression::escape(key)));

  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QRegularExpressionMatch match = assignment.match(stream.readLine());
    if (!match.hasMatch()) {
      continue;
    }
    return normalizedUserDir(match.captured(1));
  }
  return {};
}

// Resolve the first non-empty candidate, falling back to a directory under
// $HOME. Never returns empty, so callers can scan unconditionally.
QString resolve(const QString& override, const QString& xdgKey, const QString& fallbackLeaf) {
  const QString overridden = qEnvironmentVariable(override.toUtf8().constData());
  if (!overridden.isEmpty()) {
    return overridden;
  }
  const QString xdg = xdgUserDir(xdgKey);
  if (!xdg.isEmpty()) {
    return xdg;
  }
  return home() + QLatin1Char('/') + fallbackLeaf;
}

} // namespace

namespace CaptureLocations {

QString screenshots() {
  return resolve(QStringLiteral("OMARCHY_SCREENSHOT_DIR"), QStringLiteral("XDG_PICTURES_DIR"),
                 QStringLiteral("Pictures"));
}

QString recordings() {
  return resolve(QStringLiteral("OMARCHY_SCREENRECORD_DIR"), QStringLiteral("XDG_VIDEOS_DIR"),
                 QStringLiteral("Videos"));
}

QString pictures() {
  return resolve({}, QStringLiteral("XDG_PICTURES_DIR"), QStringLiteral("Pictures"));
}

QString videos() {
  return resolve({}, QStringLiteral("XDG_VIDEOS_DIR"), QStringLiteral("Videos"));
}

QString downloads() {
  return resolve({}, QStringLiteral("XDG_DOWNLOAD_DIR"), QStringLiteral("Downloads"));
}

} // namespace CaptureLocations
