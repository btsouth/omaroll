#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/HeadlessAudio.h"
#include "app/OpenRequest.h"
#include "app/SingleInstance.h"
#include "app/StartupTrace.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/DuplicateIndex.h"
#include "library/MediaMetadataIndex.h"
#include "library/MediaInspector.h"
#include "library/SimilarityIndex.h"
#include "edit/EditProvider.h"
#include "edit/ImageEditor.h"
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "pdf/PdfInspector.h"
#include "pdf/PdfProvider.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "subtitles/SubtitleIndex.h"
#include "sources/CaptureScanner.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailCache.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/ViewerSession.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>

#include <memory>

namespace {

QString optionValue(const QStringList& arguments, const QString& name) {
  const QString prefix = name + QLatin1Char('=');
  for (qsizetype index = 0; index < arguments.size(); ++index) {
    const QString& argument = arguments.at(index);
    if (argument == QStringLiteral("--")) {
      break;
    }
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

bool optionPresent(const QStringList& arguments, const QString& name) {
  const QString prefix = name + QLatin1Char('=');
  for (const QString& argument : arguments) {
    if (argument == QStringLiteral("--")) {
      break;
    }
    if (argument == name || argument.startsWith(prefix)) {
      return true;
    }
  }
  return false;
}

QString argumentError(const QStringList& arguments) {
  static const QStringList valueOptions = {
      QStringLiteral("--render"), QStringLiteral("--render-view"), QStringLiteral("--render-size")};
  static const QStringList flagOptions = {QStringLiteral("--demo"), QStringLiteral("--help"),
                                          QStringLiteral("-h"), QStringLiteral("--library"),
                                          QStringLiteral("--version")};
  for (qsizetype index = 1; index < arguments.size(); ++index) {
    const QString& argument = arguments.at(index);
    if (argument == QStringLiteral("--")) {
      break;
    }
    if (valueOptions.contains(argument)) {
      if (index + 1 >= arguments.size() || arguments.at(index + 1).startsWith(u"--")) {
        return QStringLiteral("%1 needs a value").arg(argument);
      }
      ++index;
      continue;
    }
    bool knownAssignment = false;
    for (const QString& option : valueOptions) {
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
  }
  return {};
}

// Paths after -- are literal, including names beginning with a dash.
QStringList positionalArguments(const QStringList& arguments) {
  static const QStringList valueOptions = {
      QStringLiteral("--render"), QStringLiteral("--render-view"), QStringLiteral("--render-size")};
  QStringList paths;
  bool literal = false;
  for (qsizetype index = 1; index < arguments.size(); ++index) {
    const QString& argument = arguments.at(index);
    if (!literal && argument == QStringLiteral("--")) {
      literal = true;
    } else if (!literal && valueOptions.contains(argument)) {
      ++index;
    } else if (literal || !argument.startsWith(QLatin1Char('-'))) {
      paths.append(argument);
    }
  }
  return paths;
}

void printUsage() {
  QTextStream(stdout) << R"(omaroll - your media, in one beautiful library

Usage:
  omaroll [options] [file ... | folder]

Pictures and videos open in the viewer, which steps through the rest of their
folder in name order. Multiple files open as a selection in the supplied
order. A folder, a PDF, or no argument at all opens the library. Use -- before
filenames that begin with a dash.

Options:
  --library              Open files in the library instead of the viewer. The
                         file is selected and its folder is added to the
                         library if it is not already watched.
  --demo                 Browse a deterministic fictional library instead of
                         your own files. Nothing personal appears on screen.
  --render <file.png>    Render the window to a PNG and exit. Draws offscreen,
                         so no compositor can resize it or overlap it.
  --render-view <view>   Which view to render: grid, detail, video, slideshow,
                         matte, corrections, compare, export, rename, OCR,
                         duplicates, browser, settings, viewer, viewer-video,
                         viewer-info or viewer-menu.
  --render-size <WxH>    Window size, from 560x420 to 7680x4320. Default 1280x820.
  --version              Print the version and exit.
  --help                 Show this message.)"
                      << Qt::endl;
}

// Creates a window from the module in its own context, so the names it needs
// sit beside the shared ones without leaking into the other window.
QQuickWindow* createWindow(QQmlEngine& engine, QQmlContext& context, const char* type,
                           std::unique_ptr<QObject>& root) {
  QQmlComponent component(&engine);
  component.loadFromModule("Omaroll", type);
  root.reset(component.create(&context));
  if (!root) {
    for (const QQmlError& error : component.errors()) {
      qWarning().noquote() << error.toString();
    }
    return nullptr;
  }
  return qobject_cast<QQuickWindow*>(root.get());
}

// Everything the library window needs. It is built the first time the library
// is asked for, so a picture opened from the file manager reaches the screen
// without a single watched folder being scanned.
class LibraryWindow {
public:
  LibraryWindow(QQmlEngine& engine, AppSettings& settings, ActionLauncher& actions, bool demo,
                const OpenRequest& request)
      : m_captures(&settings),
        // Demo and render order is deliberately fixed. Real libraries are
        // enriched in the background after their fast filesystem scan lands.
        m_mediaMetadata(demo ? nullptr : &m_captures),
        m_textIndex(&m_captures),
        m_qr(&m_captures),
        m_duplicates(&m_captures),
        m_similarities(&m_captures),
        m_context(engine.rootContext()) {
    // QML only ever sees the proxy, so sorting and filtering can change
    // without any delegate knowing.
    m_library.setSourceModel(&m_captures);
    QObject::connect(&m_library, &CaptureFilterModel::searchTextChanged, &m_textIndex,
                     [this] { m_textIndex.setSearchText(m_library.searchText()); });
    QObject::connect(&m_textIndex, &OcrIndex::textReady, &m_library,
                     &CaptureFilterModel::setOcrText);
    QObject::connect(&m_library, &CaptureFilterModel::duplicatesOnlyChanged, &m_duplicates,
                     [this] { m_duplicates.setActive(m_library.duplicatesOnly()); });
    QObject::connect(&m_duplicates, &DuplicateIndex::groupsChanged, &m_library,
                     [this] { m_library.setDuplicateGroups(m_duplicates.groups()); });
    QObject::connect(&m_library, &CaptureFilterModel::similarOnlyChanged, &m_similarities,
                     [this] { m_similarities.setActive(m_library.similarOnly()); });
    QObject::connect(&m_similarities, &SimilarityIndex::groupsChanged, &m_library,
                     [this] { m_library.setSimilarGroups(m_similarities.groups()); });

    // Restore what the user last chose, then keep the two in step. Doing it
    // here rather than in either class keeps the proxy unaware of persistence
    // and the settings unaware of the model.
    m_library.setSortMode(settings.sortMode());
    m_library.setKindFilter(settings.kindFilter());
    m_library.setShowHidden(settings.showHidden());
    QObject::connect(&m_library, &CaptureFilterModel::sortModeChanged, &settings,
                     [this, &settings] { settings.setSortMode(m_library.sortMode()); });
    QObject::connect(&m_library, &CaptureFilterModel::kindFilterChanged, &settings,
                     [this, &settings] { settings.setKindFilter(m_library.kindFilter()); });
    QObject::connect(&m_library, &CaptureFilterModel::showHiddenChanged, &settings,
                     [this, &settings] { settings.setShowHidden(m_library.showHidden()); });

    add(request);

    // A tracked tool's half-written output stays out of the library until the
    // run settles; writing to an existing file fires no directory event, so
    // the release also rescans to pick the finished file up.
    QObject::connect(&actions, &ActionLauncher::outputPending, &m_captures,
                     &CaptureModel::holdPath);
    QObject::connect(&actions, &ActionLauncher::outputSettled, &m_captures,
                     &CaptureModel::releasePath);

    m_context.setContextProperty(QStringLiteral("Captures"), &m_library);
    m_context.setContextProperty(QStringLiteral("Library"), &m_captures);
    m_context.setContextProperty(QStringLiteral("Matte"), &m_matte);
    m_context.setContextProperty(QStringLiteral("ImageEdit"), &m_imageEditor);
    m_context.setContextProperty(QStringLiteral("TextIndex"), &m_textIndex);
    m_context.setContextProperty(QStringLiteral("Qr"), &m_qr);
    m_context.setContextProperty(QStringLiteral("Subtitles"), &m_subtitles);
    m_context.setContextProperty(QStringLiteral("Duplicates"), &m_duplicates);
    m_context.setContextProperty(QStringLiteral("Similarities"), &m_similarities);
    m_context.setContextProperty(QStringLiteral("MediaInfo"), &m_mediaInfo);
    m_context.setContextProperty(QStringLiteral("PdfInfo"), &m_pdfInfo);
    m_context.setContextProperty(QStringLiteral("MediaMetadata"), &m_mediaMetadata);
    m_context.setContextProperty(QStringLiteral("Tailscale"), &m_tailscale);
    m_context.setContextProperty(QStringLiteral("InitialPaths"), request.files);
    m_context.setContextProperty(QStringLiteral("InitialFolderPath"), request.folder);
    m_window = createWindow(engine, m_context, "Main", m_root);
  }

  LibraryWindow(const LibraryWindow&) = delete;
  LibraryWindow& operator=(const LibraryWindow&) = delete;

  [[nodiscard]] QQuickWindow* window() const { return m_window; }

  // A request for a window that already exists: bring it forward and open
  // what was asked for, the same way the first launch would have.
  void open(const OpenRequest& opened) {
    m_window->show();
    m_window->raise();
    m_window->requestActivate();
    add(opened);
    if (!opened.folder.isEmpty()) {
      QMetaObject::invokeMethod(m_window, "openFolder", Q_ARG(QVariant, opened.folder));
    } else if (!opened.files.isEmpty()) {
      QMetaObject::invokeMethod(m_window, "openPaths", Q_ARG(QVariant, opened.files));
    }
  }

private:
  // "Open with": a file is selected once the scan lands, and its folder joins
  // the library if no watched root already covers it. A folder simply joins.
  void add(const OpenRequest& opened) {
    if (!opened.folder.isEmpty()) {
      m_captures.setExtraRoot(opened.folder);
    } else if (!opened.files.isEmpty()) {
      m_captures.addExtraFiles(opened.files);
      if (opened.files.size() == 1) {
        m_captures.setExtraRoot(QFileInfo(opened.files.first()).canonicalPath());
      }
    }
  }

  CaptureModel m_captures;
  MediaMetadataIndex m_mediaMetadata;
  CaptureFilterModel m_library;
  OcrIndex m_textIndex;
  QrDetector m_qr;
  SubtitleIndex m_subtitles;
  DuplicateIndex m_duplicates;
  SimilarityIndex m_similarities;
  MediaInspector m_mediaInfo;
  PdfInspector m_pdfInfo;
  TailscalePeers m_tailscale;
  MatteComposer m_matte;
  ImageEditor m_imageEditor;
  QQmlContext m_context;
  // Last, so the window goes before the context and services it binds to.
  std::unique_ptr<QObject> m_root;
  QQuickWindow* m_window = nullptr;
};

// The quick viewer: one window, reused for every file opened from outside.
class ViewerWindow {
public:
  explicit ViewerWindow(QQmlEngine& engine) : m_context(engine.rootContext()) {
    // Not "Viewer": that name is the window's own QML type, and a type name
    // wins over a context property.
    m_context.setContextProperty(QStringLiteral("Session"), &m_session);
    m_context.setContextProperty(QStringLiteral("MediaInfo"), &m_mediaInfo);
    m_context.setContextProperty(QStringLiteral("Subtitles"), &m_subtitles);
    m_window = createWindow(engine, m_context, "Viewer", m_root);
  }

  ViewerWindow(const ViewerWindow&) = delete;
  ViewerWindow& operator=(const ViewerWindow&) = delete;

  [[nodiscard]] QQuickWindow* window() const { return m_window; }
  [[nodiscard]] ViewerSession& session() { return m_session; }

  void open(const QStringList& files) {
    m_session.open(files);
    if (!m_window->isVisible()) {
      // A floating window opens at this size. A tiling compositor ignores
      // the request and gives the window its tile instead.
      if (const QScreen* screen = m_window->screen()) {
        m_window->resize(ViewerSession::preferredWindowSize(screen->availableGeometry().size()));
      }
      m_window->setWindowStates(Qt::WindowNoState);
      m_window->show();
    }
    if (QGuiApplication::platformName() != u"offscreen") {
      m_window->raise();
      m_window->requestActivate();
    }
  }

private:
  ViewerSession m_session;
  MediaInspector m_mediaInfo;
  SubtitleIndex m_subtitles;
  QQmlContext m_context;
  std::unique_ptr<QObject> m_root;
  QQuickWindow* m_window = nullptr;
};

} // namespace

int main(int argc, char* argv[]) {
  const StartupTrace startup;
  QGuiApplication::setApplicationName(QStringLiteral("Omaroll"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Omaroll"));
  QGuiApplication::setApplicationVersion(QStringLiteral(OMAROLL_VERSION));
  QGuiApplication::setOrganizationName(QStringLiteral("Omaroll"));

  // Informational CLI flags must work over SSH and in package containers that
  // have no graphical platform plugin available. Handle them before creating
  // QGuiApplication, which would otherwise abort before printing anything.
  for (int index = 1; index < argc; ++index) {
    const QByteArray argument(argv[index]);
    if (argument == "--") {
      break;
    }
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
  QGuiApplication::setDesktopFileName(QStringLiteral("io.github.tsouth89.omaroll"));

  // Basic rather than a platform style: every colour comes from the Omarchy
  // theme, and a style that injects its own palette fights that.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // A render must come out at the size asked for. Under a tiling compositor
  // the window is resized to its tile the moment it maps, so render against
  // the offscreen platform instead, where the requested size is the size.
  for (int index = 1; index < argc; ++index) {
    const QByteArray argument(argv[index]);
    if (argument == "--") {
      break;
    }
    if (argument == "--render" || argument.startsWith("--render=")) {
      disableHeadlessAudio();
      // Omarchy exports QT_QPA_PLATFORM=wayland session-wide, so this is
      // unconditional. OMAROLL_RENDER_PLATFORM overrides it for checking
      // something the offscreen platform cannot draw, such as video frames.
      const QByteArray platform = qgetenv("OMAROLL_RENDER_PLATFORM");
      qputenv("QT_QPA_PLATFORM", platform.isEmpty() ? QByteArray("offscreen") : platform);
      break;
    }
  }

  QGuiApplication application(argc, argv);
  startup.mark("application");
  QIcon applicationIcon = QIcon::fromTheme(QStringLiteral("io.github.tsouth89.omaroll"));
  if (applicationIcon.isNull()) {
    applicationIcon = QIcon(QStringLiteral(":/icons/resources/icons/omaroll.svg"));
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
  if (rendering && !QMediaDevices::audioOutputs().isEmpty()) {
    QTextStream(stderr) << "omaroll: refusing a headless render with audio outputs available\n";
    return 1;
  }
  if (!rendering && (optionPresent(arguments, QStringLiteral("--render-view")) ||
                     optionPresent(arguments, QStringLiteral("--render-size")))) {
    qWarning().noquote() << "omaroll: --render-view and --render-size require --render";
    return 2;
  }
  const QString renderView = optionValue(arguments, QStringLiteral("--render-view"));
  static const QStringList renderViews = {
      QStringLiteral("grid"),       QStringLiteral("detail"),  QStringLiteral("document"),
      QStringLiteral("video"),
      QStringLiteral("slideshow"),  QStringLiteral("matte"),   QStringLiteral("corrections"),
      QStringLiteral("compare"),    QStringLiteral("export"),  QStringLiteral("rename"),
      QStringLiteral("ocr"),        QStringLiteral("duplicates"), QStringLiteral("browser"),
      QStringLiteral("settings"),   QStringLiteral("viewer"),  QStringLiteral("viewer-video"),
      QStringLiteral("viewer-info"), QStringLiteral("viewer-menu")};
  if (!renderView.isEmpty() && !renderViews.contains(renderView)) {
    qWarning().noquote() << "omaroll: unknown render view:" << renderView;
    return 2;
  }
  const QString renderSize = optionValue(arguments, QStringLiteral("--render-size"));
  static const QRegularExpression sizePattern(QStringLiteral(R"(^[1-9]\d*x[1-9]\d*$)"));
  if (!renderSize.isEmpty() && !sizePattern.match(renderSize).hasMatch()) {
    qWarning().noquote() << "omaroll: render size must look like 1280x820";
    return 2;
  }
  if (!renderSize.isEmpty()) {
    const QStringList dimensions = renderSize.split(QLatin1Char('x'));
    const int width = dimensions.at(0).toInt();
    const int height = dimensions.at(1).toInt();
    if (width < 560 || height < 420 || width > 7680 || height > 4320) {
      qWarning().noquote() << "omaroll: render size must be between 560x420 and 7680x4320";
      return 2;
    }
  }
  const bool demo = rendering || optionPresent(arguments, QStringLiteral("--demo"));
  const OpenRequest request =
      OpenRequest::fromPaths(demo ? QStringList() : positionalArguments(arguments));
  if (!request.error.isEmpty()) {
    qWarning().noquote() << "omaroll:" << request.error;
    return 2;
  }
  const QStringList requestedPaths =
      request.folder.isEmpty() ? request.files : QStringList{request.folder};
  const bool preferLibrary = optionPresent(arguments, QStringLiteral("--library"));
  // Pictures and videos from outside open in the viewer. Folders, documents,
  // a plain launch and an explicit --library open the library.
  const auto opensInViewer = [](const OpenRequest& opened, bool library) {
    return !library && opened.folder.isEmpty() && ViewerSession::canOpen(opened.files);
  };

  // A render is a one-shot batch job and a demo is a throwaway window; neither
  // should take over, or be refused by, a real session's instance.
  SingleInstance instance;
  if (!rendering && !demo) {
    if (!instance.claimOrNotify(requestedPaths, preferLibrary)) {
      return 0;
    }
  }

  // The viewer renders open a demo file the way a file manager would.
  const bool renderingViewer = renderView.startsWith(QStringLiteral("viewer"));
  QStringList viewerRenderFiles;

  if (demo) {
    const DemoLibrary::Layout layout = DemoLibrary::build();
    viewerRenderFiles = {renderView == QStringLiteral("viewer-video")
                             ? layout.videos + QStringLiteral("/ocean-surface.mp4")
                             : layout.pictures + QStringLiteral("/alpine-dawn.jpg")};
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
  startup.mark("theme");
  AppSettings settings;
  if (demo) {
    settings.setSlideshowVideos(true);
  }

  // Bound the thumbnail cache off the GUI thread. A thumbnail is always
  // rebuildable, so this can never lose anything the user cares about.
  const auto pruneThumbnailCache = [&settings] {
    const qint64 bytes = qint64(settings.thumbnailCacheMb()) * 1024 * 1024;
    QThreadPool::globalInstance()->start([bytes] { ThumbnailCache::prune(bytes); });
  };
  pruneThumbnailCache();
  QObject::connect(&settings, &AppSettings::thumbnailCacheMbChanged, &application,
                   pruneThumbnailCache);

  ActionLauncher actions;
  ActionRegistry registry(&actions);

  QQmlApplicationEngine engine;
  auto* thumbnailProvider = new ThumbnailProvider;
  auto* matteProvider = new MatteProvider;
  auto* editProvider = new EditProvider;
  auto* pdfProvider = new PdfProvider;
  engine.addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), thumbnailProvider);
  engine.addImageProvider(QLatin1String(MatteProvider::kProviderId), matteProvider);
  engine.addImageProvider(QLatin1String(EditProvider::kProviderId), editProvider);
  engine.addImageProvider(QLatin1String(PdfProvider::kProviderId), pdfProvider);
  QObject::connect(&application, &QCoreApplication::aboutToQuit, &application,
                   [thumbnailProvider, matteProvider, editProvider, pdfProvider] {
                     thumbnailProvider->shutdown();
                     matteProvider->shutdown();
                     editProvider->shutdown();
                     pdfProvider->shutdown();
                     QThreadPool::globalInstance()->waitForDone();
                   });
  // Both windows share these. Each window's own services live in its context.
  engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("Actions"), &actions);
  engine.rootContext()->setContextProperty(QStringLiteral("Settings"), &settings);
  engine.rootContext()->setContextProperty(QStringLiteral("Registry"), &registry);
  engine.rootContext()->setContextProperty(QStringLiteral("DemoMode"), demo);

  startup.mark("services");

  // Declared after the engine, so both windows go before it does.
  std::unique_ptr<LibraryWindow> libraryWindow;
  std::unique_ptr<ViewerWindow> viewerWindow;

  const auto openLibrary = [&](const OpenRequest& opened) -> QQuickWindow* {
    if (libraryWindow) {
      libraryWindow->open(opened);
      return libraryWindow->window();
    }
    libraryWindow = std::make_unique<LibraryWindow>(engine, settings, actions, demo, opened);
    if (!libraryWindow->window()) {
      // The QML errors are already printed; a later request may try again.
      libraryWindow.reset();
      return nullptr;
    }
    return libraryWindow->window();
  };

  const auto openViewer = [&](const QStringList& files) -> QQuickWindow* {
    if (!viewerWindow) {
      viewerWindow = std::make_unique<ViewerWindow>(engine);
      if (!viewerWindow->window()) {
        viewerWindow.reset();
        return nullptr;
      }
      // The viewer hands a file to the library, then steps aside. The library
      // is up before the viewer closes, so closing it never ends the app.
      QObject::connect(&viewerWindow->session(), &ViewerSession::libraryRequested, &application,
                       [&](const QString& path) {
                         OpenRequest opened;
                         opened.files = {path};
                         if (QQuickWindow* library = openLibrary(opened)) {
                           library->raise();
                           library->requestActivate();
                           viewerWindow->window()->close();
                         }
                       });
    }
    viewerWindow->open(files);
    return viewerWindow->window();
  };

  const bool startInViewer = rendering ? renderingViewer : opensInViewer(request, preferLibrary);
  QQuickWindow* window = startInViewer
                             ? openViewer(rendering ? viewerRenderFiles : request.files)
                             : openLibrary(request);
  startup.mark("qml");
  if (!window) {
    return 1;
  }
  startup.watch(window);

  if (!rendering && !demo) {
    QObject::connect(
        &instance, &SingleInstance::activationRequested, &application,
        [&](const QStringList& paths, bool library) {
          const OpenRequest opened = OpenRequest::fromPaths(paths);
          if (!opened.error.isEmpty()) {
            // Said in whichever window is in front; both have a status line.
            QQuickWindow* shown = viewerWindow && viewerWindow->window()->isVisible()
                                      ? viewerWindow->window()
                                  : libraryWindow ? libraryWindow->window()
                                                  : nullptr;
            if (shown) {
              QMetaObject::invokeMethod(shown, "say", Q_ARG(QVariant, opened.error));
            } else {
              qWarning().noquote() << "omaroll:" << opened.error;
            }
            return;
          }
          if (opensInViewer(opened, library)) {
            openViewer(opened.files);
          } else {
            openLibrary(opened);
          }
        });
  }

  if (rendering) {
    const QString sizeText = renderSize;
    const QStringList parts = sizeText.split(QLatin1Char('x'), Qt::SkipEmptyParts);
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
      QTextStream(stdout) << "wrote " << renderPath << Qt::endl;
      QCoreApplication::quit();
    });
  }

  return QGuiApplication::exec();
}
