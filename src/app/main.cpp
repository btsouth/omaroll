#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/SingleInstance.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailProvider.h"

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

namespace {

QString optionValue(const QStringList& arguments, const QString& name) {
  const QString prefix = name + QLatin1Char('=');
  for (qsizetype index = 0; index < arguments.size(); ++index) {
    const QString& argument = arguments.at(index);
    if (argument.startsWith(prefix)) {
      return argument.mid(prefix.size());
    }
    if (argument == name && index + 1 < arguments.size() &&
        !arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
      return arguments.at(index + 1);
    }
  }
  return {};
}

void printUsage() {
  qInfo().noquote() << R"(omaroll - everything you capture, in one place

Usage:
  omaroll [options]

Options:
  --demo                 Browse a deterministic fictional library instead of
                         your own files. Nothing personal appears on screen.
  --render <file.png>    Render the window to a PNG and exit. Grabs the scene
                         graph, so an overlapping window cannot spoil the shot.
  --render-view <view>   Which view to render: grid, detail or matte.
  --render-size <WxH>    Window size for the render. Default 1280x820.
  --version              Print the version and exit.
  --help                 Show this message.)";
}

} // namespace

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

  const QStringList arguments = application.arguments();

  if (arguments.contains(QStringLiteral("--help")) ||
      arguments.contains(QStringLiteral("-h"))) {
    printUsage();
    return 0;
  }
  if (arguments.contains(QStringLiteral("--version"))) {
    qInfo().noquote() << QStringLiteral("omaroll %1").arg(OMAROLL_VERSION);
    return 0;
  }

  const QString renderPath = optionValue(arguments, QStringLiteral("--render"));
  const bool rendering = !renderPath.isEmpty();
  const bool demo = rendering || arguments.contains(QStringLiteral("--demo"));

  // A render is a one-shot batch job and a demo is a throwaway window; neither
  // should take over, or be refused by, a real session's instance.
  static SingleInstance instance;
  if (!rendering && !demo) {
    if (!instance.claimOrNotify()) {
      return 0;
    }
    QObject::connect(&instance, &SingleInstance::activationRequested, &application, [] {
      const auto windows = QGuiApplication::topLevelWindows();
      if (!windows.isEmpty()) {
        windows.constFirst()->show();
        windows.constFirst()->raise();
        windows.constFirst()->requestActivate();
      }
    });
  }

  if (demo) {
    const DemoLibrary::Layout layout = DemoLibrary::build();
    // Point every location resolver at the fictional tree. Setting the
    // environment is what makes this a genuine end-to-end run rather than a
    // special path through the model.
    qputenv("OMARCHY_SCREENSHOT_DIR", layout.pictures.toUtf8());
    qputenv("OMARCHY_SCREENRECORD_DIR", layout.videos.toUtf8());
    qputenv("XDG_PICTURES_DIR", layout.pictures.toUtf8());
    qputenv("XDG_VIDEOS_DIR", layout.videos.toUtf8());
    qputenv("XDG_DOWNLOAD_DIR", layout.root.toUtf8());
  }

  OmarchyTheme theme;
  AppSettings settings;

  CaptureModel captures(&settings);

  // QML only ever sees the proxy, so sorting and filtering can change without
  // any delegate knowing.
  CaptureFilterModel library;
  library.setSourceModel(&captures);

  // Restore what the user last chose, then keep the two in step. Doing it here
  // rather than in either class keeps the proxy unaware of persistence and the
  // settings unaware of the model.
  library.setSortMode(settings.sortMode());
  library.setKindFilter(settings.kindFilter());
  library.setShowHidden(settings.showHidden());
  QObject::connect(&library, &CaptureFilterModel::sortModeChanged,
                   [&] { settings.setSortMode(library.sortMode()); });
  QObject::connect(&library, &CaptureFilterModel::kindFilterChanged,
                   [&] { settings.setKindFilter(library.kindFilter()); });
  QObject::connect(&library, &CaptureFilterModel::showHiddenChanged,
                   [&] { settings.setShowHidden(library.showHidden()); });

  ActionLauncher actions;
  ActionRegistry registry(&actions);
  MatteComposer matte;

  QQmlApplicationEngine engine;
  engine.addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), new ThumbnailProvider);
  engine.addImageProvider(QLatin1String(MatteProvider::kProviderId), new MatteProvider);
  engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("Captures"), &library);
  engine.rootContext()->setContextProperty(QStringLiteral("Library"), &captures);
  engine.rootContext()->setContextProperty(QStringLiteral("Actions"), &actions);
  engine.rootContext()->setContextProperty(QStringLiteral("Settings"), &settings);
  engine.rootContext()->setContextProperty(QStringLiteral("Registry"), &registry);
  engine.rootContext()->setContextProperty(QStringLiteral("Matte"), &matte);
  engine.rootContext()->setContextProperty(QStringLiteral("DemoMode"), demo);

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
      [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);

  engine.loadFromModule("Omaroll", "Main");
  if (engine.rootObjects().isEmpty()) {
    return 1;
  }

  if (rendering) {
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (!window) {
      return 1;
    }

    const QString sizeText = optionValue(arguments, QStringLiteral("--render-size"));
    const QStringList parts = sizeText.split(QLatin1Char('x'), Qt::SkipEmptyParts);
    if (parts.size() == 2) {
      window->resize(parts.at(0).toInt(), parts.at(1).toInt());
    } else {
      window->resize(1280, 820);
    }

    const QString view = optionValue(arguments, QStringLiteral("--render-view"));

    // Thumbnails and matte previews are produced on worker threads, so a grab
    // taken the instant the window maps would capture an empty grid. Wait for
    // the visible tiles to land, open the requested view, wait again, then grab
    // the scene graph, which is unaffected by anything overlapping the window.
    QTimer::singleShot(5000, &application, [window, view] {
      if (!view.isEmpty()) {
        QMetaObject::invokeMethod(window, "openViewForRender", Q_ARG(QVariant, view));
      }
    });

    QTimer::singleShot(11000, &application, [window, renderPath] {
      const QImage frame = window->grabWindow();
      if (frame.isNull() || !frame.save(renderPath)) {
        qWarning().noquote() << "could not write" << renderPath;
        QCoreApplication::exit(1);
        return;
      }
      qInfo().noquote() << "wrote" << renderPath;
      QCoreApplication::quit();
    });
  }

  return QGuiApplication::exec();
}
