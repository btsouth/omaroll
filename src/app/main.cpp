#include "actions/ClipboardServe.h"
#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/ExternalEditors.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/Diagnostics.h"
#include "app/HyprlandSetup.h"
#include "app/HeadlessAudio.h"
#include "app/OpenRequest.h"
#include "app/PlaybackHeap.h"
#include "app/LaunchRequest.h"
#include "app/MimeDefaults.h"
#include "app/SingleInstance.h"
#include "app/StartupTrace.h"
#include "app/VideoPlayback.h"
#include "app/VideoWarmup.h"
#include "library/CaptureFilterModel.h"
#include "library/FolderTreeModel.h"
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
#include "search/DocumentTextIndex.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "subtitles/SubtitleIndex.h"
#include "sources/CaptureScanner.h"
#include "sources/DecodeJournal.h"
#include "theme/OmarchyTheme.h"
#include "sources/ImageBudget.h"
#include "thumbs/LargeImageProvider.h"
#include "thumbs/RawImageProvider.h"
#include "thumbs/Storyboard.h"
#include "thumbs/ThumbnailCache.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/MprisService.h"
#include "viewer/ViewerSession.h"
#include "viewer/ViewerWindows.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
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
#include <QDeadlineTimer>
#include <QTimer>

#include <cstring>
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
      QStringLiteral("--start"), QStringLiteral("--render"), QStringLiteral("--render-view"), QStringLiteral("--render-size")};
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
      QStringLiteral("--start"), QStringLiteral("--render"), QStringLiteral("--render-view"), QStringLiteral("--render-size")};
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
filenames that begin with a dash. Press ? or F1 in any window for shortcuts.

Options:
  --library              Open files in the library instead of the viewer. The
                         file is selected and its folder is added to the
                         library if it is not already watched.
  --start <time>         Start the request's first video at seconds, mm:ss or
                         hh:mm:ss (fractions allowed). Overrides saved resume.
  --set-default [group]  Use Omaroll for a group of file types: images,
                         videos, pdf, or all (images and videos, the default).
                         Add --verbose to list every type.
  --unset-default [group]
                         Put back the apps --set-default replaced, for the
                         same groups.
  --install-window-rules Add Omaroll's Hyprland rules to ~/.config/hypr: full
                         opacity for its windows and a floating viewer.
  --remove-window-rules  Take those rules out again.
  --diagnostics          Print the versions, graphics and optional tools that a
                         bug report needs, and exit. No window opens.
  --demo                 Browse a deterministic fictional library instead of
                         your own files. Nothing personal appears on screen.
  --render <file.png>    Render the window to a PNG and exit. Draws offscreen,
                         so no compositor can resize it or overlap it. Fails
                         without writing if the view never becomes ready.
  --render-view <view>   Which view to render: grid, detail, document, video,
                         slideshow, matte, corrections, compare, export,
                         rename, ocr, editors, pins, context-menu, selection-menu,
                         duplicates, similar, browser, sidebar, settings, viewer,
                         viewer-video, viewer-info, viewer-menu, viewer-help or
                         grid-filenames.
  --render-size <WxH>    Window size, from 560x420 (viewer 320x240) to 7680x4320.
                         Default 1280x820.
  --version              Print the version and exit.
  --help                 Show this message.

