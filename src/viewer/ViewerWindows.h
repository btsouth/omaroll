#pragma once

#include <QList>
#include <QHash>
#include <QObject>
#include <QStringList>

#include "viewer/HyprlandPlacement.h"

#include <functional>
#include <memory>
#include <vector>

class QQmlEngine;
class QQuickWindow;
class ViewerSession;

// The quick viewers: one window per file opened from outside, each with its
// own session, inspector and playback, so two pictures can sit side by side.
//
// Asking for what a viewer already shows brings that viewer forward instead of
// opening a duplicate. A closed viewer is kept hidden for the next open, so
// the second launch is as quick as the first; any more closed ones are freed.
class ViewerWindows final : public QObject {
  Q_OBJECT

public:
  explicit ViewerWindows(QQmlEngine& engine, QObject* parent = nullptr);
  ~ViewerWindows() override;

  // Shows files in a viewer and returns its window, or nullptr when the QML
  // could not be loaded (the errors are already printed).
  QQuickWindow* open(const QStringList& files, const QHash<QString, QString>& entryPaths = {});

  // Visible viewers, oldest first.
  [[nodiscard]] QList<QQuickWindow*> visibleWindows() const;
  // The visible viewer used most recently. Messages without a window of
  // their own go here, so they appear once rather than in every viewer. Of
  // the viewers showing a video, only the one used last is heard.
  [[nodiscard]] QQuickWindow* frontmost() const;
  // Every viewer window, including the hidden one kept for reuse.
  [[nodiscard]] int windowCount() const { return int(m_viewers.size()); }
  [[nodiscard]] ViewerSession* sessionOf(const QQuickWindow* window) const;

  // How a new viewer finds the viewers already on its workspace; the
  // running Hyprland by default. Tests substitute their own.
  using PlacementQuery = std::function<HyprlandPlacement::Plan()>;
  void setPlacementQuery(PlacementQuery query) { m_query = std::move(query); }

signals:
  // The source entry chosen by the user, before media-path resolution.
  void libraryRequested(const QString& path, QQuickWindow* viewer);

private:
  struct Viewer;

  Viewer* create();
  void show(Viewer& viewer, const QStringList& files, bool tiled,
            const QHash<QString, QString>& entryPaths = {});
  void activated(Viewer& viewer);
  void hidden();
  void updateFrontmost();

  QQmlEngine& m_engine;
  std::vector<std::unique_ptr<Viewer>> m_viewers;
  // Most recently active last.
  QList<Viewer*> m_recent;
  PlacementQuery m_query = HyprlandPlacement::query;
};
