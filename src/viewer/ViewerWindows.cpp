#include "viewer/ViewerWindows.h"

#include "library/MediaInspector.h"
#include "app/AppSettings.h"
#include "subtitles/SubtitleIndex.h"
#include "viewer/HyprlandPlacement.h"
#include "viewer/ViewerSession.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <algorithm>

struct ViewerWindows::Viewer {
  explicit Viewer(QQmlEngine& engine) : context(engine.rootContext()) {
    // Not "Viewer": that name is the window's own QML type, and a type name
    // wins over a context property.
    if (auto* settings = qobject_cast<AppSettings*>(
            engine.rootContext()->contextProperty(QStringLiteral("Settings")).value<QObject*>())) {
      session.setPairRawJpeg(settings->pairRawJpeg());
      QObject::connect(settings, &AppSettings::pairRawJpegChanged, &session,
                       [this, settings] { session.setPairRawJpeg(settings->pairRawJpeg()); });
    }
    context.setContextProperty(QStringLiteral("Session"), &session);
    context.setContextProperty(QStringLiteral("MediaInfo"), &mediaInfo);
    context.setContextProperty(QStringLiteral("Subtitles"), &subtitles);
    QQmlComponent component(&engine);
    component.loadFromModule("Omaroll", "Viewer");
    root.reset(component.create(&context));
    if (!root) {
      for (const QQmlError& error : component.errors()) {
        qWarning().noquote() << error.toString();
      }
    }
    window = qobject_cast<QQuickWindow*>(root.get());
  }

  // Cut loose before anything is torn down: a dying window still signals,
  // and the handlers walk the very list this viewer is leaving.
  ~Viewer() {
    for (const QMetaObject::Connection& connection : connections) {
      QObject::disconnect(connection);
    }
  }

  Viewer(const Viewer&) = delete;
  Viewer& operator=(const Viewer&) = delete;

  // Already showing these files: one file being the one on screen, or the
  // same selection of several.
  [[nodiscard]] bool shows(const QStringList& files) const {
    if (!window->isVisible() || files.isEmpty()) {
      return false;
    }
    return files.size() == 1 ? session.path() == files.first()
                             : session.selection() && session.sequence() == files;
  }

  ViewerSession session;
  MediaInspector mediaInfo;
  SubtitleIndex subtitles;
  QQmlContext context;
  // Last, so the window goes before the context and services it binds to.
  std::unique_ptr<QObject> root;
  QQuickWindow* window = nullptr;
  QList<QMetaObject::Connection> connections;
};

ViewerWindows::ViewerWindows(QQmlEngine& engine, QObject* parent)
    : QObject(parent), m_engine(engine) {}

ViewerWindows::~ViewerWindows() {
  // Nothing may call back into a list that is being destroyed.
  for (const auto& viewer : m_viewers) {
    for (const QMetaObject::Connection& connection : viewer->connections) {
      disconnect(connection);
    }
  }
}

QQuickWindow* ViewerWindows::open(const QStringList& files) {
  for (const auto& viewer : m_viewers) {
    if (viewer->shows(files)) {
      show(*viewer, {}, false);
      return viewer->window;
    }
  }

  const auto spare = std::find_if(m_viewers.begin(), m_viewers.end(),
                                  [](const auto& viewer) { return !viewer->window->isVisible(); });
  Viewer* viewer = spare != m_viewers.end() ? spare->get() : create();
  if (!viewer) {
    return nullptr;
  }

  // A second viewer on this workspace tiles, and so do the ones floating
  // there already; the first one alone floats centred as before.
  const bool others = !visibleWindows().isEmpty();
  const HyprlandPlacement::Plan plan = others ? m_query() : HyprlandPlacement::Plan{};
  show(*viewer, files, plan.tileNew());
  HyprlandPlacement::tile(plan.floating);
  if (others) {
    // Two opens in quick succession can each look before the other's window
    // has mapped, and both would float. Look again once this one is up and
    // its own title has reached the compositor.
    connect(
        viewer->window, &QQuickWindow::frameSwapped, this,
        [this] {
          QTimer::singleShot(150, this, [this] {
            const HyprlandPlacement::Plan settled = m_query();
            if (settled.viewers > 1) {
              HyprlandPlacement::tile(settled.floating);
            }
          });
        },
        Qt::SingleShotConnection);
  }
  return viewer->window;
}

