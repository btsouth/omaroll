#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/SingleInstance.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/DuplicateIndex.h"
#include "library/MediaInspector.h"
#include "library/MediaDateIndex.h"
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "sources/CaptureScanner.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailCache.h"
#include "thumbs/ThumbnailProvider.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>

namespace {

QString optionValue(const QStringList &arguments, const QString &name) {
  const QString prefix = name + QLatin1Char('=');
  for (qsizetype index = 0; index < arguments.size(); ++index) {
    const QString &argument = arguments.at(index);
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

bool optionPresent(const QStringList &arguments, const QString &name) {
  const QString prefix = name + QLatin1Char('=');
  for (const QString &argument : arguments) {
    if (argument == name || argument.startsWith(prefix)) {
      return true;
    }
  }
  return false;
}

QString argumentError(const QStringList &arguments) {
  static const QStringList valueOptions = {QStringLiteral("--render"),
                                           QStringLiteral("--render-view"),
                                           QStringLiteral("--render-size")};
  static const QStringList flagOptions = {
      QStringLiteral("--demo"), QStringLiteral("--help"), QStringLiteral("-h"),
      QStringLiteral("--version")};
  int positional = 0;
  for (qsizetype index = 1; index < arguments.size(); ++index) {
    const QString &argument = arguments.at(index);
    if (valueOptions.contains(argument)) {
      if (index + 1 >= arguments.size() ||
          arguments.at(index + 1).startsWith(u"--")) {
        return QStringLiteral("%1 needs a value").arg(argument);
      }
      ++index;
      continue;
    }
    bool knownAssignment = false;
    for (const QString &option : valueOptions) {
      if (argument.startsWith(option + QLatin1Char('='))) {
        if (argument.size() == option.size() + 1) {
          return QStringLiteral("%1 needs a value").arg(option);
        }
        knownAssignment = true;
        break;
      }
    }
    if (knownAssignment || flagOptions.contains(argument)) {
      continue;
    }
    if (argument.startsWith(QLatin1Char('-'))) {
      return QStringLiteral("unknown option: %1").arg(argument);
    }
    if (++positional > 1) {
      return QStringLiteral("open one file or folder at a time");
    }
  }
  return {};
}

// The first argument that is neither an option nor an option's value. The
// desktop entry passes %f here, so "Open with Omaroll" on a picture or a
// folder lands in it.
QString positionalArgument(const QStringList &arguments) {
  static const QStringList valueOptions = {QStringLiteral("--render"),
                                           QStringLiteral("--render-view"),
                                           QStringLiteral("--render-size")};
  for (qsizetype index = 1; index < arguments.size(); ++index) {
    const QString &argument = arguments.at(index);
    if (valueOptions.contains(argument)) {
      ++index;
      continue;
    }
    if (argument.startsWith(QLatin1Char('-'))) {
      continue;
    }
    return argument;
  }
  return {};
}

void printUsage() {
  QTextStream(stdout) << R"(omaroll - your media, in one beautiful library

Usage:
  omaroll [options] [file or folder]

A file is selected and opened in the detail view; its folder is added to the
library if it is not already watched. A folder is added to the library.

Options:
  --demo                 Browse a deterministic fictional library instead of
                         your own files. Nothing personal appears on screen.
  --render <file.png>    Render the window to a PNG and exit. Draws offscreen,
                         so no compositor can resize it or overlap it.
  --render-view <view>   Which view to render: grid, detail, video, slideshow, matte, export, rename, OCR, duplicates or settings.
  --render-size <WxH>    Window size, from 560x420 to 7680x4320. Default 1280x820.
  --version              Print the version and exit.
  --help                 Show this message.)"
                      << Qt::endl;
}

} // namespace

int main(int argc, char *argv[]) {
  QGuiApplication::setApplicationName(QStringLiteral("Omaroll"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Omaroll"));
  QGuiApplication::setApplicationVersion(QStringLiteral(OMAROLL_VERSION));
  QGuiApplication::setOrganizationName(QStringLiteral("Omaroll"));

  // Informational CLI flags must work over SSH and in package containers that
  // have no graphical platform plugin available. Handle them before creating
  // QGuiApplication, which would otherwise abort before printing anything.
  for (int index = 1; index < argc; ++index) {
    const QByteArray argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      printUsage();
      return 0;
    }
    if (argument == "--version") {
      QTextStream(stdout) << "omaroll " << OMAROLL_VERSION << Qt::endl;
      return 0;
    }
  }

  // On Wayland this is what becomes app_id. The Hyprland media-opacity rule
  // matches on it, so it must stay in lockstep with the .desktop filename and
  // StartupWMClass. Changing it silently breaks the transparency opt-out.
  QGuiApplication::setDesktopFileName(
      QStringLiteral("io.github.tsouth89.omaroll"));

  // Basic rather than a platform style: every colour comes from the Omarchy
  // theme, and a style that injects its own palette fights that.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // A render must come out at the size asked for. Under a tiling compositor
  // the window is resized to its tile the moment it maps, so render against
  // the offscreen platform instead, where the requested size is the size.
  for (int index = 1; index < argc; ++index) {
    const QByteArray argument(argv[index]);
    if (argument == "--render" || argument.startsWith("--render=")) {
      // Omarchy exports QT_QPA_PLATFORM=wayland session-wide, so this is
      // unconditional. OMAROLL_RENDER_PLATFORM overrides it for checking
      // something the offscreen platform cannot draw, such as video frames.
      const QByteArray platform = qgetenv("OMAROLL_RENDER_PLATFORM");
      qputenv("QT_QPA_PLATFORM",
              platform.isEmpty() ? QByteArray("offscreen") : platform);
      break;
    }
  }

  QGuiApplication application(argc, argv);
  QIcon applicationIcon =
      QIcon::fromTheme(QStringLiteral("io.github.tsouth89.omaroll"));
  if (applicationIcon.isNull()) {
    applicationIcon =
        QIcon(QStringLiteral(":/icons/resources/icons/omaroll.svg"));
  }
  application.setWindowIcon(applicationIcon);

  const QStringList arguments = application.arguments();

  const QString badArgument = argumentError(arguments);
  if (!badArgument.isEmpty()) {
    qWarning().noquote() << "omaroll:" << badArgument;
    qWarning().noquote() << "Try 'omaroll --help'.";
    return 2;
  }

  const QString renderPath = optionValue(arguments, QStringLiteral("--render"));
  const bool rendering = !renderPath.isEmpty();
  if (!rendering &&
      (optionPresent(arguments, QStringLiteral("--render-view")) ||
       optionPresent(arguments, QStringLiteral("--render-size")))) {
    qWarning().noquote()
        << "omaroll: --render-view and --render-size require --render";
    return 2;
  }
  const QString renderView =
      optionValue(arguments, QStringLiteral("--render-view"));
  static const QStringList renderViews = {
      QStringLiteral("grid"),  QStringLiteral("detail"),
      QStringLiteral("video"), QStringLiteral("slideshow"),
      QStringLiteral("matte"), QStringLiteral("export"),
      QStringLiteral("rename"), QStringLiteral("ocr"), QStringLiteral("duplicates"),
      QStringLiteral("settings")};
  if (!renderView.isEmpty() && !renderViews.contains(renderView)) {
    qWarning().noquote() << "omaroll: unknown render view:" << renderView;
    return 2;
  }
  const QString renderSize =
      optionValue(arguments, QStringLiteral("--render-size"));
  static const QRegularExpression sizePattern(
      QStringLiteral(R"(^[1-9]\d*x[1-9]\d*$)"));
  if (!renderSize.isEmpty() && !sizePattern.match(renderSize).hasMatch()) {
    qWarning().noquote() << "omaroll: render size must look like 1280x820";
    return 2;
  }
  if (!renderSize.isEmpty()) {
    const QStringList dimensions = renderSize.split(QLatin1Char('x'));
    const int width = dimensions.at(0).toInt();
    const int height = dimensions.at(1).toInt();
    if (width < 560 || height < 420 || width > 7680 || height > 4320) {
      qWarning().noquote()
          << "omaroll: render size must be between 560x420 and 7680x4320";
      return 2;
    }
  }
  const bool demo = rendering || arguments.contains(QStringLiteral("--demo"));
  const QString requestedPath =
      demo ? QString() : positionalArgument(arguments);

  if (!requestedPath.isEmpty()) {
    const QFileInfo handed(requestedPath);
    if (!handed.exists()) {
      qWarning().noquote() << "omaroll: file or folder does not exist:"
                           << requestedPath;
      return 2;
    }
    if (!handed.isReadable()) {
      qWarning().noquote() << "omaroll: cannot read:" << requestedPath;
      return 2;
    }
    if (handed.isDir()) {
      const QString canonical = handed.canonicalFilePath();
      const QString home = QFileInfo(QDir::homePath()).canonicalFilePath();
      if (canonical == home || canonical == QDir::rootPath()) {
        qWarning().noquote() << "omaroll: choose a media folder, not your home "
                                "or filesystem root";
        return 2;
      }
    }
    if (handed.isFile() && !CaptureScanner::isImage(handed.suffix()) &&
        !CaptureScanner::isVideo(handed.suffix())) {
      qWarning().noquote() << "omaroll: unsupported media file:"
                           << requestedPath;
      return 2;
    }
  }

  // A render is a one-shot batch job and a demo is a throwaway window; neither
  // should take over, or be refused by, a real session's instance.
  SingleInstance instance;
  if (!rendering && !demo) {
    if (!instance.claimOrNotify(requestedPath)) {
      return 0;
    }
  }

  if (demo) {
    const DemoLibrary::Layout layout = DemoLibrary::build();
    // Give the duplicate render one real match without making the normal demo
    // library intentionally repetitive. This path exists only for visual QA.
    if (renderView == QStringLiteral("duplicates")) {
      QFile::copy(layout.pictures + QStringLiteral("/alpine-dawn.jpg"),
                  layout.pictures + QStringLiteral("/alpine-dawn-copy.jpg"));
    }
    // Point every location resolver at the fictional tree. Setting the
    // environment is what makes this a genuine end-to-end run rather than a
    // special path through the model.
    qputenv("OMARCHY_SCREENSHOT_DIR", layout.pictures.toUtf8());
    qputenv("OMARCHY_SCREENRECORD_DIR", layout.videos.toUtf8());
    qputenv("XDG_PICTURES_DIR", layout.pictures.toUtf8());
    qputenv("XDG_VIDEOS_DIR", layout.videos.toUtf8());
    qputenv("XDG_DOWNLOAD_DIR", layout.root.toUtf8());

    // Settings too. A demo that read the real settings would prune the user's
    // favourites against a fictional library and leave its own filter and sort
    // behind for the next real launch.
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       layout.root + QStringLiteral("/config"));
  }

  OmarchyTheme theme;
  AppSettings settings;
  if (demo) {
    settings.setSlideshowVideos(true);
  }

  // Bound the thumbnail cache off the GUI thread. A thumbnail is always
  // rebuildable, so this can never lose anything the user cares about.
  const auto pruneThumbnailCache = [&settings] {
    const qint64 bytes = qint64(settings.thumbnailCacheMb()) * 1024 * 1024;
    QThreadPool::globalInstance()->start(
        [bytes] { ThumbnailCache::prune(bytes); });
  };
  pruneThumbnailCache();
  QObject::connect(&settings, &AppSettings::thumbnailCacheMbChanged,
                   &application, pruneThumbnailCache);

  CaptureModel captures(&settings);
  // Demo and render order is deliberately fixed. Real libraries are enriched
  // in the background after their fast filesystem scan lands.
  MediaDateIndex mediaDates(demo ? nullptr : &captures);

  // QML only ever sees the proxy, so sorting and filtering can change without
  // any delegate knowing.
  CaptureFilterModel library;
  library.setSourceModel(&captures);
  OcrIndex textIndex(&captures);
  QrDetector qrDetector(&captures);
  QObject::connect(&library, &CaptureFilterModel::searchTextChanged,
                   &textIndex, [&] { textIndex.setSearchText(library.searchText()); });
  QObject::connect(&textIndex, &OcrIndex::textReady,
                   &library, &CaptureFilterModel::setOcrText);
  DuplicateIndex duplicates(&captures);
  QObject::connect(&library, &CaptureFilterModel::duplicatesOnlyChanged,
                   &duplicates, [&] { duplicates.setActive(library.duplicatesOnly()); });
  QObject::connect(&duplicates, &DuplicateIndex::groupsChanged, &library,
                   [&] { library.setDuplicateGroups(duplicates.groups()); });
  MediaInspector mediaInfo;

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

  // "Open with": a file is selected once the scan lands, and its folder joins
  // the library if no watched root already covers it. A folder simply joins.
  QString initialPath;
  QString initialFolderPath;
  if (!demo) {
    const QFileInfo handed(requestedPath);
    if (handed.isDir()) {
      initialFolderPath = handed.canonicalFilePath();
      captures.setExtraRoot(initialFolderPath);
    } else if (handed.isFile()) {
      initialPath = handed.canonicalFilePath();
      captures.setExtraRoot(handed.canonicalPath());
    }
  }

  ActionLauncher actions;
  ActionRegistry registry(&actions);
  TailscalePeers tailscale;
  MatteComposer matte;

  // A tracked tool's half-written output stays out of the library until the
  // run settles; writing to an existing file fires no directory event, so the
  // release also rescans to pick the finished file up.
  QObject::connect(&actions, &ActionLauncher::outputPending, &captures,
                   &CaptureModel::holdPath);
  QObject::connect(&actions, &ActionLauncher::outputSettled, &captures,
                   &CaptureModel::releasePath);

  QQmlApplicationEngine engine;
  auto *thumbnailProvider = new ThumbnailProvider;
  auto *matteProvider = new MatteProvider;
  engine.addImageProvider(QLatin1String(ThumbnailProvider::kProviderId),
                          thumbnailProvider);
  engine.addImageProvider(QLatin1String(MatteProvider::kProviderId),
                          matteProvider);
  QObject::connect(&application, &QCoreApplication::aboutToQuit, &application,
                   [thumbnailProvider, matteProvider] {
                     thumbnailProvider->shutdown();
                     matteProvider->shutdown();
                     QThreadPool::globalInstance()->waitForDone();
                   });
  engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("Captures"),
                                           &library);
  engine.rootContext()->setContextProperty(QStringLiteral("Library"),
                                           &captures);
  engine.rootContext()->setContextProperty(QStringLiteral("Actions"), &actions);
  engine.rootContext()->setContextProperty(QStringLiteral("Settings"),
                                           &settings);
  engine.rootContext()->setContextProperty(QStringLiteral("Registry"),
                                           &registry);
  engine.rootContext()->setContextProperty(QStringLiteral("Matte"), &matte);
  engine.rootContext()->setContextProperty(QStringLiteral("TextIndex"),
                                           &textIndex);
  engine.rootContext()->setContextProperty(QStringLiteral("Qr"), &qrDetector);
  engine.rootContext()->setContextProperty(QStringLiteral("Duplicates"),
                                           &duplicates);
  engine.rootContext()->setContextProperty(QStringLiteral("MediaInfo"),
                                           &mediaInfo);
  engine.rootContext()->setContextProperty(QStringLiteral("MediaDates"),
                                           &mediaDates);
  engine.rootContext()->setContextProperty(QStringLiteral("Tailscale"),
                                           &tailscale);
  engine.rootContext()->setContextProperty(QStringLiteral("DemoMode"), demo);
  engine.rootContext()->setContextProperty(QStringLiteral("InitialPath"),
                                           initialPath);
  engine.rootContext()->setContextProperty(QStringLiteral("InitialFolderPath"),
                                           initialFolderPath);

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
      [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);