Examples:
  omaroll --set-default          Open pictures and videos with Omaroll
  omaroll photo.jpg              View a picture and step through its folder
  omaroll ~/Pictures             Browse a folder in the library
  omaroll --start=1:23 clip.mp4  Play a video from 1 minute 23 seconds
  omaroll --diagnostics          Details to paste into a bug report)"
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
        m_documentText(&m_captures),
        m_qr(&m_captures),
        m_duplicates(&m_captures),
        m_similarities(&m_captures),
        m_context(engine.rootContext()) {
    // QML only ever sees the proxy, so sorting and filtering can change
    // without any delegate knowing.
    m_library.setSourceModel(&m_captures);
    // Only plain words and phrases need recognized text; a query of field
    // tokens such as type:video starts no indexing.
    QObject::connect(&m_library, &CaptureFilterModel::searchTextChanged, &m_textIndex, [this] {
      m_textIndex.setSearchText(m_library.textSearch());
      m_documentText.setSearchText(m_library.textSearch());
    });
    QObject::connect(&m_textIndex, &OcrIndex::textReady, &m_library,
                     &CaptureFilterModel::setOcrText);
    // PDF text has a store of its own, so a file that changes between picture
    // and PDF keeps each producer's text apart. Most of it can wait in the
    // disk cache rather than memory.
    QObject::connect(&m_documentText, &DocumentTextIndex::textReady, &m_library,
                     &CaptureFilterModel::setDocumentText);
    m_library.setDocumentTextLoader(&DocumentTextIndex::cachedText);
    QObject::connect(&m_library, &CaptureFilterModel::documentTextLost, &m_documentText,
                     &DocumentTextIndex::reread);
    QObject::connect(&m_library, &CaptureFilterModel::duplicatesOnlyChanged, &m_duplicates,
                     [this] { m_duplicates.setActive(m_library.duplicatesOnly()); });
    QObject::connect(&m_duplicates, &DuplicateIndex::groupsChanged, &m_library,
                     [this] { m_library.setDuplicateGroups(m_duplicates.groups()); });
    QObject::connect(&m_library, &CaptureFilterModel::similarOnlyChanged, &m_similarities,
                     [this] { m_similarities.setActive(m_library.similarOnly()); });
    QObject::connect(&m_similarities, &SimilarityIndex::groupsChanged, &m_library,
                     [this] { m_library.setSimilarGroups(m_similarities.groups()); });
    // Recognized text keeps different screenshots out of one similar set.
    QObject::connect(&m_textIndex, &OcrIndex::textReady, &m_similarities,
                     &SimilarityIndex::setText);
    m_similarities.setStrictness(settings.similarStrictness());
    QObject::connect(&settings, &AppSettings::similarStrictnessChanged, &m_similarities,
                     [this, &settings] { m_similarities.setStrictness(settings.similarStrictness()); });
    m_textIndex.setScreenshotsOnly(settings.textIndexScreenshotsOnly());
    m_textIndex.setBackground(settings.backgroundTextIndex() && !demo);
    m_documentText.setBackground(settings.backgroundTextIndex() && !demo);
    QObject::connect(&settings, &AppSettings::textIndexScreenshotsOnlyChanged, &m_textIndex,
                     [this, &settings] {
                       m_textIndex.setScreenshotsOnly(settings.textIndexScreenshotsOnly());
                     });
    QObject::connect(&settings, &AppSettings::backgroundTextIndexChanged, &m_textIndex,
                     [this, &settings, demo] {
                       m_textIndex.setBackground(settings.backgroundTextIndex() && !demo);
                       m_documentText.setBackground(settings.backgroundTextIndex() && !demo);
                     });

    // Restore what the user last chose, then keep the two in step. Doing it
    // here rather than in either class keeps the proxy unaware of persistence
    // and the settings unaware of the model.
    m_library.setSortMode(settings.sortMode());
    m_library.setKindFilter(settings.kindFilter());
    m_library.setShowHidden(settings.showHidden());
    m_library.setPairRawJpeg(settings.pairRawJpeg());
    QObject::connect(&settings, &AppSettings::pairRawJpegChanged, &m_library,
                     [this, &settings] { m_library.setPairRawJpeg(settings.pairRawJpeg()); });
    QObject::connect(&m_library, &CaptureFilterModel::sortModeChanged, &settings,
                     [this, &settings] { settings.setSortMode(m_library.sortMode()); });
    QObject::connect(&m_library, &CaptureFilterModel::kindFilterChanged, &settings,
                     [this, &settings] { settings.setKindFilter(m_library.kindFilter()); });
    QObject::connect(&m_library, &CaptureFilterModel::showHiddenChanged, &settings,
                     [this, &settings] { settings.setShowHidden(m_library.showHidden()); });

    m_folderTree.bind(&m_captures, &m_library);
    add(request);

    // A tracked tool's half-written output stays out of the library until the
    // run settles; writing to an existing file fires no directory event, so
    // the release also rescans to pick the finished file up.
    QObject::connect(&actions, &ActionLauncher::outputPending, &m_captures,
                     &CaptureModel::holdPath);
    QObject::connect(&actions, &ActionLauncher::outputSettled, &m_captures,
                     &CaptureModel::releasePath);

    m_context.setContextProperty(QStringLiteral("FolderTree"), &m_folderTree);
    m_context.setContextProperty(QStringLiteral("Captures"), &m_library);
    m_context.setContextProperty(QStringLiteral("Library"), &m_captures);
    m_context.setContextProperty(QStringLiteral("Matte"), &m_matte);
    m_context.setContextProperty(QStringLiteral("ImageEdit"), &m_imageEditor);
    m_context.setContextProperty(QStringLiteral("TextIndex"), &m_textIndex);
    m_context.setContextProperty(QStringLiteral("DocumentText"), &m_documentText);
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

  void requestStop() {
    m_captures.requestStop();
    m_mediaMetadata.requestStop();
    m_textIndex.requestStop();
    m_documentText.requestStop();
    m_duplicates.requestStop();
    m_similarities.requestStop();
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
      QStringList entries;
      for (const auto& path : opened.files) entries.append(opened.entryPaths.value(path, path));
      m_captures.addExtraFiles(entries);
      if (opened.files.size() == 1) {
        m_captures.setExtraRoot(QFileInfo(opened.files.first()).canonicalPath());
      }
    }
  }

  CaptureModel m_captures;
  MediaMetadataIndex m_mediaMetadata;
  CaptureFilterModel m_library;
  FolderTreeModel m_folderTree;
  OcrIndex m_textIndex;
  DocumentTextIndex m_documentText;
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