QList<QQuickWindow*> ViewerWindows::visibleWindows() const {
  QList<QQuickWindow*> windows;
  for (const auto& viewer : m_viewers) {
    if (viewer->window->isVisible()) {
      windows.append(viewer->window);
    }
  }
  return windows;
}

QQuickWindow* ViewerWindows::frontmost() const {
  for (auto it = m_recent.crbegin(); it != m_recent.crend(); ++it) {
    if ((*it)->window->isVisible()) {
      return (*it)->window;
    }
  }
  return nullptr;
}

ViewerSession* ViewerWindows::sessionOf(const QQuickWindow* window) const {
  for (const auto& viewer : m_viewers) {
    if (viewer->window == window) {
      return &viewer->session;
    }
  }
  return nullptr;
}

ViewerWindows::Viewer* ViewerWindows::create() {
  auto viewer = std::make_unique<Viewer>(m_engine);
  if (!viewer->window) {
    return nullptr;
  }
  Viewer* created = viewer.get();
  QQuickWindow* window = created->window;
  created->connections = {
      connect(&created->session, &ViewerSession::libraryRequested, this,
              [this, window](const QString& path) { emit libraryRequested(path, window); }),
      // Stepping onto or off a video moves the sound.
      connect(&created->session, &ViewerSession::currentChanged, this,
              &ViewerWindows::updateFrontmost),
      connect(window, &QWindow::activeChanged, this,
              [this, created] {
                if (created->window->isActive()) {
                  activated(*created);
                }
              }),
      connect(window, &QWindow::visibleChanged, this,
              [this](bool visible) {
                if (!visible) {
                  // After the close has finished, never from inside it.
                  QTimer::singleShot(0, this, &ViewerWindows::hidden);
                }
              }),
  };
  m_viewers.push_back(std::move(viewer));
  return created;
}

void ViewerWindows::show(Viewer& viewer, const QStringList& files, bool tiled) {
  QQuickWindow* window = viewer.window;
  if (!files.isEmpty()) {
    viewer.session.open(files);
  }
  if (!window->isVisible()) {
    // Hyprland's float rule matches a viewer's title when the window maps.
    // Mapping under the library's plain title leaves this one tiled; its own
    // title follows once the first frame is up.
    window->setProperty("mapTiled", tiled);
    if (tiled) {
      connect(
          window, &QQuickWindow::frameSwapped, window,
          [window] { window->setProperty("mapTiled", false); }, Qt::SingleShotConnection);
    }
    // A floating window opens at this size. A tiling compositor ignores
    // the request and gives the window its tile instead.
    if (const QScreen* screen = window->screen()) {
      window->resize(ViewerSession::preferredWindowSize(screen->availableGeometry().size()));
    }
    window->setWindowStates(Qt::WindowNoState);
    window->show();
  }
  if (QGuiApplication::platformName() != u"offscreen") {
    window->raise();
    window->requestActivate();
  }
  // Counted as used now, not when the compositor gets round to focusing it,
  // so a message straight after an open lands in the window just opened.
  activated(viewer);
}

void ViewerWindows::activated(Viewer& viewer) {
  m_recent.removeAll(&viewer);
  m_recent.append(&viewer);
  updateFrontmost();
}

void ViewerWindows::hidden() {
  // Keep one closed viewer for the next open; free the rest.
  bool kept = false;
  for (auto it = m_viewers.begin(); it != m_viewers.end();) {
    if ((*it)->window->isVisible()) {
      ++it;
    } else if (!kept) {
      kept = true;
      ++it;
    } else {
      // Out of the list first, so the list is whole while it is destroyed.
      m_recent.removeAll(it->get());
      std::unique_ptr<Viewer> freed = std::move(*it);
      it = m_viewers.erase(it);
      freed.reset();
    }
  }
  updateFrontmost();
}

void ViewerWindows::updateFrontmost() {
  const QQuickWindow* front = frontmost();
  // The sound stays with the video used last, even while a picture in
  // another viewer has the focus.
  const Viewer* heard = nullptr;
  for (auto it = m_recent.crbegin(); it != m_recent.crend() && !heard; ++it) {
    if ((*it)->window->isVisible() && (*it)->session.isVideo()) {
      heard = *it;
    }
  }
  for (const auto& viewer : m_viewers) {
    viewer->window->setProperty("frontmost", viewer->window == front);
    viewer->window->setProperty("audible", !heard || viewer.get() == heard);
  }
}
