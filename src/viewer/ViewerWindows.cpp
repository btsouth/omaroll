#include "viewer/ViewerWindows.h"

#include "library/MediaInspector.h"
#include "app/AppSettings.h"
#include "subtitles/SubtitleIndex.h"
#include "viewer/HyprlandPlacement.h"
#include "viewer/ViewerSession.h"
#include "viewer/MprisService.h"

#include <QGuiApplication>
#include <QMediaPlayer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QPointer>
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
    if ((!window->isVisible() && !pending) || files.isEmpty()) {
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
  bool pending = false;
  quint64 placementGeneration = 0;
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

QQuickWindow* ViewerWindows::open(const QStringList& files, const QHash<QString, QString>& entryPaths) {
  for (const auto& viewer : m_viewers) {
    if (viewer->shows(files)) {
      QHash<QString, QString> requestedEntries;
      for (const auto& file : files) requestedEntries.insert(file, entryPaths.value(file, file));
      viewer->session.setDeletionPaths(requestedEntries);
      if (!viewer->pending) show(*viewer, {});
      return viewer->window;
    }
  }

  const auto spare = std::find_if(m_viewers.begin(), m_viewers.end(),
                                  [](const auto& viewer) {
                                    return !viewer->window->isVisible() && !viewer->pending;
                                  });
  Viewer* viewer = spare != m_viewers.end() ? spare->get() : create();
  if (!viewer) {
    return nullptr;
  }

  const bool others = std::any_of(m_viewers.begin(), m_viewers.end(), [viewer](const auto& other) {
    return other.get() != viewer && (other->window->isVisible() || other->pending);
  });
  viewer->session.open(files);
  viewer->session.setDeletionPaths(entryPaths);
  viewer->window->setProperty("mapTiled", false);
  const quint64 generation = ++viewer->placementGeneration;
  if (others) {
    // Prepare the media while hidden. IPC must finish before the float rule
    // sees the title, with one deadline for the whole placement request.
    viewer->pending = true;
    connect(viewer->window, &QQuickWindow::closing, this, [viewer] {
      viewer->pending = false;
      ++viewer->placementGeneration;
    }, Qt::SingleShotConnection);
    QTimer::singleShot(100, viewer->window, [this, viewer, generation] {
      if (!viewer->pending || generation != viewer->placementGeneration) return;
      viewer->pending = false;
      show(*viewer, {});
    });
    place(*viewer, generation);
  } else {
    show(*viewer, {});
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
              [this, created, window](const QString&) {
                emit libraryRequested(created->session.deletionPath(), window);
              }),
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

void ViewerWindows::show(Viewer& viewer, const QStringList& files,
                         const QHash<QString, QString>& entryPaths) {
  QQuickWindow* window = viewer.window;
  if (!files.isEmpty()) {
    viewer.session.open(files);
  }
  if (!entryPaths.isEmpty()) viewer.session.setDeletionPaths(entryPaths);
  if (!window->isVisible()) {
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
  if (viewer.session.isVideo()) {
    auto* mpris = qobject_cast<MprisService*>(
        m_engine.rootContext()->contextProperty(QStringLiteral("Mpris")).value<QObject*>());
    if (mpris) {
      mpris->track(viewer.window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer")),
                   viewer.window);
    }
  }
  updateFrontmost();
}

void ViewerWindows::place(Viewer& viewer, quint64 generation, bool retry) {
  QPointer<QQuickWindow> window = viewer.window;
  m_query(viewer.window, [this, &viewer, window, generation, retry](HyprlandPlacement::Plan plan) {
    if (!window || !viewer.pending || generation != viewer.placementGeneration) return;
    const bool unmappedPeer = plan.valid && plan.viewers == 0 &&
        std::any_of(m_viewers.begin(), m_viewers.end(), [&viewer](const auto& other) {
          return other.get() != &viewer && (other->window->isVisible() || other->pending);
        });
    if (!retry && (plan.retry || unmappedPeer)) {
      QTimer::singleShot(25, window, [this, &viewer, window, generation] {
        if (window && viewer.pending && generation == viewer.placementGeneration) {
          place(viewer, generation, true);
        }
      });
      return;
    }
    viewer.pending = false;
    window->setProperty("mapTiled", plan.tileNew());
    if (plan.tileNew()) {
      connect(window, &QQuickWindow::frameSwapped, window, [window] {
        if (window) window->setProperty("mapTiled", false);
      }, Qt::SingleShotConnection);
    }
    show(viewer, {});
    m_tile(plan.floating);
  });
}

void ViewerWindows::hidden() {
  // Keep one closed viewer for the next open; free the rest.
  bool kept = false;
  for (auto it = m_viewers.begin(); it != m_viewers.end();) {
    if ((*it)->window->isVisible() || (*it)->pending) {
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
  for (const auto& viewer : m_viewers) {
    viewer->window->setProperty("frontmost", viewer->window == front);
  }
}
