#pragma once

#include <QByteArray>
#include <QString>

// Omaroll's place in an Omarchy Hyprland config: its optional window rules
// (full opacity and a floating viewer) and the screenshot key named on the
// first-run screen. Nothing here runs hyprctl or touches a running session.
namespace HyprlandSetup {

enum class RulesState {
  Installed,     // hyprland.lua loads hypr/omaroll.lua
  NotInstalled,  // an Omarchy Lua config without it
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
// Writes hypr/omaroll.lua and adds one optional require to hyprland.lua.
// A file there that Omaroll did not write is kept.
Result installRules(const QString& hyprDirectory);
// Takes the require back out, and the file if Omaroll wrote it.
Result removeRules(const QString& hyprDirectory);

// "Print Screen", or whatever key the Hyprland bindings give Omarchy's
// screenshot command. The user's bindings win over Omarchy's defaults.
QString screenshotKey(const QString& hyprDirectory, const QString& omarchyPath);

} // namespace HyprlandSetup
