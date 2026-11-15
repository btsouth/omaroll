#include "app/GraphicsWarmup.h"

#include "app/StartupTrace.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QThreadPool>

#include <mutex>

namespace GraphicsWarmup {

bool wanted() {
  // Offscreen tests and renders, X11 and other backends keep Qt's own order.
  if (!QGuiApplication::platformName().startsWith(u"wayland")) return false;
  if (QQuickWindow::graphicsApi() != QSGRendererInterface::OpenGL) return false;
  // Also brings up the platform's EGL display on this thread, which the
  // first window would otherwise do; the worker then only adds a context.
  return QOpenGLContext::supportsThreadedOpenGL();
}

void start() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!wanted()) return;
    QThreadPool::globalInstance()->start([] {
      QOpenGLContext context;
      context.setFormat(QSurfaceFormat::defaultFormat());
      context.create();
      StartupTrace::note("graphics_warmup_done");
    });
  });
}

} // namespace GraphicsWarmup