// A refused launch from a file manager or launcher has no terminal, so it is
// said on screen too, when a notification daemon is there to say it.
void notifyFailure(const QString& message) {
  const QString notify = QStandardPaths::findExecutable(QStringLiteral("notify-send"));
  if (notify.isEmpty()) return;
  QProcess notification;
  notification.start(notify, {QStringLiteral("--"), QStringLiteral("Omaroll"), message});
  if (!notification.waitForFinished(2000)) {
    notification.kill();
    notification.waitForFinished();
  }
}
} // namespace

int main(int argc, char* argv[]) {
  // The clipboard helper the app starts for itself, before anything else.
  if (argc > 1 && std::strcmp(argv[1], ClipboardServe::kOption) == 0) {
    return ClipboardServe::run(argc, argv);
  }
  // Decoded pictures go straight back to the system; playing video keeps its
  // frame buffers instead (app/PlaybackHeap.h).
  PlaybackHeap::usePictureDefaults();
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
    // For someone whose window won't open: the same text as Copy diagnostics,
    // without a display. The theme comes from Omarchy's files instead.
    if (argument == "--diagnostics") {
      // No display needed, but QProcess wants an application for hyprctl.
      const QCoreApplication core(argc, argv);
      const Diagnostics::Sources sources = Diagnostics::Sources::system();
      QTextStream(stdout) << Diagnostics::report(sources, QStringLiteral("none (command line)"),
                                                 Diagnostics::themeName(sources), 0)
                          << Qt::endl;
      return 0;
    }
    if (argument == "--install-window-rules" || argument == "--remove-window-rules") {
      if (argc != 2) {
        QTextStream(stderr) << "omaroll: " << argument << " takes no other arguments\n";
        return 2;
      }
      const QString directory = HyprlandSetup::defaultDirectory();
      const HyprlandSetup::Result result = argument == "--install-window-rules"
                                               ? HyprlandSetup::installRules(directory)
                                               : HyprlandSetup::removeRules(directory);
      QTextStream(result.ok ? stdout : stderr) << result.message << Qt::endl;
      // Installed once, the library and --set-default stop offering them,
      // as when Settings installs them.
      if (result.ok && argument == "--install-window-rules" &&
          !AppSettings::endWindowRulesOfferOnDisk()) {
        QTextStream(stderr) << "omaroll: the settings file could not be read or written, so it was "
                               "left as it is; the library may offer the rules again\n";
      }
      return result.ok ? 0 : 2;
    }
  }

  QStringList rawArguments;
  for (int i = 0; i < argc; ++i) rawArguments.append(QString::fromLocal8Bit(argv[i]));
  if (optionPresent(rawArguments, "--set-default") || optionPresent(rawArguments, "--unset-default")) {
    QCoreApplication core(argc, argv);
    const bool unset = optionPresent(rawArguments, "--unset-default");
    if (unset && optionPresent(rawArguments, "--set-default")) {
      QTextStream(stderr) << "omaroll: use either --set-default or --unset-default\n";
      return 2;
    }
    const QString option = unset ? "--unset-default" : "--set-default";
    // --verbose lists every type; without it, one line and the undo.
    const bool verbose = rawArguments.contains(QStringLiteral("--verbose"));
    if (verbose) rawArguments.removeAll(QStringLiteral("--verbose"));
    QString group = optionValue(rawArguments, option);
    if (group.isEmpty()) group = "all";
    const int expected = rawArguments.contains(option) && !optionValue(rawArguments, option).isEmpty() ? 3 : 2;
    if (rawArguments.size() != expected) { QTextStream(stderr) << "Default operation accepts only one group\n"; return 2; }
    MimeDefaults defaults;
    const QString report = defaults.apply(unset, group);
    QTextStream out(stdout);
    if (verbose) out << report << Qt::endl;
    out << MimeDefaults::summarize(unset, report, defaults.succeeded(), group) << Qt::endl;
    // Any type that changed can be undone, even when another failed.
    if (!unset && report.contains(QStringLiteral(" -> "))) {
      out << "To undo: omaroll --unset-default" << (group == u"all" ? QString() : u' ' + group)
          << Qt::endl;
    }
    // Without the rules a stock Omarchy tiles and dims the viewer it just
    // made the default. Said until they are installed or the offer is turned
    // down in the library.
    if (!unset && defaults.succeeded() &&
        HyprlandSetup::rulesState(HyprlandSetup::defaultDirectory()) ==
            HyprlandSetup::RulesState::NotInstalled &&
        !AppSettings::windowRulesOfferDismissedOnDisk()) {
      out << "To float the viewer and keep it opaque: omaroll --install-window-rules" << Qt::endl;
    }
    if (!verbose && !defaults.succeeded()) {
      // The failed types, so a partial failure can be acted on.
      for (const QString& line : report.split(u'\n', Qt::SkipEmptyParts)) {
        if (line.startsWith(u"Could not") || line.startsWith(u"No matching")) {
          QTextStream(stderr) << line << Qt::endl;
        }
      }
    }
    return defaults.succeeded() ? 0 : 2;
  }

  // On Wayland this is what becomes app_id. The Hyprland media-opacity rule
  // matches on it, so it must stay in lockstep with the .desktop filename and
  // StartupWMClass. Changing it silently breaks the transparency opt-out.
  QGuiApplication::setDesktopFileName(QStringLiteral("io.github.tsouth89.omaroll"));

  // Capture caller overrides now; driver checks wait until the first video.
  videoPlaybackEnvironment();

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

  // The chrome follows Omarchy directly. Avoid loading GTK just for its
  // palette; native folder dialogs still use the configured portal chooser.
  if (qgetenv("QT_QPA_PLATFORMTHEME") == "gtk3") {
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
  }

  QGuiApplication application(argc, argv);
  startup.mark("application");
  // Before anything is decoded, Omaroll's icon included.
  ImageBudget::applyStartupLimit();
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
  if (rendering) configureVideoPlayback();
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
      QStringLiteral("grid"),       QStringLiteral("grid-filenames"),
      QStringLiteral("detail"),     QStringLiteral("document"),
      QStringLiteral("video"),
      QStringLiteral("slideshow"),  QStringLiteral("matte"),   QStringLiteral("corrections"),
      QStringLiteral("compare"),    QStringLiteral("export"),  QStringLiteral("rename"),
      QStringLiteral("ocr"),        QStringLiteral("duplicates"), QStringLiteral("similar"), QStringLiteral("browser"), QStringLiteral("sidebar"),
      QStringLiteral("settings"),   QStringLiteral("editors"), QStringLiteral("pins"),
      QStringLiteral("context-menu"), QStringLiteral("selection-menu"),
      QStringLiteral("viewer"),  QStringLiteral("viewer-video"),
      QStringLiteral("viewer-info"), QStringLiteral("viewer-menu"), QStringLiteral("viewer-help")};
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
    const bool viewer = renderView.startsWith(QStringLiteral("viewer"));
    if (width < (viewer ? 320 : 560) || height < (viewer ? 240 : 420)
        || width > 7680 || height > 4320) {
      qWarning().noquote() << (viewer
          ? "omaroll: render size must be between 320x240 and 7680x4320"
          : "omaroll: render size must be between 560x420 and 7680x4320");
      return 2;
    }
  }
  qint64 startMs = -1;
  if (optionPresent(arguments, "--start")) {
    const auto parsed = LaunchRequest::parseStart(optionValue(arguments, "--start"));
    if (!parsed) {
      // A file manager launch has no terminal to show stderr.
      const QString message = QStringLiteral("Could not use --start %1: give seconds, mm:ss or hh:mm:ss").arg(
          optionValue(arguments, "--start").left(40));
      QTextStream(stderr) << "omaroll: " << message << Qt::endl;
      notifyFailure(message);
      return 2;
    }
    startMs = *parsed;
  }
  const bool demo = rendering || optionPresent(arguments, QStringLiteral("--demo"));
  const OpenRequest request =
      OpenRequest::fromPaths(demo ? QStringList() : positionalArguments(arguments));
  if (!request.error.isEmpty()) {
    QTextStream(stderr) << "omaroll: " << request.error << Qt::endl;
    notifyFailure(request.error);
    return 2;
  }
  QStringList requestedPaths;
  if (!request.folder.isEmpty()) requestedPaths = {request.folder};
  else for (const auto& path : request.files) requestedPaths.append(request.entryPaths.value(path, path));
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
    // Said here too, since a running older version ignores the skipped names.
    if (!request.skipped.isEmpty()) {
      QTextStream(stderr) << "omaroll: " << request.note() << Qt::endl;
    }
    const auto result = instance.claimOrNotify(requestedPaths, preferLibrary, startMs, request.skipped);
    if (result == SingleInstance::Result::Forwarded) return 0;
    if (result == SingleInstance::Result::Error) return 1;
  }
  startup.mark("instance");
  // Before anything is decoded: a file that crashed the last run is skipped
  // until it changes. A render or a demo reads only its own files and leaves
  // the session's journal alone.
  if (!rendering && !demo) {
    // The journal itself on the per-boot tmpfs when there is one: no disk
    // space or copy-on-write involved. Blocked files are kept in the cache.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    DecodeJournal::open(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                            QStringLiteral("/omaroll"),
                        runtime.isEmpty() ? QString() : runtime + QStringLiteral("/omaroll"));
  }
  // A video opened from outside: load the backend and start the GPU driver
  // while the window loads (app/VideoWarmup.h).
  if (!rendering && opensInViewer(request, preferLibrary) &&
      !LaunchRequest::firstVideo(request.files).isEmpty()) {
    VideoWarmup::start();
  }

  // The viewer renders open a demo file the way a file manager would.
  const bool renderingViewer = renderView.startsWith(QStringLiteral("viewer"));
  QStringList viewerRenderFiles;

  if (demo) {
    const DemoLibrary::Layout layout = DemoLibrary::build();
    if (!layout.complete) {
      qWarning().noquote() << "omaroll: the demo library could not be written in full";
      // A render of a partial library would look right and show the wrong thing.
      if (rendering) return 1;
    }
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
  ExternalEditors editors(&actions);

  // The viewers' copies of a turned or flipped picture. Shared, each viewer
  // answering only its own jobs; declared before the engine so it outlives
  // every window, and its saves finish in the drain at quit.
  ImageEditor viewerEditor;
  // Which file crashed the last run, said once in the library.
  DecodeCrashNotice decodeCrash;

  QQmlApplicationEngine engine;
  // Windows are built with QQmlComponent rather than engine.load(), so the
  // engine never wires Qt.quit() up itself and Ctrl+Q would do nothing.
  QObject::connect(&engine, &QQmlEngine::quit, &application, &QCoreApplication::quit,
                   Qt::QueuedConnection);
  auto* thumbnailProvider = new ThumbnailProvider;
  auto* matteProvider = new MatteProvider;
  auto* editProvider = new EditProvider;
  auto* pdfProvider = new PdfProvider;
  auto* rawProvider = new RawImageProvider;
  auto* largeProvider = new LargeImageProvider;
  engine.addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), thumbnailProvider);
  engine.addImageProvider(QLatin1String(MatteProvider::kProviderId), matteProvider);
  engine.addImageProvider(QLatin1String(EditProvider::kProviderId), editProvider);
  engine.addImageProvider(QLatin1String(PdfProvider::kProviderId), pdfProvider);
  engine.addImageProvider(QLatin1String(RawImageProvider::kProviderId), rawProvider);
  engine.addImageProvider(QLatin1String(LargeImageProvider::kProviderId), largeProvider);
  Storyboard::install(engine);
  // Every window shares these. Each window's own services live in its context.
  engine.rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("Actions"), &actions);
  engine.rootContext()->setContextProperty(QStringLiteral("Settings"), &settings);
  // The library's own editor, in its window's context, takes this name there.
  engine.rootContext()->setContextProperty(QStringLiteral("ImageEdit"), &viewerEditor);
  MimeDefaults mimeDefaults;
  engine.rootContext()->setContextProperty(QStringLiteral("MimeDefaults"), &mimeDefaults);
  engine.rootContext()->setContextProperty(QStringLiteral("Registry"), &registry);
  engine.rootContext()->setContextProperty(QStringLiteral("Editors"), &editors);
  engine.rootContext()->setContextProperty(QStringLiteral("DemoMode"), demo);
  engine.rootContext()->setContextProperty(QStringLiteral("DecodeCrash"), &decodeCrash);
  // A render never appears as a player on the desktop's session bus.
  MprisService mpris(&settings, rendering ? std::optional(QDBusConnection(QString())) : std::nullopt);
  engine.rootContext()->setContextProperty(QStringLiteral("Mpris"), &mpris);

  startup.mark("services");

  // Declared after the engine, so every window goes before it does.
  std::unique_ptr<LibraryWindow> libraryWindow;
  QObject::connect(&application, &QCoreApplication::aboutToQuit, &application,
                   [&libraryWindow, thumbnailProvider, matteProvider, editProvider, pdfProvider, rawProvider,
                    largeProvider] {
                     if (libraryWindow) libraryWindow->requestStop();
                     thumbnailProvider->shutdown();
                     matteProvider->shutdown();
                     editProvider->shutdown();
                     pdfProvider->shutdown();
                     rawProvider->shutdown();
                     largeProvider->shutdown();
                     Storyboard::shutdown();
                     // Saves on the pool still use their editors, so a slow one is
                     // reported and then finished rather than cut off.
                     if (!QThreadPool::globalInstance()->waitForDone(QDeadlineTimer(1500))) {
                       qWarning("Background workers exceeded the shutdown drain deadline");
                       QThreadPool::globalInstance()->waitForDone();
                     }
                   });

  ViewerWindows viewers(engine);

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

  // A viewer hands a file to the library, then steps aside. The library is
  // up before the viewer closes, so closing it never ends the app.
  QObject::connect(&viewers, &ViewerWindows::libraryRequested, &application,
                   [&](const QString& path, QQuickWindow* viewer, bool similar) {
                     const OpenRequest opened = OpenRequest::fromPaths({path});
                     if (QQuickWindow* library = openLibrary(opened)) {
                       if (similar) {
                         QMetaObject::invokeMethod(
                             library, "findSimilar",
                             Q_ARG(QVariant, opened.files.value(0, path)));
                       }
                       library->raise();
                       library->requestActivate();
                       viewer->close();
                     }
                   });
  // A --start time belongs to the window that receives the request, so it
  // can't seek another window showing the same video, or linger.
  const auto openViewer = [&](const QStringList& files, const QHash<QString, QString>& entries = {},
                              qint64 start = -1) {
    QQuickWindow* opened = viewers.open(files, entries);
    if (start >= 0 && opened) {
      if (ViewerSession* session = viewers.sessionOf(opened)) {
        session->setStartPosition(LaunchRequest::firstVideo(files), start);
      }
    }
    return opened;
  };

  const bool startInViewer = rendering ? renderingViewer : opensInViewer(request, preferLibrary);
  QQuickWindow* window = startInViewer
                             ? openViewer(rendering ? viewerRenderFiles : request.files,
                                          rendering ? QHash<QString, QString>() : request.entryPaths,
                                          rendering ? -1 : startMs)
                             : openLibrary(request);
  startup.mark("qml");
  if (!window) {
    return 1;
  }
  startup.watch(window);
  if (!request.note().isEmpty()) QTimer::singleShot(0, window, [window, request] {
    QMetaObject::invokeMethod(window, "say", Q_ARG(QVariant, request.note()));
  });

  // Another launch, or files chosen in the empty library: the same request
  // the first launch would have made.
  const auto openRequest = [&](const QStringList& paths, bool library, qint64 start,
                               const QStringList& skipped) {
    OpenRequest opened = OpenRequest::fromPaths(paths);
    if (!opened.error.isEmpty()) {
      // Said in whichever window is in front; both have a status line.
      QQuickWindow* shown = viewers.frontmost() ? viewers.frontmost()
                            : libraryWindow   ? libraryWindow->window()
                                              : nullptr;
      if (shown) {
        QMetaObject::invokeMethod(shown, "say", Q_ARG(QVariant, opened.error));
      } else {
        qWarning().noquote() << "omaroll:" << opened.error;
      }
      return;
    }
    opened.skipped.append(skipped);
    QQuickWindow* receiver = opensInViewer(opened, library)
        ? openViewer(opened.files, opened.entryPaths, start) : openLibrary(opened);
    if (receiver && !opened.note().isEmpty())
      QMetaObject::invokeMethod(receiver, "say", Q_ARG(QVariant, opened.note()));
  };
  if (!rendering && !demo) {
    QObject::connect(&instance, &SingleInstance::activationRequested, &application, openRequest);
  }
  if (!rendering) {
    QObject::connect(&actions, &ActionLauncher::openRequested, &application,
                     [&](const QStringList& paths) { openRequest(paths, false, -1, {}); });
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

    const auto grab = [window, renderPath] {
      const QImage frame = window->grabWindow();
      if (frame.isNull() || !frame.save(renderPath)) {
        qWarning().noquote() << "could not write" << renderPath;
        QCoreApplication::exit(1);
        return;
      }
      QTextStream(stdout) << "wrote " << renderPath << Qt::endl;
      QCoreApplication::quit();
    };
    // Some views wait on a scan (Compare needs a similar set), so from the
    // usual moment keep asking the window whether the view is up, for at most
    // a minute, and give a late one a second to settle.
    auto waited = std::make_shared<int>(0);
    auto* poll = new QTimer(&application);
    poll->setInterval(250);
    QObject::connect(poll, &QTimer::timeout, &application, [window, grab, waited, poll] {
      QVariant ready = true;
      if (window->metaObject()->indexOfMethod("renderReady()") >= 0) {
        QMetaObject::invokeMethod(window, "renderReady", Q_RETURN_ARG(QVariant, ready));
      }
      if (!ready.toBool() && *waited < 49000) {
        *waited += 250;
        return;
      }
      poll->stop();
      if (!ready.toBool()) {
        // A picture of the wrong view is worse than none.
        qWarning().noquote() << "omaroll: the view never became ready; nothing was written";
        QCoreApplication::exit(1);
        return;
      }
      QTimer::singleShot(*waited > 0 ? 1000 : 0, QCoreApplication::instance(), grab);
    });
    QTimer::singleShot(11000, &application, [poll] { poll->start(); });
  }

  return QGuiApplication::exec();
}