  engine.loadFromModule("Omaroll", "Main");
  if (engine.rootObjects().isEmpty()) {
    return 1;
  }

  if (!rendering && !demo) {
    auto *window =
        qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QObject::connect(&instance, &SingleInstance::activationRequested,
                     &application, [window, &captures](const QString &path) {
                       window->show();
                       window->raise();
                       window->requestActivate();

                       const QFileInfo handed(path);
                       if (handed.isDir()) {
                         const QString canonical = handed.canonicalFilePath();
                         captures.setExtraRoot(canonical);
                         QMetaObject::invokeMethod(window, "openFolder",
                                                   Q_ARG(QVariant, canonical));
                       } else if (handed.isFile()) {
                         const QString canonical = handed.canonicalFilePath();
                         captures.setExtraRoot(handed.canonicalPath());
                         QMetaObject::invokeMethod(window, "openPath",
                                                   Q_ARG(QVariant, canonical));
                       }
                     });
  }

  if (rendering) {
    auto *window =
        qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window) {
      return 1;
    }

    const QString sizeText = renderSize;
    const QStringList parts =
        sizeText.split(QLatin1Char('x'), Qt::SkipEmptyParts);
    if (parts.size() == 2) {
      window->resize(parts.at(0).toInt(), parts.at(1).toInt());
    } else {
      window->resize(1280, 820);
    }

    const QString view = renderView;

    // Thumbnails and matte previews are produced on worker threads, so a grab
    // taken the instant the window maps would capture an empty grid. Wait for
    // the visible tiles to land, open the requested view, wait again, then grab
    // the scene graph, which is unaffected by anything overlapping the window.
    QTimer::singleShot(5000, &application, [window, view] {
      if (!view.isEmpty()) {
        QMetaObject::invokeMethod(window, "openViewForRender",
                                  Q_ARG(QVariant, view));
      }
    });

    QTimer::singleShot(11000, &application, [window, renderPath] {
      const QImage frame = window->grabWindow();
      if (frame.isNull() || !frame.save(renderPath)) {
        qWarning().noquote() << "could not write" << renderPath;
        QCoreApplication::exit(1);
        return;
      }
      QTextStream(stdout) << "wrote " << renderPath << Qt::endl;
      QCoreApplication::quit();
    });
  }

  return QGuiApplication::exec();
}
