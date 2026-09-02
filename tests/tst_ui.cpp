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
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailProvider.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QThreadPool>
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
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       layout.root + QStringLiteral("/config"));

    m_theme = new OmarchyTheme(m_scratch.filePath(QStringLiteral("state")),
                               m_scratch.filePath(QStringLiteral("config")), this);
    m_settings = new AppSettings(this);
    m_settings->setSlideshowVideos(true);
    m_captures = new CaptureModel(m_settings, this);
    m_library = new CaptureFilterModel(this);
    m_library->setSourceModel(m_captures);
    m_actions = new ActionLauncher(this);
    m_registry = new ActionRegistry(m_actions, this);
    m_tailscale = new TailscalePeers(this);
    m_matte = new MatteComposer(this);
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
    m_engine->addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), m_thumbnails);
    m_engine->addImageProvider(QLatin1String(MatteProvider::kProviderId), m_mattes);

    QQmlContext* context = m_engine->rootContext();
    context->setContextProperty(QStringLiteral("Theme"), m_theme);
    context->setContextProperty(QStringLiteral("Captures"), m_library);
    context->setContextProperty(QStringLiteral("Library"), m_captures);
    context->setContextProperty(QStringLiteral("Actions"), m_actions);
    context->setContextProperty(QStringLiteral("Settings"), m_settings);
    context->setContextProperty(QStringLiteral("Registry"), m_registry);
    context->setContextProperty(QStringLiteral("Matte"), m_matte);
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
    QQuickItem* rotate = pill(detail, QStringLiteral("Rotate"));
    QVERIFY(rotate);
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

  void rightClickOnATileOpensThatTile() {
    QQuickItem* card = cardFor(pathAt(1));
    QVERIFY(card);
    click(card, Qt::RightButton);
    settle();
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), pathAt(1));
  }

  void clickOutsideAMenuIsConsumed() {
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("libraryMenu"));
    QVERIFY(menu);
    QMetaObject::invokeMethod(menu, "open");
    QTRY_VERIFY(menu->property("visible").toBool());

    // Click a tile while the menu is open: the menu closes, the tile is not
    // touched.
    QQuickItem* card = cardFor(pathAt(2));
    QVERIFY(card);
    click(card);
    settle();
    QTRY_VERIFY(!menu->property("visible").toBool());
    QCOMPARE(item("library")->property("currentIndex").toInt(), 0);
    QVERIFY(!item("detail")->isVisible());
    QVERIFY(item("library")->isEnabled());
    // The arrow keys work again without a click on a tile first.
    QTRY_VERIFY(item("library")->hasActiveFocus());

    // The menu itself still takes clicks: choose a sort order from it.
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

  // A PillButton by its label, visible, inside the given item.
  static QQuickItem* pill(QQuickItem* within, const QString& label) {
    return find(within, [&label](QQuickItem* candidate) {
      return candidate->property("floating").isValid() &&
             candidate->property("label").toString() == label && candidate->isVisible() &&
             candidate->width() > 0;
    });
  }

  QQuickItem* cardFor(const QString& path) const {
    return find(m_window->contentItem(), [&path](QQuickItem* candidate) {
      return candidate->property("dragPaths").isValid() &&
             candidate->property("path").toString() == path && candidate->isVisible();
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

  // singleTapped waits out the double-click interval before firing.
  static void settle() { QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100); }

  QTemporaryDir m_scratch;
  OmarchyTheme* m_theme = nullptr;
  AppSettings* m_settings = nullptr;
  CaptureModel* m_captures = nullptr;
  CaptureFilterModel* m_library = nullptr;
  ActionLauncher* m_actions = nullptr;
  ActionRegistry* m_registry = nullptr;
  TailscalePeers* m_tailscale = nullptr;
  MatteComposer* m_matte = nullptr;
  ThumbnailProvider* m_thumbnails = nullptr;
  MatteProvider* m_mattes = nullptr;
  QQmlApplicationEngine* m_engine = nullptr;
  QQuickWindow* m_window = nullptr;
  QStringList m_warnings;
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
