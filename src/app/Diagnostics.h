#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>

// The text behind Copy diagnostics and `omaroll --diagnostics`. Only bounded
// identifiers and presence flags: never executable paths, environment values,
// media names or user data. Everything is read from files that need no root
// and no running compositor, so it also works when no window can open.
namespace Diagnostics {

// Where the facts come from. Tests point these at fake trees.
struct Sources {
  QString pacmanLocal = QStringLiteral("/var/lib/pacman/local");
  QString omarchyPath;  // $OMARCHY_PATH, else /usr/share/omarchy
  QString drm = QStringLiteral("/sys/class/drm");
  QString modules = QStringLiteral("/sys/module");
  QString stateHome;    // ~/.local/state
  QString configHome;   // ~/.config
  std::function<bool(const QString&)> hasProgram;  // PATH lookup by default
  // Where tesseract looks for language data, and the languages Omaroll asks
  // it for ($OMARCHY_OCR_LANGS, joined with +).
  QStringList tessdata = {QStringLiteral("/usr/share/tessdata")};
  QString ocrLanguages = QStringLiteral("eng");

  static Sources system();
};

// An optional program Omaroll calls, what it enables and where it comes from.
struct Tool {
  QString program;
  QString feature;
  QString package;  // a pacman package, or "omarchy" for Omarchy's helpers
};

const QList<Tool>& tools();

// Missing optional tools, one entry per package, Omarchy's own helpers left
// out, then any OCR language data an installed tesseract lacks. Each map has
// program, feature, package and install keys.
QVariantList missing(const Sources& sources);

// "0.57.1-2" for an installed pacman package, empty when absent.
QString packageVersion(const QString& pacmanLocal, const QString& name);
QString omarchyVersion(const Sources& sources);
QString hyprlandVersion(const Sources& sources);
// One line per display adapter: vendor, PCI id, kernel driver and version.
QStringList graphics(const Sources& sources);
// The active Omarchy theme from its files, for when no theme object exists.
QString themeName(const Sources& sources);

// The full report. platform is the Qt platform plugin, or a note that none
// was started.
QString report(const Sources& sources, const QString& platform, const QString& theme,
               qreal scale);

} // namespace Diagnostics
