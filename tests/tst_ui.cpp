// Drives the real Main.qml, offscreen, with synthetic input.
//
// The unit suite covers the C++; this covers what a person does with the
// window. It exists because the sheets shipped with an input bug no unit test
// could see: a TapHandler only takes a passive grab, so a click on a control
// inside a sheet also reached the scrim behind it and the tile behind that.
// Every scenario here is a thing someone actually did and saw go wrong, or the
// same gesture on a surface that could fail the same way.
//
// Events go through QTest into the offscreen window, never to the live seat.

#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/DuplicateIndex.h"
#include "library/MediaDateIndex.h"
#include "library/MediaInspector.h"
#include "library/SimilarityIndex.h"
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "pdf/PdfInspector.h"
#include "pdf/PdfProvider.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailProvider.h"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include <QPainter>
#include <QPdfWriter>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QVideoSink>
#include <QVideoFrame>
#include <QtTest>

#include <functional>

class UiTest : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    QVERIFY(m_scratch.isValid());
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // The same environment main.cpp builds for --demo and --render.
    const DemoLibrary::Layout layout = DemoLibrary::build();
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", layout.pictures.toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", layout.videos.toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", layout.pictures.toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", layout.videos.toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", layout.root.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch.filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME", m_scratch.filePath(QStringLiteral("data")).toUtf8()));
    // The demo UI test exercises the export sheet without depending on an
    // Omarchy installation on the build machine.
    const QString toolBin = m_scratch.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(toolBin));
    QFile transcode(toolBin + QStringLiteral("/omarchy-transcode"));
    QVERIFY(transcode.open(QIODevice::WriteOnly));
    transcode.write("#!/bin/sh\nexit 0\n");
    transcode.close();
    QVERIFY(transcode.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QFile tesseract(toolBin + QStringLiteral("/tesseract"));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\nprintf 'First  line\\nSecond line\\n'\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QFile zbar(toolBin + QStringLiteral("/zbarimg"));
    QVERIFY(zbar.open(QIODevice::WriteOnly));
    zbar.write("#!/bin/sh\n"
               "for last do :; done\n"
               "case \"$last\" in\n"
               "  *qr-positive*) printf 'otpauth://ui-private-value\\n' ;;\n"
               "  *) exit 4 ;;\n"
               "esac\n");
    zbar.close();
    QVERIFY(zbar.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    QFile wlCopy(toolBin + QStringLiteral("/wl-copy"));
    QVERIFY(wlCopy.open(QIODevice::WriteOnly));
    wlCopy.write("#!/bin/sh\ncat > \"$OMAROLL_UI_CLIPBOARD_LOG\"\n");
    wlCopy.close();
    QVERIFY(wlCopy.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("OMAROLL_UI_CLIPBOARD_LOG",
                    m_scratch.filePath(QStringLiteral("clipboard.log")).toUtf8()));
    QVERIFY(qputenv("PATH", toolBin.toUtf8() + ':' + qgetenv("PATH")));
    // A name with every character that trips URL parsing. Newest by mtime,
    // so it sits at the top of the grid.
    m_oddPath = layout.pictures + QStringLiteral("/odd %20 name #1 (copy)?.jpg");
    QVERIFY(QFile::copy(layout.pictures + QStringLiteral("/alpine-dawn.jpg"), m_oddPath));
    m_pdfPath = layout.pictures + QStringLiteral("/omaroll-guide.pdf");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       layout.root + QStringLiteral("/config"));

    m_theme = new OmarchyTheme(m_scratch.filePath(QStringLiteral("state")),
                               m_scratch.filePath(QStringLiteral("config")), this);
    m_settings = new AppSettings(this);
    m_settings->setSlideshowVideos(true);
    m_captures = new CaptureModel(m_settings, this);
    m_library = new CaptureFilterModel(this);
    m_library->setSourceModel(m_captures);
    m_duplicates = new DuplicateIndex(m_captures, this);
    connect(m_library, &CaptureFilterModel::duplicatesOnlyChanged, m_duplicates,
            [this] { m_duplicates->setActive(m_library->duplicatesOnly()); });
    connect(m_duplicates, &DuplicateIndex::groupsChanged, m_library,
            [this] { m_library->setDuplicateGroups(m_duplicates->groups()); });
    m_similarities = new SimilarityIndex(m_captures, this);
    connect(m_library, &CaptureFilterModel::similarOnlyChanged, m_similarities,
            [this] { m_similarities->setActive(m_library->similarOnly()); });
    connect(m_similarities, &SimilarityIndex::groupsChanged, m_library,
            [this] { m_library->setSimilarGroups(m_similarities->groups()); });
    m_mediaInfo = new MediaInspector(this);
    m_pdfInfo = new PdfInspector(this);
    m_mediaDates = new MediaDateIndex(nullptr, this);
    m_actions = new ActionLauncher(this);
    m_registry = new ActionRegistry(m_actions, this);
    m_tailscale = new TailscalePeers(this);
    m_matte = new MatteComposer(this);
    m_textIndex = new OcrIndex(m_captures, this);
    m_qr = new QrDetector(m_captures, this);
    connect(m_actions, &ActionLauncher::outputPending, m_captures, &CaptureModel::holdPath);
    connect(m_actions, &ActionLauncher::outputSettled, m_captures, &CaptureModel::releasePath);

    m_engine = new QQmlApplicationEngine(this);
    connect(m_engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError>& warnings) {
      for (const QQmlError& warning : warnings) {
        m_warnings.append(warning.toString());
      }
    });
    m_engine->addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    m_thumbnails = new ThumbnailProvider;
    m_mattes = new MatteProvider;
    m_pdfs = new PdfProvider;
    m_engine->addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), m_thumbnails);
    m_engine->addImageProvider(QLatin1String(MatteProvider::kProviderId), m_mattes);
    m_engine->addImageProvider(QLatin1String(PdfProvider::kProviderId), m_pdfs);

    QQmlContext* context = m_engine->rootContext();
    context->setContextProperty(QStringLiteral("Theme"), m_theme);
    context->setContextProperty(QStringLiteral("Captures"), m_library);
    context->setContextProperty(QStringLiteral("Library"), m_captures);
    context->setContextProperty(QStringLiteral("Actions"), m_actions);
    context->setContextProperty(QStringLiteral("Settings"), m_settings);
    context->setContextProperty(QStringLiteral("Registry"), m_registry);
    context->setContextProperty(QStringLiteral("Matte"), m_matte);
    context->setContextProperty(QStringLiteral("TextIndex"), m_textIndex);
    context->setContextProperty(QStringLiteral("Qr"), m_qr);
    context->setContextProperty(QStringLiteral("Duplicates"), m_duplicates);
    context->setContextProperty(QStringLiteral("Similarities"), m_similarities);
    context->setContextProperty(QStringLiteral("MediaInfo"), m_mediaInfo);
    context->setContextProperty(QStringLiteral("PdfInfo"), m_pdfInfo);
    context->setContextProperty(QStringLiteral("MediaDates"), m_mediaDates);
    context->setContextProperty(QStringLiteral("Tailscale"), m_tailscale);
    context->setContextProperty(QStringLiteral("DemoMode"), true);
    context->setContextProperty(QStringLiteral("InitialPath"), QString());
    context->setContextProperty(QStringLiteral("InitialFolderPath"), QString());

    m_engine->loadFromModule("Omaroll", "Main");
    QVERIFY2(!m_engine->rootObjects().isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
    m_window = qobject_cast<QQuickWindow*>(m_engine->rootObjects().constFirst());
    QVERIFY(m_window);
    m_window->resize(1280, 820);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowCount() >= 3, 15000);
    // Let the grid lay its tiles out before anything is clicked.
    QTRY_VERIFY_WITH_TIMEOUT(cardFor(pathAt(1)) != nullptr, 5000);
  }

  void cleanupTestCase() {
    m_thumbnails->shutdown();
    m_mattes->shutdown();
    m_pdfs->shutdown();
    delete m_engine;
    m_engine = nullptr;
    QThreadPool::globalInstance()->waitForDone();
  }

  // Every test starts from the plain grid with nothing selected.
  void init() {
    for (int guard = 0; guard < 8 && prop("anySheetOpen").toBool(); ++guard) {
      invoke("dismissTopLayer");
      QTest::qWait(20);
    }
    QVERIFY(!prop("anySheetOpen").toBool());
    m_library->setDuplicatesOnly(false);
    m_library->setSimilarOnly(false);
    QMetaObject::invokeMethod(item("library"), "clearChecked");
    item("library")->setProperty("currentIndex", 0);
    item("library")->forceActiveFocus();
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  void matteControlsKeepTheSheetOpen() {
    QQuickItem* matte = item("matteSheet");
    perform(QStringLiteral("matte"), pathAt(0));
    QTRY_VERIFY(matte->isVisible());

    click(pill(matte, QStringLiteral("1:1")));
    QCOMPARE(matte->property("aspect").toInt(), 1);
    QVERIFY(matte->isVisible());

    click(pill(matte, QStringLiteral("16:9")));
    QCOMPARE(matte->property("aspect").toInt(), 2);
    QVERIFY(matte->isVisible());

    const int padding = matte->property("paddingPercent").toInt();
    click(pill(matte, QStringLiteral("Padding %1%").arg(padding)));
    QVERIFY(matte->property("paddingPercent").toInt() != padding);
    QVERIFY(matte->isVisible());

    // A matte chip: a TapHandler on a ListView delegate, the original report.
    QQuickItem* chip = find(matte, [](QQuickItem* candidate) {
      return candidate->property("modelData").toString() == QStringLiteral("Aurora");
    });
    QVERIFY(chip);
    click(chip);
    settle();
    QCOMPARE(matte->property("selected").toInt(), 2);
    QVERIFY(matte->isVisible());

    // Nothing above reached the grid.
    QVERIFY(!item("detail")->isVisible());
    QCOMPARE(item("library")->property("currentIndex").toInt(), 0);
    QCOMPARE(item("library")->property("checkedCount").toInt(), 0);
  }

  void closedMatteDoesNotReloadItsPreviousFile() {
    const QString path = m_scratch.filePath(QStringLiteral("matte-disposable.png"));
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      m_window->resize(1280, 820);
    });
    perform(QStringLiteral("matte"), path);
    QTRY_COMPARE(item("mattePreview")->property("status").toInt(), 1);
    invoke("dismissTopLayer");
    QTRY_VERIFY(item("mattePreview")->property("source").toUrl().isEmpty());
    QVERIFY(QFile::remove(path));
    m_window->resize(560, 420);
    QTest::qWait(100);
    QVERIFY(!find(item("matteSheet"), [](QQuickItem* child) {
      return child->property("source").toString().startsWith(QStringLiteral("image://matte/"));
    }));
  }

  void rightClickOnASheetStaysOnTheSheet() {
    QQuickItem* matte = item("matteSheet");
    perform(QStringLiteral("matte"), pathAt(0));
    QTRY_VERIFY(matte->isVisible());

    // On the card, over the preview.
    click(matte, Qt::RightButton, QPoint(640, 300));
    settle();
    QVERIFY(matte->isVisible());
    QVERIFY(!item("detail")->isVisible());

    // On the scrim, over a tile. Only a left click there closes.
    click(matte, Qt::RightButton, QPoint(60, 400));
    settle();
    QVERIFY(matte->isVisible());
    QVERIFY(!item("detail")->isVisible());

    click(matte, Qt::LeftButton, QPoint(60, 400));
    QTRY_VERIFY(!matte->isVisible());
    QVERIFY(!item("detail")->isVisible());
    QCOMPARE(item("library")->property("currentIndex").toInt(), 0);
  }

  void viewerControlsDoNotReachTheGrid() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    const QString shown = detail->property("path").toString();
    QCOMPARE(shown, pathAt(0));

    // Two quick clicks on Rotate: two rotations, and never the double-tap that
    // used to open whatever tile sat under the button.
    QQuickItem* rotate = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((rotate = pill(detail, QStringLiteral("Rotate"))) != nullptr, 5000);
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(rotate));
    settle();
    QVERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), shown);
    QCOMPARE(detail->property("imageRotation").toInt(), 180);
    QCOMPARE(item("library")->property("currentIndex").toInt(), 0);
    QCOMPARE(item("library")->property("checkedCount").toInt(), 0);

    // A right click on the viewer's backdrop closes nothing and opens nothing.
    click(detail, Qt::RightButton, QPoint(30, 400));
    settle();
    QVERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), shown);
  }

  void imageViewerSupportsActualSizeFlipsDeepZoomAndAnimationPause() {
    int imageRow = -1;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (!m_library->isVideoAt(row) && !m_library->isDocumentAt(row)) {
        imageRow = row;
        break;
      }
    }
    QVERIFY(imageRow >= 0);
    openDetail(imageRow);
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->property("stillReady").toBool());
    QTRY_COMPARE(item("transparencyGrid")->property("status").toInt(), 1);

    click(item("actualSizeButton"));
    QTRY_VERIFY(qAbs(detail->property("displayedImageScale").toDouble() - 1.0) < 0.01);
    click(item("flipHorizontalButton"));
    click(item("flipVerticalButton"));
    QVERIFY(detail->property("imageFlipHorizontal").toBool());
    QVERIFY(detail->property("imageFlipVertical").toBool());

    QVERIFY(QMetaObject::invokeMethod(detail, "resetImageView"));
    QVERIFY(QMetaObject::invokeMethod(detail, "adjustImageZoom", Q_ARG(QVariant, 10.0)));
    QVERIFY(detail->property("imageZoom").toDouble() > 4.0);
    QTest::keyClick(m_window, Qt::Key_0);
    QCOMPARE(detail->property("imageZoom").toDouble(), 1.0);
    QVERIFY(!detail->property("imageFlipHorizontal").toBool());
    QVERIFY(!detail->property("imageFlipVertical").toBool());
    QTest::keyClick(m_window, Qt::Key_H, Qt::ShiftModifier);
    QTest::keyClick(m_window, Qt::Key_V, Qt::ShiftModifier);
    QVERIFY(detail->property("imageFlipHorizontal").toBool());
    QVERIFY(detail->property("imageFlipVertical").toBool());

    // Animated files reserve Space for playback instead of launching the
    // default image action. The actual decoder binding uses the same state.
    detail->setProperty("fileName", QStringLiteral("animation.gif"));
    detail->setProperty("animationPlaying", true);
    QSignalSpy action(detail, SIGNAL(actionTriggered(QString)));
    QTest::keyClick(m_window, Qt::Key_Space);
    QVERIFY(!detail->property("animationPlaying").toBool());
    QCOMPARE(action.size(), 0);

    m_window->resize(560, 420);
    QTRY_VERIFY(!item("actualSizeButton")->isVisible());
    QVERIFY(!item("flipHorizontalButton")->isVisible());
    QVERIFY(!item("flipVerticalButton")->isVisible());
    QQuickItem* fit = pill(detail, QStringLiteral("Fit"));
    QVERIFY(fit);
    QVERIFY(fit->mapToScene(QPointF(0, 0)).x() >= 0);
    m_window->resize(1280, 820);
    QTRY_VERIFY(item("actualSizeButton")->isVisible());
  }

  void videoViewerHasDefaultPlayerKeyboardAndPointerControls() {
    int videoRow = -1;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) {
        videoRow = row;
        break;
      }
    }
    QVERIFY(videoRow >= 0);
    openDetail(videoRow);
    QQuickItem* detail = item("detail");
    QTRY_COMPARE_WITH_TIMEOUT(detail->property("playbackState").toInt(),
                              static_cast<int>(QMediaPlayer::PlayingState), 5000);
    QCOMPARE(detail->property("playbackLoops").toInt(), 1);
    QVERIFY(!detail->property("playbackMuted").toBool());

    QSignalSpy action(detail, SIGNAL(actionTriggered(QString)));
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(detail->property("playbackState").toInt(),
                 static_cast<int>(QMediaPlayer::PausedState));
    QCOMPARE(action.size(), 0);
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(detail->property("playbackState").toInt(),
                 static_cast<int>(QMediaPlayer::PlayingState));

    const bool muted = detail->property("playbackMuted").toBool();
    QTest::keyClick(m_window, Qt::Key_M);
    QCOMPARE(detail->property("playbackMuted").toBool(), !muted);
    const double volume = detail->property("playbackVolume").toDouble();
    QTest::keyClick(m_window, Qt::Key_Up);
    QVERIFY(detail->property("playbackVolume").toDouble() > volume);
    QVERIFY(!detail->property("playbackMuted").toBool());

    QTest::keyClick(m_window, Qt::Key_BracketRight);
    QCOMPARE(detail->property("playbackRate").toDouble(), 1.25);
    QTest::keyClick(m_window, Qt::Key_Backspace);
    QCOMPARE(detail->property("playbackRate").toDouble(), 1.0);

    QQuickItem* output = item("videoOutput");
    QTRY_VERIFY(output->isVisible() && output->width() > 0);
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(output));
    QTRY_VERIFY(detail->property("fullScreen").toBool());
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(output));
    QTRY_VERIFY(!detail->property("fullScreen").toBool());
  }

  void rightClickOnATileOpensThatTile() {
    QQuickItem* card = cardFor(pathAt(1));
    QVERIFY(card);
    click(card, Qt::RightButton);
    settle();
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), pathAt(1));
  }

  void clickOutsideTheLibraryBrowserIsConsumed() {
    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(browser);
    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());

    // Click the scrim: the browser closes and nothing in the library is
    // touched.
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, QPoint(10, 400));
    settle();
    QTRY_VERIFY(!browser->property("visible").toBool());
    QCOMPARE(item("library")->property("currentIndex").toInt(), 0);
    QVERIFY(!item("detail")->isVisible());
    QVERIFY(item("library")->isEnabled());
    // The arrow keys work again without a click on a tile first.
    QTRY_VERIFY(item("library")->hasActiveFocus());

    // Popup menus still take clicks: choose a sort order from one.
    QObject* sortMenu = m_window->findChild<QObject*>(QStringLiteral("sortMenu"));
    QVERIFY(sortMenu);
    QMetaObject::invokeMethod(sortMenu, "open");
    QTRY_VERIFY(sortMenu->property("visible").toBool());
    QQuickItem* oldest = find(m_window->contentItem(), [](QQuickItem* candidate) {
      return candidate->property("text").toString() == QStringLiteral("Oldest") &&
             candidate->isVisible() && candidate->width() > 0 && candidate->inherits("QQuickText");
    });
    QVERIFY(oldest);
    click(oldest);
    QTRY_COMPARE(m_library->sortMode(), 1);
    QTRY_VERIFY(!sortMenu->property("visible").toBool());
    QTRY_VERIFY(item("library")->hasActiveFocus());
    m_library->setSortMode(0);
  }

  void libraryBrowserFindsAndOpensAFolderFromTheKeyboard() {
    const QStringList folders = m_library->folders();
    QVERIFY(folders.size() >= 2);
    const QString wanted = folders.constLast();

    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(browser);
    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());

    QQuickItem* search = item("librarySearch");
    QQuickItem* choices = item("libraryChoices");
    QTRY_VERIFY(search->hasActiveFocus());
    search->setProperty("text", wanted);
    QTRY_COMPARE(browser->property("query").toString(), wanted);
    QTRY_COMPARE(choices->property("count").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Return);

    QTRY_VERIFY(!browser->property("visible").toBool());
    QTRY_COMPARE(m_library->folderFilter(), wanted);
    QVERIFY(m_library->rowCount() > 0);
    for (int row = 0; row < m_library->rowCount(); ++row) {
      const QString directory = QFileInfo(pathAt(row)).absolutePath();
      QVERIFY(directory == wanted || directory.startsWith(wanted + QLatin1Char('/')));
    }

    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());
    QTRY_COMPARE(browser->property("query").toString(), QString());
    QCOMPARE(search->property("text").toString(), QString());
    QTRY_COMPARE(choices->property("count").toInt(), folders.size());
    QTRY_COMPARE(choices->property("currentIndex").toInt(), folders.indexOf(wanted));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!browser->property("visible").toBool());

    m_library->setFolderFilter({});
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  void favouriteFromTheViewerKeepsItOpen() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QVERIFY(!m_settings->isFavorite(pathAt(0)));

    QTest::keyClick(m_window, Qt::Key_V);
    QTRY_VERIFY(m_settings->isFavorite(pathAt(0)));
    QVERIFY(detail->isVisible());
    QTRY_VERIFY(detail->property("favorite").toBool());
    QVERIFY(detail->property("status").toString().contains(QStringLiteral("favourites")));

    QTest::keyClick(m_window, Qt::Key_V);
    QTRY_VERIFY(!m_settings->isFavorite(pathAt(0)));
    QVERIFY(detail->isVisible());
  }

  void tailscaleSheetHandsFocusBackToTheViewer() {
    QQuickItem* detail = item("detail");
    QQuickItem* tailscale = item("tailscaleSheet");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());

    perform(QStringLiteral("tailscale"), pathAt(0));
    QTRY_VERIFY(tailscale->isVisible());
    QVERIFY(detail->isVisible());
    QVERIFY(!detail->isEnabled());

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!tailscale->isVisible());
    QVERIFY(detail->isVisible());
    QTRY_VERIFY(detail->hasActiveFocus());

    // The viewer's own keys work again.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!detail->isVisible());
  }

  void viewerSheetRestoresTheFocusedAction() {
    QQuickItem* detail = item("detail");
    QQuickItem* exportSheet = item("exportSheet");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());

    QTest::keyClick(m_window, Qt::Key_Tab);
    for (int guard = 0;
         guard < 20 && (!activeViewerAction() || activeViewerAction()->objectName() !=
                                                     QStringLiteral("viewerAction_export"));
         ++guard) {
      QTest::keyClick(m_window, Qt::Key_Down);
    }
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_export"));

    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(exportSheet->isVisible());
    QVERIFY(detail->isVisible());
    QVERIFY(!detail->isEnabled());

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!exportSheet->isVisible());
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_export"));
  }

  void escapeUnwindsAndReturnsFocusToTheGrid() {
    QQuickItem* settings = item("settingsSheet");
    QMetaObject::invokeMethod(settings, "open");
    QTRY_VERIFY(settings->isVisible());
    QVERIFY(!item("library")->isEnabled());

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!settings->isVisible());
    QTRY_VERIFY(item("library")->hasActiveFocus());
    QVERIFY(item("library")->isEnabled());
  }

  void tileSizeFollowsControlPlusAndMinus() {
    const int before = m_settings->tileWidth();
    QTest::keyClick(m_window, Qt::Key_Plus, Qt::ControlModifier);
    QTRY_COMPARE(m_settings->tileWidth(), before + 40);
    QTest::keyClick(m_window, Qt::Key_Minus, Qt::ControlModifier);
    QTRY_COMPARE(m_settings->tileWidth(), before);
    QTest::keyClick(m_window, Qt::Key_0, Qt::ControlModifier);
    QTRY_COMPARE(m_settings->tileWidth(), 240);
    // CaptureGrid coalesces cell-width changes for 80 ms before rebuilding
    // delegate positions. Finish that relayout in this scenario so it cannot
    // detach the model while the next scenario is sending selection keys.
    QTest::qWait(120);
    QTRY_VERIFY(item("library")->property("layoutReady").toBool());
  }

  void searchFiltersTheGridAndEscapeClearsIt() {
    const int all = m_library->rowCount();
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(item("filters")->property("searchActive").toBool());
    typeText(QStringLiteral("alpine"));
    QTRY_COMPARE(m_library->rowCount(), 1);
    QCOMPARE(m_library->fileNameAt(0), QStringLiteral("alpine-dawn.jpg"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(m_library->rowCount(), all);
    QVERIFY(m_library->property("searchText").toString().isEmpty());
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  void sectionKeysSwitchTheFilter() {
    QTest::keyClick(m_window, Qt::Key_2);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_4);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 2);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), -1);
    QTest::keyClick(m_window, Qt::Key_7);
    QTRY_VERIFY(m_library->property("favoritesOnly").toBool());
    QTest::keyClick(m_window, Qt::Key_7);
    QTRY_VERIFY(!m_library->property("favoritesOnly").toBool());
  }

  void exactDuplicatesOpenAsAGroupedReviewView() {
    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(browser);
    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());

    QQuickItem* duplicatesRow = find(m_window->contentItem(), [](QQuickItem* candidate) {
      return candidate->inherits("QQuickText") && candidate->isVisible() &&
             candidate->property("text").toString() == QStringLiteral("Exact duplicates");
    });
    QVERIFY(duplicatesRow);
    click(duplicatesRow);
    QTRY_VERIFY(m_library->duplicatesOnly());
    QTRY_VERIFY_WITH_TIMEOUT(!m_duplicates->scanning(), 15000);

    // initTestCase deliberately copied alpine-dawn.jpg under an odd filename.
    // The duplicate view finds those two, keeps both files, and groups them.
    QCOMPARE(m_duplicates->groupCount(), 1);
    QCOMPARE(m_duplicates->duplicateCount(), 2);
    QCOMPARE(m_library->count(), 2);
    QCOMPARE(m_library->gridLabelAt(0), QStringLiteral("Exact match 1 of 1"));
    QVERIFY(QFileInfo::exists(m_library->pathAt(0)));
    QVERIFY(QFileInfo::exists(m_library->pathAt(1)));
    QQuickItem* keep = pill(m_window->contentItem(), QStringLiteral("Keep selected"));
    QVERIFY(keep);
    click(keep);
    QTRY_VERIFY(item("confirm")->isVisible());
    QVERIFY(item("confirm")->property("title").toString().startsWith(QStringLiteral("Keep ")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!item("confirm")->isVisible());
    QQuickItem* browse = pill(m_window->contentItem(), QStringLiteral("Duplicates  ▾"));
    QVERIFY(browse);

    m_window->resize(560, 420);
    QTRY_VERIFY(pill(m_window->contentItem(), QStringLiteral("Dupes  ▾")) != nullptr);
    m_window->resize(1280, 820);
    QTRY_VERIFY(pill(m_window->contentItem(), QStringLiteral("Duplicates  ▾")) != nullptr);
    // CaptureGrid coalesces width changes for 80 ms. Finish that rebuild here
    // so its saved selection cannot land in the following scenario.
    QTest::qWait(120);
    QTRY_VERIFY(item("library")->property("layoutReady").toBool());
  }

  void browseQuickDatesApplyTheExpectedRanges() {
    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(browser);
    const QString today = QDate::currentDate().toString(Qt::ISODate);

    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());
    click(pill(m_window->contentItem(), QStringLiteral("Today")));
    QTRY_VERIFY(!browser->property("visible").toBool());
    QCOMPARE(m_library->dateFrom(), today);
    QCOMPARE(m_library->dateTo(), today);
    QCOMPARE(m_library->dateField(), 0);
    QVERIFY(m_library->modifiedAfter().isEmpty());

    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());
    click(pill(m_window->contentItem(), QStringLiteral("Recently modified")));
    QTRY_VERIFY(!browser->property("visible").toBool());
    QCOMPARE(m_library->dateTo(), today);
    QCOMPARE(m_library->dateField(), 1);
    QCOMPARE(QDate::fromString(m_library->dateFrom(), Qt::ISODate),
             QDate::currentDate().addDays(-6));

    // A first run has no previous visit, so this shortcut deliberately falls
    // back to today's modified files instead of producing an empty view.
    QVERIFY(m_settings->previousVisit().isEmpty());
    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());
    click(pill(m_window->contentItem(), QStringLiteral("New since last visit")));
    QTRY_VERIFY(!browser->property("visible").toBool());
    QCOMPARE(m_library->dateFrom(), today);
    QCOMPARE(m_library->dateTo(), today);
    QCOMPARE(m_library->dateField(), 1);
    m_library->clearDateRange();
  }

  void similarPicturesOpenAsAGroupedReviewView() {
    const QString recompressed = QFileInfo(m_oddPath).dir().filePath(
        QStringLiteral("alpine-recompressed.jpg"));
    QImage source(m_oddPath);
    QVERIFY(!source.isNull());
    QVERIFY(source.scaled(source.width() * 2, source.height() * 2, Qt::IgnoreAspectRatio,
                          Qt::SmoothTransformation)
                .save(recompressed, "JPG", 58));
    m_captures->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(recompressed) >= 0, 5000);

    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(browser);
    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());
    click(pill(m_window->contentItem(), QStringLiteral("Similar pictures")));
    QTRY_VERIFY(m_library->similarOnly());
    QTRY_VERIFY_WITH_TIMEOUT(!m_similarities->scanning(), 15000);
    QVERIFY(m_similarities->groupCount() >= 1);
    QVERIFY(m_similarities->similarCount() >= 3);
    QVERIFY(m_library->rowOf(recompressed) >= 0);
    QVERIFY(m_library->gridLabelAt(0).startsWith(QStringLiteral("Similar set ")));

    m_library->setSimilarOnly(false);
    QVERIFY(QFile::remove(recompressed));
    m_captures->refresh();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowOf(recompressed), -1, 5000);
  }

  void smartCollectionSavesAndReappliesTheCurrentView() {
    m_library->setSearchText(QStringLiteral("alpine"));
    m_library->setSortMode(CaptureFilterModel::NameAscending);
    QTRY_COMPARE(m_library->count(), 1);

    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(browser);
    QMetaObject::invokeMethod(browser, "open");
    QTRY_VERIFY(browser->property("visible").toBool());
    click(pill(m_window->contentItem(), QStringLiteral("Save current view")));
    QQuickItem* sheet = item("albumNameSheet");
    QTRY_VERIFY(sheet->isVisible());
    QCOMPARE(sheet->property("mode").toString(), QStringLiteral("smart"));
    QQuickItem* nameInput = item("collectionNameInput");
    QTRY_VERIFY(nameInput->hasActiveFocus());
    typeText(QStringLiteral("Alpine pictures"));
    QCOMPARE(nameInput->property("text").toString(), QStringLiteral("Alpine pictures"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(!sheet->isVisible() ||
                                 !sheet->property("errorMessage").toString().isEmpty(),
                             5000);
    QVERIFY2(!sheet->isVisible(), qPrintable(sheet->property("errorMessage").toString()));
    QVERIFY(m_settings->smartCollectionNames().contains(QStringLiteral("Alpine pictures")));
    QCOMPARE(m_library->smartCollectionFilter(), QStringLiteral("Alpine pictures"));
    QCOMPARE(m_library->searchText(), QStringLiteral("alpine"));
    QCOMPARE(m_library->sortMode(), CaptureFilterModel::NameAscending);
    QCOMPARE(m_library->count(), 1);

    m_settings->deleteSmartCollection(QStringLiteral("Alpine pictures"));
    m_library->setSearchText({});
    m_library->setSortMode(CaptureFilterModel::NewestFirst);
    m_library->clearSmartCollection();
  }

  void selectionKeysActOnTheWholeSelection() {
    QQuickItem* grid = item("library");
    QTest::keyClick(m_window, Qt::Key_X);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(grid->property("currentIndex").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_X);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 2);

    QTest::keyClick(m_window, Qt::Key_V);
    QTRY_VERIFY(m_settings->isFavorite(pathAt(0)) && m_settings->isFavorite(pathAt(1)));
    QTest::keyClick(m_window, Qt::Key_V);
    QTRY_VERIFY(!m_settings->isFavorite(pathAt(0)) && !m_settings->isFavorite(pathAt(1)));

    QTest::keyClick(m_window, Qt::Key_Delete);
    QQuickItem* confirm = item("confirm");
    QTRY_VERIFY(confirm->isVisible());
    QVERIFY(confirm->property("title").toString().contains(QStringLiteral("2 items")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!confirm->isVisible());
    QCOMPARE(grid->property("checkedCount").toInt(), 2);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);
  }

  void viewerArrowsMoveThroughTheLibrary() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(detail->property("path").toString(), pathAt(1));
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(detail->property("path").toString(), pathAt(0));
    QTest::keyClick(m_window, Qt::Key_Left);
    const int last = m_library->rowCount() - 1;
    QTRY_COMPARE(detail->property("path").toString(), pathAt(last));
    QCOMPARE(item("library")->property("currentIndex").toInt(), last);
  }

  void viewerActionsHaveCompleteKeyboardFocusAndAccurateHelp() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QVERIFY(detail->hasActiveFocus());

    QTest::keyClick(m_window, Qt::Key_Tab);
    QQuickItem* focused = activeViewerAction();
    QTRY_VERIFY(focused != nullptr);
    QCOMPARE(focused->objectName(), QStringLiteral("viewerAction_matte"));
    QVERIFY(focused->property("usable").toBool());
    QCOMPARE(focused->property("shortcut").toString(),
             m_registry->shortcutFor(QStringLiteral("matte")));
    QCOMPARE(focused->property("toolTipText").toString(), QStringLiteral("Make it postable  ·  M"));

    QTest::keyClick(m_window, Qt::Key_Down);
    QQuickItem* next = activeViewerAction();
    QTRY_VERIFY(next != nullptr);
    QVERIFY(next != focused);
    QVERIFY(next->property("usable").toBool());
    QTest::keyClick(m_window, Qt::Key_Up);
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_matte"));

    QTest::keyClick(m_window, Qt::Key_Tab, Qt::ShiftModifier);
    QTRY_VERIFY(detail->hasActiveFocus());
    QVERIFY(!detail->property("actionNavigationActive").toBool());

    // Focus stays on the same common action while Left and Right retain their
    // viewer meaning and the delegate model is rebuilt for another file.
    QTest::keyClick(m_window, Qt::Key_Tab);
    for (int guard = 0;
         guard < 20 && (!activeViewerAction() ||
                        activeViewerAction()->objectName() != QStringLiteral("viewerAction_copy"));
         ++guard) {
      QTest::keyClick(m_window, Qt::Key_Down);
    }
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));
    const QString before = detail->property("path").toString();
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_VERIFY(detail->property("path").toString() != before);
    QTRY_VERIFY(detail->property("actionNavigationActive").toBool());
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));

    // Return and Space activate the focused row rather than the viewer's
    // default action. The first still action opens the matte sheet.
    openDetail(0);
    QTest::qWait(30);
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));
    QMetaObject::invokeMethod(detail, "focusPreview");
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_matte"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(item("matteSheet")->isVisible());
    QVERIFY(!detail->isVisible());
  }

  void shortcutTooltipsUseTheLiveShortcutValues() {
    QQuickItem* all = pill(m_window->contentItem(), QStringLiteral("All"));
    QVERIFY(all);
    QCOMPARE(all->property("shortcut").toString(), QStringLiteral("1"));
    QCOMPARE(all->property("resolvedToolTip").toString(), QStringLiteral("All  ·  1"));

    openDetail(0);
    QQuickItem* rotate = pill(item("detail"), QStringLiteral("Rotate"));
    QTRY_VERIFY(rotate != nullptr);
    QCOMPARE(rotate->property("shortcut").toString(), QStringLiteral("R"));
    QCOMPARE(rotate->property("resolvedToolTip").toString(), QStringLiteral("Rotate  ·  R"));
  }

  void extractedTextOpensASelectableLayoutPreservingReview() {
    const QString path = pathAt(0);
    QVERIFY(!m_library->isVideoAt(0));
    QVERIFY(m_registry->appliesTo(QStringLiteral("ocr"), false));
    QVERIFY(m_textIndex->available());
    const QFileInfo before(path);
    perform(QStringLiteral("ocr"), path);
    QQuickItem* sheet = item("textReviewSheet");
    QTRY_VERIFY(sheet->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(m_textIndex->reviewPath(), path, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_textIndex->reviewing(), 3000);
    QCOMPARE(m_textIndex->reviewError(), QString());
    QCOMPARE(m_textIndex->reviewText(), QStringLiteral("First  line\nSecond line"));

    QQuickItem* editor = item("ocrTextArea");
    QTRY_COMPARE(editor->property("text").toString(), QStringLiteral("First  line\nSecond line"));
    QTRY_VERIFY(editor->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5)));
    QTRY_COMPARE(editor->property("selectedText").toString(), QStringLiteral("First"));
    click(item("copyOcrSelection"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("First"));
    QCOMPARE(item("ocrCopyStatus")->property("text").toString(), QStringLiteral("Copied selection"));
    QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("First"));

    editor->setProperty("text", QStringLiteral("Corrected locally"));
    click(item("copyAllOcrText"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Corrected locally"));
    QCOMPARE(item("ocrCopyStatus")->property("text").toString(), QStringLiteral("Copied all text"));
    QCOMPARE(QFileInfo(path).size(), before.size());
    QCOMPARE(QFileInfo(path).lastModified(), before.lastModified());

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!sheet->isVisible());
    QVERIFY(m_textIndex->reviewPath().isEmpty());
    QCOMPARE(item("ocrCopyStatus")->property("text").toString(), QString());
  }

  void qrActionAppearsOnlyAfterDetectionAndCopiesThroughTheSecurePath() {
    openDetail(0);
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QTRY_COMPARE(m_qr->path(), detail->property("path").toString());
    QTRY_VERIFY_WITH_TIMEOUT(!m_qr->checking(), 3000);
    QVERIFY(!m_qr->detected());
    QVERIFY(find(detail, [](QQuickItem* candidate) {
              return candidate->objectName() == QStringLiteral("viewerAction_qr");
            }) == nullptr);
    QMetaObject::invokeMethod(detail, "close");
    QTRY_VERIFY(!detail->isVisible());

    const QString qrPath =
        QFileInfo(pathAt(0)).dir().filePath(QStringLiteral("qr-positive-ui.png"));
    QImage image(19, 17, QImage::Format_RGB32);
    image.fill(Qt::white);
    QVERIFY(image.save(qrPath, "PNG"));
    m_captures->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(qrPath) >= 0, 5000);
    openDetail(m_library->rowOf(qrPath));
    QTRY_COMPARE(m_qr->path(), qrPath);
    QTRY_VERIFY_WITH_TIMEOUT(!m_qr->checking(), 3000);
    QVERIFY(m_qr->detected());

    // Inserting or removing the asynchronous QR row must not move keyboard
    // focus onto a different action farther down the list.
    QVERIFY(QMetaObject::invokeMethod(detail, "focusActionById",
                                      Q_ARG(QVariant, QStringLiteral("rename"))));
    QTRY_VERIFY(find(detail, [](QQuickItem* candidate) {
                  return candidate->objectName() == QStringLiteral("viewerAction_rename") &&
                         candidate->hasActiveFocus();
                }) != nullptr);
    m_qr->inspect(pathAt(0));
    QTRY_VERIFY_WITH_TIMEOUT(!m_qr->checking(), 3000);
    QTRY_VERIFY(find(detail, [](QQuickItem* candidate) {
                  return candidate->objectName() == QStringLiteral("viewerAction_rename") &&
                         candidate->hasActiveFocus();
                }) != nullptr);
    m_qr->inspect(qrPath);
    QTRY_VERIFY(m_qr->detected());
    QTRY_VERIFY(find(detail, [](QQuickItem* candidate) {
                  return candidate->objectName() == QStringLiteral("viewerAction_rename") &&
                         candidate->hasActiveFocus();
                }) != nullptr);

    QQuickItem* qrAction = nullptr;
    QTRY_VERIFY((qrAction = find(detail, [](QQuickItem* candidate) {
                   return candidate->objectName() == QStringLiteral("viewerAction_qr") &&
                          candidate->isVisible();
                 })) != nullptr);
    QSignalSpy reported(m_actions, &ActionLauncher::reported);
    click(qrAction);
    QTRY_VERIFY_WITH_TIMEOUT(!reported.isEmpty(), 3000);
    QCOMPARE(reported.last().first().toString(), QStringLiteral("QR code copied to clipboard"));
    QFile clipboard(m_scratch.filePath(QStringLiteral("clipboard.log")));
    QVERIFY(clipboard.open(QIODevice::ReadOnly));
    QCOMPARE(clipboard.readAll(), QByteArray("otpauth://ui-private-value"));
    QVERIFY(detail->isVisible());

    QMetaObject::invokeMethod(detail, "close");
    QVERIFY(QFile::remove(qrPath));
    m_captures->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(qrPath) < 0, 5000);
  }

  void viewerShowsUsefulMediaMetadata() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("stillReady").toBool(), 10000);
    QQuickItem* metadata = item("mediaMetadata");
    QTRY_VERIFY(metadata->isVisible());
    const QString label = metadata->property("text").toString();
    QVERIFY2(label.contains(QStringLiteral(" × ")), qPrintable(label));
    QVERIFY(detail->property("mediaWidth").toInt() > 0);
    QVERIFY(detail->property("mediaHeight").toInt() > 0);
    QTRY_COMPARE_WITH_TIMEOUT(m_mediaInfo->path(), detail->property("path").toString(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mediaInfo->loading(), 5000);
    QVERIFY(!m_mediaInfo->lines().isEmpty());
    QVERIFY(m_mediaInfo->lines().first().startsWith(QStringLiteral("JPEG")));
  }

  void viewerDateFollowsBackgroundMetadataEnrichment() {
    QQuickItem* detail = item("detail");
    const QString path = pathAt(0);
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());

    const int sourceRow = m_captures->rowOf(path);
    QVERIFY(sourceRow >= 0);
    const CaptureRecord record = m_captures->recordAt(sourceRow);
    QVERIFY(!record.hasProducerTimestamp);
    const QDateTime enriched = record.captured.addSecs(-61);
    m_captures->applyCapturedDates(
        {{path, record.modified, record.bytes, enriched, record.device, record.inode}});
    QTRY_COMPARE(detail->property("dayLabel").toString(),
                 m_captures->dayLabelAt(m_captures->rowOf(path)));
    QTRY_COMPARE(detail->property("timeLabel").toString(),
                 m_library->timeLabelAt(m_library->rowOf(path)));

    m_captures->applyCapturedDates(
        {{path, record.modified, record.bytes, record.captured, record.device, record.inode}});
    QTRY_COMPARE(detail->property("timeLabel").toString(),
                 m_library->timeLabelAt(m_library->rowOf(path)));
  }

  void backgroundDateReorderKeepsTheSelectedFile() {
    int row = -1;
    for (int candidate = 1; candidate < m_library->rowCount(); ++candidate) {
      const int sourceRow = m_captures->rowOf(pathAt(candidate));
      if (sourceRow >= 0 && !m_captures->recordAt(sourceRow).hasProducerTimestamp) {
        row = candidate;
        break;
      }
    }
    QVERIFY(row > 0);

    QQuickItem* grid = item("library");
    const QString selectedPath = pathAt(row);
    grid->setProperty("currentIndex", row);
    QCOMPARE(grid->property("currentIndex").toInt(), row);
    QSignalSpy reordered(m_library, &QAbstractItemModel::layoutChanged);
    const int sourceRow = m_captures->rowOf(selectedPath);
    const CaptureRecord record = m_captures->recordAt(sourceRow);
    const QDateTime moved = QDateTime::currentDateTime().addDays(1);
    m_captures->applyCapturedDates(
        {{selectedPath, record.modified, record.bytes, moved, record.device, record.inode}});

    QTRY_VERIFY(!reordered.isEmpty());
    QTRY_COMPARE(m_library->rowOf(selectedPath), 0);
    QTRY_COMPARE(pathAt(grid->property("currentIndex").toInt()), selectedPath);

    m_captures->applyCapturedDates({{selectedPath, record.modified, record.bytes, record.captured,
                                     record.device, record.inode}});
    QTRY_COMPARE(pathAt(grid->property("currentIndex").toInt()), selectedPath);
  }

  void videoViewerShowsCodecMetadataEndToEnd() {
    int videoRow = -1;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) {
        videoRow = row;
        break;
      }
    }
    QVERIFY(videoRow >= 0);
    openDetail(videoRow);
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(m_mediaInfo->path(), detail->property("path").toString(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mediaInfo->loading(), 5000);
    QVERIFY(m_mediaInfo->lines().size() >= 2);
    QVERIFY(m_mediaInfo->lines().at(0).startsWith(QStringLiteral("MP4  ·  H.264")));
    QVERIFY(m_mediaInfo->lines().at(1).contains(QStringLiteral("fps")));
  }

  void videoFrameTimestampsAreFilenameSafeAndPrecise() {
    const auto stamp = [this](double milliseconds) {
      QVariant result;
      const bool called = QMetaObject::invokeMethod(
          m_window, "frameStamp", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, milliseconds));
      return called ? result.toString() : QString();
    };
    QCOMPARE(stamp(0), QStringLiteral("00m00s000"));
    QCOMPARE(stamp(5'123), QStringLiteral("00m05s123"));
    QCOMPARE(stamp(3'723'045), QStringLiteral("01h02m03s045"));
  }

  void videoViewerSavesItsCurrentFrameEndToEnd() {
    int videoRow = -1;
    QString picture;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row) && videoRow < 0) {
        videoRow = row;
      } else if (!m_library->isVideoAt(row) && picture.isEmpty()) {
        picture = pathAt(row);
      }
    }
    QVERIFY(videoRow >= 0);
    QVERIFY(!picture.isEmpty());

    const QString bin = m_scratch.filePath(QStringLiteral("frame-bin"));
    QVERIFY(QDir().mkpath(bin));
    const QString logPath = m_scratch.filePath(QStringLiteral("frame-args.log"));
    QFile handler(QDir(bin).filePath(QStringLiteral("ffmpeg")));
    QVERIFY(handler.open(QIODevice::WriteOnly));
    handler.write("#!/bin/sh\n"
                  "printf '%s\\n' \"$@\" > \"$OMAROLL_FRAME_LOG\"\n"
                  "for last do :; done\n"
                  "cp \"$OMAROLL_FRAME_SOURCE\" \"$last\"\n");
    handler.close();
    QVERIFY(handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));

    const QByteArray oldPath = qgetenv("PATH");
    const QByteArray oldLog = qgetenv("OMAROLL_FRAME_LOG");
    const QByteArray oldSource = qgetenv("OMAROLL_FRAME_SOURCE");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", oldPath);
      putBack("OMAROLL_FRAME_LOG", oldLog);
      putBack("OMAROLL_FRAME_SOURCE", oldSource);
    });
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + oldPath));
    QVERIFY(qputenv("OMAROLL_FRAME_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("OMAROLL_FRAME_SOURCE", picture.toUtf8()));

    const QString video = pathAt(videoRow);
    QSignalSpy pending(m_actions, &ActionLauncher::outputPending);
    QSignalSpy settled(m_actions, &ActionLauncher::outputSettled);
    openDetail(videoRow);
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QVERIFY(detail->property("isVideo").toBool());
    QTest::keyClick(m_window, Qt::Key_G);
    QTRY_COMPARE_WITH_TIMEOUT(pending.size(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);

    const QString output = settled.first().at(0).toString();
    QVERIFY(settled.first().at(1).toBool());
    QVERIFY(output.startsWith(QFileInfo(video).absolutePath() + QLatin1Char('/')));
    QVERIFY(output.contains(QStringLiteral("-frame-")));
    QVERIFY(output.endsWith(QStringLiteral(".png")));
    QVERIFY(!QImage(output).isNull());
    QVERIFY(QFileInfo(video).exists());
    QVERIFY(detail->isVisible());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QStringList arguments =
        QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const int seek = arguments.indexOf(QStringLiteral("-ss"));
    QVERIFY(seek >= 0 && seek + 1 < arguments.size());
    QVERIFY(arguments.at(seek + 1).toDouble() >= 0.0);
    QCOMPARE(arguments.at(arguments.indexOf(QStringLiteral("-i")) + 1), video);
    QCOMPARE(arguments.last(), output);
  }

  void exportSheetOffersImageAndVideoPresets() {
    QQuickItem* sheet = item("exportSheet");
    perform(QStringLiteral("export"), pathAt(0));
    QTRY_VERIFY(sheet->isVisible());
    QVERIFY(!sheet->property("isVideo").toBool());
    QCOMPARE(sheet->property("format").toString(), QStringLiteral("jpg"));
    QCOMPARE(sheet->property("resolution").toString(), QStringLiteral("medium"));
    click(pill(sheet, QStringLiteral("PNG")));
    click(pill(sheet, QStringLiteral("Low · 1080")));
    QCOMPARE(sheet->property("format").toString(), QStringLiteral("png"));
    QCOMPARE(sheet->property("resolution").toString(), QStringLiteral("low"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!sheet->isVisible());

    int videoRow = -1;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) {
        videoRow = row;
        break;
      }
    }
    QVERIFY(videoRow >= 0);
    perform(QStringLiteral("export"), pathAt(videoRow));
    QTRY_VERIFY(sheet->isVisible());
    QVERIFY(sheet->property("isVideo").toBool());
    QCOMPARE(sheet->property("format").toString(), QStringLiteral("mp4"));
    QCOMPARE(sheet->property("resolution").toString(), QStringLiteral("1080p"));
    click(pill(sheet, QStringLiteral("GIF")));
    click(pill(sheet, QStringLiteral("720p")));
    QCOMPARE(sheet->property("format").toString(), QStringLiteral("gif"));
    QCOMPARE(sheet->property("resolution").toString(), QStringLiteral("720p"));
  }

  void renameSheetKeepsTheExtensionAndRenamesEndToEnd() {
    int stillRow = -1;
    const QString odd = QFileInfo(m_oddPath).canonicalFilePath();
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (!m_library->isVideoAt(row) && pathAt(row) != odd) {
        stillRow = row;
        break;
      }
    }
    QVERIFY(stillRow >= 0);
    const QString path = pathAt(stillRow);
    perform(QStringLiteral("rename"), path);
    QQuickItem* sheet = item("renameSheet");
    QTRY_VERIFY(sheet->isVisible());
    QCOMPARE(sheet->property("path").toString(), path);
    QCOMPARE(sheet->property("suffix").toString(), QStringLiteral(".") + QFileInfo(path).suffix());

    QQuickItem* input = item("renameInput");
    input->setProperty("text", QString());
    click(pill(sheet, QStringLiteral("Rename")));
    QVERIFY(sheet->isVisible());
    QVERIFY(!sheet->property("errorMessage").toString().isEmpty());

    const QString base = QStringLiteral("renamed through sheet");
    input->setProperty("text", base);
    click(pill(sheet, QStringLiteral("Rename")));
    QTRY_VERIFY(!sheet->isVisible());
    const QString renamed =
        QFileInfo(path).dir().filePath(base + QStringLiteral(".") + QFileInfo(path).suffix());
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(QFileInfo::exists(renamed));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(renamed) >= 0, 5000);
    QTRY_COMPARE(item("library")->property("currentIndex").toInt(), m_library->rowOf(renamed));
    QTRY_VERIFY(item("library")->hasActiveFocus());

    perform(QStringLiteral("rename"), renamed);
    QTRY_VERIFY(sheet->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!sheet->isVisible());
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  void slideshowStartsFullscreenAndEscapeEndsIt() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QTest::keyClick(m_window, Qt::Key_F5);
    QTRY_VERIFY(detail->property("slideshowRunning").toBool());
    QVERIFY(detail->property("fullScreen").toBool());
    QVERIFY(!detail->property("showInfo").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!detail->property("slideshowRunning").toBool());
    QVERIFY(!detail->property("fullScreen").toBool());
    QVERIFY(detail->isVisible());
  }

  void hideFromTheViewerClosesItAndHidesTheFile() {
    QQuickItem* detail = item("detail");
    const QString path = pathAt(1);
    openDetail(1);
    QTRY_VERIFY(detail->isVisible());
    QTest::keyClick(m_window, Qt::Key_H, Qt::ControlModifier);
    QTRY_VERIFY(!detail->isVisible());
    QVERIFY(m_settings->isHidden(path));
    QTRY_VERIFY(m_library->rowOf(path) < 0);
    m_settings->toggleHidden(path);
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
  }

  void oddFilenamesGetThumbnailsAndOpen() {
    const QString odd = QFileInfo(m_oddPath).canonicalFilePath();
    QVERIFY(m_library->rowOf(odd) >= 0);
    QQuickItem* card = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((card = cardFor(odd)) != nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(card->property("thumbnailReady").toBool(), 15000);
    QQuickItem* plain = cardFor(pathAt(1));
    QVERIFY(plain);
    QTRY_VERIFY_WITH_TIMEOUT(plain->property("thumbnailReady").toBool(), 15000);

    QQuickItem* detail = item("detail");
    openDetail(m_library->rowOf(odd));
    QTRY_VERIFY(detail->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("stillReady").toBool(), 10000);
    QVERIFY2(detail->property("playbackError").toString().isEmpty(),
             qPrintable(detail->property("playbackError").toString()));
    invoke("dismissTopLayer");

    perform(QStringLiteral("matte"), odd);
    QTRY_VERIFY(item("matteSheet")->isVisible());
    QQuickItem* preview = item("mattePreview");
    // Image.Ready
    QTRY_COMPARE_WITH_TIMEOUT(preview->property("status").toInt(), 1, 15000);
  }

  void pdfPreviewPagesInPlaceAndOffersDocumentActions() {
    {
      QPdfWriter writer(m_pdfPath);
      writer.setResolution(96);
      QPainter painter(&writer);
      QVERIFY(painter.isActive());
      painter.drawText(QPoint(100, 140), QStringLiteral("Omaroll guide"));
      QVERIFY(writer.newPage());
      painter.drawText(QPoint(100, 140), QStringLiteral("Second page"));
      painter.end();
    }
    m_captures->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(m_pdfPath) >= 0, 5000);
    const int row = m_library->rowOf(m_pdfPath);
    QVERIFY(row >= 0);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPath", Q_ARG(QVariant, m_pdfPath)));
    const auto restoreFilter = qScopeGuard([&] { m_library->setFolderFilter(QString()); });
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), m_pdfPath);
    QVERIFY(detail->property("isDocument").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(m_pdfInfo->path(), m_pdfPath, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_pdfInfo->loading(), 5000);
    QCOMPARE(m_pdfInfo->pageCount(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("stillReady").toBool(), 10000);
    QVERIFY(find(detail, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("viewerAction_open-document");
    }));
    QCOMPARE(detail->property("pdfPage").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTRY_COMPARE(detail->property("pdfPage").toInt(), 2);
    invoke("dismissTopLayer");
    QVERIFY(QFile::remove(m_pdfPath));
    m_captures->refresh();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowOf(m_pdfPath), -1, 5000);
  }

  void albumFromASelection() {
    QTest::keyClick(m_window, Qt::Key_X);
    QTRY_COMPARE(item("library")->property("checkedCount").toInt(), 1);
    // The header row re-lays itself out in the next polish pass once the
    // selection pills appear; click only once the pill has its place.
    QQuickItem* albumPill = pill(m_window->contentItem(), QStringLiteral("Organize"));
    QVERIFY(albumPill);
    QTRY_VERIFY(centre(albumPill).x() > 0 && centre(albumPill).x() < m_window->width());
    QTest::qWait(120);
    albumPill = pill(m_window->contentItem(), QStringLiteral("Organize"));
    QVERIFY(albumPill);
    QTRY_VERIFY(centre(albumPill).x() > 0 && centre(albumPill).x() < m_window->width());
    click(albumPill);
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("albumActionMenu"));
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QQuickItem* newAlbum = find(m_window->contentItem(), [](QQuickItem* candidate) {
      return candidate->inherits("QQuickText") && candidate->isVisible() &&
             candidate->property("text").toString() == QStringLiteral("+ New album");
    });
    QVERIFY(newAlbum);
    click(newAlbum);
    QQuickItem* sheet = item("albumNameSheet");
    QTRY_VERIFY(sheet->isVisible());
    typeText(QStringLiteral("Trip"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!sheet->isVisible());
    QVERIFY(m_settings->albumNames().contains(QStringLiteral("Trip")));
    QCOMPARE(m_settings->albumPaths(QStringLiteral("Trip")).size(), 1);
    QCOMPARE(m_settings->albumPaths(QStringLiteral("Trip")).first(), pathAt(0));
    m_settings->deleteAlbum(QStringLiteral("Trip"));
  }

  void tagFromASelection() {
    const QString selected = pathAt(0);
    QTest::keyClick(m_window, Qt::Key_X);
    QTRY_COMPARE(item("library")->property("checkedCount").toInt(), 1);
    QQuickItem* organize = pill(m_window->contentItem(), QStringLiteral("Organize"));
    QVERIFY(organize);
    QTest::qWait(120);
    organize = pill(m_window->contentItem(), QStringLiteral("Organize"));
    click(organize);
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("albumActionMenu"));
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QQuickItem* newTag = find(m_window->contentItem(), [](QQuickItem* candidate) {
      return candidate->inherits("QQuickText") && candidate->isVisible() &&
             candidate->property("text").toString() == QStringLiteral("+ New tag");
    });
    QVERIFY(newTag);
    click(newTag);
    QQuickItem* sheet = item("albumNameSheet");
    QTRY_VERIFY(sheet->isVisible());
    QCOMPARE(sheet->property("mode").toString(), QStringLiteral("tag"));
    typeText(QStringLiteral("UI Review"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!sheet->isVisible());
    QVERIFY(m_settings->tagNames().contains(QStringLiteral("UI Review")));
    QCOMPARE(m_settings->tagPaths(QStringLiteral("UI Review")), QStringList{selected});
    m_settings->deleteTag(QStringLiteral("UI Review"));
    QMetaObject::invokeMethod(item("library"), "clearChecked");
  }

  void keepingOneExactDuplicateMovesOnlyTheOtherCopyToTrash() {
    QVERIFY(QFileInfo::exists(m_oddPath));
    m_library->setDuplicatesOnly(true);
    QTRY_VERIFY_WITH_TIMEOUT(!m_duplicates->scanning(), 15000);
    const QStringList matchingOriginals = m_duplicates->otherCopies(m_oddPath);
    QVERIFY(!matchingOriginals.isEmpty());
    const QString kept = matchingOriginals.first();
    QVERIFY(QFileInfo::exists(kept));
    const QStringList trashed = m_duplicates->otherCopies(kept);
    QVERIFY(trashed.contains(m_oddPath));
    for (const QString& path : trashed) {
      QVERIFY(QFileInfo::exists(path));
    }
    const int keptRow = m_library->rowOf(kept);
    QVERIFY(keptRow >= 0);
    item("library")->setProperty("currentIndex", keptRow);
    click(pill(m_window->contentItem(), QStringLiteral("Keep selected")));
    QTRY_VERIFY(item("confirm")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!item("confirm")->isVisible());
    for (const QString& path : trashed) {
      QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 5000);
      QTRY_VERIFY_WITH_TIMEOUT(m_captures->rowOf(path) < 0, 10000);
    }
    QVERIFY(QFileInfo::exists(kept));
    QTRY_COMPARE_WITH_TIMEOUT(m_duplicates->groupCount(), 0, 10000);
    m_library->setDuplicatesOnly(false);
    QVERIFY(m_library->rowOf(kept) >= 0);
  }

  void trashMovesTheFileAndTheGridFollows() {
    QQuickItem* grid = item("library");
    const int last = m_library->rowCount() - 1;
    const QString path = pathAt(last);
    QVERIFY(QFileInfo::exists(path));
    grid->setProperty("currentIndex", last);
    QTest::keyClick(m_window, Qt::Key_Delete);
    QQuickItem* confirm = item("confirm");
    QTRY_VERIFY(confirm->isVisible());
    QCOMPARE(confirm->property("detail").toString(), path);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!confirm->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(path) < 0, 10000);
    QTRY_VERIFY(grid->property("currentIndex").toInt() < m_library->rowCount());
  }

  void videoRenderKeepsADecodedFrameVisible() {
    QVERIFY(QMetaObject::invokeMethod(m_window, "openViewForRender",
                                      Q_ARG(QVariant, QStringLiteral("video"))));
    auto* player = m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer"));
    QVERIFY(player);
    QTRY_VERIFY(player->position() >= 1000);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QVERIFY(player->videoSink());
    QVERIFY(player->videoSink()->videoFrame().isValid());
    QVERIFY(!player->videoSink()->videoFrame().toImage().isNull());
  }

  // Fixture copies live until demo teardown so thumbnail workers can finish.
  void realAnimationsPauseResumeAndKeepTheirFrame_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("gif") << QStringLiteral("animated.gif");
    QTest::newRow("webp") << QStringLiteral("animated.webp");
  }

  void realAnimationsPauseResumeAndKeepTheirFrame() {
    QFETCH(QString, name);
    const QString source = QFINDTESTDATA(qPrintable("fixtures/viewer/" + name));
    QVERIFY(!source.isEmpty());
    const QString path = QFileInfo(m_oddPath).dir().filePath(name);
    QVERIFY(QFile::copy(source, path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
    });
    m_captures->refresh();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    openDetail(m_library->rowOf(path));
    QQuickItem* animation = item("animatedImage");
    QTRY_COMPARE(animation->property("status").toInt(), 1);
    QCOMPARE(animation->property("frameCount").toInt(), 12);
    QTRY_VERIFY(animation->property("currentFrame").toInt() >= 3);
    const int frame = animation->property("currentFrame").toInt();
    QTest::keyClick(m_window, Qt::Key_Space);
    QCOMPARE(animation->property("currentFrame").toInt(), frame);
    QSignalSpy frameChanges(animation, SIGNAL(currentFrameChanged()));
    QTest::qWait(400);
    QCOMPARE(frameChanges.size(), 0);
    QCOMPARE(animation->property("currentFrame").toInt(), frame);
    const QImage paused = m_window->grabWindow();
    QVERIFY(!paused.isNull());
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(frameChanges.size() >= 3);
    QVERIFY(m_window->grabWindow() != paused);
    // Keep decoding past the end of the first cycle, including disposal and loop handling.
    frameChanges.clear();
    QTRY_VERIFY_WITH_TIMEOUT(frameChanges.size() >= 15, 4000);
    QVERIFY(item("detail")->property("playbackError").toString().isEmpty());
  }

  void stillWebpDisplaysWithoutAnimationErrors() {
    const QString source = QFINDTESTDATA("fixtures/viewer/still.webp");
    QVERIFY(!source.isEmpty());
    const QString path = QFileInfo(m_oddPath).dir().filePath(QStringLiteral("still.webp"));
    QVERIFY(QFile::copy(source, path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
    });
    m_captures->refresh();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    openDetail(m_library->rowOf(path));
    QTRY_COMPARE(item("animatedImage")->property("status").toInt(), 1);
    QCOMPARE(item("animatedImage")->property("frameCount").toInt(), 1);
    QVERIFY(item("detail")->property("playbackError").toString().isEmpty());
  }

  void videoTracksSeekingAndMinimizeRestore() {
    const QString source = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(!source.isEmpty());
    const QString path = QFileInfo(m_oddPath).dir().filePath(QStringLiteral("tracks.mkv"));
    QVERIFY(QFile::copy(source, path));
    const auto cleanup = qScopeGuard([&] {
      m_window->showNormal();
      m_window->resize(1280, 820);
      invoke("dismissTopLayer");
    });
    m_captures->refresh();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    openDetail(m_library->rowOf(path));
    auto* player = m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer"));
    QVERIFY(player);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    QTRY_COMPARE(player->audioTracks().size(), 2);
    QTRY_COMPARE(player->subtitleTracks().size(), 1);
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      QCOMPARE(QQuickWindow::graphicsApi(), QSGRendererInterface::OpenGL);
      // A decoded frame can exist while VideoOutput renders black. Sample
      // the rendered test pattern, whose saturated bars distinguish it from
      // the dark viewer background and transport controls.
      const auto renderedVideoHasColor = [&] {
        QQuickItem* output = item("videoOutput");
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
    QAudioFormat audioFormat;
    audioFormat.setSampleRate(48000);
    audioFormat.setChannelCount(1);
    audioFormat.setSampleFormat(QAudioFormat::Float);
    QAudioBufferOutput decodedAudio(audioFormat);
    double frequency = 0;
    connect(&decodedAudio, &QAudioBufferOutput::audioBufferReceived, &decodedAudio,
            [&](const QAudioBuffer& buffer) {
      if (buffer.frameCount() < 500 || buffer.format().sampleFormat() != QAudioFormat::Float)
        return;
      const float* samples = buffer.constData<float>();
      int crossings = 0;
      for (int i = 1; i < buffer.frameCount(); ++i)
        if (samples[i - 1] <= 0 && samples[i] > 0) ++crossings;
      frequency = double(crossings) * buffer.format().sampleRate() / buffer.frameCount();
    });
    player->setAudioBufferOutput(&decodedAudio);
    const auto detachAudio = qScopeGuard([&] { player->setAudioBufferOutput(nullptr); });
    QTRY_VERIFY(frequency > 350 && frequency < 530);
    const int audioTrack = player->activeAudioTrack();
    click(pill(item("detail"), QStringLiteral("Switch audio track")));
    QTRY_COMPARE(player->activeAudioTrack(), (audioTrack + 1) % 2);
    QTRY_VERIFY(frequency > 760 && frequency < 1000);
    const int subtitleTrack = player->activeSubtitleTrack();
    click(item("subtitleButton"));
    QTRY_COMPARE(player->activeSubtitleTrack(), subtitleTrack == -1 ? 0 : -1);
    click(item("subtitleButton"));
    QTRY_COMPARE(player->activeSubtitleTrack(), subtitleTrack);
    if (player->activeSubtitleTrack() < 0) click(item("subtitleButton"));
    QVERIFY(player->videoSink());
    player->setPosition(1000);
    QTRY_VERIFY(player->videoSink()->subtitleText().contains(QStringLiteral("Omaroll subtitle test")));
    player->setPosition(7000);
    QTRY_VERIFY(player->videoSink()->subtitleText().contains(QStringLiteral("Second subtitle")));
    click(item("subtitleButton"));
    QTRY_VERIFY(player->videoSink()->subtitleText().isEmpty());

    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QTest::keyClick(m_window, Qt::Key_Home);
    QTRY_COMPARE(player->position(), 0);
    QTest::keyClick(m_window, Qt::Key_L);
    QTRY_VERIFY(qAbs(player->position() - 5000) < 100);
    QTest::keyClick(m_window, Qt::Key_J);
    QTRY_COMPARE(player->position(), 0);
    m_window->showMinimized();
    QTest::qWait(100);
    m_window->showNormal();
    QTest::qWait(200);
    QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    m_window->showMinimized();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    m_window->showNormal();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    // Basic controls must remain within the narrow window.
    m_window->resize(560, 420);
    QTest::qWait(100);
    for (const QString& name : {QStringLiteral("videoPlayButton"),
                                QStringLiteral("videoSoundButton"),
                                QStringLiteral("playbackSpeedButton")}) {
      QQuickItem* control = item(name);
      QVERIFY(control->isVisible());
      const QRectF bounds = control->mapRectToScene(control->boundingRect());
      QVERIFY2(bounds.left() >= 0 && bounds.right() <= m_window->width(), qPrintable(name));
    }
    QVERIFY(item("detail")->property("playbackError").toString().isEmpty());
  }

  void theWindowRaisedNoQmlWarnings() {
    QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
  }

private:
  QVariant prop(const char* name) const { return m_window->property(name); }

  void invoke(const char* method) { QMetaObject::invokeMethod(m_window, method); }

  void perform(const QString& id, const QString& path) {
    QMetaObject::invokeMethod(m_window, "perform", Q_ARG(QVariant, id), Q_ARG(QVariant, path),
                              Q_ARG(QVariant, QVariant()));
  }

  void openDetail(int row) {
    QMetaObject::invokeMethod(m_window, "openDetail", Q_ARG(QVariant, row),
                              Q_ARG(QVariant, QVariant()));
  }

  QString pathAt(int row) const { return m_library->pathAt(row); }

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

  // A visible PillButton by its label or its compact-mode tooltip.
  static QQuickItem* pill(QQuickItem* within, const QString& label) {
    return find(within, [&label](QQuickItem* candidate) {
      return candidate->property("floating").isValid() &&
             (candidate->property("label").toString() == label ||
              candidate->property("toolTip").toString() == label) &&
             candidate->isVisible() && candidate->width() > 0;
    });
  }

  QQuickItem* cardFor(const QString& path) const {
    return find(m_window->contentItem(), [&path](QQuickItem* candidate) {
      return candidate->property("dragPaths").isValid() &&
             candidate->property("path").toString() == path && candidate->isVisible();
    });
  }

  QQuickItem* activeViewerAction() const {
    return find(m_window->contentItem(), [](QQuickItem* candidate) {
      return candidate->objectName().startsWith(QStringLiteral("viewerAction_")) &&
             candidate->hasActiveFocus();
    });
  }

  static QPoint centre(QQuickItem* target) {
    return target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
  }

  void click(QQuickItem* target, Qt::MouseButton button = Qt::LeftButton) {
    QVERIFY(target);
    QTest::mouseClick(m_window, button, Qt::NoModifier, centre(target));
    QTest::qWait(30);
  }

  void click(QQuickItem* target, Qt::MouseButton button, QPoint at) {
    QVERIFY(target);
    QTest::mouseClick(m_window, button, Qt::NoModifier, target->mapToScene(at).toPoint());
    QTest::qWait(30);
  }

  void typeText(const QString& text) {
    for (const QChar character : text) {
      QTest::keyClick(m_window, character.toLatin1());
    }
  }

  // singleTapped waits out the double-click interval before firing.
  static void settle() {
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
  }

  QTemporaryDir m_scratch;
  OmarchyTheme* m_theme = nullptr;
  AppSettings* m_settings = nullptr;
  CaptureModel* m_captures = nullptr;
  CaptureFilterModel* m_library = nullptr;
  ActionLauncher* m_actions = nullptr;
  ActionRegistry* m_registry = nullptr;
  TailscalePeers* m_tailscale = nullptr;
  MatteComposer* m_matte = nullptr;
  OcrIndex* m_textIndex = nullptr;
  QrDetector* m_qr = nullptr;
  DuplicateIndex* m_duplicates = nullptr;
  SimilarityIndex* m_similarities = nullptr;
  MediaInspector* m_mediaInfo = nullptr;
  PdfInspector* m_pdfInfo = nullptr;
  MediaDateIndex* m_mediaDates = nullptr;
  ThumbnailProvider* m_thumbnails = nullptr;
  MatteProvider* m_mattes = nullptr;
  PdfProvider* m_pdfs = nullptr;
  QQmlApplicationEngine* m_engine = nullptr;
  QQuickWindow* m_window = nullptr;
  QStringList m_warnings;
  QString m_oddPath;
  QString m_pdfPath;
};

// Offscreen unconditionally, not just under ctest. Run by hand on a live
// session this would open a real window on the desktop, be resized by the
// tiling compositor, and click the wrong things.
int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication application(argc, argv);
  UiTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}
#include "tst_ui.moc"
