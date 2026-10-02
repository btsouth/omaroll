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
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/HeadlessAudio.h"
#include "library/MediaInspector.h"
#include "subtitles/SubtitleIndex.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/ViewerSession.h"

#include <QAudioDevice>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThreadPool>
#include <QtTest>

#include <functional>
#include <memory>

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

    QQmlContext* shared = m_engine->rootContext();
    shared->setContextProperty(QStringLiteral("Theme"), m_theme);
    shared->setContextProperty(QStringLiteral("Actions"), m_actions);
    shared->setContextProperty(QStringLiteral("Settings"), m_settings);
    shared->setContextProperty(QStringLiteral("Registry"), m_registry);
    shared->setContextProperty(QStringLiteral("DemoMode"), true);
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
    delete m_engine;
    m_engine = nullptr;
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
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(prop("effectiveScale").toReal(), 1.0 / m_window->devicePixelRatio());
    QCOMPARE(prop("zoomPercent").toInt(), 100);
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

  // The checked-in test pattern: saturated bars, two audio tracks and a
  // subtitle track. Under OpenGL the bars must reach the screen, not just a
  // decoded frame in memory.
  void videoFrameRendersAndSubtitlesCycle() {
    const QString source = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(!source.isEmpty());
    const QString folder = m_scratch.filePath(QStringLiteral("tracks"));
    QVERIFY(QDir().mkpath(folder));
    const QString path = folder + QStringLiteral("/tracks.mkv");
    QVERIFY(QFile::exists(path) || QFile::copy(source, path));
    open({path});
    QMediaPlayer* player = nullptr;
    QTRY_VERIFY((player = m_window->findChild<QMediaPlayer*>(QStringLiteral("viewerPlayer"))));
    QTRY_VERIFY_WITH_TIMEOUT(player->playbackState() == QMediaPlayer::PlayingState, 5000);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);

    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QCOMPARE(QQuickWindow::graphicsApi(), QSGRendererInterface::OpenGL);
      const auto renderedVideoHasColor = [&] {
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
      };
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

  void theWindowRaisedNoQmlWarnings() {
    QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
  }

private:
  QString media(const QString& name) const { return m_folder + QLatin1Char('/') + name; }

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
  QQmlEngine* m_engine = nullptr;
  QQmlContext* m_context = nullptr;
  std::unique_ptr<QObject> m_root;
  QQuickWindow* m_window = nullptr;
  QStringList m_warnings;
};

// Offscreen unconditionally, not just under ctest. Run by hand on a live
// session this would open a real window on the desktop.
int main(int argc, char* argv[]) {
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
