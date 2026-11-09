#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

// Omaroll's place in an Omarchy Hyprland config: its optional window rules
// (full opacity and a floating viewer) and the screenshot key named on the
// first-run screen. Nothing here runs hyprctl or touches a running session.
namespace HyprlandSetup {

enum class RulesState {
  Installed,     // hyprland.lua loads hypr/omaroll.lua, and it is there
  NotInstalled,  // an Omarchy Lua config without it, or the file has gone
  Unsupported,   // no Omarchy Lua config to add it to
};

struct Result {
  bool ok = false;
  QString message;
};

// ~/.config/hypr, where Omarchy's bootstrap looks for "hypr.*" modules.
QString defaultDirectory();
// The rules file as shipped, from the binary's resources.
QByteArray bundledRules();

RulesState rulesState(const QString& hyprDirectory);
// Writes hypr/omaroll.lua and appends one optional require to hyprland.lua.
// A file there that Omaroll did not write is kept. hyprland.lua is only
// replaced if nothing else changed it meanwhile.
Result installRules(const QString& hyprDirectory);
// Takes back out exactly what install added, so hyprland.lua comes back byte
// for byte, plus any line that is nothing but a require of hypr.omaroll. Other
// lines that load the rules are reported, not edited, and keep the file. An
// unreadable hyprland.lua stops it before anything changes.
Result removeRules(const QString& hyprDirectory);

// "Print Screen", or whatever key the Hyprland bindings give Omarchy's
// screenshot command. The user's bindings, unbinds and rebinds win over
// Omarchy's defaults; empty when no key takes a screenshot.
QString screenshotKey(const QString& hyprDirectory, const QString& omarchyPath);

#ifdef OMAROLL_TESTING
// Runs between reading hyprland.lua and replacing it, so a test can edit the
// file the way an editor save would.
extern std::function<void()> beforeConfigWrite;
#endif

} // namespace HyprlandSetup
