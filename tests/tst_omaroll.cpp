#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "app/AppSettings.h"
#include "app/SingleInstance.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "matte/HueExtractor.h"
#include "matte/MatteComposer.h"
#include "sources/CaptureLocations.h"
#include "sources/CaptureScanner.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailCache.h"

#include <QClipboard>
#include <QPainter>
#include <QScopeGuard>
#include <QSignalSpy>
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
    QVERIFY(qputenv("XDG_CACHE_HOME",
                    m_scratch.filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME",
                    m_scratch.filePath(QStringLiteral("data")).toUtf8()));
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
    QVERIFY(!second.claimOrNotify(path));
    QTRY_COMPARE_WITH_TIMEOUT(activation.size(), 1, 1000);
    QCOMPARE(activation.first().first().toString(), path);
  }

  void disabledXdgPictureDirectoryDoesNotScanHome() {
    const QByteArray previous = qgetenv("XDG_PICTURES_DIR");
    QVERIFY(qputenv("XDG_PICTURES_DIR", QDir::homePath().toUtf8()));
    QCOMPARE(CaptureLocations::pictures(),
             QDir::homePath() + QStringLiteral("/Pictures"));
    if (previous.isNull()) {
      qunsetenv("XDG_PICTURES_DIR");
    } else {
      QVERIFY(qputenv("XDG_PICTURES_DIR", previous));
    }
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
    settings.setSlideshowVideos(true);

    const QString unavailable = dir.path();
    QVERIFY(QDir(unavailable).removeRecursively());
    AppSettings restored;
    QVERIFY(restored.libraryFolders().contains(unavailable));
    QCOMPARE(restored.imagePrimaryAction(), QStringLiteral("view"));
    QCOMPARE(restored.videoPrimaryAction(), QStringLiteral("play"));
    QCOMPARE(restored.thumbnailCacheMb(), 512);
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
    shell.write(
        "[launcher]\nbackground = \"#223344\"\nbackground-alpha = 0.73\n");
    shell.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#223344")));
    QCOMPARE(theme.surfaceAlpha(), 0.73);

    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate |
                       QIODevice::Text));
    shell.write(
        "[launcher]\nbackground = \"#445566\"\nbackground-alpha = 0.91\n");
    shell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(),
                              QColor(QStringLiteral("#445566")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.91, 1500);

    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate |
                       QIODevice::Text));
    shell.write(
        "[launcher]\nbackground = \"not-a-color\"\nbackground-alpha = 4.0\n");
    shell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(),
                              QColor(QStringLiteral("#05080a")), 1500);
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
    QVERIFY(QDir(root).rename(QStringLiteral("next-theme"),
                              QStringLiteral("theme")));
    QTRY_COMPARE_WITH_TIMEOUT(theme.background(),
                              QColor(QStringLiteral("#302010")), 1500);
  }

  void themeUsesMachineLauncherOverride() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString themePath =
        state.filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themePath));
    QFile colors(themePath + QStringLiteral("/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#101820\"\nforeground = \"#f0f0f0\"\n");
    colors.close();
    QFile shell(themePath + QStringLiteral("/shell.toml"));
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Text));
    shell.write(
        "[launcher]\nbackground = \"#223344\"\nbackground-alpha = 0.73\n");
    shell.close();

    const QString userRoot = config.filePath(QStringLiteral("omarchy"));
    QVERIFY(QDir().mkpath(userRoot));
    QFile userShell(userRoot + QStringLiteral("/shell.toml"));
    QVERIFY(userShell.open(QIODevice::WriteOnly | QIODevice::Text));
    userShell.write(
        "[launcher]\nbackground = \"#445566\"\nbackground-alpha = 0.91\n");
    userShell.close();

    OmarchyTheme theme(state.path(), config.path());
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#445566")));
    QCOMPARE(theme.surfaceAlpha(), 0.91);

    QVERIFY(userShell.open(QIODevice::WriteOnly | QIODevice::Truncate |
                           QIODevice::Text));
    userShell.write(
        "[launcher]\nbackground = \"#667788\"\nbackground-alpha = 0.64\n");
    userShell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(),
                              QColor(QStringLiteral("#667788")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.64, 1500);

    QVERIFY(userShell.remove());
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(),
                              QColor(QStringLiteral("#223344")), 1500);
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
    QCOMPARE(settings.albumPaths(album), QStringList{original});

    const QString renamed = dir.filePath(QStringLiteral("renamed.png"));
    QVERIFY(QFile::rename(original, renamed));
    CaptureRecord renamedRecord;
    renamedRecord.path = renamed;
    renamedRecord.bytes = QFileInfo(renamed).size();
    settings.reconcileAlbums({renamedRecord});
    QCOMPARE(settings.albumPaths(album), QStringList{renamed});
    AppSettings afterRename;
    QCOMPARE(afterRename.albumPaths(album), QStringList{renamed});

    // Copy then remove gives the file a new inode, like a move between
    // filesystems. The content identity still repairs the album entry.
    const QString copied = dir.filePath(QStringLiteral("copied.png"));
    QVERIFY(QFile::copy(renamed, copied));
    QVERIFY(QFile::remove(renamed));
    CaptureRecord copiedRecord;
    copiedRecord.path = copied;
    copiedRecord.bytes = QFileInfo(copied).size();
    settings.reconcileAlbums({copiedRecord});
    QCOMPARE(settings.albumPaths(album), QStringList{copied});

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

  void classifiesOmarchyScreenshot() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(
        QStringLiteral("screenshot-2026-08-31_23-26-39.png"), kind, captured));
    QCOMPARE(kind, CaptureRecord::Screenshot);
    QCOMPARE(captured.date(), QDate(2026, 8, 31));
    QCOMPARE(captured.time(), QTime(23, 26, 39));
  }

  void classifiesOmarchyRecording() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(
        QStringLiteral("screenrecording-2026-08-31_23-26-39.mp4"), kind,
        captured));
    QCOMPARE(kind, CaptureRecord::Recording);
    QCOMPARE(captured.time(), QTime(23, 26, 39));
  }

  void classifiesThirdPartyProducers_data() {
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<int>("expected");

    QTest::newRow("grim") << "20260831_23h26m39s_grim.png"
                          << int(CaptureRecord::Screenshot);
    QTest::newRow("flameshot")
        << "flameshot_2026-08-31.png" << int(CaptureRecord::Screenshot);
    QTest::newRow("gnome style")
        << "Screenshot from 2026-08-31.png" << int(CaptureRecord::Screenshot);
    QTest::newRow("obs") << "2026-08-31 23-26-39.mkv"
                         << int(CaptureRecord::Recording);
    QTest::newRow("gsr") << "Video_2026-08-31.mp4"
                         << int(CaptureRecord::Recording);
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
    QVERIFY(!CaptureScanner::classifyByName(QStringLiteral("holiday-photo.jpg"),
                                            kind, captured));
    QVERIFY(!CaptureScanner::classifyByName(QStringLiteral("IMG_4821.jpeg"),
                                            kind, captured));
  }

  // --- Traversal --------------------------------------------------------

  void scanAppliesDepthAndSkipRules() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto write = [&](const QString &relative) {
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
    auto shallow = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(shallow.size(), 1);
    QCOMPARE(shallow.first().fileName, QStringLiteral("top.png"));

    // Depth 2 reaches nested/ but not nested/deeper/, and never the skipped
    // directories at any depth.
    auto deeper = CaptureScanner::scan(
        {{dir.path(), 2, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(deeper.size(), 2);

    QStringList names;
    for (const auto &record : deeper) {
      names << record.fileName;
    }
    std::sort(names.begin(), names.end());
    QCOMPARE(names, QStringList({QStringLiteral("one.png"),
                                 QStringLiteral("top.png")}));
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

    const auto write = [&](const QString &relative) {
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

    const auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Download, CaptureRecord::Download}});
    QCOMPARE(records.size(), 4);
    for (const auto &record : records) {
      QCOMPARE(record.kind, CaptureRecord::Download);
      // A downloaded clip still plays, scrubs and trims like a recording.
      QCOMPARE(record.isVideo(),
               CaptureScanner::isVideo(QFileInfo(record.path).suffix()));
    }
  }

  void videoNameOnImageFileStaysAnImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::green);
    // A recording name on a PNG: the medium has to win, or the grid offers
    // "Trim" on something omacut cannot open.
    QVERIFY(image.save(
        dir.filePath(QStringLiteral("screenrecording-2026-08-31_10-00-00.png")),
        "PNG"));

    const auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Screenshot);
  }

  // --- Filtering and sorting -------------------------------------------

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
    const auto write = [&](const QString &name, int edge,
                           const QDateTime &stamp) {
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
    write(QStringLiteral("beach.png"), 16,
          QDateTime(QDate(2026, 8, 20), QTime(9, 0)));

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
    QCOMPARE(proxy.fileNameAt(0),
             QStringLiteral("screenshot-2026-08-31_10-00-00.png"));

    proxy.setSortMode(CaptureFilterModel::OldestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    // Size sorting reads the file, not the name.
    proxy.setSortMode(CaptureFilterModel::LargestFirst);
    QCOMPARE(proxy.fileNameAt(0),
             QStringLiteral("screenshot-2026-08-31_10-00-00.png"));

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
    write(QStringLiteral("mountain.png"), 12,
          QDateTime(QDate(2026, 8, 19), QTime(9, 0)));
    model.refresh();
    QVERIFY(rescanned.wait(5000));
    QCOMPARE(proxy.sourceCount(), 4);
    QCOMPARE(proxy.count(), 1);
    QVERIFY(!sourceCountChanged.isEmpty());

    proxy.setSearchText({});
    QCOMPARE(proxy.count(), 4);

    rescanned.clear();
    write(QStringLiteral("album/forest.png"), 18,
          QDateTime(QDate(2026, 8, 18), QTime(9, 0)));
    model.refresh();
    QVERIFY(rescanned.wait(5000));
    QCOMPARE(proxy.count(), 5);
    QCOMPARE(proxy.folders(),
             QStringList({dir.path(), dir.filePath(QStringLiteral("album"))}));
    proxy.setFolderFilter(dir.path());
    QCOMPARE(proxy.count(), 5);
    proxy.setFolderFilter(dir.filePath(QStringLiteral("album")));
    QCOMPARE(proxy.count(), 1);
    proxy.setFolderFilter(dir.filePath(QStringLiteral("another-folder")));
    QCOMPARE(proxy.count(), 0);
    proxy.setFolderFilter({});
    QCOMPARE(proxy.count(), 5);
    const QString beach = dir.filePath(QStringLiteral("beach.png"));
    proxy.setAlbumFilter(
        QStringLiteral("Weekend"),
        {beach, dir.filePath(QStringLiteral("album/forest.png"))});
    QCOMPARE(proxy.count(), 2);
    QCOMPARE(proxy.adjacentPath(beach, 1),
             dir.filePath(QStringLiteral("album/forest.png")));
    QCOMPARE(proxy.adjacentPath(beach, -1),
             dir.filePath(QStringLiteral("album/forest.png")));
    proxy.setAlbumFilter({}, {});
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
    QCOMPARE(proxy.adjacentPathInFolder(QStringLiteral("/missing.png"), 1),
             QString());
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
    for (const int mode : {CaptureFilterModel::NewestFirst,
                           CaptureFilterModel::OldestFirst,
                           CaptureFilterModel::LargestFirst,
                           CaptureFilterModel::SmallestFirst,
                           CaptureFilterModel::NameAscending}) {
      proxy.setSortMode(mode);
      QStringList actual;
      for (int row = 0; row < proxy.count(); ++row) {
        actual.append(proxy.pathAt(row));
      }
      QCOMPARE(actual, expected);
    }
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
    const QString path =
        dir.filePath(QStringLiteral("screenshot-2026-08-31_10-00-00.png"));
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
    QCOMPARE(CaptureModel::missingMarks({kept, gone}, {}), QStringList{gone});
  }

  void scanSkipsTheRecorderTransients() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto touch = [&](const QString &name) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("x");
    };
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00-preview.png"));
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00-processed.mp4"));

    const auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
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
    QVERIFY(image.save(QDir(album).filePath(QStringLiteral("new-photo.png")),
                       "PNG"));
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1, 5000);
  }

  void uriListIsEncodedAndCrlfTerminated() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    const QString list = model.uriList(
        {QStringLiteral("/tmp/a b.png"), QStringLiteral("/tmp/c#d.mp4")});
    QCOMPARE(list, QStringLiteral(
                       "file:///tmp/a%20b.png\r\nfile:///tmp/c%23d.mp4\r\n"));
  }

  // --- Actions ----------------------------------------------------------

  void registryKeysOnMedium() {
    ActionRegistry registry(nullptr);
    QCOMPARE(registry.primaryActionFor(true), QStringLiteral("trim"));
    QCOMPARE(registry.primaryActionFor(false), QStringLiteral("matte"));
    // The public single-path overload must dispatch to the path-list overload,
    // not resolve its braced argument back to itself and recurse forever.
    QVERIFY(!registry.run(QStringLiteral("favorite"),
                          QStringLiteral("/tmp/not-a-capture")));

    QStringList moving;
    for (const QVariant &row : registry.actionsFor(true)) {
      moving << row.toMap().value(QStringLiteral("id")).toString();
    }
    QVERIFY(moving.contains(QStringLiteral("play")));
    QVERIFY(moving.contains(QStringLiteral("copy")));
    QVERIFY(!moving.contains(QStringLiteral("matte")));
    QVERIFY(!moving.contains(QStringLiteral("ocr")));
  }

  void clipboardFailuresAreReportedAndSecretsStayOutOfHistory() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath =
        qScopeGuard([&] { qputenv("PATH", previousPath); });

    const auto script = [&](const QString &name, const QByteArray &body) {
      QFile file(dir.filePath(name));
      if (!file.open(QIODevice::WriteOnly)) {
        return false;
      }
      file.write("#!/bin/sh\n");
      file.write(body);
      file.close();
      return file.setPermissions(QFileDevice::ReadOwner |
                                 QFileDevice::WriteOwner |
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
             QStringLiteral(
                 "file\n/tmp/first capture.png\n/tmp/second # capture.mp4\n"));
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
    QVERIFY(launcher.runTracked(QStringLiteral("sh"),
                                {QStringLiteral("-c"),
                                 QStringLiteral("printf data > '%1'").arg(saved)},
                                {}, saved));
    QVERIFY(launcher.isPending(saved));
    QCOMPARE(pending.size(), 1);
    QVERIFY(reported.last().at(0).toString().contains(
        QStringLiteral("Making clip-720p.gif")));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
    QCOMPARE(settled.last().at(0).toString(), saved);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QVERIFY(!launcher.isPending(saved));
    QVERIFY(reported.last().at(0).toString().contains(
        QStringLiteral("Saved clip-720p.gif beside the original")));
    QCOMPARE(failed.size(), 0);

    // While a run is in flight, asking again says so rather than lying that
    // the partial file means the job is already done.
    const QString source =
        dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    const QString busy =
        dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00-720p.gif"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"),
         QStringLiteral("sleep 0.3 && printf data > '%1'").arg(busy)},
        {}, busy));
    ActionRegistry registry(&launcher);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QVERIFY(reported.last().at(0).toString().contains(
        QStringLiteral("Still working on")));
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
    QVERIFY2(moved, failed.isEmpty()
                        ? "moveToTrash failed without a message"
                        : qPrintable(failed.last().first().toString()));
    QVERIFY(!QFileInfo::exists(path));

    const QDir trash(m_scratch.filePath(QStringLiteral("data/Trash/files")));
    QVERIFY2(!trash.entryList({QStringLiteral("recover me*")}, QDir::Files)
                  .isEmpty(),
             qPrintable(trash.absolutePath()));

    QVERIFY(!launcher.moveToTrash(path));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(),
             QStringLiteral("That file is no longer there"));
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

    const QImage result = MatteComposer::compose(
        source, MatteComposer::Adaptive, MatteComposer::Original, 0.10);
    QVERIFY(!result.isNull());
    // Padding has a 24 px floor, applied on both sides.
    QCOMPARE(result.width(), 200 + 2 * 24);
    QCOMPARE(result.height(), 100 + 2 * 24);
  }

  void composeNoneReturnsTheOriginal() {
    QImage source(40, 30, QImage::Format_RGB32);
    source.fill(Qt::magenta);
    const QImage result = MatteComposer::compose(source, MatteComposer::None,
                                                 MatteComposer::Square, 0.2);
    QCOMPARE(result.size(), source.size());
  }

  void composeHonoursAspectWithoutCropping() {
    QImage source(100, 400, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(280, 180, 180));

    const QImage result = MatteComposer::compose(source, MatteComposer::Deep,
                                                 MatteComposer::Wide, 0.05);
    QVERIFY(!result.isNull());

    const qreal ratio = qreal(result.width()) / result.height();
    QVERIFY2(qAbs(ratio - 16.0 / 9.0) < 0.02,
             qPrintable(QString::number(ratio)));
    // The capture is tall, so the canvas had to grow sideways rather than crop.
    QVERIFY(result.width() >= source.width());
    QVERIFY(result.height() >= source.height());
  }

  void composeStaysWithinTheOutputBudget() {
    // A very tall capture forced into a wide aspect is the case that could
    // otherwise ask for a multi-gigabyte canvas.
    QImage source(400, 12000, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(90, 180, 180));

    const QImage result = MatteComposer::compose(source, MatteComposer::Slate,
                                                 MatteComposer::Social, 0.05);
    QVERIFY(!result.isNull());
    const qint64 pixels = qint64(result.width()) * result.height();
    QVERIFY2(pixels <= MatteComposer::kMaxOutputPixels,
             qPrintable(QString::number(pixels)));
  }

  void everyMatteProducesSomething() {
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(15, 190, 190));

    for (int matte = 0; matte < MatteComposer::MatteCount; ++matte) {
      const QImage result = MatteComposer::compose(
          source, static_cast<MatteComposer::Matte>(matte),
          MatteComposer::Original, 0.08);
      QVERIFY2(!result.isNull(),
               qPrintable(QStringLiteral("matte %1 was null").arg(matte)));
    }
  }

private:
  QTemporaryDir m_scratch;
};

QTEST_MAIN(OmarollTest)
#include "tst_omaroll.moc"
