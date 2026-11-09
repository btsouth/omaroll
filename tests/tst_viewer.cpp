// Drives the real Viewer.qml, offscreen, with synthetic input.
//
// The library suite covers the grid and its sheets; this covers the window a
// file opens into from the file manager. It has its own executable so its
// window never competes with the library's for focus, and its own disposable
// folder so the natural name order it steps through is known.
//
// Events go through QTest into the offscreen window, never to the live seat.

#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/FileDrag.h"
#include "actions/ExternalEditors.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/HeadlessAudio.h"
#include "app/OpenRequest.h"
#include "app/VideoPlayback.h"
#include "app/VideoPresentation.h"
#include "edit/ImageEditor.h"
#include "edit/JpegTransform.h"
#include "library/MediaInspector.h"
#include "sources/FileVersion.h"
#include "sources/ImageBudget.h"
#include "subtitles/SubtitleIndex.h"
#include "sources/CameraRaw.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/LargeImageProvider.h"
#include "thumbs/RawImageProvider.h"
#include "thumbs/Storyboard.h"
#include "thumbs/ThumbnailCache.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/HyprlandPlacement.h"
#include "viewer/MprisService.h"
#include "viewer/PictureDiagnosis.h"
#include "viewer/ViewerSession.h"
#include "viewer/ViewerWindows.h"

#include <QAccessible>
#include <QBuffer>
#include <QAudioDevice>
#include <QClipboard>
#include <QCryptographicHash>
#include <QMimeData>
#include <QColorSpace>
#include <QPainter>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusVariant>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QJSValue>
#include <cmath>
#include <QQmlListReference>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QPointingDevice>
#include <QProcess>
#include <QAbstractVideoBuffer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QtEndian>
#include <QImage>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>
#include <limits>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

// Twelve seconds of moving pattern with a keyframe every two seconds, so a
// storyboard has several distinct frames. The checked-in fixture has one.
static QString makeStoryboardVideo(const QString& path) {
  const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty()) return {};
  QProcess make;
  make.start(ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                      QStringLiteral("-i"), QStringLiteral("testsrc2=size=320x180:rate=12:duration=12"),
                      QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-q:v"),
                      QStringLiteral("5"), QStringLiteral("-g"), QStringLiteral("24"), path});
  return make.waitForFinished(30000) && make.exitStatus() == QProcess::NormalExit &&
                 make.exitCode() == 0
             ? path
             : QString();
}

// A copy of an MP4 with its moov box overwritten: FFmpeg opens it and finds
// no stream at all, and Qt's player raises no error for it.
static bool writeStreamlessVideo(const QString& source, const QString& path) {
  QFile in(source);
  if (!in.open(QIODevice::ReadOnly)) return false;
  QByteArray bytes = in.readAll();
  const qsizetype at = bytes.indexOf("moov");
  if (at < 4) return false;
  const quint32 size = qFromBigEndian<quint32>(bytes.constData() + at - 4);
  if (size < 8 || at - 4 + qsizetype(size) > bytes.size()) return false;
  for (qsizetype i = at + 4; i < at - 4 + qsizetype(size); ++i) bytes[i] = '\x5a';
  QFile out(path);
  return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
}

// A video buffer that maps a fresh copy every time, as a backend may: what is
// written through a mapping is lost.
class CopyMappedBuffer final : public QAbstractVideoBuffer {
public:
  CopyMappedBuffer(const QVideoFrameFormat& format, QByteArray bytes, int stride)
      : m_format(format), m_bytes(std::move(bytes)), m_stride(stride) {}
  MapData map(QVideoFrame::MapMode) override {
    m_mapped = QByteArray(m_bytes.constData(), m_bytes.size());
    MapData data;
    data.planeCount = 1;
    data.bytesPerLine[0] = m_stride;
    data.data[0] = reinterpret_cast<uchar*>(m_mapped.data());
    data.dataSize[0] = int(m_mapped.size());
    return data;
  }
  QVideoFrameFormat format() const override { return m_format; }

private:
  QVideoFrameFormat m_format;
  QByteArray m_bytes;
  QByteArray m_mapped;
  int m_stride = 0;
};

class StartupPosterProvider final : public QQuickImageProvider {
public:
  StartupPosterProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  // A poster requested as "held" loads only once the test releases it.
  static QSemaphore& held() {
    static QSemaphore semaphore;
    return semaphore;
  }

  QImage requestImage(const QString& id, QSize* size, const QSize&) override {
    if (id == QStringLiteral("held")) held().tryAcquire(1, 10000);
    QImage image(160, 90, QImage::Format_RGB32);
    image.fill(QColor(30, 140, 210));
    *size = image.size();
    return image;
  }
};

// Collects PropertiesChanged and Seeked from the bus, which QSignalSpy cannot
// attach to.
class PropertiesWatcher : public QObject {
  Q_OBJECT

public:
  QList<QVariantMap> changes;
  QList<qlonglong> seeks;

public slots:
  void changed(const QString&, const QVariantMap& values, const QStringList&) {
    changes.append(values);
  }
  void seeked(qlonglong position) { seeks.append(position); }
};

