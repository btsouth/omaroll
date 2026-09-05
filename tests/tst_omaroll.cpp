#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/HeadlessAudio.h"
#include "app/SingleInstance.h"
#include "app/OpenRequest.h"
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/DuplicateIndex.h"
#include "library/MediaMetadataIndex.h"
#include "library/MediaInspector.h"
#include "library/SimilarityIndex.h"
#include "matte/HueExtractor.h"
#include "matte/MatteComposer.h"
#include "pdf/PdfInspector.h"
#include "pdf/PdfSupport.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "sources/CaptureLocations.h"
#include "sources/CaptureScanner.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailCache.h"

#include <QClipboard>
#include <QImageReader>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class OmarollTest : public QObject {
  Q_OBJECT

private slots:
  // Settings and the thumbnail cache both write to the home directory. Point
  // them at a scratch tree so a test run cannot touch a real library's marks.
  void initTestCase() {
    QVERIFY(m_scratch.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_scratch.filePath(QStringLiteral("config")));
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch.filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME", m_scratch.filePath(QStringLiteral("data")).toUtf8()));
  }

  void singleInstanceForwardsTheOpenedPath() {
    const QString server = QStringLiteral("omaroll-test-%1-%2")
                               .arg(QCoreApplication::applicationPid())
                               .arg(QRandomGenerator::global()->generate());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);

    SingleInstance second(server);
    const QString path = QStringLiteral("/tmp/a strange # capture.png");
    QVERIFY(!second.claimOrNotify({path}));
    QTRY_COMPARE_WITH_TIMEOUT(activation.size(), 1, 1000);
    QCOMPARE(activation.first().first().toStringList(), QStringList{path});
  }

  void singleInstanceForwardsMultiplePaths() {
    const QString server = QStringLiteral("omaroll-selection-%1").arg(QCoreApplication::applicationPid());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);
    const QStringList paths{QStringLiteral("/tmp/one folder/a # 雪.png"),
                            QStringLiteral("/tmp/other folder/b.webp"),
                            QStringLiteral("/tmp/one folder/c.gif")};
    SingleInstance second(server);
    QVERIFY(!second.claimOrNotify(paths));
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().first().toStringList(), paths);
    SingleInstance third(server);
    QVERIFY(!third.claimOrNotify());
    QTRY_COMPARE(activation.size(), 2);
    QVERIFY(activation.last().first().toStringList().isEmpty());
  }

  void openRequestValidatesAndPreservesSelection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a space # 雪.png"));
    const QString second = dir.filePath(QStringLiteral(".hidden.webp"));
    for (const QString& path : {first, second}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("fixture");
    }
    const QString alias = dir.filePath(QStringLiteral("alias.png"));
    QVERIFY(QFile::link(first, alias));
    const OpenRequest selection = OpenRequest::fromPaths({second, first, alias});
    QVERIFY(selection.error.isEmpty());
    QCOMPARE(selection.files, (QStringList{second, first}));
    QVERIFY(selection.folder.isEmpty());
    const OpenRequest folder = OpenRequest::fromPaths({dir.path()});
    QVERIFY(folder.error.isEmpty());
    QCOMPARE(folder.folder, dir.path());
    for (const QStringList& paths : {QStringList{first, dir.filePath("missing.png")},
                                     QStringList{first, dir.path()},
                                     QStringList{QDir::homePath()}}) {
      const OpenRequest rejected = OpenRequest::fromPaths(paths);
      QVERIFY(!rejected.error.isEmpty());
      QVERIFY(rejected.files.isEmpty());
      QVERIFY(rejected.folder.isEmpty());
    }
    // Explicit files do not pull in neighbors and may include dotfiles.
    const auto records = CaptureScanner::scan({{second, 1}, {first, 1}, {alias, 1}});
    QCOMPARE(records.size(), 2);
    QSet<QString> found;
    for (const auto& record : records) found.insert(record.path);
    QCOMPARE(found, (QSet<QString>{first, second}));
  }

  void singleInstanceAcceptsLegacyAndFragmentedRequests() {
    const QString server = QStringLiteral("omaroll-fragments-%1").arg(QCoreApplication::applicationPid());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);
    QLocalSocket sender;
    sender.connectToServer(server);
    QVERIFY(sender.waitForConnected());
    const QString path = QStringLiteral("/tmp/space # 雪.png");
    const QByteArray message = QJsonDocument(QJsonObject{{QStringLiteral("path"), path}}).toJson(QJsonDocument::Compact);
    sender.write(message.left(message.size() / 2));
    sender.flush();
    QTest::qWait(30);
    QCOMPARE(activation.size(), 0);
    sender.write(message.mid(message.size() / 2) + "\n");
    sender.flush();
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().first().toStringList(), QStringList{path});
  }

  void executableForwardsCanonicalSelectionAndRejectsInvalidBatch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("--render=雪.png"));
    const QString second = dir.filePath(QStringLiteral("a space #.webp"));
    for (const QString& path : {first, second}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("fixture");
    }
    const QByteArray oldDisplay = qgetenv("WAYLAND_DISPLAY");
    qputenv("WAYLAND_DISPLAY", QFileInfo(dir.path()).fileName().toUtf8());
    SingleInstance receiver;
    oldDisplay.isNull() ? qunsetenv("WAYLAND_DISPLAY") : qputenv("WAYLAND_DISPLAY", oldDisplay);
    QVERIFY(receiver.claimOrNotify());
    QSignalSpy activation(&receiver, &SingleInstance::activationRequested);
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QString());
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QFileInfo(dir.path()).fileName());
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(dir.path());
    const QString binary = QCoreApplication::applicationDirPath() + QStringLiteral("/omaroll");
    process.start(binary, {QStringLiteral("--"), QFileInfo(first).fileName(), second, first});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().first().toStringList(), (QStringList{first, second}));
    activation.clear();
    process.start(binary, {first, dir.filePath(QStringLiteral("missing.png"))});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 2);
    QVERIFY(process.readAllStandardError().contains("does not exist"));
    QTest::qWait(30);
    QVERIFY(activation.isEmpty());
  }

  void disabledXdgPictureDirectoryDoesNotScanHome() {
    const QByteArray previous = qgetenv("XDG_PICTURES_DIR");
    QVERIFY(qputenv("XDG_PICTURES_DIR", QDir::homePath().toUtf8()));
    QCOMPARE(CaptureLocations::pictures(), QDir::homePath() + QStringLiteral("/Pictures"));
    if (previous.isNull()) {
      qunsetenv("XDG_PICTURES_DIR");
    } else {
      QVERIFY(qputenv("XDG_PICTURES_DIR", previous));
    }
  }

  void automaticFoldersAreVisibleCoalescedAndReportAvailability() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pictures = dir.filePath(QStringLiteral("Pictures"));
    const QString videos = dir.filePath(QStringLiteral("Videos"));
    const QString downloads = dir.filePath(QStringLiteral("Downloads"));
    QVERIFY(QDir().mkpath(pictures));
    QVERIFY(QDir().mkpath(downloads));

    const QByteArray oldPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray oldVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray oldDownloads = qgetenv("XDG_DOWNLOAD_DIR");
    const QByteArray oldShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray oldRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("XDG_PICTURES_DIR", oldPictures);
      putBack("XDG_VIDEOS_DIR", oldVideos);
      putBack("XDG_DOWNLOAD_DIR", oldDownloads);
      putBack("OMARCHY_SCREENSHOT_DIR", oldShots);
      putBack("OMARCHY_SCREENRECORD_DIR", oldRecordings);
    });
    QVERIFY(qputenv("XDG_PICTURES_DIR", pictures.toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", videos.toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", downloads.toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", pictures.toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", videos.toUtf8()));

    AppSettings settings;
    settings.setScanDownloads(true);
    CaptureModel model(&settings);
    const QVariantList folders = model.automaticFolders();
    QCOMPARE(folders.size(), 3);

    QHash<QString, QVariantMap> byPath;
    for (const QVariant& value : folders) {
      const QVariantMap row = value.toMap();
      byPath.insert(row.value(QStringLiteral("path")).toString(), row);
    }
    QCOMPARE(byPath.value(pictures).value(QStringLiteral("label")).toString(),
             QStringLiteral("Screenshots + Pictures"));
    QVERIFY(byPath.value(pictures).value(QStringLiteral("available")).toBool());
    QCOMPARE(byPath.value(videos).value(QStringLiteral("label")).toString(),
             QStringLiteral("Screen recordings + Videos"));
    QVERIFY(!byPath.value(videos).value(QStringLiteral("available")).toBool());
    QVERIFY(byPath.value(downloads).value(QStringLiteral("available")).toBool());
    QVERIFY(model.folderAvailable(pictures));
    QVERIFY(!model.folderAvailable(videos));

    QSignalSpy changed(&model, &CaptureModel::automaticFoldersChanged);
    settings.setScanDownloads(false);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(model.automaticFolders().size(), 2);
  }

  void additionalLibraryFoldersPersistAndRejectHome() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AppSettings settings;
    QVERIFY(!settings.addLibraryFolder(QUrl::fromLocalFile(QDir::homePath())));
    QVERIFY(!settings.addLibraryFolder(QUrl::fromLocalFile(QDir::rootPath())));
    QVERIFY(settings.addLibraryFolder(QUrl::fromLocalFile(dir.path())));
    QVERIFY(!settings.addLibraryFolder(QUrl::fromLocalFile(dir.path())));
    QVERIFY(settings.libraryFolders().contains(dir.path()));
    settings.setImagePrimaryAction(QStringLiteral("view"));
    settings.setVideoPrimaryAction(QStringLiteral("play"));
    settings.setThumbnailCacheMb(512);
    settings.setTileWidth(320);
    settings.setSlideshowVideos(true);

    const QString unavailable = dir.path();
    QVERIFY(QDir(unavailable).removeRecursively());
    AppSettings restored;
    QVERIFY(restored.libraryFolders().contains(unavailable));
    QCOMPARE(restored.imagePrimaryAction(), QStringLiteral("view"));
    QCOMPARE(restored.videoPrimaryAction(), QStringLiteral("play"));
    QCOMPARE(restored.thumbnailCacheMb(), 512);
    QCOMPARE(restored.tileWidth(), 320);
    restored.setTileWidth(10);
    QCOMPARE(restored.tileWidth(), 160);
    restored.setTileWidth(5000);
    QCOMPARE(restored.tileWidth(), 480);
    restored.setTileWidth(240);
    QVERIFY(restored.slideshowVideos());
    restored.setImagePrimaryAction(QStringLiteral("invalid"));
    restored.setVideoPrimaryAction(QStringLiteral("invalid"));
    QCOMPARE(restored.imagePrimaryAction(), QStringLiteral("view"));
    QCOMPARE(restored.videoPrimaryAction(), QStringLiteral("play"));
    restored.removeLibraryFolder(unavailable);
    QVERIFY(!restored.libraryFolders().contains(unavailable));
    restored.setImagePrimaryAction(QStringLiteral("matte"));
    restored.setVideoPrimaryAction(QStringLiteral("trim"));
    restored.setThumbnailCacheMb(256);
    restored.setSlideshowVideos(false);
  }

  void themeUsesLauncherSurfaceColorAndAlpha() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("mode = \"dark\"\nbackground = \"#101820\"\n"
                 "darker_background = \"#05080a\"\nforeground = \"#f0f0f0\"\n");
    colors.close();

    QFile shell(root + QStringLiteral("/theme/shell.toml"));
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"#223344\"\nbackground-alpha = 0.73\n");
    shell.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#223344")));
    QCOMPARE(theme.surfaceAlpha(), 0.73);

    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"#445566\"\nbackground-alpha = 0.91\n");
    shell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#445566")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.91, 1500);

    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"not-a-color\"\nbackground-alpha = 4.0\n");
    shell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#05080a")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 1.0, 1500);
  }

  void themeResolvesCompactTerminalPalette() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("accent = \"#589df6\"\nselection = \"#b5d5ff\"\n"
                 "background = \"#1d2837\"\nforeground = \"#ffffff\"\n"
                 "color0 = \"#000000\"\ncolor1 = \"#f9555f\"\n"
                 "color2 = \"#21b089\"\ncolor3 = \"#fef02a\"\n"
                 "color4 = \"#589df6\"\ncolor5 = \"#944d95\"\n"
                 "color6 = \"#1f9ee7\"\ncolor7 = \"#bbbbbb\"\n"
                 "color8 = \"#555555\"\ncolor9 = \"#fa8c8f\"\n"
                 "color10 = \"#35bb9a\"\ncolor11 = \"#ffff55\"\n"
                 "color12 = \"#589df6\"\ncolor13 = \"#e75699\"\n"
                 "color14 = \"#3979bc\"\ncolor15 = \"#ffffff\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.mode(), QStringLiteral("dark"));
    QCOMPARE(theme.accent(), QColor(QStringLiteral("#589df6")));
    QCOMPARE(theme.selection(), QColor(QStringLiteral("#b5d5ff")));
    QCOMPARE(theme.background(), QColor(QStringLiteral("#1d2837")));
    QCOMPARE(theme.darkBackground(), QColor(QStringLiteral("#161e29")));
    QCOMPARE(theme.darkerBackground(), QColor(QStringLiteral("#0f141c")));
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#0f141c")));
    QCOMPARE(theme.lighterBackground(), QColor(QStringLiteral("#1d2837")));
    QCOMPARE(theme.darkForeground(), QColor(QStringLiteral("#555555")));
    QCOMPARE(theme.lightForeground(), QColor(QStringLiteral("#ffffff")));
    QCOMPARE(theme.brightForeground(), QColor(QStringLiteral("#ffffff")));
    QCOMPARE(theme.muted(), QColor(QStringLiteral("#555555")));
    QCOMPARE(theme.red(), QColor(QStringLiteral("#f9555f")));
    QCOMPARE(theme.green(), QColor(QStringLiteral("#21b089")));
    QCOMPARE(theme.yellow(), QColor(QStringLiteral("#fef02a")));
    QCOMPARE(theme.blue(), QColor(QStringLiteral("#589df6")));
  }

  void themeFallsBackAccentToTerminalBlue() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#16181d\"\nforeground = \"#c5c5d2\"\n"
                 "color4 = \"#6c9ef8\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.accent(), QColor(QStringLiteral("#6c9ef8")));
  }

  void themeResolvesLegacySemanticNames() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("mode = \"dark\"\naccent = \"#fcef0c\"\n"
                 "bg = \"#1b1d1e\"\ndark_bg = \"#353738\"\n"
                 "darker_bg = \"#1a1b1c\"\nlighter_bg = \"#1b1d1e\"\n"
                 "fg = \"#a7a8a3\"\ndark_fg = \"#8a8c89\"\n"
                 "light_fg = \"#c5c5be\"\nbright_fg = \"#dadad5\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.background(), QColor(QStringLiteral("#1b1d1e")));
    QCOMPARE(theme.darkBackground(), QColor(QStringLiteral("#353738")));
    QCOMPARE(theme.darkerBackground(), QColor(QStringLiteral("#1a1b1c")));
    QCOMPARE(theme.lighterBackground(), QColor(QStringLiteral("#1b1d1e")));
    QCOMPARE(theme.foreground(), QColor(QStringLiteral("#a7a8a3")));
    QCOMPARE(theme.darkForeground(), QColor(QStringLiteral("#8a8c89")));
    QCOMPARE(theme.lightForeground(), QColor(QStringLiteral("#c5c5be")));
    QCOMPARE(theme.brightForeground(), QColor(QStringLiteral("#dadad5")));
  }

  void themeFindsLegacyOmarchyLocation() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = config.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#202040\"\nforeground = \"#eeeeff\"\n"
                 "accent = \"#6060ff\"\n");
    colors.close();

    QFile name(root + QStringLiteral("/theme.name"));
    QVERIFY(name.open(QIODevice::WriteOnly | QIODevice::Text));
    name.write("legacy-blue\n");
    name.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.themeName(), QStringLiteral("Legacy Blue"));
    QCOMPARE(theme.background(), QColor(QStringLiteral("#202040")));
    QCOMPARE(theme.accent(), QColor(QStringLiteral("#6060ff")));
  }

  void themeFollowsAtomicDirectoryReplacement() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    const QString themePath = root + QStringLiteral("/theme");
    QVERIFY(QDir().mkpath(themePath));
    QFile colors(themePath + QStringLiteral("/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#101020\"\nforeground = \"#eeeeff\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QCOMPARE(theme.background(), QColor(QStringLiteral("#101020")));

    const QString nextPath = root + QStringLiteral("/next-theme");
    QVERIFY(QDir().mkpath(nextPath));
    QFile nextColors(nextPath + QStringLiteral("/colors.toml"));
    QVERIFY(nextColors.open(QIODevice::WriteOnly | QIODevice::Text));
    nextColors.write("background = \"#302010\"\nforeground = \"#fff0ee\"\n");
    nextColors.close();

    QVERIFY(QDir(themePath).removeRecursively());
    QVERIFY(QDir(root).rename(QStringLiteral("next-theme"), QStringLiteral("theme")));
    QTRY_COMPARE_WITH_TIMEOUT(theme.background(), QColor(QStringLiteral("#302010")), 3000);
  }

  void themeUsesMachineLauncherOverride() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString themePath = state.filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themePath));
    QFile colors(themePath + QStringLiteral("/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#101820\"\nforeground = \"#f0f0f0\"\n");
    colors.close();
    QFile shell(themePath + QStringLiteral("/shell.toml"));
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"#223344\"\nbackground-alpha = 0.73\n");
    shell.close();

    const QString userRoot = config.filePath(QStringLiteral("omarchy"));
    QVERIFY(QDir().mkpath(userRoot));
    QFile userShell(userRoot + QStringLiteral("/shell.toml"));
    QVERIFY(userShell.open(QIODevice::WriteOnly | QIODevice::Text));
    userShell.write("[launcher]\nbackground = \"#445566\"\nbackground-alpha = 0.91\n");
    userShell.close();

    OmarchyTheme theme(state.path(), config.path());
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#445566")));
    QCOMPARE(theme.surfaceAlpha(), 0.91);

    QVERIFY(userShell.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    userShell.write("[launcher]\nbackground = \"#667788\"\nbackground-alpha = 0.64\n");
    userShell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#667788")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.64, 1500);

    QVERIFY(userShell.remove());
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#223344")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.73, 1500);
  }

  void albumsFollowMovesAndNeverGuessBetweenDuplicates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString album = QStringLiteral("Move test");
    AppSettings settings;
    settings.deleteAlbum(album);
    QVERIFY(settings.createAlbum(album));

    QImage image(12, 12, QImage::Format_RGB32);
    image.fill(Qt::magenta);
    const QString original = dir.filePath(QStringLiteral("original.png"));
    QVERIFY(image.save(original, "PNG"));
    QVERIFY(settings.addToAlbum(album, {original}));
    QCOMPARE(settings.albumPaths(album), QStringList {original});

    const QString renamed = dir.filePath(QStringLiteral("renamed.png"));
    QVERIFY(QFile::rename(original, renamed));
    CaptureRecord renamedRecord;
    renamedRecord.path = renamed;
    renamedRecord.bytes = QFileInfo(renamed).size();
    settings.reconcileAlbums({renamedRecord});
    QCOMPARE(settings.albumPaths(album), QStringList {renamed});
    AppSettings afterRename;
    QCOMPARE(afterRename.albumPaths(album), QStringList {renamed});

    // Copy then remove gives the file a new inode, like a move between
    // filesystems. The content identity still repairs the album entry.
    const QString copied = dir.filePath(QStringLiteral("copied.png"));
    QVERIFY(QFile::copy(renamed, copied));
    QVERIFY(QFile::remove(renamed));
    CaptureRecord copiedRecord;
    copiedRecord.path = copied;
    copiedRecord.bytes = QFileInfo(copied).size();
    settings.reconcileAlbums({copiedRecord});
    QCOMPARE(settings.albumPaths(album), QStringList {copied});

    const QString duplicateA = dir.filePath(QStringLiteral("duplicate-a.png"));
    const QString duplicateB = dir.filePath(QStringLiteral("duplicate-b.png"));
    QVERIFY(QFile::copy(copied, duplicateA));
    QVERIFY(QFile::copy(copied, duplicateB));
    QVERIFY(QFile::remove(copied));
    CaptureRecord a;
    a.path = duplicateA;
    a.bytes = QFileInfo(duplicateA).size();
    CaptureRecord b;
    b.path = duplicateB;
    b.bytes = QFileInfo(duplicateB).size();
    settings.reconcileAlbums({a, b});
    QVERIFY(settings.albumPaths(album).isEmpty());

    AppSettings restored;
    QVERIFY(restored.albumNames().contains(album));
    restored.reconcileAlbums({a, b});
    QVERIFY(restored.albumPaths(album).isEmpty());
    QCOMPARE(restored.albumItemCount(album), 1);
    QCOMPARE(restored.unavailableAlbumItemCount(album), 1);
    restored.removeUnavailableFromAlbum(album);
    QCOMPARE(restored.albumItemCount(album), 0);
    restored.deleteAlbum(album);
  }

  // --- Classification ---------------------------------------------------

  void albumInodeMatchNeedsTheSizeToAgree() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("screenshot-2026-09-02_12-00-00.png"));
    QImage(64, 64, QImage::Format_RGB32).save(path);

    AppSettings settings;
    QVERIFY(settings.createAlbum(QStringLiteral("Inode")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Inode"), {path}));
    QCOMPARE(settings.unavailableAlbumItemCount(QStringLiteral("Inode")), 0);

    // Pretend the inode number was handed to a different file: same path,
    // same device and inode on disk, but the entry remembers another size.
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                     QStringLiteral("omaroll"));
    QVariantMap albums = stored.value(QStringLiteral("library/albums")).toMap();
    QVariantList entries = albums.value(QStringLiteral("Inode")).toList();
    QCOMPARE(entries.size(), 1);
    QVariantMap entry = entries.first().toMap();
    entry.insert(QStringLiteral("bytes"), entry.value(QStringLiteral("bytes")).toLongLong() + 1);
    entry.insert(QStringLiteral("fingerprint"), QByteArray("not-this-file"));
    entries[0] = entry;
    albums.insert(QStringLiteral("Inode"), entries);
    stored.setValue(QStringLiteral("library/albums"), albums);
    stored.sync();

    AppSettings restored;
    QCOMPARE(restored.unavailableAlbumItemCount(QStringLiteral("Inode")), 1);

    CaptureRecord record;
    record.path = path;
    record.bytes = QFileInfo(path).size();
    record.modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    restored.reconcileAlbums({record});
    QCOMPARE(restored.unavailableAlbumItemCount(QStringLiteral("Inode")), 1);
    restored.deleteAlbum(QStringLiteral("Inode"));
  }

  void tagsAndSmartCollectionsPersistAndFollowRenames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString suffix = QString::number(QRandomGenerator::global()->generate());
    const QString tag = QStringLiteral("Tag ") + suffix;
    const QString collection = QStringLiteral("Smart ") + suffix;
    const QString original = dir.filePath(QStringLiteral("original.png"));
    const QString renamed = dir.filePath(QStringLiteral("renamed.png"));
    QImage image(24, 18, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(original, "PNG"));

    AppSettings settings;
    QVERIFY(settings.createTag(tag));
    QVERIFY(settings.addTag(tag, {original}));
    QCOMPARE(settings.tagsForPath(original), QStringList {tag});
    QCOMPARE(settings.tagItemCount(tag), 1);

    QVariantMap view;
    view.insert(QStringLiteral("kind"), CaptureRecord::Picture);
    view.insert(QStringLiteral("favorites"), true);
    view.insert(QStringLiteral("tag"), tag);
    QVERIFY(settings.saveSmartCollection(collection, view));

    AppSettings restored;
    QCOMPARE(restored.tagPaths(tag), QStringList {original});
    QCOMPARE(restored.smartCollection(collection), view);
    QVERIFY(QFile::rename(original, renamed));
    restored.relocatePath(original, renamed);
    QCOMPARE(restored.tagPaths(tag), QStringList {renamed});
    QCOMPARE(restored.tagsForPath(renamed), QStringList {tag});

    AppSettings finalRestore;
    QCOMPARE(finalRestore.tagPaths(tag), QStringList {renamed});
    QCOMPARE(finalRestore.smartCollection(collection), view);
    finalRestore.deleteTag(tag);
    finalRestore.deleteSmartCollection(collection);
  }

  void addingOverAnUnavailableAlbumEntryReplacesIt() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("out.png"));
    QImage(32, 32, QImage::Format_RGB32).save(path);

    AppSettings settings;
    QVERIFY(settings.createAlbum(QStringLiteral("Replace")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Replace"), {path}));
    QVERIFY(QFile::remove(path));
    settings.reconcileAlbums({});
    QCOMPARE(settings.unavailableAlbumItemCount(QStringLiteral("Replace")), 1);

    // A different file lands at the same name and is added again.
    QImage(48, 48, QImage::Format_RGB32).save(path);
    QVERIFY(settings.addToAlbum(QStringLiteral("Replace"), {path}));
    QCOMPARE(settings.albumItemCount(QStringLiteral("Replace")), 1);
    QCOMPARE(settings.unavailableAlbumItemCount(QStringLiteral("Replace")), 0);
    settings.deleteAlbum(QStringLiteral("Replace"));
  }

  void themeIgnoresUnrelatedWritesBesideItsState() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());
    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("mode = \"dark\"\nbackground = \"#101820\"\nforeground = "
                 "\"#f0f0f0\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QSignalSpy changed(&theme, &OmarchyTheme::themeChanged);

    // What omarchy-shell does on every copy: an atomic write beside the theme.
    QFile history(state.filePath(QStringLiteral("omarchy/clipboard-history.json.tmp")));
    QVERIFY(history.open(QIODevice::WriteOnly));
    history.write("[]");
    history.close();
    QVERIFY(QFile::rename(history.fileName(),
                          state.filePath(QStringLiteral("omarchy/clipboard-history.json"))));
    QTest::qWait(400);
    QCOMPARE(changed.size(), 0);

    // A real change still lands.
    QTest::qWait(20);
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    colors.write("mode = \"dark\"\nbackground = \"#202020\"\nforeground = "
                 "\"#f0f0f0\"\n");
    colors.close();
    QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 1500);
    QCOMPARE(theme.background(), QColor(QStringLiteral("#202020")));
  }

  void classifiesOmarchyScreenshot() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(QStringLiteral("screenshot-2026-08-31_23-26-39.png"),
                                           kind, captured));
    QCOMPARE(kind, CaptureRecord::Screenshot);
    QCOMPARE(captured.date(), QDate(2026, 8, 31));
    QCOMPARE(captured.time(), QTime(23, 26, 39));
  }

  void classifiesOmarchyRecording() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(
        QStringLiteral("screenrecording-2026-08-31_23-26-39.mp4"), kind, captured));
    QCOMPARE(kind, CaptureRecord::Recording);
    QCOMPARE(captured.time(), QTime(23, 26, 39));
  }

  void classifiesThirdPartyProducers_data() {
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<int>("expected");

    QTest::newRow("grim") << "20260831_23h26m39s_grim.png" << int(CaptureRecord::Screenshot);
    QTest::newRow("flameshot") << "flameshot_2026-08-31.png" << int(CaptureRecord::Screenshot);
    QTest::newRow("gnome style") << "Screenshot from 2026-08-31.png"
                                 << int(CaptureRecord::Screenshot);
    QTest::newRow("obs") << "2026-08-31 23-26-39.mkv" << int(CaptureRecord::Recording);
    QTest::newRow("gsr") << "Video_2026-08-31.mp4" << int(CaptureRecord::Recording);
  }

  void classifiesThirdPartyProducers() {
    QFETCH(QString, fileName);
    QFETCH(int, expected);

    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY2(CaptureScanner::classifyByName(fileName, kind, captured),
             qPrintable(QStringLiteral("no pattern matched %1").arg(fileName)));
    QCOMPARE(int(kind), expected);
  }

  void ignoresUnrelatedNames() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(!CaptureScanner::classifyByName(QStringLiteral("holiday-photo.jpg"), kind, captured));
    QVERIFY(!CaptureScanner::classifyByName(QStringLiteral("IMG_4821.jpeg"), kind, captured));
  }

  // --- Traversal --------------------------------------------------------

  void scanAppliesDepthAndSkipRules() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto write = [&](const QString& relative) {
      const QString full = dir.filePath(relative);
      QDir().mkpath(QFileInfo(full).absolutePath());
      QImage image(4, 4, QImage::Format_RGB32);
      image.fill(Qt::red);
      QVERIFY(image.save(full, "PNG"));
    };

    write(QStringLiteral("top.png"));
    write(QStringLiteral("nested/one.png"));
    write(QStringLiteral("nested/deeper/two.png"));
    write(QStringLiteral(".hidden/secret.png"));
    write(QStringLiteral("thumbnails/cached.png"));

    // A dotfile at the top level is skipped too.
    write(QStringLiteral(".dotfile.png"));

    // Depth 1: only the top-level file.
    auto shallow =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(shallow.size(), 1);
    QCOMPARE(shallow.first().fileName, QStringLiteral("top.png"));

    // Depth 2 reaches nested/ but not nested/deeper/, and never the skipped
    // directories at any depth.
    auto deeper =
        CaptureScanner::scan({{dir.path(), 2, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(deeper.size(), 2);

    QStringList names;
    for (const auto& record : deeper) {
      names << record.fileName;
    }
    std::sort(names.begin(), names.end());
    QCOMPARE(names, QStringList({QStringLiteral("one.png"), QStringLiteral("top.png")}));
  }

  void scanDeduplicatesAcrossOverlappingRoots() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(dir.filePath(QStringLiteral("shot.png")), "PNG"));

    // The same directory listed twice, which is exactly what happens when
    // OMARCHY_SCREENSHOT_DIR and XDG_PICTURES_DIR resolve to the same place.
    const auto records = CaptureScanner::scan({
        {dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video},
        {dir.path(), 4, CaptureRecord::Picture, CaptureRecord::Video},
    });
    QCOMPARE(records.size(), 1);
  }

  void scanRecursesPastAShallowerOverlappingRoot() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto write = [&](const QString& relative) {
      const QString full = dir.filePath(relative);
      QDir().mkpath(QFileInfo(full).absolutePath());
      QImage image(4, 4, QImage::Format_RGB32);
      image.fill(Qt::red);
      QVERIFY(image.save(full, "PNG"));
    };
    write(QStringLiteral("top.png"));
    write(QStringLiteral("albums/holiday.png"));

    // The default layout: the depth-1 screenshot root and the recursive
    // Pictures root are the same directory. The shallow one must not stop the
    // deep one from seeing the subfolder.
    const auto records = CaptureScanner::scan({
        {dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video},
        {dir.path(), 3, CaptureRecord::Picture, CaptureRecord::Video},
    });
    QCOMPARE(records.size(), 2);
  }

  void downloadsKeepTheirMedium() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(dir.filePath(QStringLiteral("meme.png")), "PNG"));
    // The scanner never decodes, so a stand-in is enough to classify.
    QFile clip(dir.filePath(QStringLiteral("clip.mp4")));
    QVERIFY(clip.open(QIODevice::WriteOnly));
    clip.write("not really a video");
    clip.close();
    QFile phonePhoto(dir.filePath(QStringLiteral("phone.heic")));
    QVERIFY(phonePhoto.open(QIODevice::WriteOnly));
    phonePhoto.write("not really a photo");
    phonePhoto.close();
    QFile phoneClip(dir.filePath(QStringLiteral("phone.3gp")));
    QVERIFY(phoneClip.open(QIODevice::WriteOnly));
    phoneClip.write("not really a video");
    phoneClip.close();
    QFile document(dir.filePath(QStringLiteral("manual.pdf")));
    QVERIFY(document.open(QIODevice::WriteOnly));
    document.write("not really a PDF");
    document.close();

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Download, CaptureRecord::Download}});
    QCOMPARE(records.size(), 5);
    for (const auto& record : records) {
      QCOMPARE(record.kind, CaptureRecord::Download);
      // A downloaded clip still plays, scrubs and trims like a recording.
      QCOMPARE(record.isVideo(), CaptureScanner::isVideo(QFileInfo(record.path).suffix()));
      QCOMPARE(record.isDocument(),
               CaptureScanner::isDocument(QFileInfo(record.path).suffix()));
    }
  }

  void directOpenRecognizesEveryViewerMediumAndCommonImageFormat() {
    const QStringList images = {QStringLiteral("png"),  QStringLiteral("JPG"),
                                QStringLiteral("svg"),  QStringLiteral("svgz"),
                                QStringLiteral("ico"),  QStringLiteral("jxl"),
                                QStringLiteral("jp2"),  QStringLiteral("j2k"),
                                QStringLiteral("qoi"),  QStringLiteral("psd"),
                                QStringLiteral("dds"),  QStringLiteral("exr"),
                                QStringLiteral("tga")};
    for (const QString& suffix : images) {
      QVERIFY2(CaptureScanner::isImage(suffix), qPrintable(suffix));
      QVERIFY2(CaptureScanner::isSupported(suffix), qPrintable(suffix));
    }
    QVERIFY(CaptureScanner::isSupported(QStringLiteral("mkv")));
    QVERIFY(CaptureScanner::isSupported(QStringLiteral("PDF")));
    QVERIFY(!CaptureScanner::isSupported(QStringLiteral("txt")));

    const QList<QByteArray> decoders = QImageReader::supportedImageFormats();
    for (const QByteArray format : {QByteArray("svg"), QByteArray("ico"), QByteArray("jxl"),
                                    QByteArray("jp2"), QByteArray("qoi"), QByteArray("psd"),
                                    QByteArray("dds"), QByteArray("exr"), QByteArray("tga")}) {
      QVERIFY2(decoders.contains(format),
               qPrintable(QStringLiteral("missing image decoder for %1; found: %2")
                              .arg(QString::fromLatin1(format),
                                   QString::fromLatin1(decoders.join(',')))));
    }
  }

  void executableAcceptsDirectPdfOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = dir.filePath(QStringLiteral("document # 1.pdf"));
    {
      QPdfWriter writer(pdf);
      QPainter painter(&writer);
      QVERIFY(painter.isActive());
      painter.drawText(QPoint(100, 100), QStringLiteral("Direct PDF open"));
    }
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    // Keep settings, discovery, the instance socket and the window away from
    // any real user session, including when this test runs outside CTest.
    for (const QString& name : {QStringLiteral("HOME"), QStringLiteral("XDG_CONFIG_HOME"),
                                QStringLiteral("XDG_DATA_HOME"), QStringLiteral("XDG_CACHE_HOME"),
                                QStringLiteral("XDG_PICTURES_DIR"), QStringLiteral("XDG_VIDEOS_DIR"),
                                QStringLiteral("XDG_DOWNLOAD_DIR"), QStringLiteral("OMARCHY_SCREENSHOT_DIR"),
                                QStringLiteral("OMARCHY_SCREENRECORD_DIR")})
      environment.insert(name, dir.path());
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QFileInfo(dir.path()).fileName());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QString());
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/omaroll"), {pdf});
    QVERIFY(process.waitForStarted());
    const auto stop = qScopeGuard([&] {
      process.terminate();
      if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished();
      }
    });
    QVERIFY2(!process.waitForFinished(1500), process.readAllStandardError().constData());
    QVERIFY(!process.readAllStandardError().contains("unsupported media file"));
  }

  void videoNameOnImageFileStaysAnImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::green);
    // A recording name on a PNG: the medium has to win, or the grid offers
    // "Trim" on something omacut cannot open.
    QVERIFY(
        image.save(dir.filePath(QStringLiteral("screenrecording-2026-08-31_10-00-00.png")), "PNG"));

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Screenshot);
  }

  void pdfsAreScannedRenderedAndInspected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("reference.pdf"));
    {
      QPdfWriter writer(path);
      writer.setResolution(96);
      QPainter painter(&writer);
      QVERIFY(painter.isActive());
      painter.drawText(QPoint(120, 160), QStringLiteral("Omaroll PDF page one"));
      QVERIFY(writer.newPage());
      painter.drawText(QPoint(120, 160), QStringLiteral("Omaroll PDF page two"));
      painter.end();
    }

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Document);
    QVERIFY(records.first().isDocument());
    QVERIFY(!records.first().isVideo());

    if (!PdfSupport::available()) {
      QSKIP("Poppler is not installed");
    }
    const QImage preview = PdfSupport::renderPage(path, 2, QSize(420, 420));
    QVERIFY(!preview.isNull());
    QVERIFY(preview.width() > 0);
    QVERIFY(preview.height() > 0);

    PdfInspector inspector;
    QVERIFY(inspector.available());
    inspector.inspect(path);
    QTRY_VERIFY_WITH_TIMEOUT(!inspector.loading(), 5000);
    QCOMPARE(inspector.pageCount(), 2);
    QVERIFY(inspector.error().isEmpty());
  }

  // --- Filtering and sorting -------------------------------------------

  void namesSortNaturally_data() {
    QTest::addColumn<QString>("localeName");
    QTest::newRow("minimal-environment") << QStringLiteral("C");
    QTest::newRow("english") << QStringLiteral("en_US");
    QTest::newRow("german") << QStringLiteral("de_DE");
  }

  void namesSortNaturally() {
    QFETCH(QString, localeName);
    const QLocale previousLocale;
    const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(previousLocale); });
    QLocale::setDefault(QLocale(localeName));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const char* variable : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                                 "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR"}) {
      QVERIFY(qputenv(variable, dir.path().toUtf8()));
    }
    const QStringList names = {QStringLiteral("photo10.png"), QStringLiteral("photo2.png"),
                              QStringLiteral("Photo02.png"), QStringLiteral("photo1.png"),
                              QStringLiteral("旅行10.png"), QStringLiteral("旅行2.png")};
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::gray);
    for (const QString& name : names) {
      QVERIFY(image.save(dir.filePath(name)));
    }
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("other"))));
    QVERIFY(image.save(dir.filePath(QStringLiteral("other/photo2.png"))));
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    proxy.setSortMode(CaptureFilterModel::NameAscending);
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 7, 5000);
    const auto row = [&](const QString& name) { return proxy.rowOf(dir.filePath(name)); };
    QVERIFY(row(QStringLiteral("photo1.png")) < row(QStringLiteral("photo2.png")));
    QVERIFY(row(QStringLiteral("photo2.png")) < row(QStringLiteral("photo10.png")));
    QVERIFY(row(QStringLiteral("Photo02.png")) < row(QStringLiteral("photo10.png")));
    QVERIFY(row(QStringLiteral("旅行2.png")) < row(QStringLiteral("旅行10.png")));
    QVERIFY(row(QStringLiteral("other/photo2.png")) < row(QStringLiteral("photo2.png")));
    QStringList firstOrder;
    for (int i = 0; i < proxy.count(); ++i) {
      firstOrder.append(proxy.pathAt(i));
    }
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    proxy.setSortMode(CaptureFilterModel::NameAscending);
    for (int i = 0; i < proxy.count(); ++i) {
      QCOMPARE(proxy.pathAt(i), firstOrder.at(i));
    }
  }

  void filterModelSortsAndFilters() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", dir.path().toUtf8()));

    // mtime is set explicitly. A file with a producer name takes its timestamp
    // from the name, but an unnamed one falls back to mtime, and a file written
    // during the test would otherwise always be the newest thing in the
    // library.
    const auto write = [&](const QString& name, int edge, const QDateTime& stamp) {
      QImage image(edge, edge, QImage::Format_RGB32);
      image.fill(Qt::gray);
      const QString path = dir.filePath(name);
      QDir().mkpath(QFileInfo(path).absolutePath());
      QVERIFY(image.save(path, "PNG"));
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadWrite));
      QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
      file.close();
    };

    write(QStringLiteral("screenshot-2026-08-30_10-00-00.png"), 8,
          QDateTime(QDate(2026, 8, 30), QTime(10, 0)));
    write(QStringLiteral("screenshot-2026-08-31_10-00-00.png"), 64,
          QDateTime(QDate(2026, 8, 31), QTime(10, 0)));
    write(QStringLiteral("beach.png"), 16, QDateTime(QDate(2026, 8, 20), QTime(9, 0)));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);

    QSignalSpy spy(&model, &CaptureModel::countChanged);
    QVERIFY(spy.wait(5000));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 3);

    // Newest first by default.
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("screenshot-2026-08-31_10-00-00.png"));

    proxy.setSortMode(CaptureFilterModel::OldestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    // Size sorting reads the file, not the name.
    proxy.setSortMode(CaptureFilterModel::LargestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("screenshot-2026-08-31_10-00-00.png"));

    proxy.setSortMode(CaptureFilterModel::NameAscending);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    // Only the two named screenshots are Screenshots; beach.png falls back.
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    proxy.setKindFilter(CaptureRecord::Screenshot);
    QCOMPARE(proxy.count(), 2);

    proxy.setKindFilter(CaptureFilterModel::kAllKinds);
    proxy.setSearchText(QStringLiteral("beach"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    // The header's total must update even when a new source row is excluded
    // by the active filter and the proxy's visible count stays unchanged.
    QSignalSpy sourceCountChanged(&proxy, &CaptureFilterModel::countChanged);
    QSignalSpy rescanned(&model, &CaptureModel::countChanged);
    write(QStringLiteral("mountain.png"), 12, QDateTime(QDate(2026, 8, 19), QTime(9, 0)));
    model.refresh();
    QVERIFY(rescanned.wait(5000));
    QCOMPARE(proxy.sourceCount(), 4);
    QCOMPARE(proxy.count(), 1);
    QVERIFY(!sourceCountChanged.isEmpty());
    QTRY_COMPARE(proxy.folderItemCount(dir.path()), 4);

    proxy.setSearchText({});
    QCOMPARE(proxy.count(), 4);

    const QString beachPath = dir.filePath(QStringLiteral("beach.png"));
    proxy.setOcrText(beachPath, QStringLiteral("Invoice overdue total"));
    proxy.setSearchText(QStringLiteral("beach overdue"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), beachPath);
    proxy.setSearchText({});

    rescanned.clear();
    write(QStringLiteral("album/forest.png"), 18, QDateTime(QDate(2026, 8, 18), QTime(9, 0)));
    model.refresh();
    QVERIFY(rescanned.wait(5000));
    QCOMPARE(proxy.count(), 5);
    QTRY_COMPARE(proxy.folders(), QStringList({dir.path(), dir.filePath(QStringLiteral("album"))}));
    QCOMPARE(proxy.folderItemCount(dir.path()), 5);
    QCOMPARE(proxy.folderItemCount(dir.filePath(QStringLiteral("album"))), 1);
    QCOMPARE(proxy.folderItemCount(dir.filePath(QStringLiteral("missing"))), 0);
    proxy.setFolderFilter(dir.path());
    QCOMPARE(proxy.count(), 5);
    proxy.setFolderFilter(dir.filePath(QStringLiteral("album")));
    QCOMPARE(proxy.count(), 1);
    proxy.setFolderFilter(dir.filePath(QStringLiteral("another-folder")));
    QCOMPARE(proxy.count(), 0);
    proxy.setFolderFilter({});
    QCOMPARE(proxy.count(), 5);
    const QString beach = dir.filePath(QStringLiteral("beach.png"));
    proxy.setAlbumFilter(QStringLiteral("Weekend"),
                         {beach, dir.filePath(QStringLiteral("album/forest.png"))});
    QCOMPARE(proxy.count(), 2);
    QCOMPARE(proxy.adjacentPath(beach, 1), dir.filePath(QStringLiteral("album/forest.png")));
    QCOMPARE(proxy.adjacentPath(beach, -1), dir.filePath(QStringLiteral("album/forest.png")));
    proxy.setAlbumFilter({}, {});
    QCOMPARE(proxy.count(), 5);

    proxy.setDateRange(QStringLiteral("2026-08-30"), QStringLiteral("2026-08-31"));
    QCOMPARE(proxy.count(), 2);
    QCOMPARE(proxy.dateFrom(), QStringLiteral("2026-08-30"));
    QCOMPARE(proxy.dateTo(), QStringLiteral("2026-08-31"));
    proxy.clearDateRange();
    QCOMPARE(proxy.count(), 5);
    proxy.setModifiedAfter(QStringLiteral("2026-08-20T09:00:00"));
    QCOMPARE(proxy.count(), 2);
    QVERIFY(!proxy.modifiedAfter().isEmpty());
    proxy.clearDateRange();
    QCOMPARE(proxy.count(), 5);
    QTRY_VERIFY_WITH_TIMEOUT(!proxy.dateBuckets().isEmpty(), 1000);
    QCOMPARE(proxy.dateDays(QStringLiteral("2026-08")).size(), 5);

    // Camera and lens filters come from the metadata index. The choice lists
    // count the library, not the current view, and the filter survives a
    // saved view round trip.
    QVERIFY(proxy.cameras().isEmpty());
    const CaptureRecord& beachRecord = model.recordAt(model.rowOf(beach));
    model.applyMetadata({{beach, beachRecord.modified, beachRecord.bytes, {}, beachRecord.device,
                          beachRecord.inode, QStringLiteral("Sony ILCE-7M3"),
                          QStringLiteral("FE 24-70mm F2.8 GM")}});
    QTRY_COMPARE_WITH_TIMEOUT(proxy.cameras().size(), 1, 1000);
    QCOMPARE(proxy.cameras().first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Sony ILCE-7M3"));
    QCOMPARE(proxy.cameras().first().toMap().value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(proxy.lenses().size(), 1);
    proxy.setCameraFilter(QStringLiteral("Sony ILCE-7M3"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), beach);
    proxy.setLensFilter(QStringLiteral("Some other lens"));
    QCOMPARE(proxy.count(), 0);
    proxy.setLensFilter(QStringLiteral("FE 24-70mm F2.8 GM"));
    QCOMPARE(proxy.count(), 1);
    const QVariantMap cameraView = proxy.currentView();
    QCOMPARE(cameraView.value(QStringLiteral("camera")).toString(), QStringLiteral("Sony ILCE-7M3"));
    proxy.setCameraFilter({});
    proxy.setLensFilter({});
    QCOMPARE(proxy.count(), 5);
    proxy.applyView(QStringLiteral("Sony shots"), cameraView);
    QCOMPARE(proxy.cameraFilter(), QStringLiteral("Sony ILCE-7M3"));
    QCOMPARE(proxy.count(), 1);
    proxy.clearSmartCollection();
    proxy.setCameraFilter({});
    proxy.setLensFilter({});
    QCOMPARE(proxy.count(), 5);

    const QString forest = dir.filePath(QStringLiteral("album/forest.png"));
    proxy.setTagFilter(QStringLiteral("Review"), {beach, forest});
    QCOMPARE(proxy.count(), 2);
    const QVariantMap savedView = proxy.currentView();
    proxy.applyView(QStringLiteral("Review pictures"), savedView, {beach, forest});
    QCOMPARE(proxy.smartCollectionFilter(), QStringLiteral("Review pictures"));
    QCOMPARE(proxy.count(), 2);
    proxy.setSearchText(QStringLiteral("beach"));
    QVERIFY(proxy.smartCollectionFilter().isEmpty());
    QCOMPARE(proxy.count(), 1);
    proxy.setSearchText({});
    proxy.setTagFilter({}, {});
    QCOMPARE(proxy.count(), 5);

    // Navigation must visit every visible row once before wrapping, in both
    // directions. A one-step assertion misses cycles that accidentally cover
    // only part of a mixed library.
    const QString first = proxy.pathAt(0);
    for (const int direction : {1, -1}) {
      QSet<QString> visited;
      QString path = first;
      for (int step = 0; step < proxy.count(); ++step) {
        QVERIFY(!path.isEmpty());
        QVERIFY(!visited.contains(path));
        visited.insert(path);
        path = proxy.adjacentPath(path, direction);
      }
      QCOMPARE(visited.size(), proxy.count());
      QCOMPARE(path, first);
    }

    const QString next = proxy.adjacentPathInFolder(beach, 1);
    const QString previous = proxy.adjacentPathInFolder(beach, -1);
    QVERIFY(!next.isEmpty());
    QVERIFY(!previous.isEmpty());
    QCOMPARE(QFileInfo(next).absolutePath(), dir.path());
    QCOMPARE(QFileInfo(previous).absolutePath(), dir.path());
    for (const int direction : {1, -1}) {
      QSet<QString> visited;
      QString path = beach;
      do {
        QVERIFY(!visited.contains(path));
        visited.insert(path);
        path = proxy.adjacentPathInFolder(path, direction);
      } while (path != beach);
      QCOMPARE(visited.size(), 4);
    }
    QCOMPARE(proxy.adjacentPathInFolder(QStringLiteral("/missing.png"), 1), QString());
  }

  void localOcrSearchRunsOnceThenUsesItsPrivateCache() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_TEST_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_TEST_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("ocr-runs.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf 'Invoice total forty two\\n'\n"
                    "printf 'run\\n' >> \"$OMAROLL_OCR_TEST_LOG\"\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_OCR_TEST_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const QString imagePath = dir.filePath(QStringLiteral("capture-001.png"));
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::white);
    QVERIFY(image.save(imagePath, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 1);

    const auto runSearch = [&] {
      CaptureFilterModel proxy;
      proxy.setSourceModel(&model);
      OcrIndex index(&model);
      QVERIFY(index.available());
      connect(&index, &OcrIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);
      proxy.setSearchText(QStringLiteral("invoice forty"));
      index.setSearchText(proxy.searchText());
      QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 1, 3000);
      QCOMPARE(proxy.pathAt(0), imagePath);
      QCOMPARE(proxy.ocrSnippetAt(0), QStringLiteral("Invoice total forty two"));
      QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
      QCOMPARE(index.completed(), 1);
      QCOMPARE(index.total(), 1);
      proxy.setSearchText(QStringLiteral("capture"));
      QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 1, 1000);
      QVERIFY(proxy.ocrSnippetAt(0).isEmpty());
    };

    runSearch();
    runSearch();

    image = QImage(20, 14, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(imagePath, "PNG"));
    QFile changed(imagePath);
    QVERIFY(changed.open(QIODevice::ReadWrite));
    QVERIFY(changed.setFileTime(QDateTime::currentDateTime().addSecs(2),
                                QFileDevice::FileModificationTime));
    changed.close();
    scanned.clear();
    model.refresh();
    QVERIFY(scanned.wait(5000));
    runSearch();

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll(), QByteArray("run\nrun\n"));
    const QDir cache(OcrIndex::cacheDirectory());
    QCOMPARE(cache.entryList({QStringLiteral("*.ocr")}, QDir::Files).size(), 1);
    const QFileInfo entry(
        cache.filePath(cache.entryList({QStringLiteral("*.ocr")}, QDir::Files).first()));
    QVERIFY(!(entry.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    OcrIndex index(&model);
    connect(&index, &OcrIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);
    proxy.setSearchText(QStringLiteral("invoice forty"));
    index.setSearchText(proxy.searchText());
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
    QCOMPARE(index.completed(), 1);
    QCOMPARE(index.total(), 1);
    QCOMPARE(index.clearCache(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 0, 1000);
    QCOMPARE(cache.entryList({QStringLiteral("*.ocr")}, QDir::Files).size(), 0);
  }

  void sparseOcrRetryImprovesWeakRecognitionAndCachesTheResult() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_SPARSE_LOG");
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_SPARSE_LOG", previousLog);
      putBack("XDG_CACHE_HOME", previousCache);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("sparse-runs.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf '%s\\n' \"$*\" >> \"$OMAROLL_OCR_SPARSE_LOG\"\n"
                    "sparse_mode=false\n"
                    "for argument do\n"
                    "  [ \"$argument\" = 11 ] && sparse_mode=true\n"
                    "done\n"
                    "case \"$1\" in\n"
                    "  *sparse*)\n"
                    "    if $sparse_mode; then printf 'Isolated label found\\n'; "
                    "else printf 'x\\n'; fi ;;\n"
                    "  *background*) printf 'x\\n' ;;\n"
                    "  *) printf 'Enough text from the normal pass\\n' ;;\n"
                    "esac\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousPath));
    QVERIFY(qputenv("OMAROLL_OCR_SPARSE_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const auto makeImage = [&dir](const QString& name) {
      const QString path = dir.filePath(name);
      QImage image(16, 12, QImage::Format_RGB32);
      image.fill(Qt::white);
      return image.save(path, "PNG") ? path : QString();
    };
    const QString sparse = makeImage(QStringLiteral("sparse.png"));
    const QString normal = makeImage(QStringLiteral("normal.png"));
    const QString background = makeImage(QStringLiteral("background.png"));
    QVERIFY(!sparse.isEmpty() && !normal.isEmpty() && !background.isEmpty());

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 3);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    OcrIndex index(&model);
    QVERIFY(index.available());
    connect(&index, &OcrIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);

    index.recognize(sparse);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewText(), QStringLiteral("Isolated label found"));
    proxy.setSearchText(QStringLiteral("isolated label"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), sparse);
    proxy.setSearchText({});

    index.cancelReview();
    index.recognize(sparse);
    QVERIFY(!index.reviewing());
    QCOMPARE(index.reviewText(), QStringLiteral("Isolated label found"));

    index.recognize(normal);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewText(), QStringLiteral("Enough text from the normal pass"));

    // Background search remains single-pass even when its result is weak.
    index.cancelReview();
    proxy.setSearchText(QStringLiteral("not present"));
    index.setSearchText(proxy.searchText());
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
    QCOMPARE(proxy.count(), 0);

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QStringList runs =
        QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(runs.size(), 4);
    QVERIFY(!runs.at(0).contains(QStringLiteral("--psm 11")));
    QVERIFY(runs.at(1).contains(QStringLiteral("--psm 11")));
    QVERIFY(!runs.at(2).contains(QStringLiteral("--psm 11")));
    QVERIFY(!runs.at(3).contains(QStringLiteral("--psm 11")));
  }

  void clearingOcrSearchStopsTheQueuedLibraryWork() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_CANCEL_LOG");
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_CANCEL_LOG", previousLog);
      putBack("XDG_CACHE_HOME", previousCache);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("ocr-cancel.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf 'run\\n' >> \"$OMAROLL_OCR_CANCEL_LOG\"\n"
                    "/usr/bin/sleep 0.15\n"
                    "printf 'Nothing relevant here\\n'\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_OCR_CANCEL_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int index = 0; index < 16; ++index) {
      QVERIFY(image.save(dir.filePath(QStringLiteral("photo-%1.png").arg(index)), "PNG"));
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 16);

    OcrIndex index(&model);
    index.setSearchText(QStringLiteral("needle"));
    QTRY_VERIFY(index.indexing());
    QCOMPARE(index.total(), 16);
    index.setSearchText({});
    QVERIFY(!index.indexing());
    QCOMPARE(index.total(), 0);

    QTest::qWait(300);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll().count("run\n"), 1);
  }

  void ocrReviewPreservesLayoutHandlesEmptyAndRejectsStaleResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_REVIEW_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_REVIEW_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("review-runs.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf 'run\\n' >> \"$OMAROLL_OCR_REVIEW_LOG\"\n"
                    "case \"$1\" in\n"
                    "  *slow*) /usr/bin/sleep 0.2; printf 'STALE\\n' ;;\n"
                    "  *empty*) ;;\n"
                    "  *fail*) exit 2 ;;\n"
                    "  *) printf 'Alpha  beta\\nSecond line\\n' ;;\n"
                    "esac\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousPath));
    QVERIFY(qputenv("OMAROLL_OCR_REVIEW_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const auto makeImage = [&dir](const QString& name) {
      const QString path = dir.filePath(name);
      QImage image(16, 12, QImage::Format_RGB32);
      image.fill(Qt::white);
      return image.save(path, "PNG") ? path : QString();
    };
    const QString layout = makeImage(QStringLiteral("layout.png"));
    const QString empty = makeImage(QStringLiteral("empty.png"));
    const QString failure = makeImage(QStringLiteral("fail.png"));
    const QString slow = makeImage(QStringLiteral("slow.png"));
    const QString fresh = makeImage(QStringLiteral("fresh.png"));
    QVERIFY(!layout.isEmpty() && !empty.isEmpty() && !failure.isEmpty() && !slow.isEmpty() &&
            !fresh.isEmpty());

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 5);

    OcrIndex index(&model);
    QVERIFY(index.available());
    index.recognize(layout);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewPath(), layout);
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));
    QVERIFY(index.reviewError().isEmpty());

    // The second review comes from the shared private cache without another
    // process, and the layout text remains distinct from normalized search.
    index.cancelReview();
    index.recognize(layout);
    QVERIFY(!index.reviewing());
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));

    index.recognize(empty);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QVERIFY(index.reviewText().isEmpty());
    QVERIFY(index.reviewError().isEmpty());

    index.recognize(failure);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QVERIFY(index.reviewText().isEmpty());
    QCOMPARE(index.reviewError(), QStringLiteral("Could not extract text"));

    // A result from the previous image must not overwrite a newer request.
    index.recognize(slow);
    index.recognize(fresh);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewPath(), fresh);
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));
    QVERIFY(index.reviewError().isEmpty());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const qsizetype runs = log.readAll().count("run\n");
    // The replaced slow review may be killed before its helper reaches the
    // first instruction, or immediately after it. Either way only the fresh
    // result is published.
    QVERIFY(runs == 5 || runs == 6);
    log.close();

    // An explicit review jumps ahead of an in-flight background search. The
    // interrupted search item is then retried, rather than being recorded as
    // a failure or silently dropped.
    const auto runCount = [&] {
      QFile runsFile(logPath);
      return runsFile.open(QIODevice::ReadOnly) ? runsFile.readAll().count("run\n") : qsizetype(-1);
    };
    index.cancelReview();
    index.setSearchText(QStringLiteral("needle"));
    QTRY_VERIFY(index.indexing());
    QTRY_VERIFY(runCount() > runs);
    const qsizetype beforePriorityReview = runCount();
    index.recognize(layout, true);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 1000);
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
    QCOMPARE(runCount(), beforePriorityReview + 2);
    index.setSearchText({});
  }

  void qrDetectionCachesOnlyPresenceAndRejectsStaleResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_QR_TEST_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_QR_TEST_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("qr-runs.log"));
    QFile zbar(dir.filePath(QStringLiteral("zbarimg")));
    QVERIFY(zbar.open(QIODevice::WriteOnly));
    zbar.write("#!/bin/sh\n"
               "printf 'run\\n' >> \"$OMAROLL_QR_TEST_LOG\"\n"
               "for last do :; done\n"
               "case \"$last\" in\n"
               "  *slow*) /usr/bin/sleep 0.2; printf 'old-secret\\n' ;;\n"
               "  *positive*) printf 'otpauth://private-value\\n' ;;\n"
               "  *) exit 4 ;;\n"
               "esac\n");
    zbar.close();
    QVERIFY(zbar.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousPath));
    QVERIFY(qputenv("OMAROLL_QR_TEST_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const auto makeImage = [&dir](const QString& name) {
      const QString path = dir.filePath(name);
      QImage image(16, 12, QImage::Format_RGB32);
      image.fill(Qt::white);
      return image.save(path, "PNG") ? path : QString();
    };
    const QString positive = makeImage(QStringLiteral("positive.png"));
    const QString negative = makeImage(QStringLiteral("negative.png"));
    const QString slow = makeImage(QStringLiteral("slow.png"));
    const QString fresh = makeImage(QStringLiteral("fresh-positive.png"));
    QVERIFY(!positive.isEmpty() && !negative.isEmpty() && !slow.isEmpty() && !fresh.isEmpty());

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    QrDetector detector(&model);
    QVERIFY(detector.available());
    detector.inspect(positive);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QCOMPARE(detector.path(), positive);
    QVERIFY(detector.detected());

    // A second inspection uses the in-memory identity cache. The decoded
    // value is intentionally not exposed by the detector at all.
    detector.inspect(positive);
    QVERIFY(!detector.checking());
    QVERIFY(detector.detected());
    {
      QFile log(logPath);
      QVERIFY(log.open(QIODevice::ReadOnly));
      QCOMPARE(log.readAll().count("run\n"), 1);
    }

    detector.inspect(negative);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QVERIFY(!detector.detected());

    detector.inspect(slow);
    detector.inspect(fresh);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QCOMPARE(detector.path(), fresh);
    QVERIFY(detector.detected());

    // Cancelling the slow check must not cache a false negative for it.
    detector.inspect(slow);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QCOMPARE(detector.path(), slow);
    QVERIFY(detector.detected());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const qsizetype runs = log.readAll().count("run\n");
    QVERIFY(runs == 4 || runs == 5);
    detector.clear();
    QVERIFY(detector.path().isEmpty());
    QVERIFY(!detector.checking());
    QVERIFY(!detector.detected());
  }

  void equalSortKeysHaveStableOrder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", dir.path().toUtf8()));

    const QDateTime stamp(QDate(2026, 8, 20), QTime(9, 0));
    const auto write = [&](const QString& relativePath) {
      const QString path = dir.filePath(relativePath);
      QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
      QImage image(16, 16, QImage::Format_RGB32);
      image.fill(Qt::gray);
      QVERIFY(image.save(path, "PNG"));
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadWrite));
      QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
    };

    // Reverse path order on disk so deterministic results cannot come from
    // insertion order. All three otherwise have the same sort keys.
    write(QStringLiteral("z/duplicate.png"));
    write(QStringLiteral("m/duplicate.png"));
    write(QStringLiteral("a/duplicate.png"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 3);
    const QStringList expected = {
        dir.filePath(QStringLiteral("a/duplicate.png")),
        dir.filePath(QStringLiteral("m/duplicate.png")),
        dir.filePath(QStringLiteral("z/duplicate.png")),
    };
    for (const int mode : {CaptureFilterModel::NewestFirst, CaptureFilterModel::OldestFirst,
                           CaptureFilterModel::LargestFirst, CaptureFilterModel::SmallestFirst,
                           CaptureFilterModel::NameAscending}) {
      proxy.setSortMode(mode);
      QStringList actual;
      for (int row = 0; row < proxy.count(); ++row) {
        actual.append(proxy.pathAt(row));
      }
      QCOMPARE(actual, expected);
    }
  }

  void exactDuplicateReviewHashesOnlyPlausibleMatchesAndUpdates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    const QByteArray same(2 * 1024 * 1024, 'a');
    const QByteArray sameSizeDifferent(2 * 1024 * 1024, 'b');
    const auto write = [&](const QString& name, const QByteArray& content) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QCOMPARE(file.write(content), content.size());
    };
    write(QStringLiteral("first.png"), same);
    write(QStringLiteral("copy.png"), same);
    write(QStringLiteral("same-size-but-different.png"), sameSizeDifferent);
    write(QStringLiteral("unique.png"), QByteArray("one of a kind"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    DuplicateIndex duplicates(&model);
    connect(&duplicates, &DuplicateIndex::groupsChanged, &proxy,
            [&] { proxy.setDuplicateGroups(duplicates.groups()); });
    proxy.setDuplicatesOnly(true);
    duplicates.setActive(true);

    QTRY_COMPARE_WITH_TIMEOUT(duplicates.groupCount(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!duplicates.scanning(), 5000);
    QCOMPARE(duplicates.total(), 3);
    QCOMPARE(duplicates.completed(), 3);
    QCOMPARE(duplicates.duplicateCount(), 2);
    QCOMPARE(proxy.count(), 2);
    const QString firstPath = dir.filePath(QStringLiteral("first.png"));
    const QString copyPath = dir.filePath(QStringLiteral("copy.png"));
    QCOMPARE(duplicates.groupPaths(firstPath), QStringList({copyPath, firstPath}));
    QCOMPARE(duplicates.otherCopies(firstPath), QStringList {copyPath});
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Exact match 1 of 1"));
    QCOMPARE(proxy.gridLabelAt(1), QStringLiteral("Exact match 1 of 1"));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("first.png"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("copy.png"))));

    // Reopening an unchanged review reuses the in-memory hashes immediately.
    duplicates.setActive(false);
    duplicates.setActive(true);
    QVERIFY(!duplicates.scanning());
    QCOMPARE(proxy.count(), 2);

    // Rewriting one copy invalidates its identity. The live library change
    // triggers a new comparison and both former matches leave the view.
    write(QStringLiteral("copy.png"), QByteArray("no longer the same"));
    model.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(duplicates.groupCount(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 0, 5000);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("first.png"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("copy.png"))));
  }

  void visuallySimilarPicturesAreSuggestedWithoutGroupingDifferentColour() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const char* name : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                             "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage source(320, 240, QImage::Format_RGB32);
    QPainter painter(&source);
    QLinearGradient gradient(0, 0, source.width(), source.height());
    gradient.setColorAt(0, QColor(24, 70, 130));
    gradient.setColorAt(1, QColor(230, 190, 80));
    painter.fillRect(source.rect(), gradient);
    painter.setPen(QPen(Qt::white, 12));
    painter.drawEllipse(QRect(70, 45, 180, 150));
    painter.drawLine(20, 210, 300, 30);
    painter.end();
    const QString original = dir.filePath(QStringLiteral("original.png"));
    const QString recompressed = dir.filePath(QStringLiteral("recompressed.jpg"));
    const QString different = dir.filePath(QStringLiteral("different.png"));
    QVERIFY(source.save(original, "PNG"));
    QVERIFY(source.scaled(640, 480, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(recompressed, "JPG", 58));
    QImage other(320, 240, QImage::Format_RGB32);
    other.fill(QColor(220, 20, 30));
    QVERIFY(other.save(different, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 3);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    SimilarityIndex similarities(&model);
    connect(&similarities, &SimilarityIndex::groupsChanged, &proxy,
            [&] { proxy.setSimilarGroups(similarities.groups()); });
    proxy.setSimilarOnly(true);
    similarities.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(similarities.groupCount(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!similarities.scanning(), 5000);
    QCOMPARE(similarities.similarCount(), 2);
    QCOMPARE(proxy.count(), 2);
    QVERIFY(proxy.rowOf(original) >= 0);
    QVERIFY(proxy.rowOf(recompressed) >= 0);
    QCOMPARE(proxy.rowOf(different), -1);
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Similar set 1 of 1"));
  }

  void duplicateSetsStayTogetherAndFollowTheChosenSort() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    const QStringList oldSet = {
        dir.filePath(QStringLiteral("screenshot-2025-01-02_08-00-00-old-a.png")),
        dir.filePath(QStringLiteral("screenshot-2025-01-02_09-00-00-old-b.png")),
    };
    const QStringList newSet = {
        dir.filePath(QStringLiteral("screenshot-2026-08-02_08-00-00-new-a.png")),
        dir.filePath(QStringLiteral("screenshot-2026-08-02_09-00-00-new-b.png")),
    };
    for (const QString& path : oldSet + newSet) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("media");
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QHash<QString, QString> groups;
    for (const QString& path : oldSet) {
      groups.insert(path, QStringLiteral("old"));
    }
    for (const QString& path : newSet) {
      groups.insert(path, QStringLiteral("new"));
    }
    proxy.setDuplicateGroups(groups);
    proxy.setDuplicatesOnly(true);

    QCOMPARE(groups.value(proxy.pathAt(0)), QStringLiteral("new"));
    QCOMPARE(groups.value(proxy.pathAt(1)), QStringLiteral("new"));
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Exact match 1 of 2"));
    QCOMPARE(proxy.gridLabelAt(2), QStringLiteral("Exact match 2 of 2"));

    proxy.setSortMode(CaptureFilterModel::OldestFirst);
    QCOMPARE(groups.value(proxy.pathAt(0)), QStringLiteral("old"));
    QCOMPARE(groups.value(proxy.pathAt(1)), QStringLiteral("old"));
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Exact match 1 of 2"));
    QCOMPARE(proxy.gridLabelAt(2), QStringLiteral("Exact match 2 of 2"));
  }

  void mediaDetailsParseCameraExposureVideoAndAudio() {
    const QByteArray image = "JPEG\n2020:05:06 07:08:09\n\nTestMake\nTestMake Model X\n"
                             "Prime 50mm\n28/10\n1/125\n400\n50/1\n8\nsRGB\n";
    const QStringList imageLines = MediaInspector::parseImage(image);
    QCOMPARE(imageLines.value(0), QStringLiteral("JPEG  ·  sRGB  ·  8-bit"));
    QVERIFY(imageLines.value(1).startsWith(QStringLiteral("Taken  ·  ")));
    QCOMPARE(imageLines.value(2), QStringLiteral("Camera  ·  TestMake Model X"));
    QCOMPARE(imageLines.value(3), QStringLiteral("Lens  ·  Prime 50mm"));
    QCOMPARE(imageLines.value(4), QStringLiteral("f/2.8  ·  1/125 s  ·  ISO 400  ·  50 mm"));

    const QByteArray video = R"({
      "streams": [
        {"codec_type":"video","codec_name":"h264","profile":"High",
         "pix_fmt":"yuv420p","r_frame_rate":"30000/1001"},
        {"codec_type":"audio","codec_name":"aac","sample_rate":"48000",
         "channel_layout":"stereo"}
      ],
      "format": {"format_name":"mov,mp4,m4a,3gp,3g2,mj2","bit_rate":"3608000"}
    })";
    const QStringList videoLines = MediaInspector::parseVideo(video, QStringLiteral("mp4"));
    QCOMPARE(videoLines.value(0), QStringLiteral("MP4  ·  H.264  ·  High  ·  yuv420p"));
    QCOMPARE(videoLines.value(1), QStringLiteral("29.97 fps  ·  3.6 Mbps"));
    QCOMPARE(videoLines.value(2), QStringLiteral("AAC  ·  stereo  ·  48 kHz"));
  }

  void embeddedMediaDatesParseWithoutChangingWallClockPhotoTime() {
    QCOMPARE(MediaMetadataIndex::parseImageDate("2020:05:06 07:08:09\n2019:01:02 03:04:05\n"),
             QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
    QCOMPARE(MediaMetadataIndex::parseImageDate("\n2019:01:02 03:04:05\n"),
             QDateTime(QDate(2019, 1, 2), QTime(3, 4, 5)));
    QVERIFY(!MediaMetadataIndex::parseImageDate("undefined\n\n").isValid());

    const QByteArray video = R"({
      "streams": [{"tags":{"creation_time":"2018-04-03T02:01:00"}}],
      "format": {"tags":{"com.apple.quicktime.creationdate":"2021-08-09T10:11:12"}}
    })";
    QCOMPARE(MediaMetadataIndex::parseVideoDate(video),
             QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
    QVERIFY(!MediaMetadataIndex::parseVideoDate("not json").isValid());
  }

  void embeddedCameraNamesParseAndNormalize() {
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("Apple"), QStringLiteral("iPhone 12")),
             QStringLiteral("Apple iPhone 12"));
    // Canon and Nikon repeat the maker in the model; do not print it twice.
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("Canon"),
                                            QStringLiteral("Canon EOS R6")),
             QStringLiteral("Canon EOS R6"));
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("SONY  "), QString()),
             QStringLiteral("SONY"));
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("undefined"), QStringLiteral("X-T5")),
             QStringLiteral("X-T5"));
    QVERIFY(MediaMetadataIndex::cameraName({}, {}).isEmpty());

    const MediaMetadataIndex::Details image = MediaMetadataIndex::parseImageDetails(
        "2020:05:06 07:08:09\n\nFUJIFILM\nX-T5\nXF16-55mmF2.8 R LM WR\n");
    QCOMPARE(image.captured, QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
    QCOMPARE(image.camera, QStringLiteral("FUJIFILM X-T5"));
    QCOMPARE(image.lens, QStringLiteral("XF16-55mmF2.8 R LM WR"));
    // A record cut short by the producer still parses positionally.
    const MediaMetadataIndex::Details bare = MediaMetadataIndex::parseImageDetails("\n\n");
    QVERIFY(!bare.captured.isValid());
    QVERIFY(bare.camera.isEmpty());
    QVERIFY(bare.lens.isEmpty());
    QCOMPARE(bare, MediaMetadataIndex::Details {});

    const MediaMetadataIndex::Details video = MediaMetadataIndex::parseVideoDetails(
        "{\"format\":{\"tags\":{\"creation_time\":\"2021-08-09T10:11:12\","
        "\"com.apple.quicktime.make\":\"Apple\",\"com.apple.quicktime.model\":\"iPhone 12\"}}}");
    QCOMPARE(video.captured, QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
    QCOMPARE(video.camera, QStringLiteral("Apple iPhone 12"));
    QVERIFY(MediaMetadataIndex::parseVideoDetails("not json").camera.isEmpty());
  }

  void generalMediaDatesAreEnrichedOnceAndProducerDatesAlwaysWin() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_DATE_TEST_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_DATE_TEST_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("date-runs.log"));
    QFile magick(dir.filePath(QStringLiteral("magick")));
    QVERIFY(magick.open(QIODevice::WriteOnly));
    magick.write("#!/bin/sh\n"
                 "printf 'image\\n' >> \"$OMAROLL_DATE_TEST_LOG\"\n"
                 "index=0\n"
                 "while [ \"$#\" -gt 0 ]; do\n"
                 "  if [ \"$1\" = -format ] && [ \"$#\" -ge 3 ]; then\n"
                 "    printf '\\036OMAROLL_DATE:%s\\n' \"$index\"\n"
                 "    case \"$3\" in\n"
                 "      *undated*) printf '\\n\\n' ;;\n"
                 "      *) printf '2020:05:06 07:08:09\\n\\nApple\\niPhone 12\\n"
                 "iPhone 12 back camera\\n' ;;\n"
                 "    esac\n"
                 "    index=$((index + 1))\n"
                 "    shift 3\n"
                 "  else\n"
                 "    shift\n"
                 "  fi\n"
                 "done\n");
    magick.close();
    QVERIFY(magick.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));

    QFile ffprobe(dir.filePath(QStringLiteral("ffprobe")));
    QVERIFY(ffprobe.open(QIODevice::WriteOnly));
    ffprobe.write("#!/bin/sh\n"
                  "printf '%s\\n' "
                  "'{\"format\":{\"tags\":{\"creation_time\":\"2021-08-09T10:"
                  "11:12\",\"com.apple.quicktime.make\":\"Apple\","
                  "\"com.apple.quicktime.model\":\"iPhone 12\"}}}'\n"
                  "printf 'video\\n' >> \"$OMAROLL_DATE_TEST_LOG\"\n");
    ffprobe.close();
    QVERIFY(ffprobe.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_DATE_TEST_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QString photo = dir.filePath(QStringLiteral("phone photo #1.jpg"));
    const QString capture = dir.filePath(QStringLiteral("screenshot-2026-08-31_10-00-00.png"));
    const QString undated = dir.filePath(QStringLiteral("undated.png"));
    QVERIFY(image.save(photo, "JPG"));
    QVERIFY(image.save(capture, "PNG"));
    QVERIFY(image.save(undated, "PNG"));
    const QString video = dir.filePath(QStringLiteral("family clip.mp4"));
    QFile clip(video);
    QVERIFY(clip.open(QIODevice::WriteOnly));
    clip.write("test video");
    clip.close();

    QFile::remove(MediaMetadataIndex::cachePath());
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    const int captureRow = model.rowOf(capture);
    QVERIFY(captureRow >= 0);
    QVERIFY(model.recordAt(captureRow).hasProducerTimestamp);
    QCOMPARE(model.recordAt(captureRow).captured, QDateTime(QDate(2026, 8, 31), QTime(10, 0)));
    const QDateTime undatedFallback = model.recordAt(model.rowOf(undated)).captured;

    {
      MediaMetadataIndex dates(&model);
      QTRY_COMPARE_WITH_TIMEOUT(dates.total(), 3, 2000);
      QTRY_VERIFY_WITH_TIMEOUT(!dates.indexing(), 3000);
      QCOMPARE(dates.completed(), 3);
      QCOMPARE(model.recordAt(model.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
      QCOMPARE(model.recordAt(model.rowOf(video)).captured,
               QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
      QCOMPARE(model.recordAt(model.rowOf(capture)).captured,
               QDateTime(QDate(2026, 8, 31), QTime(10, 0)));
      QCOMPARE(model.recordAt(model.rowOf(undated)).captured, undatedFallback);
      // The same probe fills in the camera, so a photo library can be browsed
      // by camera without a second pass over the files.
      QCOMPARE(model.recordAt(model.rowOf(photo)).camera, QStringLiteral("Apple iPhone 12"));
      QCOMPARE(model.recordAt(model.rowOf(photo)).lens, QStringLiteral("iPhone 12 back camera"));
      QCOMPARE(model.recordAt(model.rowOf(video)).camera, QStringLiteral("Apple iPhone 12"));
      QVERIFY(model.recordAt(model.rowOf(undated)).camera.isEmpty());
      QVERIFY(model.recordAt(model.rowOf(capture)).camera.isEmpty());

      // A result for an older version of the file cannot reorder the live row.
      model.applyMetadata({{photo, 0, 0, QDateTime(QDate(1990, 1, 1), QTime(0, 0)), 0, 0}});
      QCOMPARE(model.recordAt(model.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
      // Embedded metadata can never override a producer timestamp.
      const CaptureRecord& producer = model.recordAt(model.rowOf(capture));
      model.applyMetadata(
          {{capture, producer.modified, producer.bytes, QDateTime(QDate(1990, 1, 1), QTime(0, 0)),
            producer.device, producer.inode}});
      QCOMPARE(model.recordAt(model.rowOf(capture)).captured,
               QDateTime(QDate(2026, 8, 31), QTime(10, 0)));

      // A watcher rescan keeps resolved dates in place instead of briefly
      // restoring mtimes and reshuffling the grid.
      scanned.clear();
      model.refresh();
      QVERIFY(scanned.wait(5000));
      QCOMPARE(model.recordAt(model.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
      QCOMPARE(model.recordAt(model.rowOf(video)).captured,
               QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
      QCOMPARE(model.recordAt(model.rowOf(photo)).camera, QStringLiteral("Apple iPhone 12"));
    }

    QFile firstLog(logPath);
    QVERIFY(firstLog.open(QIODevice::ReadOnly));
    const QByteArray firstRuns = firstLog.readAll();
    firstLog.close();
    QCOMPARE(firstRuns.count("image\n"), 1);
    QCOMPARE(firstRuns.count("video\n"), 1);
    const QFileInfo dateCache(MediaMetadataIndex::cachePath());
    QVERIFY(dateCache.isFile());
    const QFileDevice::Permissions publicPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    QCOMPARE(dateCache.permissions() & publicPermissions, QFileDevice::Permissions());

    // The second index applies its private cache and starts no process. A
    // fresh model gets its cameras from that cache too.
    {
      CaptureModel fresh(&settings);
      QSignalSpy freshScan(&fresh, &CaptureModel::countChanged);
      QVERIFY(freshScan.wait(5000));
      MediaMetadataIndex cached(&fresh);
      QTest::qWait(300);
      QVERIFY(!cached.indexing());
      QCOMPARE(fresh.recordAt(fresh.rowOf(photo)).camera, QStringLiteral("Apple iPhone 12"));
      QCOMPARE(fresh.recordAt(fresh.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
    }
    QFile secondLog(logPath);
    QVERIFY(secondLog.open(QIODevice::ReadOnly));
    QCOMPARE(secondLog.readAll(), firstRuns);
  }

  void imageDatesUseBoundedBatchesAndPathIndexesStayCorrect() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_DATE_BATCH_LOG");
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_DATE_BATCH_LOG", previousLog);
      putBack("XDG_CACHE_HOME", previousCache);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("date-batches.log"));
    QFile magick(dir.filePath(QStringLiteral("magick")));
    QVERIFY(magick.open(QIODevice::WriteOnly));
    magick.write("#!/bin/sh\n"
                 "printf 'batch\\n' >> \"$OMAROLL_DATE_BATCH_LOG\"\n"
                 "index=0\n"
                 "status=0\n"
                 "while [ \"$#\" -gt 0 ]; do\n"
                 "  if [ \"$1\" = -format ] && [ \"$#\" -ge 3 ]; then\n"
                 "    case \"$3\" in\n"
                 "      *'photo 32 #'*) status=1 ;;\n"
                 "      *) printf '\\036OMAROLL_DATE:%s\\n2020:05:06 "
                 "07:08:09\\n\\n' \"$index\" ;;\n"
                 "    esac\n"
                 "    index=$((index + 1))\n"
                 "    shift 3\n"
                 "  else\n"
                 "    shift\n"
                 "  fi\n"
                 "done\n"
                 "exit \"$status\"\n");
    magick.close();
    QVERIFY(magick.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_DATE_BATCH_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::white);
    QStringList paths;
    for (int index = 0; index < 65; ++index) {
      const QString path =
          dir.filePath(QStringLiteral("photo %1 #.png").arg(index, 2, 10, QLatin1Char('0')));
      QVERIFY(image.save(path, "PNG"));
      paths.append(path);
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), paths.size());
    QSignalSpy dateChanges(&model, &QAbstractItemModel::dataChanged);

    {
      MediaMetadataIndex dates(&model);
      QTRY_COMPARE_WITH_TIMEOUT(dates.total(), paths.size(), 2000);
      QTRY_VERIFY_WITH_TIMEOUT(!dates.indexing(), 5000);
      QCOMPARE(dates.completed(), paths.size());
      for (const QString& path : std::as_const(paths)) {
        const int row = model.rowOf(path);
        QVERIFY(row >= 0);
        if (path.endsWith(QStringLiteral("photo 32 #.png"))) {
          QCOMPARE(model.recordAt(row).captured.toMSecsSinceEpoch(), model.recordAt(row).modified);
        } else {
          QCOMPARE(model.recordAt(row).captured, QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
        }
      }
    }
    QCOMPARE(dateChanges.count(), 1);

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll().count("batch\n"), 3);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(proxy.rowOf(paths.first()) >= 0);
    proxy.setSearchText(QStringLiteral("does not exist"));
    QCOMPARE(proxy.rowOf(paths.first()), -1);
    proxy.setSearchText({});

    const QString removed = paths.takeFirst();
    QVERIFY(QFile::remove(removed));
    const QString added = dir.filePath(QStringLiteral("new photo.png"));
    QVERIFY(image.save(added, "PNG"));
    scanned.clear();
    model.refresh();
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowOf(removed), -1);
    QVERIFY(model.rowOf(added) >= 0);
    for (const QString& path : std::as_const(paths)) {
      QVERIFY(model.rowOf(path) >= 0);
    }
  }

  void imageDetailsRunLocallyForAnOddPath() {
    if (QStandardPaths::findExecutable(QStringLiteral("magick")).isEmpty()) {
      QSKIP("ImageMagick not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("odd # photo ?.png"));
    QImage image(37, 23, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(path, "PNG"));
    const qint64 before = QFileInfo(path).size();

    MediaInspector inspector;
    inspector.inspect(path, false);
    QTRY_VERIFY_WITH_TIMEOUT(!inspector.loading(), 5000);
    QCOMPARE(inspector.path(), path);
    QVERIFY(!inspector.lines().isEmpty());
    QVERIFY(inspector.lines().first().startsWith(QStringLiteral("PNG")));
    QCOMPARE(QFileInfo(path).size(), before);
  }

  void hiddenIsExcludedUntilAskedFor() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QString path = dir.filePath(QStringLiteral("screenshot-2026-08-31_10-00-00.png"));
    QVERIFY(image.save(path, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy spy(&model, &CaptureModel::countChanged);
    QVERIFY(spy.wait(5000));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 1);

    // Marks are keyed on the canonical path the scanner produced.
    settings.toggleHidden(proxy.pathAt(0));
    QCOMPARE(proxy.count(), 0);

    proxy.setShowHidden(true);
    QCOMPARE(proxy.count(), 1);

    settings.toggleHidden(proxy.pathAt(0));
    proxy.setShowHidden(false);
    QCOMPARE(proxy.count(), 1);
  }

  void marksSurviveARootGoingAway() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString kept = dir.filePath(QStringLiteral("kept.png"));
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(kept, "PNG"));
    const QString gone = dir.filePath(QStringLiteral("deleted.png"));

    // A scan that saw neither, as happens when a root is unmounted or switched
    // off in settings. Only the mark whose file is actually gone may go.
    QCOMPARE(CaptureModel::missingMarks({kept, gone}, {}), QStringList {gone});
  }

  void scanSkipsTheRecorderTransients() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto touch = [&](const QString& name) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("x");
    };
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00-preview.png"));
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00-processed.mp4"));

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Recording);
  }

  void rescanIsADiffNotAReset() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(dir.filePath(QStringLiteral("a.png")), "PNG"));
    QVERIFY(image.save(dir.filePath(QStringLiteral("b.png")), "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 2);

    // One gone, one new: rows are removed and inserted, never reset, so the
    // grid keeps its scroll position and selection across a live update.
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removes(&model, &QAbstractItemModel::rowsRemoved);
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("a.png"))));
    QVERIFY(image.save(dir.filePath(QStringLiteral("c.png")), "PNG"));
    model.refresh();
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(inserts.size(), 1);
    QCOMPARE(removes.size(), 1);
    QCOMPARE(model.pathAt(0), dir.filePath(QStringLiteral("c.png")));
  }

  void fragmentedLargeRemovalUsesBoundedReset() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString seed = dir.filePath(QStringLiteral("item-000.png"));
    QVERIFY(image.save(seed, "PNG"));
    for (int index = 1; index < 600; ++index) {
      QVERIFY(QFile::copy(
          seed, dir.filePath(QStringLiteral("item-%1.png").arg(index, 3, 10, QLatin1Char('0')))));
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 600);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy removes(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy folderChanges(&proxy, &CaptureFilterModel::foldersChanged);
    for (int row = 0; row < model.rowCount(); row += 2) {
      QVERIFY(QFile::remove(model.pathAt(row)));
    }

    count.clear();
    model.refresh();
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 300);
    QCOMPARE(proxy.rowCount(), 300);
    QCOMPARE(resets.size(), 1);
    QCOMPARE(removes.size(), 0);
    QTRY_COMPARE(proxy.folderItemCount(dir.path()), 300);
    QCOMPARE(folderChanges.size(), 1);
  }

  void heldPathsStayOutOfTheLibraryUntilReleased() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(dir.filePath(QStringLiteral("done.png")), "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 1);

    // A transcode's output mid-write: held, so the scan leaves it out.
    const QString partial = dir.filePath(QStringLiteral("done-720p.gif"));
    model.holdPath(partial);
    QFile file(partial);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();
    model.refresh();
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowOf(partial), -1);

    // Released once the tool finishes, and the rescan it schedules brings the
    // finished file in without being told to.
    model.releasePath(partial);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 5000);
    QVERIFY(model.rowOf(partial) >= 0);
  }

  void nestedFoldersUpdateWhileTheAppIsOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString album = dir.filePath(QStringLiteral("Trips/Autumn"));
    QVERIFY(QDir().mkpath(album));
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 0);

    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::yellow);
    QVERIFY(image.save(QDir(album).filePath(QStringLiteral("new-photo.png")), "PNG"));
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1, 5000);
  }

  void uriListIsEncodedAndCrlfTerminated() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    const QString list =
        model.uriList({QStringLiteral("/tmp/a b.png"), QStringLiteral("/tmp/c#d.mp4")});
    QCOMPARE(list, QStringLiteral("file:///tmp/a%20b.png\r\nfile:///tmp/c%23d.mp4\r\n"));
  }

  // --- Actions ----------------------------------------------------------

  void registryKeysOnMedium() {
    ActionRegistry registry(nullptr);
    QCOMPARE(registry.primaryActionFor(true), QStringLiteral("trim"));
    QCOMPARE(registry.primaryActionFor(false), QStringLiteral("matte"));
    QCOMPARE(registry.shortcutFor(QStringLiteral("matte")), QStringLiteral("M"));
    QCOMPARE(registry.shortcutFor(QStringLiteral("hide")), QStringLiteral("Ctrl+H"));
    QVERIFY(registry.shortcutFor(QStringLiteral("missing")).isEmpty());
    // The public single-path overload must dispatch to the path-list overload,
    // not resolve its braced argument back to itself and recurse forever.
    QVERIFY(!registry.run(QStringLiteral("favorite"), QStringLiteral("/tmp/not-a-capture")));

    QStringList moving;
    for (const QVariant& row : registry.actionsFor(true)) {
      moving << row.toMap().value(QStringLiteral("id")).toString();
    }
    QVERIFY(moving.contains(QStringLiteral("play")));
    QVERIFY(moving.contains(QStringLiteral("frame")));
    QVERIFY(moving.contains(QStringLiteral("copy")));
    QVERIFY(moving.contains(QStringLiteral("export")));
    QVERIFY(moving.contains(QStringLiteral("rename")));
    QVERIFY(!moving.contains(QStringLiteral("gif")));
    QVERIFY(!moving.contains(QStringLiteral("shrink")));
    QVERIFY(!moving.contains(QStringLiteral("matte")));
    QVERIFY(!moving.contains(QStringLiteral("ocr")));
    QVERIFY(!moving.contains(QStringLiteral("background")));

    QStringList still;
    QString copyLabel;
    for (const QVariant& row : registry.actionsFor(false)) {
      const QVariantMap values = row.toMap();
      still << values.value(QStringLiteral("id")).toString();
      if (values.value(QStringLiteral("id")).toString() == QStringLiteral("copy")) {
        copyLabel = values.value(QStringLiteral("label")).toString();
      }
    }
    QVERIFY(still.contains(QStringLiteral("background")));
    QVERIFY(still.contains(QStringLiteral("export")));
    QVERIFY(still.contains(QStringLiteral("rename")));
    QVERIFY(!still.contains(QStringLiteral("frame")));
    QVERIFY(!still.contains(QStringLiteral("convert")));
    QCOMPARE(copyLabel, QStringLiteral("Copy image"));

    QStringList documents;
    for (const QVariant& row : registry.actionsForKind(false, true)) {
      documents << row.toMap().value(QStringLiteral("id")).toString();
    }
    QCOMPARE(registry.primaryActionForKind(false, true), QStringLiteral("open-document"));
    QVERIFY(documents.contains(QStringLiteral("open-document")));
    QVERIFY(documents.contains(QStringLiteral("send")));
    QVERIFY(documents.contains(QStringLiteral("rename")));
    QVERIFY(documents.contains(QStringLiteral("trash")));
    QVERIFY(!documents.contains(QStringLiteral("matte")));
    QVERIFY(!documents.contains(QStringLiteral("export")));
    QVERIFY(!documents.contains(QStringLiteral("copy")));
  }

  void backgroundActionUsesTheOmarchyHandlerEndToEnd() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    const QByteArray previousLog = qgetenv("OMAROLL_TEST_LOG");
    const auto restoreLog = qScopeGuard([&] {
      previousLog.isNull() ? qunsetenv("OMAROLL_TEST_LOG")
                           : qputenv("OMAROLL_TEST_LOG", previousLog);
    });

    QFile handler(dir.filePath(QStringLiteral("omarchy-theme-bg-set")));
    QVERIFY(handler.open(QIODevice::WriteOnly));
    handler.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$OMAROLL_TEST_LOG\"\n");
    handler.close();
    QVERIFY(handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    const QString logPath = dir.filePath(QStringLiteral("background.log"));
    QVERIFY(qputenv("OMAROLL_TEST_LOG", logPath.toUtf8()));

    const QString imagePath = dir.filePath(QStringLiteral("photo #1.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));

    ActionLauncher launcher;
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    ActionRegistry registry(&launcher);
    QVERIFY(registry.available(QStringLiteral("background")));
    QVERIFY(registry.appliesTo(QStringLiteral("background"), false));
    QVERIFY(!registry.appliesTo(QStringLiteral("background"), true));
    QVERIFY(registry.run(QStringLiteral("background"), imagePath));
    QCOMPARE(reported.size(), 1);
    QCOMPARE(reported.first().first().toString(), QStringLiteral("Set as current background"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(logPath), 1000);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(log.readAll()), imagePath + QLatin1Char('\n'));
  }

  void renamePreservesTheExtensionAndMovesLibraryState() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString original = dir.filePath(QStringLiteral("capture #1.PNG"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(original, "PNG"));
    const QString occupied = dir.filePath(QStringLiteral("occupied.PNG"));
    QVERIFY(image.save(occupied, "PNG"));

    const QString album = QStringLiteral("Rename test");
    AppSettings settings;
    settings.deleteAlbum(album);
    QVERIFY(settings.createAlbum(album));
    QVERIFY(settings.addToAlbum(album, {original}));
    settings.toggleFavorite(original);
    settings.toggleHidden(original);

    ActionLauncher launcher;
    QVERIFY(!launcher.renameFile(original, QString()).value(QStringLiteral("ok")).toBool());
    QVERIFY(!launcher.renameFile(original, QStringLiteral("bad/name"))
                 .value(QStringLiteral("ok"))
                 .toBool());
    const QVariantMap collision = launcher.renameFile(original, QStringLiteral("occupied"));
    QVERIFY(!collision.value(QStringLiteral("ok")).toBool());
    QCOMPARE(collision.value(QStringLiteral("error")).toString(),
             QStringLiteral("A file with that name already exists"));

    const QVariantMap renamed = launcher.renameFile(original, QStringLiteral("useful name"));
    QVERIFY(renamed.value(QStringLiteral("ok")).toBool());
    const QString newPath = dir.filePath(QStringLiteral("useful name.PNG"));
    QCOMPARE(renamed.value(QStringLiteral("path")).toString(), newPath);
    QCOMPARE(renamed.value(QStringLiteral("fileName")).toString(),
             QStringLiteral("useful name.PNG"));
    QVERIFY(!QFileInfo::exists(original));
    QVERIFY(QFileInfo::exists(newPath));

    settings.relocatePath(original, newPath);
    QVERIFY(settings.isFavorite(newPath));
    QVERIFY(settings.isHidden(newPath));
    QCOMPARE(settings.albumPaths(album), QStringList {newPath});
    AppSettings restored;
    QVERIFY(restored.isFavorite(newPath));
    QVERIFY(restored.isHidden(newPath));
    QCOMPARE(restored.albumPaths(album), QStringList {newPath});

    settings.toggleFavorite(newPath);
    settings.toggleHidden(newPath);
    settings.deleteAlbum(album);
  }

  void exportChoicesReachTheTranscoderForEverySelectedFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    QFile handler(dir.filePath(QStringLiteral("omarchy-transcode")));
    QVERIFY(handler.open(QIODevice::WriteOnly));
    handler.write("#!/bin/sh\n"
                  "printf '%s\\n%s\\n%s\\n' \"$1\" \"$2\" \"$3\" > \"$1.args\"\n"
                  "out=\"${1%.*}-$3.$2\"\n"
                  "printf converted > \"$out\"\n");
    handler.close();
    QVERIFY(handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));

    const QString first = dir.filePath(QStringLiteral("first photo.png"));
    const QString second = dir.filePath(QStringLiteral("second # photo.webp"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(first, "PNG"));
    QVERIFY(image.save(second, "WEBP"));

    ActionLauncher launcher;
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    ActionRegistry registry(&launcher);
    QVERIFY(registry.runBatchWith(QStringLiteral("export"),
                                  {{QStringLiteral("format"), QStringLiteral("png")},
                                   {QStringLiteral("resolution"), QStringLiteral("low")}},
                                  {first, second}));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 2, 3000);

    for (const QString& path : {first, second}) {
      QFile arguments(path + QStringLiteral(".args"));
      QVERIFY(arguments.open(QIODevice::ReadOnly));
      QCOMPARE(QString::fromUtf8(arguments.readAll()), path + QStringLiteral("\npng\nlow\n"));
      const QString output = QFileInfo(path).absolutePath() + QLatin1Char('/') +
                             QFileInfo(path).completeBaseName() + QStringLiteral("-low.png");
      QFile converted(output);
      QVERIFY(converted.open(QIODevice::ReadOnly));
      QCOMPARE(converted.readAll(), QByteArray("converted"));
    }
  }

  void currentVideoFrameIsSavedAtTheChosenPosition() {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
      QSKIP("ffmpeg is not installed");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString video = dir.filePath(QStringLiteral("short clip.mp4"));
    QProcess encoder;
    encoder.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                           QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                           QStringLiteral("-i"), QStringLiteral("color=c=red:s=160x90:d=1:r=10"),
                           QStringLiteral("-c:v"), QStringLiteral("libx264"),
                           QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), video});
    QVERIFY(encoder.waitForFinished(10000));
    QCOMPARE(encoder.exitStatus(), QProcess::NormalExit);
    QCOMPARE(encoder.exitCode(), 0);

    ActionLauncher launcher;
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    ActionRegistry registry(&launcher);
    QVERIFY(registry.runBatchWith(QStringLiteral("frame"),
                                  {{QStringLiteral("seek"), QStringLiteral("0.500")},
                                   {QStringLiteral("frame"), QStringLiteral("00m00s500")}},
                                  {video}));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 5000);
    QVERIFY(settled.first().at(1).toBool());

    const QString output = dir.filePath(QStringLiteral("short clip-frame-00m00s500.png"));
    QCOMPARE(settled.first().at(0).toString(), output);
    const QImage frame(output);
    QVERIFY(!frame.isNull());
    QCOMPARE(frame.size(), QSize(160, 90));
    QVERIFY(QFileInfo(video).exists());
  }

  void clipboardFailuresAreReportedAndSecretsStayOutOfHistory() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    const auto script = [&](const QString& name, const QByteArray& body) {
      QFile file(dir.filePath(name));
      if (!file.open(QIODevice::WriteOnly)) {
        return false;
      }
      file.write("#!/bin/sh\n");
      file.write(body);
      file.close();
      return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
    };
    QVERIFY(script(QStringLiteral("wl-copy"), "exit 1\n"));
    QVERIFY(script(QStringLiteral("zbarimg"), "printf 'otpauth://private'\n"));
    QVERIFY(script(QStringLiteral("omarchy-menu-share"),
                   "printf '%s\\n' \"$@\" > \"$OMAROLL_TEST_LOG\"\n"));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    const QString logPath = dir.filePath(QStringLiteral("arguments.log"));
    const QByteArray previousLog = qgetenv("OMAROLL_TEST_LOG");
    const auto restoreLog = qScopeGuard([&] {
      previousLog.isNull() ? qunsetenv("OMAROLL_TEST_LOG")
                           : qputenv("OMAROLL_TEST_LOG", previousLog);
    });
    QVERIFY(qputenv("OMAROLL_TEST_LOG", logPath.toUtf8()));

    const QString imagePath = dir.filePath(QStringLiteral("capture.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(imagePath, "PNG"));

    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QVERIFY(!launcher.copyFile(imagePath));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(reported.size(), 0);

    failed.clear();
    QGuiApplication::clipboard()->setText(QStringLiteral("unchanged"));
    ActionRegistry registry(&launcher);
    QVERIFY(registry.run(QStringLiteral("qr"), imagePath));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1000);
    QCOMPARE(reported.size(), 0);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("unchanged"));

    const QStringList paths = {QStringLiteral("/tmp/first capture.png"),
                               QStringLiteral("/tmp/second # capture.mp4")};
    QVERIFY(registry.runBatch(QStringLiteral("send"), paths));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(logPath), 1000);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(log.readAll()),
             QStringLiteral("file\n/tmp/first capture.png\n/tmp/second # capture.mp4\n"));
  }

  void trackedRunsReportSettleAndCleanUpAfterFailure() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy pending(&launcher, &ActionLauncher::outputPending);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);

    // Success: pending while the tool runs, settled saved, and the user is
    // told where the file went.
    const QString saved = dir.filePath(QStringLiteral("clip-720p.gif"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"), QStringLiteral("printf data > '%1'").arg(saved)}, {}, saved));
    QVERIFY(launcher.isPending(saved));
    QCOMPARE(pending.size(), 1);
    QVERIFY(reported.last().at(0).toString().contains(QStringLiteral("Making clip-720p.gif")));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
    QCOMPARE(settled.last().at(0).toString(), saved);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QVERIFY(!launcher.isPending(saved));
    QVERIFY(reported.last().at(0).toString().contains(
        QStringLiteral("Saved clip-720p.gif beside the original")));
    QCOMPARE(failed.size(), 0);

    // While a run is in flight, asking again says so rather than lying that
    // the partial file means the job is already done.
    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    const QString busy =
        dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00-720p.gif"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"), QStringLiteral("sleep 0.3 && printf data > '%1'").arg(busy)}, {},
        busy));
    ActionRegistry registry(&launcher);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QVERIFY(reported.last().at(0).toString().contains(QStringLiteral("Still working on")));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 2, 3000);
    QCOMPARE(settled.last().at(1).toBool(), true);

    // Failure: the partial write this run left behind is removed, and the
    // tool's own last words are quoted rather than a bare exit code.
    const QString broken = dir.filePath(QStringLiteral("clip-1080p.mp4"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"),
         QStringLiteral("printf junk > '%1'; echo boom >&2; exit 1").arg(broken)},
        {}, broken));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 3, 3000);
    QCOMPARE(settled.last().at(1).toBool(), false);
    QVERIFY(!QFileInfo::exists(broken));
    QVERIFY(failed.last().at(0).toString().contains(QStringLiteral("boom")));
  }

  void existingOutputsAreRevealedAndEmptyCorpsesCleared() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    // A stand-in transcoder that writes the same output name the real one
    // would, so the registry's guard and launch path both run for real.
    QFile script(dir.filePath(QStringLiteral("omarchy-transcode")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nout=\"${1%.*}-720p.gif\"\nprintf fresh > \"$out\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);

    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    const QString output =
        dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00-720p.gif"));

    // The corpse of a transcode that died before runs were tracked: an empty
    // file that used to make every retry report "already done" and stop.
    QFile corpse(output);
    QVERIFY(corpse.open(QIODevice::WriteOnly));
    corpse.close();
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QFile finished(output);
    QVERIFY(finished.open(QIODevice::ReadOnly));
    QCOMPARE(finished.readAll(), QByteArray("fresh"));
    finished.close();

    // A genuinely finished file is handed to the viewer without relaunching,
    // so pressing the action again always shows the result.
    reported.clear();
    QSignalSpy alreadyDone(&launcher, &ActionLauncher::outputAlreadyDone);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QCOMPARE(alreadyDone.size(), 1);
    QCOMPARE(alreadyDone.last().at(0).toString(), output);
    QCOMPARE(settled.size(), 1);
    QVERIFY(reported.last().at(0).toString().contains(QStringLiteral("Already made earlier")));
    QVERIFY(finished.open(QIODevice::ReadOnly));
    QCOMPARE(finished.readAll(), QByteArray("fresh"));
  }

  void truncatedLeftoversAreRedoneNotCalledDone() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
      QSKIP("ffprobe not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    QFile script(dir.filePath(QStringLiteral("omarchy-transcode")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nout=\"${1%.*}-1080p.mp4\"\nprintf fresh > \"$out\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    // The stub shadows the real transcoder, but ffprobe stays reachable.
    QVERIFY(qputenv(
        "PATH",
        (dir.path() + QStringLiteral(":") + QString::fromLocal8Bit(previousPath)).toLocal8Bit()));

    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-00-00.mp4"));
    const QString output =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-00-00-1080p.mp4"));

    // The fire-and-forget era could die mid-write: a non-empty file that no
    // player can open, which used to block every retry as "already done".
    QFile corpse(output);
    QVERIFY(corpse.open(QIODevice::WriteOnly));
    corpse.write(QByteArray(4096, 'x'));
    corpse.close();

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    QSignalSpy alreadyDone(&launcher, &ActionLauncher::outputAlreadyDone);
    QVERIFY(registry.run(QStringLiteral("shrink"), source));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 5000);
    QCOMPARE(alreadyDone.size(), 0);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QFile redone(output);
    QVERIFY(redone.open(QIODevice::ReadOnly));
    QCOMPARE(redone.readAll(), QByteArray("fresh"));
  }

  void tailscalePeersAreTheMachinesThatCanTakeAFile() {
    QVariantList peers;
    QString error;

    QVERIFY(!TailscalePeers::parse(R"({"BackendState":"NeedsLogin","Self":{"UserID":0}})", &peers,
                                   &error));
    QVERIFY(error.contains(QStringLiteral("not logged in")));
    QVERIFY(!TailscalePeers::parse("not json", &peers, &error));
    QVERIFY(!error.isEmpty());

    // The daemon's own verdict wins: the offline laptop and the machine owned
    // by someone else are out, the phone is in, and a daemon too old to give
    // a verdict falls back to online and same owner.
    const QByteArray running = R"({
      "BackendState": "Running",
      "Self": {"UserID": 7, "HostName": "desk"},
      "Peer": {
        "a": {"HostName": "phone", "DNSName": "phone.tail.ts.net.", "OS": "android",
              "UserID": 7, "Online": true, "TaildropTarget": 1},
        "b": {"HostName": "laptop", "DNSName": "laptop.tail.ts.net.", "OS": "linux",
              "UserID": 7, "Online": false, "TaildropTarget": 5},
        "c": {"HostName": "shared", "DNSName": "shared.tail.ts.net.", "OS": "linux",
              "UserID": 9, "Online": true, "TaildropTarget": 9},
        "d": {"HostName": "old", "DNSName": "", "OS": "macOS",
              "UserID": 7, "Online": true, "TaildropTarget": 0},
        "e": {"HostName": "theirs", "DNSName": "theirs.tail.ts.net.", "OS": "windows",
              "UserID": 9, "Online": true, "TaildropTarget": 0}
      }})";
    QVERIFY(TailscalePeers::parse(running, &peers, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(peers.size(), 2);
    QCOMPARE(peers.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("old"));
    QCOMPARE(peers.at(0).toMap().value(QStringLiteral("machine")).toString(),
             QStringLiteral("old"));
    QCOMPARE(peers.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("phone"));
    QCOMPARE(peers.at(1).toMap().value(QStringLiteral("machine")).toString(),
             QStringLiteral("phone.tail.ts.net"));

    QVERIFY(TailscalePeers::parse(R"({"BackendState":"Running","Self":{"UserID":7},"Peer":{}})",
                                  &peers, &error));
    QVERIFY(peers.isEmpty());
    QVERIFY(error.contains(QStringLiteral("No other machine")));
  }

  void sendToMachineHandsTheMachineAndEveryFileToTheHouseSender() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    // A stand-in for omarchy-tailscale-send that records its argv, one per
    // line, so the test can see exactly what the real one would be handed.
    const QString record = dir.filePath(QStringLiteral("argv.txt"));
    QFile script(dir.filePath(QStringLiteral("omarchy-tailscale-send")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"" +
                 record.toUtf8() + "\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QVERIFY(registry.available(QStringLiteral("tailscale")));
    QVERIFY(registry.appliesTo(QStringLiteral("tailscale"), true));
    QVERIFY(registry.appliesTo(QStringLiteral("tailscale"), false));

    const QString first = dir.filePath(QStringLiteral("a shot.png"));
    const QString second = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(registry.runBatchWith(
        QStringLiteral("tailscale"),
        {{QStringLiteral("machine"), QStringLiteral("laptop.tail.ts.net")}}, {first, second}));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(record) && QFileInfo(record).size() > 0, 3000);
    QFile recorded(record);
    QVERIFY(recorded.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList argv =
        QString::fromUtf8(recorded.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(argv, (QStringList {QStringLiteral("laptop.tail.ts.net"), first, second}));
  }

  void clipToGifRunsRealConversionWithoutDesktopSideEffects() {
    if (QStandardPaths::findExecutable(QStringLiteral("omarchy-transcode")).isEmpty() ||
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
      QSKIP("omarchy-transcode or ffmpeg not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Keep the real conversion, but never notify the desktop or replace the
    // user's clipboard from an automated test. The session bus stays blocked
    // when this suite runs in the local sandbox.
    const QString bin = dir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    for (const QString& name : {QStringLiteral("omarchy-notification-send"), QStringLiteral("wl-copy")}) {
      QFile helper(bin + QLatin1Char('/') + name);
      QVERIFY(helper.open(QIODevice::WriteOnly));
      helper.write(name == QStringLiteral("wl-copy") ? "#!/bin/sh\ncat >/dev/null\n"
                                                     : "#!/bin/sh\nexit 0\n");
      helper.close();
      QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner));
    }
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + previousPath));

    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-02_09-00-00.mp4"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("testsrc2=size=320x240:rate=10:duration=3"),
                  QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
                  QStringLiteral("yuv420p"), source});
    QVERIFY(ffmpeg.waitForFinished(30000));
    QCOMPARE(ffmpeg.exitCode(), 0);

    // The exact path the Clip to GIF button takes, with the real tool.
    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 60000);
    if (!failed.isEmpty()) {
      qWarning() << "failure reported:" << failed.last().at(0).toString();
    }
    QCOMPARE(settled.last().at(1).toBool(), true);
    const QString output =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_09-00-00-720p.gif"));
    QVERIFY(QFileInfo(output).size() > 0);
  }

  void trashIsRecoverableAndMissingFilesFailClearly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("recover me.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("keep this recoverable");
    file.close();

    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    const bool moved = launcher.moveToTrash(path);
    QVERIFY2(moved, failed.isEmpty() ? "moveToTrash failed without a message"
                                     : qPrintable(failed.last().first().toString()));
    QVERIFY(!QFileInfo::exists(path));

    const QDir trash(m_scratch.filePath(QStringLiteral("data/Trash/files")));
    QVERIFY2(!trash.entryList({QStringLiteral("recover me*")}, QDir::Files).isEmpty(),
             qPrintable(trash.absolutePath()));

    QVERIFY(!launcher.moveToTrash(path));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("That file is no longer there"));
  }

  // --- Thumbnails -------------------------------------------------------

  void thumbnailCoversTheTileWithoutUpscaling() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("photo.png"));
    QImage image(400, 300, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(path, "PNG"));

    // A wide tile from a 4:3 source: the short side has to meet the tile so
    // the grid's crop never stretches, and the aspect must survive.
    const QImage tile = ThumbnailCache::thumbnail(path, QSize(200, 100), 1.0);
    QVERIFY(!tile.isNull());
    QCOMPARE(tile.width(), 200);
    QCOMPARE(tile.height(), 150);

    // A source smaller than the tile is left alone rather than blown up.
    const QImage small = ThumbnailCache::thumbnail(path, QSize(800, 800), 1.0);
    QCOMPARE(small.size(), image.size());
  }

  void derivedFilesWearTheirSourcesTile() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty() ||
        QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer")).isEmpty()) {
      QSKIP("ffmpeg or ffmpegthumbnailer not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Dark for the opening quarter then green, the shape of a screen
    // recording that starts on an idle desktop. Short on purpose: a short
    // re-encode has a single keyframe, the case where a percent seek fails
    // and the derived file used to fall back to its dark first frame.
    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-40-00.mp4"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=0x0a0a0a:s=320x240:r=30:d=2"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=0x22aa66:s=320x240:r=30:d=6"),
                  QStringLiteral("-filter_complex"), QStringLiteral("[0][1]concat=n=2:v=1"),
                  QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
                  QStringLiteral("yuv420p"), source});
    QVERIFY(ffmpeg.waitForFinished(30000));
    QCOMPARE(ffmpeg.exitCode(), 0);

    // The derived names omarchy-transcode writes. Contents deliberately
    // unrelated (a black frame), because the tile must come from the source.
    const QString gif =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-40-00-720p.gif"));
    const QString resized =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-40-00-1080p.mp4"));
    QProcess black;
    black.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                 QStringLiteral("lavfi"), QStringLiteral("-i"),
                 QStringLiteral("color=black:s=320x240:r=10:d=1"), gif});
    QVERIFY(black.waitForFinished(30000));
    QCOMPARE(black.exitCode(), 0);
    QVERIFY(QFile::copy(source, resized));

    const QImage sourceTile = ThumbnailCache::thumbnail(source, QSize(80, 60), 1.0, 20);
    const QImage gifTile = ThumbnailCache::thumbnail(gif, QSize(80, 60), 1.0, 20);
    const QImage resizedTile = ThumbnailCache::thumbnail(resized, QSize(80, 60), 1.0, 20);
    QVERIFY(!sourceTile.isNull());
    QCOMPARE(gifTile, sourceTile);
    QCOMPARE(resizedTile, sourceTile);
  }

  void animatedGifThumbnailSeeksLikeAVideo() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
      QSKIP("ffmpeg not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // One second of red then four of blue, so frame zero and the 20% frame a
    // video's tile would show are unmistakably different colours.
    const QString path = dir.filePath(QStringLiteral("clip-720p.gif"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=red:s=64x64:r=5:d=1"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=blue:s=64x64:r=5:d=4"), QStringLiteral("-filter_complex"),
                  QStringLiteral("[0][1]concat=n=2:v=1"), path});
    QVERIFY(ffmpeg.waitForFinished(15000));
    QCOMPARE(ffmpeg.exitCode(), 0);

    // The default grid seek is 20%: past the red opening, into the blue.
    const QImage tile = ThumbnailCache::thumbnail(path, QSize(64, 64), 1.0, 20);
    QVERIFY(!tile.isNull());
    const QColor centre = tile.pixelColor(tile.width() / 2, tile.height() / 2);
    // The GIF palette dims pure blue, so compare channels rather than expect
    // full saturation.
    QVERIFY2(
        centre.blue() > 120 && centre.red() < 80,
        qPrintable(QStringLiteral("expected the 20%% frame (blue), got %1").arg(centre.name())));
  }

  // --- Mattes -----------------------------------------------------------

  void hueExtractorFindsDominantColour() {
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(QColor::fromHsv(200, 200, 200));
    const int hue = HueExtractor::dominantHue(image);
    QVERIFY(hue >= 190 && hue <= 210);
  }

  void hueExtractorReportsGreyscaleHonestly() {
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(QColor(120, 120, 120));
    QCOMPARE(HueExtractor::dominantHue(image), -1);
    // Still hands back something usable rather than an invalid colour.
    QVERIFY(HueExtractor::dominantColor(image).isValid());
  }

  void composeGrowsTheCanvasAndKeepsTheCapture() {
    QImage source(200, 100, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(30, 200, 200));

    const QImage result =
        MatteComposer::compose(source, MatteComposer::Adaptive, MatteComposer::Original, 0.10);
    QVERIFY(!result.isNull());
    // Padding has a 24 px floor, applied on both sides.
    QCOMPARE(result.width(), 200 + 2 * 24);
    QCOMPARE(result.height(), 100 + 2 * 24);
  }

  void composeNoneReturnsTheOriginal() {
    QImage source(40, 30, QImage::Format_RGB32);
    source.fill(Qt::magenta);
    const QImage result =
        MatteComposer::compose(source, MatteComposer::None, MatteComposer::Square, 0.2);
    QCOMPARE(result.size(), source.size());
  }

  void composeHonoursAspectWithoutCropping() {
    QImage source(100, 400, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(280, 180, 180));

    const QImage result =
        MatteComposer::compose(source, MatteComposer::Deep, MatteComposer::Wide, 0.05);
    QVERIFY(!result.isNull());

    const qreal ratio = qreal(result.width()) / result.height();
    QVERIFY2(qAbs(ratio - 16.0 / 9.0) < 0.02, qPrintable(QString::number(ratio)));
    // The capture is tall, so the canvas had to grow sideways rather than crop.
    QVERIFY(result.width() >= source.width());
    QVERIFY(result.height() >= source.height());
  }

  void budgetScalingKeepsTheCaptureWholeAndProportioned() {
    // A tall scrolling screenshot at the largest padding stop, forced wide:
    // the canvas is shrunk to the budget, and the padding has to shrink with
    // it or the capture is cropped top and bottom.
    QImage tall(1080, 7000, QImage::Format_RGB32);
    tall.fill(QColor(255, 0, 255));
    const QImage result =
        MatteComposer::compose(tall, MatteComposer::Slate, MatteComposer::Wide, 0.18);
    QVERIFY(!result.isNull());

    int top = result.height();
    int bottom = -1;
    for (int y = 0; y < result.height(); ++y) {
      const QRgb pixel = result.pixel(result.width() / 2, y);
      if (qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) > 200) {
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
    QVERIFY(bottom > top);
    // Not cropped: matte above and below the capture.
    QVERIFY(top > 0);
    QVERIFY(bottom < result.height() - 1);
    // Proportioned: the vertical padding is the requested 18% of the long
    // edge, which on this canvas is about 13% of its height either side.
    const qreal above = qreal(top) / result.height();
    const qreal below = qreal(result.height() - 1 - bottom) / result.height();
    QVERIFY2(above > 0.10 && above < 0.16, qPrintable(QString::number(above)));
    QVERIFY2(below > 0.10 && below < 0.16, qPrintable(QString::number(below)));
    // The capture keeps its own shape rather than becoming a sliver.
    int left = result.width();
    int right = -1;
    for (int x = 0; x < result.width(); ++x) {
      const QRgb pixel = result.pixel(x, result.height() / 2);
      if (qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) > 200) {
        left = std::min(left, x);
        right = std::max(right, x);
      }
    }
    const qreal ratio = qreal(right - left + 1) / (bottom - top + 1);
    QVERIFY2(std::abs(ratio - 1080.0 / 7000.0) < 0.01, qPrintable(QString::number(ratio)));
  }

  void composeStaysWithinTheOutputBudget() {
    // A very tall capture forced into a wide aspect is the case that could
    // otherwise ask for a multi-gigabyte canvas.
    QImage source(400, 12000, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(90, 180, 180));

    const QImage result =
        MatteComposer::compose(source, MatteComposer::Slate, MatteComposer::Social, 0.05);
    QVERIFY(!result.isNull());
    const qint64 pixels = qint64(result.width()) * result.height();
    QVERIFY2(pixels <= MatteComposer::kMaxOutputPixels, qPrintable(QString::number(pixels)));
  }

  void everyMatteProducesSomething() {
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(15, 190, 190));

    for (int matte = 0; matte < MatteComposer::MatteCount; ++matte) {
      const QImage result = MatteComposer::compose(source, static_cast<MatteComposer::Matte>(matte),
                                                   MatteComposer::Original, 0.08);
      QVERIFY2(!result.isNull(), qPrintable(QStringLiteral("matte %1 was null").arg(matte)));
    }
  }

private:
  QTemporaryDir m_scratch;
};

int main(int argc, char* argv[]) {
  disableHeadlessAudio();
  QGuiApplication application(argc, argv);
  OmarollTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}
#include "tst_omaroll.moc"
