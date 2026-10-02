#pragma once

#include <QByteArray>
#include <QStringList>

// Where a second quick viewer goes on Hyprland.
//
// The first viewer floats centred, by the window rule in hypr/omaroll.lua.
// Floating a second one would stack it exactly on top of the first, so once a
// workspace holds more than one viewer they all tile and the user's own
// layout arranges them. Hyprland applies the float rule only when a window
// maps, so the new viewer maps under the library's title (which the rule
// leaves tiled), and the viewers already floating there are tiled through
// IPC. Elsewhere this does nothing and each viewer is simply its own window.
namespace HyprlandPlacement {

// The class Omaroll's windows carry, from the desktop file's app ID.
inline constexpr char kWindowClass[] = "io.github.tsouth89.omaroll";

struct Plan {
  // This process's viewers on the active workspace.
  int viewers = 0;
  // Addresses of those viewers that are still floating.
  QStringList floating;

  // A viewer about to open there joins the others in tiles.
  [[nodiscard]] bool tileNew() const { return viewers > 0; }
};

// Reads `hyprctl -j clients` and `hyprctl -j activeworkspace` output. Only
// this process's viewers count: the library's title is "Omaroll" alone, a
// viewer's ends in " · Omaroll".
[[nodiscard]] Plan plan(const QByteArray& clients, const QByteArray& activeWorkspace,
                        qint64 pid);

// True for a Wayland window in a Hyprland session with hyprctl installed.
[[nodiscard]] bool available();

// The plan for this process, asked of the running compositor. Each query is
// bounded, so a stuck compositor costs a moment, never a hang.
[[nodiscard]] Plan query();

// Tiles each window, without waiting for the compositor.
void tile(const QStringList& addresses);

} // namespace HyprlandPlacement
