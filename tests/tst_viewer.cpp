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
#include "actions/ExternalEditors.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/HeadlessAudio.h"
#include "app/OpenRequest.h"
#include "app/VideoPlayback.h"
#include "library/MediaInspector.h"
#include "sources/FileVersion.h"
#include "subtitles/SubtitleIndex.h"
#include "sources/CameraRaw.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/RawImageProvider.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/HyprlandPlacement.h"
#include "viewer/MprisService.h"
#include "viewer/ViewerSession.h"
#include "viewer/ViewerWindows.h"

#include <QAudioDevice>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusVariant>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QJSValue>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QPointingDevice>
#include <QProcess>
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
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThreadPool>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <atomic>
#include <memory>

class StartupPosterProvider final : public QQuickImageProvider {
public:
  StartupPosterProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  QImage requestImage(const QString& id, QSize* size, const QSize&) override {
    QThread::msleep(id.toULong());
    QImage image(160, 90, QImage::Format_RGB32);
    image.fill(QColor(30, 140, 210));
    *size = image.size();
    return image;
  }
};

class StartupPlayerProbe final : public QObject {
  Q_OBJECT
public:
  QQuickWindow* window = nullptr;
  std::atomic<int> posterFrames{0};
  int framesAtCreation = -1;
  QQuickItem* poster = nullptr;
  int posterStatusAtCreation = -1;

public slots:
  void playerChanged() {
    if (window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))) {
      framesAtCreation = posterFrames.load();
      posterStatusAtCreation = poster->property("status").toInt();
    }
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
        if (!warning.toString().contains(QStringLiteral("No thumbnail for "))) {
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

    QQmlContext* shared = m_engine->rootContext();
    shared->setContextProperty(QStringLiteral("Theme"), m_theme);
    shared->setContextProperty(QStringLiteral("Actions"), m_actions);
    shared->setContextProperty(QStringLiteral("Settings"), m_settings);
    shared->setContextProperty(QStringLiteral("Registry"), m_registry);
    shared->setContextProperty(QStringLiteral("Editors"), new ExternalEditors(m_actions, m_engine));
    shared->setContextProperty(QStringLiteral("DemoMode"), true);
    shared->setContextProperty(QStringLiteral("Mpris"), m_mpris);
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
    delete m_engine;
    m_engine = nullptr;
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
    m_window->setProperty("chromeTimeout", 2200);
    m_settings->setVideoMuted(false);
    m_settings->setVideoVolume(0.8);
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
    const QString sibling = dir.filePath(QStringLiteral("a.png"));
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
      // Six megapixels: 24 MB at 32 bits, 48 MB at 64 bits. Release the
      // fixture writer's storage before probing the viewer's own policy.
      QImage image(3000, 2000, format);
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

  void videoStartupSubmitsThePosterBeforeCreatingThePlayer_data() {
    QTest::addColumn<QString>("posterSource");
    QTest::addColumn<bool>("expectPoster");
    QTest::newRow("asynchronous-poster") << QStringLiteral("image://startup-poster/90") << true;
    QTest::newRow("slow-poster-fallback") << QStringLiteral("image://startup-poster/600") << false;
    QTest::newRow("missing-poster-fallback") << QString() << false;
  }

  void videoStartupSubmitsThePosterBeforeCreatingThePlayer() {
    QFETCH(QString, posterSource);
    QFETCH(bool, expectPoster);
    ViewerSession session;
    QQmlContext context(m_context);
    context.setContextProperty(QStringLiteral("Session"), &session);
    QQmlComponent component(m_engine);
    component.loadFromModule("Omaroll", "Viewer");
    std::unique_ptr<QObject> root(component.beginCreate(&context));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    QVERIFY(window);
    auto* poster = find(window->contentItem(), [](QQuickItem* item) {
      return item->objectName() == QStringLiteral("viewerVideoPoster");
    });
    QVERIFY(poster);

    std::atomic<bool> readyAtSync{false};
    bool synchronized = false;
    StartupPlayerProbe probe;
    probe.window = window;
    probe.poster = poster;
    QVERIFY(connect(window, SIGNAL(playerChanged()), &probe, SLOT(playerChanged())));
    const auto sample = connect(window, &QQuickWindow::afterAnimating, this, [&] {
      readyAtSync.store(poster->property("status").toInt() == 1);
    });
    const auto sync = connect(window, &QQuickWindow::beforeSynchronizing, window, [&] {
      synchronized = readyAtSync.load();
    }, Qt::DirectConnection);
    bool delayed = false;
    const auto delay = connect(window, &QQuickWindow::beforeRendering, window, [&] {
      // Let thumbnail readiness change while an older frame is still being
      // rendered. A queued completion must retain that older frame's state.
      if (expectPoster && !delayed && QThread::currentThread() != window->thread()) {
        delayed = true;
        QThread::msleep(150);
      }
    }, Qt::DirectConnection);
    const auto submitted = connect(window, &QQuickWindow::afterFrameEnd, window, [&] {
      if (synchronized) ++probe.posterFrames;
    }, Qt::DirectConnection);
    const auto disconnect = qScopeGuard([&] {
      QObject::disconnect(sample);
      QObject::disconnect(sync);
      QObject::disconnect(delay);
      QObject::disconnect(submitted);
      window->hide();
    });

    // Component completion attaches the production watcher. Register our
    // submission counter first, before it can queue startup to the GUI.
    component.completeCreate();
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    // Remove the normal source binding so session changes cannot replace the
    // controlled thumbnail with a real decoder request.
    QVERIFY(QQmlProperty::write(poster, QStringLiteral("source"), QUrl(posterSource)));
    session.open({media(QStringLiteral("clip.mp4"))});
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(probe.framesAtCreation >= 0, 5000);
    if (expectPoster) {
      QVERIFY2(probe.framesAtCreation > 0,
               qPrintable(QStringLiteral("The player started before a poster frame was submitted (status %1)")
                              .arg(probe.posterStatusAtCreation)));
    } else {
      QCOMPARE(probe.framesAtCreation, 0);
    }
    auto* player = window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"));
    QVERIFY(player);
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
    QTest::qWait(100);
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
    QTest::newRow("dark") << QStringLiteral("dark");
    QTest::newRow("light") << QStringLiteral("light");
  }

  void narrowTransportKeepsSeekingWithCaptionsAndChangedSpeed() {
    QFETCH(QString, palette);
    QTemporaryDir themeHome;
    QVERIFY(themeHome.isValid());
    QVERIFY(QDir().mkpath(themeHome.filePath(QStringLiteral("omarchy"))));
    const QString fixture = QFINDTESTDATA(qPrintable(QStringLiteral("fixtures/themes/%1").arg(palette)));
    QVERIFY(QFile::link(fixture, themeHome.filePath(QStringLiteral("omarchy/current"))));
    OmarchyTheme theme(themeHome.path(), themeHome.path());
    m_context->setContextProperty(QStringLiteral("Theme"), &theme);
    const auto restoreTheme = qScopeGuard([this] {
      m_context->setContextProperty(QStringLiteral("Theme"), m_theme);
    });
    open({media(QStringLiteral("clip.mp4"))});
    QQuickItem* transport = item(QStringLiteral("viewerTransport"));
    QQuickItem* scrub = item(QStringLiteral("viewerScrub"));
    QVERIFY(transport && scrub);
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
    transport->setProperty("player", QVariant::fromValue(player.get()));
    transport->setProperty("hasCaptions", true);
    transport->setProperty("captionsOn", true);
    m_window->setProperty("chromePinned", true);
    for (const double rate : {1.0, 1.5}) {
      player->setProperty("playbackRate", rate);
      for (const int width : {320, 400}) {
        m_window->resize(width, 240);
        QTRY_VERIFY(scrub->width() >= 64);
        QVERIFY(scrub->mapToScene(QPointF()).x() >= 0);
        QVERIFY(scrub->mapToScene(QPointF(scrub->width(), 0)).x() <= width);
        if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
          QTRY_VERIFY(!m_window->grabWindow().isNull());
          QVERIFY(m_window->grabWindow().save(QStringLiteral("narrow-transport-%1-%2-%3.png")
                                               .arg(palette).arg(width).arg(rate)));
        }
      }
    }
    transport->setProperty("player", QVariant::fromValue(m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    m_window->setProperty("chromePinned", false);
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
    QTRY_COMPARE(player->position(), 1000);
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
    QTest::keyClick(m_window, Qt::Key_C);
    QTRY_VERIFY(!captions->property("active").toBool());
    QCOMPARE(player->activeSubtitleTrack(), -1);
    m_window->setProperty("chromePinned", false);
    player->pause();
  }

  // Video codecs and containers encoded here so the check needs no downloads.
  void lessCommonContainersPlay_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QStringList>("codec");
    QTest::newRow("h264-mp4") << QStringLiteral("clip.mp4")
                              << QStringList{QStringLiteral("libx264")};
    QTest::newRow("hevc-mp4") << QStringLiteral("clip-hevc.mp4")
                              << QStringList{QStringLiteral("libx265"), QStringLiteral("-threads"),
                                             QStringLiteral("1"), QStringLiteral("-x265-params"),
                                             QStringLiteral("pools=1:frame-threads=1")};
    QTest::newRow("vp9-webm") << QStringLiteral("clip.webm")
                              << QStringList{QStringLiteral("libvpx-vp9")};
    QTest::newRow("mpeg4-avi") << QStringLiteral("clip.avi")
                               << QStringList{QStringLiteral("mpeg4")};
    QTest::newRow("mpeg2-mts") << QStringLiteral("clip.mts")
                              << QStringList{QStringLiteral("mpeg2video")};
    QTest::newRow("asf") << QStringLiteral("clip.asf") << QStringList{QStringLiteral("wmv2")};
    QTest::newRow("ogm") << QStringLiteral("clip.ogm")
                         << QStringList{QStringLiteral("libtheora"), QStringLiteral("-f"),
                                        QStringLiteral("ogg")};
    QTest::newRow("3g2") << QStringLiteral("clip.3g2") << QStringList{QStringLiteral("mpeg4")};
  }

  void lessCommonContainersPlay() {
    QFETCH(QString, name);
    QFETCH(QStringList, codec);
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
                                      QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
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
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QTRY_VERIFY_WITH_TIMEOUT(renderedVideoHasColor(), 5000);
    }
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
    QVERIFY(QMetaObject::invokeMethod(item(QStringLiteral("viewerMenu_favorite")), "click"));
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
    QVERIFY(QMetaObject::invokeMethod(item(QStringLiteral("viewerMenu_slideshow")), "click"));
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
    viewers.setPlacementQuery([] { return HyprlandPlacement::Plan{}; });
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

  // Each file opened from outside gets a viewer of its own. Asking again for
  // what one already shows brings that one forward, a selection stays one
  // viewer, and closing leaves the rest alone with one closed viewer kept.
  // A viewer opened beside another maps under the plain title, so Hyprland's
  // float rule leaves it tiled, and then takes its own.
  void severalViewersOpenSideBySideAndCloseCleanly() {
    ViewerWindows viewers(*m_engine);
    int queries = 0;
    viewers.setPlacementQuery([&queries] {
      ++queries;
      HyprlandPlacement::Plan plan;
      plan.viewers = 1;
      return plan;
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
    // Asked once before mapping and once after, for opens that raced.
    QTRY_COMPARE(queries, 2);
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
    viewers.setPlacementQuery([] { return HyprlandPlacement::Plan{}; });
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
    QCOMPARE(prop("sourceWidth").toReal(), 48.0);
    QCOMPARE(prop("sourceHeight").toReal(), 64.0);
    // Fitted to the window, the preview is far past its own pixels.
    QVERIFY(prop("rawDetailWanted").toBool());
    QQuickItem* detail = item(QStringLiteral("viewerRawDetail"));
    QTRY_COMPARE(detail->property("status").toInt(), 1);
    QCOMPARE(detail->property("sourceSize").toSize(), QSize(48, 64));
    QCOMPARE(prop("rawDetailPath").toString(), camera);

    QTest::keyClick(m_window, Qt::Key_A);
    QTRY_COMPARE(prop("status").toString(), QStringLiteral("That one cannot open camera raws"));

    // A raw without a preview gets the plugin's half-size decode, upright.
    QTRY_COMPARE(m_session->count(), 2);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(m_session->path(), bare);
    QTRY_VERIFY(prop("imageReady").toBool());
    QCOMPARE(prop("loadedSize").toSize(), QSize(24, 32));
    QCOMPARE(prop("sourceWidth").toReal(), 48.0);
    QTRY_COMPARE(detail->property("status").toInt(), 1);
    m_window->close();
    m_session->clear();
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

  QString media(const QString& name) const { return m_folder + QLatin1Char('/') + name; }

  // Whether the frame drawn in the viewer's video area is mostly saturated
  // color, which the red and colorful fixtures are and a blank output is not.
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