class ViewerTest : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    QVERIFY(m_scratch.isValid());
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch.filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME", m_scratch.filePath(QStringLiteral("data")).toUtf8()));
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_scratch.filePath(QStringLiteral("config")));

    // Real decodable media under names whose natural order is the point:
    // "shot 2" before "shot 10", case folded, documents and other files left
    // out, a dotfile hidden unless it is the one opened.
    const DemoLibrary::Layout demo = DemoLibrary::build();
    m_folder = m_scratch.filePath(QStringLiteral("media"));
    QVERIFY(QDir().mkpath(m_folder));
    const QString picture = demo.pictures + QStringLiteral("/alpine-dawn.jpg");
    const QString wide = demo.pictures + QStringLiteral("/coastal-wave.jpg");
    const QString video = demo.videos + QStringLiteral("/ocean-surface.mp4");
    QVERIFY(QFile::copy(picture, media(QStringLiteral("Shot 1.jpg"))));
    QVERIFY(QFile::copy(wide, media(QStringLiteral("shot 2.jpg"))));
    QVERIFY(QFile::copy(picture, media(QStringLiteral("shot 10.jpg"))));
    QVERIFY(QFile::copy(video, media(QStringLiteral("clip.mp4"))));
    QVERIFY(QFile::copy(picture, media(QStringLiteral(".hidden.jpg"))));
    QVERIFY(QFile::copy(demo.pictures + QStringLiteral("/quiet-horizons-guide.pdf"),
                        media(QStringLiteral("notes.pdf"))));
    QFile text(media(QStringLiteral("readme.txt")));
    QVERIFY(text.open(QIODevice::WriteOnly));
    text.write("not media");
    text.close();
    m_order = {media(QStringLiteral("clip.mp4")), media(QStringLiteral("Shot 1.jpg")),
               media(QStringLiteral("shot 2.jpg")), media(QStringLiteral("shot 10.jpg"))};

    m_theme = new OmarchyTheme(m_scratch.filePath(QStringLiteral("state")),
                               m_scratch.filePath(QStringLiteral("config")), this);
    m_settings = new AppSettings(this);
    m_actions = new ActionLauncher(this);
    m_registry = new ActionRegistry(m_actions, this);
    m_session = new ViewerSession(this);
    m_mediaInfo = new MediaInspector(this);
    m_subtitles = new SubtitleIndex(this);
    // A bus of the test's own, so the player never reaches the desktop's.
    const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    if (!daemon.isEmpty()) {
      m_busDaemon.start(daemon, {QStringLiteral("--session"), QStringLiteral("--nofork"),
                                 QStringLiteral("--nopidfile"), QStringLiteral("--print-address")});
      if (m_busDaemon.waitForReadyRead(5000)) {
        const QString address = QString::fromUtf8(m_busDaemon.readLine()).trimmed();
        QDBusConnection::connectToBus(address, QStringLiteral("omaroll-test-player"));
        QDBusConnection::connectToBus(address, QStringLiteral("omaroll-test-client"));
      }
    }
    m_mpris = new MprisService(m_settings, QDBusConnection(QStringLiteral("omaroll-test-player")), this);

    m_engine = new QQmlEngine(this);
    connect(m_engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError>& warnings) {
      for (const QQmlError& warning : warnings) {
        // The sandbox may have no thumbnailer for the video poster; the
        // player's own frame is what the tests look at.
        // The broken-file tests' pictures fail to decode on purpose.
        if (!warning.toString().contains(QStringLiteral("No thumbnail for "))
            && !warning.toString().contains(QStringLiteral("/broken/a notes.jpg"))
            && !warning.toString().contains(QStringLiteral("/focus/b notes.jpg"))
            // Deleted on purpose while the viewer may still be prefetching it.
            && !warning.toString().contains(QStringLiteral("/a removed sibling.png"))) {
          m_warnings.append(warning.toString());
        }
      }
    });
    m_engine->addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    m_thumbnails = new ThumbnailProvider;
    m_engine->addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), m_thumbnails);
    m_engine->addImageProvider(QStringLiteral("startup-poster"), new StartupPosterProvider);
    m_raws = new RawImageProvider;
    m_engine->addImageProvider(QLatin1String(RawImageProvider::kProviderId), m_raws);
    m_large = new LargeImageProvider;
    m_engine->addImageProvider(QLatin1String(LargeImageProvider::kProviderId), m_large);
    Storyboard::install(*m_engine);

    QQmlContext* shared = m_engine->rootContext();
    shared->setContextProperty(QStringLiteral("Theme"), m_theme);
    shared->setContextProperty(QStringLiteral("Actions"), m_actions);
    shared->setContextProperty(QStringLiteral("Settings"), m_settings);
    shared->setContextProperty(QStringLiteral("Registry"), m_registry);
    shared->setContextProperty(QStringLiteral("Editors"), new ExternalEditors(m_actions, m_engine));
    shared->setContextProperty(QStringLiteral("DemoMode"), true);
    shared->setContextProperty(QStringLiteral("Mpris"), m_mpris);
    // One editor for every viewer, as main.cpp shares it.
    shared->setContextProperty(QStringLiteral("ImageEdit"), new ImageEditor(this));
    // The same split main.cpp makes: the viewer's own services in its context.
    m_context = new QQmlContext(shared, this);
    m_context->setContextProperty(QStringLiteral("Session"), m_session);
    m_context->setContextProperty(QStringLiteral("MediaInfo"), m_mediaInfo);
    m_context->setContextProperty(QStringLiteral("Subtitles"), m_subtitles);

    QQmlComponent component(m_engine);
    component.loadFromModule("Omaroll", "Viewer");
    m_root.reset(component.create(m_context));
    QVERIFY2(m_root, qPrintable(component.errorString()));
    m_window = qobject_cast<QQuickWindow*>(m_root.get());
    QVERIFY(m_window);
    QVERIFY(!m_window->isVisible());
    m_window->resize(1200, 800);
    // Browsing pictures never starts the multimedia backend.
    QVERIFY(!m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer")));
  }

  void cleanupTestCase() {
    m_root.reset();
    if (m_thumbnails) {
      m_thumbnails->shutdown();
    }
    if (m_raws) {
      m_raws->shutdown();
    }
    if (m_large) {
      m_large->shutdown();
    }
    delete m_engine;
    m_engine = nullptr;
    // The storyboards are gone; their workers finish saving before the
    // cache and the pools go away.
    Storyboard::shutdown();
    Storyboard::waitForWorkers();
    delete m_mpris;
    m_mpris = nullptr;
    QDBusConnection::disconnectFromBus(QStringLiteral("omaroll-test-player"));
    QDBusConnection::disconnectFromBus(QStringLiteral("omaroll-test-client"));
    if (m_busDaemon.state() != QProcess::NotRunning) {
      m_busDaemon.kill();
      m_busDaemon.waitForFinished(5000);
    }
    QThreadPool::globalInstance()->waitForDone();
  }

  // Every test starts from a closed viewer.
  void init() {
    if (m_window->isVisible()) {
      m_window->close();
    }
    QTRY_VERIFY(!m_window->isVisible());
    m_window->setWindowState(Qt::WindowNoState);
    m_window->resize(1200, 800);
    m_window->setProperty("chromeTimeout", 2200);
    m_settings->setVideoMuted(false);
    m_settings->setVideoVolume(0.8);
    m_settings->setRememberPlaybackSpeed(false);
    m_settings->setVideoPlaybackRate(1.0);
  }

  void explicitSelectionsPruneMissingFilesBeforeNavigation_data() {
    QTest::addColumn<bool>("jump");
    QTest::addColumn<int>("direction");
    QTest::newRow("next") << false << 1;
    QTest::newRow("previous") << false << -1;
    QTest::newRow("jump") << true << 1;
  }

  void explicitSelectionsPruneMissingFilesBeforeNavigation() {
    QFETCH(bool, jump);
    QFETCH(int, direction);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QStringList paths;
    for (const auto& name : {QStringLiteral("a.jpg"), QStringLiteral("b.jpg"), QStringLiteral("c.jpg")}) {
      paths.append(dir.filePath(name));
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), paths.last()));
    }
    ViewerSession session;
    session.open(paths);
    const int removed = direction < 0 ? 2 : 1;
    QVERIFY(QFile::remove(paths.at(removed)));
    // Navigate before the directory watcher's debounce can run.
    QVERIFY(jump ? session.jump(removed) : session.step(direction));
    QCOMPARE(session.count(), 2);
    QCOMPARE(session.path(), paths.at(direction < 0 ? 1 : 2));
    QVERIFY(QFile::exists(session.path()));
    QVERIFY(QFile::remove(session.path()));
    QVERIFY(session.step(direction));
    QCOMPARE(session.path(), paths.first());
    QCOMPARE(session.count(), 1);
    QVERIFY(!session.step(direction));
    QSignalSpy emptied(&session, &ViewerSession::emptied);
    QVERIFY(QFile::remove(paths.first()));
    QVERIFY(!session.step(direction));
    QCOMPARE(session.count(), 0);
    QCOMPARE(emptied.count(), 1);
  }

  void explicitSelectionsWatchEveryParentWithoutAdmittingNewFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QStringList paths;
    for (int index = 0; index < 3; ++index) {
      const QString folder = dir.filePath(QString::number(index));
      QVERIFY(QDir().mkpath(folder));
      paths.append(folder + QStringLiteral("/selected.jpg"));
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), paths.last()));
    }
    ViewerSession session;
    session.open(paths);
    QVERIFY(QFile::copy(paths.first(), dir.filePath(QStringLiteral("1/new.jpg"))));
    QVERIFY(QFile::remove(paths.at(1)));
    QTRY_COMPARE(session.sequence(), (QStringList{paths.at(0), paths.at(2)}));
    QCOMPARE(session.path(), paths.first());
    const QString version = session.contentVersion();
    const QString staged = dir.filePath(QStringLiteral("0/staged.jpg"));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), staged));
    QVERIFY(QFile::remove(paths.first()));
    QVERIFY(QFile::rename(staged, paths.first()));
    QTRY_VERIFY(session.contentVersion() != version);
    QCOMPARE(session.path(), paths.first());
    QCOMPARE(session.count(), 2);
    QVERIFY(QFile::remove(paths.first()));
    QTRY_COMPARE(session.path(), paths.last());
    QSignalSpy emptied(&session, &ViewerSession::emptied);
    QVERIFY(QFile::remove(paths.last()));
    QTRY_COMPARE(emptied.count(), 1);
    QCOMPARE(session.count(), 0);
  }

  void derivedViewerPosterFollowsItsSourceVersion() {
    QTemporaryDir dir;
    const QString source = dir.filePath(QStringLiteral("clip.mp4"));
    const QString derived = dir.filePath(QStringLiteral("clip-1080p.mp4"));
    const QString fixture = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(QFile::copy(fixture, source));
    QVERIFY(QFile::copy(fixture, derived));
    ViewerSession session;
    session.open({derived});
    const QString own = session.contentVersion();
    const QString poster = session.thumbnailVersion();
    QVERIFY(poster != own);
    QSignalSpy changed(&session, &ViewerSession::currentChanged);
    QTest::qWait(5);
    QFile rewritten(source);
    QVERIFY(rewritten.open(QIODevice::Append));
    rewritten.write("changed source");
    rewritten.close();
    QTRY_VERIFY(session.thumbnailVersion() != poster);
    QCOMPARE(session.contentVersion(), own);
    QVERIFY(!changed.isEmpty());
  }

  void folderRenameFollowsTheCurrentFile_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("sort-later") << QStringLiteral("z.jpg");
    QTest::newRow("sort-earlier") << QStringLiteral("0.jpg");
    QTest::newRow("raw-pair") << QStringLiteral("z.dng");
    QTest::newRow("alias") << QStringLiteral("alias");
  }

  void folderRenameFollowsTheCurrentFile() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const bool raw = kind.endsWith(QLatin1String("dng"));
    if (raw && !CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString current = dir.filePath(raw ? QStringLiteral("b.dng") : QStringLiteral("b.jpg"));
    for (const auto& name : {QStringLiteral("a.jpg"), QStringLiteral("c.jpg")})
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), dir.filePath(name)));
    QVERIFY(QFile::copy(raw ? QFINDTESTDATA("fixtures/raw/camera.dng")
                           : media(QStringLiteral("shot 2.jpg")), current));
    if (raw) QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), dir.filePath(QStringLiteral("b.jpg"))));
    ViewerSession session;
    session.open({current});
    QTRY_COMPARE(session.count(), 3);
    QCOMPARE(session.path(), current);
    const QString version = session.contentVersion();
    QString renamed = dir.filePath(kind == QLatin1String("alias") ? QStringLiteral("z.jpg") : kind);
    if (kind == QLatin1String("alias")) {
      QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("outside"))));
      const QString target = dir.filePath(QStringLiteral("outside/target.jpg"));
      QVERIFY(QFile::rename(current, target));
      QVERIFY(QFile::link(target, renamed));
      renamed = target;
    } else {
      QVERIFY(QFile::rename(current, renamed));
    }
    QTRY_COMPARE(session.path(), renamed);
    QCOMPARE(session.contentVersion(), version);
    QVERIFY(QFile::exists(session.path()));
    if (kind == QLatin1String("alias"))
      QCOMPARE(session.deletionPath(), dir.filePath(QStringLiteral("z.jpg")));
  }

  void ambiguousRenameFallsBackToTheNearestSurvivor() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.jpg"));
    const QString b = dir.filePath(QStringLiteral("b.jpg"));
    const QString c = dir.filePath(QStringLiteral("c.jpg"));
    for (const auto& path : {a, b, c}) QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), path));
    ViewerSession session;
    session.open({b});
    QTRY_COMPARE(session.count(), 3);
    QVERIFY(QFile::rename(b, dir.filePath(QStringLiteral("y.jpg"))));
    QVERIFY(::link(QFile::encodeName(dir.filePath(QStringLiteral("y.jpg"))).constData(),
                   QFile::encodeName(dir.filePath(QStringLiteral("z.jpg"))).constData()) == 0);
    QTRY_COMPARE(session.count(), 4);
    QCOMPARE(session.path(), c);
  }

  void trashOfAnAliasKeepsTheTarget_data() {
    QTest::addColumn<bool>("explicitAlias");
    QTest::addColumn<bool>("outside");
    QTest::newRow("explicit-outside") << true << true;
    QTest::newRow("explicit-inside") << true << false;
    QTest::newRow("sibling-outside") << false << true;
    QTest::newRow("sibling-inside") << false << false;
  }

  void trashOfAnAliasKeepsTheTarget() {
    QFETCH(bool, explicitAlias);
    QFETCH(bool, outside);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("original"))));
    const QString target = dir.filePath(outside ? QStringLiteral("original/target.jpg")
                                              : QStringLiteral("z.jpg"));
    const QString alias = dir.filePath(QStringLiteral("a.jpg"));
    const QString sibling = dir.filePath(QStringLiteral("b.jpg"));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), target));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), sibling));
    QVERIFY(QFile::link(target, alias));
    if (explicitAlias) {
      const OpenRequest request = OpenRequest::fromPaths({alias});
      open(request.files);
      m_session->setDeletionPaths(request.entryPaths);
      QTRY_VERIFY(m_session->sequenceRevision() >= 2);
    } else {
      open({sibling});
      QTRY_COMPARE(m_session->count(), 2);
      QVERIFY(m_session->jump(0));
    }
    QCOMPARE(m_session->path(), target);
    QCOMPARE(m_session->deletionPath(), alias);
    QTest::keyClick(m_window, Qt::Key_Delete);
    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    QTRY_VERIFY(confirm->isVisible());
    QCOMPARE(confirm->property("path").toString(), alias);
    QCOMPARE(confirm->property("detail").toString(), alias);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFileInfo(alias).isSymLink());
    QVERIFY(QFile::exists(target));
    QVERIFY(m_session->path() != target);
  }

  void slideshowDialogsHoldTheTargetAndResume_data() {
    QTest::addColumn<QString>("dialogName");
    QTest::addColumn<bool>("accept");
    QTest::newRow("trash-cancel") << QStringLiteral("viewerConfirm") << false;
    QTest::newRow("trash-accept") << QStringLiteral("viewerConfirm") << true;
    QTest::newRow("permanent-cancel") << QStringLiteral("viewerPermanentConfirm") << false;
    QTest::newRow("permanent-accept") << QStringLiteral("viewerPermanentConfirm") << true;
    QTest::newRow("raw-cancel") << QStringLiteral("editorChooser") << false;
  }

  void slideshowDialogsHoldTheTargetAndResume() {
    QFETCH(QString, dialogName);
    QFETCH(bool, accept);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const bool raw = dialogName == QLatin1String("editorChooser");
    if (raw && !CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString first = dir.filePath(raw ? QStringLiteral("a.dng") : QStringLiteral("a.jpg"));
    const QString second = dir.filePath(QStringLiteral("b.jpg"));
    QVERIFY(QFile::copy(raw ? QFINDTESTDATA("fixtures/raw/camera.dng")
                           : media(QStringLiteral("Shot 1.jpg")), first));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), second));
    const int interval = m_settings->slideshowIntervalSeconds();
    const bool shuffle = m_settings->slideshowShuffle();
    const auto restore = qScopeGuard([&] {
      m_settings->setSlideshowIntervalSeconds(interval);
      m_settings->setSlideshowShuffle(shuffle);
    });
    m_settings->setSlideshowIntervalSeconds(2);
    m_settings->setSlideshowShuffle(false);
    open({first, second});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_F5);
    if (raw) QTest::keyClick(m_window, Qt::Key_D, Qt::ShiftModifier);
    else QTest::keyClick(m_window, Qt::Key_Delete, dialogName == QLatin1String("viewerPermanentConfirm")
                                               ? Qt::ShiftModifier : Qt::NoModifier);
    QObject* dialog = m_window->findChild<QObject*>(dialogName);
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QSignalSpy changed(m_session, &ViewerSession::currentChanged);
    QTest::qWait(2300);
    QVERIFY(QMetaObject::invokeMethod(m_window, "advanceSlideshow"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(m_session->path(), first);
    QTest::keyClick(m_window, accept ? Qt::Key_Return : Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(m_session->path(), second, 4000);
    if (accept) QVERIFY(!QFile::exists(first));
    else QVERIFY(QFile::exists(first));
    QVERIFY(prop("slideshowRunning").toBool());
    QTRY_VERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());
  }

  void slideshowTrashHoldsAVideoThroughEndOfMedia_data() {
    QTest::addColumn<bool>("accept");
    QTest::newRow("cancel") << false;
    QTest::newRow("accept") << true;
  }

  void slideshowTrashHoldsAVideoThroughEndOfMedia() {
    QFETCH(bool, accept);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath(QStringLiteral("clip.mp4"));
    const QString picture = dir.filePath(QStringLiteral("picture.jpg"));
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), clip));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), picture));
    const bool videos = m_settings->slideshowVideos(), shuffle = m_settings->slideshowShuffle();
    const int interval = m_settings->slideshowIntervalSeconds();
    const auto restore = qScopeGuard([&] {
      m_settings->setSlideshowVideos(videos);
      m_settings->setSlideshowShuffle(shuffle);
      m_settings->setSlideshowIntervalSeconds(interval);
    });
    m_settings->setSlideshowVideos(true);
    m_settings->setSlideshowShuffle(false);
    m_settings->setSlideshowIntervalSeconds(2);
    open({clip, picture});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    QTest::keyClick(m_window, Qt::Key_F5);
    QTest::keyClick(m_window, Qt::Key_Delete);
    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    QTRY_VERIFY(confirm->isVisible());
    // Let the decoder reach EOF while the confirmation owns its target.
    player->setPosition(std::max<qint64>(0, player->duration() - 100));
    QTRY_COMPARE(player->mediaStatus(), QMediaPlayer::EndOfMedia);
    QCOMPARE(m_session->path(), clip);
    QVERIFY(confirm->isVisible());
    QTest::keyClick(m_window, accept ? Qt::Key_Return : Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(m_session->path(), picture, 4000);
    QCOMPARE(QFile::exists(clip), !accept);
    QVERIFY(prop("slideshowRunning").toBool());
  }

  void manualSlideshowNavigationSkipsExcludedVideos_data() {
    QTest::addColumn<bool>("shuffle");
    QTest::newRow("ordered") << false;
    QTest::newRow("shuffled") << true;
  }

  void manualSlideshowNavigationSkipsExcludedVideos() {
    QFETCH(bool, shuffle);
    const bool videos = m_settings->slideshowVideos(), oldShuffle = m_settings->slideshowShuffle();
    const auto restore = qScopeGuard([&] {
      m_settings->setSlideshowVideos(videos);
      m_settings->setSlideshowShuffle(oldShuffle);
    });
    m_settings->setSlideshowVideos(false);
    m_settings->setSlideshowShuffle(shuffle);
    const QString first = media(QStringLiteral("Shot 1.jpg"));
    const QString second = media(QStringLiteral("shot 2.jpg"));
    const QString clip = media(QStringLiteral("clip.mp4"));
    open({first, clip, second});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_F5);
    QSignalSpy changed(m_session, &ViewerSession::currentChanged);
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), second);
    QCOMPARE(changed.count(), 1);
    QTest::keyClick(m_window, Qt::Key_Left);
    QCOMPARE(m_session->path(), first);
    QCOMPARE(changed.count(), 2);
    // With one eligible image, manual navigation keeps it on screen.
    QVERIFY(m_session->jump(0));
    m_session->forget(second);
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), first);
    QVERIFY(prop("slideshowRunning").toBool());
  }

  // WCAG relative luminance and contrast, for the checks below.
  static double luminance(const QColor& colour) {
    const auto channel = [](double value) {
      return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) +
           0.0722 * channel(colour.blueF());
  }
  static double contrast(const QColor& a, const QColor& b) {
    const double first = luminance(a), second = luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
  }
  // A translucent colour over an opaque one.
  static QColor over(const QColor& top, const QColor& bottom) {
    const double a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + bottom.redF() * (1 - a), top.greenF() * a + bottom.greenF() * (1 - a),
                            top.blueF() * a + bottom.blueF() * (1 - a));
  }

  void controlsOverMediaStayDarkInEveryTheme_data() {
    QTest::addColumn<QString>("palette");
    QTest::newRow("dark") << QStringLiteral("dark");
    QTest::newRow("light") << QStringLiteral("light");
  }

  void controlsOverMediaStayDarkInEveryTheme() {
    QFETCH(QString, palette);
    QTemporaryDir themeDir;
    QVERIFY(themeDir.isValid());
    const QString themeRoot = themeDir.filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themeRoot));
    const QString fixture = QFINDTESTDATA("fixtures/themes") + QLatin1Char('/') + palette;
    for (const auto& name : {QStringLiteral("colors.toml"), QStringLiteral("shell.toml")})
      QVERIFY(QFile::copy(fixture + QStringLiteral("/theme/") + name, themeRoot + QLatin1Char('/') + name));
    OmarchyTheme theme(themeDir.path(), themeDir.path());
    m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    const auto restoreTheme = qScopeGuard([&] {
      m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), m_theme);
    });
    if (palette == QStringLiteral("light")) QVERIFY(luminance(theme.background()) > 0.6);

    // The worst case under a panel: a white frame.
    const QColor white(Qt::white);
    open({media(QStringLiteral("clip.mp4"))});
    QQuickItem* transport = item(QStringLiteral("viewerTransport"));
    QTRY_VERIFY(transport->isVisible());
    const QColor panel = transport->property("color").value<QColor>();
    const QColor backing = over(panel, white);
    QVERIFY2(luminance(backing) < 0.08, qPrintable(panel.name(QColor::HexArgb)));
    // Time text, the seek track and its fill clear WCAG AA on that.
    for (const char* name : {"viewerElapsed", "viewerDuration"}) {
      const QColor text = item(QString::fromLatin1(name))->property("color").value<QColor>();
      QVERIFY2(contrast(text, backing) >= 4.5, name);
    }
    const QColor track = over(item(QStringLiteral("viewerSeekTrack"))->property("color").value<QColor>(), backing);
    QVERIFY2(contrast(track, backing) >= 3.0, qPrintable(QString::number(contrast(track, backing))));
    const QColor fill = item(QStringLiteral("viewerSeekFill"))->property("color").value<QColor>();
    QVERIFY2(contrast(fill, backing) >= 4.5, qPrintable(fill.name()));
    // Icons are light on it too, whatever the theme's foreground.
    QQuickItem* play = item(QStringLiteral("viewerPlayButton"));
    QVERIFY(play->property("osd").toBool());
    QVERIFY(contrast(play->property("textColour").value<QColor>(), backing) >= 4.5);

    // The top scrim is dark, never the theme's light surface.
    QQuickItem* scrim = item(QStringLiteral("viewerHeaderScrim"));
    QVariant gradientValue = scrim->property("gradient");
    QObject* gradient = gradientValue.value<QObject*>();
    if (!gradient) gradient = gradientValue.value<QJSValue>().toQObject();
    QVERIFY(gradient);
    QQmlListReference stops(gradient, "stops");
    QVERIFY(stops.count() >= 2);
    const QColor top = stops.at(0)->property("color").value<QColor>();
    QVERIFY2(luminance(over(top, white)) < 0.15, qPrintable(top.name(QColor::HexArgb)));
    QVERIFY(contrast(item(QStringLiteral("viewerTitle"))->property("color").value<QColor>(), over(top, white)) >= 4.5);

    // The picture toolbar matches the video bar.
    open({media(QStringLiteral("Shot 1.jpg"))});
    QQuickItem* toolbar = item(QStringLiteral("viewerToolbar"));
    QTRY_VERIFY(toolbar->isVisible());
    QCOMPARE(toolbar->property("color").value<QColor>(), panel);
    QVERIFY(item(QStringLiteral("viewerZoomIn"))->property("osd").toBool());
    // A light theme's pale surround gets a hairline round the picture.
    QTRY_VERIFY(prop("imageReady").toBool());
    QCOMPARE(item(QStringLiteral("viewerPictureEdge"))->isVisible(), luminance(theme.surfaceBackground()) > 0.4);
  }

  void shortcutHelpFitsALaptopViewer_data() {
    QTest::addColumn<bool>("video");
    QTest::newRow("picture") << false;
    QTest::newRow("video") << true;
  }

  void shortcutHelpFitsALaptopViewer() {
    QFETCH(bool, video);
    open({media(video ? QStringLiteral("clip.mp4") : QStringLiteral("Shot 1.jpg"))});
    const QSize before = m_window->size();
    const auto restore = qScopeGuard([&] { m_window->resize(before); });
    m_window->resize(1280, 720);
    QTRY_COMPARE(m_window->size(), QSize(1280, 720));
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("viewerShortcutHelp"));
    QVERIFY(help);
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    QQuickItem* flick = item(QStringLiteral("viewerShortcutScroll"))->property("contentItem").value<QQuickItem*>();
    QVERIFY(flick);
    // Everything shows without scrolling, inside the window.
    QTRY_VERIFY(flick->property("contentHeight").toReal() <= flick->height() + 0.5);
    const qreal fullHeight = help->property("height").toReal();
    QVERIFY(fullHeight <= 720 - 24);
    QVERIFY(!item(QStringLiteral("shortcutHelpFade"))->isVisible());

    // One row per shortcut, label left and caps right, one cap per key, with
    // the pan arrows collapsed.
    auto* content = help->property("contentItem").value<QQuickItem*>();
    if (!video) {
      QQuickItem* pan = find(content, [](QQuickItem* candidate) {
        return candidate->property("text").toString() ==
               QStringLiteral("Shift+Left / Shift+Right / Shift+Up / Shift+Down");
      });
      QVERIFY(pan);
      const QVariantList caps = pan->property("modelData").toMap().value(QStringLiteral("caps")).toList();
      QCOMPARE(caps.size(), 1);
      QCOMPARE(caps.first().toStringList(), (QStringList{QStringLiteral("Shift"), QStringLiteral("Arrows")}));
      QVERIFY(!pan->property("stacked").toBool());
      // The RAW entries are actions now.
      QVariant groups = help->property("groups");
      bool rawInActions = false;
      for (const QVariant& group : groups.value<QJSValue>().toVariant().toList()) {
        const QVariantMap map = group.toMap();
        for (const QVariant& row : map.value(QStringLiteral("rows")).toList()) {
          if (row.toMap().value(QStringLiteral("label")).toString() == QStringLiteral("Open RAW editor"))
            rawInActions = map.value(QStringLiteral("title")).toString() == QStringLiteral("Actions");
        }
      }
      QVERIFY(rawInActions);
    }

    // A filter down to a few rows shrinks the sheet.
    help->setProperty("filter", QStringLiteral("trash"));
    QTRY_VERIFY(help->property("height").toReal() < fullHeight / 2);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!help->property("visible").toBool());
  }

  void detailsStayInsideAShortWindowAndScrollToTheFolder_data() {
    QTest::addColumn<QString>("palette");
    QTest::newRow("dark") << QStringLiteral("dark");
    QTest::newRow("light") << QStringLiteral("light");
  }

  void detailsStayInsideAShortWindowAndScrollToTheFolder() {
    QFETCH(QString, palette);
    QTemporaryDir themeDir;
    QVERIFY(themeDir.isValid());
    const QString themeRoot = themeDir.filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themeRoot));
    const QString fixture = QFINDTESTDATA("fixtures/themes") + QLatin1Char('/') + palette;
    for (const auto& name : {QStringLiteral("colors.toml"), QStringLiteral("shell.toml")})
      QVERIFY(QFile::copy(fixture + QStringLiteral("/theme/") + name, themeRoot + QLatin1Char('/') + name));
    OmarchyTheme theme(themeDir.path(), themeDir.path());
    m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    const auto restoreTheme = qScopeGuard([&] {
      m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), m_theme);
    });
    const QSize before = m_window->size();
    const auto restore = qScopeGuard([&] { m_window->resize(before); });
    open({media(QStringLiteral("Shot 1.jpg"))});
    m_window->resize(320, 240);
    QTest::keyClick(m_window, Qt::Key_I);
    QQuickItem* panel = item(QStringLiteral("viewerInfoPanel"));
    QVariantList lines;
    for (int index = 0; index < 30; ++index) lines.append(QStringLiteral("Long metadata line %1").arg(index));
    // Break this test instance's async metadata binding for deterministic size.
    QVERIFY(QQmlProperty(panel, QStringLiteral("lines")).write(lines));
    QTRY_VERIFY(panel->height() > 100);
    QVERIFY(panel->y() + panel->height() <= m_window->height() - 16);
    QQuickItem* flick = find(panel, [](QQuickItem* candidate) {
      return candidate->property("contentHeight").isValid();
    });
    QVERIFY(flick);
    QTRY_VERIFY(flick->property("contentHeight").toReal() > flick->height());
    flick->setProperty("contentY", flick->property("contentHeight").toReal() - flick->height());
    QQuickItem* folder = item(QStringLiteral("viewerInfoFolder"));
    QTRY_VERIFY(folder->mapToScene(QPointF(0, folder->height())).y() <= m_window->height() - 16);
    QVERIFY(folder->mapToScene(QPointF()).y() >= panel->y() + 16);
  }

  void picturesReloadAfterSavesAndKeepTheirViewWhenSiblingsChange() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("z.png"));
    QImage initial(40, 30, QImage::Format_RGB32);
    initial.fill(Qt::red);
    QVERIFY(initial.save(path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTRY_COMPARE(prop("sourceWidth").toInt(), 40);
    const QString version = m_session->contentVersion();
    QImage updated(80, 60, QImage::Format_RGB32);
    updated.fill(Qt::blue);
    QVERIFY(updated.save(path));
    QTRY_VERIFY_WITH_TIMEOUT(m_session->contentVersion() != version, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(prop("sourceWidth").toInt(), 80, 5000);
    const QString temporary = dir.filePath(QStringLiteral("replacement.png"));
    QImage replacement(120, 90, QImage::Format_RGB32);
    replacement.fill(Qt::green);
    QVERIFY(replacement.save(temporary));
    QVERIFY(QFile::remove(path));
    QVERIFY(QFile::rename(temporary, path));
    QTRY_COMPARE_WITH_TIMEOUT(prop("sourceWidth").toInt(), 120, 5000);

    QTest::keyClick(m_window, Qt::Key_R);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(!prop("zooming").toBool());
    const qreal scale = prop("viewScale").toReal();
    const int rotation = prop("viewRotation").toInt();
    const QString sibling = dir.filePath(QStringLiteral("a removed sibling.png"));
    QVERIFY(initial.save(sibling));
    QTRY_COMPARE_WITH_TIMEOUT(m_session->count(), 2, 5000);
    QCOMPARE(m_session->path(), path);
    QCOMPARE(prop("viewScale").toReal(), scale);
    QCOMPARE(prop("viewRotation").toInt(), rotation);
    QVERIFY(QFile::remove(sibling));
    QTRY_COMPARE_WITH_TIMEOUT(m_session->count(), 1, 5000);
    QCOMPARE(prop("viewScale").toReal(), scale);
    QCOMPARE(prop("viewRotation").toInt(), rotation);
    m_window->close();
  }

  void neighbourPreloadsShareADecodedPixelBudget_data() {
    QTest::addColumn<QSize>("nextSize");
    QTest::addColumn<QSize>("previousSize");
    QTest::addColumn<bool>("nextAllowed");
    QTest::addColumn<bool>("previousAllowed");
    QTest::newRow("small-pair") << QSize(400, 300) << QSize(800, 600) << true << true;
    QTest::newRow("exact-budget") << QSize(4096, 4096) << QSize(1, 1) << true << false;
    QTest::newRow("over-budget") << QSize(4096, 4097) << QSize(1, 1) << false << true;
    QTest::newRow("combined-budget") << QSize(3072, 3072) << QSize(3072, 3072) << true << false;
    QTest::newRow("large-first-keeps-small-second") << QSize(6000, 4000) << QSize(400, 300)
                                                   << false << true;
    QTest::newRow("hostile-dimensions") << QSize(2147483647, 2147483647) << QSize(400, 300)
                                       << false << true;
  }

  void neighbourPreloadsShareADecodedPixelBudget() {
    QFETCH(QSize, nextSize);
    QFETCH(QSize, previousSize);
    QFETCH(bool, nextAllowed);
    QFETCH(bool, previousAllowed);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString current = media(QStringLiteral("Shot 1.jpg"));
    const QString next = dir.filePath(QStringLiteral("next.bmp"));
    const QString previous = dir.filePath(QStringLiteral("previous.bmp"));
    // Headers alone exercise admission without allocating large decoded images.
    QVERIFY(writeBitmapHeader(next, nextSize));
    QVERIFY(writeBitmapHeader(previous, previousSize));
    QCOMPARE(QImageReader(previous).size(), previousSize);
    ViewerSession session;
    session.open({current, next, previous});
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::sendPostedEvents();
    QCOMPARE(!session.nextPreloadUrl().isEmpty(), nextAllowed);
    QCOMPARE(!session.previousPreloadUrl().isEmpty(), previousAllowed);
    if (nextAllowed) QCOMPARE(session.nextPreloadUrl(), session.neighbourImageUrl(1));
    if (previousAllowed) QCOMPARE(session.previousPreloadUrl(), session.neighbourImageUrl(-1));
  }

  void neighbourPreloadsSkipAnimationVideoAndUnknownDimensions_data() {
    QTest::addColumn<QString>("fixture");
    QTest::newRow("animation") << QStringLiteral("animated.gif");
    QTest::newRow("animated-webp") << QStringLiteral("animated.webp");
    QTest::newRow("still-webp-uses-animation-viewer") << QStringLiteral("still.webp");
    QTest::newRow("video") << QStringLiteral("tracks.mkv");
    QTest::newRow("unknown-header") << QString();
  }

  void neighbourPreloadsAccountForDecoderDepth_data() {
    QTest::addColumn<QImage::Format>("format");
    QTest::addColumn<QByteArray>("nextCodec");
    QTest::addColumn<QByteArray>("previousCodec");
    QTest::addColumn<int>("depth");
    QTest::addColumn<bool>("previousAllowed");
    QTest::newRow("jpeg-and-png-fit-together") << QImage::Format_RGB32 << QByteArray("JPG")
                                             << QByteArray("PNG") << 32 << true;
    QTest::newRow("rgba64-pair-exceeds-budget") << QImage::Format_RGBA64 << QByteArray("PNG")
                                               << QByteArray("PNG") << 64 << false;
  }

  void neighbourPreloadsAccountForDecoderDepth() {
    QFETCH(QImage::Format, format);
    QFETCH(QByteArray, nextCodec);
    QFETCH(QByteArray, previousCodec);
    QFETCH(int, depth);
    QFETCH(bool, previousAllowed);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString next = dir.filePath(QStringLiteral("next.") + QString::fromLatin1(nextCodec));
    const QString previous = dir.filePath(QStringLiteral("previous.") + QString::fromLatin1(previousCodec));
    {
      // Within the screen's copy, so shown whole: 17 MB at 32 bits, 35 MB at
      // 64 bits. Release the fixture writer's storage before probing the
      // viewer's own policy.
      QImage image(2560, 1700, format);
      image.fill(Qt::red);
      QVERIFY(image.save(next, nextCodec.constData()));
      QVERIFY(image.save(previous, previousCodec.constData()));
    }
    QCOMPARE(QImage::toPixelFormat(QImageReader(next).imageFormat()).bitsPerPixel(), depth);
    ViewerSession session;
    session.open({media(QStringLiteral("Shot 1.jpg")), next, previous});
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::sendPostedEvents();
    QCOMPARE(session.nextPreloadUrl().toLocalFile(), next);
    QCOMPARE(!session.previousPreloadUrl().isEmpty(), previousAllowed);
  }

  // Photos larger than the screen are preloaded as the copies the viewer
  // shows, so two fit the budget that one whole decode would fill.
  void neighbourPreloadsAreTheScreensCopies() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString next = dir.filePath(QStringLiteral("next.jpg"));
    const QString previous = dir.filePath(QStringLiteral("previous.jpg"));
    {
      // 48 MB each whole; their copies decode at 2500 x 1875.
      QImage image(4000, 3000, QImage::Format_RGB32);
      image.fill(QColor(200, 120, 40));
      QVERIFY(image.save(next, nullptr, 85));
      QVERIFY(image.save(previous, nullptr, 85));
    }
    ViewerSession session;
    session.open({media(QStringLiteral("Shot 1.jpg")), next, previous});
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::sendPostedEvents();
    const QString prefix = QStringLiteral("image://large/");
    QVERIFY(session.nextPreloadUrl().toString().startsWith(prefix));
    QVERIFY(session.previousPreloadUrl().toString().startsWith(prefix));
    QCOMPARE(session.nextPreloadUrl(), session.neighbourImageUrl(1));
    QCOMPARE(session.previousPreloadUrl(), session.neighbourImageUrl(-1));
    // Stepping on shows the copy that was preloaded.
    const QUrl preloaded = session.nextPreloadUrl();
    QVERIFY(session.step(1));
    QCOMPARE(session.imageUrl(), preloaded);
  }

  void neighbourPreloadsSkipAnimationVideoAndUnknownDimensions() {
    QFETCH(QString, fixture);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Extensionless media also requires classification on the worker.
    const QString next = dir.filePath(fixture.isEmpty() ? QStringLiteral("broken.png")
                                                       : QStringLiteral("without extension"));
    if (fixture.isEmpty()) {
      QFile file(next);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write("not an image"), qint64(12));
    } else {
      const QString source = QFINDTESTDATA(qPrintable("fixtures/viewer/" + fixture));
      QVERIFY(!source.isEmpty());
      QVERIFY(QFile::copy(source, next));
    }
    ViewerSession session;
    const QString previous = media(QStringLiteral("shot 2.jpg"));
    session.open({media(QStringLiteral("Shot 1.jpg")), next, previous});
    QTRY_COMPARE(session.previousPreloadUrl().toLocalFile(), previous);
    QVERIFY(session.nextPreloadUrl().isEmpty());
  }

  void wrappedNeighbourPreloadsAreUniqueAndExcludeTheCurrentPath() {
    ViewerSession session;
    const QString a = media(QStringLiteral("Shot 1.jpg"));
    const QString b = media(QStringLiteral("shot 2.jpg"));
    session.open({a, b});
    QTRY_COMPARE(session.nextPreloadUrl().toLocalFile(), b);
    QVERIFY(session.previousPreloadUrl().isEmpty());
    session.open({a, a, b});
    QTRY_COMPARE(session.previousPreloadUrl().toLocalFile(), b);
    QVERIFY(session.nextPreloadUrl().isEmpty());
    session.open({a, a});
    QVERIFY(session.nextPreloadUrl().isEmpty());
    QVERIFY(session.previousPreloadUrl().isEmpty());
    session.clear();
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::sendPostedEvents();
    QVERIFY(session.nextPreloadUrl().isEmpty());
    QVERIFY(session.previousPreloadUrl().isEmpty());
  }

  void replacedNeighbourCannotUseAnOldHeaderApproval() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString next = dir.filePath(QStringLiteral("next.bmp"));
    QVERIFY(writeBitmapHeader(next, QSize(40, 30)));
    ViewerSession session;
    const QString previous = media(QStringLiteral("shot 2.jpg"));
    const QStringList paths{media(QStringLiteral("Shot 1.jpg")), next, previous};
    session.open(paths);
    // Finish the probe without dispatching its GUI callback, then atomically
    // replace the small neighbour with one that would exceed the budget.
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    const QString staged = dir.filePath(QStringLiteral("staged.bmp"));
    QVERIFY(writeBitmapHeader(staged, QSize(6000, 4000)));
    const QString oldVersion = FileVersion::key(next);
    QVERIFY(QFile::remove(next));
    QVERIFY(QFile::rename(staged, next));
    QVERIFY(FileVersion::key(next) != oldVersion);
    QTRY_COMPARE(session.previousPreloadUrl().toLocalFile(), previous);
    QVERIFY(session.nextPreloadUrl().isEmpty());
    // Reopening an unchanged sequence must probe a newly saved version too.
    QVERIFY(writeBitmapHeader(next, QSize(80, 60)));
    session.open(paths);
    QTRY_COMPARE(session.nextPreloadUrl(), session.neighbourImageUrl(1));
    QVERIFY(session.nextPreloadUrl().query().contains(FileVersion::key(next)));
  }

  void obsoleteNeighbourResultsCannotSurviveNavigationOrClear() {
    ViewerSession session;
    const QString a = media(QStringLiteral("Shot 1.jpg"));
    const QString b = media(QStringLiteral("shot 2.jpg"));
    const QString c = media(QStringLiteral("shot 10.jpg"));
    const QString d = media(QStringLiteral(".hidden.jpg"));
    const QStringList paths{a, b, c, d};
    session.open(paths);
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    // Several requests arrive while a finished older probe awaits delivery.
    for (int i = 0; i < 100; ++i) QVERIFY(session.jump(i % paths.size()));
    QCOMPARE(session.path(), d);
    bool obsoletePublished = false;
    connect(&session, &ViewerSession::preloadsChanged, this, [&] {
      obsoletePublished |= session.nextPreloadUrl().toLocalFile() == b ||
                           session.previousPreloadUrl().toLocalFile() == d;
    });
    QTRY_COMPARE(session.nextPreloadUrl().toLocalFile(), a);
    QTRY_COMPARE(session.previousPreloadUrl().toLocalFile(), c);
    QVERIFY(!obsoletePublished);
    session.open(paths);
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    session.clear();
    QSignalSpy changed(&session, &ViewerSession::preloadsChanged);
    QCoreApplication::sendPostedEvents();
    QCOMPARE(changed.size(), 0);
    QVERIFY(session.nextPreloadUrl().isEmpty());
    QVERIFY(session.previousPreloadUrl().isEmpty());
  }

  void neighbourPreloadsFollowSequenceRefreshesWithTheSameCurrentFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.png"));
    const QString b = dir.filePath(QStringLiteral("b.png"));
    const QString z = dir.filePath(QStringLiteral("z.png"));
    QImage image(40, 30, QImage::Format_RGBA64);
    image.fill(Qt::red);
    QVERIFY(image.save(a));
    QVERIFY(image.save(z));
    ViewerSession session;
    session.open({a});
    QTRY_COMPARE(session.count(), 2);
    QTRY_COMPARE(session.nextPreloadUrl().toLocalFile(), z);
    QVERIFY(session.previousPreloadUrl().isEmpty());
    QSignalSpy current(&session, &ViewerSession::currentChanged);
    QVERIFY(image.save(b));
    QTRY_COMPARE_WITH_TIMEOUT(session.count(), 3, 5000);
    QTRY_COMPARE(session.nextPreloadUrl().toLocalFile(), b);
    QTRY_COMPARE(session.previousPreloadUrl().toLocalFile(), z);
    QVERIFY(QFile::remove(b));
    QTRY_COMPARE_WITH_TIMEOUT(session.count(), 2, 5000);
    QTRY_COMPARE(session.nextPreloadUrl().toLocalFile(), z);
    QVERIFY(session.previousPreloadUrl().isEmpty());
    const QUrl oldUrl = session.nextPreloadUrl();
    const QString staged = dir.filePath(QStringLiteral(".replacement"));
    QVERIFY(image.save(staged, "PNG"));
    QVERIFY(QFile::remove(z));
    QVERIFY(QFile::rename(staged, z));
    QTRY_VERIFY_WITH_TIMEOUT(session.nextPreloadUrl() != oldUrl, 5000);
    QTRY_COMPARE(session.nextPreloadUrl(), session.neighbourImageUrl(1));
    QCOMPARE(session.path(), a);
    QCOMPARE(current.size(), 0);
  }

  void smallPreloadsReuseOriginalResolutionWhenStepping() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.png"));
    const QString b = dir.filePath(QStringLiteral("b.png"));
    QImage first(400, 300, QImage::Format_RGBA64);
    first.fill(Qt::red);
    QImage second(480, 360, QImage::Format_RGBA64);
    second.fill(Qt::blue);
    QVERIFY(first.save(a));
    QVERIFY(second.save(b));
    open({a, b});
    QTRY_VERIFY(prop("imageReady").toBool());
    QQuickItem* prefetch = item(QStringLiteral("viewerPrefetch1"));
    QTRY_COMPARE(prefetch->property("source").toUrl(), m_session->neighbourImageUrl(1));
    QTRY_COMPARE(prefetch->property("status").toInt(), 1); // Image.Ready
    QCOMPARE(prefetch->implicitWidth(), 480.0);
    QCOMPARE(prefetch->implicitHeight(), 360.0);
    QVERIFY(item(QStringLiteral("viewerPrefetch-1"))->property("source").toUrl().isEmpty());
    QTest::keyClick(m_window, Qt::Key_Right);
    QQuickItem* displayed = item(QStringLiteral("viewerImage"));
    QCOMPARE(displayed->property("status").toInt(), 1);
    QVERIFY(!displayed->property("waited").toBool());
    QCOMPARE(prop("sourceWidth").toInt(), 480);
    QCOMPARE(prop("sourceHeight").toInt(), 360);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(!prop("zooming").toBool());
    QCOMPARE(prop("effectiveScale").toReal(), 1.0 / m_window->devicePixelRatio());
  }

  void siblingSymlinksUseTheLibraryIdentity() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.png"));
    const QString target = dir.filePath(QStringLiteral("z.png"));
    const QString alias = dir.filePath(QStringLiteral("b.png"));
    QImage image(40, 30, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(first));
    QVERIFY(image.save(target));
    QVERIFY(QFile::link(target, alias));
    open({first});
    QTRY_COMPARE(m_session->count(), 2);
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), target);
    QTest::keyClick(m_window, Qt::Key_V);
    QVERIFY(m_settings->isFavorite(target));
    QVERIFY(!m_settings->isFavorite(alias));
    QSignalSpy requested(m_session, &ViewerSession::libraryRequested);
    QTest::keyClick(m_window, Qt::Key_Return);
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().first().toString(), target);
    m_settings->setFavorite({target}, false);
    m_window->close();
  }

  // A slow thumbnail must not hold back the player: both load side by side.
  // The poster stays loading until released, and the player has to exist
  // after a few event loop turns, before any fallback timer could expire.
  void videoStartsWithoutWaitingForThePoster() {
    ViewerSession session;
    QQmlContext context(m_context);
    context.setContextProperty(QStringLiteral("Session"), &session);
    QQmlComponent component(m_engine);
    component.loadFromModule("Omaroll", "Viewer");
    std::unique_ptr<QObject> root(component.create(&context));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    QVERIFY(window);
    auto* poster = find(window->contentItem(), [](QQuickItem* item) {
      return item->objectName() == QStringLiteral("viewerVideoPoster");
    });
    QVERIFY(poster);
    // Release the poster before the window goes, so its reader is not blocked.
    const auto cleanup = qScopeGuard([window] {
      StartupPosterProvider::held().release();
      window->hide();
    });
    // Remove the normal source binding so session changes cannot replace the
    // controlled thumbnail with a real decoder request.
    QVERIFY(QQmlProperty::write(poster, QStringLiteral("source"),
                                QUrl(QStringLiteral("image://startup-poster/held"))));
    session.open({media(QStringLiteral("clip.mp4"))});
    window->show();
    QMediaPlayer* player = nullptr;
    for (int turn = 0; turn < 10 && !player; ++turn) {
      QCoreApplication::processEvents();
      player = window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"));
    }
    QVERIFY2(player, "The player waited for the poster to load");
    QCOMPARE(poster->property("status").toInt(), 2); // Image.Loading
    // A poster on its way counts as media: the header keeps its scrim.
    QVERIFY(root->property("stageShowsMedia").toBool());
    StartupPosterProvider::held().release();
    QTRY_COMPARE_WITH_TIMEOUT(poster->property("status").toInt(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PlayingState, 5000);
  }

  void replacedVideoReopensTheSamePath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), path));
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    QCOMPARE(player->subtitleTracks().size(), 0);
    const QString version = m_session->contentVersion();
    const QUrl url = player->source();
    const QString replacement = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(!replacement.isEmpty());
    // Atomic replacement keeps the old decoder's open descriptor valid until
    // the watcher asks the player to reopen the unchanged local URL.
    const QString staged = dir.filePath(QStringLiteral("replacement"));
    QVERIFY(QFile::copy(replacement, staged));
    QVERIFY(QFile::remove(path));
    QVERIFY(QFile::rename(staged, path));
    QTRY_VERIFY_WITH_TIMEOUT(m_session->contentVersion() != version, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(player->subtitleTracks().size(), 1, 5000);
    QCOMPARE(player->source(), url);
    QCOMPARE(m_session->path(), path);
    m_window->close();
  }

  void opensAPictureFittedEdgeToEdge() {
    open({media(QStringLiteral("shot 2.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(m_window->title().startsWith(QStringLiteral("shot 2.jpg")));
    // What the float rule in packaging/hypr matches.
    QCOMPARE(m_window->title(), QStringLiteral("shot 2.jpg · Omaroll"));
    QCOMPARE(prop("viewScale").toReal(), 0.0);
    // The picture fills the window in one dimension and fits in the other.
    const qreal width = prop("displayWidth").toReal();
    const qreal height = prop("displayHeight").toReal();
    QVERIFY(width <= m_window->width() + 0.5 && height <= m_window->height() + 0.5);
    QVERIFY(qAbs(width - m_window->width()) < 1 || qAbs(height - m_window->height()) < 1);
    // The controls fade in on open, so the header is up once that has begun.
    QTRY_VERIFY(item(QStringLiteral("viewerHeader"))->isVisible());
    QCOMPARE(item(QStringLiteral("viewerTitle"))->property("text").toString(),
             QStringLiteral("shot 2.jpg"));
  }

  void extensionlessPicturesDecodeAndAnimationsPlay_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("animated");
    QTest::newRow("png") << QStringLiteral("transparent.png") << false;
    QTest::newRow("gif") << QStringLiteral("animated.gif") << true;
    QTest::newRow("webp") << QStringLiteral("animated.webp") << true;
  }

  void extensionlessPicturesDecodeAndAnimationsPlay() {
    QFETCH(QString, fixture);
    QFETCH(bool, animated);
    const QString source = QFINDTESTDATA(qPrintable("fixtures/viewer/" + fixture));
    QVERIFY(!source.isEmpty());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("picture without extension"));
    QVERIFY(QFile::copy(source, path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(prop("playbackError").toString().isEmpty());
    QCOMPARE(m_session->isAnimated(), animated);
    if (animated) {
      QQuickItem* image = item(QStringLiteral("viewerAnimation"));
      QTRY_VERIFY(image->property("currentFrame").toInt() >= 3);
    }
    m_window->close();
    m_session->clear();
  }

  // A small picture fills the window rather than sitting at its real size in
  // the middle of it, and the real size is still one key away. The edges keep
  // the theme's translucency, but the picture's own area never shows the
  // desktop through it, even where a PNG is transparent.
  void aSmallPictureFillsTheWindowAndStaysOpaque() {
    const QString folder = m_scratch.filePath(QStringLiteral("small"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/headshot.png");
    QImage picture(400, 300, QImage::Format_ARGB32);
    picture.fill(QColor(200, 60, 40));
    for (int y = 0; y < picture.height(); ++y) {
      for (int x = 0; x < picture.width() / 2; ++x) {
        picture.setPixelColor(x, y, Qt::transparent);
      }
    }
    QVERIFY(picture.save(path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    const qreal actual = 1.0 / m_window->devicePixelRatio();
    QVERIFY(prop("fitScale").toReal() > actual);
    QCOMPARE(prop("viewScale").toReal(), 0.0);
    QVERIFY(qAbs(prop("displayHeight").toReal() - m_window->height()) < 1);

    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("effectiveScale").toReal(), actual);
    QVERIFY(prop("viewScale").toReal() > 0);
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);

    // 4:3 in a 3:2 window leaves a band either side. The pointer rests in
    // the middle, off the header, so the controls fade for the grab.
    m_window->setProperty("chromeTimeout", 50);
    QTest::mouseMove(m_window, QPoint(m_window->width() / 2, m_window->height() * 2 / 3));
    QTRY_VERIFY(!prop("chromeShown").toBool());
    QTRY_VERIFY(!item(QStringLiteral("viewerHeader"))->isVisible());
    QSignalSpy rendered(m_window, &QQuickWindow::afterRendering);
    m_window->update();
    QTRY_VERIFY(!rendered.isEmpty());
    const QImage frame = m_window->grabWindow();
    QVERIFY(!frame.isNull());
    const qreal scale = frame.devicePixelRatio();
    const auto at = [&](qreal x, qreal y) { return frame.pixelColor(QPointF(x * scale, y * scale).toPoint()); };
    const qreal left = (m_window->width() - prop("displayWidth").toReal()) / 2;
    const qreal middleY = m_window->height() / 2.0;
    QCOMPARE(at(left + 20, middleY).alpha(), 255);
    const QColor red = at(m_window->width() - left - 20, middleY);
    QVERIFY2(red.alpha() == 255 && red.red() > 150 && red.green() < 100,
             qPrintable(red.name(QColor::HexArgb)));
    if (m_theme->surfaceAlpha() < 0.99) {
      QVERIFY2(at(4, middleY).alpha() < 255, qPrintable(at(4, middleY).name(QColor::HexArgb)));
    }
  }

  // Display P3 green is drawn as its sRGB equivalent, #00cc0d, rather than
  // the washed-out stored values, #1ec83c.
  void aWideGamutPictureIsShownInSrgb() {
    const QString folder = m_scratch.filePath(QStringLiteral("wide-gamut"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/p3.png");
    QImage image(400, 300, QImage::Format_RGB32);
    image.fill(QColor(30, 200, 60));
    image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    QVERIFY(image.save(path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    QSignalSpy rendered(m_window, &QQuickWindow::afterRendering);
    m_window->update();
    QTRY_VERIFY(!rendered.isEmpty());
    QImage frame;
    const auto convertedPixelReady = [&] {
      frame = m_window->grabWindow();
      if (frame.isNull()) return false;
      const QColor color = frame.pixelColor(frame.width() / 2, frame.height() / 2);
      return color.red() < 10 && color.green() > 150 && color.blue() < 25;
    };
    // imageReady precedes the image's opacity animation. Observe the pixels
    // instead of assuming that one submitted frame has completed that fade.
    QTRY_VERIFY(convertedPixelReady());
    // Chrome left over from earlier tests can dim the frame a little, so
    // check what the conversion changes: the stored red and blue are gone.
    const QColor color = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    QVERIFY2(color.red() < 10 && color.green() > 150 && color.blue() < 25,
             qPrintable(color.name()));
  }

  void theToolbarHasTheCommonPictureControls() {
    const QString folder = m_scratch.filePath(QStringLiteral("toolbar"));
    QVERIFY(QDir().mkpath(folder));
    const QString first = folder + QStringLiteral("/a.jpg");
    const QString second = folder + QStringLiteral("/b.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), first));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), second));
    open({first});
    QTRY_COMPARE(m_session->count(), 2);
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->setProperty("chromePinned", true);
    QQuickItem* toolbar = item(QStringLiteral("viewerToolbar"));
    QTRY_VERIFY(toolbar->isVisible());

    click(item(QStringLiteral("viewerRotateRight")));
    QCOMPARE(prop("viewRotation").toInt(), 90);
    click(item(QStringLiteral("viewerRotateLeft")));
    click(item(QStringLiteral("viewerRotateLeft")));
    QCOMPARE(prop("viewRotation").toInt(), 270);
    click(item(QStringLiteral("viewerRotateRight")));
    QCOMPARE(prop("viewRotation").toInt(), 0);

    QQuickItem* fit = item(QStringLiteral("viewerFitToggle"));
    QCOMPARE(fit->property("label").toString(), QStringLiteral("1:1"));
    click(item(QStringLiteral("viewerZoomIn")));
    QTRY_VERIFY(!prop("zooming").toBool());
    QVERIFY(prop("viewScale").toReal() > prop("fitScale").toReal());
    QCOMPARE(fit->property("label").toString(), QStringLiteral("Fit"));
    click(fit);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);
    click(fit);
    QTRY_COMPARE(prop("effectiveScale").toReal(), 1.0 / m_window->devicePixelRatio());
    click(item(QStringLiteral("viewerZoomOut")));
    click(item(QStringLiteral("viewerZoomOut")));
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);

    click(item(QStringLiteral("viewerTrashButton")));
    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    QVERIFY(confirm->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!confirm->isVisible());
    QVERIFY(QFile::exists(first));

    // The toolbar never sits over a video; the video has its own bar.
    m_window->setProperty("chromePinned", false);
    open({media(QStringLiteral("clip.mp4"))});
    QVERIFY(!toolbar->isVisible());
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    player->pause();
  }

  // Flips, like the turn, change only the view: the file is untouched, a
  // clockwise turn under a mirror still turns the screen clockwise, both
  // mirrors are a half turn, and the next file starts plain.
  void flipsTheViewAndStartsTheNextFilePlain() {
    const QString folder = m_scratch.filePath(QStringLiteral("flip"));
    QVERIFY(QDir().mkpath(folder));
    const QString first = folder + QStringLiteral("/a.png");
    QImage picture(400, 200, QImage::Format_RGB32);
    picture.fill(Qt::red);
    {
      QPainter painter(&picture);
      painter.fillRect(200, 0, 200, 200, Qt::blue);
    }
    QVERIFY(picture.save(first));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), folder + QStringLiteral("/b.jpg")));
    const QByteArray before = digest(first);
    open({first});
    QTRY_COMPARE(m_session->count(), 2);
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->setProperty("chromePinned", true);

    // Red or blue, sampled a quarter of the way along the shown picture.
    const auto colourAt = [this](qreal fx, qreal fy) {
      QTest::qWait(60);
      const QImage frame = m_window->grabWindow();
      const QRectF shown = item(QStringLiteral("viewerFrame"))
                               ->mapRectToScene(QRectF(0, 0, prop("displayWidth").toReal(),
                                                       prop("displayHeight").toReal()));
      const QPointF at(shown.left() + shown.width() * fx, shown.top() + shown.height() * fy);
      const QColor pixel = frame.pixelColor((at * frame.devicePixelRatio()).toPoint());
      // Relative, since the picture may still be fading in.
      return pixel.red() > 60 && pixel.red() > pixel.blue() * 2 ? QStringLiteral("red")
             : pixel.blue() > 60 && pixel.blue() > pixel.red() * 2 ? QStringLiteral("blue")
                                                                     : pixel.name();
    };
    QTRY_COMPARE(colourAt(0.25, 0.5), QStringLiteral("red"));

    QTest::keyClick(m_window, Qt::Key_H, Qt::ShiftModifier);
    QVERIFY(prop("viewFlipHorizontal").toBool());
    QVERIFY(prop("viewTransformed").toBool());
    QTRY_COMPARE(colourAt(0.25, 0.5), QStringLiteral("blue"));
    QVERIFY(item(QStringLiteral("viewerFlipHorizontal"))->property("active").toBool());
    // The flip buttons are toggles to assistive technology too.
    {
      QAccessibleInterface* horizontal =
          QAccessible::queryAccessibleInterface(item(QStringLiteral("viewerFlipHorizontal")));
      QAccessibleInterface* vertical =
          QAccessible::queryAccessibleInterface(item(QStringLiteral("viewerFlipVertical")));
      QVERIFY(horizontal && vertical);
      QVERIFY(horizontal->state().checkable);
      QVERIFY(horizontal->state().checked);
      QVERIFY(vertical->state().checkable);
      QVERIFY(!vertical->state().checked);
    }
    QVERIFY(item(QStringLiteral("viewerSaveCopy"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerSaveCopy"))->isEnabled());

    // The left of the screen, blue, goes to the top.
    QTest::keyClick(m_window, Qt::Key_R);
    QCOMPARE(prop("viewRotation").toInt(), 270);
    QTRY_COMPARE(colourAt(0.5, 0.25), QStringLiteral("blue"));
    QTRY_COMPARE(colourAt(0.5, 0.75), QStringLiteral("red"));

    QTest::keyClick(m_window, Qt::Key_V, Qt::ShiftModifier);
    QVERIFY(!prop("viewFlipHorizontal").toBool());
    QVERIFY(!prop("viewFlipVertical").toBool());
    QCOMPARE(prop("viewRotation").toInt(), 90);
    QTRY_COMPARE(colourAt(0.5, 0.25), QStringLiteral("red"));
    // Shift+V flips; plain V is still the favourite.
    QVERIFY(!m_settings->isFavorite(first));

    click(item(QStringLiteral("viewerFlipVertical")));
    QVERIFY(prop("viewFlipVertical").toBool());
    QTRY_COMPARE(colourAt(0.5, 0.25), QStringLiteral("blue"));

    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QStringList ids = menuIds(menu);
    for (const QString& id : {QStringLiteral("flip-horizontal"), QStringLiteral("flip-vertical"),
                              QStringLiteral("save-view-copy")})
      QVERIFY2(ids.contains(id), qPrintable(id));
    for (const QVariant& entry : menu->property("entries").toList()) {
      if (entry.toMap().value(QStringLiteral("id")) == QStringLiteral("save-view-copy"))
        QCOMPARE(entry.toMap().value(QStringLiteral("label")).toString(),
                 QStringLiteral("Save rotated copy"));
    }

    // The new keys are in the help sheet.
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("viewerShortcutHelp"));
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    auto* content = help->property("contentItem").value<QQuickItem*>();
    for (const QString& key : {QStringLiteral("Shift+H"), QStringLiteral("Shift+V"),
                               QStringLiteral("Ctrl+S")}) {
      QVERIFY2(find(content, [&key](QQuickItem* child) {
                 return child->property("text").toString() == key;
               }), qPrintable(key));
    }
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!help->property("visible").toBool());

    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(prop("viewRotation").toInt(), 0);
    QVERIFY(!prop("viewFlipHorizontal").toBool());
    QVERIFY(!prop("viewFlipVertical").toBool());
    QVERIFY(!menuIds(menu).contains(QStringLiteral("save-view-copy")));
    // Kept in place on the bar, greyed until there is a turn or flip to save,
    // and named for what it waits on rather than a flip that is not there.
    QVERIFY(item(QStringLiteral("viewerSaveCopy"))->isVisible());
    QVERIFY(!item(QStringLiteral("viewerSaveCopy"))->isEnabled());
    QCOMPARE(item(QStringLiteral("viewerSaveCopy"))->property("toolTip").toString(),
             QStringLiteral("Rotate or flip to save a copy"));
    QCOMPARE(digest(first), before);
    QCOMPARE(QDir(folder).entryList(QDir::Files).size(), 2);

    // Nothing to mirror in a video; H there is not a flip.
    m_window->setProperty("chromePinned", false);
    open({media(QStringLiteral("clip.mp4"))});
    QTest::keyClick(m_window, Qt::Key_H, Qt::ShiftModifier);
    QVERIFY(!prop("viewFlipHorizontal").toBool());
    QVERIFY(!menuIds(menu).contains(QStringLiteral("flip-horizontal")));
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    player->pause();
  }

  // Save comes up after a turn or flip and goes while the copy is written
  // and after. Its place on a still's toolbar is kept throughout, so no
  // button moves under the pointer either way, on the bar or off it.
  void toolbarKeepsItsPlaceAfterSaveCopy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    const QString source = folder + QStringLiteral("/plain.png");
    QImage picture(240, 160, QImage::Format_RGB32);
    picture.fill(QColor(30, 120, 200));
    QVERIFY(picture.save(source));
    open({source});
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->setProperty("chromePinned", true);
    const auto unpin = qScopeGuard([&] { m_window->setProperty("chromePinned", false); });
    QQuickItem* save = item(QStringLiteral("viewerSaveCopy"));
    QQuickItem* slot = item(QStringLiteral("viewerSaveCopySlot"));
    QQuickItem* trash = item(QStringLiteral("viewerTrashButton"));
    QQuickItem* flip = item(QStringLiteral("viewerFlipHorizontal"));
    QQuickItem* rotate = item(QStringLiteral("viewerRotateRight"));
    // Nothing to save yet: the place is there, greyed.
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(save->isVisible() && !save->isEnabled());
    QTest::qWait(30);
    const qreal trashX = trash->mapToScene(QPointF()).x();
    const qreal flipX = flip->mapToScene(QPointF()).x();
    const auto centre = [](QQuickItem* target) {
      return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
    };

    // Appearing: flip from the bar, and the button under the pointer stays.
    QTest::mouseMove(m_window, centre(flip));
    QTRY_VERIFY(item(QStringLiteral("viewerToolbar"))->property("hovered").toBool());
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(flip));
    QTRY_VERIFY(save->isEnabled());
    QTest::qWait(30);
    QCOMPARE(flip->mapToScene(QPointF()).x(), flipX);
    QCOMPARE(trash->mapToScene(QPointF()).x(), trashX);
    // A turn on top changes nothing on the bar.
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(rotate));
    QTRY_VERIFY(prop("viewRotation").toInt() != 0);
    QVERIFY(save->isEnabled());
    QCOMPARE(trash->mapToScene(QPointF()).x(), trashX);

    // Going: save, while the copy is written and once it is open.
    const QPoint onSave = centre(save);
    QTest::mouseMove(m_window, onSave);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, onSave);
    QTRY_VERIFY_WITH_TIMEOUT(m_session->path() != source, 15000);
    QTRY_VERIFY(!save->isEnabled());
    QVERIFY(slot->isVisible());
    QTest::qWait(30);
    QCOMPARE(trash->mapToScene(QPointF()).x(), trashX);
    // Off the bar the place stays too: the copy is a still that can be turned.
    QTest::mouseMove(m_window, QPoint(m_window->width() / 2, 40));
    QTRY_VERIFY(!item(QStringLiteral("viewerToolbar"))->property("hovered").toBool());
    QTest::qWait(30);
    QVERIFY(slot->isVisible());
    QCOMPARE(trash->mapToScene(QPointF()).x(), trashX);
  }

  void savesWhatTheViewShowsAsACopy_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("flip");
    // A quarter turn needs whole JPEG blocks to be lossless; a transpose
    // (turned and flipped) never does.
    QTest::newRow("jpeg-lossless") << QStringLiteral("aligned.jpg") << QSize(240, 160) << false;
    QTest::newRow("jpeg-partial-blocks") << QStringLiteral("partial.jpg") << QSize(250, 170) << false;
    QTest::newRow("jpeg-flipped") << QStringLiteral("flipped.jpg") << QSize(250, 170) << true;
    QTest::newRow("png") << QStringLiteral("plain.png") << QSize(250, 170) << true;
  }

  // Turned and flipped, then saved: a copy beside the original, which is not
  // touched, opens next to it. A JPEG is turned by jpegtran without
  // re-encoding when its size allows, and otherwise says it was re-encoded.
  void savesWhatTheViewShowsAsACopy() {
    QFETCH(QString, name);
    QFETCH(QSize, size);
    QFETCH(bool, flip);
    const bool jpeg = name.endsWith(QLatin1String(".jpg"));
    if (jpeg && !JpegTransform::available()) QSKIP("jpegtran is not installed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    const QString source = folder + QLatin1Char('/') + name;
    QImage picture(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y)
      for (int x = 0; x < size.width(); ++x)
        picture.setPixelColor(x, y, QColor(x * 255 / size.width(), y * 255 / size.height(), 90));
    QVERIFY(picture.save(source, nullptr, 95));
    const QByteArray before = digest(source);
    const QDateTime modified = QFileInfo(source).lastModified();
    const QImage decoded = QImage(source).convertToFormat(QImage::Format_RGB32);

    open({source});
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->setProperty("chromePinned", true);
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE(prop("status").toString(), QStringLiteral("Rotate or flip the picture first"));
    QTest::keyClick(m_window, Qt::Key_R);
    if (flip) QTest::keyClick(m_window, Qt::Key_H, Qt::ShiftModifier);
    QTRY_VERIFY(item(QStringLiteral("viewerSaveCopy"))->isEnabled());
    if (jpeg) QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    else click(item(QStringLiteral("viewerSaveCopy")));

    QTRY_VERIFY_WITH_TIMEOUT(m_session->path() != source, 15000);
    const QString copy = m_session->path();
    QCOMPARE(QFileInfo(copy).absolutePath(), folder);
    QVERIFY2(QFileInfo(copy).fileName().contains(QLatin1String("-edited")), qPrintable(copy));
    QCOMPARE(QFileInfo(copy).suffix(), QFileInfo(source).suffix());
    // The original, byte for byte and untouched; the copy is next to it.
    QCOMPARE(digest(source), before);
    QCOMPARE(QFileInfo(source).lastModified(), modified);
    QTRY_COMPARE(m_session->count(), 2);
    QCOMPARE(qAbs(m_session->sequence().indexOf(copy) - m_session->sequence().indexOf(source)), 1);
    const QString status = prop("status").toString();
    QVERIFY2(status.startsWith(flip ? QStringLiteral("Saved a rotated and flipped copy beside the original")
                                    : QStringLiteral("Saved a rotated copy beside the original")),
             qPrintable(status));
    // The copy is shown as it is, upright.
    QCOMPARE(prop("viewRotation").toInt(), 0);
    QVERIFY(!prop("viewFlipHorizontal").toBool());
    QVERIFY(!prop("viewTransformed").toBool());

    const QImage saved = QImage(copy).convertToFormat(QImage::Format_RGB32);
    QCOMPARE(saved.size(), size.transposed());
    // A quarter turn clockwise, then mirrored across the screen: a transpose.
    const QImage turned = decoded.transformed(QTransform().rotate(90), Qt::FastTransformation);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    const QImage expected = flip ? turned.flipped(Qt::Horizontal) : turned;
#else
    const QImage expected = flip ? turned.mirrored(true, false) : turned;
#endif
    if (jpeg && name != QLatin1String("partial.jpg")) {
      QVERIFY2(!status.contains(QLatin1String("Re-encoded")), qPrintable(status));
      // Exactly what jpegtran writes for the same transform.
      const QString reference = dir.filePath(QStringLiteral("reference.jpg"));
      QProcess jpegtran;
      QStringList transform = flip ? QStringList{QStringLiteral("-transpose")}
                                   : QStringList{QStringLiteral("-rotate"), QStringLiteral("90")};
      jpegtran.start(QStandardPaths::findExecutable(QStringLiteral("jpegtran")),
                     transform + QStringList{QStringLiteral("-perfect"),
                      QStringLiteral("-copy"), QStringLiteral("all"), QStringLiteral("-outfile"),
                      reference, source});
      QVERIFY(jpegtran.waitForFinished(15000));
      QCOMPARE(jpegtran.exitCode(), 0);
      QCOMPARE(saved, QImage(reference).convertToFormat(QImage::Format_RGB32));
    } else if (jpeg) {
      QVERIFY2(status.contains(QLatin1String("Re-encoded, since its size cannot be turned losslessly")),
               qPrintable(status));
    } else {
      QVERIFY2(!status.contains(QLatin1String("Re-encoded")), qPrintable(status));
      QCOMPARE(saved, expected);
    }
    if (jpeg) {
      int worst = 0;
      for (int y = 0; y < saved.height(); y += 7)
        for (int x = 0; x < saved.width(); x += 7) {
          const QColor a = saved.pixelColor(x, y);
          const QColor b = expected.pixelColor(x, y);
          worst = std::max({worst, qAbs(a.red() - b.red()), qAbs(a.green() - b.green()),
                            qAbs(a.blue() - b.blue())});
        }
      QVERIFY2(worst <= 24, qPrintable(QString::number(worst)));
    }
    m_window->setProperty("chromePinned", false);
  }

  // What the copy holds is what the screen showed, for every way of getting
  // there: a vertical flip, a half turn, a left turn, and turns after flips.
  // It lands next to its original in a folder of more than two files.
  void savedCopyMatchesTheView_data() {
    QTest::addColumn<QStringList>("keys");
    QTest::newRow("vertical") << QStringList{u"Shift+V"_s};
    QTest::newRow("half-turn") << QStringList{u"R"_s, u"R"_s};
    QTest::newRow("left-turn") << QStringList{u"Shift+R"_s};
    QTest::newRow("flip-then-turn") << QStringList{u"Shift+H"_s, u"R"_s};
    QTest::newRow("turn-then-flip") << QStringList{u"R"_s, u"Shift+V"_s};
    QTest::newRow("both-flips") << QStringList{u"Shift+H"_s, u"Shift+V"_s};
    QTest::newRow("flip-then-turn-left") << QStringList{u"Shift+V"_s, u"Shift+R"_s};
  }

  static QImage mirror(const QImage& image, bool horizontal) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return image.flipped(horizontal ? Qt::Horizontal : Qt::Vertical);
#else
    return image.mirrored(horizontal, !horizontal);
#endif
  }

  void savedCopyMatchesTheView() {
    QFETCH(QStringList, keys);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    QImage picture(120, 80, QImage::Format_RGB32);
    picture.fill(Qt::red);
    {
      QPainter painter(&picture);
      painter.fillRect(60, 0, 60, 40, Qt::blue);
      painter.fillRect(0, 40, 60, 40, Qt::green);
      painter.fillRect(90, 60, 30, 20, Qt::yellow);
    }
    for (const QString& name : {u"a.png"_s, u"b.png"_s, u"c.png"_s})
      QVERIFY(picture.save(folder + u'/' + name));
    const QString source = folder + u"/b.png"_s;
    open({source});
    QTRY_COMPARE(m_session->count(), 3);
    QTRY_VERIFY(prop("imageReady").toBool());

    // The screen's own moves, one at a time.
    QImage expected = picture;
    for (const QString& key : keys) {
      if (key == u"R"_s) {
        QTest::keyClick(m_window, Qt::Key_R);
        expected = expected.transformed(QTransform().rotate(90), Qt::FastTransformation);
      } else if (key == u"Shift+R"_s) {
        QTest::keyClick(m_window, Qt::Key_R, Qt::ShiftModifier);
        expected = expected.transformed(QTransform().rotate(270), Qt::FastTransformation);
      } else if (key == u"Shift+H"_s) {
        QTest::keyClick(m_window, Qt::Key_H, Qt::ShiftModifier);
        expected = mirror(expected, true);
      } else {
        QTest::keyClick(m_window, Qt::Key_V, Qt::ShiftModifier);
        expected = mirror(expected, false);
      }
    }
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(m_session->path() != source, 15000);
    const QString copy = m_session->path();
    QCOMPARE(QFileInfo(copy).fileName(), u"b-edited.png"_s);
    QVERIFY2(prop("status").toString().endsWith(u" copy beside the original"_s),
             qPrintable(prop("status").toString()));
    QCOMPARE(QImage(copy).convertToFormat(QImage::Format_RGB32), expected.convertToFormat(QImage::Format_RGB32));
    QTRY_COMPARE(m_session->count(), 4);
    QCOMPARE(qAbs(m_session->sequence().indexOf(copy) - m_session->sequence().indexOf(source)), 1);
  }

  // Stepping on while a copy is saved: the window stays where it went, and
  // the copy joins next to its own original, not the file now shown.
  void savedCopyStaysBesideItsSourceAfterStepping_data() {
    QTest::addColumn<bool>("selection");
    QTest::newRow("folder") << false;
    QTest::newRow("selection") << true;
  }

  void savedCopyStaysBesideItsSourceAfterStepping() {
    QFETCH(bool, selection);
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    {
      QFile helper(bin.filePath(QStringLiteral("jpegtran")));
      QVERIFY(helper.open(QIODevice::WriteOnly));
      helper.write("#!/bin/sh\nsleep 1\nwhile test \"$1\" != -outfile; do shift; done\ncp \"$3\" \"$2\"\n");
      helper.close();
      QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", bin.path().toUtf8() + ':' + previousPath));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    QStringList paths;
    for (const QString& name : {u"a.jpg"_s, u"b.jpg"_s, u"c.jpg"_s}) {
      paths.append(folder + u'/' + name);
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), paths.last()));
    }
    if (selection) {
      open(paths);
      QVERIFY(m_session->jump(1));
    } else {
      open({paths.at(1)});
      QTRY_COMPARE(m_session->count(), 3);
    }
    QCOMPARE(m_session->path(), paths.at(1));
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_R);
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    QVERIFY(prop("savingViewCopy").toBool());
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), paths.at(2));
    QTRY_VERIFY_WITH_TIMEOUT(!prop("savingViewCopy").toBool(), 15000);
    const QString copy = folder + u"/b-edited.jpg"_s;
    QVERIFY(QFileInfo::exists(copy));
    QTRY_COMPARE(prop("status").toString(), u"Saved a rotated copy of b.jpg beside it"_s);
    QCOMPARE(m_session->path(), paths.at(2));
    QTRY_COMPARE(m_session->count(), 4);
    if (selection) {
      QCOMPARE(m_session->sequence(), (QStringList{paths.at(0), paths.at(1), copy, paths.at(2)}));
    } else {
      QCOMPARE(qAbs(m_session->sequence().indexOf(copy) - m_session->sequence().indexOf(paths.at(1))), 1);
    }
    QTest::qWait(300);
    QCOMPARE(m_session->path(), paths.at(2));
  }

  // A link's copy is saved where the link is, the folder being browsed, and
  // shown there; opened on its own, the notice says where the copy went.
  void savedCopyOfALinkGoesBesideTheLink() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    const QString browsed = root + u"/browsed"_s;
    const QString store = root + u"/store"_s;
    QVERIFY(QDir().mkpath(browsed));
    QVERIFY(QDir().mkpath(store));
    QImage picture(64, 48, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    QVERIFY(picture.save(browsed + u"/a.png"_s));
    const QString target = store + u"/target.png"_s;
    QVERIFY(picture.save(target));
    const QString link = browsed + u"/linked.png"_s;
    QVERIFY(QFile::link(target, link));

    open({browsed + u"/a.png"_s});
    QTRY_COMPARE(m_session->count(), 2);
    QVERIFY(m_session->jump(int(m_session->sequence().indexOf(target))));
    QCOMPARE(m_session->deletionPath(), link);
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_R);
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    const QString copy = browsed + u"/linked-edited.png"_s;
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(copy), 15000);
    QTRY_COMPARE(m_session->path(), copy);
    QCOMPARE(prop("status").toString(), u"Saved a rotated copy beside the original"_s);
    QVERIFY(!QFileInfo::exists(store + u"/target-edited.png"_s));

    // Opened by the link from the file manager: the window shows the store.
    const OpenRequest request = OpenRequest::fromPaths({link});
    open(request.files);
    m_session->setDeletionPaths(request.entryPaths);
    QCOMPARE(m_session->path(), target);
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_R);
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    const QString second = browsed + u"/linked-edited-2.png"_s;
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(second), 15000);
    QTRY_COMPARE(prop("status").toString(), u"Saved a rotated copy to "_s + second);
    QCOMPARE(m_session->path(), target);
    QVERIFY(!QFileInfo::exists(store + u"/target-edited.png"_s));
  }

  // A taken name gets a number; a folder that cannot be written says so and
  // leaves the window on the original.
  void savedCopyTakesAFreeNameOrSaysWhy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    QImage picture(64, 48, QImage::Format_RGB32);
    picture.fill(Qt::darkGreen);
    const QString source = folder + u"/p.png"_s;
    QVERIFY(picture.save(source));
    QVERIFY(picture.save(folder + u"/p-edited.png"_s));
    open({source});
    QTRY_COMPARE(m_session->count(), 2);
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_V, Qt::ShiftModifier);
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(m_session->path(), folder + u"/p-edited-2.png"_s, 15000);
    QCOMPARE(prop("status").toString(), u"Saved a flipped copy beside the original"_s);

    if (::geteuid() == 0) QSKIP("root writes into read-only folders");
    open({source});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto writable = qScopeGuard([&] {
      QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    });
    const QStringList before = QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    QTest::keyClick(m_window, Qt::Key_R);
    QTest::keyClick(m_window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(prop("status").toString(),
                              u"Could not safely save a copy beside p.png; check its folder"_s, 15000);
    QCOMPARE(m_session->path(), source);
    QCOMPARE(QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), before);
  }

  // A menu taller than the room below the pointer scrolls, and sizing it
  // never loops: at the pointer near the top and the bottom of the window,
  // and in a short window.
  void viewerMenuFitsWhereverItOpens() {
    QTest::failOnWarning(QRegularExpression(u"[Bb]inding loop"_s));
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    const auto popUpAt = [&](const QPoint& at) {
      m_warnings.clear();
      QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, at);
      QTRY_VERIFY(menu->property("visible").toBool());
      QTest::qWait(100);
      QVERIFY(menu->property("height").toReal() <= m_window->height());
      for (const QString& warning : std::as_const(m_warnings))
        QVERIFY2(!warning.contains(u"inding loop"_s), qPrintable(warning));
      QTest::keyClick(m_window, Qt::Key_Escape);
      QTRY_VERIFY(!menu->property("visible").toBool());
    };
    popUpAt(QPoint(150, 120));
    popUpAt(QPoint(150, m_window->height() - 80));
    m_window->resize(640, 260);
    QTRY_COMPARE(m_window->height(), 260);
    popUpAt(QPoint(320, 130));
    QMetaObject::invokeMethod(m_window, "showActionMenu");
    QTRY_VERIFY(menu->property("visible").toBool());
    auto* list = menu->property("contentItem").value<QQuickItem*>();
    QVERIFY(list);
    // Taller than the window: it scrolls.
    QVERIFY(list->property("interactive").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
  }

  // The menu is a short top level with View, Share and Edit submenus. It keeps
  // every action and its key, stays inside an 820 px window, and in a short
  // one scrolls with the wheel and follows the keyboard.
  void viewerMenuIsGroupedFitsAndScrolls() {
    open({media(QStringLiteral("Shot 1.jpg")), media(QStringLiteral("shot 2.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->resize(1280, 820);
    QTRY_COMPARE(m_window->height(), 820);
    auto* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    auto* viewMenu = m_window->findChild<QObject*>(QStringLiteral("viewerSubmenu_view"));
    auto* shareMenu = m_window->findChild<QObject*>(QStringLiteral("viewerSubmenu_share"));
    QVERIFY(menu && viewMenu && shareMenu);
    const auto inWindow = [&](QObject* popup) {
      auto* box = popup->property("background").value<QQuickItem*>();
      const QRectF bounds = box->mapRectToScene(box->boundingRect());
      return bounds.top() >= 0 && bounds.bottom() <= m_window->height() && bounds.left() >= 0
             && bounds.right() <= m_window->width();
    };
    // As the toolbar's More button opens it.
    QVERIFY(QMetaObject::invokeMethod(m_window, "openViewForRender", Q_ARG(QVariant, QStringLiteral("viewer-menu"))));
    QTRY_VERIFY(menu->property("visible").toBool());
    QTest::qWait(50);
    QVERIFY(inWindow(menu));
    QVERIFY(!menu->property("contentItem").value<QQuickItem*>()->property("interactive").toBool());
    // Short at the top: the groups and the common window actions.
    int topRows = 0;
    for (const QVariant& row : menu->property("rows").toList())
      topRows += row.toMap().value(QStringLiteral("separator")).toBool() ? 0 : 1;
    QVERIFY2(topRows <= 12, qPrintable(QString::number(topRows)));
    for (const QString& group : {QStringLiteral("view"), QStringLiteral("share"), QStringLiteral("edit")}) {
      QQuickItem* row = item(QStringLiteral("viewerMenu_group-") + group);
      QVERIFY(row->property("subMenu").value<QObject*>());
      QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(row);
      QVERIFY(accessible);
      QCOMPARE(accessible->role(), QAccessible::MenuItem);
      QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
    }
    // Every action is still offered, with its key.
    const QStringList ids = menuIds(menu);
    for (const QString& id : {QStringLiteral("library"), QStringLiteral("fit"), QStringLiteral("fit-width"),
                              QStringLiteral("fill"), QStringLiteral("actual"), QStringLiteral("fit-shrink"),
                              QStringLiteral("rotate"), QStringLiteral("flip-horizontal"),
                              QStringLiteral("flip-vertical"), QStringLiteral("filmstrip"),
                              QStringLiteral("copy"), QStringLiteral("copy-path"), QStringLiteral("copy-name"),
                              QStringLiteral("open-with"), QStringLiteral("files"),
                              QStringLiteral("slideshow"), QStringLiteral("fullscreen"), QStringLiteral("info"),
                              QStringLiteral("help"), QStringLiteral("favorite"), QStringLiteral("trash")})
      QVERIFY2(ids.contains(id), qPrintable(id));
    QCOMPARE(menuRow(QStringLiteral("rotate"))->property("shortcutText").toString(), QStringLiteral("R"));
    QCOMPARE(menuRow(QStringLiteral("copy-path"))->property("shortcutText").toString(),
             QStringLiteral("Ctrl+Shift+C"));

    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(!menu->property("visible").toBool());
    QTRY_VERIFY(!viewMenu->property("visible").toBool() && !shareMenu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(m_window, "openViewForRender", Q_ARG(QVariant, QStringLiteral("viewer-menu"))));
    QTRY_VERIFY(menu->property("visible").toBool());

    // View opens beside it, also inside the window, and its rows act.
    QQuickItem* viewRow = item(QStringLiteral("viewerMenu_group-view"));
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                      viewRow->mapToScene(QPointF(viewRow->width() / 2, viewRow->height() / 2)).toPoint());
    QTRY_VERIFY(viewMenu->property("visible").toBool());
    QTest::qWait(50);
    QVERIFY(inWindow(viewMenu));
    QQuickItem* fitWidth = menuRow(QStringLiteral("fit-width"));
    QTRY_VERIFY(fitWidth->isVisible());
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                      fitWidth->mapToScene(QPointF(fitWidth->width() / 2, fitWidth->height() / 2)).toPoint());
    QTRY_VERIFY(!viewMenu->property("visible").toBool());
    QTRY_VERIFY(!menu->property("visible").toBool());
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("width"));

    // A short window: held inside it, scrolled by the wheel both ways.
    m_window->resize(640, 300);
    QTRY_COMPARE(m_window->height(), 300);
    QMetaObject::invokeMethod(m_window, "showActionMenu");
    QTRY_VERIFY(menu->property("visible").toBool());
    QTest::qWait(50);
    QVERIFY(inWindow(menu));
    auto* list = menu->property("contentItem").value<QQuickItem*>();
    QVERIFY(list->property("interactive").toBool());
    QVERIFY(item(QStringLiteral("viewerMenuBottomFade"))->isVisible());
    const QPoint over = list->mapToScene(QPointF(list->width() / 2, list->height() / 2)).toPoint();
    const auto wheel = [&](int notches) {
      for (int i = 0; i < std::abs(notches); ++i) {
        QWheelEvent event(over, m_window->mapToGlobal(over), QPoint(), QPoint(0, notches < 0 ? -120 : 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(m_window, &event);
      }
    };
    wheel(-10);
    QTRY_VERIFY(list->property("contentY").toReal() > list->property("originY").toReal() + 20);
    QVERIFY(list->property("atYEnd").toBool());
    QVERIFY(!item(QStringLiteral("viewerMenuBottomFade"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerMenuTopFade"))->isVisible());
    // The wheel stays on the menu: the picture did not zoom.
    QCOMPARE(prop("viewScale").toReal(), 0.0);
    wheel(10);
    QTRY_VERIFY(list->property("atYBeginning").toBool());
    // The keyboard's row is always in view.
    for (int step = 0; step < 14; ++step) {
      QTest::keyClick(m_window, Qt::Key_Down);
      const int current = menu->property("currentIndex").toInt();
      if (current < 0) continue;
      QQuickItem* highlighted = nullptr;
      QVERIFY(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, highlighted),
                                        Q_ARG(int, current)));
      QVERIFY(highlighted);
      QTRY_VERIFY(highlighted->mapToItem(list, QPointF()).y() >= -1);
      QTRY_VERIFY(highlighted->mapToItem(list, QPointF(0, highlighted->height())).y() <= list->height() + 1);
    }
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());

    // Each submenu fits the short window too.
    QMetaObject::invokeMethod(m_window, "showActionMenu");
    QTRY_VERIFY(menu->property("visible").toBool());
    QQuickItem* shareRow = item(QStringLiteral("viewerMenu_group-share"));
    QTRY_VERIFY(shareRow->mapToItem(list, QPointF()).y() >= 0);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                      shareRow->mapToScene(QPointF(shareRow->width() / 2, shareRow->height() / 2)).toPoint());
    QTRY_VERIFY(shareMenu->property("visible").toBool());
    QTest::qWait(50);
    QVERIFY(inWindow(shareMenu));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!shareMenu->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
  }

  // The file goes out as the same text/uri-list and text/plain a library
  // tile carries, from a Ctrl+drag on the picture or a drag on its name.
  void dragOutCarriesWhatALibraryTileCarries() {
    const QString path = media(QStringLiteral("Shot 1.jpg"));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    const QVariantMap data = prop("dragMimeData").toMap();
    QCOMPARE(data, m_actions->fileDragData({path}));
    QCOMPARE(data.value(QStringLiteral("text/uri-list")).toString(), FileDrag::uriList({path}));
    QCOMPARE(data.value(QStringLiteral("text/uri-list")).toString(),
             QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) + QStringLiteral("\r\n"));
    QCOMPARE(data.value(QStringLiteral("text/plain")).toString(), path);
    QObject* stage = m_window->findChild<QObject*>(QStringLiteral("viewerStageFileDrag"));
    QVERIFY(stage);
    QCOMPARE(stage->property("acceptedModifiers").toInt(), int(Qt::ControlModifier));
    QVERIFY(m_window->findChild<QObject*>(QStringLiteral("viewerTitleFileDrag")));
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(prop("dragMimeData").toMap().value(QStringLiteral("text/plain")).toString(),
             m_session->path());
  }

  void droppedFilesOpenInThisViewer() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    const QString a = folder + QStringLiteral("/a.jpg");
    const QString b = folder + QStringLiteral("/b.jpg");
    const QString text = folder + QStringLiteral("/notes.txt");
    const QString pdf = folder + QStringLiteral("/guide.pdf");
    const QString inner = folder + QStringLiteral("/inner");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), a));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), b));
    QVERIFY(QFile::copy(media(QStringLiteral("readme.txt")), text));
    QVERIFY(QFile::copy(media(QStringLiteral("notes.pdf")), pdf));
    QVERIFY(QDir().mkpath(inner));
    QSignalSpy handedOff(m_actions, &ActionLauncher::openRequested);
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());

    // While it is over the window, the window says what a drop will do.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(a)}, [this] {
      QVERIFY(item(QStringLiteral("viewerDropHighlight"))->property("shown").toBool());
    }));
    QTRY_COMPARE(m_session->path(), a);
    QTRY_VERIFY(!item(QStringLiteral("viewerDropHighlight"))->property("shown").toBool());
    QTRY_COMPARE(m_session->count(), 2);
    QVERIFY(!m_session->selection());

    // Mixed: the picture opens and the skipped file is named.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(b), QUrl::fromLocalFile(text)}));
    QTRY_COMPARE(m_session->path(), b);
    QVERIFY2(prop("status").toString().contains(QLatin1String("Skipped:"))
                 && prop("status").toString().contains(QLatin1String("notes.txt")),
             qPrintable(prop("status").toString()));

    // Two pictures are a selection, as two files from the file manager are.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)}));
    QTRY_VERIFY(m_session->selection());
    QCOMPARE(m_session->sequence(), (QStringList{a, b}));
    QCOMPARE(handedOff.size(), 0);

    // A folder or a document goes to the library, as from the command line.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(inner)}));
    QTRY_COMPARE(handedOff.size(), 1);
    QCOMPARE(handedOff.last().first().toStringList(), QStringList{inner});
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(pdf)}));
    QTRY_COMPARE(handedOff.size(), 2);
    QCOMPARE(handedOff.last().first().toStringList(), QStringList{pdf});
    QCOMPARE(m_session->sequence(), (QStringList{a, b}));

    // Nothing local: said, and the window keeps what it shows.
    QVERIFY(dropOn(m_window, {QUrl(QStringLiteral("https://example.com/cat.jpg"))}));
    QTRY_COMPARE(prop("status").toString(), QStringLiteral("Skipped: cat.jpg"));
    QCOMPARE(m_session->sequence(), (QStringList{a, b}));
    QCOMPARE(handedOff.size(), 2);

    // A picture and a folder: the picture opens here, the folder in the
    // library, on its own as `omaroll FOLDER` would.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(a), QUrl::fromLocalFile(inner)}));
    QTRY_COMPARE(handedOff.size(), 3);
    QCOMPARE(handedOff.last().first().toStringList(), QStringList{inner});
    QTRY_COMPARE(m_session->path(), a);
    QVERIFY(!m_session->selection());
    // Two folders are two library launches, not an error.
    const QString other = folder + QStringLiteral("/other");
    QVERIFY(QDir().mkpath(other));
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(inner), QUrl::fromLocalFile(other)}));
    QTRY_COMPARE(handedOff.size(), 5);
    QCOMPARE(handedOff.at(3).first().toStringList(), QStringList{inner});
    QCOMPARE(handedOff.at(4).first().toStringList(), QStringList{other});
    QCOMPARE(m_session->path(), a);
  }

  void stepsThroughTheFolderInNameOrder() {
    open({media(QStringLiteral("shot 2.jpg"))});
    // The folder is read off the GUI thread; the opened file shows first.
    QTRY_COMPARE(m_session->count(), int(m_order.size()));
    QCOMPARE(ViewerSession::siblings(m_folder), m_order);
    QCOMPARE(m_session->index(), 2);
    QCOMPARE(item(QStringLiteral("viewerCounter"))->property("text").toString(),
             QStringLiteral("3 / 4"));

    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), m_order.at(3));
    // Wraps from the last file to the first, which here is a video.
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), m_order.at(0));
    QVERIFY(m_session->isVideo());
    // On a video the arrows seek; Page Down moves on to the next file.
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), m_order.at(0));
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QCOMPARE(m_session->path(), m_order.at(1));
    QTest::keyClick(m_window, Qt::Key_Left);
    QCOMPARE(m_session->path(), m_order.at(0));
    QTest::keyClick(m_window, Qt::Key_PageUp);
    QCOMPARE(m_session->path(), m_order.at(3));
    QTest::keyClick(m_window, Qt::Key_Home);
    QCOMPARE(m_session->path(), m_order.at(0));
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QCOMPARE(m_session->path(), m_order.at(1));

    // The toolbar's buttons do the same, and stand in for the side arrows a
    // video keeps.
    m_window->setProperty("chromePinned", true);
    QVERIFY(!item(QStringLiteral("viewerNext"))->isVisible());
    click(item(QStringLiteral("viewerToolbarNext")));
    QCOMPARE(m_session->path(), m_order.at(2));
    click(item(QStringLiteral("viewerToolbarPrevious")));
    QCOMPARE(m_session->path(), m_order.at(1));
    m_window->setProperty("chromePinned", false);
  }

  void mouseSideButtonsStepAndRespectModalSheets() {
    const QString first = media(QStringLiteral("Shot 1.jpg"));
    const QString second = media(QStringLiteral("shot 2.jpg"));
    open({first});
    QTRY_COMPARE(m_session->count(), int(m_order.size()));
    QCOMPARE(m_session->path(), first);

    const QPoint at(m_window->width() / 2, m_window->height() / 2);
    QTest::mouseClick(m_window, Qt::ForwardButton, Qt::NoModifier, at);
    QTRY_COMPARE(m_session->path(), second);
    QTest::mouseClick(m_window, Qt::BackButton, Qt::NoModifier, at);
    QTRY_COMPARE(m_session->path(), first);

    // A modal keeps the side buttons from changing the file underneath it.
    QTest::keyClick(m_window, Qt::Key_Delete);
    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    QTRY_VERIFY(confirm->isVisible());
    QTest::mouseClick(m_window, Qt::ForwardButton, Qt::NoModifier, at);
    QCOMPARE(m_session->path(), first);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!confirm->isVisible());

    // On a video the side buttons change files, while the arrow keys seek.
    const QString clip = media(QStringLiteral("clip.mp4"));
    open({clip, first});
    QTRY_COMPARE(m_session->count(), 2);
    QTRY_VERIFY(m_session->isVideo());
    QTest::mouseClick(m_window, Qt::ForwardButton, Qt::NoModifier, at);
    QTRY_COMPARE(m_session->path(), first);
    QTest::mouseClick(m_window, Qt::BackButton, Qt::NoModifier, at);
    QTRY_COMPARE(m_session->path(), clip);
  }

  void openingADotfileShowsItInItsFolder() {
    const QString hidden = media(QStringLiteral(".hidden.jpg"));
    open({hidden});
    QTRY_COMPARE(m_session->count(), int(m_order.size()) + 1);
    QCOMPARE(m_session->path(), hidden);
  }

  void anExplicitSelectionKeepsItsOrder() {
    const QStringList selection{media(QStringLiteral("shot 10.jpg")),
                                media(QStringLiteral("Shot 1.jpg"))};
    open(selection);
    QCOMPARE(m_session->count(), 2);
    QVERIFY(m_session->selection());
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), selection.at(1));
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), selection.at(0));
  }

  void tabWalksTheControlsAndShortcutsKeepWorking_data() {
    QTest::addColumn<bool>("video");
    QTest::newRow("picture") << false;
    QTest::newRow("video") << true;
  }

  // Tab and Shift+Tab reach every visible control, with the controls held
  // up and a focus ring on the one focused, then return to the picture.
  // Viewer shortcuts work wherever focus is.
  void tabWalksTheControlsAndShortcutsKeepWorking() {
    QFETCH(bool, video);
    const QString picture = media(QStringLiteral("Shot 1.jpg"));
    const QString clip = media(QStringLiteral("clip.mp4"));
    open(video ? QStringList{clip, picture} : QStringList{picture, clip});
    if (!video) QTRY_VERIFY(prop("imageReady").toBool());
    if (video) {
      // The transport lays out its controls once the clip is loaded; a slow
      // runner could otherwise walk a shorter row than Shift+Tab finds later.
      QMediaPlayer* player = nullptr;
      QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
      QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus() == QMediaPlayer::LoadedMedia
                                   || player->mediaStatus() == QMediaPlayer::BufferingMedia
                                   || player->mediaStatus() == QMediaPlayer::BufferedMedia
                                   || player->mediaStatus() == QMediaPlayer::EndOfMedia,
                               30000);
      QTRY_VERIFY(player->duration() > 0);
    }
    QQuickItem* keys = item(QStringLiteral("viewerKeys"));
    const auto focused = [this] {
      QQuickItem* at = m_window->activeFocusItem();
      return at ? at->objectName() : QString();
    };

    QStringList order;
    for (int step = 0; step < 60; ++step) {
      QTest::keyClick(m_window, Qt::Key_Tab);
      if (keys->hasActiveFocus()) break;
      order.append(focused());
      QVERIFY2(prop("controlsFocused").toBool(), qPrintable(focused()));
      QVERIFY(prop("chromeShown").toBool());
      QVERIFY(m_window->activeFocusItem()->isVisible());
    }
    QVERIFY2(keys->hasActiveFocus(), qPrintable(order.join(u',')));
    QVERIFY2(order.size() >= 3, qPrintable(order.join(u',')));
    QCOMPARE(order.first(), QStringLiteral("viewerInfoButton"));
    const QStringList expected = video
        ? QStringList{QStringLiteral("viewerInfoButton"), QStringLiteral("viewerLibraryButton"),
                      QStringLiteral("viewerMoreButton")}
        : QStringList{QStringLiteral("viewerInfoButton"), QStringLiteral("viewerLibraryButton"),
                      QStringLiteral("viewerMoreButton"), QStringLiteral("viewerZoomOut"),
                      QStringLiteral("viewerFitToggle"), QStringLiteral("viewerZoomIn"),
                      QStringLiteral("viewerToolbarPrevious"), QStringLiteral("viewerSlideshowButton"),
                      QStringLiteral("viewerToolbarNext"), QStringLiteral("viewerRotateLeft"),
                      QStringLiteral("viewerRotateRight"), QStringLiteral("viewerTrashButton")};
    int at = -1;
    for (const QString& name : expected) {
      const int found = int(order.indexOf(name));
      QVERIFY2(found > at, qPrintable(name + u" in " + order.join(u',')));
      at = found;
    }

    // Shift+Tab goes the other way, from the picture to the last control.
    QTest::keyClick(m_window, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(focused(), order.last());
    QTest::keyClick(m_window, Qt::Key_Tab);
    QVERIFY(keys->hasActiveFocus());

    // On a control, the viewer's keys still step, seek and play.
    QTest::keyClick(m_window, Qt::Key_Tab);
    QCOMPARE(focused(), QStringLiteral("viewerInfoButton"));
    QQuickItem* button = m_window->activeFocusItem();
    QVERIFY(button->property("activeFocus").toBool());
    if (video) {
      QMediaPlayer* player = nullptr;
      QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
      QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
      QTest::keyClick(m_window, Qt::Key_K);
      QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
      QTest::keyClick(m_window, Qt::Key_Right);
      QTRY_VERIFY(player->position() > 0);
      QCOMPARE(m_session->path(), clip);
    } else {
      QTest::keyClick(m_window, Qt::Key_Right);
      QCOMPARE(m_session->path(), clip);
      QTest::keyClick(m_window, Qt::Key_PageUp);
      QCOMPARE(m_session->path(), picture);
    }
    QCOMPARE(m_window->activeFocusItem(), button);
    // Enter on a control presses it; Escape hands the keyboard back to the
    // picture without closing the viewer.
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(prop("infoOpen").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(keys->hasActiveFocus());
    QVERIFY(m_window->isVisible());
    QVERIFY(prop("infoOpen").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!prop("infoOpen").toBool());
  }

  // A focused control that is switched off or hidden hands the keyboard back
  // to the picture: shortcuts keep working, and Space or Enter cannot press a
  // control nobody can see.
  void hiddenOrDisabledControlsHandTheKeyboardBack() {
    const QString folder = m_scratch.filePath(QStringLiteral("focus"));
    QVERIFY(QDir().mkpath(folder));
    const QString fine = folder + QStringLiteral("/a fine.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), fine));
    const QString notes = folder + QStringLiteral("/b notes.jpg");
    {
      QFile file(notes);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("These are notes, saved with the wrong name.\n");
    }
    const QString other = folder + QStringLiteral("/c other.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), other));
    const QString picture = media(QStringLiteral("Shot 1.jpg"));
    const QString clip = media(QStringLiteral("clip.mp4"));
    QQuickItem* keys = item(QStringLiteral("viewerKeys"));
    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    // Within the filmstrip, focus sits on the current tile.
    const auto focusOn = [this](const QString& name) {
      QQuickItem* target = item(name);
      for (int step = 0; step < 60; ++step) {
        QTest::keyClick(m_window, Qt::Key_Tab);
        if (target->hasActiveFocus()) return true;
      }
      return false;
    };

    // Switched off: Rotate right is focused when the next file can't be shown.
    open({fine, notes});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(focusOn(QStringLiteral("viewerRotateRight")));
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), notes);
    QTRY_VERIFY(prop("imageError").toBool());
    QVERIFY(!item(QStringLiteral("viewerRotateRight"))->isEnabled());
    QTRY_VERIFY(keys->hasActiveFocus());
    QVERIFY(!prop("controlsFocused").toBool());
    QTest::keyClick(m_window, Qt::Key_Left);
    QCOMPARE(m_session->path(), fine);

    // Hidden with the error: its Trash button is focused when the next
    // picture is fine. Space must not ask to trash the good file.
    open({notes, other});
    QTRY_VERIFY(prop("imageError").toBool());
    QVERIFY(focusOn(QStringLiteral("viewerErrorTrash")));
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), other);
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(!item(QStringLiteral("viewerErrorTrash"))->isVisible());
    QTRY_VERIFY(keys->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Space);
    QTest::qWait(50);
    QVERIFY(!confirm->isVisible());

    // Hidden with the details: I closes the panel with Favourite focused.
    QTest::keyClick(m_window, Qt::Key_I);
    QTRY_VERIFY(prop("infoOpen").toBool());
    QVERIFY(focusOn(QStringLiteral("viewerInfoFavourite")));
    QTest::keyClick(m_window, Qt::Key_I);
    QVERIFY(!prop("infoOpen").toBool());
    QTRY_VERIFY(keys->hasActiveFocus());
    QVERIFY(!prop("controlsFocused").toBool());

    // Hidden with the filmstrip when a slideshow starts.
    m_settings->setViewerFilmstrip(true);
    const auto restore = qScopeGuard([this] { m_settings->setViewerFilmstrip(false); });
    open({fine, other});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTRY_VERIFY(item(QStringLiteral("viewerFilmstrip"))->isVisible());
    QVERIFY(focusOn(QStringLiteral("viewerFilmstripList")));
    QTest::keyClick(m_window, Qt::Key_F5);
    QVERIFY(prop("slideshowRunning").toBool());
    QVERIFY(!item(QStringLiteral("viewerFilmstrip"))->isVisible());
    QTRY_VERIFY(keys->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!prop("slideshowRunning").toBool());

    // Hidden with the toolbar: Enter on Next opens a video, and Space then
    // plays or pauses rather than pressing the hidden Next again.
    open({picture, clip});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(focusOn(QStringLiteral("viewerToolbarNext")));
    QTest::keyClick(m_window, Qt::Key_Return);
    QCOMPARE(m_session->path(), clip);
    QVERIFY(!item(QStringLiteral("viewerToolbarNext"))->isVisible());
    QTRY_VERIFY(keys->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Space);
    QCOMPARE(m_session->path(), clip);
  }

  // Pictures that cannot be shown, named by cause.
  void pictureDiagnosisNamesTheCause() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto write = [&dir](const QString& name, const QByteArray& bytes) {
      QFile file(dir.filePath(name));
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return QString();
      return file.fileName();
    };
    QFile good(media(QStringLiteral("Shot 1.jpg")));
    QVERIFY(good.open(QIODevice::ReadOnly));
    const QByteArray jpeg = good.readAll();
    QImage square(64, 48, QImage::Format_RGB32);
    square.fill(Qt::darkCyan);
    QByteArray png;
    {
      QBuffer buffer(&png);
      QVERIFY(buffer.open(QIODevice::WriteOnly));
      QVERIFY(square.save(&buffer, "PNG"));
    }

    using PictureDiagnosis::failure;
    using PictureDiagnosis::truncation;
    QCOMPARE(failure(dir.filePath(QStringLiteral("gone.jpg"))).cause, QStringLiteral("missing"));
    QCOMPARE(failure(write(QStringLiteral("empty.png"), {})).cause, QStringLiteral("empty"));
    const auto text = failure(write(QStringLiteral("notes.jpg"), "Just some notes, not a photo.\n"));
    QCOMPARE(text.cause, QStringLiteral("not-picture"));
    QVERIFY2(text.detail.contains(QStringLiteral("text")), qPrintable(text.detail));
    QVERIFY2(text.detail.contains(QStringLiteral("JPEG")), qPrintable(text.detail));
    // A DjVu page: a picture, but in a format no installed plugin reads.
    const auto djvu = failure(write(QStringLiteral("page.png"),
                                    QByteArray("AT&TFORM\0\0\0\x20" "DJVUINFO", 20) + QByteArray(40, '\0')));
    QCOMPARE(djvu.cause, QStringLiteral("unsupported"));
    // A JPEG whose segments give way to noise.
    QByteArray garbled = QByteArray::fromHex("ffd8ffe000104a46494600010100000100010000");
    garbled += QByteArray(64, '\x12');
    const auto damaged = failure(write(QStringLiteral("damaged.jpg"), garbled));
    QCOMPARE(damaged.cause, QStringLiteral("damaged"));
    QVERIFY(truncation(dir.filePath(QStringLiteral("damaged.jpg"))).isEmpty());
    // Over the memory limit, said with the numbers.
    const QString big = dir.filePath(QStringLiteral("big.png"));
    QImage wide(3000, 2000, QImage::Format_RGB32);
    wide.fill(Qt::gray);
    QVERIFY(wide.save(big));
    QImageReader::setAllocationLimit(10);
    const auto large = failure(big);
    QImageReader::setAllocationLimit(256);
    QCOMPARE(large.cause, QStringLiteral("too-large"));
    QVERIFY2(large.detail.contains(QStringLiteral("MiB")), qPrintable(large.detail));

    // Cut short, a JPEG or PNG says so; whole files, and a motion photo's
    // trailing video, do not.
    QVERIFY(truncation(write(QStringLiteral("whole.jpg"), jpeg)).isEmpty());
    QVERIFY(truncation(write(QStringLiteral("motion.jpg"), jpeg + QByteArray(4096, '\x42'))).isEmpty());
    QVERIFY(!truncation(write(QStringLiteral("cut.jpg"), jpeg.left(jpeg.size() * 6 / 10))).isEmpty());
    QVERIFY(truncation(write(QStringLiteral("whole.png"), png)).isEmpty());
    QVERIFY(!truncation(write(QStringLiteral("cut.png"), png.left(png.size() - 20))).isEmpty());
    QVERIFY(truncation(write(QStringLiteral("other.gif"), QByteArray("GIF89a") + QByteArray(30, '\0'))).isEmpty());
  }

  // A video that will not open is named for its cause too: gone, empty, not a
  // video, no decoder for its codec, cut off or damaged.
  void videoDiagnosisNamesTheCause() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto write = [&dir](const QString& name, const QByteArray& bytes) {
      QFile file(dir.filePath(name));
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return QString();
      return file.fileName();
    };
    QFile clipFile(media(QStringLiteral("clip.mp4")));
    QVERIFY(clipFile.open(QIODevice::ReadOnly));
    const QByteArray clip = clipFile.readAll();
    // ffmpeg as a script that answers the way a real one does.
    const auto fakeFfmpeg = [&dir](const QString& name, const QByteArray& message, int code) {
      const QString path = dir.filePath(name);
      QFile script(path);
      if (!script.open(QIODevice::WriteOnly)) return QString();
      script.write("#!/bin/sh\nprintf '%s\\n' '" + message + "' >&2\nexit " + QByteArray::number(code) + "\n");
      script.close();
      script.setPermissions(script.permissions() | QFileDevice::ExeOwner);
      return path;
    };
    using PictureDiagnosis::videoFailure;
    const QString none = dir.filePath(QStringLiteral("no-ffmpeg"));
    QCOMPARE(videoFailure(dir.filePath(QStringLiteral("gone.mp4")), {}, none).cause, QStringLiteral("missing"));
    QCOMPARE(videoFailure(write(QStringLiteral("empty.mp4"), {}), {}, none).cause, QStringLiteral("empty"));
    const auto text = videoFailure(write(QStringLiteral("notes.mp4"), "Just some notes, not a video.\n"), {}, none);
    QCOMPARE(text.cause, QStringLiteral("not-video"));
    QVERIFY2(text.title == QStringLiteral("This file isn't a video"), qPrintable(text.title));
    QVERIFY2(text.detail.contains(QStringLiteral("text")), qPrintable(text.detail));
    QVERIFY2(text.detail.endsWith(QStringLiteral("not an MPEG-4 video.")), qPrintable(text.detail));
    // A Windows Media file reads as an ASF container, application/vnd.ms-asf:
    // a damaged one is still a video.
    const QByteArray asfHeader = QByteArray::fromHex("3026b2758e66cf11a6d900aa0062ce6c") + QByteArray(512, '\0');
    const auto wmv = videoFailure(write(QStringLiteral("cut.wmv"), asfHeader), {}, none);
    QVERIFY2(wmv.cause != QStringLiteral("not-video"), qPrintable(wmv.cause + QStringLiteral(": ") + wmv.detail));

    const QString video = write(QStringLiteral("clip.mp4"), clip);
    const auto codec = videoFailure(video, QStringLiteral("Could not open file"),
        fakeFfmpeg(QStringLiteral("codec"), "[vist#0:0/hevc @ 0x55] Decoder (codec hevc) not found for input stream #0:0", 1));
    QCOMPARE(codec.cause, QStringLiteral("unsupported"));
    QVERIFY2(codec.detail.contains(QStringLiteral("hevc")), qPrintable(codec.detail));
    const auto cut = videoFailure(video, {},
        fakeFfmpeg(QStringLiteral("cut"), "[mov,mp4,m4a,3gp,3g2,mj2 @ 0x55] moov atom not found", 1));
    QCOMPARE(cut.cause, QStringLiteral("truncated"));
    const auto damaged = videoFailure(video, {},
        fakeFfmpeg(QStringLiteral("damaged"), "[in#0 @ 0x55] Error opening input: Invalid data found when processing input", 1));
    QCOMPARE(damaged.cause, QStringLiteral("damaged"));
    QVERIFY2(damaged.detail.contains(QStringLiteral("Invalid data found")), qPrintable(damaged.detail));
    QVERIFY2(!damaged.detail.contains(QStringLiteral("0x55")), qPrintable(damaged.detail));
    // FFmpeg reads it, so the player's own failure is all there is to say.
    const auto other = videoFailure(video, QStringLiteral("Could not open file"),
                                    fakeFfmpeg(QStringLiteral("fine"), "", 0));
    QCOMPARE(other.cause, QStringLiteral("unknown"));
    QVERIFY2(other.detail.contains(QStringLiteral("Could not open file")), qPrintable(other.detail));
    // FFmpeg can recover a frame and exit cleanly while saying what was wrong.
    const auto recovered = videoFailure(video, QStringLiteral("Could not open file"),
        fakeFfmpeg(QStringLiteral("recovered"), "[h264 @ 0x55] error while decoding MB 3 7, bytestream -5", 0));
    QCOMPARE(recovered.cause, QStringLiteral("damaged"));
    QVERIFY2(recovered.detail.contains(QStringLiteral("error while decoding")), qPrintable(recovered.detail));
    QCOMPARE(videoFailure(video, {}, fakeFfmpeg(QStringLiteral("short"), "[mov @ 0x55] Packet corrupt; End of file", 0)).cause,
             QStringLiteral("truncated"));
    // Opened, with nothing in it to decode.
    const auto streamless = videoFailure(video, QStringLiteral("No video or audio stream could be read"),
        fakeFfmpeg(QStringLiteral("streamless"), "[out#0/null @ 0x55] Output file does not contain any stream", 234));
    QCOMPARE(streamless.cause, QStringLiteral("damaged"));
    QVERIFY2(streamless.detail.startsWith(QStringLiteral("No video can be found")), qPrintable(streamless.detail));
    // Without ffmpeg the player's words decide.
    QCOMPARE(videoFailure(video, QStringLiteral("No decoder for this codec"), none).cause,
             QStringLiteral("unsupported"));

    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) return;
    // The real thing: a header followed by noise.
    const auto real = videoFailure(write(QStringLiteral("noise.mp4"), clip.left(32) + QByteArray(8192, '\x5a')), {});
    QVERIFY2(real.cause == QStringLiteral("damaged") || real.cause == QStringLiteral("truncated"),
             qPrintable(real.cause + QStringLiteral(": ") + real.detail));
    QCOMPARE(videoFailure(video, {}).cause, QStringLiteral("unknown"));
    const QString streamlessFile = dir.filePath(QStringLiteral("streamless.mp4"));
    QVERIFY(writeStreamlessVideo(media(QStringLiteral("clip.mp4")), streamlessFile));
    const auto realStreamless = videoFailure(streamlessFile, QStringLiteral("No video or audio stream could be read"));
    QCOMPARE(realStreamless.cause, QStringLiteral("damaged"));
    QVERIFY2(realStreamless.detail.startsWith(QStringLiteral("No video can be found")), qPrintable(realStreamless.detail));
  }

  // A video that will not open says why like a picture does, hides the seek
  // bar, and its arrow keys step to the next file instead of seeking.
  void aVideoThatCannotOpenSaysWhyAndStepsFiles() {
    const QString folder = m_scratch.filePath(QStringLiteral("broken-video"));
    QVERIFY(QDir().mkpath(folder));
    QFile clipFile(media(QStringLiteral("clip.mp4")));
    QVERIFY(clipFile.open(QIODevice::ReadOnly));
    const QString broken = folder + QStringLiteral("/a broken.mp4");
    QFile file(broken);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(clipFile.read(32) + QByteArray(8192, '\x5a'));
    file.close();
    const QString fine = folder + QStringLiteral("/b fine.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), fine));
    open({broken});
    QTRY_VERIFY_WITH_TIMEOUT(!prop("videoError").toString().isEmpty(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(prop("failureKnown").toBool(), 20000);
    const QString cause = prop("errorCause").toString();
    QVERIFY2(cause == QStringLiteral("damaged") || cause == QStringLiteral("truncated"), qPrintable(cause));
    QVERIFY(item(QStringLiteral("viewerErrorTitle"))->property("text").toString().startsWith(QStringLiteral("This video is")));
    QVERIFY(!item(QStringLiteral("viewerErrorDetail"))->property("text").toString().isEmpty());
    QVERIFY(item(QStringLiteral("viewerErrorState"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerErrorTrash"))->isVisible());
    QVERIFY(!item(QStringLiteral("viewerTransport"))->isVisible());
    // P still hands it to mpv, which the error offers.
    {
      QTemporaryDir bin;
      QVERIFY(bin.isValid());
      const QString log = bin.filePath(QStringLiteral("mpv.log"));
      QFile mpv(bin.filePath(QStringLiteral("mpv")));
      QVERIFY(mpv.open(QIODevice::WriteOnly));
      mpv.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" + log.toUtf8() + "\"\n");
      mpv.close();
      QVERIFY(mpv.setPermissions(mpv.permissions() | QFileDevice::ExeOwner));
      const QByteArray path = qgetenv("PATH");
      qputenv("PATH", bin.path().toUtf8() + ':' + path);
      const auto restorePath = qScopeGuard([path] { qputenv("PATH", path); });
      QTest::keyClick(m_window, Qt::Key_P);
      QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(log), 5000);
      // The shell creates the log before it writes, so read it afresh each try.
      const auto logged = [&log] {
        QFile args(log);
        return args.open(QIODevice::ReadOnly) ? args.readAll() : QByteArray();
      };
      QTRY_VERIFY(logged().contains(broken.toUtf8()));
    }
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(m_session->path(), fine);
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(!item(QStringLiteral("viewerErrorState"))->isVisible());
  }

  // An error while frames still arrive, as when only the audio track fails,
  // is said as a notice; the seek bar and the seeking keys stay.
  void aVideoErrorWhileFramesArriveKeepsTheControls() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QObject* presentation = m_window->property("presentation").value<QObject*>();
    QVERIFY(presentation);
    QTRY_VERIFY_WITH_TIMEOUT(presentation->property("ready").toBool(), 10000);
    player->pause();
    QVERIFY(m_window->setProperty("videoError", QStringLiteral("Audio decoder failed")));
    QVERIFY(!prop("videoUnplayable").toBool());
    QVERIFY(!item(QStringLiteral("viewerErrorState"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerTransport"))->isVisible());
    QTRY_COMPARE(prop("status").toString(), QStringLiteral("Audio decoder failed"));
    const qint64 before = player->position();
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_VERIFY(player->position() >= before + 4000);
    QCOMPARE(m_session->path(), media(QStringLiteral("clip.mp4")));
    // Once playback stops, nothing is left to watch and the error takes over.
    player->stop();
    QTRY_VERIFY(prop("videoUnplayable").toBool());
    QVERIFY(item(QStringLiteral("viewerErrorState"))->isVisible());
    QVERIFY(!item(QStringLiteral("viewerTransport"))->isVisible());
  }

  // Stepping from a playing video to one that will not open shows the error
  // over an empty stage: the last file's frame and the header's dark scrim
  // go, and the header takes the theme's colours.
  void aBrokenVideoAfterAPlayingOneShowsNoOldFrame() {
    const QString folder = m_scratch.filePath(QStringLiteral("stale-frame"));
    QVERIFY(QDir().mkpath(folder));
    const QString playing = folder + QStringLiteral("/a tracks.mkv");
    QVERIFY(QFile::exists(playing) || QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), playing));
    const QString broken = folder + QStringLiteral("/b notvideo.mp4");
    {
      QFile file(broken);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("Just some notes, not a video.\n");
    }
    open({playing, broken});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QObject* presentation = m_window->property("presentation").value<QObject*>();
    QVERIFY(presentation);
    QTRY_VERIFY_WITH_TIMEOUT(presentation->property("ready").toBool(), 10000);
    m_window->setProperty("chromePinned", true);
    const auto unpin = qScopeGuard([this] { m_window->setProperty("chromePinned", false); });
    QQuickItem* output = item(QStringLiteral("viewerVideoOutput"));
    QQuickItem* scrim = item(QStringLiteral("viewerHeaderScrim"));
    QTRY_VERIFY(output->isVisible());
    QVERIFY(prop("stageShowsMedia").toBool());
    QVERIFY(scrim->isVisible());
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) QTRY_VERIFY_WITH_TIMEOUT(renderedVideoHasColor(), 5000);

    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTRY_COMPARE(m_session->path(), broken);
    QTRY_VERIFY_WITH_TIMEOUT(prop("videoUnplayable").toBool(), 15000);
    QTRY_COMPARE(prop("errorCause").toString(), QStringLiteral("not-video"));
    QVERIFY(item(QStringLiteral("viewerErrorState"))->isVisible());
    // The decoder's last frame may still be held, but nothing shows it.
    QVERIFY(!output->isVisible());
    QVERIFY(!item(QStringLiteral("viewerVideoPoster"))->isVisible());
    QVERIFY(!prop("stageShowsMedia").toBool());
    QVERIFY(!scrim->isVisible());
    QCOMPARE(item(QStringLiteral("viewerTitle"))->property("color").value<QColor>(), m_theme->brightForeground());
    QVERIFY(!item(QStringLiteral("viewerMoreButton"))->property("osd").toBool());
    // A text file is no video: mpv is not offered, and the copy reads well.
    QVERIFY(!item(QStringLiteral("viewerErrorPlay"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerErrorOpenWith"))->isVisible()
            || !m_registry->available(QStringLiteral("open-with")));
    QVERIFY2(item(QStringLiteral("viewerErrorDetail"))->property("text").toString()
                 .endsWith(QStringLiteral("not an MPEG-4 video.")),
             qPrintable(item(QStringLiteral("viewerErrorDetail"))->property("text").toString()));
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QSignalSpy frames(m_window, &QQuickWindow::frameSwapped);
      m_window->requestUpdate();
      QTRY_VERIFY(frames.size() >= 1);
      QVERIFY(!renderedVideoHasColor());
    }

    // Back to the playing file: its picture and the scrim return.
    QTest::keyClick(m_window, Qt::Key_PageUp);
    QTRY_COMPARE(m_session->path(), playing);
    QTRY_VERIFY_WITH_TIMEOUT(output->isVisible(), 10000);
    QTRY_VERIFY(scrim->isVisible());
    player->pause();
  }

  // A file that opens with no stream at all raises no error from the player,
  // whose clock would run on as if playing. It fails like any broken video,
  // with the diagnosis, and the player stops, so MPRIS says Stopped.
  void aVideoWithNoStreamsFailsAndStops() {
    const QString folder = m_scratch.filePath(QStringLiteral("streamless"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/streamless.mp4");
    QVERIFY(writeStreamlessVideo(media(QStringLiteral("clip.mp4")), path));
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(prop("videoUnplayable").toBool(), 10000);
    QCOMPARE(prop("videoError").toString(), QStringLiteral("No video or audio stream could be read"));
    QCOMPARE(player->playbackState(), QMediaPlayer::StoppedState);
    QTRY_VERIFY_WITH_TIMEOUT(prop("failureKnown").toBool(), 20000);
    QCOMPARE(prop("errorCause").toString(), QStringLiteral("damaged"));
    QVERIFY(item(QStringLiteral("viewerErrorState"))->isVisible());
    QVERIFY(!item(QStringLiteral("viewerTransport"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerErrorPlay"))->isVisible() || !m_registry->available(QStringLiteral("play")));
    // A tap or Space doesn't start its clock again under the error.
    QVERIFY(QMetaObject::invokeMethod(m_window, "togglePlayback"));
    QTest::qWait(300);
    QCOMPARE(player->playbackState(), QMediaPlayer::StoppedState);
    QDBusConnection client(QStringLiteral("omaroll-test-client"));
    if (!client.isConnected()) QSKIP("dbus-daemon is not available");
    QTRY_VERIFY(m_mpris->isPublished());
    {
      const QString remote = QStringLiteral("org.mpris.MediaPlayer2.Player");
      QTRY_COMPARE(mprisProperty(remote, QStringLiteral("PlaybackStatus")).toString(), QStringLiteral("Stopped"));
      // Nor does MPRIS Play.
      mprisCall(remote, QStringLiteral("Play"));
      QTRY_COMPARE(player->playbackState(), QMediaPlayer::StoppedState);
      QTest::qWait(500);
      QCOMPARE(player->playbackState(), QMediaPlayer::StoppedState);
      QTRY_COMPARE(mprisProperty(remote, QStringLiteral("PlaybackStatus")).toString(), QStringLiteral("Stopped"));
      QVERIFY(prop("videoUnplayable").toBool());
    }
  }

  // The presentation's own watch, without a window. Time counts only while
  // playing with the clock standing still, so audio that plays before late
  // video, or a pause, never fails a file. Played to its end without a frame,
  // or opened with no stream at all, it fails, and a failed file stays
  // stopped when asked to play.
  void presentationFailsWithoutAFirstFrame() {
    FirstFrameWatch watch;
    QCOMPARE(watch.update(0, true, 0), 0);
    QCOMPARE(watch.update(300, true, 0), 300);      // Stuck.
    QCOMPARE(watch.update(400, false, 0), 300);     // Paused: the time so far is kept...
    QCOMPARE(watch.update(5000, false, 0), 300);    // ...and the pause adds none.
    QCOMPARE(watch.update(5100, true, 0), 300);     // Resumed: counting starts again here.
    QCOMPARE(watch.update(5300, true, 0), 500);
    QCOMPARE(watch.update(5600, true, 250), 500);   // The clock moves: content is playing.
    QCOMPARE(watch.update(9600, true, 4250), 500);
    QCOMPARE(watch.update(9700, true, 4250), 600);  // Stuck again, added to before.
    watch.reset();
    QCOMPARE(watch.update(9800, true, 0), 0);

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto encode = [&ffmpeg](const QStringList& arguments) {
      QProcess encoder;
      encoder.start(ffmpeg, QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                                        QStringLiteral("error"), QStringLiteral("-y")} + arguments);
      return encoder.waitForFinished(30000) && encoder.exitStatus() == QProcess::NormalExit
             && encoder.exitCode() == 0;
    };
    const QString clip = dir.filePath(QStringLiteral("clip.mkv"));
    QVERIFY(encode({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=d=4"),
                    QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                    QStringLiteral("testsrc2=s=64x64:r=10:d=4"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
                    QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-c:a"), QStringLiteral("aac"),
                    clip}));

    QVideoSink output;
    VideoPresentation presentation(&output);
    QSignalSpy failures(&presentation, &VideoPresentation::failureChanged);
    QMediaPlayer player;
    presentation.setPlayer(&player);
    QVERIFY(presentation.setProperty("firstFrameTimeoutMs", 600));

    // Frames that arrive end the watch.
    player.setVideoOutput(presentation.videoSink());
    player.setSource(QUrl::fromLocalFile(clip));
    QTRY_VERIFY_WITH_TIMEOUT(player.mediaStatus() == QMediaPlayer::LoadedMedia, 5000);
    QVERIFY(player.hasVideo() && player.hasAudio());
    QTest::qWait(1000);
    QVERIFY(presentation.failure().isEmpty()); // Not playing yet.
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(presentation.ready(), 5000);
    QVERIFY(output.videoFrame().isValid());
    player.stop();

    // No frame reaches it while the clock runs, as with audio ahead of late
    // video: well past the allowed time it still waits. Played to the end
    // without a picture, it fails.
    QVideoSink elsewhere;
    player.setVideoOutput(&elsewhere);
    presentation.reset();
    player.setPosition(0);
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.position() >= 2000, 5000);
    QVERIFY(elsewhere.videoFrame().isValid());
    QVERIFY(presentation.failure().isEmpty());
    QCOMPARE(failures.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(presentation.failure(), QStringLiteral("No picture came from this video"), 10000);
    QCOMPARE(failures.size(), 1);
    QVERIFY(!presentation.ready());
    // Asked to play again, it stops again.
    QTRY_COMPARE(player.playbackState(), QMediaPlayer::StoppedState);
    player.play();
    QTRY_COMPARE(player.playbackState(), QMediaPlayer::StoppedState);
    QTest::qWait(300);
    QCOMPARE(player.playbackState(), QMediaPlayer::StoppedState);
    presentation.reset();
    QVERIFY(presentation.failure().isEmpty());
    QCOMPARE(failures.size(), 2);

    const QString streamless = dir.filePath(QStringLiteral("streamless.mp4"));
    QVERIFY(writeStreamlessVideo(media(QStringLiteral("clip.mp4")), streamless));
    player.setSource(QUrl::fromLocalFile(streamless));
    QTRY_COMPARE_WITH_TIMEOUT(presentation.failure(), QStringLiteral("No video or audio stream could be read"), 5000);
  }

  // Qt draws the padding byte of bgr0, rgb0, 0rgb and 0bgr frames as alpha.
  // The copy sent to the screen keeps the colours and makes it opaque; real
  // alpha and other formats pass through untouched.
  void paddedRgbFramesAreDrawnOpaque_data() {
    QTest::addColumn<int>("format");
    QTest::addColumn<int>("padding");
    QTest::newRow("xrgb") << int(QVideoFrameFormat::Format_XRGB8888) << 0;
    QTest::newRow("xbgr") << int(QVideoFrameFormat::Format_XBGR8888) << 0;
    QTest::newRow("rgbx") << int(QVideoFrameFormat::Format_RGBX8888) << 3;
    QTest::newRow("bgrx") << int(QVideoFrameFormat::Format_BGRX8888) << 3;
    QTest::newRow("bgra") << int(QVideoFrameFormat::Format_BGRA8888) << -1;
    QTest::newRow("argb") << int(QVideoFrameFormat::Format_ARGB8888) << -1;
  }

  void paddedRgbFramesAreDrawnOpaque() {
    QFETCH(int, format);
    QFETCH(int, padding);
    QVideoFrame frame(QVideoFrameFormat(QSize(5, 3), QVideoFrameFormat::PixelFormat(format)));
    QVERIFY(frame.map(QVideoFrame::WriteOnly));
    for (int y = 0; y < 3; ++y) {
      uchar* row = frame.bits(0) + y * frame.bytesPerLine(0);
      for (int x = 0; x < 5 * 4; ++x) row[x] = uchar(x % 4 == qMax(0, padding) ? 0 : 40 + x + y);
    }
    frame.unmap();
    frame.setStartTime(1000);
    frame.setEndTime(2000);
    const QVideoFrame shown = VideoPresentation::opaqueFrame(frame);
    QCOMPARE(shown.pixelFormat(), frame.pixelFormat());
    QCOMPARE(shown.size(), frame.size());
    QCOMPARE(shown.startTime(), 1000);
    QCOMPARE(shown.endTime(), 2000);
    QVideoFrame read(shown);
    QVERIFY(read.map(QVideoFrame::ReadOnly));
    for (int y = 0; y < 3; ++y) {
      const uchar* row = read.bits(0) + y * read.bytesPerLine(0);
      for (int x = 0; x < 5 * 4; ++x) {
        const int wanted = padding >= 0 && x % 4 == padding ? 0xff
                           : x % 4 == qMax(0, padding)     ? 0
                                                           : 40 + x + y;
        QCOMPARE(int(row[x]), wanted);
      }
    }
    read.unmap();

    if (padding < 0) return;
    // A buffer whose mappings are copies gets a copy of its own instead.
    QByteArray bytes(5 * 4 * 3, char(40));
    for (int i = padding; i < bytes.size(); i += 4) bytes[i] = 0;
    QVideoFrame copied(std::make_unique<CopyMappedBuffer>(
        QVideoFrameFormat(QSize(5, 3), QVideoFrameFormat::PixelFormat(format)), bytes, 5 * 4));
    const QVideoFrame opaque = VideoPresentation::opaqueFrame(copied);
    QVideoFrame result(opaque);
    QVERIFY(result.map(QVideoFrame::ReadOnly));
    for (int i = 0; i < 5 * 4 * 3; ++i) {
      const uchar byte = result.bits(0)[(i / 20) * result.bytesPerLine(0) + i % 20];
      QCOMPARE(int(byte), i % 4 == padding ? 0xff : 40);
    }
    result.unmap();
  }

  // The error state names the cause, offers what still applies to the file
  // and turns off the view controls that do not.
  void aPictureThatCannotBeShownSaysWhyAndKeepsItsActions() {
    const QString folder = m_scratch.filePath(QStringLiteral("broken"));
    QVERIFY(QDir().mkpath(folder));
    const QString notes = folder + QStringLiteral("/a notes.jpg");
    QFile file(notes);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("These are notes, saved with the wrong name.\n");
    file.close();
    const QString fine = folder + QStringLiteral("/b fine.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), fine));
    m_settings->setViewerFilmstrip(true);
    const auto restore = qScopeGuard([this] { m_settings->setViewerFilmstrip(false); });
    open({notes});
    QTRY_VERIFY(prop("imageError").toBool());
    QTRY_COMPARE(prop("errorCause").toString(), QStringLiteral("not-picture"));
    QTRY_COMPARE(item(QStringLiteral("viewerErrorTitle"))->property("text").toString(),
                 QStringLiteral("This file isn't a picture"));
    QVERIFY(item(QStringLiteral("viewerErrorDetail"))->property("text").toString()
                .contains(QStringLiteral("text")));
    QVERIFY(item(QStringLiteral("viewerErrorState"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerErrorTrash"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerErrorShowInFolder"))->isVisible()
            || !m_registry->available(QStringLiteral("files")));
    for (const auto& name : {QStringLiteral("viewerZoomIn"), QStringLiteral("viewerZoomOut"),
                             QStringLiteral("viewerFitToggle"), QStringLiteral("viewerRotateLeft"),
                             QStringLiteral("viewerRotateRight")}) {
      QVERIFY2(!item(name)->isEnabled(), qPrintable(name));
    }
    QVERIFY(item(QStringLiteral("viewerTrashButton"))->isEnabled());
    QTest::keyClick(m_window, Qt::Key_R);
    QCOMPARE(prop("viewRotation").toInt(), 0);
    // The broken file's filmstrip tile is marked, not blank, and named.
    QQuickItem* broken = nullptr;
    QTRY_VERIFY((broken = find(m_window->contentItem(), [](QQuickItem* candidate) {
                   return candidate->objectName() == QStringLiteral("viewerFilmstripBroken0")
                          && candidate->isVisible();
                 })));
    QQuickItem* tile = item(QStringLiteral("viewerFilmstripTile0"));
    QVERIFY(QAccessible::queryAccessibleInterface(tile)->text(QAccessible::Name)
                .startsWith(QStringLiteral("a notes.jpg")));
    QVERIFY(QAccessible::queryAccessibleInterface(tile)->state().selected);
    // Tab reaches the error's actions; Delete still asks to trash it.
    bool reached = false;
    for (int step = 0; step < 30 && !reached; ++step) {
      QTest::keyClick(m_window, Qt::Key_Tab);
      reached = m_window->activeFocusItem()
                && m_window->activeFocusItem()->objectName() == QStringLiteral("viewerErrorTrash");
      QVERIFY(m_window->activeFocusItem()->objectName() != QStringLiteral("viewerZoomIn"));
    }
    QVERIFY(reached);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());

    // The next picture is fine, and the controls come back.
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(!item(QStringLiteral("viewerErrorState"))->isVisible());
    QVERIFY(item(QStringLiteral("viewerZoomIn"))->isEnabled());
    QVERIFY(item(QStringLiteral("viewerFilmstripBroken0"))->isVisible());
  }

  // A JPEG cut short decodes with a grey band; the viewer says it is cut off.
  void aCutOffPictureSaysSo() {
    const QString folder = m_scratch.filePath(QStringLiteral("cut"));
    QVERIFY(QDir().mkpath(folder));
    QFile good(media(QStringLiteral("Shot 1.jpg")));
    QVERIFY(good.open(QIODevice::ReadOnly));
    const QByteArray bytes = good.readAll();
    const QString cut = folder + QStringLiteral("/cut.jpg");
    QFile output(cut);
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(bytes.left(bytes.size() / 2));
    output.close();
    const QString whole = folder + QStringLiteral("/whole.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), whole));
    open({cut});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTRY_VERIFY(!m_session->pictureWarning().isEmpty());
    QTRY_VERIFY(item(QStringLiteral("viewerPictureWarning"))->isVisible());
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(m_session->path(), whole);
    QTRY_VERIFY(!item(QStringLiteral("viewerPictureWarning"))->isVisible());
  }

  // Menu rows carry their names; separators are not rows at all.
  void viewerMenuItemsHaveAccessibleNames() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QMetaObject::invokeMethod(m_window, "showActionMenu");
    QTRY_VERIFY(menu->property("visible").toBool());
    // Menu rows live in the window's overlay, beside the content.
    QQuickItem* top = m_window->contentItem();
    // Rotate is in View.
    QVERIFY(QMetaObject::invokeMethod(item(QStringLiteral("viewerMenu_group-view")), "click"));
    QTRY_VERIFY(m_window->findChild<QObject*>(QStringLiteral("viewerSubmenu_view"))->property("visible").toBool());
    QQuickItem* rotate = nullptr;
    QTRY_VERIFY((rotate = find(top, [](QQuickItem* candidate) {
                   return candidate->objectName() == QStringLiteral("viewerMenu_rotate");
                 })));
    QAccessibleInterface* row = QAccessible::queryAccessibleInterface(rotate);
    QVERIFY(row);
    QCOMPARE(row->text(QAccessible::Name), QStringLiteral("Rotate"));
    QCOMPARE(row->text(QAccessible::Description), QStringLiteral("Shortcut R"));
    // Every row the menu exposes has a name, and the separators are left
    // out of it altogether.
    // The rows of the View submenu that holds Rotate.
    QList<QQuickItem*> entries;
    for (QQuickItem* candidate : rotate->parentItem()->childItems())
      if (candidate->inherits("QQuickMenuItem")) entries.append(candidate);
    int separators = 0;
    for (QQuickItem* entry : entries) separators += entry->property("separator").toBool() ? 1 : 0;
    QVERIFY(separators > 0);
    QAccessibleInterface* list = row->parent();
    QVERIFY(list);
    int rows = 0;
    for (int i = 0; i < list->childCount(); ++i) {
      QAccessibleInterface* child = list->child(i);
      if (child && child->role() == QAccessible::Separator) continue;
      if (!child || child->role() != QAccessible::MenuItem) continue;
      ++rows;
      QVERIFY2(!child->text(QAccessible::Name).isEmpty(), qPrintable(QString::number(i)));
    }
    // A row scrolled out of the menu may be left out; none is unnamed.
    QVERIFY2(rows >= int(entries.size()) - separators - 2, qPrintable(QString::number(rows)));
    for (QQuickItem* entry : entries) {
      if (!entry->property("separator").toBool()) continue;
      QAccessibleInterface* face = QAccessible::queryAccessibleInterface(entry);
      QVERIFY(!face || face->role() == QAccessible::Separator);
    }
    // Escape leaves View, then the menu.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!m_window->findChild<QObject*>(QStringLiteral("viewerSubmenu_view"))->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
  }

  void controlsFadeAndReturnWithThePointer() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->setProperty("chromeTimeout", 150);
    const QPoint middle(m_window->width() / 2, m_window->height() / 2);
    // A prior mouse test may leave the synthetic pointer at the same point.
    // Move first so reopening the window cannot leave a stale header hover.
    QTest::mouseMove(m_window, middle + QPoint(12, 6));
    QTest::mouseMove(m_window, middle);
    QVERIFY(prop("chromeShown").toBool());
    QTRY_VERIFY(!prop("chromeShown").toBool());
    QTRY_VERIFY(!item(QStringLiteral("viewerHeader"))->isVisible());
    QTest::mouseMove(m_window, middle + QPoint(12, 6));
    QVERIFY(prop("chromeShown").toBool());
    // Resting on a control keeps the controls up.
    QTest::mouseMove(m_window, centre(item(QStringLiteral("viewerInfoButton"))));
    QTest::qWait(400);
    QVERIFY(prop("chromeShown").toBool());
  }

  // A playing video redraws every frame, and Qt Quick repeats the hover with
  // each one. The controls must still fade while the pointer rests.
  void controlsFadeWhileAVideoPlays() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    m_window->setProperty("chromeTimeout", 150);
    QTest::mouseMove(m_window, QPoint(m_window->width() / 2, m_window->height() / 3));
    QVERIFY(prop("chromeShown").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!prop("chromeShown").toBool(), 2000);
    QVERIFY(player->playbackState() == QMediaPlayer::PlayingState);
    QVERIFY(prop("pointerHidden").toBool());
    player->pause();
  }

  void zoomFollowsThePointerAndFitsAgain() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    const qreal fit = prop("fitScale").toReal();
    QVERIFY(fit > 0);

    QTest::keyClick(m_window, Qt::Key_Plus);
    QTRY_VERIFY(prop("viewScale").toReal() > fit);
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);
    QTest::keyClick(m_window, Qt::Key_Plus);
    QTRY_VERIFY(prop("viewScale").toReal() > fit);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("effectiveScale").toReal(), 1.0 / m_window->devicePixelRatio());
    QCOMPARE(prop("zoomPercent").toInt(), 100);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("effectiveScale").toReal(), 1.0 / m_window->devicePixelRatio());
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);

    // A wheel notch eases in rather than jumping, and notches that arrive
    // while it moves add up.
    QQuickItem* frame = item(QStringLiteral("viewerFrame"));
    const QPointF at(m_window->width() * 0.3, m_window->height() * 0.6);
    const QPointF before = frame->mapFromScene(at) / prop("effectiveScale").toReal();
    wheel(at, 1, 0);
    QVERIFY(prop("zooming").toBool());
    const qreal midway = prop("effectiveScale").toReal();
    QVERIFY(midway < fit * 1.15 - 0.001);
    wheel(at, 1);
    QTRY_VERIFY(!prop("zooming").toBool());
    QVERIFY(qAbs(prop("effectiveScale").toReal() - fit * 1.15 * 1.15) < 0.001);
    // The point under the pointer stays under it the whole way.
    const QPointF after = frame->mapFromScene(at) / prop("effectiveScale").toReal();
    QVERIFY2(qAbs(before.x() - after.x()) < 1.5 && qAbs(before.y() - after.y()) < 1.5,
             qPrintable(QStringLiteral("%1,%2 became %3,%4")
                            .arg(before.x()).arg(before.y()).arg(after.x()).arg(after.y())));
    // Zooming back out past the fit returns to fitting the window.
    wheel(at, -8);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);

    // A double click zooms to the real size and back.
    doubleClick(at.toPoint());
    QTRY_VERIFY(prop("viewScale").toReal() > 0);
    doubleClick(at.toPoint());
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);

    // Rotating swaps the fitted box.
    const qreal width = prop("rotatedWidth").toReal();
    QTest::keyClick(m_window, Qt::Key_R);
    QCOMPARE(prop("viewRotation").toInt(), 90);
    QCOMPARE(prop("rotatedHeight").toReal(), width);
    // A new file starts upright and fitted.
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(prop("viewRotation").toInt(), 0);
  }

  // Qt on Wayland can report a mouse wheel as coming from a touchpad. The
  // notches must still zoom; only a phased swipe pans.
  void aWheelReportedAsATouchpadStillZooms() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_VERIFY(prop("imageReady").toBool());
    const qreal fit = prop("fitScale").toReal();
    QPointingDevice touchpad(QStringLiteral("touchpad"), 4242, QInputDevice::DeviceType::TouchPad,
                             QPointingDevice::PointerType::Finger,
                             QInputDevice::Capability::Position | QInputDevice::Capability::Scroll,
                             1, 3);
    const QPointF at(m_window->width() / 2.0, m_window->height() / 2.0);
    QWheelEvent notch(at, m_window->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false,
                      Qt::MouseEventNotSynthesized, &touchpad);
    QCoreApplication::sendEvent(m_window, &notch);
    QTRY_VERIFY(!prop("zooming").toBool());
    QVERIFY(qAbs(prop("effectiveScale").toReal() - fit * 1.15) < 0.001);
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);
  }

  // A long screenshot, zoomed in, moves with the mouse: drag up to go down
  // the picture. At the fitted size there is nothing to move.
  void draggingAZoomedLongPictureMovesAlongIt() {
    const QString folder = m_scratch.filePath(QStringLiteral("long"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/long.png");
    QImage picture(600, 4000, QImage::Format_RGB32);
    picture.fill(QColor(40, 90, 160));
    QVERIFY(picture.save(path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    QQuickItem* still = item(QStringLiteral("viewerStill"));
    QVERIFY(!still->property("interactive").toBool());

    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(!prop("zooming").toBool());
    QTRY_VERIFY(still->property("interactive").toBool());
    const qreal top = still->property("contentY").toReal();
    const QPoint from(m_window->width() / 2, m_window->height() * 3 / 4);
    const QPoint to(from.x(), m_window->height() / 4);
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, from);
    QTest::qWait(20);
    for (int step = 1; step <= 8; ++step) {
      QTest::mouseMove(m_window, from + (to - from) * step / 8);
      QTest::qWait(20);
    }
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, to);
    QTRY_VERIFY(!still->property("moving").toBool());
    const qreal moved = still->property("contentY").toReal() - top;
    QVERIFY2(moved > (from.y() - to.y()) * 0.6, qPrintable(QString::number(moved)));
    // A plain drag pans; only Ctrl+drag takes the file out.
    QVERIFY(!prop("draggingOut").toBool());
    // A release after a drag is not a tap: still zoomed, nothing toggled.
    QVERIFY(prop("viewScale").toReal() > 0);

    // A two-finger touchpad scroll moves along it too, rather than zooming.
    // QTest's timestamps make that drag a fast flick, so this goes back up.
    const qreal scale = prop("effectiveScale").toReal();
    const qreal before = still->property("contentY").toReal();
    QVERIFY(before > 300);
    const QPointF middle(m_window->width() / 2.0, m_window->height() / 2.0);
    for (const Qt::ScrollPhase phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollEnd}) {
      QWheelEvent swipe(middle, m_window->mapToGlobal(middle.toPoint()), QPoint(0, 120),
                        QPoint(0, 120), Qt::NoButton, Qt::NoModifier, phase, false);
      QCoreApplication::sendEvent(m_window, &swipe);
    }
    QTRY_VERIFY(still->property("contentY").toReal() < before - 100);
    QCOMPARE(prop("effectiveScale").toReal(), scale);
  }

  void videoPosterUsesLogicalDimensions() {
    open({media(QStringLiteral("clip.mp4"))});
    QQuickItem* poster = item(QStringLiteral("viewerVideoPoster"));
    QVERIFY(poster);
    QVERIFY(!poster->property("source").toUrl().isEmpty());
    QCOMPARE(poster->property("sourceSize").toSize(),
             QSize(qRound(poster->width()), qRound(poster->height())));
  }

  // , and . pause and step one frame, and the paused picture follows.
  void commaAndPeriodStepOneFrame() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_VERIFY(player->duration() > 0);
    QVideoSink* sink = player->videoSink();
    QVERIFY(sink);
    // The fixture is 30 frames a second. The frame on show holds the position.
    const auto showing = [sink, player] {
      const QVideoFrame frame = sink->videoFrame();
      const qint64 at = player->position() * 1000;
      return frame.startTime() <= at && frame.endTime() > at;
    };

    QTest::keyClick(m_window, Qt::Key_Period);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    player->setPosition(1000);
    QTRY_VERIFY(qAbs(sink->videoFrame().endTime() / 1000 - 1000) <= 34);
    QTest::keyClick(m_window, Qt::Key_Period);
    QTRY_COMPARE(player->position(), 1017);
    QTRY_VERIFY(showing());
    QCOMPARE(sink->videoFrame().startTime(), 1000000);
    QTest::keyClick(m_window, Qt::Key_Comma);
    QTRY_COMPARE(player->position(), 983);
    QTRY_VERIFY(showing());
    QCOMPARE(sink->videoFrame().startTime() / 1000, 966);
    QTest::keyClick(m_window, Qt::Key_Comma);
    QTRY_COMPARE(player->position(), 950);
    QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
    // Never before the start.
    player->setPosition(10);
    QTest::keyClick(m_window, Qt::Key_Comma);
    QTRY_COMPARE(player->position(), 17);
  }

  void videoPlaysWithPlayerKeysAndOneThinBar() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_VERIFY(player->duration() > 0);
    QQuickItem* transport = item(QStringLiteral("viewerTransport"));
    QVERIFY(transport->isVisible());
    QVERIFY(transport->height() <= 48);

    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    // Paused, the controls stay up even with the pointer at rest.
    m_window->setProperty("chromeTimeout", 100);
    QTest::qWait(300);
    QVERIFY(prop("chromeShown").toBool());

    player->setPosition(0);
    QTRY_VERIFY(player->position() < 500);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_VERIFY(player->position() >= 3000);
    QTest::keyClick(m_window, Qt::Key_J);
    QTRY_VERIFY(player->position() < 500);

    const qreal volume = m_settings->videoVolume();
    QTest::keyClick(m_window, Qt::Key_Down);
    QCOMPARE(m_settings->videoVolume(), qMax(0.0, volume - 0.05));
    QTest::keyClick(m_window, Qt::Key_M);
    QVERIFY(m_settings->videoMuted());
    QTest::keyClick(m_window, Qt::Key_M);
    QVERIFY(!m_settings->videoMuted());

    QTest::keyClick(m_window, Qt::Key_BracketRight);
    QCOMPARE(player->playbackRate(), 1.25);
    QTest::keyClick(m_window, Qt::Key_Backspace);
    QCOMPARE(player->playbackRate(), 1.0);

    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    // Moving on to a picture stops the sound.
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QVERIFY(!m_session->isVideo());
    QTRY_VERIFY(player->playbackState() != QMediaPlayer::PlayingState);
    QVERIFY(!item(QStringLiteral("viewerTransport"))->isVisible());
  }

  void videoLoopRestartsButSlideshowStillAdvances_data() {
    QTest::addColumn<bool>("loop");
    QTest::addColumn<bool>("slideshow");
    QTest::newRow("replay-button") << false << false;
    QTest::newRow("loop") << true << false;
    QTest::newRow("slideshow-with-loop") << true << true;
  }

  void videoLoopRestartsButSlideshowStillAdvances() {
    QFETCH(bool, loop);
    QFETCH(bool, slideshow);
    const bool videos = m_settings->slideshowVideos();
    const auto restore = qScopeGuard([this, videos] { m_settings->setSlideshowVideos(videos); });
    m_settings->setSlideshowVideos(true);
    const QString path = media(QStringLiteral("clip.mp4"));
    const QString next = media(QStringLiteral("Shot 1.jpg"));
    open({path, next});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    QVERIFY(!prop("loopVideo").toBool());
    if (loop) QTest::keyClick(m_window, Qt::Key_L, Qt::ShiftModifier);
    QCOMPARE(prop("loopVideo").toBool(), loop);
    if (slideshow) QTest::keyClick(m_window, Qt::Key_F5);
    QSignalSpy statuses(player, &QMediaPlayer::mediaStatusChanged);
    player->setPosition(player->duration() - 250);
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(statuses.cbegin(), statuses.cend(), [](const auto& args) {
      return args.first().toInt() == QMediaPlayer::EndOfMedia;
    }), 5000);
    if (slideshow) {
      QTRY_COMPARE(m_session->path(), next);
      QVERIFY(prop("slideshowRunning").toBool());
    } else {
      QCOMPARE(m_session->path(), path);
      QTRY_VERIFY(player->position() < 2000);
      QTRY_COMPARE(player->playbackState(), loop ? QMediaPlayer::PlayingState
                                               : QMediaPlayer::PausedState);
    }
    m_window->close();
    QVERIFY(!prop("loopVideo").toBool());
  }

  void videoLoopMenuTogglesWithoutSeeking() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    player->pause();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    player->setPosition(1000);
    QTRY_COMPARE(player->position(), 1000);
    QTest::keyClick(m_window, Qt::Key_L, Qt::ShiftModifier);
    QVERIFY(prop("loopVideo").toBool());
    QCOMPARE(player->position(), 1000);
    QTest::keyClick(m_window, Qt::Key_Menu);
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("video-loop")), "click"));
    QVERIFY(!prop("loopVideo").toBool());
  }

  void playbackSpeedPreferenceIsOptionalAndSharedByControls() {
    QVERIFY(!m_settings->rememberPlaybackSpeed());
    const QString video = media(QStringLiteral("clip.mp4"));
    const QString picture = media(QStringLiteral("Shot 1.jpg"));
    open({video, picture});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    QTest::keyClick(m_window, Qt::Key_BracketRight);
    QCOMPARE(player->playbackRate(), 1.25);
    QCOMPARE(m_settings->videoPlaybackRate(), 1.0);
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTest::keyClick(m_window, Qt::Key_PageUp);
    QTRY_COMPARE(player->playbackRate(), 1.0);

    m_settings->setRememberPlaybackSpeed(true);
    QTest::keyClick(m_window, Qt::Key_BracketRight);
    QCOMPARE(m_settings->videoPlaybackRate(), 1.25);
    m_window->setProperty("chromePinned", true);
    const auto unpin = qScopeGuard([this] { m_window->setProperty("chromePinned", false); });
    auto* speed = item(QStringLiteral("viewerSpeedButton"));
    QTRY_VERIFY(speed->isVisible());
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                     speed->mapToScene(QPointF(speed->width() / 2, speed->height() / 2)).toPoint());
    QTRY_COMPARE(player->playbackRate(), 1.5);
    QCOMPARE(m_settings->videoPlaybackRate(), 1.5);
    {
      AppSettings reloaded;
      QVERIFY(reloaded.rememberPlaybackSpeed());
      QCOMPARE(reloaded.videoPlaybackRate(), 1.5);
    }
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTest::keyClick(m_window, Qt::Key_PageUp);
    QTRY_COMPARE(player->playbackRate(), 1.5);
    QTest::keyClick(m_window, Qt::Key_Backspace);
    QCOMPARE(player->playbackRate(), 1.0);
    QCOMPARE(m_settings->videoPlaybackRate(), 1.0);
    QTest::keyClick(m_window, Qt::Key_BracketRight);
    m_window->close();
    open({video});
    QTRY_COMPARE(player->playbackRate(), 1.25);
    m_settings->setRememberPlaybackSpeed(false);
    m_window->close();
    open({video});
    QTRY_COMPARE(player->playbackRate(), 1.0);
    QCOMPARE(m_settings->videoPlaybackRate(), 1.25);
  }

  void playbackSpeedPreferenceIsBounded() {
    m_settings->setVideoPlaybackRate(20);
    QCOMPARE(m_settings->videoPlaybackRate(), 4.0);
    m_settings->setVideoPlaybackRate(-1);
    QCOMPARE(m_settings->videoPlaybackRate(), 0.25);
    m_settings->setVideoPlaybackRate(std::numeric_limits<qreal>::quiet_NaN());
    QCOMPARE(m_settings->videoPlaybackRate(), 0.25);
    AppSettings reloaded;
    QCOMPARE(reloaded.videoPlaybackRate(), 0.25);
  }

  void audioTrackAndSidecarTimingControlsFollowAvailability_data() {
    QTest::addColumn<QString>("palette");
    QTest::newRow("dark-narrow") << QStringLiteral("dark");
    QTest::newRow("light-narrow") << QStringLiteral("light");
  }

  void audioTrackAndSidecarTimingControlsFollowAvailability() {
    QFETCH(QString, palette);
    QTemporaryDir themeHome;
    QVERIFY(themeHome.isValid());
    QVERIFY(QDir().mkpath(themeHome.filePath(QStringLiteral("omarchy"))));
    const QString fixture = QFINDTESTDATA("fixtures/themes") + QLatin1Char('/') + palette;
    QVERIFY(QFile::link(fixture, themeHome.filePath(QStringLiteral("omarchy/current"))));
    OmarchyTheme theme(themeHome.path(), themeHome.path());
    m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    const auto restoreTheme = qScopeGuard([this] {
      m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), m_theme);
    });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("tracks.mkv"));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/viewer/captions.srt"),
                        dir.filePath(QStringLiteral("tracks.srt"))));
    const QString picture = media(QStringLiteral("Shot 1.jpg"));
    open({path, picture});
    m_window->resize(560, 420);
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_COMPARE(player->audioTracks().size(), 2);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);
    player->pause();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QTRY_VERIFY(menuIds(menu).contains(QStringLiteral("audio-track")));
    QVERIFY(!menuIds(menu).contains(QStringLiteral("subtitles-earlier")));
    const int audio = player->activeAudioTrack();
    QTest::keyClick(m_window, Qt::Key_A, Qt::ShiftModifier);
    QCOMPARE(player->activeAudioTrack(), (audio + 1) % 2);
    // Named by the track's language, read from its metadata.
    const QString said = prop("status").toString();
    QVERIFY2(said == QStringLiteral("Audio: Track %1: %2")
                         .arg(player->activeAudioTrack() + 1)
                         .arg(player->activeAudioTrack() == 0 ? QStringLiteral("English")
                                                              : QStringLiteral("French")),
             qPrintable(said));
    QTest::keyClick(m_window, Qt::Key_Menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("audio-track")), "click"));
    QCOMPARE(player->activeAudioTrack(), audio);

    QTest::keyClick(m_window, Qt::Key_X);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
    QVariant choices;
    QVERIFY(QMetaObject::invokeMethod(m_window, "subtitleChoices", Q_RETURN_ARG(QVariant, choices)));
    QCOMPARE(choices.toList().value(1).toMap().value(QStringLiteral("label")).toString(),
             QStringLiteral("English"));
    QTest::keyClick(m_window, Qt::Key_C); // An embedded track can be shifted too.
    QTRY_VERIFY(menuIds(menu).contains(QStringLiteral("subtitles-earlier")));
    QVERIFY(menuIds(menu).contains(QStringLiteral("subtitles-later")));
    QTest::keyClick(m_window, Qt::Key_C); // Sidecar.
    QTRY_VERIFY(!prop("externalSubtitle").toString().isEmpty());
    QTRY_VERIFY(menuIds(menu).contains(QStringLiteral("subtitles-earlier")));
    QTRY_VERIFY(menuIds(menu).contains(QStringLiteral("subtitles-later")));
    player->setPosition(6200);
    QTRY_COMPARE(player->position(), 6200);
    QTRY_COMPARE(prop("subtitleText").toString(), QStringLiteral("Second subtitle"));
    QTest::keyClick(m_window, Qt::Key_X);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 500);
    QTRY_COMPARE(prop("subtitleText").toString(), QStringLiteral("Omaroll subtitle test"));
    QTest::keyClick(m_window, Qt::Key_Menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    auto* content = menu->property("contentItem").value<QQuickItem*>();
    QVERIFY(content);
    QTRY_VERIFY(content->height() <= m_window->height());
    QTRY_VERIFY(content->mapToScene(QPointF()).y() >= 0);
    QTRY_VERIFY(content->mapToScene(QPointF(0, content->height())).y() <= m_window->height());
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QSignalSpy frames(m_window, &QQuickWindow::frameSwapped);
      m_window->requestUpdate();
      QTRY_VERIFY(!frames.isEmpty());
      QVERIFY(m_window->grabWindow().save(QCoreApplication::applicationDirPath()
          + QStringLiteral("/video-menu-%1-narrow.png").arg(palette)));
    }
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("subtitles-earlier")), "click"));
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_Z);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), -500);
    for (int i = 0; i < 25; ++i) QTest::keyClick(m_window, Qt::Key_Z);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), -10000);
    for (int i = 0; i < 45; ++i) QTest::keyClick(m_window, Qt::Key_X);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 10000);
    QTest::keyClick(m_window, Qt::Key_C); // Off: delay controls disappear.
    QTRY_VERIFY(!menuIds(menu).contains(QStringLiteral("subtitles-earlier")));
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
    QTRY_VERIFY(!menuIds(menu).contains(QStringLiteral("audio-track")));
    QTRY_VERIFY(!menuIds(menu).contains(QStringLiteral("video-loop")));
    open({media(QStringLiteral("clip.mp4"))});
    QTRY_COMPARE(player->audioTracks().size(), 0);
    QTRY_VERIFY(!menuIds(menu).contains(QStringLiteral("audio-track")));
  }

  void explicitSeeksClearSavedResumeState() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    player->pause();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);

    const auto markResume = [this, player] {
      player->setPosition(1000);
      m_window->setProperty("resumePosition", 1000);
      m_window->setProperty("resumeAvailable", true);
      m_window->setProperty("resumePending", true);
    };

    QQuickItem* scrub = item(QStringLiteral("viewerScrub"));
    QVERIFY(scrub->width() > 0);
    markResume();
    QVERIFY(QMetaObject::invokeMethod(scrub, "seekTo",
                                      Q_ARG(QVariant, scrub->width() * 0.75)));
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    QTRY_VERIFY(player->position() > player->duration() / 2);

    markResume();
    QTest::keyClick(m_window, Qt::Key_Home);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    QTRY_COMPARE(player->position(), 0);

    markResume();
    QTest::keyClick(m_window, Qt::Key_End);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    QTRY_VERIFY(player->position() > player->duration() * 0.9);

    // The arrow keys still seek by their difference from the current position.
    markResume();
    QTest::keyClick(m_window, Qt::Key_Right);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    QTRY_VERIFY(player->position() > 4000);
  }

  void narrowTransportKeepsSeekingWithCaptionsAndChangedSpeed_data() {
    QTest::addColumn<QString>("palette");
    QTest::addColumn<int>("width");
    QTest::newRow("dark-320") << QStringLiteral("dark") << 320;
    QTest::newRow("dark-400") << QStringLiteral("dark") << 400;
    QTest::newRow("light-320") << QStringLiteral("light") << 320;
    QTest::newRow("light-400") << QStringLiteral("light") << 400;
  }

  void narrowTransportKeepsSeekingWithCaptionsAndChangedSpeed() {
    QFETCH(QString, palette);
    QFETCH(int, width);
    QTemporaryDir themeHome;
    QVERIFY(themeHome.isValid());
    QVERIFY(QDir().mkpath(themeHome.filePath(QStringLiteral("omarchy"))));
    const QString fixture = QFINDTESTDATA(qPrintable(QStringLiteral("fixtures/themes/%1").arg(palette)));
    QVERIFY(QFile::link(fixture, themeHome.filePath(QStringLiteral("omarchy/current"))));
    OmarchyTheme theme(themeHome.path(), themeHome.path());
    m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), &theme);
    const auto restoreTheme = qScopeGuard([this] {
      m_engine->rootContext()->setContextProperty(QStringLiteral("Theme"), m_theme);
    });
    ViewerWindows viewers(*m_engine);
    viewers.setPlacementQuery([](QObject*, HyprlandPlacement::Reply reply) { reply({}); });
    QQuickWindow* window = viewers.open({media(QStringLiteral("clip.mp4"))});
    QVERIFY(window);
    const auto control = [window](const QString& name) {
      return find(window->contentItem(), [&name](QQuickItem* candidate) {
        return candidate->objectName() == name;
      });
    };
    QQuickItem* transport = control(QStringLiteral("viewerTransport"));
    QQuickItem* scrub = control(QStringLiteral("viewerScrub"));
    QVERIFY(transport && scrub);
    window->resize(width, 240);
    QTRY_COMPARE(transport->width(), width - 32);
    QQmlComponent component(m_engine);
    component.setData(R"(
      import QtQuick
      QtObject {
        property real duration: 7200000
        property real position: 3660000
        property real playbackRate: 1.5
        property int playbackState: 2
      }
    )", QUrl());
    std::unique_ptr<QObject> player(component.create());
    QVERIFY2(player, qPrintable(component.errorString()));
    QVERIFY(QQmlProperty::write(transport, QStringLiteral("player"), QVariant::fromValue(player.get())));
    QVERIFY(QQmlProperty::write(transport, QStringLiteral("hasCaptions"), true));
    QVERIFY(QQmlProperty::write(transport, QStringLiteral("captionsOn"), true));
    window->setProperty("chromePinned", true);
    for (const int pixels : {11, 14}) {
      for (const QString& clock : {QStringLiteral("1:01:00"), QStringLiteral("2:00:00")}) {
        QQuickItem* label = find(transport, [&clock](QQuickItem* candidate) {
          return candidate->property("text").toString() == clock;
        });
        QVERIFY(label);
        QVERIFY(QQmlProperty::write(label, QStringLiteral("font.pixelSize"), pixels));
      }
      for (const double rate : {1.0, 1.5}) {
        player->setProperty("playbackRate", rate);
        QTRY_VERIFY(scrub->width() >= 64);
        QTRY_VERIFY(control(QStringLiteral("viewerCaptionsButton"))->isVisible());
        if (rate != 1.0) {
          QTRY_VERIFY(control(QStringLiteral("viewerSpeedButton"))->isVisible());
        }
        QTRY_VERIFY(scrub->mapToScene(QPointF()).x() >= 0);
        QTRY_VERIFY(scrub->mapToScene(QPointF(scrub->width(), 0)).x() <= width);
        if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
          QSignalSpy frames(window, &QQuickWindow::frameSwapped);
          window->requestUpdate();
          QTRY_VERIFY(frames.size() >= 2);
          QTRY_VERIFY(!window->grabWindow().isNull());
          QVERIFY(window->grabWindow().save(QCoreApplication::applicationDirPath()
              + QStringLiteral("/narrow-transport-%1-%2-%3-%4.png")
                    .arg(palette).arg(width).arg(rate).arg(pixels)));
        }
      }
    }
    window->close();
  }

  // Hovering the seek bar shows the storyboard frame nearest the pointer,
  // above the time, once the video's keyframes have been decoded.
  void seekBarHoverShowsAStoryboardFrameAboveTheTime() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
      QSKIP("ffmpeg is not installed");
    }
    const QString path = m_scratch.filePath(QStringLiteral("storyboard.mkv"));
    QVERIFY(QFile::exists(path) || !makeStoryboardVideo(path).isEmpty());
    open({path});
    m_window->resize(1200, 800);
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->duration() > 0);
    auto* storyboard = m_window->findChild<Storyboard*>(QStringLiteral("viewerStoryboard"));
    QVERIFY(storyboard);
    QCOMPARE(storyboard->source(), path);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard->complete(), 30000);
    QVERIFY(storyboard->frameCount() >= 2);
    player->pause();
    m_window->setProperty("chromePinned", true);

    QQuickItem* scrub = item(QStringLiteral("viewerScrub"));
    QQuickItem* preview = item(QStringLiteral("viewerSeekPreview"));
    QQuickItem* frame = find(preview, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("seekPreviewFrame");
    });
    QQuickItem* time = find(preview, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("seekPreviewTime");
    });
    QVERIFY(frame && time);
    const auto along = [scrub](qreal fraction) {
      return scrub->mapToScene(QPointF(scrub->width() * fraction, scrub->height() / 2)).toPoint();
    };
    // The frame is there as soon as the pointer is: no decoder, no wait.
    QList<qint64> latencies;
    QStringList sources;
    for (const qreal fraction : {0.05, 0.25, 0.5, 0.75, 0.95}) {
      QElapsedTimer hover;
      hover.start();
      QTest::mouseMove(m_window, along(fraction));
      QVERIFY(preview->isVisible());
      QVERIFY(frame->isVisible());
      QCOMPARE(frame->property("status").toInt(), 1); // Image.Ready
      latencies.append(hover.nsecsElapsed() / 1000);
      sources.append(frame->property("source").toString());
    }
    std::sort(latencies.begin(), latencies.end());
    qInfo("hover to preview frame: median %lld us, max %lld us", latencies.at(2), latencies.constLast());
    sources.removeDuplicates();
    QVERIFY(sources.size() >= 2);

    // Frames are keyframes. One from another second says its own time in
    // its corner; the label under it stays the time a click seeks to.
    QQuickItem* frameTime = find(preview, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("seekPreviewFrameTime");
    });
    QVERIFY(frameTime);
    bool sawElsewhere = false;
    for (const qreal fraction : {0.05, 0.25, 0.42, 0.5, 0.75}) {
      QTest::mouseMove(m_window, along(fraction));
      const qreal at = preview->property("frameTime").toReal();
      QVERIFY(at >= 0);
      const int seconds = qRound(at / 1000);
      const QString own = QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
      const bool elsewhere = own != time->property("text").toString();
      QCOMPARE(frameTime->isVisible(), elsewhere);
      QCOMPARE(preview->property("frameLabel").toString(), own);
      sawElsewhere = sawElsewhere || elsewhere;
    }
    QVERIFY(sawElsewhere);

    QTest::mouseMove(m_window, along(0.5));
    QCOMPARE(time->property("text").toString(), QStringLiteral("0:06"));
    // The frame sits above the time, and the whole preview above the bar,
    // in the theme's rounded box.
    const QRectF frameBounds = frame->mapRectToScene(frame->boundingRect());
    const QRectF timeBounds = time->mapRectToScene(time->boundingRect());
    const QRectF previewBounds = preview->mapRectToScene(preview->boundingRect());
    QVERIFY(frameBounds.bottom() <= timeBounds.top());
    QVERIFY(previewBounds.contains(frameBounds) && previewBounds.contains(timeBounds));
    QVERIFY(previewBounds.bottom() < scrub->mapToScene(QPointF()).y());
    QCOMPARE(qRound(frameBounds.width()), 192);
    QCOMPARE(qRound(frameBounds.height()), 108);
    QCOMPARE(preview->property("radius").toReal(),
             m_theme->cornerRadius() > 0 ? qMin(qreal(m_theme->cornerRadius()), 8.0) : 3.0);
    // Rendered: the test pattern's colours, not an empty box.
    QSignalSpy frames(m_window, &QQuickWindow::frameSwapped);
    m_window->requestUpdate();
    QTRY_VERIFY(frames.size() >= 1);
    const QImage shot = m_window->grabWindow();
    QVERIFY(!shot.isNull());
    const qreal scale = shot.devicePixelRatio();
    const QRect pixels = QRectF(frameBounds.topLeft() * scale, frameBounds.size() * scale)
                             .toAlignedRect().adjusted(2, 2, -2, -2).intersected(shot.rect());
    QSet<QRgb> colours;
    for (int y = pixels.top(); y <= pixels.bottom(); y += 6) {
      for (int x = pixels.left(); x <= pixels.right(); x += 6) {
        colours.insert(shot.pixel(x, y) & 0xf0f0f0);
      }
    }
    QVERIFY2(colours.size() >= 6, qPrintable(QString::number(colours.size())));

    // A drag keeps the preview with the pointer and holds the extraction.
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, along(0.3));
    QVERIFY(storyboard->paused());
    QTest::mouseMove(m_window, along(0.8));
    QVERIFY(preview->isVisible());
    QTRY_COMPARE(time->property("text").toString(), QStringLiteral("0:10"));
    QVERIFY(frame->isVisible());
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, along(0.8));
    QTRY_VERIFY(player->position() > 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!storyboard->paused(), 3000);
    // A keyboard seek holds it too, for a moment.
    QTest::keyClick(m_window, Qt::Key_Left);
    QVERIFY(storyboard->paused());
    QTRY_VERIFY_WITH_TIMEOUT(!storyboard->paused(), 3000);

    // A narrow viewer keeps the preview inside the window.
    m_window->resize(320, 240);
    QTRY_VERIFY(scrub->width() > 0 && scrub->mapToScene(QPointF(scrub->width(), 0)).x() <= 320);
    QTest::mouseMove(m_window, along(0.98));
    QTRY_VERIFY(preview->isVisible());
    const QRectF narrow = preview->mapRectToScene(preview->boundingRect());
    QVERIFY2(narrow.left() >= 0 && narrow.right() <= m_window->width(),
             qPrintable(QStringLiteral("%1..%2").arg(narrow.left()).arg(narrow.right())));
    QVERIFY2(narrow.top() >= 0, qPrintable(QString::number(narrow.top())));
    QTest::mouseMove(m_window, along(0.02));
    QTRY_VERIFY(preview->mapRectToScene(preview->boundingRect()).left() >= 0);
    m_window->resize(1200, 800);
    m_window->setProperty("chromePinned", false);
    QTest::mouseMove(m_window, QPoint(600, 200));
    QTRY_VERIFY(!preview->isVisible());

    // Moving on to a picture clears the storyboard.
    m_session->open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_COMPARE(storyboard->frameCount(), 0);
    QVERIFY(!storyboard->running());
  }

  // Only the focused viewer decodes previews: one left behind another window
  // stops its decoders, as does closing it.
  void previewDecodersFollowFocusAndStopOnClose() {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
      QSKIP("ffmpeg is not installed");
    }
    const QString path = m_scratch.filePath(QStringLiteral("storyboard-focus.mkv"));
    QVERIFY(QFile::exists(path) || !makeStoryboardVideo(path).isEmpty());
    // Storyboard decodes hang and log their pid; anything else is ffmpeg.
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const QString log = bin.filePath(QStringLiteral("starts"));
    const QString helper = bin.filePath(QStringLiteral("ffmpeg"));
    {
      QFile script(helper);
      QVERIFY(script.open(QIODevice::WriteOnly));
      script.write(QStringLiteral("#!/bin/sh\ncase \" $* \" in *\" -skip_frame \"*) echo $$ >> '%1'; exec sleep 30;; esac\n"
                                  "exec '%2' \"$@\"\n").arg(log, ffmpeg).toUtf8());
    }
    QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(bin.path()) + ':' + oldPath);
    const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
    const auto pids = [&log] {
      QList<qint64> found;
      QFile file(log);
      if (file.open(QIODevice::ReadOnly)) {
        for (const QByteArray& line : file.readAll().split('\n')) {
          if (!line.isEmpty()) found.append(line.toLongLong());
        }
      }
      return found;
    };
    const auto alive = [](qint64 pid) { return ::kill(pid_t(pid), 0) == 0; };

    open({path});
    QPointer<Storyboard> storyboard = m_window->findChild<Storyboard*>(QStringLiteral("viewerStoryboard"));
    QVERIFY(storyboard);
    QTRY_VERIFY_WITH_TIMEOUT(pids().size() == Storyboard::kParallelProcesses, 10000);
    QVERIFY(storyboard->active());
    const QList<qint64> first = pids();

    // The controls fading with the pointer at rest stop nothing: the
    // decoders carry on, and none start over.
    m_window->setProperty("chromeTimeout", 50);
    QTest::mouseMove(m_window, QPoint(m_window->width() / 2, m_window->height() / 3));
    QTRY_VERIFY(!prop("chromeShown").toBool());
    QTRY_VERIFY(!item(QStringLiteral("viewerTransport"))->isVisible());
    QTest::qWait(Storyboard::kStartDelayMs + 200);
    QVERIFY(storyboard->active());
    QVERIFY(storyboard->running());
    for (const qint64 pid : first) QVERIFY(alive(pid));
    QCOMPARE(pids().size(), first.size());
    m_window->setProperty("chromeTimeout", 2200);

    // Minimized, it stops; restored, it carries on.
    m_window->showMinimized();
    QTRY_VERIFY(!storyboard->active());
    for (const qint64 pid : first) QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 3000);
    m_window->showNormal();
    m_window->requestActivate();
    QTRY_VERIFY(m_window->isActive());
    QTRY_VERIFY(storyboard->active());
    QTRY_VERIFY_WITH_TIMEOUT(pids().size() == 2 * Storyboard::kParallelProcesses, 10000);
    const QList<qint64> restored = pids().mid(first.size());

    // Another window takes focus: this one's decoders stop.
    QQuickWindow other;
    other.resize(200, 200);
    other.show();
    other.requestActivate();
    QTRY_VERIFY(other.isActive());
    QTRY_VERIFY(!storyboard->active());
    for (const qint64 pid : restored) QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 3000);
    QTest::qWait(Storyboard::kStartDelayMs + 200);
    QCOMPARE(pids().size(), first.size() + restored.size());

    // Back in front, it carries on.
    other.close();
    m_window->requestActivate();
    QTRY_VERIFY(m_window->isActive());
    QTRY_VERIFY(storyboard->active());
    QTRY_VERIFY_WITH_TIMEOUT(pids().size() == 3 * Storyboard::kParallelProcesses, 10000);
    const QList<qint64> second = pids().mid(first.size() + restored.size());

    // Closing the viewer window stops them too.
    m_window->close();
    QTRY_VERIFY(!m_window->isVisible());
    for (const qint64 pid : second) QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 3000);
    QVERIFY(!storyboard || !storyboard->running());
  }

  void explicitStartOverridesSavedResume() {
    const QString path = m_scratch.filePath("explicit-start.mkv");
    QVERIFY(QFile::exists(path) || QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
    m_settings->setVideoPosition(path, 7000);
    m_session->setStartPosition(path, 1000);
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_COMPARE(m_session->pendingStartPosition(path), -1);
    QTRY_VERIFY(player->position() >= 900 && player->position() < 5000);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    player->pause();
    // A forwarded request for the already-open source seeks again, including zero,
    // and holds seek-preview decodes like any other seek.
    auto* storyboard = m_window->findChild<Storyboard*>(QStringLiteral("viewerStoryboard"));
    QVERIFY(storyboard);
    QTRY_VERIFY_WITH_TIMEOUT(!storyboard->paused(), 3000);
    m_session->setStartPosition(path, 0);
    QTRY_VERIFY(storyboard->paused());
    QTRY_VERIFY(player->position() < 1500);
    QCOMPARE(m_session->pendingStartPosition(path), -1);
    QVERIFY(!prop("resumeAvailable").toBool());
  }

  // A launch with --start, as main.cpp makes it: a viewer of its own with a
  // player created on demand, the start set after the window opens, and the
  // GUI thread slow. The start must survive, and a pause straight after
  // launch must show the frame at the position, not the poster.
  void launchStartSurvivesSlowStartup_data() {
    QTest::addColumn<QString>("load");
    QTest::addColumn<bool>("pauseEarly");
    QTest::newRow("idle") << QStringLiteral("idle") << false;
    QTest::newRow("busy-gui") << QStringLiteral("busy") << false;
    QTest::newRow("slow-status") << QStringLiteral("status") << false;
    QTest::newRow("idle-pause") << QStringLiteral("idle") << true;
    QTest::newRow("busy-gui-pause") << QStringLiteral("busy") << true;
    QTest::newRow("slow-status-pause") << QStringLiteral("status") << true;
    QTest::newRow("lost-seek") << QStringLiteral("lost") << false;
  }

  void launchStartSurvivesSlowStartup() {
    QFETCH(QString, load);
    QFETCH(bool, pauseEarly);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A long GOP: the start lands 5.5 s past the last keyframe, so the exact
    // seek decodes for a while before its frame exists.
    const QString clip = dir.filePath(QStringLiteral("gop.mkv"));
    QVERIFY(encodeClip(clip, 40, 300));
    const int loops = qEnvironmentVariableIntValue("OMAROLL_RACE_LOOPS") > 0
                          ? qEnvironmentVariableIntValue("OMAROLL_RACE_LOOPS") : 2;
    const qint64 start = 25500;
    int failures = 0;
    for (int attempt = 0; attempt < loops; ++attempt) {
      const QString path = dir.filePath(QStringLiteral("launch-%1.mkv").arg(attempt));
      QVERIFY(QFile::copy(clip, path));
      FreshViewer viewer = freshViewer();
      QVERIFY(viewer.window);
      QTimer busy;
      if (load == u"busy") {
        // A loaded machine: every turn of the event loop runs late.
        busy.setInterval(0);
        connect(&busy, &QTimer::timeout, this, [] { QThread::msleep(15); });
        busy.start();
      }
      std::vector<QMetaObject::Connection> stalls;
      const auto unstall = qScopeGuard([&] {
        for (const auto& connection : stalls) disconnect(connection);
      });
      viewer.session->open({path});
      viewer.session->setStartPosition(path, start);
      viewer.window->show();
      QMediaPlayer* player = nullptr;
      QTRY_VERIFY((player = viewer.window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
      if (load == u"status") {
        // Status, duration and seekability reach QML late and out of step.
        stalls.push_back(connect(player, &QMediaPlayer::mediaStatusChanged, this,
                                 [] { QThread::msleep(120); }));
        stalls.push_back(connect(player, &QMediaPlayer::seekableChanged, this,
                                 [] { QThread::msleep(80); }));
        stalls.push_back(connect(player, &QMediaPlayer::durationChanged, this,
                                 [] { QThread::msleep(80); }));
      }
      if (load == u"lost") {
        // The backend loses the launch seek: it lands, then the player is
        // back at the start before playback begins.
        auto lost = std::make_shared<bool>(false);
        stalls.push_back(connect(player, &QMediaPlayer::positionChanged, this,
                                 [player, lost, start](qint64 position) {
                                   if (*lost || position < start - 100) return;
                                   *lost = true;
                                   player->setPosition(0);
                                 }));
      }
      if (pauseEarly) {
        // Space as soon as playback starts, while the exact seek still decodes.
        QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 10000);
        QVERIFY(QMetaObject::invokeMethod(viewer.window, "togglePlayback"));
        QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
      }
      const auto settled = [&] {
        if (pauseEarly) {
          const QVideoFrame frame = player->videoSink()->videoFrame();
          const qint64 at = player->position() * 1000;
          QObject* presentation = viewer.window->property("presentation").value<QObject*>();
          return player->playbackState() == QMediaPlayer::PausedState && frame.isValid()
                 && presentation && presentation->property("ready").toBool()
                 && frame.startTime() <= at + 100000 && frame.endTime() >= at - 100000
                 && player->position() >= start - 200;
        }
        return player->playbackState() == QMediaPlayer::PlayingState
               && player->position() >= start;
      };
      const bool ok = QTest::qWaitFor(settled, 12000);
      busy.stop();
      if (!ok) {
        ++failures;
        const QVideoFrame frame = player->videoSink()->videoFrame();
        qWarning().noquote() << "launch" << attempt << load << "state" << player->playbackState()
                             << "status" << player->mediaStatus() << "position" << player->position()
                             << "frame" << (frame.isValid() ? frame.startTime() / 1000 : -1)
                             << "poster" << viewer.item(QStringLiteral("viewerVideoPoster"))->isVisible();
      }
      viewer.window->close();
    }
    QCOMPARE(failures, 0);
  }

  // Changing the subtitle or audio track early, while the backend is still
  // starting its streams, must not freeze the picture under a running clock.
  // The first attempt switches as soon as playback starts, as a key pressed
  // right after opening does; there the stall can follow one new frame.
  void earlyTrackChangeKeepsFramesMoving_data() {
    QTest::addColumn<QString>("track");
    QTest::newRow("subtitle") << QStringLiteral("subtitle");
    QTest::newRow("audio") << QStringLiteral("audio");
  }

  void earlyTrackChangeKeepsFramesMoving() {
    QFETCH(QString, track);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int loops = qEnvironmentVariableIntValue("OMAROLL_RACE_LOOPS") > 0
                          ? qEnvironmentVariableIntValue("OMAROLL_RACE_LOOPS") : 4;
    int failures = 0;
    for (int attempt = 0; attempt < loops; ++attempt) {
      const QString path = dir.filePath(QStringLiteral("tracks-%1.mkv").arg(attempt));
      QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
      FreshViewer viewer = freshViewer();
      QVERIFY(viewer.window);
      viewer.session->open({path});
      viewer.window->show();
      QVERIFY(QTest::qWaitForWindowExposed(viewer.window));
      QMediaPlayer* player = nullptr;
      QTRY_VERIFY((player = viewer.window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
      const char* cycle = track == u"subtitle" ? "cycleSubtitles" : "cycleAudioTrack";
      bool switched = false;
      QMetaObject::Connection startWatch;
      if (attempt == 0) {
        startWatch = QObject::connect(
            player, &QMediaPlayer::playbackStateChanged, viewer.window,
            [&](QMediaPlayer::PlaybackState state) {
              if (switched || state != QMediaPlayer::PlayingState) return;
              switched = QMetaObject::invokeMethod(viewer.window, cycle);
            },
            Qt::QueuedConnection);
        // The waits above may already have let it autoplay; then no new
        // PlayingState arrives, so switch now, still right at the start.
        if (player->playbackState() == QMediaPlayer::PlayingState) {
          switched = QMetaObject::invokeMethod(viewer.window, cycle);
        }
      }
      QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus() != QMediaPlayer::LoadingMedia
                                   && player->mediaStatus() != QMediaPlayer::NoMedia, 10000);
      if (attempt == 0) {
        QTRY_VERIFY(switched);
        QObject::disconnect(startWatch);
      } else {
        // Spread the switch over the first 300 ms, as a quick key press does.
        QTest::qWait((attempt * 37) % 300);
        QVERIFY(QMetaObject::invokeMethod(viewer.window, cycle));
      }
      qint64 lastFrame = -1;
      int frozen = 0;
      int changes = 0;
      QElapsedTimer clock;
      clock.start();
      while (clock.elapsed() < 2500) {
        QTest::qWait(100);
        if (player->playbackState() != QMediaPlayer::PlayingState) break;
        const qint64 frame = player->videoSink()->videoFrame().startTime();
        if (frame != lastFrame) {
          ++changes;
          frozen = 0;
        } else {
          ++frozen;
        }
        lastFrame = frame;
        if (frozen >= 8) break;
      }
      if (frozen >= 8 || changes < 10) {
        ++failures;
        // Still a failure; say whether the recovery seeks ran and whether the
        // picture came back later, which tells a slow recovery from none.
        QObject* presentation = viewer.window->property("presentation").value<QObject*>();
        const int resyncs = presentation ? presentation->property("stallResyncs").toInt() : -1;
        const bool watching = presentation && presentation->property("watchingStall").toBool();
        const qint64 stuck = player->videoSink()->videoFrame().startTime();
        QElapsedTimer after;
        after.start();
        while (after.elapsed() < 3000 && player->videoSink()->videoFrame().startTime() == stuck) QTest::qWait(50);
        const bool resumed = player->videoSink()->videoFrame().startTime() != stuck;
        qWarning().noquote() << "track change" << attempt << track << "frame" << lastFrame / 1000
                             << "position" << player->position() << "changes" << changes
                             << "frozen" << frozen << "state" << player->playbackState()
                             << "resyncs" << resyncs << "watching" << watching
                             << (resumed ? QStringLiteral("resumed after %1 ms more").arg(after.elapsed())
                                         : QStringLiteral("still frozen 3 s later"));
      }
      viewer.window->close();
    }
    QCOMPARE(failures, 0);
  }

  void remoteCommandsRespectResumeIntent() {
    if (!QDBusConnection(QStringLiteral("omaroll-test-client")).isConnected())
      QSKIP("dbus-daemon is not available");
    const QString path = m_scratch.filePath(QStringLiteral("mpris-resume.mkv"));
    QVERIFY(QFile::exists(path) || QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
    m_settings->setVideoPosition(path, 7000);
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(prop("resumeAvailable").toBool());
    QTRY_VERIFY(m_mpris->isPublished());
    const QString remote = QStringLiteral("org.mpris.MediaPlayer2.Player");
    QCOMPARE(mprisCall(remote, QStringLiteral("Play")).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    QTRY_VERIFY(player->position() >= 6500);
    QCOMPARE(mprisCall(remote, QStringLiteral("Pause")).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    m_window->setProperty("resumeAvailable", true);
    m_window->setProperty("resumePending", true);
    QCOMPARE(mprisCall(remote, QStringLiteral("Seek"), {qlonglong(-1000000)}).type(),
             QDBusMessage::ReplyMessage);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    const QVariantMap metadata = m_mpris->metadata();
    m_window->setProperty("resumeAvailable", true);
    QCOMPARE(mprisCall(remote, QStringLiteral("SetPosition"),
                       {metadata.value(QStringLiteral("mpris:trackid")), qlonglong(1000000)}).type(),
             QDBusMessage::ReplyMessage);
    QVERIFY(!prop("resumeAvailable").toBool());
    QTRY_COMPARE(player->position(), 1001); // Enter the frame at the exact boundary.
    QCOMPARE(mprisCall(remote, QStringLiteral("PlayPause")).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    m_window->setProperty("resumeAvailable", true);
    m_window->setProperty("resumePending", true);
    QCOMPARE(mprisCall(remote, QStringLiteral("Stop")).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!prop("resumeAvailable").toBool());
    QVERIFY(!prop("resumePending").toBool());
    m_settings->clearVideoPosition(path);
  }

  // The checked-in test pattern: saturated bars, two audio tracks and a
  // subtitle track. Under OpenGL the bars must reach the screen, not just a
  // decoded frame in memory.
  void videoFrameRendersAndSubtitlesCycle_data() {
    QTest::addColumn<bool>("extensionless");
    QTest::newRow("with-extension") << false;
    QTest::newRow("without-extension") << true;
  }

  void videoFrameRendersAndSubtitlesCycle() {
    QFETCH(bool, extensionless);
    const QString source = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(!source.isEmpty());
    const QString folder = m_scratch.filePath(QStringLiteral("tracks"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + (extensionless ? QStringLiteral("/tracks-without-extension")
                                                : QStringLiteral("/tracks.mkv"));
    QVERIFY(QFile::exists(path) || QFile::copy(source, path));
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);

    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QCOMPARE(QQuickWindow::graphicsApi(), QSGRendererInterface::OpenGL);
      QTRY_VERIFY_WITH_TIMEOUT(renderedVideoHasColor(), 5000);
    }

    m_window->setProperty("chromePinned", true);
    QQuickItem* captions = item(QStringLiteral("viewerCaptionsButton"));
    QTRY_VERIFY(captions->isVisible());
    QVERIFY(!captions->property("active").toBool());
    QTest::keyClick(m_window, Qt::Key_C);
    QTRY_VERIFY(captions->property("active").toBool());
    QCOMPARE(player->activeSubtitleTrack(), 0);
    player->setPosition(1001);
    QTRY_VERIFY(player->videoSink()->subtitleText().contains(QStringLiteral("Omaroll subtitle test")));
    player->pause();
    auto* overlay = item(QStringLiteral("viewerSubtitles"));
    QTRY_VERIFY(overlay->isVisible());
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      // With our overlay hidden, embedded text must change no video pixels.
      // This fails if Qt's VideoOutput also paints a second caption.
      const auto capture = [&] {
        auto* output = item(QStringLiteral("viewerVideoOutput"));
        const QImage image = m_window->grabWindow();
        const QRectF rect = output->mapRectToScene(output->boundingRect());
        return image.copy(QRectF(rect.topLeft() * image.devicePixelRatio(),
                                rect.size() * image.devicePixelRatio()).toAlignedRect());
      };
      // Caption controls also show a transient notice and a hover tooltip.
      // Clear those independently changing overlays before comparing pixels.
      QTest::mouseMove(m_window, QPoint(5, 5));
      QVERIFY(QMetaObject::invokeMethod(m_window, "say", Q_ARG(QVariant, QString())));
      // Pause acknowledgement can precede the last queued frame on slower
      // decoders. Detach delivery and re-submit one immutable frame, so the
      // only pixels allowed to change in this comparison are captions.
      auto* input = player->videoSink();
      auto* originalOutput = player->videoOutput();
      const QVideoFrame frozen = input->videoFrame();
      const QString cue = input->subtitleText();
      player->setVideoOutput(nullptr);
      const auto restoreOutput = qScopeGuard([&] { player->setVideoOutput(originalOutput); });
      input->setVideoFrame(frozen);
      input->setSubtitleText(cue);
      QTest::qWait(100);
      const QImage themed = capture();
      overlay->setVisible(false);
      QTest::qWait(100);
      const QImage embeddedOnly = capture();
      QVERIFY(themed != embeddedOnly); // The themed caption really drew pixels.
      input->setSubtitleText({});
      QTest::qWait(100);
      QCOMPARE(capture(), embeddedOnly);
      overlay->setVisible(true);
    }
    QTest::keyClick(m_window, Qt::Key_C);
    QTRY_VERIFY(!captions->property("active").toBool());
    QCOMPARE(player->activeSubtitleTrack(), -1);
    m_window->setProperty("chromePinned", false);
    player->pause();
  }

  void videoNavigationRetainsPictureWhileLoading() {
    QTemporaryDir dir;
    const QString first = dir.filePath(QStringLiteral("a.mkv"));
    const QString second = dir.filePath(QStringLiteral("b.mkv"));
    const QString fixture = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(QFile::copy(fixture, first));
    QVERIFY(QFile::copy(fixture, second));
    open({first, second});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->videoSink() && player->videoSink()->videoFrame().isValid());
    auto* output = item(QStringLiteral("viewerVideoOutput"));
    auto* sink = output->property("videoSink").value<QVideoSink*>();
    QVERIFY(sink);
    bool sawClear = false;
    bool retained = true;
    const auto connection = connect(player->videoSink(), &QVideoSink::videoFrameChanged,
        this, [&](const QVideoFrame& frame) {
          if (frame.isValid()) return;
          sawClear = true;
          retained = retained && sink->videoFrame().isValid();
        });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTRY_COMPARE(m_session->path(), second);
    QTRY_VERIFY(sawClear);
    QVERIFY(retained);
    QTRY_VERIFY(player->videoSink()->videoFrame().isValid());
  }

  void exactFrameBoundarySeek() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("boundary.mkv"));
    QProcess encoder;
    encoder.start(QStringLiteral("ffmpeg"), {"-v", "error", "-f", "lavfi", "-i",
                  "testsrc2=s=160x90:r=30:d=4", "-c:v", "ffv1", path});
    QVERIFY(encoder.waitForFinished(20000));
    QCOMPARE(encoder.exitCode(), 0);
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(player->hasVideo());
    player->pause();
    QVERIFY(QMetaObject::invokeMethod(m_window, "seekTo", Q_ARG(QVariant, 2000)));
    QTRY_COMPARE(player->position(), 2001); // Seek inside frame 60, never its preceding boundary.
    QTRY_VERIFY_WITH_TIMEOUT(player->videoSink()->videoFrame().startTime() >= 2000000, 5000);
    QVERIFY(player->videoSink()->videoFrame().startTime() < 2034000);
  }

  // Video codecs and containers encoded here so the check needs no downloads.
  void lessCommonContainersPlay_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QStringList>("codec");
    QTest::addColumn<QString>("pixelFormat");
    QTest::newRow("h264-mp4") << QStringLiteral("clip.mp4")
                              << QStringList{QStringLiteral("libx264")}
                              << QStringLiteral("yuv420p");
    QTest::newRow("hevc-mp4") << QStringLiteral("clip-hevc.mp4")
                              << QStringList{QStringLiteral("libx265"), QStringLiteral("-threads"),
                                             QStringLiteral("1"), QStringLiteral("-x265-params"),
                                             QStringLiteral("pools=1:frame-threads=1")}
                              << QStringLiteral("yuv420p");
    QTest::newRow("vp9-webm") << QStringLiteral("clip.webm")
                              << QStringList{QStringLiteral("libvpx-vp9")}
                              << QStringLiteral("yuv420p");
    QTest::newRow("mpeg4-avi") << QStringLiteral("clip.avi")
                               << QStringList{QStringLiteral("mpeg4")}
                               << QStringLiteral("yuv420p");
    QTest::newRow("mpeg2-mts") << QStringLiteral("clip.mts")
                              << QStringList{QStringLiteral("mpeg2video")}
                               << QStringLiteral("yuv420p");
    QTest::newRow("asf") << QStringLiteral("clip.asf") << QStringList{QStringLiteral("wmv2")}
                         << QStringLiteral("yuv420p");
    QTest::newRow("ogm") << QStringLiteral("clip.ogm")
                         << QStringList{QStringLiteral("libtheora"), QStringLiteral("-f"),
                                        QStringLiteral("ogg")}
                         << QStringLiteral("yuv420p");
    QTest::newRow("3g2") << QStringLiteral("clip.3g2") << QStringList{QStringLiteral("mpeg4")}
                         << QStringLiteral("yuv420p");
    // RGB with a padding byte, as FFV1 stores RGB: it must not draw blank.
    QTest::newRow("ffv1-bgr0") << QStringLiteral("clip-bgr0.mkv") << QStringList{QStringLiteral("ffv1")}
                               << QStringLiteral("bgr0");
  }

  void lessCommonContainersPlay() {
    QFETCH(QString, name);
    QFETCH(QStringList, codec);
    QFETCH(QString, pixelFormat);
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
      QSKIP("ffmpeg is not installed");
    }
    const QString folder = m_scratch.filePath(QStringLiteral("containers"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QLatin1Char('/') + name;
    QProcess encoder;
    encoder.start(ffmpeg, QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                                      QStringLiteral("error"), QStringLiteral("-y"),
                                      QStringLiteral("-f"), QStringLiteral("lavfi"),
                                      QStringLiteral("-i"),
                                      QStringLiteral("color=c=red:s=176x144:d=2:r=10"),
                                      QStringLiteral("-pix_fmt"), pixelFormat,
                                      QStringLiteral("-c:v")} +
                              codec + QStringList{path});
    QVERIFY(encoder.waitForFinished(20000));
    QVERIFY2(encoder.exitStatus() == QProcess::NormalExit && encoder.exitCode() == 0,
             encoder.readAllStandardError().constData());

    open({path});
    QVERIFY(m_session->isVideo());
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QCOMPARE(player->error(), QMediaPlayer::NoError);
    QVERIFY(player->hasVideo());
    QTRY_VERIFY_WITH_TIMEOUT(player->videoSink() && player->videoSink()->videoFrame().isValid(), 5000);
    if (pixelFormat == u"bgr0") {
      // What reaches the screen has its padding opaque.
      auto* shown = item(QStringLiteral("viewerVideoOutput"))->property("videoSink").value<QVideoSink*>();
      QVERIFY(shown);
      // The last file's frame is held until this one's first arrives.
      QTRY_COMPARE(shown->videoFrame().pixelFormat(), QVideoFrameFormat::Format_BGRX8888);
      QVideoFrame frame = shown->videoFrame();
      QVERIFY(frame.map(QVideoFrame::ReadOnly));
      QCOMPARE(int(frame.bits(0)[3]), 0xff);
      QVERIFY2(frame.bits(0)[2] > 200 && frame.bits(0)[0] < 60, "the padding is set, not the colour");
      frame.unmap();
    }
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QTRY_VERIFY_WITH_TIMEOUT(renderedVideoHasColor(), 5000);
      // The window is transparent, so a padding byte drawn as alpha leaves
      // the picture see-through: a compositor shows it washed out or white.
      QVERIFY(renderedVideoIsOpaque());
    }
    player->pause();
  }

  // PQ and HLG video is drawn without tone mapping. A notice with the
  // controls says so and hands it to mpv at the position, and the viewer
  // pauses so only one player is heard. SDR video gets no notice.
  void hdrVideoOffersMpvAndPausesOnHandoff_data() {
    QTest::addColumn<QString>("transfer");
    QTest::addColumn<QString>("shown");
    QTest::newRow("pq") << QStringLiteral("smpte2084") << QStringLiteral("PQ");
    QTest::newRow("hlg") << QStringLiteral("arib-std-b67") << QStringLiteral("HLG");
    QTest::newRow("sdr") << QStringLiteral("bt709") << QString();
  }

  void hdrVideoOffersMpvAndPausesOnHandoff() {
    QFETCH(QString, transfer);
    QFETCH(QString, shown);
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    const QString folder = m_scratch.filePath(QStringLiteral("hdr"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/clip-%1.mp4").arg(transfer);
    if (!QFile::exists(path)) {
      QProcess encoder;
      encoder.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                             QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                             QStringLiteral("testsrc2=s=320x180:r=10:d=6"), QStringLiteral("-vf"),
                             // FFmpeg 9 tags the stream from the frames, not -color_trc.
                             QStringLiteral("setparams=color_primaries=%1:color_trc=%2:colorspace=%3,format=yuv420p")
                                 .arg(transfer == u"bt709" ? QStringLiteral("bt709") : QStringLiteral("bt2020"), transfer,
                                      transfer == u"bt709" ? QStringLiteral("bt709") : QStringLiteral("bt2020nc")),
                             QStringLiteral("-c:v"), QStringLiteral("libx264"), path});
      QVERIFY(encoder.waitForFinished(30000));
      QVERIFY2(encoder.exitStatus() == QProcess::NormalExit && encoder.exitCode() == 0,
               encoder.readAllStandardError().constData());
    }
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const QString log = bin.filePath(QStringLiteral("mpv.log"));
    QFile mpv(bin.filePath(QStringLiteral("mpv")));
    QVERIFY(mpv.open(QIODevice::WriteOnly));
    mpv.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" + log.toUtf8() + "\"\n");
    mpv.close();
    QVERIFY(mpv.setPermissions(mpv.permissions() | QFileDevice::ExeOwner));
    const QByteArray systemPath = qgetenv("PATH");
    qputenv("PATH", bin.path().toUtf8() + ':' + systemPath);
    const auto restorePath = qScopeGuard([systemPath] { qputenv("PATH", systemPath); });

    // The notice tells a screen reader when it appears.
    static QStringList announced;
    announced.clear();
    const bool accessible = QAccessible::isActive();
    QAccessible::setActive(true);
    const auto previousHandler = QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
      if (event->type() == QAccessible::Announcement) {
        announced.append(static_cast<QAccessibleAnnouncementEvent*>(event)->message());
      }
    });
    const auto restoreAccessibility = qScopeGuard([previousHandler, accessible] {
      QAccessible::installUpdateHandler(previousHandler);
      QAccessible::setActive(accessible);
    });

    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QObject* presentation = m_window->property("presentation").value<QObject*>();
    QVERIFY(presentation);
    QTRY_VERIFY_WITH_TIMEOUT(presentation->property("ready").toBool(), 10000);
    QCOMPARE(presentation->property("hdr").toString(), shown);
    if (!shown.isEmpty()) {
      QTRY_COMPARE(announced.size(), 1);
      QVERIFY2(announced.first().startsWith(shown == u"PQ" ? QStringLiteral("HDR10 video") : QStringLiteral("HLG video"))
                   && announced.first().endsWith(QStringLiteral("P opens it in mpv")),
               qPrintable(announced.first()));
    }
    m_window->setProperty("chromePinned", true);
    const auto unpin = qScopeGuard([this] { m_window->setProperty("chromePinned", false); });
    QQuickItem* notice = item(QStringLiteral("viewerHdrNotice"));
    QTRY_COMPARE(notice->isVisible(), !shown.isEmpty());
    if (shown.isEmpty()) {
      player->pause();
      return;
    }
    QVERIFY(item(QStringLiteral("viewerHdrNoticeText"))->property("text").toString()
                .startsWith(shown == u"PQ" ? QStringLiteral("HDR10 video") : QStringLiteral("HLG video")));
    // Inside the window, clear of the header.
    const QRectF bounds = notice->mapRectToScene(notice->boundingRect());
    QVERIFY(bounds.left() >= 0 && bounds.right() <= m_window->width());
    QVERIFY(bounds.top() >= item(QStringLiteral("viewerHeader"))->height() - 4);
    QQuickItem* play = item(QStringLiteral("viewerHdrPlay"));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState && player->position() >= 500, 10000);
    // mpv is looked for again for each HDR file, so the button is there.
    QVERIFY(play->isVisible());
    click(play);
    const auto logged = [&log] {
      QFile args(log);
      return args.open(QIODevice::ReadOnly) ? args.readAll() : QByteArray();
    };
    QTRY_VERIFY_WITH_TIMEOUT(logged().contains(path.toUtf8()), 5000);
    QVERIFY2(logged().contains("--start="), logged().constData());
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    // The click stayed on the button: the stage didn't take it as a tap.
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
    QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
  }

  // Z and X shift an embedded text track too, through a copy of the track,
  // and the caption size setting scales the overlay.
  void embeddedSubtitlesTakeADelayAndASize() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) QSKIP("ffmpeg is not installed");
    const QString folder = m_scratch.filePath(QStringLiteral("embedded-delay"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/tracks.mkv");
    QVERIFY(QFile::exists(path) || QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);
    QTest::keyClick(m_window, Qt::Key_X);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0); // Nothing shown, nothing to shift.
    QCOMPARE(prop("status").toString(), QStringLiteral("Turn on subtitles first"));
    QTest::keyClick(m_window, Qt::Key_C);
    QTRY_COMPARE(player->activeSubtitleTrack(), 0);
    // Qt hands over embedded cues as it plays, so the unshifted one is
    // checked playing; the shifted ones come from the copy and hold paused.
    player->setPosition(6500);
    QTRY_COMPARE_WITH_TIMEOUT(prop("subtitleText").toString(), QStringLiteral("Second subtitle"), 5000);
    player->pause();
    player->setPosition(6500);

    QTest::keyClick(m_window, Qt::Key_X);
    QTest::keyClick(m_window, Qt::Key_X);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!prop("embeddedSubtitleCopy").toString().isEmpty(), 15000);
    // A second later: the first cue is still up at 6.5 s.
    QTRY_COMPARE(prop("subtitleText").toString(), QStringLiteral("Omaroll subtitle test"));
    for (int i = 0; i < 4; ++i) QTest::keyClick(m_window, Qt::Key_Z);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), -1000);
    player->setPosition(5500);
    QTRY_COMPARE(prop("subtitleText").toString(), QStringLiteral("Second subtitle"));

    auto* overlay = item(QStringLiteral("viewerSubtitles"));
    QTRY_VERIFY(overlay->isVisible());
    const int normal = overlay->property("font").value<QFont>().pixelSize();
    m_settings->setCaptionScale(200);
    const auto restoreScale = qScopeGuard([this] { m_settings->setCaptionScale(100); });
    QCOMPARE(overlay->property("font").value<QFont>().pixelSize(), normal * 2);
    m_settings->setCaptionScale(33); // Not one of the sizes.
    QCOMPARE(m_settings->captionScale(), 200);
  }

  // The copy behind an embedded delay: a text track comes out as SubRip, a
  // file without one fails rather than waiting forever.
  void embeddedSubtitleTracksAreCopiedOut() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) QSKIP("ffmpeg is not installed");
    SubtitleIndex index;
    const QString tracks = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(index.embedded(tracks, 0).isEmpty());
    QCOMPARE(index.embeddedState(tracks, 0), QStringLiteral("working"));
    QTRY_VERIFY_WITH_TIMEOUT(!index.embedded(tracks, 0).isEmpty(), 15000);
    QCOMPARE(index.embeddedState(tracks, 0), QStringLiteral("ready"));
    const QString copy = index.embedded(tracks, 0);
    QTRY_COMPARE(index.cueCount(copy), 2);
    QCOMPARE(index.textAt(copy, 7000), QStringLiteral("Second subtitle"));
    const QString clip = media(QStringLiteral("clip.mp4"));
    QVERIFY(index.embedded(clip, 0).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(index.embeddedState(clip, 0), QStringLiteral("failed"), 15000);
    QVERIFY(index.embedded(clip, 0).isEmpty());
  }

  // Only frames Qt draws as they are get the notice: it tone-maps PQ and HLG
  // in P010 and P016, which hardware decoders hand over.
  void hdrNoticeIsOnlyForFramesQtDoesNotToneMap() {
    using F = QVideoFrameFormat;
    QCOMPARE(VideoPresentation::hdrUnmapped(F::Format_P010, F::ColorTransfer_ST2084), 0);
    QCOMPARE(VideoPresentation::hdrUnmapped(F::Format_P016, F::ColorTransfer_STD_B67), 0);
    QCOMPARE(VideoPresentation::hdrUnmapped(F::Format_YUV420P10, F::ColorTransfer_ST2084), 1);
    QCOMPARE(VideoPresentation::hdrUnmapped(F::Format_NV12, F::ColorTransfer_STD_B67), 2);
    QCOMPARE(VideoPresentation::hdrUnmapped(F::Format_YUV420P, F::ColorTransfer_STD_B67), 2);
    QCOMPARE(VideoPresentation::hdrUnmapped(F::Format_YUV420P10, F::ColorTransfer_BT709), 0);
  }

  // An ffmpeg stand-in on the path, for copies that fail or never finish.
  static QString fakeFfmpeg(const QTemporaryDir& bin, const QByteArray& body) {
    QFile script(bin.filePath(QStringLiteral("ffmpeg")));
    if (!script.open(QIODevice::WriteOnly)) return {};
    script.write("#!/bin/sh\n" + body + "\n");
    script.close();
    script.setPermissions(script.permissions() | QFileDevice::ExeOwner);
    return script.fileName();
  }

  static qint64 readPid(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : 0;
  }

  // A copy is a process of its own, not a pool thread: changing videos or
  // quitting kills it at once, a copy that runs too long gives up with its
  // own reason, and a missing ffmpeg says so.
  void embeddedSubtitleCopiesStopWithTheirVideo() {
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    const QString pidFile = bin.filePath(QStringLiteral("pid"));
    QVERIFY(!fakeFfmpeg(bin, "echo $$ > \"" + pidFile.toUtf8() + "\"\nexec sleep 60").isEmpty());
    const QByteArray systemPath = qgetenv("PATH");
    qputenv("PATH", bin.path().toUtf8() + ':' + systemPath);
    const auto restorePath = qScopeGuard([systemPath] { qputenv("PATH", systemPath); });
    const QString tracks = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    const auto running = [](qint64 pid) { return pid > 0 && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid)); };
    const auto startCopy = [&](SubtitleIndex& index) {
      QFile::remove(pidFile);
      if (!index.embedded(tracks, 0).isEmpty()) return qint64(0);
      if (index.embeddedState(tracks, 0) != u"working") return qint64(0);
      QElapsedTimer started;
      started.start();
      while (readPid(pidFile) <= 0 && started.elapsed() < 5000) QTest::qWait(20);
      return readPid(pidFile);
    };

    {
      SubtitleIndex index;
      // The video changes.
      qint64 pid = startCopy(index);
      QVERIFY(running(pid));
      QElapsedTimer timer;
      timer.start();
      index.releaseEmbedded(tracks);
      QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
      QVERIFY(!running(pid));
      QCOMPARE(index.embeddedState(tracks, 0), QString());
      // The app quits: aboutToQuit stops every copy.
      pid = startCopy(index);
      QVERIFY(running(pid));
      timer.restart();
      index.stopEmbedded();
      QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
      QVERIFY(!running(pid));
      // Too slow: not a picture track, so not "failed".
      index.setEmbeddedTimeoutMs(300);
      pid = startCopy(index);
      QVERIFY(running(pid));
      QTRY_COMPARE_WITH_TIMEOUT(index.embeddedState(tracks, 0), QStringLiteral("slow"), 5000);
      QVERIFY(!running(pid));
      QCOMPARE(index.embeddedProblem(tracks, 0),
               QStringLiteral("These subtitles took too long to prepare for a delay"));
      // Asked again, it tries again.
      QFile::remove(pidFile);
      QVERIFY(index.embedded(tracks, 0).isEmpty());
      QCOMPARE(index.embeddedState(tracks, 0), QStringLiteral("working"));
      QTRY_VERIFY(running(readPid(pidFile)));
      QTRY_COMPARE_WITH_TIMEOUT(index.embeddedState(tracks, 0), QStringLiteral("slow"), 5000);
      index.releaseEmbedded(tracks);
      index.setEmbeddedTimeoutMs(SubtitleIndex::kEmbeddedTimeoutMs);
      // Destroyed with one running.
      pid = startCopy(index);
      QVERIFY(running(pid));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!running(readPid(pidFile)), 1500);

    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    qputenv("PATH", empty.path().toUtf8());
    SubtitleIndex index;
    QVERIFY(index.embedded(tracks, 0).isEmpty());
    QCOMPARE(index.embeddedState(tracks, 0), QStringLiteral("unavailable"));
    QCOMPARE(index.embeddedProblem(tracks, 0), QStringLiteral("Subtitle delay for this track needs ffmpeg"));
  }

  // A picture-based track (ffmpeg refuses to write it as text) takes the
  // delay back and says why; the delay is only claimed once its cues are in.
  void aPictureSubtitleTrackSaysItCannotBeShifted() {
    QTemporaryDir bin;
    QVERIFY(bin.isValid());
    QVERIFY(!fakeFfmpeg(bin, "echo 'Subtitle encoding currently only possible from text to text or bitmap to bitmap' >&2\n"
                             "exit 1").isEmpty());
    const QString folder = m_scratch.filePath(QStringLiteral("picture-subtitles"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/pictures.mkv");
    QVERIFY(QFile::exists(path) || QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
    const QByteArray systemPath = qgetenv("PATH");
    qputenv("PATH", bin.path().toUtf8() + ':' + systemPath);
    const auto restorePath = qScopeGuard([systemPath] { qputenv("PATH", systemPath); });
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);
    QTest::keyClick(m_window, Qt::Key_C);
    QTRY_COMPARE(player->activeSubtitleTrack(), 0);
    player->pause();
    QTest::keyClick(m_window, Qt::Key_X);
    // Not claimed before the shifted cues are in.
    QVERIFY2(prop("status").toString() != u"Subtitle delay +0.5 s", qPrintable(prop("status").toString()));
    QTRY_COMPARE_WITH_TIMEOUT(prop("status").toString(), QStringLiteral("Subtitle delay works only for text subtitles"), 5000);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
    m_window->setProperty("status", QString());
    QTest::keyClick(m_window, Qt::Key_X);
    QCOMPARE(prop("status").toString(), QStringLiteral("Subtitle delay works only for text subtitles"));
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
  }

  // A delay on one video's embedded track stays with that video. The next
  // one starts with no delay and no copy, and an earlier failure on its own
  // track number isn't reported against it.
  void anEmbeddedDelayStaysWithItsVideo() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) QSKIP("ffmpeg is not installed");
    const QString folder = m_scratch.filePath(QStringLiteral("embedded-step"));
    QVERIFY(QDir().mkpath(folder));
    QStringList paths;
    for (const char* name : {"a.mkv", "b.mkv", "c.mkv"}) {
      const QString path = folder + QLatin1Char('/') + QLatin1String(name);
      QVERIFY(QFile::exists(path) || QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), path));
      paths.append(path);
    }
    {
      // c's track 0 once failed to copy.
      QTemporaryDir bin;
      QVERIFY(bin.isValid());
      QVERIFY(!fakeFfmpeg(bin, "exit 1").isEmpty());
      const QByteArray systemPath = qgetenv("PATH");
      qputenv("PATH", bin.path().toUtf8() + ':' + systemPath);
      const auto restorePath = qScopeGuard([systemPath] { qputenv("PATH", systemPath); });
      QVERIFY(m_subtitles->embedded(paths[2], 0).isEmpty());
      QTRY_COMPARE_WITH_TIMEOUT(m_subtitles->embeddedState(paths[2], 0), QStringLiteral("failed"), 5000);
    }
    open(paths);
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    const auto delayOn = [&](const QString& path) {
      QTRY_COMPARE(m_session->property("path").toString(), path);
      QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
      QTRY_COMPARE(player->subtitleTracks().size(), 1);
      QTest::keyClick(m_window, Qt::Key_C);
      QTRY_COMPARE(player->activeSubtitleTrack(), 0);
      QTest::keyClick(m_window, Qt::Key_X);
      QTRY_VERIFY_WITH_TIMEOUT(!prop("embeddedSubtitleCopy").toString().isEmpty(), 15000);
      QCOMPARE(prop("status").toString(), QStringLiteral("Subtitle delay +0.5 s"));
    };
    delayOn(paths[0]);
    const QString copy = prop("embeddedSubtitleCopy").toString();
    QVERIFY(QFile::exists(copy));
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTRY_COMPARE(m_session->property("path").toString(), paths[1]);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
    QCOMPARE(prop("activeEmbeddedTrack").toInt(), -1);
    QCOMPARE(prop("embeddedSubtitleCopy").toString(), QString());
    // The last video's copy went with it, and none was started for this one.
    QCOMPARE(m_subtitles->embeddedState(paths[0], 0), QString());
    QVERIFY(!QFile::exists(copy));
    QTest::qWait(300);
    QCOMPARE(m_subtitles->embeddedState(paths[1], 0), QString());

    delayOn(paths[1]);
    m_window->setProperty("status", QString());
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTRY_COMPARE(m_session->property("path").toString(), paths[2]);
    QTest::qWait(300);
    QCOMPARE(prop("subtitleOffsetMs").toInt(), 0);
    QVERIFY2(prop("status").toString() != u"Subtitle delay works only for text subtitles",
             qPrintable(prop("status").toString()));
    player->pause();
  }

  // Without mpv the handoff fails and says so, and the video keeps playing.
  void aFailedMpvHandoffKeepsPlaying() {
    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    const QByteArray systemPath = qgetenv("PATH");
    qputenv("PATH", empty.path().toUtf8());
    const auto restorePath = qScopeGuard([systemPath] { qputenv("PATH", systemPath); });
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QMetaObject::invokeMethod(m_window, "perform", Q_ARG(QVariant, QStringLiteral("play")));
    QTRY_VERIFY(prop("status").toString().contains(QStringLiteral("mpv")));
    QTest::qWait(200);
    QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    player->pause();
  }

  // Qt 6.12's player stops on any write to its source, so the session saying
  // the same file again must leave playback alone.
  void videoKeepsPlayingWhenTheSameFileIsAnnounced() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    emit m_session->currentChanged();
    QTest::qWait(200);
    QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    player->pause();
  }

  // Media keys and the shell's player controls reach a playing video, and the
  // player leaves the bus with it.
  void videoIsControlledOverMpris() {
    QDBusConnection client(QStringLiteral("omaroll-test-client"));
    if (!client.isConnected()) {
      QSKIP("dbus-daemon is not available");
    }
    QTRY_VERIFY(!m_mpris->isPublished());
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_VERIFY(m_mpris->isPublished());
    const QString service = m_mpris->serviceName();
    QCOMPARE(service, QStringLiteral("org.mpris.MediaPlayer2.omaroll"));
    QVERIFY(client.interface()->isServiceRegistered(service));

    const QString root = QStringLiteral("org.mpris.MediaPlayer2");
    const QString remote = QStringLiteral("org.mpris.MediaPlayer2.Player");
    QCOMPARE(mprisProperty(root, QStringLiteral("Identity")).toString(), QStringLiteral("Omaroll"));
    QCOMPARE(mprisProperty(root, QStringLiteral("DesktopEntry")).toString(),
             QStringLiteral("io.github.tsouth89.omaroll"));
    QVERIFY(mprisProperty(root, QStringLiteral("CanRaise")).toBool());
    QCOMPARE(mprisProperty(remote, QStringLiteral("PlaybackStatus")).toString(),
             QStringLiteral("Playing"));
    QVERIFY(mprisProperty(remote, QStringLiteral("CanPause")).toBool());
    QVERIFY(!mprisProperty(remote, QStringLiteral("CanGoNext")).toBool());

    const QVariantMap metadata =
        qdbus_cast<QVariantMap>(mprisProperty(remote, QStringLiteral("Metadata")).value<QDBusArgument>());
    QVERIFY(metadata.value(QStringLiteral("xesam:url")).toString().endsWith(QStringLiteral("/clip.mp4")));
    QVERIFY(!metadata.value(QStringLiteral("xesam:title")).toString().isEmpty());
    const QDBusObjectPath track = metadata.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>();
    QVERIFY(track.path().startsWith(QStringLiteral("/io/github/tsouth89/omaroll/track/")));
    // Cover art is a poster frame kept in the thumbnail cache.
    if (!QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer")).isEmpty()) {
      QTRY_VERIFY_WITH_TIMEOUT(
          m_mpris->metadata().contains(QStringLiteral("mpris:artUrl")), 15000);
      const QUrl art(m_mpris->metadata().value(QStringLiteral("mpris:artUrl")).toString());
      QVERIFY(art.isLocalFile());
      QVERIFY(art.toLocalFile().startsWith(ThumbnailCache::cacheDirectory()));
      QVERIFY(!QImage(art.toLocalFile()).isNull());
    }

    PropertiesWatcher watcher;
    QVERIFY(client.connect(service, QStringLiteral("/org/mpris/MediaPlayer2"),
                           QStringLiteral("org.freedesktop.DBus.Properties"),
                           QStringLiteral("PropertiesChanged"), &watcher,
                           SLOT(changed(QString, QVariantMap, QStringList))));
    QVERIFY(client.connect(service, QStringLiteral("/org/mpris/MediaPlayer2"), remote,
                           QStringLiteral("Seeked"), &watcher, SLOT(seeked(qlonglong))));
    QCOMPARE(mprisCall(remote, QStringLiteral("Pause")).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QTRY_VERIFY(std::any_of(watcher.changes.cbegin(), watcher.changes.cend(), [](const QVariantMap& values) {
      return values.value(QStringLiteral("PlaybackStatus")).toString() == QStringLiteral("Paused");
    }));
    // Even a short step while paused is announced, so clients do not drift.
    QTRY_VERIFY(player->isSeekable());
    QVERIFY(player->duration() > 2000);
    const qint64 before = player->position();
    QCOMPARE(mprisCall(remote, QStringLiteral("Seek"), {qlonglong(300000)}).type(),
             QDBusMessage::ReplyMessage);
    QTRY_VERIFY(player->position() >= before + 250);
    QTRY_VERIFY(!watcher.seeks.isEmpty());
    QCOMPARE(mprisCall(remote, QStringLiteral("PlayPause")).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);

    QCOMPARE(mprisCall(remote, QStringLiteral("SetPosition"),
                       {QVariant::fromValue(track), qlonglong(1500000)})
                 .type(),
             QDBusMessage::ReplyMessage);
    QTRY_VERIFY(player->position() >= 1400);

    const qreal volume = m_settings->videoVolume();
    QDBusMessage set = mprisCall(QStringLiteral("org.freedesktop.DBus.Properties"),
                                 QStringLiteral("Set"),
                                 {remote, QStringLiteral("Volume"),
                                  QVariant::fromValue(QDBusVariant(0.25))});
    QCOMPARE(set.type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(m_settings->videoVolume(), 0.25);
    m_settings->setVideoVolume(volume);

    // Stop rewinds and reads as stopped, unlike a pause, until played again.
    QCOMPARE(mprisCall(remote, QStringLiteral("Stop")).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QTRY_VERIFY(player->position() < 500);
    QCOMPARE(mprisProperty(remote, QStringLiteral("PlaybackStatus")).toString(),
             QStringLiteral("Stopped"));
    QCOMPARE(mprisCall(remote, QStringLiteral("Play")).type(), QDBusMessage::ReplyMessage);
    QTRY_COMPARE(mprisProperty(remote, QStringLiteral("PlaybackStatus")).toString(),
                 QStringLiteral("Playing"));

    m_window->close();
    QTRY_VERIFY(!m_mpris->isPublished());
    QTRY_VERIFY(!client.interface()->isServiceRegistered(service));
  }

  void closingStopsPlaybackAndForgetsTheFile() {
    open({media(QStringLiteral("clip.mp4"))});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!m_window->isVisible());
    QVERIFY(player->playbackState() != QMediaPlayer::PlayingState);
    QCOMPARE(m_session->count(), 0);
    QVERIFY(m_session->path().isEmpty());
  }

  void escapeLeavesFullScreenBeforeClosing() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTest::keyClick(m_window, Qt::Key_F);
    QTRY_VERIFY(prop("fullScreen").toBool());
    QTest::keyClick(m_window, Qt::Key_I);
    QVERIFY(prop("infoOpen").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!prop("fullScreen").toBool());
    QVERIFY(m_window->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!prop("infoOpen").toBool());
    QVERIFY(m_window->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!m_window->isVisible());
  }

  void enterAndTheGridButtonHandTheFileToTheLibrary() {
    const QString path = media(QStringLiteral("shot 10.jpg"));
    open({path});
    QSignalSpy library(m_session, &ViewerSession::libraryRequested);
    QTest::keyClick(m_window, Qt::Key_Return);
    QCOMPARE(library.size(), 1);
    QCOMPARE(library.first().first().toString(), path);
    m_window->setProperty("chromePinned", true);
    click(item(QStringLiteral("viewerLibraryButton")));
    QCOMPARE(library.size(), 2);
    m_window->setProperty("chromePinned", false);
  }

  void theFilmstripShowsTheFolderAndOpensWhatIsClicked() {
    m_settings->setViewerFilmstrip(true);
    open({media(QStringLiteral("shot 2.jpg"))});
    QTRY_COMPARE(m_session->count(), int(m_order.size()));
    QTRY_VERIFY(prop("imageReady").toBool());
    m_window->setProperty("chromePinned", true);
    const auto restore = qScopeGuard([this] {
      m_window->setProperty("chromePinned", false);
      m_settings->setViewerFilmstrip(false);
    });

    QQuickItem* strip = item(QStringLiteral("viewerFilmstrip"));
    QTRY_VERIFY(strip->isVisible());
    QCOMPARE(strip->property("currentIndex").toInt(), 2);
    // It sits above the toolbar rather than over it.
    QQuickItem* toolbar = item(QStringLiteral("viewerToolbar"));
    QVERIFY(strip->mapToScene(QPointF(0, strip->height())).y() <=
            toolbar->mapToScene(QPointF(0, 0)).y());

    QQuickItem* tile = nullptr;
    QTRY_VERIFY((tile = find(m_window->contentItem(), [](QQuickItem* candidate) {
                   return candidate->objectName() == QStringLiteral("viewerFilmstripTile1");
                 })));
    click(tile);
    QTRY_COMPARE(m_session->path(), m_order.at(1));
    QCOMPARE(strip->property("currentIndex").toInt(), 1);

    // B puts it away and brings it back, and the choice is remembered.
    QTest::keyClick(m_window, Qt::Key_B);
    QVERIFY(!m_settings->viewerFilmstrip());
    QTRY_VERIFY(!strip->isVisible());
    QObject* list = m_window->findChild<QObject*>(QStringLiteral("viewerFilmstripList"));
    QVERIFY(list);
    QTRY_COMPARE(list->property("count").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_B);
    QVERIFY(m_settings->viewerFilmstrip());
    QTRY_VERIFY(strip->isVisible());

    // A single file has nothing to show beside it.
    const QString alone = m_scratch.filePath(QStringLiteral("alone"));
    QVERIFY(QDir().mkpath(alone));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), alone + QStringLiteral("/only.jpg")));
    open({alone + QStringLiteral("/only.jpg")});
    QTRY_VERIFY(prop("imageReady").toBool());
    QCOMPARE(m_session->count(), 1);
    QVERIFY(!strip->isVisible());
  }

  void filmstripRefreshesWhenFolderPathsChangeWithoutChangingCount() {
    const QString folder = m_scratch.filePath(QStringLiteral("filmstrip-refresh"));
    QVERIFY(QDir().mkpath(folder));
    const QString first = folder + QStringLiteral("/a.png");
    const QString second = folder + QStringLiteral("/b.png");
    const QString renamed = folder + QStringLiteral("/0.png");
    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(first));
    QVERIFY(image.save(second));
    m_settings->setViewerFilmstrip(true);
    m_window->setProperty("chromePinned", true);
    const auto restore = qScopeGuard([this] {
      m_window->setProperty("chromePinned", false);
      m_settings->setViewerFilmstrip(false);
    });
    open({first});
    QTRY_COMPARE(m_session->count(), 2);
    QQuickItem* picture = nullptr;
    QTRY_VERIFY((picture = find(m_window->contentItem(), [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("viewerFilmstripPicture0");
    })));
    const QUrl before = picture->property("source").toUrl();
    QVERIFY(!before.isEmpty());
    const quint64 revision = m_session->sequenceRevision();
    QVERIFY(QFile::rename(second, renamed));
    QTRY_VERIFY(m_session->sequenceRevision() > revision);
    QCOMPARE(m_session->count(), 2);
    QTRY_COMPARE(picture->property("source").toUrl(),
                 QUrl(m_session->thumbnailUrl(0, m_window->devicePixelRatio())));
    QVERIFY(picture->property("source").toUrl() != before);

    // Replacing the current file also refreshes its thumbnail URL.
    const quint64 replacementRevision = m_session->sequenceRevision();
    QVERIFY(QFile::remove(first));
    image.fill(Qt::blue);
    QVERIFY(image.save(first));
    QTRY_VERIFY(m_session->sequenceRevision() > replacementRevision);
    QCOMPARE(m_session->count(), 2);
  }

  void filmstripShowsRawFormatsAndPairedBadgesInBothDirections() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString folder = m_scratch.filePath(QStringLiteral("filmstrip-raw-pairs"));
    QVERIFY(QDir().mkpath(folder));
    const QString raw = folder + QStringLiteral("/a.DNG");
    const QString jpeg = folder + QStringLiteral("/a.jpg");
    const QString loneRaw = folder + QStringLiteral("/b.dng");
    const QString fixture = QFINDTESTDATA("fixtures/raw/camera.dng");
    QVERIFY(QFile::copy(fixture, raw));
    QVERIFY(QFile::copy(fixture, loneRaw));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), jpeg));
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), folder + QStringLiteral("/clip.mp4")));
    const bool previousPairing = m_session->pairRawJpeg();
    const bool previousStrip = m_settings->viewerFilmstrip();
    const auto restore = qScopeGuard([this, previousPairing, previousStrip] {
      m_window->setProperty("chromePinned", false);
      m_settings->setViewerFilmstrip(previousStrip);
      m_session->setPairRawJpeg(previousPairing);
    });
    m_session->setPairRawJpeg(true);
    m_settings->setViewerFilmstrip(true);
    m_window->setProperty("chromePinned", true);
    open({jpeg});
    QTRY_COMPARE(m_session->count(), 3);
    QCOMPARE(m_session->path(), jpeg);
    const auto badge = [this](int index) {
      const QString name = QStringLiteral("viewerFilmstripRawBadge%1").arg(index);
      return find(m_window->contentItem(), [&name](QQuickItem* candidate) {
        return candidate->objectName() == name;
      });
    };
    const auto label = [&badge](int index) {
      QQuickItem* current = badge(index);
      return current ? current->property("label").toString() : QString();
    };
    QTRY_COMPARE(label(0), QStringLiteral("RAW+JPG"));
    QVERIFY(badge(0)->isVisible());
    QTRY_COMPARE(label(1), QStringLiteral("DNG"));
    QVERIFY(badge(1)->isVisible());
    QVERIFY(m_session->isVideoAt(2));
    QTRY_VERIFY(badge(2));
    QVERIFY(label(2).isEmpty());
    QVERIFY(m_session->switchCompanion());
    QCOMPARE(m_session->path(), raw);
    // Switching can rebuild ListView delegates. Resolve the current tile on
    // each check instead of keeping a pointer to the old QML object.
    QTRY_COMPARE(label(0), QStringLiteral("RAW+JPG"));
    m_session->setPairRawJpeg(false);
    QCOMPARE(m_session->count(), 4);
    QCOMPARE(m_session->path(), raw);
    QTRY_COMPARE(label(0), QStringLiteral("DNG"));
  }

  void copyPathAndNamePreserveTheOpenedEntry_data() {
    QTest::addColumn<QSize>("windowSize");
    QTest::newRow("normal") << QSize(1200, 800);
    QTest::newRow("minimum") << QSize(320, 240);
  }

  void copyPathAndNamePreserveTheOpenedEntry() {
    QFETCH(QSize, windowSize);
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString target = folder.filePath(QStringLiteral("target.png"));
    QImage image(80, 60, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(target));
    const QString alias = folder.filePath(QStringLiteral("opened ü name.png"));
    QVERIFY(QFile::link(target, alias));
    open({target});
    m_window->resize(windowSize);
    m_session->setDeletionPaths({{target, alias}});
    QTRY_VERIFY(prop("imageReady").toBool());
    // Exercise the helper's Qt fallback without a compositor clipboard process.
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    qputenv("PATH", QByteArray());
    QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), alias);
    QCOMPARE(prop("status").toString(), QStringLiteral("Path copied"));
    QTest::keyClick(m_window, Qt::Key_I);
    QCOMPARE(item(QStringLiteral("viewerInfoPanel"))->property("filePath").toString(), alias);
    QCOMPARE(item(QStringLiteral("viewerInfoPanel"))->property("resolvedPath").toString(), target);
    QVariantList lines;
    for (int index = 0; index < 30; ++index) lines.append(QStringLiteral("Metadata line %1").arg(index));
    QQuickItem* panel = item(QStringLiteral("viewerInfoPanel"));
    QVERIFY(QQmlProperty(panel, QStringLiteral("lines")).write(lines));
    // Copy actions remain inside the card even when its details need scrolling.
    QQuickItem* copyName = item(QStringLiteral("viewerInfoCopyName"));
    QQuickItem* copyPath = item(QStringLiteral("viewerInfoCopyPath"));
    const auto insideCard = [panel](QQuickItem* button) {
      const QRectF card(panel->mapToScene(QPointF(16, 16)), panel->size() - QSizeF(32, 32));
      return button->isVisible() && card.contains(QRectF(button->mapToScene(QPointF()), button->size()));
    };
    QTRY_VERIFY(insideCard(copyName) && insideCard(copyPath));
    click(copyName);
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("opened ü name.png"));
    click(copyPath);
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), alias);
    QTest::keyClick(m_window, Qt::Key_I);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, QPoint(150, 120));
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(menuIds(menu).contains(QStringLiteral("copy-path")));
    QVERIFY(menuIds(menu).contains(QStringLiteral("copy-name")));
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("copy-name")), "click"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("opened ü name.png"));
  }

  void fitWidthAndKeyboardPanKeepPlainNavigation() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString tall = folder.filePath(QStringLiteral("tall.png"));
    const QString next = folder.filePath(QStringLiteral("next.png"));
    QImage image(600, 4000, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(tall));
    QVERIFY(image.save(next));
    open({tall, next});
    QTRY_VERIFY(prop("imageReady").toBool());
    QQuickItem* still = item(QStringLiteral("viewerStill"));
    QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(still->property("contentY").toReal(), 0.0);
    QTest::keyClick(m_window, Qt::Key_W);
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("width"));
    QTRY_VERIFY(qAbs(prop("displayWidth").toReal() - m_window->width()) < 0.01);
    QCOMPARE(still->property("contentY").toReal(), 0.0);
    QVERIFY(still->property("interactive").toBool());
    QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(still->property("contentY").toReal(), 80.0);
    QCOMPARE(m_session->path(), tall);
    QTest::keyClick(m_window, Qt::Key_Up, Qt::ShiftModifier);
    QCOMPARE(still->property("contentY").toReal(), 0.0);
    for (int i = 0; i < 120; ++i) QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(still->property("contentY").toReal(),
             still->property("contentHeight").toReal() - still->height());
    m_window->resize(600, 400);
    QTRY_VERIFY(qAbs(prop("displayWidth").toReal() - 600) < 0.01);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("zoomPercent").toInt(), 100);
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("fit"));
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_COMPARE(prop("viewScale").toReal(), 0.0);
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("fit"));
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("zoomPercent").toInt(), 100);
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), next);
    QTRY_COMPARE(prop("fitMode").toString(), QStringLiteral("fit"));
  }

  // Shift+W crops a picture to cover the window, centred, stays centred as
  // the tile changes shape and while stepping through the folder, and fits
  // the whole picture again on a second press.
  void fillCoversTheWindowCentredAndTogglesBack() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString wide = folder.filePath(QStringLiteral("wide.png"));
    const QString tall = folder.filePath(QStringLiteral("tall.png"));
    QImage image(1600, 400, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(wide));
    const QString tallAgain = folder.filePath(QStringLiteral("tall2.png"));
    QVERIFY(image.transformed(QTransform().rotate(90)).save(tall));
    QVERIFY(image.transformed(QTransform().rotate(90)).save(tallAgain));
    open({wide, tall, tallAgain});
    QTRY_VERIFY(prop("imageReady").toBool());
    QQuickItem* still = item(QStringLiteral("viewerStill"));
    m_window->resize(600, 400);
    QTRY_VERIFY(qAbs(prop("displayWidth").toReal() - still->width()) < 0.01);
    const auto centred = [still] {
      return qAbs(still->property("contentX").toReal()
                  - (still->property("contentWidth").toReal() - still->width()) / 2) < 0.5 &&
             qAbs(still->property("contentY").toReal()
                  - (still->property("contentHeight").toReal() - still->height()) / 2) < 0.5;
    };

    QTest::keyClick(m_window, Qt::Key_W, Qt::ShiftModifier);
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("fill"));
    QTRY_VERIFY(qAbs(prop("displayHeight").toReal() - still->height()) < 0.01);
    QVERIFY(prop("displayWidth").toReal() > still->width());
    QTRY_VERIFY(centred());
    QVERIFY(still->property("interactive").toBool());

    m_window->resize(500, 500);
    QTRY_VERIFY(qAbs(prop("displayHeight").toReal() - still->height()) < 0.01);
    QTRY_VERIFY(centred());

    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), tall);
    QTRY_VERIFY(prop("imageReady").toBool());
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("fill"));
    QTRY_VERIFY(qAbs(prop("displayWidth").toReal() - still->width()) < 0.01);
    QVERIFY(prop("displayHeight").toReal() > still->height());
    QTRY_VERIFY(centred());
    // A picture of the same size changes no dimensions, and is centred too.
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->path(), tallAgain);
    QTRY_VERIFY(prop("imageReady").toBool());
    QTRY_VERIFY(centred());

    QTest::keyClick(m_window, Qt::Key_W, Qt::ShiftModifier);
    QCOMPARE(prop("fitMode").toString(), QStringLiteral("fit"));
    QTRY_VERIFY(qAbs(prop("displayHeight").toReal() - still->height()) < 0.01);
    QVERIFY(prop("displayWidth").toReal() < still->width());
  }

  void keyboardPanClampsBothAxesAtActualSize() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = folder.filePath(QStringLiteral("large.png"));
    QImage image(2000, 1800, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(!prop("zooming").toBool());
    QQuickItem* still = item(QStringLiteral("viewerStill"));
    still->setProperty("contentX", 0);
    still->setProperty("contentY", 0);
    QTest::keyClick(m_window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(still->property("contentX").toReal(), 80.0);
    QCOMPARE(still->property("contentY").toReal(), 80.0);
    for (int i = 0; i < 40; ++i) {
      QTest::keyClick(m_window, Qt::Key_Left, Qt::ShiftModifier);
      QTest::keyClick(m_window, Qt::Key_Up, Qt::ShiftModifier);
    }
    QCOMPARE(still->property("contentX").toReal(), 0.0);
    QCOMPARE(still->property("contentY").toReal(), 0.0);
  }

  void smallPictureEnlargementIsOptionalAndRemembered() {
    QVERIFY(m_settings->enlargeSmallPictures());
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString path = folder.filePath(QStringLiteral("tiny.png"));
    QImage image(40, 30, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(path));
    open({path});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(prop("zoomPercent").toInt() > 100);
    QSignalSpy changed(m_settings, &AppSettings::enlargeSmallPicturesChanged);
    const auto restore = qScopeGuard([&] { m_settings->setEnlargeSmallPictures(true); });
    m_settings->setEnlargeSmallPictures(false);
    QCOMPARE(changed.count(), 1);
    m_settings->setEnlargeSmallPictures(false);
    QCOMPARE(changed.count(), 1);
    AppSettings saved;
    QVERIFY(!saved.enlargeSmallPictures());
    QTRY_COMPARE(prop("zoomPercent").toInt(), 100);
    m_settings->setEnlargeSmallPictures(true);
    QTRY_VERIFY(prop("zoomPercent").toInt() > 100);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, QPoint(150, 120));
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("fit-shrink")), "click"));
    QTRY_COMPARE(prop("zoomPercent").toInt(), 100);
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_VERIFY(prop("zoomPercent").toInt() > 100);
  }

  void shortcutHelpIsModalScrollableAndRestoresFocus_data() {
    QTest::addColumn<bool>("video");
    QTest::addColumn<QSize>("size");
    QTest::newRow("picture") << false << QSize(420, 800);
    QTest::newRow("wide-picture") << false << QSize(1920, 1080);
    QTest::newRow("narrow-picture") << false << QSize(320, 240);
    QTest::newRow("video") << true << QSize(420, 800);
    QTest::newRow("wide-video") << true << QSize(1920, 1080);
  }

  void shortcutHelpIsModalScrollableAndRestoresFocus() {
    QFETCH(bool, video);
    QFETCH(QSize, size);
    open({media(video ? QStringLiteral("clip.mp4") : QStringLiteral("Shot 1.jpg"))});
    m_window->resize(size);
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("viewerShortcutHelp"));
    QVERIFY(help);
    QTest::keyClick(m_window, Qt::Key_Question, Qt::ShiftModifier);
    QTRY_VERIFY(help->property("visible").toBool());
    QVERIFY(prop("blockingActionOpen").toBool());
    QCOMPARE(help->property("video").toBool(), video);
    QVERIFY(help->property("width").toReal() <= size.width() - 24);
    QVERIFY(help->property("height").toReal() <= size.height() - 24);
    QQuickItem* scroll = item(QStringLiteral("viewerShortcutScroll"));
    QVERIFY(scroll);
    QQuickItem* flick = scroll->property("contentItem").value<QQuickItem*>();
    QVERIFY(flick);
    const QString path = m_session->path();
    if (size.width() >= 1920) {
      QTRY_VERIFY(flick->property("contentHeight").toReal() <= flick->height());
    } else {
      QTRY_VERIFY(flick->property("contentHeight").toReal() > flick->height());
      QTest::keyClick(m_window, Qt::Key_PageDown);
      QTRY_VERIFY(flick->property("contentY").toReal() > 0);
      QCOMPARE(m_session->path(), path);
      QTest::keyClick(m_window, Qt::Key_Home);
      QTRY_COMPARE(flick->property("contentY").toReal(), 0.0);
    }
    QTest::keyClick(m_window, Qt::Key_Question, Qt::ShiftModifier);
    QTRY_VERIFY(!help->property("visible").toBool());
    QTRY_VERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!help->property("visible").toBool());
    QVERIFY(m_window->isVisible());
    QTRY_VERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());
  }

  void typingInShortcutHelpFiltersAndCtrlQAsksToQuit() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("viewerShortcutHelp"));
    QVERIFY(help);
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    // Typing right away filters, without clicking the field first.
    for (const char key : {'t', 'r', 'a', 's', 'h'}) QTest::keyClick(m_window, key);
    QQuickItem* filter = item(QStringLiteral("shortcutFilter"));
    QVERIFY(filter);
    QTRY_COMPARE(filter->property("text").toString(), QStringLiteral("trash"));
    QCOMPARE(help->property("filter").toString(), QStringLiteral("trash"));
    QVERIFY(filter->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!help->property("visible").toBool());
    QTRY_VERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());

    // Ctrl+Q asks the engine to quit; main.cpp connects that to the app.
    QSignalSpy quit(m_engine, &QQmlEngine::quit);
    QTest::keyClick(m_window, Qt::Key_Q, Qt::ControlModifier);
    QTRY_COMPARE(quit.count(), 1);
  }

  void shortcutHelpShowsAvailableVideoControls() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tracks = dir.filePath(QStringLiteral("tracks.mkv"));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/viewer/tracks.mkv"), tracks));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/viewer/captions.srt"),
                        dir.filePath(QStringLiteral("tracks.srt"))));
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("viewerShortcutHelp"));
    QVERIFY(help);
    const auto hasKey = [help](const QString& key) {
      auto* content = help->property("contentItem").value<QQuickItem*>();
      return content && find(content, [&key](QQuickItem* child) {
        return child->property("text").toString() == key;
      });
    };
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    QVERIFY(!hasKey(QStringLiteral("Shift+L")));
    QVERIFY(!hasKey(QStringLiteral("Shift+A")));
    QVERIFY(!hasKey(QStringLiteral("Z / X")));
    QTest::keyClick(m_window, Qt::Key_Escape);

    open({tracks});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_COMPARE(player->audioTracks().size(), 2);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);
    player->pause();
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(hasKey(QStringLiteral("Shift+L")));
    QVERIFY(hasKey(QStringLiteral("Shift+A")));
    QVERIFY(!hasKey(QStringLiteral("Z / X")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTest::keyClick(m_window, Qt::Key_C); // Embedded subtitles, which a delay shifts too.
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(hasKey(QStringLiteral("Z / X")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTest::keyClick(m_window, Qt::Key_C); // Sidecar subtitles.
    QTRY_VERIFY(!prop("externalSubtitle").toString().isEmpty());
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(hasKey(QStringLiteral("Z / X")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTest::keyClick(m_window, Qt::Key_C); // Off.
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(!hasKey(QStringLiteral("Z / X")));
    QTest::keyClick(m_window, Qt::Key_Escape);

    open({media(QStringLiteral("clip.mp4"))});
    QTRY_VERIFY(!prop("hasAudioChoices").toBool());
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(hasKey(QStringLiteral("Shift+L")));
    QVERIFY(!hasKey(QStringLiteral("Shift+A")));
    QVERIFY(!hasKey(QStringLiteral("Z / X")));
    QTest::keyClick(m_window, Qt::Key_Escape);
  }

  void theMenuOffersOnlyWhatSuitsTheFile() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_COMPARE(m_session->count(), int(m_order.size()));
    QTRY_VERIFY(prop("imageReady").toBool());
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier,
                      QPoint(m_window->width() / 2, m_window->height() / 2));
    QTRY_VERIFY(menu->property("visible").toBool());
    const QStringList still = menuIds(menu);
    for (const QString& id : {QStringLiteral("library"), QStringLiteral("rotate"),
                              QStringLiteral("fullscreen"), QStringLiteral("info"),
                              QStringLiteral("favorite"), QStringLiteral("trash")}) {
      QVERIFY2(still.contains(id), qPrintable(id));
    }
    QVERIFY(!still.contains(QStringLiteral("trim")));
    QVERIFY(!still.contains(QStringLiteral("frame")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
    // The menu closing leaves the keyboard with the viewer.
    QVERIFY(m_window->isVisible());
    QTest::keyClick(m_window, Qt::Key_Right);
    QCOMPARE(m_session->index(), 2);

    m_session->jump(0);
    QVERIFY(m_session->isVideo());
    const QStringList video = menuIds(menu);
    QVERIFY(!video.contains(QStringLiteral("rotate")));
    QVERIFY(video.contains(QStringLiteral("library")));
  }

  void slideshowMenuKeepsItsTargetAndResumesAfterAnAction() {
    const QString first = media(QStringLiteral("Shot 1.jpg"));
    const QString second = media(QStringLiteral("shot 2.jpg"));
    const int interval = m_settings->slideshowIntervalSeconds();
    const bool shuffle = m_settings->slideshowShuffle();
    const bool firstFavorite = m_settings->isFavorite(first);
    const bool secondFavorite = m_settings->isFavorite(second);
    const auto restoreSettings = qScopeGuard([this, interval, shuffle, first, second,
                                              firstFavorite, secondFavorite] {
      m_settings->setSlideshowIntervalSeconds(interval);
      m_settings->setSlideshowShuffle(shuffle);
      m_settings->setFavorite({first}, firstFavorite);
      m_settings->setFavorite({second}, secondFavorite);
    });
    m_settings->setSlideshowIntervalSeconds(2);
    m_settings->setSlideshowShuffle(false);
    m_settings->setFavorite({first, second}, false);
    open({first, second});
    QTRY_VERIFY(prop("imageReady").toBool());
    QTest::keyClick(m_window, Qt::Key_F5);
    QVERIFY(prop("slideshowRunning").toBool());
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier,
                      QPoint(m_window->width() / 2, m_window->height() / 2));
    QTRY_VERIFY(menu->property("visible").toBool());
    QSignalSpy changed(m_session, &ViewerSession::currentChanged);
    // Cross the real timer interval while the menu owns the action target.
    QTest::qWait(2300);
    // Video completion uses this same advancement entry point.
    QVERIFY(QMetaObject::invokeMethod(m_window, "advanceSlideshow"));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(m_session->path(), first);
    QVERIFY(menu->property("visible").toBool());
    QVERIFY(prop("slideshowRunning").toBool());
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("favorite")), "click"));
    QTRY_VERIFY(!menu->property("visible").toBool());
    QVERIFY(m_settings->isFavorite(first));
    QVERIFY(!m_settings->isFavorite(second));
    QTRY_COMPARE_WITH_TIMEOUT(m_session->path(), second, 4000);
    QVERIFY(prop("slideshowRunning").toBool());
  }

  void slideshowMenuSkipsAnExcludedPausedVideo_data() {
    QTest::addColumn<bool>("shuffle");
    QTest::newRow("ordered") << false;
    QTest::newRow("shuffled") << true;
  }

  void slideshowMenuSkipsAnExcludedPausedVideo() {
    QFETCH(bool, shuffle);
    const bool oldVideos = m_settings->slideshowVideos(), oldShuffle = m_settings->slideshowShuffle();
    const auto restore = qScopeGuard([&] { m_settings->setSlideshowVideos(oldVideos); m_settings->setSlideshowShuffle(oldShuffle); });
    m_settings->setSlideshowVideos(false);
    m_settings->setSlideshowShuffle(shuffle);
    const QString image = media(QStringLiteral("Shot 1.jpg"));
    open({media(QStringLiteral("clip.mp4")), image});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    player->pause();
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, QPoint(m_window->width() / 2, m_window->height() / 2));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(menuRow(QStringLiteral("slideshow")), "click"));
    QTRY_COMPARE_WITH_TIMEOUT(m_session->path(), image, 1500);
    QTRY_VERIFY(!menu->property("visible").toBool());
    QVERIFY(prop("slideshowRunning").toBool());
  }

  void changingTheMenuTargetClosesIt_data() {
    QTest::addColumn<bool>("replaceSession");
    QTest::newRow("manual-navigation") << false;
    QTest::newRow("replacement-open") << true;
  }

  void changingTheMenuTargetClosesIt() {
    QFETCH(bool, replaceSession);
    const QString first = media(QStringLiteral("Shot 1.jpg"));
    const QString second = media(QStringLiteral("shot 2.jpg"));
    open({first, second});
    QTRY_VERIFY(prop("imageReady").toBool());
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QVERIFY(menu);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier,
                      QPoint(m_window->width() / 2, m_window->height() / 2));
    QTRY_VERIFY(menu->property("visible").toBool());
    if (replaceSession) {
      m_session->open({second, first});
    } else {
      QVERIFY(QMetaObject::invokeMethod(m_window, "step", Q_ARG(QVariant, 1)));
    }
    QCOMPARE(m_session->path(), second);
    QTRY_VERIFY(!menu->property("visible").toBool());
    QTRY_VERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());
    // The dismissed popup leaves the viewer's navigation controls usable.
    QTest::keyClick(m_window, replaceSession ? Qt::Key_Right : Qt::Key_Left);
    QCOMPARE(m_session->path(), first);
  }

  void favouriteAndRatingFromTheKeyboardShowInTheDetails() {
    const QString path = media(QStringLiteral("shot 2.jpg"));
    open({path});
    QVERIFY(!m_settings->isFavorite(path));
    QTest::keyClick(m_window, Qt::Key_I);
    QQuickItem* panel = item(QStringLiteral("viewerInfoPanel"));
    QVERIFY(panel->isVisible());
    QCOMPARE(item(QStringLiteral("viewerInfoName"))->property("text").toString(),
             QStringLiteral("shot 2.jpg"));
    QTest::keyClick(m_window, Qt::Key_V);
    QVERIFY(m_settings->isFavorite(path));
    QVERIFY(item(QStringLiteral("viewerInfoFavourite"))->property("active").toBool());
    QTest::keyClick(m_window, Qt::Key_4, Qt::AltModifier);
    QCOMPARE(m_settings->rating(path), 4);
    QCOMPARE(prop("rating").toInt(), 4);
    QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(m_settings->rating(path), 0);
    m_settings->setFavorite({path}, false);
    QTest::keyClick(m_window, Qt::Key_I);
    QVERIFY(!panel->isVisible());
  }

  void slideshowRunsFullScreenAndEscapeEndsIt() {
    open({media(QStringLiteral("Shot 1.jpg"))});
    QTRY_COMPARE(m_session->count(), int(m_order.size()));
    QTRY_VERIFY(prop("imageReady").toBool());
    m_settings->setSlideshowIntervalSeconds(2);
    QTest::keyClick(m_window, Qt::Key_F5);
    QVERIFY(prop("slideshowRunning").toBool());
    QTRY_VERIFY(prop("fullScreen").toBool());
    const QString first = m_session->path();
    QTRY_VERIFY_WITH_TIMEOUT(m_session->path() != first, 4000);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!prop("slideshowRunning").toBool());
    QTRY_VERIFY(!prop("fullScreen").toBool());
    QVERIFY(m_window->isVisible());
  }

  void shuffledSlideshowFindsTheOnlyPictureWithoutBouncingBetweenVideos() {
    const QString folder = m_scratch.filePath(QStringLiteral("slideshow-shuffle"));
    QVERIFY(QDir().mkpath(folder));
    const QString first = folder + QStringLiteral("/a.mp4");
    const QString second = folder + QStringLiteral("/b.mp4");
    const QString picture = folder + QStringLiteral("/only.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), first));
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), second));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), picture));

    const bool videos = m_settings->slideshowVideos();
    const bool shuffle = m_settings->slideshowShuffle();
    const auto restoreSettings = qScopeGuard([this, videos, shuffle] {
      m_settings->setSlideshowVideos(videos);
      m_settings->setSlideshowShuffle(shuffle);
    });
    m_settings->setSlideshowVideos(false);
    m_settings->setSlideshowShuffle(true);

    // The old retry loop chooses index 1 forever with this value, bouncing
    // between the videos and never visiting the picture at index 2.
    QJSValue math = m_engine->globalObject().property(QStringLiteral("Math"));
    const QJSValue random = math.property(QStringLiteral("random"));
    const auto restoreRandom = qScopeGuard([math, random]() mutable {
      math.setProperty(QStringLiteral("random"), random);
    });
    math.setProperty(QStringLiteral("random"),
                     m_engine->evaluate(QStringLiteral("(function () { return 0; })")));

    open({first, second, picture});
    QCOMPARE(m_session->count(), 3);
    QCOMPARE(m_session->index(), 0);
    QVERIFY(m_session->isVideo());
    QVERIFY(QMetaObject::invokeMethod(m_window, "setSlideshow", Q_ARG(QVariant, true)));
    QCOMPARE(m_session->path(), picture);
    QVERIFY(prop("slideshowRunning").toBool());
    QVERIFY(prop("status").toString() != QStringLiteral("No pictures here for a slideshow"));
    QVERIFY(QMetaObject::invokeMethod(m_window, "advanceSlideshow"));
    QCOMPARE(m_session->path(), picture);
    QVERIFY(prop("slideshowRunning").toBool());
    QVERIFY(prop("status").toString() != QStringLiteral("No pictures here for a slideshow"));
    QMetaObject::invokeMethod(m_window, "setSlideshow", Q_ARG(QVariant, false));
  }

  void orderedSlideshowStillStepsToTheOnlyPictureAmongVideos() {
    const QString folder = m_scratch.filePath(QStringLiteral("slideshow-ordered"));
    QVERIFY(QDir().mkpath(folder));
    const QString first = folder + QStringLiteral("/a.mp4");
    const QString picture = folder + QStringLiteral("/b.jpg");
    const QString second = folder + QStringLiteral("/c.mp4");
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), first));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), picture));
    QVERIFY(QFile::copy(media(QStringLiteral("clip.mp4")), second));

    const bool videos = m_settings->slideshowVideos();
    const bool shuffle = m_settings->slideshowShuffle();
    const auto restoreSettings = qScopeGuard([this, videos, shuffle] {
      m_settings->setSlideshowVideos(videos);
      m_settings->setSlideshowShuffle(shuffle);
    });
    m_settings->setSlideshowVideos(false);
    m_settings->setSlideshowShuffle(false);

    open({first, picture, second});
    QCOMPARE(m_session->count(), 3);
    QVERIFY(QMetaObject::invokeMethod(m_window, "setSlideshow", Q_ARG(QVariant, true)));
    QCOMPARE(m_session->path(), picture);
    QVERIFY(prop("slideshowRunning").toBool());
    QMetaObject::invokeMethod(m_window, "setSlideshow", Q_ARG(QVariant, false));
  }

  void trashAsksFirstThenMovesOn() {
    const QString folder = m_scratch.filePath(QStringLiteral("trash"));
    QVERIFY(QDir().mkpath(folder));
    const QString first = folder + QStringLiteral("/a.jpg");
    const QString second = folder + QStringLiteral("/b.jpg");
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), first));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), second));
    open({first});
    QTRY_COMPARE(m_session->count(), 2);

    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    QTest::keyClick(m_window, Qt::Key_Delete);
    QVERIFY(confirm->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(!confirm->isVisible());
    QVERIFY(QFile::exists(first));
    QVERIFY(m_window->isVisible());

    QTest::keyClick(m_window, Qt::Key_Delete);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(first));
    QCOMPARE(m_session->path(), second);
    QCOMPARE(m_session->count(), 1);

    // The last file gone, there is nothing left to show.
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(second));
    QTRY_VERIFY(!m_window->isVisible());
  }

  // The prompt is bound to the file it was opened on. A file swapped in under
  // the same name, or the symlink pointed at another file, is not trashed.
  void trashRefusesAFileReplacedWhileThePromptIsOpen_data() {
    QTest::addColumn<bool>("symlink");
    QTest::newRow("replaced") << false;
    QTest::newRow("symlink-retargeted") << true;
  }

  void trashRefusesAFileReplacedWhileThePromptIsOpen() {
    QFETCH(bool, symlink);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("elsewhere"))));
    const QString name = dir.filePath(QStringLiteral("review ü.jpg"));
    const QString other = dir.filePath(QStringLiteral("z.jpg"));
    const QString first = dir.filePath(QStringLiteral("elsewhere/first.jpg"));
    const QString second = dir.filePath(QStringLiteral("elsewhere/second.jpg"));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), symlink ? first : name));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), symlink ? second : dir.filePath(QStringLiteral("elsewhere/incoming.jpg"))));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 10.jpg")), other));
    if (symlink) {
      QVERIFY(QFile::link(first, name));
      const OpenRequest request = OpenRequest::fromPaths({name});
      open(request.files);
      m_session->setDeletionPaths(request.entryPaths);
      QTRY_VERIFY(m_session->sequenceRevision() >= 2);
    } else {
      open({name});
    }
    QTRY_COMPARE(m_session->count(), 2);
    QCOMPARE(m_session->deletionPath(), name);
    const QDir trash(m_scratch.filePath(QStringLiteral("data/Trash/files")));
    const QStringList trashedBefore = trash.entryList(QDir::Files | QDir::System);

    QTest::keyClick(m_window, Qt::Key_Delete);
    QQuickItem* confirm = item(QStringLiteral("viewerConfirm"));
    QTRY_VERIFY(confirm->isVisible());
    const QString preserved = dir.filePath(QStringLiteral("original-preserved.jpg"));
    if (symlink) {
      QVERIFY(QFile::remove(name));
      QVERIFY(QFile::link(second, name));
    } else {
      QVERIFY(QFile::rename(name, preserved));
      QVERIFY(QFile::rename(dir.filePath(QStringLiteral("elsewhere/incoming.jpg")), name));
    }
    QSignalSpy failed(m_actions, &ActionLauncher::failed);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!confirm->isVisible());
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("changed or disappeared")));
    QCOMPARE(trash.entryList(QDir::Files | QDir::System), trashedBefore);
    if (symlink) {
      QVERIFY(QFileInfo(name).isSymLink());
      QCOMPARE(QFileInfo(name).symLinkTarget(), second);
      QVERIFY(QFile::exists(first));
    } else {
      QVERIFY(QFile::exists(preserved));
      QFile replacement(name), expected(media(QStringLiteral("shot 2.jpg")));
      QVERIFY(replacement.open(QIODevice::ReadOnly) && expected.open(QIODevice::ReadOnly));
      QCOMPARE(replacement.readAll(), expected.readAll());
    }
    QVERIFY(QFile::exists(second) || !symlink);
    // Nothing moved, so the viewer stays put and keeps stepping normally.
    QVERIFY(m_window->isVisible());
    QVERIFY(!m_session->path().isEmpty());
    QVERIFY(QFileInfo::exists(m_session->path()));
    QVERIFY(!prop("undoId").toInt());
    const QString before = m_session->path();
    QTRY_VERIFY(m_session->count() >= 2);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_VERIFY(m_session->path() != before);
    QVERIFY(QFileInfo::exists(m_session->path()));
  }

  // Undo puts the file back on disk and back in the viewer where it was.
  void trashUndoPutsTheFileBackAtItsPlace_data() {
    QTest::addColumn<bool>("selection");
    QTest::addColumn<bool>("button");
    QTest::newRow("folder-ctrl-z") << false << false;
    QTest::newRow("folder-toast-undo") << false << true;
    QTest::newRow("selection-ctrl-z") << true << false;
    QTest::newRow("selection-toast-undo") << true << true;
  }

  void trashUndoPutsTheFileBackAtItsPlace() {
    QFETCH(bool, selection);
    QFETCH(bool, button);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QStringList files{dir.filePath(QStringLiteral("a.jpg")), dir.filePath(QStringLiteral("b.jpg")),
                            dir.filePath(QStringLiteral("c.jpg"))};
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), files.at(0)));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), files.at(1)));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 10.jpg")), files.at(2)));
    const QString middle = files.at(1);
    struct stat before {};
    QVERIFY(::lstat(QFile::encodeName(middle).constData(), &before) == 0);
    if (selection) open(files);
    else open({files.at(0)});
    QTRY_COMPARE(m_session->count(), 3);
    QVERIFY(m_session->jump(1));
    QCOMPARE(m_session->path(), middle);
    const bool favourite = m_settings->isFavorite(files.at(2));
    const auto restoreFavourite = qScopeGuard([&] { m_settings->setFavorite({files.at(2)}, favourite); });

    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(item(QStringLiteral("viewerConfirm"))->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(middle));
    QTRY_COMPARE(m_session->path(), files.at(2));
    QCOMPARE(m_session->count(), 2);
    QCOMPARE(prop("status").toString(), QStringLiteral("Moved to Trash"));
    QQuickItem* undo = item(QStringLiteral("viewerUndoButton"));
    QTRY_VERIFY(undo->isVisible());

    if (button) {
      click(undo);
    } else {
      // Ctrl+Z takes back whatever happened last, so a favourite set after
      // the move comes off first, and the Trash's Undo offer goes with it.
      QTest::keyClick(m_window, Qt::Key_V);
      QTRY_VERIFY(m_settings->isFavorite(files.at(2)) != favourite);
      QTRY_VERIFY(!undo->isVisible());
      QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
      QTRY_COMPARE(m_settings->isFavorite(files.at(2)), favourite);
      QVERIFY(!QFile::exists(middle));
      QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    }
    QTRY_VERIFY(QFile::exists(middle));
    struct stat after {};
    QVERIFY(::lstat(QFile::encodeName(middle).constData(), &after) == 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QTRY_COMPARE(m_session->path(), middle);
    QCOMPARE(m_session->index(), 1);
    QTRY_COMPARE(m_session->count(), 3);
    QCOMPARE(m_session->sequence(), files);
    QCOMPARE(prop("status").toString(), QStringLiteral("Restored b.jpg"));
    QVERIFY(!undo->isVisible());
    // The listing that follows keeps it in its place.
    QTest::qWait(200);
    QCOMPARE(m_session->sequence(), files);
    QCOMPARE(m_session->path(), middle);
  }

  // A window that has moved on to other files gets the file back on disk
  // only, not slipped into an unrelated selection.
  void trashUndoLeavesALaterSelectionAlone() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QStringList files;
    for (const QString& name : {QStringLiteral("a.jpg"), QStringLiteral("b.jpg"), QStringLiteral("x.jpg"),
                                QStringLiteral("y.jpg")}) {
      files.append(dir.filePath(name));
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), files.last()));
    }
    open({files.at(0), files.at(1)});
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(item(QStringLiteral("viewerConfirm"))->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(files.at(0)));
    open({files.at(2), files.at(3)});
    QTRY_COMPARE(m_session->path(), files.at(2));
    QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(QFile::exists(files.at(0)));
    QCOMPARE(m_session->sequence(), QStringList({files.at(2), files.at(3)}));
    QCOMPARE(m_session->path(), files.at(2));
  }

  // Ctrl+Z after the original name was taken leaves both files alone.
  void trashUndoRefusesWhenTheNameIsTaken() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.jpg"));
    const QString second = dir.filePath(QStringLiteral("b.jpg"));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), first));
    QVERIFY(QFile::copy(media(QStringLiteral("shot 2.jpg")), second));
    open({first});
    QTRY_COMPARE(m_session->count(), 2);
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(item(QStringLiteral("viewerConfirm"))->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(first));
    QTRY_COMPARE(m_session->path(), second);
    QVERIFY(QFile::copy(media(QStringLiteral("shot 10.jpg")), first));
    QTRY_COMPARE(m_session->count(), 2);
    const QString current = m_session->path();
    QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(prop("status").toString(),
                 QStringLiteral("Another file is already named a.jpg. The trashed one is still in the Trash"));
    QFile kept(first), expected(media(QStringLiteral("shot 10.jpg")));
    QVERIFY(kept.open(QIODevice::ReadOnly) && expected.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), expected.readAll());
    QCOMPARE(m_session->path(), current);
    QCOMPARE(m_session->count(), 2);
  }

  void permanentDeleteConfirmsAndOptOutDoesNotRepeatOrChangeTrash() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.jpg"));
    const QString second = dir.filePath(QStringLiteral("b.jpg"));
    const QString third = dir.filePath(QStringLiteral("c.jpg"));
    for (const auto& path : {first, second, third})
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), path));
    m_settings->setConfirmPermanentDelete(true);
    const auto restore = qScopeGuard([&] { m_settings->setConfirmPermanentDelete(true); });
    open({first});
    QTRY_COMPARE(m_session->count(), 3);
    QQuickItem* confirm = item(QStringLiteral("viewerPermanentConfirm"));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(confirm->isVisible());
    QCOMPARE(confirm->property("path").toString(), first);
    QVERIFY(confirm->property("detail").toString().contains(QStringLiteral("cannot be undone")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(QFile::exists(first));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(first));
    QTRY_COMPARE(m_session->path(), second);

    m_settings->setConfirmPermanentDelete(false);
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Delete, Qt::ShiftModifier, QString(), true);
    QCoreApplication::sendEvent(m_window, &repeat);
    QVERIFY(QFile::exists(second));
    QVERIFY(!confirm->isVisible());
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(item(QStringLiteral("viewerConfirm"))->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(QFile::exists(second));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(!QFile::exists(second));
    QVERIFY(!confirm->isVisible());
    QTRY_COMPARE(m_session->path(), third);
    QVERIFY(QFile::exists(third));
  }

  void permanentDeleteOfAnExplicitAliasKeepsItsTargetAfterFolderListing() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("original"))));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("links"))));
    const QString target = dir.filePath(QStringLiteral("original/same.jpg"));
    const QString alias = dir.filePath(QStringLiteral("links/same.jpg"));
    const QString sibling = dir.filePath(QStringLiteral("original/other.jpg"));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), target));
    QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), sibling));
    QVERIFY(QFile::link(target, alias));
    const OpenRequest request = OpenRequest::fromPaths({alias});
    QCOMPARE(request.files, QStringList{target});
    QCOMPARE(request.entryPaths.value(target), alias);
    open(request.files);
    m_session->setDeletionPaths(request.entryPaths);
    QTRY_COMPARE(m_session->count(), 2);
    QCOMPARE(m_session->deletionPath(), alias);
    m_settings->setConfirmPermanentDelete(false);
    const auto restore = qScopeGuard([&] { m_settings->setConfirmPermanentDelete(true); });
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(!QFileInfo(alias).isSymLink());
    QVERIFY(QFile::exists(target));
    QTRY_COMPARE(m_session->path(), sibling);
  }

  void reopeningOneSelectionAliasPreservesTheOtherAndLibraryHandoff() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("original"))));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("links"))));
    QStringList targets, aliases;
    for (const auto& name : {QStringLiteral("a.jpg"), QStringLiteral("b.jpg")}) {
      const QString target = dir.filePath(QStringLiteral("original/") + name);
      const QString alias = dir.filePath(QStringLiteral("links/") + name);
      QVERIFY(QFile::copy(media(QStringLiteral("Shot 1.jpg")), target));
      QVERIFY(QFile::link(target, alias));
      targets.append(target);
      aliases.append(alias);
    }
    const OpenRequest request = OpenRequest::fromPaths(aliases);
    ViewerWindows viewers(*m_engine);
    viewers.setPlacementQuery([](QObject*, HyprlandPlacement::Reply reply) { reply({}); });
    QQuickWindow* window = viewers.open(request.files, request.entryPaths);
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QCOMPARE(viewers.open({targets.first()}, {{targets.first(), aliases.first()}}), window);
    ViewerSession* session = viewers.sessionOf(window);
    QVERIFY(session->step(1));
    QCOMPARE(session->deletionPath(), aliases.last());
    QSignalSpy library(&viewers, &ViewerWindows::libraryRequested);
    session->openInLibrary();
    QCOMPARE(library.size(), 1);
    const OpenRequest handoff = OpenRequest::fromPaths({library.first().at(0).toString()});
    QCOMPARE(handoff.files, QStringList{targets.last()});
    QCOMPARE(handoff.entryPaths.value(targets.last()), aliases.last());
    m_settings->setConfirmPermanentDelete(false);
    const auto restore = qScopeGuard([&] { m_settings->setConfirmPermanentDelete(true); });
    QTest::keyClick(window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(!QFileInfo(aliases.last()).isSymLink());
    QVERIFY(QFile::exists(targets.last()));
    QVERIFY(QFileInfo(aliases.first()).isSymLink());
    window->close();
  }

  // A forwarded request's activation token belongs to the viewer it opens.
  // Placement can hold that viewer back while later requests arrive, so each
  // one is shown with its own token, and none is left in the environment.
  void aViewerWaitingForPlacementKeepsItsOwnActivationToken() {
    const QByteArray saved = qgetenv("XDG_ACTIVATION_TOKEN");
    const auto restore = qScopeGuard([&] {
      saved.isNull() ? qunsetenv("XDG_ACTIVATION_TOKEN") : qputenv("XDG_ACTIVATION_TOKEN", saved);
    });
    ViewerWindows viewers(*m_engine);
    viewers.setPlacementDeadline(30000);
    QList<HyprlandPlacement::Reply> pending;
    viewers.setPlacementQuery([&](QObject*, HyprlandPlacement::Reply reply) {
      pending.append(std::move(reply));
    });
    viewers.setPlacementTile([](const QStringList&) {});
    qunsetenv("XDG_ACTIVATION_TOKEN");
    auto* first = viewers.open({media(QStringLiteral("Shot 1.jpg"))});
    QVERIFY(first && first->isVisible());
    // As SingleInstance does: the token only while the request is handled.
    const auto request = [&](const QByteArray& token, const QString& name) {
      qputenv("XDG_ACTIVATION_TOKEN", token);
      QQuickWindow* window = viewers.open({media(name)});
      qunsetenv("XDG_ACTIVATION_TOKEN");
      return window;
    };
    auto* second = request("second-token", QStringLiteral("shot 2.jpg"));
    auto* third = request("third-token", QStringLiteral("shot 10.jpg"));
    QVERIFY(second && !second->isVisible());
    QVERIFY(third && !third->isVisible());
    QCOMPARE(pending.size(), 2);
    QByteArray secondSaw, thirdSaw;
    connect(second, &QWindow::visibleChanged, this,
            [&] { secondSaw = qgetenv("XDG_ACTIVATION_TOKEN"); });
    connect(third, &QWindow::visibleChanged, this,
            [&] { thirdSaw = qgetenv("XDG_ACTIVATION_TOKEN"); });
    HyprlandPlacement::Plan plan;
    plan.viewers = 1;
    plan.floating = {QStringLiteral("0xabc")};
    pending.takeLast()(plan);
    QVERIFY(third->isVisible());
    QCOMPARE(thirdSaw, QByteArray("third-token"));
    QVERIFY(qEnvironmentVariableIsEmpty("XDG_ACTIVATION_TOKEN"));
    pending.takeFirst()(plan);
    QVERIFY(second->isVisible());
    QCOMPARE(secondSaw, QByteArray("second-token"));
    QVERIFY(qEnvironmentVariableIsEmpty("XDG_ACTIVATION_TOKEN"));
    third->close();
    second->close();
    first->close();
  }

  // Each file opened from outside gets a viewer of its own. Asking again for
  // what one already shows brings that one forward, a selection stays one
  // viewer, and closing leaves the rest alone with one closed viewer kept.
  // The new viewer stays hidden until its map-time title is decided.
  void placementRepliesKeepOpensResponsiveAndRejectClosedWindows() {
    ViewerWindows viewers(*m_engine);
    // A held reply must decide this viewer's mapping, never the placement
    // deadline. The deadline has its own fallback test, and a loaded machine
    // must not slip these opens into it before the reply arrives.
    viewers.setPlacementDeadline(30000);
    QList<HyprlandPlacement::Reply> pending;
    viewers.setPlacementQuery([&](QObject*, HyprlandPlacement::Reply reply) {
      pending.append(std::move(reply));
    });
    QStringList tiled;
    viewers.setPlacementTile([&](const QStringList& addresses) { tiled += addresses; });
    auto* first = viewers.open({media(QStringLiteral("Shot 1.jpg"))});
    QVERIFY(first);
    QElapsedTimer timer; timer.start();
    auto* second = viewers.open({media(QStringLiteral("shot 2.jpg"))});
    QVERIFY(second && !second->isVisible());
    QCOMPARE(pending.size(), 1);
    // A held compositor reply must not block event delivery.
    bool heartbeat = false;
    QTimer::singleShot(5, this, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    QVERIFY(!second->isVisible());
    HyprlandPlacement::Plan plan;
    plan.viewers = 1;
    plan.floating = {QStringLiteral("0xabc")};
    pending.takeFirst()(plan);
    qInfo() << "PERF second viewer mapped ms" << timer.nsecsElapsed() / 1e6;
    QVERIFY(second->isVisible());
    QVERIFY(second->property("mapTiled").toBool());
    QCOMPARE(second->title(), QStringLiteral("Omaroll"));
    QCOMPARE(tiled, plan.floating);
    QTRY_VERIFY(!second->property("mapTiled").toBool());
    QCOMPARE(second->title(), QStringLiteral("shot 2.jpg · Omaroll"));

    auto* third = viewers.open({media(QStringLiteral("shot 10.jpg"))});
    QVERIFY(third && !third->isVisible());
    // Closing another viewer neither cancels this request nor schedules tiles.
    first->close();
    QCoreApplication::processEvents();
    QCOMPARE(pending.size(), 1);
    pending.takeFirst()(plan);
    QVERIFY(third->isVisible() && third->property("mapTiled").toBool());
    QCOMPARE(tiled.size(), 2);
    second->close();
    QTest::qWait(160);
    QCOMPARE(pending.size(), 0);
    QCOMPARE(tiled.size(), 2);

    // This open deliberately reaches the placement deadline instead of a
    // reply, so bring the fallback back within reach.
    viewers.setPlacementDeadline(100);
    auto* fourth = viewers.open({media(QStringLiteral("Shot 1.jpg"))});
    QVERIFY(fourth && !fourth->isVisible());
    QTRY_VERIFY(fourth->isVisible());
    fourth->close();
    pending.takeFirst()(plan);
    QVERIFY(!fourth->isVisible());
    QCOMPARE(tiled.size(), 2);
    third->close();
  }

  void placementFailureAndWorkspaceSwitchHaveBoundedFallbacks() {
    ViewerWindows viewers(*m_engine);
    QList<HyprlandPlacement::Reply> pending;
    bool settleRetry = false;
    viewers.setPlacementQuery([&](QObject*, HyprlandPlacement::Reply reply) {
      if (settleRetry) {
        settleRetry = false;
        HyprlandPlacement::Plan settled;
        settled.viewers = 1;
        reply(settled);
      } else {
        pending.append(std::move(reply));
      }
    });
    viewers.setPlacementTile([](const QStringList&) {});
    auto* first = viewers.open({media(QStringLiteral("Shot 1.jpg"))});
    auto* second = viewers.open({media(QStringLiteral("shot 2.jpg"))});
    QVERIFY(first && second && !second->isVisible());
    bool mappedTiled = false;
    connect(second, &QWindow::visibleChanged, this, [second, &mappedTiled](bool visible) {
      if (visible) mappedTiled = second->property("mapTiled").toBool();
    }, Qt::SingleShotConnection);
    HyprlandPlacement::Plan switched;
    switched.retry = true;
    settleRetry = true;
    pending.takeFirst()(switched);
    QTRY_VERIFY(second->isVisible());
    QVERIFY(mappedTiled);
    QVERIFY(!settleRetry && pending.isEmpty());
    HyprlandPlacement::Plan settled;
    settled.viewers = 1;
    auto* third = viewers.open({media(QStringLiteral("shot 10.jpg"))});
    QVERIFY(third && !third->isVisible());
    pending.takeFirst()({});
    QVERIFY(third->isVisible());
    QVERIFY(!third->property("mapTiled").toBool());
    third->close();
    QCoreApplication::processEvents();
    QElapsedTimer timer; timer.start();
    auto* timedOut = viewers.open({media(QStringLiteral("shot 10.jpg"))});
    QVERIFY(timedOut && !timedOut->isVisible());
    connect(timedOut, &QWindow::visibleChanged, this, [&timer](bool visible) {
      if (visible) qInfo() << "PERF placement timeout mapped ms" << timer.nsecsElapsed() / 1e6;
    }, Qt::SingleShotConnection);
    QTRY_VERIFY(timedOut->isVisible());
    QVERIFY(!timedOut->property("mapTiled").toBool());
    pending.takeFirst()(settled);
    QVERIFY(!timedOut->property("mapTiled").toBool());
    first->close(); second->close(); timedOut->close();
  }

  void placementWaitsForAValidEmptySnapshotToSettle() {
    ViewerWindows viewers(*m_engine);
    QList<HyprlandPlacement::Reply> pending;
    int queries = 0;
    viewers.setPlacementQuery([&](QObject*, HyprlandPlacement::Reply reply) {
      if (++queries == 2) {
        HyprlandPlacement::Plan mapped;
        mapped.valid = true;
        mapped.viewers = 1;
        reply(mapped);
      } else {
        pending.append(std::move(reply));
      }
    });
    viewers.setPlacementTile([](const QStringList&) {});
    auto* first = viewers.open({media(QStringLiteral("Shot 1.jpg"))});
    auto* second = viewers.open({media(QStringLiteral("shot 2.jpg"))});
    QVERIFY(first && second && !second->isVisible());
    bool mappedTiled = false;
    connect(second, &QWindow::visibleChanged, this, [second, &mappedTiled](bool visible) {
      if (visible) mappedTiled = second->property("mapTiled").toBool();
    }, Qt::SingleShotConnection);
    HyprlandPlacement::Plan notMapped;
    notMapped.valid = true;
    pending.takeFirst()(notMapped);
    QVERIFY(!second->isVisible());
    QTRY_VERIFY(second->isVisible());
    QVERIFY(mappedTiled);
    QCOMPARE(queries, 2);
    QVERIFY(pending.isEmpty());
    first->close(); second->close();
  }

  // Two opens close together can both decide before either viewer maps.
  // Once the second is up, a fresh look tiles the pair.
  void placementTilesRacingViewersOnceBothHaveMapped() {
    ViewerWindows viewers(*m_engine);
    int queries = 0;
    viewers.setPlacementQuery([&](QObject*, HyprlandPlacement::Reply reply) {
      HyprlandPlacement::Plan plan;
      plan.valid = true;
      if (++queries == 3) {
        plan.viewers = 2;
        plan.floating = {QStringLiteral("0xa"), QStringLiteral("0xb")};
      }
      reply(plan);
    });
    QStringList tiled;
    viewers.setPlacementTile([&](const QStringList& addresses) { tiled += addresses; });
    auto* first = viewers.open({media(QStringLiteral("Shot 1.jpg"))});
    auto* second = viewers.open({media(QStringLiteral("shot 2.jpg"))});
    QVERIFY(first && second);
    QTRY_VERIFY(second->isVisible());
    QVERIFY(!second->property("mapTiled").toBool());
    QCOMPARE(queries, 2);
    QTRY_COMPARE(tiled, (QStringList{QStringLiteral("0xa"), QStringLiteral("0xb")}));
    QCOMPARE(queries, 3);
    first->close(); second->close();
  }

  void severalViewersOpenSideBySideAndCloseCleanly() {
    ViewerWindows viewers(*m_engine);
    int queries = 0;
    viewers.setPlacementQuery([&queries](QObject*, HyprlandPlacement::Reply reply) {
      ++queries;
      HyprlandPlacement::Plan plan;
      plan.viewers = 2;
      reply(plan);
    });
    const QString first = media(QStringLiteral("Shot 1.jpg"));
    const QString second = media(QStringLiteral("shot 2.jpg"));
    QQuickWindow* a = viewers.open({first});
    QVERIFY(a);
    QCOMPARE(queries, 0);
    QCOMPARE(a->title(), QStringLiteral("Shot 1.jpg · Omaroll"));
    QQuickWindow* b = viewers.open({second});
    QVERIFY(b && a != b);
    QCOMPARE(b->title(), QStringLiteral("Omaroll"));
    QVERIFY(QTest::qWaitForWindowExposed(a));
    QVERIFY(QTest::qWaitForWindowExposed(b));
    QTRY_COMPARE(b->title(), QStringLiteral("shot 2.jpg · Omaroll"));
    // The query decides the title before mapping.
    QTRY_COMPARE(queries, 1);
    QCOMPARE(viewers.visibleWindows(), (QList<QQuickWindow*>{a, b}));
    QCOMPARE(viewers.sessionOf(a)->path(), first);
    QCOMPARE(viewers.sessionOf(b)->path(), second);

    // Each steps through its folder on its own.
    QTRY_COMPARE(viewers.sessionOf(b)->count(), 4);
    QVERIFY(viewers.sessionOf(b)->step(1));
    QCOMPARE(viewers.sessionOf(a)->path(), first);
    QCOMPARE(viewers.sessionOf(b)->path(), media(QStringLiteral("shot 10.jpg")));

    QCOMPARE(viewers.frontmost(), b);
    QVERIFY(b->property("frontmost").toBool());
    QVERIFY(!a->property("frontmost").toBool());
    QCOMPARE(viewers.open({first}), a);
    QCOMPARE(viewers.windowCount(), 2);
    QCOMPARE(viewers.frontmost(), a);
    QVERIFY(!b->property("frontmost").toBool());

    // Several files at once are one viewer, found again the same way.
    QQuickWindow* c = viewers.open({first, second});
    QVERIFY(c && c != a && c != b);
    QCOMPARE(viewers.sessionOf(c)->count(), 2);
    QCOMPARE(viewers.open({first, second}), c);
    QCOMPARE(viewers.windowCount(), 3);

    // A message that belongs to no viewer appears once, in the one used last.
    emit m_actions->failed(QStringLiteral("Could not open that"));
    QCOMPARE(c->property("status").toString(), QStringLiteral("Could not open that"));
    QVERIFY(a->property("status").toString().isEmpty());
    QVERIFY(b->property("status").toString().isEmpty());

    // The handoff to the library names the viewer it came from.
    QSignalSpy library(&viewers, &ViewerWindows::libraryRequested);
    viewers.sessionOf(b)->openInLibrary();
    QCOMPARE(library.size(), 1);
    QCOMPARE(library.first().at(0).toString(), media(QStringLiteral("shot 10.jpg")));
    QCOMPARE(library.first().at(1).value<QQuickWindow*>(), b);

    // Closing two leaves the first, keeps one closed viewer and frees the other.
    b->close();
    c->close();
    QTRY_COMPARE(viewers.windowCount(), 2);
    QCOMPARE(viewers.visibleWindows(), QList<QQuickWindow*>{a});
    QCOMPARE(viewers.frontmost(), a);
    QVERIFY(a->property("frontmost").toBool());

    // The kept one is reused, with nothing of its last file left behind.
    QQuickWindow* d = viewers.open({media(QStringLiteral("shot 10.jpg"))});
    QVERIFY(d && d != a);
    QCOMPARE(viewers.windowCount(), 2);
    QTRY_COMPARE(viewers.sessionOf(d)->count(), 4);
    QTRY_COMPARE(d->title(), QStringLiteral("shot 10.jpg · Omaroll"));

    a->close();
    d->close();
    QTRY_COMPARE(viewers.windowCount(), 1);
    QCOMPARE(viewers.frontmost(), nullptr);
    QVERIFY(viewers.visibleWindows().isEmpty());
  }

  // Of the viewers showing a video, only the one used last is heard; the
  // others keep playing silently and the saved mute setting is untouched.
  // A picture in front leaves the sound where it was.
  void onlyTheVideoUsedLastIsHeard() {
    ViewerWindows viewers(*m_engine);
    viewers.setPlacementQuery([](QObject*, HyprlandPlacement::Reply reply) { reply({}); });
    const QString clip = media(QStringLiteral("clip.mp4"));
    const QString picture = media(QStringLiteral("Shot 1.jpg"));
    QQuickWindow* first = viewers.open({clip});
    QQuickWindow* second = viewers.open({clip, picture});
    QVERIFY(first && second && first != second);
    QMediaPlayer* a = nullptr;
    QMediaPlayer* b = nullptr;
    QTRY_VERIFY((a = first->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY((b = second->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY(second->property("audible").toBool());
    QTRY_VERIFY(!first->property("audible").toBool());
    QTRY_VERIFY(a->duration() > 0 && b->duration() > 0);
    a->pause();
    b->pause();

    QCOMPARE(viewers.open({clip}), first);
    QVERIFY(first->property("audible").toBool());
    QVERIFY(!second->property("audible").toBool());
    QCOMPARE(m_mpris->position(), qlonglong(a->position()) * 1000);
    const qint64 silentPosition = b->position();
    m_mpris->seek(1000000);
    QTRY_VERIFY(a->position() >= 900);
    QCOMPARE(b->position(), silentPosition);

    QQuickWindow* still = viewers.open({media(QStringLiteral("shot 2.jpg"))});
    QCOMPARE(viewers.frontmost(), still);
    QVERIFY(first->property("audible").toBool());
    QVERIFY(!second->property("audible").toBool());

    // Stepping the heard viewer off its video hands the sound on.
    QTRY_COMPARE(viewers.sessionOf(first)->count(), 4);
    QVERIFY(viewers.sessionOf(first)->step(1));
    QVERIFY(!viewers.sessionOf(first)->isVideo());
    QTRY_VERIFY(second->property("audible").toBool());
    QCOMPARE(m_mpris->position(), qlonglong(b->position()) * 1000);
    m_mpris->play();
    QTRY_COMPARE(b->playbackState(), QMediaPlayer::PlayingState);
    QQuickWindow* third = viewers.open({clip, media(QStringLiteral("shot 2.jpg"))});
    QVERIFY(third && third != second);
    QTRY_VERIFY(!second->property("audible").toBool());
    third->close();
    QTRY_VERIFY(second->property("audible").toBool());
    QCOMPARE(m_mpris->playbackStatus(), QStringLiteral("Playing"));
    QVERIFY(!m_settings->videoMuted());

    // The library uses the same ownership policy as the quick viewers.
    QMediaPlayer libraryPlayer;
    QObject libraryController;
    libraryController.setProperty("audible", false);
    libraryPlayer.setSource(QUrl::fromLocalFile(clip));
    m_mpris->track(&libraryPlayer, &libraryController);
    QVERIFY(libraryController.property("audible").toBool());
    QVERIFY(!second->property("audible").toBool());
    QCOMPARE(viewers.open({clip, picture}), second);
    QVERIFY(second->property("audible").toBool());
    QVERIFY(!libraryController.property("audible").toBool());
    m_mpris->track(&libraryPlayer, &libraryController);
    m_mpris->release(&libraryPlayer);
    QVERIFY(second->property("audible").toBool());
    QCOMPARE(m_mpris->playbackStatus(), QStringLiteral("Playing"));

    for (QQuickWindow* window : viewers.visibleWindows()) {
      window->close();
    }
    QTRY_VERIFY(viewers.visibleWindows().isEmpty());
  }

  // A camera raw opens on the camera's embedded preview, measured as the raw
  // itself. Drawn larger than that preview, the raw is demosaiced and laid
  // over it. Tools that cannot open a raw decline with a reason.
  void aCameraRawOpensOnItsPreviewAndSharpensWhenEnlarged() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) {
      QSKIP("Qt has no camera raw decoder here (kimageformats with LibRaw)");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString camera = dir.filePath(QStringLiteral("DSC00041.DNG"));
    const QString bare = dir.filePath(QStringLiteral("DSC00042.DNG"));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/camera.dng"), camera));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/no-preview.dng"), bare));

    open({camera});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(m_session->isRaw());
    QVERIFY(prop("playbackError").toString().isEmpty());
    // The upright preview is 24x32; the raw, 48x64.
    QCOMPARE(prop("loadedSize").toSize(), QSize(24, 32));
    // The raw's own size comes from a probe that can finish after the preview.
    QTRY_COMPARE(prop("sourceWidth").toReal(), 48.0);
    QTRY_COMPARE(prop("sourceHeight").toReal(), 64.0);
    // Fitted to the window, the preview is far past its own pixels.
    QVERIFY(prop("detailWanted").toBool());
    QQuickItem* detail = item(QStringLiteral("viewerDetail"));
    QTRY_COMPARE(detail->property("status").toInt(), 1);
    QCOMPARE(detail->property("sourceSize").toSize(), QSize(48, 64));
    QCOMPARE(prop("detailPath").toString(), camera);

    QTest::keyClick(m_window, Qt::Key_A);
    QTRY_COMPARE(prop("status").toString(), QStringLiteral("That one cannot open camera raws"));

    // A raw without a preview gets the plugin's half-size decode, upright.
    QTRY_COMPARE(m_session->count(), 2);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(m_session->path(), bare);
    QTRY_VERIFY(prop("imageReady").toBool());
    QCOMPARE(prop("loadedSize").toSize(), QSize(24, 32));
    // As above, the raw's size comes from a probe that can finish after it shows.
    QTRY_COMPARE(prop("sourceWidth").toReal(), 48.0);
    QTRY_COMPARE(detail->property("status").toInt(), 1);
    m_window->close();
    m_session->clear();
  }

  // A picture too large to show whole opens from a copy the size of the
  // screen, measured as the picture itself. Zoomed past that copy, its full
  // resolution is loaded over it, and shown only while zoomed in.
  void aVeryLargePictureOpensFromAReducedCopy() {
    const auto restore = qScopeGuard([this] {
      ImageBudget::setLimits({});
      m_window->close();
      m_session->clear();
    });
    ImageBudget::setLimits({.displayBytes = 1 * ImageBudget::kMiB});
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A PNG cannot decode part of itself, so it loads full resolution whole.
    const QString scan = dir.filePath(QStringLiteral("scan.png"));
    const QString next = dir.filePath(QStringLiteral("scan-2.png"));
    QImage image(3000, 2000, QImage::Format_RGB32);
    image.fill(QColor(40, 90, 160));
    QVERIFY(image.save(scan));
    QVERIFY(image.save(next));

    open({scan});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(m_session->reduced());
    QCOMPARE(m_session->fullSize(), QSize(3000, 2000));
    QVERIFY(item(QStringLiteral("viewerImage"))->property("source").toUrl().toString()
                .startsWith(QStringLiteral("image://large/")));
    QCOMPARE(prop("loadedSize").toSize(), QSize(2048, 1365));
    QCOMPARE(prop("sourceWidth").toReal(), 3000.0);
    QCOMPARE(prop("sourceHeight").toReal(), 2000.0);
    QVERIFY(prop("playbackError").toString().isEmpty());
    // Fitted, the copy has pixels to spare.
    QVERIFY(!prop("detailWanted").toBool());
    QQuickItem* detail = item(QStringLiteral("viewerDetail"));
    QVERIFY(detail->property("source").toUrl().isEmpty());

    QVERIFY(!m_session->areas());
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(prop("detailWanted").toBool());
    QCOMPARE(detail->property("source").toUrl(), m_session->detailUrl());
    QCOMPARE(m_session->detailUrl().toLocalFile(), scan);
    QTRY_COMPARE(detail->property("status").toInt(), 1);
    QCOMPARE(detail->property("sourceSize").toSize(), QSize(3000, 2000));
    QTRY_COMPARE(detail->property("opacity").toReal(), 1.0);
    // Back to fit: the copy shows again, the decode is kept for next time.
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_VERIFY(!prop("detailWanted").toBool());
    QTRY_COMPARE(detail->property("opacity").toReal(), 0.0);
    QCOMPARE(prop("detailPath").toString(), scan);

    // A neighbour's header was read with the preloads, so stepping onto it
    // does not wait for another read. Too large to preload, it is not.
    QTRY_COMPARE(m_session->count(), 2);
    QTest::qWait(500);
    QVERIFY(m_session->nextPreloadUrl().isEmpty());
    QVERIFY(m_session->step(1));
    QCOMPARE(m_session->path(), next);
    QVERIFY(m_session->reduced());
    QVERIFY(m_session->imageUrl().toString().startsWith(QStringLiteral("image://large/")));
    QTRY_VERIFY(prop("imageReady").toBool());
    QCOMPARE(prop("sourceWidth").toReal(), 3000.0);
  }

  // A very large JPEG zoomed in decodes only the part on screen, at the
  // screen's resolution, and decodes again where the view moves.
  void aVeryLargeJpegDecodesOnlyWhatIsOnScreen() {
    const auto restore = qScopeGuard([this] {
      ImageBudget::setLimits({});
      m_window->close();
      m_session->clear();
    });
    ImageBudget::setLimits({.displayBytes = 1 * ImageBudget::kMiB});
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString scan = dir.filePath(QStringLiteral("scan.jpg"));
    // Blue on the left, orange on the right.
    QImage image(6000, 4000, QImage::Format_RGB32);
    image.fill(QColor(40, 90, 160));
    QPainter(&image).fillRect(3000, 0, 3000, 4000, QColor(220, 130, 40));
    QVERIFY(image.save(scan, nullptr, 85));

    open({scan});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(m_session->reduced());
    QVERIFY(m_session->areas());
    QVERIFY(m_session->detailUrl().isEmpty());
    QVERIFY(m_session->pictureNote().isEmpty());
    QQuickItem* area = item(QStringLiteral("viewerArea"));
    QVERIFY(area);
    QVERIFY(!area->isVisible());
    QVERIFY(area->property("source").toUrl().isEmpty());

    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(prop("enlargedPastCopy").toBool());
    QVERIFY(!prop("detailMissing").toBool());
    QTRY_VERIFY(area->isVisible());
    const QString first = area->property("source").toUrl().toString();
    QVERIFY(first.startsWith(QStringLiteral("image://large/")));
    // No more pixels than the view, with its margin, needs.
    const qreal dpr = m_window->devicePixelRatio();
    const QSize loaded = area->property("sourceSize").toSize();
    QVERIFY2(loaded.width() <= qCeil(m_window->width() * dpr * 1.5) + 2
                 && loaded.height() <= qCeil(m_window->height() * dpr * 1.5) + 2,
             qPrintable(QStringLiteral("%1x%2").arg(loaded.width()).arg(loaded.height())));
    const QRectF shown = area->property("shown").toRectF();
    QVERIFY(shown.contains(QPointF(3000, 2000)));
    QVERIFY(shown.width() < 6000 && shown.height() < 4000);

    // Panned to the right edge: the orange half is decoded there.
    QObject* flick = m_window->findChild<QObject*>(QStringLiteral("viewerStill"));
    QVERIFY(flick);
    flick->setProperty("contentX", flick->property("contentWidth").toReal() - m_window->width());
    QTRY_VERIFY(area->property("source").toUrl().toString() != first);
    // The last area stays while the next one decodes.
    QVERIFY(area->isVisible());
    QTRY_VERIFY(area->property("shown").toRectF().right() >= 5999);
    QTRY_COMPARE(area->property("status").toInt(), 1);
    const QImage frame = m_window->grabWindow();
    const QColor color = frame.pixelColor(frame.width() * 3 / 4, frame.height() / 2);
    QVERIFY2(color.red() > 180 && color.blue() < 90, qPrintable(color.name()));

    // Back to fit: the area goes, the copy shows.
    QTest::keyClick(m_window, Qt::Key_0);
    QTRY_VERIFY(!area->isVisible());
    QTRY_VERIFY(area->property("source").toUrl().isEmpty());
  }

  // A photo larger than the screen opens from the screen's copy, as a very
  // large one does, and zooming in loads its full resolution.
  void aPhotoLargerThanTheScreenOpensFromACopy() {
    const auto restore = qScopeGuard([this] {
      m_window->close();
      m_session->clear();
    });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.filePath(QStringLiteral("photo.jpg"));
    QImage image(4000, 3000, QImage::Format_RGB32);
    image.fill(QColor(70, 130, 90));
    QVERIFY(image.save(photo, nullptr, 85));

    open({photo});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(m_session->reduced());
    QVERIFY(m_session->pictureNote().isEmpty());
    const int edge = ImageBudget::reducedEdge();
    QVERIFY(item(QStringLiteral("viewerImage"))->property("source").toUrl().toString()
                .startsWith(QStringLiteral("image://large/")));
    QCOMPARE(prop("loadedSize").toSize(), ImageBudget::reducedSize(ImageBudget::estimate(photo), edge));
    QCOMPARE(prop("sourceWidth").toReal(), 4000.0);
    QCOMPARE(prop("sourceHeight").toReal(), 3000.0);
    QVERIFY(!prop("detailWanted").toBool());

    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(prop("detailWanted").toBool());
    QQuickItem* detail = item(QStringLiteral("viewerDetail"));
    QTRY_COMPARE(detail->property("status").toInt(), 1);
    QCOMPARE(detail->property("sourceSize").toSize(), QSize(4000, 3000));
    QTRY_COMPARE(detail->property("opacity").toReal(), 1.0);
  }

  // Past Qt's limit, a JPEG still opens from its reduced copy and says why
  // zooming in stays soft. A format that has to decode whole says why it
  // cannot open at all.
  void aPictureOverTheLimitSaysHowMuchItNeeds() {
    const auto restore = qScopeGuard([this] {
      QImageReader::setAllocationLimit(256);
      m_window->close();
      m_session->clear();
    });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.filePath(QStringLiteral("photo.jpg"));
    const QString scan = dir.filePath(QStringLiteral("scan.png"));
    QImage image(3000, 2000, QImage::Format_RGB32);
    image.fill(QColor(160, 90, 40));
    QVERIFY(image.save(photo, nullptr, 85));
    QVERIFY(image.save(scan));
    QImageReader::setAllocationLimit(10);

    // A JPEG still opens, from a copy the limit allows, and zoomed in shows
    // the part on screen, which needs far less than the whole.
    open({photo});
    QTRY_VERIFY(prop("imageReady").toBool());
    QVERIFY(m_session->reduced());
    QVERIFY(m_session->areas());
    QVERIFY(m_session->pictureNote().isEmpty());
    QCOMPARE(prop("loadedSize").toSize(), QSize(1536, 1024));
    QCOMPARE(prop("sourceWidth").toReal(), 3000.0);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(item(QStringLiteral("viewerArea"))->isVisible());
    QVERIFY(!prop("detailMissing").toBool());

    m_session->open({scan});
    QTRY_COMPARE(m_session->path(), scan);
    QTRY_COMPARE(prop("playbackError").toString(),
                 QStringLiteral("This picture needs about 23 MiB; the limit on this computer is "
                                "10 MiB"));
  }

  void theWindowRaisedNoQmlWarnings() {
    QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
  }

private:
  static bool writeBitmapHeader(const QString& path, const QSize& size) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0x4d42) << quint32(54) << quint16(0) << quint16(0) << quint32(54)
           << quint32(40) << qint32(size.width()) << qint32(size.height()) << quint16(1)
           << quint16(32) << quint32(0) << quint32(0) << qint32(0) << qint32(0)
           << quint32(0) << quint32(0);
    return stream.status() == QDataStream::Ok;
  }

  // A viewer window of its own, as ViewerWindows makes for each launch, so
  // the player is created fresh rather than reused from earlier tests.
  struct FreshViewer {
    std::unique_ptr<ViewerSession> session;
    std::unique_ptr<QQmlContext> context;
    std::unique_ptr<QObject> root;
    QQuickWindow* window = nullptr;
    QQuickItem* item(const QString& name) const {
      return find(window->contentItem(), [&name](QQuickItem* candidate) {
        return candidate->objectName() == name;
      });
    }
  };

  FreshViewer freshViewer() {
    FreshViewer viewer;
    viewer.session = std::make_unique<ViewerSession>();
    viewer.context = std::make_unique<QQmlContext>(m_engine->rootContext());
    viewer.context->setContextProperty(QStringLiteral("Session"), viewer.session.get());
    viewer.context->setContextProperty(QStringLiteral("MediaInfo"), m_mediaInfo);
    viewer.context->setContextProperty(QStringLiteral("Subtitles"), m_subtitles);
    QQmlComponent component(m_engine);
    component.loadFromModule("Omaroll", "Viewer");
    viewer.root.reset(component.create(viewer.context.get()));
    viewer.window = qobject_cast<QQuickWindow*>(viewer.root.get());
    if (viewer.window) viewer.window->resize(640, 400);
    return viewer;
  }

  static bool encodeClip(const QString& path, int seconds, int gop) {
    QProcess encoder;
    encoder.start(QStringLiteral("ffmpeg"),
                  {"-v", "error", "-f", "lavfi", "-i",
                   QStringLiteral("testsrc2=s=320x180:r=30:d=%1").arg(seconds), "-f", "lavfi",
                   "-i", QStringLiteral("sine=frequency=440:duration=%1").arg(seconds), "-c:v",
                   "libx264", "-preset", "ultrafast", "-g", QString::number(gop),
                   "-keyint_min", QString::number(gop), "-sc_threshold", "0", "-c:a", "aac",
                   "-shortest", path});
    return encoder.waitForFinished(60000) && encoder.exitStatus() == QProcess::NormalExit
           && encoder.exitCode() == 0;
  }

  QString media(const QString& name) const { return m_folder + QLatin1Char('/') + name; }

  static QByteArray digest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
  }

  // Files dragged in from another app: enter, move, then drop at the middle.
  static bool dropOn(QQuickWindow* window, const QList<QUrl>& urls,
                     const std::function<void()>& whileOver = {}) {
    QMimeData data;
    data.setUrls(urls);
    const QPoint at(window->width() / 2, window->height() / 2);
    QDragEnterEvent enter(at, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    QDragMoveEvent move(at, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
    if (whileOver) whileOver();
    QDropEvent drop(QPointF(at), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    return drop.isAccepted();
  }


  // Whether the frame drawn in the viewer's video area is mostly saturated
  // color, which the red and colorful fixtures are and a blank output is not.
  bool renderedVideoIsOpaque() const {
    QQuickItem* output = item(QStringLiteral("viewerVideoOutput"));
    const QImage frame = m_window->grabWindow();
    if (frame.isNull() || !frame.hasAlphaChannel()) return !frame.isNull();
    const qreal scale = frame.devicePixelRatio();
    const QRectF scene = output->mapRectToScene(output->property("contentRect").toRectF());
    const QRect bounds = QRectF(scene.topLeft() * scale, scene.size() * scale)
                             .toAlignedRect().adjusted(2, 2, -2, -2).intersected(frame.rect());
    if (bounds.isEmpty()) return false;
    for (int y = bounds.top(); y <= bounds.bottom(); y += 8) {
      for (int x = bounds.left(); x <= bounds.right(); x += 8) {
        if (qAlpha(frame.pixel(x, y)) < 250) {
          qWarning("see-through video pixel at %d,%d: alpha %d", x, y, qAlpha(frame.pixel(x, y)));
          return false;
        }
      }
    }
    return true;
  }

  bool renderedVideoHasColor() const {
    QQuickItem* output = item(QStringLiteral("viewerVideoOutput"));
    const QImage frame = m_window->grabWindow();
    if (frame.isNull()) return false;
    const qreal scale = frame.devicePixelRatio();
    const QRectF scene = output->mapRectToScene(output->property("contentRect").toRectF());
    const QRect bounds = QRectF(scene.topLeft() * scale, scene.size() * scale)
                             .toAlignedRect().intersected(frame.rect());
    if (bounds.isEmpty()) return false;
    int colorful = 0;
    int samples = 0;
    for (int y = bounds.top(); y <= bounds.bottom(); y += 8) {
      for (int x = bounds.left(); x <= bounds.right(); x += 8) {
        const QColor pixel = frame.pixelColor(x, y);
        colorful += pixel.saturation() > 150 && pixel.value() > 120;
        ++samples;
      }
    }
    return colorful > samples / 10;
  }

  // A call to the published player, made from another connection and waited
  // for without blocking the player's own thread.
  QDBusMessage mprisCall(const QString& interface, const QString& method,
                         const QVariantList& arguments = {}) const {
    QDBusMessage message = QDBusMessage::createMethodCall(
        m_mpris->serviceName(), QStringLiteral("/org/mpris/MediaPlayer2"), interface, method);
    message.setArguments(arguments);
    QDBusPendingCall call = QDBusConnection(QStringLiteral("omaroll-test-client")).asyncCall(message);
    if (!QTest::qWaitFor([&call] { return call.isFinished(); }, 5000)) {
      return {};
    }
    return call.reply();
  }

  QVariant mprisProperty(const QString& interface, const QString& name) const {
    const QDBusMessage reply =
        mprisCall(QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                  {interface, name});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
      return {};
    }
    return reply.arguments().constFirst().value<QDBusVariant>().variant();
  }

  QVariant prop(const char* name) const { return m_window->property(name); }

  void open(const QStringList& paths) {
    m_session->open(paths);
    if (!m_window->isVisible()) {
      m_window->show();
      QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }
    m_window->requestActivate();
    QTRY_VERIFY(item(QStringLiteral("viewerKeys"))->hasActiveFocus());
  }

  static QQuickItem* find(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    for (QQuickItem* child : root->childItems()) {
      if (match(child)) {
        return child;
      }
      if (QQuickItem* found = find(child, match)) {
        return found;
      }
    }
    return nullptr;
  }

  QQuickItem* item(const QString& objectName) const {
    QQuickItem* found = find(m_window->contentItem(), [&objectName](QQuickItem* candidate) {
      return candidate->objectName() == objectName;
    });
    if (!found) {
      qFatal("no item named %s", qPrintable(objectName));
    }
    return found;
  }

  // A row of the open viewer menu by action id. A row in View, Share or Edit
  // is placed once its submenu opens, so that submenu is opened first.
  QQuickItem* menuRow(const QString& id) {
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("viewerMenu"));
    QString group;
    for (const QVariant& entry : menu->property("entries").toList()) {
      const QVariantMap map = entry.toMap();
      if (map.value(QStringLiteral("id")).toString() == id) group = map.value(QStringLiteral("group")).toString();
    }
    const QString name = QStringLiteral("viewerMenu_") + id;
    const auto lookup = [&] {
      return find(m_window->contentItem(), [&name](QQuickItem* candidate) { return candidate->objectName() == name; });
    };
    if (!group.isEmpty()) {
      QObject* submenu = m_window->findChild<QObject*>(QStringLiteral("viewerSubmenu_") + group);
      if (submenu && !submenu->property("visible").toBool()) {
        QMetaObject::invokeMethod(item(QStringLiteral("viewerMenu_group-") + group), "click");
        QTest::qWaitFor([&] { return submenu->property("visible").toBool() && lookup(); }, 3000);
      }
    }
    QQuickItem* found = lookup();
    if (!found) qFatal("no menu row %s", qPrintable(id));
    return found;
  }

  static QStringList menuIds(QObject* menu) {
    QStringList ids;
    for (const QVariant& entry : menu->property("entries").toList()) {
      const QString id = entry.toMap().value(QStringLiteral("id")).toString();
      if (!id.isEmpty()) {
        ids.append(id);
      }
    }
    return ids;
  }

  static QPoint centre(QQuickItem* target) {
    return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
  }

  void click(QQuickItem* target) {
    QVERIFY(target);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(target));
    QTest::qWait(30);
  }

  // Two clicks a human interval apart. The first waits out the double-click
  // interval, so two pairs in a row are not read as a triple click; QTest's
  // default delay would also push the second click out of the pair.
  void doubleClick(const QPoint& at) {
    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    const int gap = interval / 4;
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at, interval + 100);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at, gap);
    QTest::qWait(30);
  }

  // Mouse wheel notches over a point; positive zooms in. Like a wheel on
  // Wayland, it carries pixel deltas as well as the angle.
  void wheel(const QPointF& at, int notches, int waitMs = 40) {
    QWheelEvent event(at, m_window->mapToGlobal(at.toPoint()), QPoint(0, 15 * notches),
                      QPoint(0, 120 * notches), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(m_window, &event);
    if (waitMs > 0) {
      QTest::qWait(waitMs);
    }
  }

  QTemporaryDir m_scratch;
  QString m_folder;
  QStringList m_order;
  OmarchyTheme* m_theme = nullptr;
  AppSettings* m_settings = nullptr;
  ActionLauncher* m_actions = nullptr;
  ActionRegistry* m_registry = nullptr;
  ViewerSession* m_session = nullptr;
  MediaInspector* m_mediaInfo = nullptr;
  SubtitleIndex* m_subtitles = nullptr;
  ThumbnailProvider* m_thumbnails = nullptr;
  RawImageProvider* m_raws = nullptr;
  LargeImageProvider* m_large = nullptr;
  QQmlEngine* m_engine = nullptr;
  QQmlContext* m_context = nullptr;
  MprisService* m_mpris = nullptr;
  QProcess m_busDaemon;
  std::unique_ptr<QObject> m_root;
  QQuickWindow* m_window = nullptr;
  QStringList m_warnings;
};

// Offscreen unconditionally, not just under ctest. Run by hand on a live
// session this would open a real window on the desktop.
int main(int argc, char* argv[]) {
  configureVideoPlayback();
  disableHeadlessAudio();
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication application(argc, argv);
  if (!QMediaDevices::audioOutputs().isEmpty()) {
    QTextStream(stderr) << "Refusing playback tests with audio outputs available\n";
    return 1;
  }
  ViewerTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}
#include "tst_viewer.moc"
