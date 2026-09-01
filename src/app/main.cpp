#include "actions/ActionLauncher.h"
#include "app/SingleInstance.h"
#include "library/CaptureModel.h"
#include "sources/CaptureLocations.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailProvider.h"

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>

int main(int argc, char* argv[]) {
  QGuiApplication::setApplicationName(QStringLiteral("Omaroll"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Omaroll"));
  QGuiApplication::setApplicationVersion(QStringLiteral(OMAROLL_VERSION));
  QGuiApplication::setOrganizationName(QStringLiteral("Omaroll"));

  // On Wayland this is what becomes app_id. The Hyprland media-opacity rule
  // matches on it, so it must stay in lockstep with the .desktop filename and
  // StartupWMClass. Changing it silently breaks the transparency opt-out.
  QGuiApplication::setDesktopFileName(QStringLiteral("omaroll"));

  // Basic rather than a platform style: every colour comes from the Omarchy
  // theme, and a style that injects its own palette fights that.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  QGuiApplication application(argc, argv);
  application.setWindowIcon(QIcon::fromTheme(QStringLiteral("omaroll")));

  SingleInstance instance;
  if (!instance.claimOrNotify()) {
    return 0;
  }

  OmarchyTheme theme;

  CaptureModel captures;
  // v0.1 scans the two capture roots at depth 1. That is where Omarchy writes
  // screenshots and recordings, and because those directories are also
  // ~/Pictures and ~/Videos, flat images and videos there are classified and
  // shown too. Bounded recursion for the general media sections lands in v0.2.
  captures.setRoots({
      {CaptureLocations::screenshots(), 1, CaptureRecord::Picture},
      {CaptureLocations::recordings(), 1, CaptureRecord::Video},
  });

  ActionLauncher actions;

  QQmlApplicationEngine engine;
  engine.addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), new ThumbnailProvider);
  engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("Captures"), &captures);
  engine.rootContext()->setContextProperty(QStringLiteral("Actions"), &actions);

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
      [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);

  engine.loadFromModule("Omaroll", "Main");
  if (engine.rootObjects().isEmpty()) {
    return 1;
  }

  // A second launch raises the window we already have rather than opening
  // another one.
  QObject::connect(&instance, &SingleInstance::activationRequested, &application, [&engine] {
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
      window->show();
      window->raise();
      window->requestActivate();
    }
  });

  return QGuiApplication::exec();
}
