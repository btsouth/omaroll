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
#include "actions/ExternalEditors.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/HeadlessAudio.h"
#include "app/MimeDefaults.h"
#include "app/VideoPlayback.h"
#include "library/CaptureFilterModel.h"
#include "library/SectionRows.h"
#include "library/FolderTreeModel.h"
#include "library/CaptureModel.h"
#include "library/DuplicateIndex.h"
#include "library/MediaMetadataIndex.h"
#include "library/CaptureRoles.h"
#include "library/MediaInspector.h"
#include "library/SimilarityIndex.h"
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "edit/EditProvider.h"
#include "edit/ImageEditor.h"
#include "pdf/PdfInspector.h"
#include "pdf/PdfProvider.h"
#include "pdf/PdfSupport.h"
#include "search/OcrIndex.h"
#include "search/SearchQuery.h"
#include "subtitles/SubtitleIndex.h"
#include "search/QrDetector.h"
#include "theme/OmarchyTheme.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"
#include "thumbs/LargeImageProvider.h"
#include "thumbs/RawImageProvider.h"
#include "thumbs/Storyboard.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/MprisService.h"

#include <QAccessible>
#include <QJSValue>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QClipboard>
#include <QDir>
#include <QJSValue>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QRegularExpression>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include <QMimeData>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPdfWriter>
#include <QPointingDevice>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTextStream>
#include <QVideoSink>
#include <QVideoFrame>
#include <QtQuickTest/quicktest.h>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

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

// Hold real QML Image requests open to check sizing, cache reuse and retained
// pixels without depending on decoder speed or filesystem caches.
class HeldThumbnailProvider final : public QQuickAsyncImageProvider {
public:
  struct Request {
    int row;
    int stamp;
    QString id;
    QSize size;
    std::atomic_bool cancelled{false};
    bool completed = false;
    QQuickImageResponse* response = nullptr;
  };

  class Response final : public QQuickImageResponse {
  public:
    explicit Response(std::shared_ptr<Request> request) : m_request(std::move(request)) {
      m_image = QImage(8, 8, QImage::Format_RGB32);
      m_image.fill((m_request->row + m_request->stamp) % 2 == 0 ? Qt::red : Qt::blue);
    }
    QQuickTextureFactory* textureFactory() const override {
      return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    void cancel() override { m_request->cancelled.store(true); }

  private:
    std::shared_ptr<Request> m_request;
    QImage m_image;
  };

  QQuickImageResponse* requestImageResponse(const QString& id, const QSize& size) override {
    auto request = std::make_shared<Request>();
    request->row = QUrl::fromPercentEncoding(id.toUtf8()).section('/', -1).section('.', 0, 0).toInt();
    request->stamp = id.section('~', 1, 1).section('!', 0, 0).section('%', 0, 0).toInt();
    request->id = id;
    request->size = size;
    request->response = new Response(request);
    QMutexLocker lock(&m_mutex);
    m_requests.append(request);
    return request->response;
  }

  QList<int> rows() const {
    QMutexLocker lock(&m_mutex);
    QList<int> result;
    for (const auto& request : m_requests) result.append(request->row);
    return result;
  }

  int count(int row, int stamp) const {
    QMutexLocker lock(&m_mutex);
    return std::count_if(m_requests.cbegin(), m_requests.cend(), [&](const auto& request) {
      return request->row == row && request->stamp == stamp;
    });
  }

  bool hasEmptySizeRequest() const {
    QMutexLocker lock(&m_mutex);
    return std::any_of(m_requests.cbegin(), m_requests.cend(), [](const auto& request) {
      return request->size.isEmpty();
    });
  }

  void dump() const {
    QMutexLocker lock(&m_mutex);
    for (const auto& request : m_requests) {
      qInfo() << "Held thumbnail state" << request->row << request->id << request->size
              << "cancelled" << request->cancelled.load() << "completed" << request->completed;
    }
  }

  void completeAll() {
    QMutexLocker lock(&m_mutex);
    for (const auto& request : m_requests) {
      if (request->completed) continue;
      request->completed = true;
      auto* response = request->response;
      // Like the production provider, complete on the response's own thread.
      QMetaObject::invokeMethod(response, [response] { emit response->finished(); }, Qt::QueuedConnection);
    }
  }

private:
  mutable QMutex m_mutex;
  QList<std::shared_ptr<Request>> m_requests;
};

class UiTest : public QObject {
  Q_OBJECT

private slots:
  // Organization that loads far smaller than today's backup is offered for
  // restore once, after the first frame, and Keep as is ends it.
  void incompleteOrganizationIsOfferedForRestoreOnce() {
    m_settings->flushBackups();
    const QDir backups(m_settings->backupDirectory());
    QVERIFY(QDir().mkpath(backups.path()));
    const QString today = backups.filePath(QStringLiteral("daily-%1.json")
                                               .arg(QDate::currentDate().toString(Qt::ISODate)));
    QJsonArray favorites;
    for (int i = 0; i < 1000; ++i) favorites.append(QStringLiteral("/gone/%1.jpg").arg(i));
    const QJsonObject snapshot{{QStringLiteral("format"), QStringLiteral("omaroll.organization")},
                               {QStringLiteral("version"), 1},
                               {QStringLiteral("favorites"), favorites}};
    {
      QFile file(today);
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      file.write(QJsonDocument(snapshot).toJson());
    }
    m_settings->recheckOrganizationHealth();
    m_settings->flushBackups();
    QVERIFY(!m_settings->launchWarning().isEmpty());
    QVERIFY(m_settings->canRestoreOrganization());
    QQuickItem* sheet = item("launchRestore");
    QTRY_VERIFY(sheet->isVisible());
    QVERIFY2(sheet->property("detail").toString().contains(QStringLiteral("1000 items")),
             qPrintable(sheet->property("detail").toString()));
    QQuickItem* keep = find(sheet, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("confirmSheetCancel");
    });
    QVERIFY(keep);
    QCOMPARE(keep->property("label").toString(), QStringLiteral("Keep as is"));
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                      keep->mapToScene(QPointF(keep->width() / 2, keep->height() / 2)).toPoint());
    QTRY_VERIFY(!sheet->isVisible());
    QVERIFY(m_settings->launchWarning().isEmpty());
    QVERIFY(!m_settings->canRestoreOrganization());
    m_settings->flushBackups();
    QFile file(today);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("/gone/999.jpg"));
  }

  void helpFiltersByContextAndDiagnosticsArePrivate() {
    const auto restoreSize = qScopeGuard([&] { m_window->resize(1280, 820); });
    m_window->resize(1920, 1080);
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("libraryShortcutHelp"));
    QVERIFY(help);
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    QCOMPARE(help->property("context").toString(), QStringLiteral("library"));
    auto* content = help->property("contentItem").value<QQuickItem*>();
    QVERIFY(content);
    auto* scroll = content->findChild<QQuickItem*>(QStringLiteral("viewerShortcutScroll"));
    QVERIFY(scroll);
    auto* flick = scroll->property("contentItem").value<QQuickItem*>();
    QVERIFY(flick);
    QTRY_VERIFY(flick->property("contentHeight").toReal() <= flick->height());
    help->setProperty("filter", QStringLiteral("zz-no-match-zz"));
    QCoreApplication::processEvents();
    const auto groups = help->property("groups").value<QJSValue>();
    QCOMPARE(groups.property(QStringLiteral("length")).toInt(), 0);
    help->setProperty("filter", QStringLiteral("search"));
    QCoreApplication::processEvents();
    QVERIFY(help->property("groups").value<QJSValue>().property(QStringLiteral("length")).toInt() > 0);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!help->property("visible").toBool());
    openDetail(0);
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    QCOMPARE(help->property("context").toString(), QStringLiteral("preview"));
    QTRY_VERIFY(flick->property("contentHeight").toReal() <= flick->height());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!help->property("visible").toBool());
    const QString report = m_settings->diagnostics(QDir::homePath() + QStringLiteral("/theme"), 1.5);
    QVERIFY(!report.contains(QDir::homePath()));
    QVERIFY(report.contains(QStringLiteral("Qt: ")));
    QVERIFY(report.contains(QStringLiteral("Scale factor: 1.5")));
    QVERIFY(report.contains(QStringLiteral("ffmpeg: ")));
  }

  void libraryHelpCapsStayInTheirColumn() {
    const auto restoreSize = qScopeGuard([&] { m_window->resize(1280, 820); });
    m_window->resize(1280, 820);
    QObject* help = m_window->findChild<QObject*>(QStringLiteral("libraryShortcutHelp"));
    QVERIFY(help);
    QTest::keyClick(m_window, Qt::Key_F1);
    QTRY_VERIFY(help->property("visible").toBool());
    const auto close = qScopeGuard([&] {
      help->setProperty("filter", QString());
      QTest::keyClick(m_window, Qt::Key_Escape);
    });
    auto* content = help->property("contentItem").value<QQuickItem*>();
    QVERIFY(content);
    auto row = [&](const QString& label) {
      return find(content, [&](QQuickItem* candidate) {
        return candidate->property("modelData").toMap().value(QStringLiteral("label")).toString() == label;
      });
    };
    // "Ctrl++" is Ctrl and Plus, not Ctrl and two blank caps.
    QQuickItem* larger = nullptr;
    QTRY_VERIFY((larger = row(QStringLiteral("Larger tiles"))));
    const QVariantList caps = larger->property("modelData").toMap().value(QStringLiteral("caps")).toList();
    QCOMPARE(caps.size(), 2);
    QCOMPARE(caps.at(0).toStringList(), (QStringList{QStringLiteral("Ctrl"), QStringLiteral("+")}));
    QCOMPARE(caps.at(1).toStringList(), (QStringList{QStringLiteral("Ctrl"), QStringLiteral("=")}));
    // Four alternatives wrap inside their column instead of running past it.
    QQuickItem* far = row(QStringLiteral("Extend the selection to the first / last tile or a page"));
    QVERIFY(far);
    QQuickItem* keys = find(far, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("shortcutCaps");
    });
    QVERIFY(keys);
    QTRY_VERIFY(keys->x() >= -0.5 && keys->x() + keys->width() <= far->width() + 0.5);
    for (QQuickItem* alternative : keys->childItems()) {
      if (alternative->width() > 0)
        QVERIFY2(alternative->x() + alternative->width() <= keys->width() + 0.5, "a key cap runs past its column");
    }
    // The RAW rows moved to Actions in the library help.
    const QVariantList groups = help->property("groups").value<QJSValue>().toVariant().toList();
    int rawRows = 0;
    for (const QVariant& group : groups) {
      const QVariantMap map = group.toMap();
      for (const QVariant& entry : map.value(QStringLiteral("rows")).toList()) {
        const QString label = entry.toMap().value(QStringLiteral("label")).toString();
        if (label == QStringLiteral("Develop RAW") || label == QStringLiteral("Choose RAW editor")) {
          QCOMPARE(map.value(QStringLiteral("title")).toString(), QStringLiteral("Actions"));
          ++rawRows;
        }
      }
    }
    QCOMPARE(rawRows, 2);
  }

  void aboutLinksMissingToolsAndWindowRules() {
    // A scratch Hyprland config, so nothing reaches the real one.
    const QString hypr = m_scratch.filePath(QStringLiteral("hypr"));
    QVERIFY(QDir().mkpath(hypr));
    const QByteArray original = "require(\"default.hypr.omarchy\")\n";
    {
      QFile config(hypr + QStringLiteral("/hyprland.lua"));
      QVERIFY(config.open(QIODevice::WriteOnly));
      config.write(original);
      QFile bindings(hypr + QStringLiteral("/bindings.lua"));
      QVERIFY(bindings.open(QIODevice::WriteOnly));
      bindings.write("o.bind(\"SUPER + P\", \"Screenshot\", \"omarchy-capture-screenshot\")\n");
    }
    m_settings->setHyprDirectory(hypr);
    QQuickItem* settings = item("settingsSheet");
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(settings, "close");
      m_settings->setHyprDirectory(QString());
    });

    // The first-run screen names the key the bindings give, never the old one.
    QCOMPARE(m_settings->screenshotKey(), QStringLiteral("Super + P"));
    QVariant state;
    QVERIFY(QMetaObject::invokeMethod(m_window, "emptyState", Q_RETURN_ARG(QVariant, state),
                                      Q_ARG(QVariant, QStringLiteral("library"))));
    if (state.metaType() == QMetaType::fromType<QJSValue>()) state = state.value<QJSValue>().toVariant();
    const QString detail = state.toMap().value(QStringLiteral("detail")).toString();
    QVERIFY2(!detail.contains(QStringLiteral("Super + Shift + S")), qPrintable(detail));
    QCOMPARE(detail.contains(QStringLiteral("Take a screenshot with Super + P")), m_theme->omarchyAvailable());
    {
      // No key takes a screenshot: point at the Capture menu without naming one.
      QFile bindings(hypr + QStringLiteral("/bindings.lua"));
      QVERIFY(bindings.open(QIODevice::WriteOnly | QIODevice::Truncate));
      bindings.write("hl.unbind(\"SUPER + P\")\n");
      bindings.close();
      QFile config(hypr + QStringLiteral("/hyprland.lua"));
      QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
      config.write("omarchy_default_bindings = false\n" + original);
      config.close();
      m_settings->setHyprDirectory(hypr);
      QCOMPARE(m_settings->screenshotKey(), QString());
      QVariant bare;
      QVERIFY(QMetaObject::invokeMethod(m_window, "emptyState", Q_RETURN_ARG(QVariant, bare),
                                        Q_ARG(QVariant, QStringLiteral("library"))));
      if (bare.metaType() == QMetaType::fromType<QJSValue>()) bare = bare.value<QJSValue>().toVariant();
      const QString text = bare.toMap().value(QStringLiteral("detail")).toString();
      QVERIFY2(!text.contains(QStringLiteral("screenshot with")), qPrintable(text));
      QCOMPARE(text.contains(QStringLiteral("Take a screenshot or record from the Capture menu.")),
               m_theme->omarchyAvailable());
      QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
      config.write(original);
      config.close();
      m_settings->setHyprDirectory(hypr);
    }

    QVERIFY(QMetaObject::invokeMethod(settings, "open"));
    QTRY_VERIFY(settings->isVisible());
    const auto child = [&](const char* name) {
      return find(settings, [name](QQuickItem* candidate) {
        return candidate->objectName() == QLatin1String(name);
      });
    };
    QCOMPARE(child("aboutChangelog")->property("url").toString(),
             QStringLiteral("https://github.com/btsouth/omaroll/releases/tag/v" OMAROLL_VERSION));
    QCOMPARE(child("aboutReportIssue")->property("url").toString(),
             QStringLiteral("https://github.com/btsouth/omaroll/issues/new/choose"));

    // Missing tools, each with its package; the suite's fake tesseract counts.
    // The build machine may have every tool, so the list can be empty.
    const QVariantList missing = m_settings->missingTools();
    for (const QVariant& row : missing) {
      const QVariantMap map = row.toMap();
      QVERIFY(map.value(QStringLiteral("install")).toString().startsWith(QStringLiteral("sudo pacman -S ")));
      QVERIFY(map.value(QStringLiteral("package")).toString() != QStringLiteral("tesseract tesseract-data-eng"));
    }
    auto* tools = m_window->findChild<QObject*>(QStringLiteral("aboutMissingTools"));
    QVERIFY(tools);
    QCOMPARE(tools->property("count").toInt(), int(missing.size()));
    QCOMPARE(child("aboutMissingToolsTitle")->isVisible(), !missing.isEmpty());
    // Both ways, whatever this machine has: a row and its title, then none.
    const QVariantList one = {QVariantMap{{QStringLiteral("program"), QStringLiteral("zbarimg")},
                                          {QStringLiteral("feature"), QStringLiteral("Copy QR codes")},
                                          {QStringLiteral("package"), QStringLiteral("zbar")},
                                          {QStringLiteral("install"), QStringLiteral("sudo pacman -S zbar")}}};
    QVERIFY(settings->setProperty("missingTools", one));
    QTRY_COMPARE(tools->property("count").toInt(), 1);
    QVERIFY(child("aboutMissingToolsTitle")->isVisible());
    QVERIFY(settings->setProperty("missingTools", QVariantList()));
    QTRY_COMPARE(tools->property("count").toInt(), 0);
    QVERIFY(!child("aboutMissingToolsTitle")->isVisible());

    // Install, then Remove, from Settings.
    QQuickItem* rules = child("settingsWindowRules");
    QQuickItem* button = child("settingsWindowRulesButton");
    QVERIFY(rules->isVisible());
    QCOMPARE(button->property("label").toString(), QStringLiteral("Install"));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_COMPARE(button->property("label").toString(), QStringLiteral("Remove"));
    QVERIFY(QFile::exists(hypr + QStringLiteral("/omaroll.lua")));
    QVERIFY(child("settingsWindowRulesStatus")->property("text").toString().contains(QStringLiteral("installed")));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_COMPARE(button->property("label").toString(), QStringLiteral("Install"));
    QVERIFY(!QFile::exists(hypr + QStringLiteral("/omaroll.lua")));
    QFile config(hypr + QStringLiteral("/hyprland.lua"));
    QVERIFY(config.open(QIODevice::ReadOnly));
    QCOMPARE(config.readAll(), original);
  }

  void footerDropsWholeHintsAtNarrowWidth() {
    const auto restoreSize = qScopeGuard([&] { m_window->resize(1280, 820); });
    m_window->resize(560, 700);
    QCoreApplication::processEvents();
    auto* hints = m_window->findChild<QQuickItem*>(QStringLiteral("footerHints"));
    QVERIFY(hints);
    int visible = 0;
    const auto visit = [&](auto&& self, QQuickItem* parent) -> void {
      for (auto* child : parent->childItems()) {
        if (child->isVisible() && child->property("text").isValid()) {
          ++visible;
          QCOMPARE(child->property("elide").toInt(), 3); // Text.ElideNone
          QVERIFY(child->mapToItem(hints, QPointF(child->width(), 0)).x() <= hints->width() + 1);
        }
        self(self, child);
      }
    };
    visit(visit, hints);
    QVERIFY(visible > 0);
    QVERIFY(visible < 8);
  }

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
    // The empty library asks which app opens pictures; another one does.
    QFile xdgMime(toolBin + QStringLiteral("/xdg-mime"));
    QVERIFY(xdgMime.open(QIODevice::WriteOnly));
    xdgMime.write("#!/bin/sh\n[ \"$1\" = query ] && echo other.desktop\nexit 0\n");
    xdgMime.close();
    QVERIFY(xdgMime.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("OMAROLL_UI_CLIPBOARD_LOG",
                    m_scratch.filePath(QStringLiteral("clipboard.log")).toUtf8()));
    QVERIFY(qputenv("PATH", toolBin.toUtf8() + ':' + qgetenv("PATH")));
    // A name with every character that trips URL parsing. Newest by mtime,
    // so it sits at the top of the grid.
    m_demoPictures = layout.pictures;
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
    m_folderTree = new FolderTreeModel(this);
    m_folderTree->bind(m_captures, m_library);
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
    m_mediaMetadata = new MediaMetadataIndex(nullptr, this);
    m_actions = new ActionLauncher(this);
    m_registry = new ActionRegistry(m_actions, this);
    m_tailscale = new TailscalePeers(this);
    m_matte = new MatteComposer(this);
    m_imageEditor = new ImageEditor(this);
    m_textIndex = new OcrIndex(m_captures, this);
    connect(m_textIndex, &OcrIndex::textReady, m_similarities, &SimilarityIndex::setText);
    m_similarities->setStrictness(m_settings->similarStrictness());
    connect(m_settings, &AppSettings::similarStrictnessChanged, m_similarities,
            [this] { m_similarities->setStrictness(m_settings->similarStrictness()); });
    m_qr = new QrDetector(m_captures, this);
    m_subtitles = new SubtitleIndex(this);
    connect(m_actions, &ActionLauncher::outputPending, m_captures, &CaptureModel::holdPath);
    connect(m_actions, &ActionLauncher::outputSettled, m_captures, &CaptureModel::releasePath);

    m_engine = new QQmlApplicationEngine(this);
    connect(m_engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError>& warnings) {
      for (const QQmlError& warning : warnings) {
        const auto expected = std::find_if(m_expectedQmlWarnings.begin(), m_expectedQmlWarnings.end(),
                                          [&](const QString& suffix) {
                                            return warning.toString().endsWith(suffix);
                                          });
        // A tile can ask for a file a test has just removed. That failure is
        // expected; one for a file that still exists is not.
        const QString text = warning.toString();
        const qsizetype missing = text.indexOf(QStringLiteral("No thumbnail for /"));
        QString thumbnailPath = missing < 0 ? QString() : text.mid(missing + 17);
        if (thumbnailPath.endsWith(QLatin1Char(')'))) thumbnailPath.chop(1);
        const bool disposable =
            (!thumbnailPath.isEmpty() && !QFileInfo::exists(thumbnailPath)) ||
            std::any_of(m_disposablePaths.cbegin(), m_disposablePaths.cend(), [&](const QString& path) {
              return text.endsWith(QStringLiteral("No thumbnail for ") + path);
            });
        if (expected != m_expectedQmlWarnings.end()) {
          m_expectedQmlWarnings.erase(expected);
        } else if (disposable) {
          // Files a test removes while tiles or the viewer still ask for them.
        } else {
          m_warnings.append(warning.toString());
        }
      }
    });
    m_engine->addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    m_thumbnails = new ThumbnailProvider;
    m_mattes = new MatteProvider;
    m_edits = new EditProvider;
    m_pdfs = new PdfProvider;
    m_raws = new RawImageProvider;
    m_large = new LargeImageProvider;
    m_engine->addImageProvider(QLatin1String(LargeImageProvider::kProviderId), m_large);
    m_engine->addImageProvider(QLatin1String(ThumbnailProvider::kProviderId), m_thumbnails);
    m_engine->addImageProvider(QLatin1String(MatteProvider::kProviderId), m_mattes);
    m_engine->addImageProvider(QLatin1String(EditProvider::kProviderId), m_edits);
    m_engine->addImageProvider(QLatin1String(PdfProvider::kProviderId), m_pdfs);
    m_engine->addImageProvider(QLatin1String(RawImageProvider::kProviderId), m_raws);
    Storyboard::install(*m_engine);

    QQmlContext* context = m_engine->rootContext();
    context->setContextProperty(QStringLiteral("Theme"), m_theme);
    context->setContextProperty(QStringLiteral("FolderTree"), m_folderTree);
    context->setContextProperty(QStringLiteral("Captures"), m_library);
    context->setContextProperty(QStringLiteral("Library"), m_captures);
    context->setContextProperty(QStringLiteral("Actions"), m_actions);
    context->setContextProperty(QStringLiteral("Settings"), m_settings);
    m_mimeDefaults = new MimeDefaults(this);
    context->setContextProperty(QStringLiteral("MimeDefaults"), m_mimeDefaults);
    context->setContextProperty(QStringLiteral("Registry"), m_registry);
    context->setContextProperty(QStringLiteral("Editors"), new ExternalEditors(m_actions, m_engine));
    context->setContextProperty(QStringLiteral("Matte"), m_matte);
    context->setContextProperty(QStringLiteral("ImageEdit"), m_imageEditor);
    context->setContextProperty(QStringLiteral("TextIndex"), m_textIndex);
    context->setContextProperty(QStringLiteral("Qr"), m_qr);
    context->setContextProperty(QStringLiteral("Subtitles"), m_subtitles);
    context->setContextProperty(QStringLiteral("Duplicates"), m_duplicates);
    context->setContextProperty(QStringLiteral("Similarities"), m_similarities);
    context->setContextProperty(QStringLiteral("MediaInfo"), m_mediaInfo);
    context->setContextProperty(QStringLiteral("PdfInfo"), m_pdfInfo);
    context->setContextProperty(QStringLiteral("MediaMetadata"), m_mediaMetadata);
    context->setContextProperty(QStringLiteral("Tailscale"), m_tailscale);
    context->setContextProperty(QStringLiteral("DemoMode"), true);
    // Without a bus connection, so playback never reaches the desktop's.
    context->setContextProperty(QStringLiteral("Mpris"),
                                new MprisService(m_settings, QDBusConnection(QString()), this));
    context->setContextProperty(QStringLiteral("InitialPaths"), QStringList());
    context->setContextProperty(QStringLiteral("InitialFolderPath"), QString());

    m_engine->loadFromModule("Omaroll", "Main");
    QVERIFY2(!m_engine->rootObjects().isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
    m_window = qobject_cast<QQuickWindow*>(m_engine->rootObjects().constFirst());
    QVERIFY(m_window);
    m_window->resize(1280, 820);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    // The first scan must finish before any test picks a row by position:
    // rows still arriving would reorder under a key press.
    QVERIFY(QTest::qWaitFor([&] { return !m_captures->scanning() && m_library->rowCount() >= 3; }, 60000));
    // Let the grid lay its tiles out before anything is clicked.
    QTRY_VERIFY(cardFor(pathAt(1)) != nullptr);
    QTRY_VERIFY(item("library")->property("layoutReady").toBool());
    QVERIFY(!m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer")));
    QVERIFY(!m_window->findChild<QObject*>(QStringLiteral("videoOutput")));
  }

  void cleanupTestCase() {
    m_thumbnails->shutdown();
    m_mattes->shutdown();
    m_edits->shutdown();
    m_pdfs->shutdown();
    m_raws->shutdown();
    m_large->shutdown();
    delete m_engine;
    m_engine = nullptr;
    // The storyboards are gone; their workers finish saving before the
    // cache and the pools go away.
    Storyboard::shutdown();
    Storyboard::waitForWorkers();
    QThreadPool::globalInstance()->waitForDone();
  }

  // Every test starts from the plain grid with nothing selected.
  void init() {
    for (const auto& name : {QStringLiteral("libraryContextMenu"), QStringLiteral("detailContextMenu")}) {
      if (QObject* menu = m_window->findChild<QObject*>(name)) QMetaObject::invokeMethod(menu, "close");
    }
    for (int guard = 0; guard < 8 && prop("anySheetOpen").toBool(); ++guard) {
      invoke("dismissTopLayer");
      QCoreApplication::processEvents();
    }
    QTRY_VERIFY(!prop("anySheetOpen").toBool());
    // A test that resized the window leaves the grid rebuilding its columns,
    // and a key pressed while the model is detached for that is lost.
    QTRY_VERIFY(item("library")->property("layoutSettled").toBool());
    m_library->setDuplicatesOnly(false);
    m_library->setSimilarOnly(false);
    QMetaObject::invokeMethod(item("library"), "clearChecked");
    item("library")->setProperty("currentIndex", 0);
    item("library")->forceActiveFocus();
    QTRY_VERIFY(item("library")->hasActiveFocus());
    // Playback preferences and resume spots persist, so a test that mutes or
    // seeks would otherwise change the next test's starting state.
    m_settings->setVideoVolume(0.8);
    m_settings->setVideoMuted(false);
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) {
        m_settings->clearVideoPosition(m_library->pathAt(row));
      }
    }
  }


  void firstGridMarkImmediatelyEnablesUndoAgain() {
    while (m_settings->canUndo()) m_settings->undo();
    const QString path = pathAt(0);
    const bool favorite = m_settings->isFavorite(path);
    for (int round = 0; round < 2; ++round) {
      QTest::keyClick(m_window, Qt::Key_V);
      QTRY_COMPARE(m_settings->isFavorite(path), !favorite);
      QVERIFY(m_settings->canUndo());
      QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
      QTRY_COMPARE(m_settings->isFavorite(path), favorite);
      QVERIFY(!m_settings->canUndo());
    }
  }

  void unpinIconIsReachableAndActivatesFromTheKeyboard() {
    const QString folder = QFileInfo(pathAt(0)).absolutePath();
    QVERIFY(m_settings->pinFolderPath(folder));
    const auto restore = qScopeGuard([&] { m_settings->unpinFolder(folder); });
    QQuickItem* unpin = nullptr;
    QTRY_VERIFY((unpin = find(item("pinnedFolders"), [&](QQuickItem* candidate) {
      return candidate->property("toolTip").toString() == QStringLiteral("Unpin ") + folder;
    })) != nullptr);
    unpin->parentItem()->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(unpin->hasActiveFocus());
    QVERIFY(unpin->activeFocusOnTab());
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(!m_settings->isFolderPinned(folder));
  }

  void captionShortcutAndTechnicalDetailsWorkWithoutAPointer() {
    m_window->resize(560, 420);
    const auto restore = qScopeGuard([&] { m_window->resize(1280, 820); });
    openDetail(0);
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QQuickItem* caption = item("viewerCaption");
    QTRY_VERIFY(caption->hasActiveFocus());
    QVERIFY(caption->activeFocusOnTab());
    typeText(QStringLiteral("cancel this caption"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!caption->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QTRY_VERIFY(caption->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Tab);
    QQuickItem* technical = item("viewerTechnicalToggle");
    QTRY_VERIFY(technical->hasActiveFocus());
    auto* scroll = item("inspectorScroll");
    QTRY_VERIFY(technical->mapToItem(scroll, QPointF()).y() >= 0);
    QVERIFY(technical->mapToItem(scroll, QPointF(0, technical->height())).y() <= scroll->height());
    const bool expanded = detail->property("technicalExpanded").toBool();
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(detail->property("technicalExpanded").toBool(), !expanded);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(detail->property("technicalExpanded").toBool(), expanded);
  }

  void settingsTabRevealsEveryFocusedControl() {
    m_window->resize(560, 420);
    const auto restore = qScopeGuard([&] { m_window->resize(1280, 820); });
    QVERIFY(QMetaObject::invokeMethod(item("settingsSheet"), "open"));
    QQuickItem* scroll = item("settingsScroll");
    QQuickItem* first = find(item("settingsSheet"), [](QQuickItem* candidate) {
      return candidate->property("accessibleName").toString() == QStringLiteral("Scan Downloads");
    });
    QVERIFY(first);
    first->forceActiveFocus();
    bool scrolled = false;
    for (int step = 0; step < 18; ++step) {
      QTest::keyClick(m_window, Qt::Key_Tab);
      QQuickItem* focused = m_window->activeFocusItem();
      QVERIFY(focused);
      QQuickItem* ancestor = focused;
      while (ancestor && ancestor != scroll) ancestor = ancestor->parentItem();
      if (ancestor != scroll) break;
      QTRY_VERIFY(focused->mapToItem(scroll, QPointF()).y() >= -1);
      QTRY_VERIFY(focused->mapToItem(scroll, QPointF(0, focused->height())).y() <= scroll->height() + 1);
      scrolled |= scroll->property("contentY").toReal() > 0;
    }
    QVERIFY(scrolled);
  }

  // Every on/off setting is a switch that says its state to assistive
  // technology, flips from the mouse, the keyboard or its words, and is kept
  // for the next start. The settings with several values step through them.
  void everySettingTogglesAndPersists() {
    QQuickItem* settings = item("settingsSheet");
    QVERIFY(QMetaObject::invokeMethod(settings, "open"));
    QTRY_VERIFY(settings->isVisible());
    QQuickItem* scroll = item("settingsScroll");
    const auto restore = qScopeGuard([&] { QMetaObject::invokeMethod(settings, "close"); });
    const auto child = [&](const QString& name) {
      return find(settings, [&name](QQuickItem* candidate) { return candidate->objectName() == name; });
    };
    // Shown in the scrolled list, after the list follows the focus to it.
    const auto inView = [&](QQuickItem* control) {
      const QPointF top = control->mapToItem(scroll, QPointF());
      return top.y() >= 0 && top.y() + control->height() <= scroll->height();
    };
    struct Switch { const char* object; const char* property; const char* name; QObject* owner; };
    const QList<Switch> switches = {
        {"scanDownloadsToggle", "scanDownloads", "Scan Downloads", m_settings},
        {"showHiddenToggle", "showHidden", "Hidden items", m_library},
        {"gridFilenamesToggle", "showGridFilenames", "Show filenames in grid", m_settings},
        {"pairRawJpegToggle", "pairRawJpeg", "Pair RAW and JPEG", m_settings},
        {"permanentDeleteConfirmation", "confirmPermanentDelete", "Confirm permanent deletion", m_settings},
        {"enlargeSmallPicturesButton", "enlargeSmallPictures", "Enlarge small pictures to fit", m_settings},
        {"slideshowShuffleToggle", "slideshowShuffle", "Shuffle slideshow", m_settings},
        {"rememberPlaybackSpeedButton", "rememberPlaybackSpeed", "Remember playback speed", m_settings},
        {"slideshowVideosToggle", "slideshowVideos", "Include videos in slideshow", m_settings},
        {"backgroundTextIndexToggle", "backgroundTextIndex", "Index text in the background", m_settings},
        {"textIndexScreenshotsOnlyToggle", "textIndexScreenshotsOnly", "Index screenshots only", m_settings},
    };
    int way = 0;
    for (const Switch& entry : switches) {
      const QString object = QString::fromLatin1(entry.object);
      QQuickItem* control = child(object);
      QVERIFY2(control, entry.object);
      if (object == QStringLiteral("textIndexScreenshotsOnlyToggle")) {
        // Shown only while background indexing is on.
        m_settings->setBackgroundTextIndex(true);
      }
      if (object.startsWith(QStringLiteral("backgroundTextIndex")) || object.startsWith(QStringLiteral("textIndex"))) {
        if (!m_textIndex->available()) continue;
      }
      QTRY_VERIFY2(control->isVisible(), entry.object);
      const bool before = entry.owner->property(entry.property).toBool();
      const auto putBack = qScopeGuard([&] {
        entry.owner->setProperty(entry.property, before);
        if (object == QStringLiteral("textIndexScreenshotsOnlyToggle")) m_settings->setBackgroundTextIndex(false);
      });

      QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(control);
      QVERIFY(accessible);
      QCOMPARE(accessible->role(), QAccessible::CheckBox);
      QCOMPARE(accessible->text(QAccessible::Name), QString::fromLatin1(entry.name));
      QVERIFY(accessible->state().checkable);
      QCOMPARE(bool(accessible->state().checked), before);

      control->forceActiveFocus();
      QTRY_VERIFY2(inView(control), entry.object);
      // Background indexing rewrites its status line as it starts, which can
      // move the rows below it; press where the switch is once it stays put.
      QPointF placed = control->mapToScene(QPointF());
      QTRY_VERIFY2([&] {
        const QPointF now = control->mapToScene(QPointF());
        const bool still = now == placed;
        placed = now;
        QTest::qWait(60);
        return still && control->mapToScene(QPointF()) == now && inView(control);
      }(), entry.object);
      switch (way++ % 3) {
        case 0: QTest::keyClick(m_window, Qt::Key_Space); break;
        case 1: click(control); break;
        default: {
          // The words beside a switch flip it too.
          QQuickItem* row = control->parentItem()->parentItem();
          QVERIFY(row->property("toggle").value<QObject*>() == control);
          QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                            row->mapToScene(QPointF(20, row->height() / 2)).toPoint());
        }
      }
      QTRY_VERIFY2_WITH_TIMEOUT(entry.owner->property(entry.property).toBool() == !before,
                                qPrintable(QStringLiteral("%1 by way %2").arg(object).arg((way - 1) % 3)), 2000);
      QTRY_COMPARE(bool(QAccessible::queryAccessibleInterface(control)->state().checked), !before);
      QCOMPARE(control->property("checked").toBool(), !before);
      if (entry.owner == m_settings) {
        AppSettings restarted;
        QCOMPARE(restarted.property(entry.property).toBool(), !before);
      }
      QTest::keyClick(m_window, Qt::Key_Return);
      QTRY_VERIFY2(entry.owner->property(entry.property).toBool() == before, entry.object);
    }

    // The several-value settings step to the next value, and keep it.
    for (const auto& [object, property] : {std::pair{"folderDepthButton", "recursionDepth"},
                                            std::pair{"thumbnailCacheButton", "thumbnailCacheMb"},
                                            std::pair{"slideshowIntervalButton", "slideshowIntervalSeconds"},
                                            std::pair{"imageDefaultButton", "imagePrimaryAction"},
                                            std::pair{"videoDefaultButton", "videoPrimaryAction"}}) {
      QQuickItem* control = child(QString::fromLatin1(object));
      QVERIFY2(control, object);
      const QVariant before = m_settings->property(property);
      const auto putBack = qScopeGuard([&] { m_settings->setProperty(property, before); });
      const QString name = control->property("accessibleName").toString();
      QVERIFY2(name.endsWith(control->property("label").toString()), qPrintable(name));
      control->forceActiveFocus();
      QTRY_VERIFY2(inView(control), object);
      click(control);
      if (QByteArray(property).endsWith("PrimaryAction")) {
        // Only installed handlers are offered; with one, there is nothing to step to.
        QVariant offered;
        const QVariantList all = QByteArray(property).startsWith("image")
            ? QVariantList{QStringLiteral("preview"), QStringLiteral("omaframe"), QStringLiteral("corrections"), QStringLiteral("matte"), QStringLiteral("view"), QStringLiteral("edit")}
            : QVariantList{QStringLiteral("preview"), QStringLiteral("trim"), QStringLiteral("play")};
        QVERIFY(QMetaObject::invokeMethod(settings, "availableDefaults", Q_RETURN_ARG(QVariant, offered),
                                          Q_ARG(QVariant, all)));
        if (offered.metaType() == QMetaType::fromType<QJSValue>()) offered = offered.value<QJSValue>().toVariant();
        if (offered.toList().size() < 2) continue;
      }
      QTRY_VERIFY2(m_settings->property(property) != before, object);
      AppSettings restarted;
      QCOMPARE(restarted.property(property), m_settings->property(property));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!m_captures->scanning(), 15000);
    QTest::mouseMove(m_window, QPoint(5, 5));
  }

  // Sections with a side index on a big window, one column on a small one,
  // and nothing smaller than 12 px to read.
  void settingsSectionsFitWideAndNarrowWindows() {
    QQuickItem* settings = item("settingsSheet");
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(settings, "close");
      m_window->resize(1280, 820);
      QTRY_COMPARE(m_window->width(), 1280);
      // The wheel left the pointer over the grid, where a tile's hover tip
      // would answer for the next test's keyboard one.
      QTest::mouseMove(m_window, QPoint(5, 5));
      QTRY_VERIFY(item("library")->property("layoutReady").toBool());
      QTRY_VERIFY(item("library")->hasActiveFocus());
    });
    m_window->resize(1600, 1000);
    QTRY_COMPARE(m_window->width(), 1600);
    QTRY_COMPARE(m_window->height(), 1000);
    QVERIFY(QMetaObject::invokeMethod(settings, "open"));
    QTRY_VERIFY(settings->isVisible());
    QQuickItem* card = item("settingsCard");
    QQuickItem* scroll = item("settingsScroll");
    // Well past the old 520 by 620 column: it takes the room the window gives.
    QTRY_VERIFY2(card->width() > 800, qPrintable(QString::number(card->width())));
    QVERIFY2(card->height() >= qMin(880.0, settings->height() - 64) - 1,
             qPrintable(QStringLiteral("%1 of %2").arg(card->height()).arg(settings->height())));
    QVERIFY(card->height() > 700);
    QQuickItem* index = item("settingsIndex");
    QVERIFY(index->isVisible());
    const QStringList titles = {QStringLiteral("Library"), QStringLiteral("Viewer"), QStringLiteral("Video"),
                                QStringLiteral("Search and text"), QStringLiteral("Albums and backups"),
                                QStringLiteral("Integration"), QStringLiteral("About")};
    QCOMPARE(settings->property("sectionItems").toList().size(), titles.size());
    for (int i = 0; i < titles.size(); ++i) {
      QQuickItem* entry = item(QStringLiteral("settingsIndex_%1").arg(i));
      QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(entry);
      QVERIFY(accessible);
      QCOMPARE(accessible->text(QAccessible::Name), titles.at(i));
    }
    // An index entry brings its section up and is marked as the one shown.
    click(item(QStringLiteral("settingsIndex_4")));
    QTRY_COMPARE(settings->property("currentSection").toInt(), 4);
    QQuickItem* albums = item(QStringLiteral("settingsSection_albums"));
    QTRY_VERIFY(albums->mapToItem(scroll, QPointF()).y() >= -1);
    QVERIFY(albums->mapToItem(scroll, QPointF()).y() < scroll->height() / 2);
    QVERIFY(item("settingsTopFade")->isVisible());
    // The wheel moves the list and lets the index follow.
    const QPoint over = scroll->mapToScene(QPointF(scroll->width() / 2, scroll->height() / 2)).toPoint();
    for (int notch = 0; notch < 40; ++notch) {
      QWheelEvent wheel(over, m_window->mapToGlobal(over), QPoint(), QPoint(0, 120), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
      QCoreApplication::sendEvent(m_window, &wheel);
    }
    QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);
    QTRY_COMPARE(settings->property("currentSection").toInt(), 0);
    QVERIFY(!item("settingsTopFade")->isVisible());
    QVERIFY(item("settingsBottomFade")->isVisible());

    // Supporting text is 12 px or more everywhere in the sheet.
    const auto smallText = [&]() {
      QStringList small;
      find(card, [&small](QQuickItem* candidate) {
        const QVariant font = candidate->property("font");
        if (candidate->isVisible() && candidate->property("text").isValid() && font.isValid()
            && font.value<QFont>().pixelSize() > 0 && font.value<QFont>().pixelSize() < 12)
          small.append(candidate->property("text").toString());
        return false;
      });
      return small;
    };
    QVERIFY2(smallText().isEmpty(), qPrintable(smallText().join(u'|')));

    // At the narrowest window the sheet keeps a margin, drops the index and
    // still scrolls to its end.
    m_window->resize(560, 700);
    QTRY_COMPARE(m_window->width(), 560);
    QTRY_VERIFY(!index->isVisible());
    QVERIFY(card->width() <= 560 - 24);
    QVERIFY(card->height() <= 700 - 24);
    const QRectF cardBounds = card->mapRectToScene(card->boundingRect());
    QVERIFY(cardBounds.left() >= 0 && cardBounds.right() <= 560);
    QQuickItem* about = item(QStringLiteral("settingsSection_about"));
    // The last section comes up with its title in view.
    QVERIFY(QMetaObject::invokeMethod(settings, "showSection", Q_ARG(QVariant, 6)));
    QTRY_VERIFY(about->mapToItem(scroll, QPointF()).y() >= 0);
    QVERIFY(about->mapToItem(scroll, QPointF()).y() < 40);
    QCOMPARE(settings->property("currentSection").toInt(), 6);
    QVERIFY2(smallText().isEmpty(), qPrintable(smallText().join(u'|')));
  }

  // Date sorts put a header over each day or month, with its count and a
  // control that selects it; tiles from other days carry their date; and
  // selection works as in a file manager across those sections.
  void dateSectionsSelectByDayRangeToggleAndBand() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage tile(40, 30, QImage::Format_RGB32);
    tile.fill(QColor(40, 90, 160));
    QStringList added;
    // Five on 4 May 2019, three on 3 May, newest first within each day.
    for (const auto& [date, hours] : {std::pair{QDate(2019, 5, 4), QList<int>{14, 13, 12, 11, 10}},
                                      std::pair{QDate(2019, 5, 3), QList<int>{12, 11, 10}}}) {
      for (int hour : hours) {
        const QString path = dir.filePath(QStringLiteral("screenshot-%1_%2-00-00.png")
                                              .arg(date.toString(Qt::ISODate))
                                              .arg(hour, 2, 10, QLatin1Char('0')));
        QVERIFY(tile.save(path));
        added.append(path);
      }
    }
    for (const QString& path : added) m_disposablePaths.append(path);
    m_captures->addExtraFiles(added);
    auto* grid = item("library");
    auto* rows = qobject_cast<SectionRows*>(m_library->sectionRows());
    QVERIFY(rows);
    const auto cleanup = qScopeGuard([&] {
      // Off the tiles, so no hover tooltip outlives this test.
      QTest::mouseMove(m_window, QPoint(5, 5));
      invoke("dismissTopLayer");
      QMetaObject::invokeMethod(grid, "clearChecked");
      m_library->setSearchText({});
      m_library->setSimilarOnly(false);
      m_library->setSortMode(CaptureFilterModel::NewestFirst);
      m_library->setFolderFilter({});
      m_window->resize(1280, 820);
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(added.cbegin(), added.cend(), [&](const QString& path) {
      return m_library->rowOf(path) >= 0;
    }), 10000);
    m_library->setFolderFilter(dir.path());
    QTRY_COMPARE(m_library->count(), 8);
    QTRY_VERIFY(item("reviewSets")->isVisible());
    QVERIFY(!item("flatGrid")->isVisible());
    QTRY_COMPARE(rows->sectionCount(), 2);

    const QLocale locale = QLocale::system();
    const QString may4 = locale.toString(QDate(2019, 5, 4), QStringLiteral("d MMMM yyyy"));
    const QString may3 = locale.toString(QDate(2019, 5, 3), QStringLiteral("d MMMM yyyy"));
    // Visible header rows by label.
    const auto headers = [&] {
      QMap<QString, QQuickItem*> found;
      std::function<void(QQuickItem*)> walk = [&](QQuickItem* node) {
        for (QQuickItem* child : node->childItems()) {
          if (child->objectName() == QStringLiteral("sectionHeader") && child->isVisible()
              && child->parentItem()->property("index").toInt() >= 0) {
            found.insert(child->parentItem()->property("label").toString(), child);
          }
          walk(child);
        }
      };
      walk(item("reviewSetsList"));
      return found;
    };
    QTRY_COMPARE(headers().keys().size(), 2);
    QVERIFY(headers().contains(may4) && headers().contains(may3));
    QAccessibleInterface* heading = QAccessible::queryAccessibleInterface(headers().value(may4));
    QVERIFY(heading);
    QCOMPARE(heading->role(), QAccessible::Heading);
    QCOMPARE(heading->text(QAccessible::Name), may4 + QStringLiteral(", 5 items"));
    // The header is at the top, so the floating day pill has nothing to add.
    QCOMPARE(grid->property("currentDayLabel").toString(), QString());

    // Inside a day's section a tile shows only its time; the header has the
    // day. Assistive technology still hears the date.
    QQuickItem* first = nullptr;
    QTRY_VERIFY((first = cardFor(added.first())) != nullptr);
    QQuickItem* when = find(first, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("captureWhenLabel");
    });
    QVERIFY(when);
    QVERIFY2(!when->property("text").toString().contains(QStringLiteral(" · ")),
             qPrintable(when->property("text").toString()));
    QVERIFY2(!when->property("text").toString().contains(
                 locale.toString(QDate(2019, 5, 4), QStringLiteral("d MMM"))),
             qPrintable(when->property("text").toString()));
    QVERIFY(when->width() <= first->width());
    QVERIFY(QAccessible::queryAccessibleInterface(first)->text(QAccessible::Name).contains(
        locale.toString(QDate(2019, 5, 4), QStringLiteral("d MMM yyyy"))));

    const auto checkedPaths = [&] {
      QVariant paths;
      QMetaObject::invokeMethod(grid, "checkedPaths", Q_RETURN_ARG(QVariant, paths));
      QStringList list = paths.value<QJSValue>().toVariant().toStringList();
      if (list.isEmpty()) list = paths.toStringList();
      list.sort();
      return list;
    };
    const auto expected = [&](QList<int> indexes) {
      QStringList list;
      for (int index : indexes) list.append(added.at(index));
      list.sort();
      return list;
    };
    const auto clickCard = [&](int index, Qt::KeyboardModifiers modifiers) {
      QQuickItem* card = nullptr;
      QTRY_VERIFY((card = cardFor(added.at(index))) != nullptr);
      QTest::mouseClick(m_window, Qt::LeftButton, modifiers, centre(card));
      QTest::qWait(30);
    };

    // The section control selects the day, and clears it once all of it is
    // selected.
    const auto selectControl = [&](const QString& label) {
      return find(headers().value(label), [](QQuickItem* candidate) {
        return candidate->objectName() == QStringLiteral("sectionSelect") && candidate->isVisible();
      });
    };
    QQuickItem* select = selectControl(may3);
    QVERIFY(select);
    QCOMPARE(QAccessible::queryAccessibleInterface(select)->role(), QAccessible::CheckBox);
    click(select);
    QTRY_COMPARE(checkedPaths(), expected({5, 6, 7}));
    QVERIFY(QAccessible::queryAccessibleInterface(selectControl(may3))->state().checked);
    click(selectControl(may3));
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // Shift+click extends from the tile clicked last, across the header.
    clickCard(2, Qt::NoModifier);
    QTRY_COMPARE(grid->property("currentIndex").toInt(), m_library->rowOf(added.at(2)));
    clickCard(6, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({2, 3, 4, 5, 6}));
    QString why;
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    // Shift+click again moves the far end rather than adding a second range.
    clickCard(3, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({2, 3}));
    // Ctrl+click adds or removes one tile and moves the anchor there.
    clickCard(7, Qt::ControlModifier);
    QTRY_COMPARE(checkedPaths(), expected({2, 3, 7}));
    clickCard(2, Qt::ControlModifier);
    QTRY_COMPARE(checkedPaths(), expected({3, 7}));
    clickCard(0, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2, 3, 7}));
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // Shift+arrows extend from the current tile, from one day into the next.
    clickCard(4, Qt::NoModifier);
    QTRY_VERIFY(item("reviewSetsList")->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({4, 5}));
    QTest::keyClick(m_window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({4, 5, 6}));
    QTest::keyClick(m_window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({4, 5}));
    // Up keeps the column through the header into the previous day.
    QTest::keyClick(m_window, Qt::Key_Up, Qt::ShiftModifier);
    QVERIFY(checkedPaths().contains(added.at(4)));
    QCOMPARE(grid->property("currentIndex").toInt() <= m_library->rowOf(added.at(4)), true);
    QTest::keyClick(m_window, Qt::Key_End, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({4, 5, 6, 7}));
    QTest::keyClick(m_window, Qt::Key_Home, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2, 3, 4}));
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    // X still toggles the current tile, and a plain move just moves.
    QTest::keyClick(m_window, Qt::Key_X);
    QTRY_COMPARE(checkedPaths(), expected({1, 2, 3, 4}));
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(grid->property("currentIndex").toInt(), m_library->rowOf(added.at(1)));
    QCOMPARE(checkedPaths(), expected({1, 2, 3, 4}));
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 8);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // A drag from the gap beside a tile draws a band over what it touches.
    QQuickItem* zero = cardFor(added.at(0));
    QQuickItem* two = cardFor(added.at(2));
    QVERIFY(zero && two);
    const QPointF gap = zero->mapToScene(QPointF(zero->width() + 3, zero->height() / 2));
    const QPoint from = gap.toPoint();
    // Into the next tile, a little lower, so the band has both dimensions.
    const QPoint to = centre(two) + QPoint(0, 20);
    const auto drag = [&](Qt::KeyboardModifiers modifiers) {
      QTest::mousePress(m_window, Qt::LeftButton, modifiers, from);
      for (int step = 1; step <= 6; ++step) {
        QTest::mouseMove(m_window, from + (to - from) * step / 6, 16);
      }
      QQuickItem* band = item("rubberBandRect");
      QTRY_VERIFY(band->isVisible());
      QVERIFY(band->width() > 0 && band->height() > 0);
      QTest::mouseRelease(m_window, Qt::LeftButton, modifiers, to, 16);
      QTRY_VERIFY(!item("rubberBandRect")->isVisible());
    };
    drag(Qt::NoModifier);
    QTRY_COMPARE(checkedPaths(), expected({1, 2}));
    // Without Ctrl a new band replaces the selection; with it, it adds.
    clickCard(7, Qt::ControlModifier);
    QTRY_COMPARE(checkedPaths(), expected({1, 2, 7}));
    drag(Qt::NoModifier);
    QTRY_COMPARE(checkedPaths(), expected({1, 2}));
    clickCard(7, Qt::ControlModifier);
    drag(Qt::ControlModifier);
    QTRY_COMPARE(checkedPaths(), expected({1, 2, 7}));
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    // The keyboard carries on after a band.
    QTRY_VERIFY(grid->hasActiveFocus() || item("reviewSetsList")->hasActiveFocus());
    const int before = grid->property("currentIndex").toInt();
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_VERIFY(grid->property("currentIndex").toInt() != before);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // A search keeps only the sections it matches.
    m_library->setSearchText(QStringLiteral("2019-05-03"));
    QTRY_COMPARE(m_library->count(), 3);
    QTRY_COMPARE(headers().keys(), QStringList({may3}));
    m_library->setSearchText({});
    QTRY_COMPARE(headers().keys().size(), 2);

    // Name, size and rating sorts are the flat grid again.
    m_library->setSortMode(CaptureFilterModel::NameAscending);
    QTRY_VERIFY(item("flatGrid")->isVisible());
    QTRY_VERIFY(!item("reviewSets")->isVisible());
    // Shift+click and Shift+arrows work the same in the flat grid, where
    // names put 3 May first: 0 is the last row and 3 the fifth.
    clickCard(0, Qt::NoModifier);
    clickCard(3, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2, 3}));
    QTest::keyClick(m_window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2, 3, 4}));
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);
    m_library->setSortMode(CaptureFilterModel::NewestFirst);
    QTRY_VERIFY(item("reviewSets")->isVisible());
    QTRY_COMPARE(headers().keys().size(), 2);

    // Similar pictures replace the days with their sets.
    m_library->setSimilarOnly(true);
    QTRY_COMPARE(rows->mode(), int(SectionRows::ReviewSections));
    QTRY_VERIFY(headers().isEmpty() || find(item("reviewSetsList"), [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("sectionSelect") && candidate->isVisible();
    }) == nullptr);
    m_library->setSimilarOnly(false);
    QTRY_COMPARE(rows->mode(), int(SectionRows::DateSections));
    QTRY_COMPARE(headers().keys().size(), 2);
    // The sets' summary row leaves with them, out of the exact offsets too.
    QTRY_COMPARE(item("reviewSets")->property("summaryHeight").toReal(), 0.0);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));

    // A narrow window keeps the date and time inside the tile.
    m_window->resize(560, 600);
    QTRY_VERIFY(cardFor(added.first()));
    QTRY_VERIFY([&] {
      QQuickItem* narrow = cardFor(added.first());
      QQuickItem* label = narrow ? find(narrow, [](QQuickItem* candidate) {
        return candidate->objectName() == QStringLiteral("captureWhenLabel");
      }) : nullptr;
      return label && label->width() > 0 && label->x() + label->width() <= narrow->width();
    }());
    // Clicking a tile cut off at the bottom picks it without scrolling, so a
    // double click cannot land on a different tile.
    QQuickItem* list = item("reviewSetsList");
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(300);
    const QRectF viewport = list->mapRectToScene(list->boundingRect());
    QQuickItem* partial = nullptr;
    for (const QString& path : added) {
      QQuickItem* card = cardFor(path);
      if (!card) continue;
      const QRectF rect = card->mapRectToScene(card->boundingRect());
      if (rect.top() < viewport.bottom() - 24 && rect.bottom() > viewport.bottom()) {
        partial = card;
        break;
      }
    }
    if (partial) {
      const QString path = partial->property("path").toString();
      const qreal before = list->property("contentY").toReal();
      const QRectF rect = partial->mapRectToScene(partial->boundingRect());
      QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                        QPoint(qRound(rect.center().x()), qRound((rect.top() + viewport.bottom()) / 2)));
      QTRY_COMPARE(pathAt(grid->property("currentIndex").toInt()), path);
      QTest::qWait(300);
      QCOMPARE(list->property("contentY").toReal(), before);
    }
  }

  // Rows move under a selection: a re-sort, a capture arriving at the top,
  // a rescan that removes one, and a capture arriving mid-band. The count
  // the window shows before Trash must match the files it would act on, and
  // each day's Select control must describe its own day afterwards.
  void selectionCountsFollowResortsAndRescans() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage picture(40, 30, QImage::Format_RGB32);
    picture.fill(QColor(70, 140, 90));
    QStringList added;
    // Newest first: 0..4 on 4 May 2019 (14:00 down to 10:00), 5..7 on 3 May.
    for (const auto& [date, hours] : {std::pair{QDate(2019, 5, 4), QList<int>{14, 13, 12, 11, 10}},
                                      std::pair{QDate(2019, 5, 3), QList<int>{12, 11, 10}}}) {
      for (int hour : hours) {
        const QString path = dir.filePath(QStringLiteral("screenshot-%1_%2-00-00.png")
                                              .arg(date.toString(Qt::ISODate))
                                              .arg(hour, 2, 10, QLatin1Char('0')));
        QVERIFY(picture.save(path));
        added.append(path);
      }
    }
    for (const QString& path : added) m_disposablePaths.append(path);
    m_captures->addExtraFiles(added);
    auto* grid = item("library");
    auto* rows = qobject_cast<SectionRows*>(m_library->sectionRows());
    QVERIFY(rows);
    const auto cleanup = qScopeGuard([&] {
      QTest::mouseMove(m_window, QPoint(5, 5));
      invoke("dismissTopLayer");
      QMetaObject::invokeMethod(grid, "clearChecked");
      m_library->setSortMode(CaptureFilterModel::NewestFirst);
      m_library->setFolderFilter({});
      m_window->resize(1280, 820);
      waitForLayout(QSize(1280, 820));
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(added.cbegin(), added.cend(), [&](const QString& path) {
      return m_library->rowOf(path) >= 0;
    }), 10000);
    m_library->setFolderFilter(dir.path());
    QTRY_COMPARE(m_library->count(), 8);
    QTRY_VERIFY(item("reviewSets")->isVisible());
    QTRY_COMPARE(rows->sectionCount(), 2);
    QTest::qWait(100);

    const QLocale locale = QLocale::system();
    const QString may4 = locale.toString(QDate(2019, 5, 4), QStringLiteral("d MMMM yyyy"));
    const QString may3 = locale.toString(QDate(2019, 5, 3), QStringLiteral("d MMMM yyyy"));
    const auto checkedPaths = [&] {
      QVariant paths;
      QMetaObject::invokeMethod(grid, "checkedPaths", Q_RETURN_ARG(QVariant, paths));
      QStringList list = paths.value<QJSValue>().toVariant().toStringList();
      list.sort();
      return list;
    };
    const auto expected = [&](QList<int> indexes) {
      QStringList list;
      for (int index : indexes) list.append(added.at(index));
      list.sort();
      return list;
    };
    const auto clickCard = [&](int index, Qt::KeyboardModifiers modifiers) {
      QQuickItem* card = nullptr;
      QTRY_VERIFY((card = cardFor(added.at(index))) != nullptr);
      QTest::mouseClick(m_window, Qt::LeftButton, modifiers, centre(card));
      QTest::qWait(30);
    };
    // The Select control's state for a day: 0 none, 1 mixed, 2 all, -1 when
    // its header is not on screen.
    const auto dayState = [&](const QString& label) {
      QQuickItem* control = nullptr;
      find(item("reviewSetsList"), [&](QQuickItem* candidate) {
        if (candidate->objectName() == QStringLiteral("sectionHeader") && candidate->isVisible() &&
            candidate->parentItem()->property("label").toString() == label &&
            shownByItsView(candidate)) {
          control = find(candidate, [](QQuickItem* inner) {
            return inner->objectName() == QStringLiteral("sectionSelect");
          });
        }
        return false;
      });
      if (!control) return -1;
      QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(control);
      const int selection = control->property("selection").toInt();
      // What a screen reader hears agrees with what is drawn.
      if (!accessible || accessible->state().checked != (selection == 2) ||
          accessible->state().checkStateMixed != (selection == 1)) {
        return -2;
      }
      return selection;
    };
    QString why;

    // A Shift range, then a re-sort: Oldest first puts 3 May on top.
    clickCard(0, Qt::NoModifier);
    clickCard(4, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2, 3, 4}));
    QTRY_COMPARE(dayState(may4), 2);
    QTRY_COMPARE(dayState(may3), 0);
    m_library->setSortMode(CaptureFilterModel::OldestFirst);
    QTRY_COMPARE(m_library->rowOf(added.at(7)), 0);
    QTRY_COMPARE(rows->sectionOfItem(0).value(QStringLiteral("label")).toString(), may3);
    // Each header describes its own day after the move, not the day that
    // used to start at its first row.
    QTRY_COMPARE(dayState(may3), 0);
    QTRY_COMPARE(dayState(may4), 2);
    QTest::qWait(100);
    QCOMPARE(dayState(may3), 0);
    QCOMPARE(dayState(may4), 2);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    // Shift+click again: the range runs from the anchor (0, now row 7) to 2.
    clickCard(2, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2}));
    QCOMPARE(grid->property("checkedCount").toInt(), 3);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTRY_COMPARE(dayState(may4), 1);
    QTRY_COMPARE(dayState(may3), 0);
    // And back, with the day half selected.
    m_library->setSortMode(CaptureFilterModel::NewestFirst);
    QTRY_COMPARE(m_library->rowOf(added.at(0)), 0);
    QTRY_COMPARE(rows->sectionOfItem(0).value(QStringLiteral("label")).toString(), may4);
    QTest::qWait(100);
    QCOMPARE(dayState(may4), 1);
    QCOMPARE(dayState(may3), 0);
    clickCard(5, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({0, 1, 2, 3, 4, 5}));
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // A range on 3 May, then a capture arriving at the very top.
    clickCard(5, Qt::NoModifier);
    clickCard(7, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({5, 6, 7}));
    const QString arriving = dir.filePath(QStringLiteral("screenshot-2019-05-04_15-00-00.png"));
    QVERIFY(picture.save(arriving));
    m_disposablePaths.append(arriving);
    m_captures->addExtraFiles({arriving});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(arriving) == 0, 10000);
    QTRY_COMPARE(rows->sectionOfItem(0).value(QStringLiteral("count")).toInt(), 6);
    QTest::qWait(100);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTRY_COMPARE(dayState(may3), 2);
    QTRY_COMPARE(dayState(may4), 0);
    clickCard(6, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({5, 6}));
    QCOMPARE(grid->property("checkedCount").toInt(), 2);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTRY_COMPARE(dayState(may3), 1);

    // A rescan that takes the new capture away again.
    QVERIFY(QFile::remove(arriving));
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(arriving) < 0 && !m_captures->scanning(), 15000);
    QTRY_COMPARE(m_library->count(), 8);
    QTest::qWait(100);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    clickCard(7, Qt::ShiftModifier);
    QTRY_COMPARE(checkedPaths(), expected({5, 6, 7}));
    QCOMPARE(grid->property("checkedCount").toInt(), 3);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTRY_COMPARE(dayState(may3), 2);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // A band held while a capture arrives above it: the count stays true
    // as the band goes on.
    QMetaObject::invokeMethod(item("reviewSetsList"), "positionViewAtBeginning");
    QTest::qWait(100);
    QQuickItem* zero = nullptr;
    QQuickItem* two = nullptr;
    QTRY_VERIFY((zero = cardFor(added.at(0))) != nullptr && (two = cardFor(added.at(2))) != nullptr);
    const QPoint from = zero->mapToScene(QPointF(zero->width() + 3, zero->height() / 2)).toPoint();
    const QPoint to = centre(two) + QPoint(0, 20);
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= 4; ++step) QTest::mouseMove(m_window, from + (to - from) * step / 6, 16);
    QTRY_VERIFY(item("rubberBandRect")->isVisible());
    QTRY_VERIFY(grid->property("checkedCount").toInt() > 0);
    const QString early = dir.filePath(QStringLiteral("screenshot-2019-05-04_16-00-00.png"));
    QVERIFY(picture.save(early));
    m_disposablePaths.append(early);
    m_captures->addExtraFiles({early});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(early) == 0, 10000);
    QTest::qWait(100);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    for (int step = 5; step <= 6; ++step) QTest::mouseMove(m_window, from + (to - from) * step / 6, 16);
    QTest::mouseMove(m_window, to + QPoint(4, 0), 16);
    QTest::qWait(50);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, to + QPoint(4, 0), 16);
    QTRY_VERIFY(!item("rubberBandRect")->isVisible());
    QVERIFY(grid->property("checkedCount").toInt() > 0);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    // A Shift+click after the band counts from its anchor.
    clickCard(7, Qt::ShiftModifier);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QVERIFY(checkedPaths().contains(added.at(7)));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
  }

  // Narrowing the window leaves rows holding more tile slots than they
  // show until the pool is trimmed, and short rows always do. A hidden slot
  // takes no input, says nothing to a screen reader and asks for no image.
  void narrowingLeavesHiddenTilesInert() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage picture(40, 30, QImage::Format_RGB32);
    picture.fill(QColor(120, 60, 150));
    QStringList added;
    // Nine on one day and one on the next, so full and short rows occur.
    for (const auto& [date, count] : {std::pair{QDate(2019, 6, 2), 9}, std::pair{QDate(2019, 6, 1), 1}}) {
      for (int hour = 20; hour > 20 - count; --hour) {
        const QString path = dir.filePath(QStringLiteral("screenshot-%1_%2-00-00.png")
                                              .arg(date.toString(Qt::ISODate))
                                              .arg(hour, 2, 10, QLatin1Char('0')));
        QVERIFY(picture.save(path));
        added.append(path);
      }
    }
    for (const QString& path : added) m_disposablePaths.append(path);
    m_captures->addExtraFiles(added);
    auto* grid = item("library");
    QQuickItem* view = item("reviewSets");
    const auto cleanup = qScopeGuard([&] {
      QTest::mouseMove(m_window, QPoint(5, 5));
      QMetaObject::invokeMethod(grid, "clearChecked");
      m_library->setFolderFilter({});
      m_window->resize(1280, 820);
      waitForLayout(QSize(1280, 820));
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(added.cbegin(), added.cend(), [&](const QString& path) {
      return m_library->rowOf(path) >= 0;
    }), 10000);
    m_library->setFolderFilter(dir.path());
    QTRY_COMPARE(m_library->count(), added.size());
    QTRY_VERIFY(view->isVisible());
    m_window->resize(1280, 820);
    waitForLayout(QSize(1280, 820));
    const int wide = grid->property("columns").toInt();
    QTRY_COMPARE(view->property("poolColumns").toInt(), wide);
    m_window->resize(620, 820);
    QTRY_VERIFY(grid->property("columns").toInt() < wide);
    const int narrow = grid->property("columns").toInt();
    QTest::qWait(100);
    // Until the pool is trimmed every row keeps the wide count of slots.
    QCOMPARE(view->property("poolColumns").toInt(), wide);

    const auto checkHidden = [&](const char* stage) {
      QList<QQuickItem*> cells;
      find(item("reviewSetsList"), [&cells](QQuickItem* candidate) {
        if (candidate->property("used").isValid() && candidate->property("row").isValid() &&
            candidate->parentItem() && candidate->parentItem()->parentItem() &&
            shownByItsView(candidate))
          cells.append(candidate);
        return false;
      });
      int hidden = 0;
      int shown = 0;
      for (QQuickItem* cell : std::as_const(cells)) {
        auto* card = find(cell, [](QQuickItem* candidate) {
          return candidate->property("dragPaths").isValid();
        });
        QVERIFY2(card, stage);
        if (cell->property("used").toBool()) {
          ++shown;
          continue;
        }
        ++hidden;
        QVERIFY2(!cell->isVisible() && !card->isVisible(), stage);
        QVERIFY2(card->property("path").toString().isEmpty(), stage);
        // No thumbnail request: every image in the slot has no source.
        find(card, [stage](QQuickItem* candidate) {
          if (candidate->property("sourceSize").isValid() && candidate->property("source").isValid()) {
            const QString source = candidate->property("source").toUrl().toString();
            if (!source.isEmpty()) qWarning() << stage << "hidden slot requests" << source;
            QTest::qVerify(source.isEmpty(), "source.isEmpty()", stage, __FILE__, __LINE__);
          }
          return false;
        });
        // Qt Quick lists hidden items to assistive technology marked
        // invisible, which screen readers skip; nothing here may say otherwise.
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(card);
        QVERIFY2(!accessible || accessible->state().invisible, stage);
        QVERIFY2(!accessible || accessible->text(QAccessible::Name).isEmpty(), stage);
      }
      QVERIFY2(shown > 0 && hidden > 0, qPrintable(QStringLiteral("%1: %2 shown, %3 hidden")
                                                        .arg(QLatin1String(stage)).arg(shown).arg(hidden)));
    };
    checkHidden("before trim");
    if (QTest::currentTestFailed()) return;

    // A click where a hidden slot would be, past the end of a short row,
    // picks nothing.
    QMetaObject::invokeMethod(grid, "clearChecked");
    const int before = grid->property("currentIndex").toInt();
    // The short row is the last one; bring it into view.
    QMetaObject::invokeMethod(grid, "scrollTo", Q_ARG(QVariant, 1e9));
    QTest::qWait(100);
    QQuickItem* lastCard = nullptr;
    QTRY_VERIFY((lastCard = cardFor(added.last())) != nullptr);
    QVERIFY(centre(lastCard).y() > 0 && centre(lastCard).y() < m_window->height());
    const QPoint past = lastCard->mapToScene(QPointF(lastCard->width() * 1.5 + 12, lastCard->height() / 2)).toPoint();
    QVERIFY(past.x() < m_window->width() - 20);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, past);
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
    QCOMPARE(grid->property("currentIndex").toInt(), before);
    QCOMPARE(grid->property("checkedCount").toInt(), 0);
    QVERIFY(!item("detail")->isVisible());

    // Once the resize settles the rows hold only what fits.
    QTRY_COMPARE(view->property("poolColumns").toInt(), narrow);
    QTest::qWait(100);
    checkHidden("after trim");
  }

  // Section selection and the band at the edges: Select pressed while a
  // search is waiting, a band that touches nothing, a band held while a day
  // arrives above it, two quick Ctrl+clicks, and the top row kept through a
  // reshuffle when it is a section's first tile row.
  void sectionSelectAndBandEdgeCases() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage picture(40, 30, QImage::Format_RGB32);
    picture.fill(QColor(60, 110, 170));
    // Newest first: twelve days in May 2019, each with a 20:00 capture.
    const QList<int> sizes{5, 2, 5, 1, 6, 3, 4, 6, 2, 5, 6, 3};
    QStringList added;
    QList<int> dayFirst;
    for (int day = 0; day < sizes.size(); ++day) {
      dayFirst.append(added.size());
      const QDate date = QDate(2019, 5, 28).addDays(-day);
      for (int hour = 20; hour > 20 - sizes.at(day); --hour) {
        const QString path = dir.filePath(QStringLiteral("screenshot-%1_%2-00-00.png")
                                              .arg(date.toString(Qt::ISODate))
                                              .arg(hour, 2, 10, QLatin1Char('0')));
        QVERIFY(picture.save(path));
        added.append(path);
      }
    }
    for (const QString& path : added) m_disposablePaths.append(path);
    m_captures->addExtraFiles(added);
    auto* grid = item("library");
    auto* rows = qobject_cast<SectionRows*>(m_library->sectionRows());
    QVERIFY(rows);
    const auto cleanup = qScopeGuard([&] {
      QTest::mouseMove(m_window, QPoint(5, 5));
      invoke("dismissTopLayer");
      QMetaObject::invokeMethod(grid, "clearChecked");
      m_library->setSearchText({});
      m_library->setSortMode(CaptureFilterModel::NewestFirst);
      m_library->setFolderFilter({});
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(added.cbegin(), added.cend(), [&](const QString& path) {
      return m_library->rowOf(path) >= 0;
    }), 10000);
    m_library->setFolderFilter(dir.path());
    QTRY_COMPARE(m_library->count(), added.size());
    QTRY_VERIFY(item("reviewSets")->isVisible());
    QTRY_COMPARE(rows->sectionCount(), sizes.size());
    QQuickItem* view = item("reviewSets");
    QQuickItem* list = item("reviewSetsList");
    QTest::qWait(200);
    QString why;
    const auto checkedPaths = [&] {
      QVariant paths;
      QMetaObject::invokeMethod(grid, "checkedPaths", Q_RETURN_ARG(QVariant, paths));
      QStringList result = paths.value<QJSValue>().toVariant().toStringList();
      result.sort();
      return result;
    };
    const auto top = [&] {
      QQuickItem* row = nullptr;
      const qreal contentY = list->property("contentY").toReal();
      QMetaObject::invokeMethod(list, "itemAt", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(qreal, 20),
                                Q_ARG(qreal, contentY + 1));
      return row ? std::pair{row->property("index").toInt(), qRound(contentY - row->y())}
                 : std::pair{-1, 0};
    };
    const auto headerRowOf = [&](int item) {
      return rows->sectionOfItem(item).value(QStringLiteral("headerRow")).toInt();
    };

    // Ten pixels into the first tile row of the fourth day: a reshuffle
    // brings back that row at that offset, not its header.
    const int firstTiles = headerRowOf(dayFirst.at(3)) + 1;
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, firstTiles),
                              Q_ARG(int, 0 /* ListView.Beginning */));
    list->setProperty("contentY", list->property("contentY").toReal() + 10);
    QTest::qWait(50);
    const auto before = top();
    QCOMPARE(before.first, firstTiles);
    QCOMPARE(before.second, 10);
    QMetaObject::invokeMethod(view, "rememberTop");
    QMetaObject::invokeMethod(view, "restoreTop");
    QTest::qWait(50);
    QCOMPARE(top(), before);

    // Select pressed with a search still waiting: the search applies first,
    // then the day selected is the day as the search leaves it. The first
    // day's old rows 0 to 4 would hold five days' 20:00 captures.
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    QQuickItem* select = nullptr;
    QTRY_VERIFY([&] {
      QQuickItem* header = nullptr;
      QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, header), Q_ARG(int, 0));
      select = header ? find(header, [](QQuickItem* candidate) {
        return candidate->objectName() == QStringLiteral("sectionSelect") && candidate->isVisible();
      }) : nullptr;
      return select != nullptr;
    }());
    m_library->queueSearchText(QStringLiteral("20-00-00"));
    QCOMPARE(m_library->count(), added.size());
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(select));
    QCOMPARE(m_library->count(), sizes.size());
    QTRY_COMPARE(checkedPaths(), QStringList{added.at(0)});
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QMetaObject::invokeMethod(grid, "clearChecked");
    m_library->setSearchText({});
    QTRY_COMPARE(m_library->count(), added.size());
    QTRY_COMPARE(rows->sectionCount(), sizes.size());
    QTest::qWait(150);

    // Two Ctrl+clicks on one tile, however quick, toggle it twice.
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = cardFor(added.at(1))) != nullptr);
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::ControlModifier, centre(card));
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
    QCOMPARE(grid->property("checkedCount").toInt(), 0);
    QVERIFY(!item("detail")->isVisible());

    // A band that touches no tile, drawn in the empty end of a short row,
    // replaces a Shift range with nothing.
    QStringList firstThree{added.at(0), added.at(1), added.at(2)};
    firstThree.sort();
    QTRY_VERIFY((card = cardFor(added.at(0))) != nullptr);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(card));
    QTRY_VERIFY((card = cardFor(added.at(2))) != nullptr);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::ShiftModifier, centre(card));
    QTRY_COMPARE(checkedPaths(), firstThree);
    QQuickItem* last = nullptr;
    QTRY_VERIFY((last = cardFor(added.at(dayFirst.at(1) + 1))) != nullptr);
    const qreal cell = grid->property("cellWidth").toReal();
    QVERIFY(grid->property("columns").toInt() >= 4);
    const QPoint emptyFrom = last->mapToScene(QPointF(last->width() + cell * 0.5, last->height() * 0.3)).toPoint();
    const QPoint emptyTo = emptyFrom + QPoint(qRound(cell), qRound(last->height() * 0.3));
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, emptyFrom);
    for (int step = 1; step <= 5; ++step) QTest::mouseMove(m_window, emptyFrom + (emptyTo - emptyFrom) * step / 5, 16);
    QTRY_VERIFY(item("rubberBandRect")->isVisible());
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, emptyTo, 16);
    QTRY_VERIFY(!item("rubberBandRect")->isVisible());
    QCOMPARE(grid->property("checkedCount").toInt(), 0);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    // Shift+click on the same end again builds on that empty selection.
    QTRY_VERIFY((card = cardFor(added.at(2))) != nullptr);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::ShiftModifier, centre(card));
    QTRY_COMPARE(checkedPaths(), firstThree);
    QCOMPARE(grid->property("checkedCount").toInt(), 3);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QMetaObject::invokeMethod(grid, "clearChecked");

    // A band held while a new day arrives above the view: what it selects
    // stays inside what it draws.
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, firstTiles),
                              Q_ARG(int, 0 /* ListView.Beginning */));
    QTest::qWait(150);
    QQuickItem* startCard = nullptr;
    QQuickItem* endCard = nullptr;
    QTRY_VERIFY((startCard = cardFor(added.at(dayFirst.at(3)))) != nullptr &&
                (endCard = cardFor(added.at(dayFirst.at(4) + 1))) != nullptr);
    const QPoint from = startCard->mapToScene(QPointF(startCard->width() + 3, startCard->height() / 2)).toPoint();
    const QPoint to = centre(endCard);
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= 4; ++step) QTest::mouseMove(m_window, from + (to - from) * step / 6, 16);
    QTRY_VERIFY(item("rubberBandRect")->isVisible());
    QTRY_VERIFY(grid->property("checkedCount").toInt() > 0);
    const QString arriving = dir.filePath(QStringLiteral("screenshot-2019-05-29_12-00-00.png"));
    QVERIFY(picture.save(arriving));
    m_disposablePaths.append(arriving);
    m_captures->addExtraFiles({arriving});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(arriving) == 0, 10000);
    QTRY_COMPARE(rows->sectionCount(), sizes.size() + 1);
    QTest::qWait(150);
    for (int step = 5; step <= 6; ++step) QTest::mouseMove(m_window, from + (to - from) * step / 6, 16);
    QTest::mouseMove(m_window, to + QPoint(3, 3), 16);
    QTest::qWait(50);
    QQuickItem* bandRect = item("rubberBandRect");
    QVERIFY(bandRect->isVisible());
    const QRectF drawn = bandRect->mapRectToScene(bandRect->boundingRect());
    const QStringList selected = checkedPaths();
    QVERIFY(!selected.isEmpty());
    for (const QString& path : selected) {
      QQuickItem* tile = cardFor(path);
      QVERIFY2(tile, qPrintable(path + QStringLiteral(" is selected but not on screen")));
      const QRectF rect = tile->mapRectToScene(tile->boundingRect());
      QVERIFY2(rect.intersects(drawn),
               qPrintable(QStringLiteral("%1 at %2,%3 is outside the band %4,%5 %6x%7")
                              .arg(path).arg(rect.x()).arg(rect.y()).arg(drawn.x()).arg(drawn.y())
                              .arg(drawn.width()).arg(drawn.height())));
    }
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, to + QPoint(3, 3), 16);
    QTRY_VERIFY(!item("rubberBandRect")->isVisible());
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QMetaObject::invokeMethod(grid, "clearChecked");
    added.prepend(arriving);

    // The same in the flat grid: captures leaving above while a band is held
    // deep in the grid.
    m_library->setSortMode(CaptureFilterModel::RatingFirst);
    QQuickItem* flat = item("flatGrid");
    QTRY_VERIFY(flat->isVisible());
    QTest::qWait(300);
    const qreal flatCell = flat->property("cellHeight").toReal();
    QMetaObject::invokeMethod(grid, "scrollTo", Q_ARG(QVariant, flatCell * 3));
    QTest::qWait(150);
    const int columns = grid->property("columns").toInt();
    const auto visibleCard = [&](int row, int column) -> QQuickItem* {
      int index = -1;
      QMetaObject::invokeMethod(flat, "indexAt", Q_RETURN_ARG(int, index),
                                Q_ARG(qreal, flat->property("contentX").toReal() + 4),
                                Q_ARG(qreal, flat->property("contentY").toReal() + 4));
      return index < 0 ? nullptr : cardFor(m_library->pathAt(index + row * columns + column));
    };
    QQuickItem* flatStart = nullptr;
    QQuickItem* flatEnd = nullptr;
    QTRY_VERIFY((flatStart = visibleCard(1, 0)) != nullptr && (flatEnd = visibleCard(2, 1)) != nullptr);
    const QPoint flatFrom = flatStart->mapToScene(QPointF(flatStart->width() + 3, flatStart->height() / 2)).toPoint();
    const QPoint flatTo = centre(flatEnd);
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, flatFrom);
    for (int step = 1; step <= 4; ++step) QTest::mouseMove(m_window, flatFrom + (flatTo - flatFrom) * step / 6, 16);
    QTRY_VERIFY(item("rubberBandRect")->isVisible());
    QTRY_VERIFY(grid->property("checkedCount").toInt() > 0);
    // A rescan finds the seven newest gone: rows leave above the band and
    // the grid moves where its rows start.
    const qreal originBefore = flat->property("originY").toReal();
    const int remaining = m_library->count() - 7;
    for (int i = 0; i < 7; ++i) QVERIFY(QFile::remove(m_library->pathAt(i)));
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(!m_captures->scanning() && m_library->count() == remaining, 15000);
    QTest::qWait(300);
    QVERIFY2(flat->property("originY").toReal() != originBefore,
             "the grid kept its origin, so this no longer tests a moved origin");
    for (int step = 5; step <= 6; ++step) QTest::mouseMove(m_window, flatFrom + (flatTo - flatFrom) * step / 6, 16);
    QTest::mouseMove(m_window, flatTo + QPoint(3, 3), 16);
    QTest::qWait(50);
    QVERIFY(bandRect->isVisible());
    const QRectF flatDrawn = bandRect->mapRectToScene(bandRect->boundingRect());
    const QStringList flatSelected = checkedPaths();
    QVERIFY(!flatSelected.isEmpty());
    for (const QString& path : flatSelected) {
      QQuickItem* tile = cardFor(path);
      QVERIFY2(tile, qPrintable(path + QStringLiteral(" is selected but not on screen")));
      const QRectF rect = tile->mapRectToScene(tile->boundingRect());
      QVERIFY2(rect.intersects(flatDrawn),
               qPrintable(QStringLiteral("%1 at %2,%3 is outside the band %4,%5 %6x%7")
                              .arg(path).arg(rect.x()).arg(rect.y()).arg(flatDrawn.x()).arg(flatDrawn.y())
                              .arg(flatDrawn.width()).arg(flatDrawn.height())));
    }
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, flatTo + QPoint(3, 3), 16);
    QTRY_VERIFY(!item("rubberBandRect")->isVisible());
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QMetaObject::invokeMethod(grid, "clearChecked");

    // From a name sort to a date sort, the sections open on the current
    // tile. Names put the oldest capture first; by date it is the last.
    m_library->setSortMode(CaptureFilterModel::NameAscending);
    QTRY_VERIFY(flat->isVisible());
    QTest::qWait(200);
    const QString oldest = m_library->pathAt(0);
    QCOMPARE(oldest, added.last());
    grid->setProperty("currentIndex", 0);
    QTRY_COMPARE(grid->property("selectedPath").toString(), oldest);
    m_library->setSortMode(CaptureFilterModel::NewestFirst);
    QTRY_VERIFY(view->isVisible());
    QTRY_COMPARE(grid->property("selectedPath").toString(), oldest);
    QTRY_VERIFY_WITH_TIMEOUT([&] {
      QQuickItem* current = cardFor(oldest);
      if (!current) return false;
      const QRectF viewport = list->mapRectToScene(list->boundingRect());
      const QRectF rect = current->mapRectToScene(current->boundingRect());
      return rect.top() >= viewport.top() - 1 && rect.bottom() <= viewport.bottom() + 1;
    }(), 3000);
  }

  // A longer dated library: exact jumps and the scrollbar, the page keys,
  // Shift+X and the mixed state of a day's control, Ctrl+wheel over the
  // sections, a band that scrolls, an anchor a search hides, and a capture
  // arriving while the view is scrolled.
  void dateSectionsScrollKeysAndKeepTheirPlace() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage picture(40, 30, QImage::Format_RGB32);
    picture.fill(QColor(150, 70, 40));
    // Newest first: twelve days in May 2019 holding 48 tiles. The first day
    // fills a row of five, so a sixth capture there adds a row.
    const QList<int> sizes{5, 2, 5, 1, 6, 3, 4, 6, 2, 5, 6, 3};
    QStringList added;
    QList<int> dayFirst;
    for (int day = 0; day < sizes.size(); ++day) {
      dayFirst.append(added.size());
      const QDate date = QDate(2019, 5, 28).addDays(-day);
      for (int hour = 20; hour > 20 - sizes.at(day); --hour) {
        const QString path = dir.filePath(QStringLiteral("screenshot-%1_%2-00-00.png")
                                              .arg(date.toString(Qt::ISODate))
                                              .arg(hour, 2, 10, QLatin1Char('0')));
        QVERIFY(picture.save(path));
        added.append(path);
      }
    }
    for (const QString& path : added) m_disposablePaths.append(path);
    m_captures->addExtraFiles(added);
    auto* grid = item("library");
    auto* rows = qobject_cast<SectionRows*>(m_library->sectionRows());
    QVERIFY(rows);
    const int tileWidth = m_settings->tileWidth();
    const auto cleanup = qScopeGuard([&] {
      QTest::mouseMove(m_window, QPoint(5, 5));
      invoke("dismissTopLayer");
      QMetaObject::invokeMethod(grid, "clearChecked");
      m_settings->setTileWidth(tileWidth);
      m_library->setSearchText({});
      m_library->setFolderFilter({});
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(added.cbegin(), added.cend(), [&](const QString& path) {
      return m_library->rowOf(path) >= 0;
    }), 10000);
    m_library->setFolderFilter(dir.path());
    QTRY_COMPARE(m_library->count(), added.size());
    QTRY_VERIFY(item("reviewSets")->isVisible());
    QTRY_COMPARE(rows->sectionCount(), sizes.size());
    QQuickItem* list = item("reviewSetsList");
    QVERIFY(list);
    QTest::qWait(200);

    const auto scrollExtent = [&] {
      QVariant value;
      QMetaObject::invokeMethod(grid, "scrollExtent", Q_RETURN_ARG(QVariant, value));
      return value.toDouble();
    };
    const auto scrollPosition = [&] {
      QVariant value;
      QMetaObject::invokeMethod(grid, "scrollPosition", Q_RETURN_ARG(QVariant, value));
      return value.toDouble();
    };
    const auto scrollTo = [&](double y) {
      QMetaObject::invokeMethod(grid, "scrollTo", Q_ARG(QVariant, y));
      QTest::qWait(60);
    };
    // The row at the top of the viewport and how far into it the view is.
    const auto top = [&] {
      QQuickItem* row = nullptr;
      const qreal contentY = list->property("contentY").toReal();
      QMetaObject::invokeMethod(list, "itemAt", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(qreal, 20),
                                Q_ARG(qreal, contentY));
      return row ? std::pair{row->property("index").toInt(), qRound(contentY - row->y())}
                 : std::pair{-1, 0};
    };
    const double extent = scrollExtent();
    QVERIFY2(extent > list->height(), qPrintable(QString::number(extent)));
    // The rows' exact heights add up to what the list holds once laid out.
    const double headers = rows->sectionCount() * 40.0;
    const double cellHeight = grid->property("cellWidth").toDouble() > 0
                                  ? std::round(grid->property("cellWidth").toDouble() * 0.68) : 0;
    QCOMPARE(rows->totalHeight(40, cellHeight),
             headers + (rows->rowCount() - rows->sectionCount()) * cellHeight);

    // The same position shows the same row however the view got there.
    scrollTo(extent / 2);
    const auto middle = top();
    QVERIFY(middle.first > 0);
    QVERIFY(std::abs(scrollPosition() - extent / 2) < 1);
    QVERIFY2(std::abs(rows->rowY(middle.first, 40, cellHeight) + middle.second - extent / 2) <= 1,
             qPrintable(QStringLiteral("row %1 at %2 + %3, wanted %4; contentY %5 originY %6 cell %7 summary %8 header %9 exact %10")
                            .arg(middle.first)
                            .arg(rows->rowY(middle.first, 40, cellHeight))
                            .arg(middle.second)
                            .arg(extent / 2)
                            .arg(list->property("contentY").toReal())
                            .arg(list->property("originY").toReal())
                            .arg(cellHeight)
                            .arg(item("reviewSets")->property("summaryHeight").toReal())
                            .arg(list->property("headerItem").value<QQuickItem*>() != nullptr)
                            .arg(item("reviewSets")->property("exactHeight").toReal())));
    // The end is exact too, not where the list estimates it to be.
    scrollTo(extent);
    QVERIFY2(std::abs(scrollPosition() - extent) < 1, qPrintable(QString::number(scrollPosition())));
    scrollTo(extent / 5);
    scrollTo(extent / 2);
    // The same row, at the same offset give or take the rounding of a
    // fractional position to whole pixels.
    QCOMPARE(top().first, middle.first);
    QVERIFY2(std::abs(top().second - middle.second) <= 1,
             qPrintable(QStringLiteral("%1 against %2").arg(top().second).arg(middle.second)));
    // The scrollbar reports that exact position.
    QQuickItem* scrollbar = item("sectionsScrollBar");
    QVERIFY(scrollbar);
    QVERIFY(std::abs(scrollbar->property("position").toDouble() * (extent + list->height())
                     - extent / 2) < 2);

    // A capture arriving while scrolled lands above without moving what is
    // on screen.
    const auto firstVisibleCard = [&]() -> QQuickItem* {
      const QRectF viewport = list->mapRectToScene(list->boundingRect());
      QQuickItem* best = nullptr;
      qreal bestY = 0;
      for (const QString& path : added) {
        QQuickItem* card = cardFor(path);
        if (!card) continue;
        const QRectF rect = card->mapRectToScene(card->boundingRect());
        if (rect.top() < viewport.top() || rect.bottom() > viewport.bottom()) continue;
        if (!best || rect.top() < bestY) {
          best = card;
          bestY = rect.top();
        }
      }
      return best;
    };
    QQuickItem* anchorCard = nullptr;
    QTRY_VERIFY((anchorCard = firstVisibleCard()) != nullptr);
    const QString anchorPath = anchorCard->property("path").toString();
    const QPointF anchorAt = anchorCard->mapToScene(QPointF());
    QSignalSpy removed(rows, &QAbstractItemModel::rowsRemoved);
    const auto before = top();
    const qreal beforeY = list->property("contentY").toReal();
    const QString arriving = dir.filePath(QStringLiteral("screenshot-2019-05-28_23-00-00.png"));
    QVERIFY(picture.save(arriving));
    m_disposablePaths.append(arriving);
    m_captures->addExtraFiles({arriving});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(arriving) == 0, 10000);
    QTRY_COMPARE(rows->sectionOfItem(0).value(QStringLiteral("count")).toInt(), sizes.first() + 1);
    QTest::qWait(150);
    QVERIFY(removed.isEmpty());
    QQuickItem* still = nullptr;
    QTRY_VERIFY2((still = cardFor(anchorPath)) != nullptr,
                 qPrintable(QStringLiteral("top %1+%2 at %3 became %4+%5 at %6; current %7")
                                .arg(before.first).arg(before.second).arg(beforeY)
                                .arg(top().first).arg(top().second)
                                .arg(list->property("contentY").toReal())
                                .arg(grid->property("currentIndex").toInt())));
    QVERIFY2((still->mapToScene(QPointF()) - anchorAt).manhattanLength() < 1,
             qPrintable(QStringLiteral("%1,%2 moved to %3,%4")
                            .arg(anchorAt.x()).arg(anchorAt.y())
                            .arg(still->mapToScene(QPointF()).x())
                            .arg(still->mapToScene(QPointF()).y())));
    added.prepend(arriving);
    for (int& first : dayFirst) ++first;
    dayFirst[0] = 0;

    // Page Down moves a page in the same column, through the headers;
    // Shift extends, Page Up comes back.
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = cardFor(added.at(1))) != nullptr);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, centre(card));
    QTRY_COMPARE(grid->property("currentIndex").toInt(), 1);
    QTRY_VERIFY(list->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_PageDown);
    const int paged = grid->property("currentIndex").toInt();
    QVERIFY2(paged > 1, qPrintable(QString::number(paged)));
    const auto columnOf = [&](int item) {
      const int row = rows->rowOfItem(item);
      return item - rows->rowAt(row).value(QStringLiteral("first")).toInt();
    };
    QCOMPARE(columnOf(paged), std::min(columnOf(1), rows->rowAt(rows->rowOfItem(paged))
                                                         .value(QStringLiteral("count")).toInt() - 1));
    QTest::keyClick(m_window, Qt::Key_PageUp);
    const int back = grid->property("currentIndex").toInt();
    QVERIFY(back < paged);
    QTest::keyClick(m_window, Qt::Key_PageDown, Qt::ShiftModifier);
    const int extended = grid->property("currentIndex").toInt();
    QVERIFY(extended > back);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), extended - back + 1);
    QString why;
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_Home);
    QTRY_COMPARE(grid->property("currentIndex").toInt(), 0);

    // Shift+X selects the current tile's day, and again clears it.
    QTest::keyClick(m_window, Qt::Key_X, Qt::ShiftModifier);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), sizes.first() + 1);
    const auto selectFor = [&](int sectionFirst) -> QQuickItem* {
      const int header = rows->sectionOfItem(sectionFirst).value(QStringLiteral("headerRow")).toInt();
      QQuickItem* row = nullptr;
      QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, header));
      return row ? find(row, [](QQuickItem* candidate) {
        return candidate->objectName() == QStringLiteral("sectionSelect") && candidate->isVisible();
      }) : nullptr;
    };
    QQuickItem* select = nullptr;
    QTRY_VERIFY((select = selectFor(0)) != nullptr);
    QTRY_VERIFY(QAccessible::queryAccessibleInterface(select)->state().checked);
    QVERIFY(!QAccessible::queryAccessibleInterface(select)->state().checkStateMixed);
    // X on one tile leaves the day partly selected, which the control says.
    QTest::keyClick(m_window, Qt::Key_X);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), sizes.first());
    QTRY_VERIFY(QAccessible::queryAccessibleInterface(selectFor(0))->state().checkStateMixed);
    QVERIFY(!QAccessible::queryAccessibleInterface(selectFor(0))->state().checked);
    QTest::keyClick(m_window, Qt::Key_X, Qt::ShiftModifier);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), sizes.first() + 1);
    QTest::keyClick(m_window, Qt::Key_X, Qt::ShiftModifier);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // The section counts follow a Shift range across days, both ways.
    QTest::keyClick(m_window, Qt::Key_Right);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(grid->property("currentIndex").toInt(), 2);
    QTest::keyClick(m_window, Qt::Key_End, Qt::ShiftModifier);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), added.size() - 2);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTRY_VERIFY(selectFor(0) && QAccessible::queryAccessibleInterface(selectFor(0))->state().checkStateMixed);
    QTRY_VERIFY(selectFor(dayFirst.at(1))
                && QAccessible::queryAccessibleInterface(selectFor(dayFirst.at(1)))->state().checked);
    QTest::keyClick(m_window, Qt::Key_Home, Qt::ShiftModifier);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 3);
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTRY_VERIFY(selectFor(dayFirst.at(1))
                && !QAccessible::queryAccessibleInterface(selectFor(dayFirst.at(1)))->state().checked
                && !QAccessible::queryAccessibleInterface(selectFor(dayFirst.at(1)))->state().checkStateMixed);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // A drag takes the selection with it, worked out as the drag starts.
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, added.at(2)));
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, added.at(3)));
    QVariant dragged;
    QMetaObject::invokeMethod(grid, "dragPathsFor", Q_RETURN_ARG(QVariant, dragged),
                              Q_ARG(QVariant, added.at(2)));
    QStringList draggedPaths = dragged.value<QJSValue>().toVariant().toStringList();
    draggedPaths.sort();
    QStringList both{added.at(2), added.at(3)};
    both.sort();
    QCOMPARE(draggedPaths, both);
    QMetaObject::invokeMethod(grid, "dragPathsFor", Q_RETURN_ARG(QVariant, dragged),
                              Q_ARG(QVariant, added.at(4)));
    QCOMPARE(dragged.value<QJSValue>().toVariant().toStringList(), QStringList{added.at(4)});
    QMetaObject::invokeMethod(grid, "clearChecked");

    // A search that hides the anchor: Shift+click extends from the current
    // tile instead, and only shown tiles are selected.
    QTRY_VERIFY((card = cardFor(added.at(1))) != nullptr);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::ControlModifier, centre(card));
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 1);
    QCOMPARE(grid->property("anchorPath").toString(), added.at(1));
    m_library->setSearchText(QStringLiteral("2019-05-26"));
    QTRY_COMPARE(m_library->count(), sizes.at(2));
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);
    QTest::qWait(150);
    QTRY_VERIFY((card = cardFor(added.at(dayFirst.at(2) + 3))) != nullptr);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::ShiftModifier, centre(card));
    QTRY_VERIFY(grid->property("checkedCount").toInt() >= 1);
    QVariant checked;
    QMetaObject::invokeMethod(grid, "checkedPaths", Q_RETURN_ARG(QVariant, checked));
    const QStringList checkedList = checked.value<QJSValue>().toVariant().toStringList();
    QVERIFY(checkedList.contains(added.at(dayFirst.at(2) + 3)));
    for (const QString& path : checkedList) QVERIFY(m_library->rowOf(path) >= 0);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);
    m_library->setSearchText({});
    QTRY_COMPARE(m_library->count(), added.size());
    QTest::qWait(150);

    // A band dragged to the bottom edge scrolls on and keeps selecting.
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    QTRY_VERIFY((card = cardFor(added.at(0))) != nullptr);
    const QPoint from = card->mapToScene(QPointF(card->width() + 3, card->height() / 2)).toPoint();
    const QRectF viewport = list->mapRectToScene(list->boundingRect());
    const QPoint edge(from.x() + 40, qRound(viewport.bottom()) - 4);
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= 6; ++step) QTest::mouseMove(m_window, from + (edge - from) * step / 6, 16);
    QTRY_VERIFY(item("rubberBandRect")->isVisible());
    const qreal startY = list->property("contentY").toReal();
    QTRY_VERIFY_WITH_TIMEOUT(list->property("contentY").toReal() > startY + 4 * cellHeight, 5000);
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, edge, 16);
    QTRY_VERIFY(!item("rubberBandRect")->isVisible());
    // It selected tiles that were below the viewport when the drag began.
    QVariant banded;
    QMetaObject::invokeMethod(grid, "checkedPaths", Q_RETURN_ARG(QVariant, banded));
    double lowest = 0;
    for (const QString& path : banded.value<QJSValue>().toVariant().toStringList()) {
      lowest = std::max(lowest, rows->rowY(rows->rowOfItem(m_library->rowOf(path)), 40, cellHeight));
    }
    QVERIFY2(lowest > list->height(), qPrintable(QString::number(lowest)));
    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 0);

    // Ctrl+wheel resizes the tiles over the sections; a plain wheel scrolls.
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    QElapsedTimer wheelClock;
    wheelClock.start();
    const auto wheel = [&](int delta, Qt::KeyboardModifiers modifiers) {
      const QPointF at = list->mapToScene(QPointF(list->width() / 2, list->height() / 2));
      QWheelEvent event(at, m_window->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, delta),
                        Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
      event.setTimestamp(1000 + wheelClock.elapsed());
      QCoreApplication::sendEvent(m_window, &event);
      QTest::qWait(80);
    };
    wheel(120, Qt::ControlModifier);
    QTRY_COMPARE(m_settings->tileWidth(), tileWidth + 40);
    wheel(-120, Qt::ControlModifier);
    QTRY_COMPARE(m_settings->tileWidth(), tileWidth);
    QTest::qWait(150);
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    const qreal beforeWheel = list->property("contentY").toReal();
    // Flickable can drop a synthetic wheel event that arrives without time
    // having passed since its last one, so offer the plain wheel twice.
    wheelOver(list, 3);
    wheelOver(list, 3);
    QTRY_VERIFY(list->property("contentY").toReal() > beforeWheel);
    QCOMPARE(m_settings->tileWidth(), tileWidth);    QVERIFY2(selectionConsistent(grid, &why), qPrintable(why));
  }

  void gridCardsAndMenusExposeNamesAndSelection() {
    const QString path = pathAt(0);
    QQuickItem* card = nullptr;
    QAccessibleInterface* accessible = nullptr;
    QTRY_VERIFY((card = cardFor(path)) != nullptr
                && (accessible = QAccessible::queryAccessibleInterface(card)) != nullptr);
    QCOMPARE(accessible->role(), QAccessible::ListItem);
    QVERIFY(accessible->text(QAccessible::Name).contains(m_library->fileNameAt(0)));
    QVERIFY(accessible->state().selected);
    QVERIFY(!accessible->state().checked);
    QVERIFY(accessible->actionInterface());
    accessible->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_VERIFY([&] {
      auto* live = cardFor(path);
      auto* interface = live ? QAccessible::queryAccessibleInterface(live) : nullptr;
      return interface && interface->state().checked;
    }());
    // The grid can release a delegate while a popup's delay is running.
    const auto filenameTip = [&]() -> QObject* {
      auto* live = cardFor(path);
      return live ? live->findChild<QObject*>(QStringLiteral("captureFilenameTip")) : nullptr;
    };
    QTRY_VERIFY(filenameTip());
    QCOMPARE(filenameTip()->property("text").toString(), m_library->fileNameAt(0));
    QTRY_VERIFY(cardFor(path) && cardFor(path)->property("keyboardCurrent").toBool());
    // Initial selection alone must not leave a keyboard tooltip behind.
    QVERIFY(item("library")->property("layoutSettled").toBool());
    QTest::keyClick(m_window, Qt::Key_Right);
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(item("library")->property("currentIndex").toInt(), 0);
    QTRY_VERIFY(filenameTip() && filenameTip()->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu", Q_ARG(QVariant, 0),
                                    Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(filenameTip() && !filenameTip()->property("visible").toBool());
    QQuickItem* rename = item("contextAction_rename");
    QCOMPARE(rename->property("text").toString(), QStringLiteral("Rename"));
    auto* namedAction = QAccessible::queryAccessibleInterface(rename);
    QVERIFY(namedAction);
    QCOMPARE(namedAction->text(QAccessible::Name), QStringLiteral("Rename"));
    // Once the menu closes, moving with the keyboard shows tips again, on
    // other tiles and on the one that was up.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(item("library")->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Right);
    const auto tipOn = [&](int row) {
      QQuickItem* live = cardFor(pathAt(row));
      QObject* tip = live ? live->findChild<QObject*>(QStringLiteral("captureFilenameTip")) : nullptr;
      return tip && tip->property("visible").toBool();
    };
    QTRY_COMPARE(item("library")->property("currentIndex").toInt(), 1);
    QTRY_VERIFY(tipOn(1));
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(item("library")->property("currentIndex").toInt(), 0);
    QTRY_VERIFY(tipOn(0));
  }

  void filenameToggleKeepsCardAndThumbnailDimensions() {
    QVERIFY(!m_settings->showGridFilenames());
    const QString path = pathAt(0);
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = cardFor(path)) != nullptr);
    const QSizeF size(card->width(), card->height());
    QQuickItem* thumbnail = find(card, [](QQuickItem* candidate) {
      return candidate->property("source").toUrl().toString().startsWith(QStringLiteral("image://thumbs/"));
    });
    QVERIFY(thumbnail);
    const QSize sourceSize = thumbnail->property("sourceSize").toSize();
    const QUrl source = thumbnail->property("source").toUrl();
    QQuickItem* label = find(card, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("captureFilenameLabel");
    });
    QVERIFY(label);
    QVERIFY(!label->isVisible());
    QVERIFY(QMetaObject::invokeMethod(item("settingsSheet"), "open"));
    item("gridFilenamesToggle")->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(m_settings->showGridFilenames());
    const auto restore = qScopeGuard([&] { m_settings->setShowGridFilenames(false); m_window->resize(1280, 820); });
    invoke("dismissTopLayer");
    QTRY_VERIFY(label->isVisible());
    QCOMPARE(QSizeF(card->width(), card->height()), size);
    QCOMPARE(thumbnail->property("sourceSize").toSize(), sourceSize);
    QCOMPARE(thumbnail->property("source").toUrl(), source);
    QCOMPARE(label->property("text").toString(), m_library->fileNameAt(0));
    m_window->resize(560, 420);
    QTRY_VERIFY(cardFor(path));
    card = cardFor(path);
    label = find(card, [](QQuickItem* candidate) { return candidate->objectName() == QStringLiteral("captureFilenameLabel"); });
    QVERIFY(label);
    QVERIFY(label->width() > 0 && label->width() <= card->width());
  }

  void libraryCopiesOpenedEntryPathAndName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("original.png"));
    const QString alias = dir.filePath(QStringLiteral("alias # 雪.png"));
    m_disposablePaths.append(target);
    m_disposablePaths.append(alias);
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      dir.remove();
      rescan();
      const bool removed = QTest::qWaitFor([&] { return !m_captures->scanning() && m_library->rowOf(target) < 0; }, 10000);
      QVERIFY(removed);
    });
    QImage picture(200, 150, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(target));
    QVERIFY(QFile::link(target, alias));
    m_captures->addExtraFiles({alias});
    m_captures->setExtraRoot(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!m_captures->scanning(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(target) >= 0, 10000);
    const int row = m_library->rowOf(target);
    QCOMPARE(m_library->deletionPathAt(row), alias);
    item("library")->setProperty("currentIndex", row);
    const auto copiedText = [&] {
      QFile log(m_scratch.filePath(QStringLiteral("clipboard.log")));
      return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
    };
    QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(copiedText(), alias);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu", Q_ARG(QVariant, row),
                                     Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QVERIFY(item("contextAction_copy-path"));
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("libraryContextMenu"));
    QTRY_VERIFY(menu->property("visible").toBool());
    QQuickItem* copyName = item("contextAction_copy-name");
    copyName->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(copiedText(), QFileInfo(alias).fileName());
    QTRY_VERIFY(!menu->property("visible").toBool());
    openDetail(row);
    QTRY_VERIFY(item("detail")->isVisible());
    QTest::keyClick(m_window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE(copiedText(), alias);
    QVERIFY(item("detail")->isVisible());
    perform(QStringLiteral("copy-name"), target);
    QTRY_COMPARE(copiedText(), QFileInfo(alias).fileName());
    QVERIFY(item("detail")->isVisible());
  }

  void previewResizeRemembersSizeAndClampsToWindow() {
    QQuickItem* detail = item("detail");
    const auto restore = qScopeGuard([&] {
      detail->setProperty("previewWidth", 1000);
      detail->setProperty("previewHeight", 700);
      m_settings->setPreviewWidth(1000);
      m_settings->setPreviewHeight(700);
      m_window->resize(1280, 820);
    });
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QQuickItem* panel = item("detailPanel");
    QQuickItem* handle = item("detailResize_corner");
    // The grip is drawn inside the panel's corner, not over its edge.
    QQuickItem* grip = item("detailResizeGrip");
    QVERIFY(grip->isVisible());
    const QRectF gripBounds = grip->mapRectToScene(grip->boundingRect());
    const QRectF panelBounds = panel->mapRectToScene(panel->boundingRect());
    QVERIFY(panelBounds.contains(gripBounds));
    QVERIFY(panelBounds.right() - gripBounds.right() < 12);
    QVERIFY(panelBounds.bottom() - gripBounds.bottom() < 12);
    const QSizeF before(panel->width(), panel->height());
    dragBetween(handle, QPointF(8, 8), QPointF(-72, -52));
    QTRY_VERIFY(panel->width() < before.width() - 100);
    QTRY_VERIFY(panel->height() < before.height() - 80);
    const QSize remembered(m_settings->previewWidth(), m_settings->previewHeight());
    QCOMPARE(qRound(panel->width()), remembered.width());
    QCOMPARE(qRound(panel->height()), remembered.height());
    AppSettings restarted;
    QCOMPARE(restarted.previewWidth(), remembered.width());
    QCOMPARE(restarted.previewHeight(), remembered.height());
    invoke("dismissTopLayer");
    openDetail(0);
    QCOMPARE(qRound(panel->width()), remembered.width());
    m_window->resize(560, 420);
    QTRY_COMPARE(panel->width(), 500.0);
    QTRY_COMPARE(panel->height(), 360.0);
    QCOMPARE(m_settings->previewWidth(), remembered.width());
    m_window->resize(1280, 820);
    QTRY_COMPARE(qRound(panel->width()), remembered.width());
    QVERIFY(QMetaObject::invokeMethod(detail, "setFullScreen", Q_ARG(QVariant, true)));
    QTRY_VERIFY(!handle->isVisible());
    QVERIFY(QMetaObject::invokeMethod(detail, "setFullScreen", Q_ARG(QVariant, false)));
  }

  void imageZoomKeepsThePointerFixed_data() {
    QTest::addColumn<int>("rotation");
    for (int rotation : {0, 90, 180, 270}) QTest::newRow(qPrintable(QString::number(rotation))) << rotation;
  }

  void imageZoomKeepsThePointerFixed() {
    QFETCH(int, rotation);
    openDetail(m_library->rowOf(m_oddPath));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->property("imageReady").toBool());
    detail->setProperty("imageRotation", rotation);
    detail->setProperty("imageFlipHorizontal", true);
    QVERIFY(QMetaObject::invokeMethod(detail, "zoomImageAt", Q_ARG(QVariant, 3.0),
                                     Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
    QQuickItem* viewport = item("detailStillViewport");
    QQuickItem* canvas = item("detailImageCanvas");
    QTRY_VERIFY(viewport->width() > 0 && viewport->height() > 0 && canvas->width() > 0);
    const QPointF pointer(viewport->width() * 0.6, viewport->height() * 0.6);
    const auto underPointer = [&] {
      return QPointF((viewport->property("contentX").toDouble() + pointer.x() - canvas->x()) / canvas->width(),
                     (viewport->property("contentY").toDouble() + pointer.y() - canvas->y()) / canvas->height());
    };
    const QPointF before = underPointer();
    const QPointF at = viewport->mapToScene(pointer);
    for (int delta : {120, -120}) {
      const qreal zoom = detail->property("imageZoom").toDouble();
      QWheelEvent wheel(at, m_window->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, delta),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QCoreApplication::sendEvent(m_window, &wheel);
      // The wheel must zoom, and zooming must keep the pointer's spot fixed.
      QTRY_VERIFY(delta > 0 ? detail->property("imageZoom").toDouble() > zoom
                            : detail->property("imageZoom").toDouble() < zoom);
      QTRY_VERIFY(QLineF(before, underPointer()).length() < 0.001);
    }
  }

  // The library preview shows a very large picture from its reduced copy, at
  // the picture's own size, and loads full resolution once zoomed past it.
  void aVeryLargePictureIsPreviewedFromAReducedCopy() {
    const auto restore = qScopeGuard([this] {
      ImageBudget::setLimits({});
      m_captures->readPicture(m_oddPath);
      invoke("dismissTopLayer");
    });
    ImageBudget::setLimits({.displayBytes = 1 * ImageBudget::kMiB});
    openDetail(m_library->rowOf(m_oddPath));
    QQuickItem* detail = item("detail");
    // The sheet may already show this file; ask again under the new budget.
    m_captures->readPicture(m_oddPath);
    QQuickItem* still = item("detailStill");
    QTRY_VERIFY(still->property("source").toUrl().toString().startsWith(QStringLiteral("image://large/")));
    QTRY_VERIFY(detail->property("imageReady").toBool());
    QCOMPARE(detail->property("imageSourceWidth").toDouble(), 1536.0);
    QCOMPARE(detail->property("imageSourceHeight").toDouble(), 1024.0);
    QQuickItem* full = item("detailFullResolution");
    QVERIFY(full->property("source").toUrl().isEmpty());
    QQuickItem* area = item("detailArea");
    QVERIFY(!area->isVisible());

    // A JPEG zoomed in loads only the part on screen, never the whole.
    QVERIFY(QMetaObject::invokeMethod(detail, "zoomImageAt", Q_ARG(QVariant, 3.0),
                                     Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
    QTRY_VERIFY(area->isVisible());
    QVERIFY(area->property("source").toUrl().toString().startsWith(QStringLiteral("image://large/")));
    QCOMPARE(area->property("status").toInt(), 1);
    const QRectF shown = area->property("shown").toRectF();
    QVERIFY(shown.width() > 0 && shown.width() < 3000);
    QVERIFY(full->property("source").toUrl().isEmpty());
    // Back to fit, it goes.
    QVERIFY(QMetaObject::invokeMethod(detail, "zoomImageAt", Q_ARG(QVariant, 1.0 / 3.0),
                                     Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
    QTRY_VERIFY(!area->isVisible());

    // Past Qt's limit, a format that decodes whole says why it cannot open,
    // and nothing is asked of a decoder that would refuse it.
    const QString scan = m_scratch.filePath(QStringLiteral("too-large.png"));
    QImage image(1200, 1200, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QVERIFY(image.save(scan));
    QImageReader::setAllocationLimit(2);
    const auto limit = qScopeGuard([] { QImageReader::setAllocationLimit(256); });
    QSignalSpy read(m_captures, &CaptureModel::pictureRead);
    m_captures->readPicture(scan);
    QTRY_COMPARE(read.count(), 1);
    const QVariantMap picture = read.first().at(1).toMap();
    QVERIFY(picture.value(QStringLiteral("reduced")).toBool());
    QVERIFY(picture.value(QStringLiteral("source")).toUrl().isEmpty());
    QCOMPARE(picture.value(QStringLiteral("error")).toString(),
             QStringLiteral("This picture needs about 5 MiB; the limit on this computer is 2 MiB"));
  }

  void imagePinchKeepsItsCentroidFixed() {
    openDetail(m_library->rowOf(m_oddPath));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->property("imageReady").toBool());
    QVERIFY(QMetaObject::invokeMethod(detail, "zoomImageAt", Q_ARG(QVariant, 3.0),
                                     Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
    QQuickItem* viewport = item("detailStillViewport");
    QQuickItem* canvas = item("detailImageCanvas");
    QTRY_VERIFY(viewport->width() > 0 && viewport->height() > 0 && canvas->width() > 0);
    const QPointF pointer(viewport->width() * 0.6, viewport->height() * 0.6);
    const auto underPointer = [&] {
      return QPointF((viewport->property("contentX").toDouble() + pointer.x() - canvas->x()) / canvas->width(),
                     (viewport->property("contentY").toDouble() + pointer.y() - canvas->y()) / canvas->height());
    };
    const QPointF before = underPointer();
    pinchAt(viewport->mapToScene(pointer).toPoint());
    QTRY_VERIFY(detail->property("imageZoom").toDouble() > 3.0);
    QVERIFY(QLineF(before, underPointer()).length() < 0.003);
  }

  void failedThumbnailHasADistinctSettledState() {
    const QString missing = m_scratch.filePath(QStringLiteral("failed-preview.png"));
    m_disposablePaths.append(missing);
    QQmlComponent component(m_engine);
    component.setData(R"QML(
      import QtQuick
      import Omaroll
      CaptureCard { width: 240; height: 160; fileName: "failed-preview.png" }
    )QML", QUrl());
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    object->setProperty("path", missing);
    QTRY_VERIFY(object->property("thumbnailFailed").toBool());
    QVERIFY(!object->property("thumbnailReady").toBool());
    auto* placeholder = object->findChild<QQuickItem*>(QStringLiteral("thumbnailPlaceholder"));
    QVERIFY(placeholder);
    QCOMPARE(placeholder->property("text").toString(), QStringLiteral("Preview unavailable"));
    object->setProperty("path", QString());
    QTRY_VERIFY(!object->property("thumbnailFailed").toBool());
  }

  void failedTrashTargetsRemainChecked() {
    QTemporaryDir dir;
    const QString good = dir.filePath(QStringLiteral("trash-good.png"));
    const QString changed = dir.filePath(QStringLiteral("trash-changed.png"));
    QImage image(30, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(good));
    QVERIFY(image.save(changed));
    m_disposablePaths.append(good);
    m_disposablePaths.append(changed);
    m_captures->addExtraFiles({good, changed});
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY(m_library->rowOf(good) >= 0 && m_library->rowOf(changed) >= 0);
    auto* grid = item("library");
    for (const QString& path : {good, changed})
      QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, path)));
    QVERIFY(QMetaObject::invokeMethod(m_window, "requestDeleteBatch",
                                    Q_ARG(QVariant, QVariant(QStringList{good, changed})),
                                    Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
    QTRY_VERIFY(item("confirm")->isVisible());
    // The captured identity must reject a replacement, leaving it retryable.
    QVERIFY(QFile::rename(changed, dir.filePath(QStringLiteral("old.png"))));
    image.fill(Qt::blue);
    QVERIFY(image.save(changed));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!item("confirm")->isVisible());
    QVERIFY(!QFileInfo::exists(good));
    QCOMPARE(QImage(changed).pixelColor(0, 0), QColor(Qt::blue));
    QTRY_VERIFY(!m_captures->scanning());
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 1);
    QVariant checked;
    QVERIFY(QMetaObject::invokeMethod(grid, "isChecked", Q_RETURN_ARG(QVariant, checked),
                                    Q_ARG(QVariant, changed)));
    QVERIFY(checked.toBool());
  }

  void renameKeepsOtherChecksAndFollowsTheRenamedCheck_data() {
    QTest::addColumn<bool>("albumView");
    QTest::newRow("all-media") << false;
    QTest::newRow("album") << true;
  }

  void renameKeepsOtherChecksAndFollowsTheRenamedCheck() {
    QFETCH(bool, albumView);
    QTemporaryDir dir;
    const QString source = dir.filePath(QStringLiteral("rename-checked.png"));
    const QString other = dir.filePath(QStringLiteral("rename-other.png"));
    QVERIFY(QImage(30, 20, QImage::Format_RGB32).save(source));
    QVERIFY(QImage(31, 21, QImage::Format_RGB32).save(other));
    m_disposablePaths.append(source);
    m_disposablePaths.append(other);
    m_disposablePaths.append(dir.filePath(QStringLiteral("renamed-check.png")));
    m_captures->addExtraFiles({source, other});
    const QString album = QStringLiteral("Rename checked files");
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      m_library->setAlbumFilter({}, {});
      m_settings->deleteAlbum(album);
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY(m_library->rowOf(source) >= 0 && m_library->rowOf(other) >= 0);
    if (albumView) {
      QVERIFY(m_settings->createAlbum(album));
      QVERIFY(m_settings->addToAlbum(album, {source, other}));
      m_library->setAlbumFilter(album, m_settings->albumPaths(album));
      QTRY_COMPARE(m_library->count(), 2);
    }
    auto* grid = item("library");
    for (const QString& path : {source, other})
      QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, path)));
    perform(QStringLiteral("rename"), source);
    QTRY_VERIFY(item("renameSheet")->isVisible());
    item("renameInput")->setProperty("text", QStringLiteral("renamed-check"));
    QTest::keyClick(m_window, Qt::Key_Return);
    const QString renamed = dir.filePath(QStringLiteral("renamed-check.png"));
    QTRY_VERIFY(m_library->rowOf(renamed) >= 0 && !m_captures->scanning());
    QTRY_COMPARE(grid->property("checkedCount").toInt(), 2);
    for (const QString& path : {renamed, other}) {
      QVariant checked;
      QVERIFY(QMetaObject::invokeMethod(grid, "isChecked", Q_RETURN_ARG(QVariant, checked),
                                      Q_ARG(QVariant, path)));
      QVERIFY(checked.toBool());
    }
  }

  void libraryHeaderKeepsOnePrimaryRow_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("selection");
    // 941 is Omarchy's default half tile on a 1920 screen.
    for (int width : {560, 941, 1280, 1920}) {
      QTest::newRow(qPrintable(QStringLiteral("%1").arg(width))) << width << false;
      QTest::newRow(qPrintable(QStringLiteral("%1 selected").arg(width))) << width << true;
    }
  }

  void libraryHeaderKeepsOnePrimaryRow() {
    QFETCH(int, width);
    QFETCH(bool, selection);
    const bool downloads = m_settings->scanDownloads();
    m_settings->setScanDownloads(true);
    QQuickItem* grid = item("library");
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(grid, "clearChecked");
      m_settings->setScanDownloads(downloads);
      m_window->resize(1280, 820);
    });
    m_window->resize(width, 760);
    if (selection) {
      for (int row = 0; row < 6; ++row)
        QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, pathAt(row)));
      QTRY_COMPARE(grid->property("checkedCount").toInt(), 6);
    }
    waitForLayout(QSize(width, 760));
    QTRY_COMPARE(item("headerSearchSlot")->parentItem()->width(), qreal(width));
    waitForLayout();

    // The title, the status and its count, whole and clear of everything else.
    QQuickItem* status = item("libraryStatus");
    if (selection) QTRY_COMPARE(status->property("text").toString(), QStringLiteral("6 selected"));
    QVERIFY2(status->width() + 1 >= status->property("implicitWidth").toReal(),
             qPrintable(QStringLiteral("%1 < %2").arg(status->width()).arg(status->property("implicitWidth").toReal())));
    const QRectF statusRect = status->mapRectToScene(status->boundingRect());
    QQuickItem* slot = item("headerSearchSlot");
    QQuickItem* search = item("librarySearchBox");
    QQuickItem* actions = item("folderSidebarToggle")->parentItem();
    QVERIFY(statusRect.right() < slot->mapToScene(QPointF()).x());
    if (search->isVisible()) {
      const QRectF searchRect = search->mapRectToScene(search->boundingRect());
      QVERIFY(searchRect.width() >= 110);
      QVERIFY(searchRect.left() > statusRect.right());
      QVERIFY(searchRect.right() < actions->mapToScene(QPointF()).x());
      if (width >= 1280) QVERIFY(searchRect.width() >= (selection ? 200 : 300));
    } else {
      QVERIFY2(selection && width < 941, "the search gives way only to a selection in a narrow window");
      // A typed query stays in sight as a chip with its own clear, and the
      // search key says why it cannot open the field.
      const auto clearSearch = qScopeGuard([&] { m_library->setSearchText({}); });
      // Filter first, then select what it shows, so the selection survives
      // the search and the field stays hidden.
      QMetaObject::invokeMethod(grid, "clearChecked");
      const QString query = QFileInfo(pathAt(0)).completeBaseName();
      m_library->setSearchText(query);
      QTRY_VERIFY(m_library->count() >= 1);
      const int selected = std::min(6, m_library->count());
      for (int row = 0; row < selected; ++row)
        QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, pathAt(row)));
      QTRY_COMPARE(grid->property("checkedCount").toInt(), selected);
      const auto reselect = qScopeGuard([&] {
        m_library->setSearchText({});
        QMetaObject::invokeMethod(grid, "clearChecked");
        for (int row = 0; row < 6; ++row)
          QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, pathAt(row)));
      });
      QTRY_VERIFY(!search->isVisible());
      const auto chipShown = [this] {
        return find(m_window->contentItem(), [](QQuickItem* candidate) {
                 return candidate->objectName() == QStringLiteral("filterChip-search") && candidate->isVisible();
               }) != nullptr;
      };
      QTRY_VERIFY(chipShown());
      QTest::keyClick(m_window, Qt::Key_Slash);
      QTRY_VERIFY(item("footerStatus")->property("text").toString().contains(QStringLiteral("to search")));
      QVERIFY2(chipShown(), qPrintable(QStringLiteral("chip gone after the search key; query '%1', field shown %2")
                                           .arg(m_library->searchText())
                                           .arg(item("librarySearchBox")->isVisible())));
      QQuickItem* clear = find(m_window->contentItem(), [](QQuickItem* candidate) {
        return candidate->objectName() == QStringLiteral("filterChipClear-search");
      });
      QVERIFY(clear);
      QTest::mouseClick(m_window, Qt::LeftButton, {},
                        clear->mapToScene(QPointF(clear->width() / 2, clear->height() / 2)).toPoint());
      QTRY_VERIFY(m_library->searchText().isEmpty());
      // With another chip beside it, Clear all clears the search as well.
      m_library->setSearchText(query);
      // A date range that keeps every tile, so the selection stays.
      m_library->setDateRange(QStringLiteral("1970-01-01"), QStringLiteral("2100-12-31"));
      const auto clearDates = qScopeGuard([&] { m_library->clearDateRange(); });
      QTRY_COMPARE(grid->property("checkedCount").toInt(), selected);
      QTRY_VERIFY(chipShown());
      QMetaObject::invokeMethod(item("activeFilterChips"), "clearAll");
      QTRY_VERIFY(m_library->searchText().isEmpty());
      QVERIFY(m_library->dateFrom().isEmpty());
    }
    // Every header button sits on the row's centre line, inside the window,
    // and clear of the status and of each other.
    QQuickItem* header = actions->parentItem();
    const qreal middle = header->mapToScene(QPointF(0, header->height() / 2)).y();
    qreal previousRight = statusRect.right();
    QList<QQuickItem*> buttons;
    for (QQuickItem* child : actions->childItems())
      if (child->isVisible() && child->width() > 0) buttons.append(child);
    std::sort(buttons.begin(), buttons.end(), [](QQuickItem* a, QQuickItem* b) { return a->x() < b->x(); });
    for (QQuickItem* button : std::as_const(buttons)) {
      const QRectF rect = button->mapRectToScene(button->boundingRect());
      QVERIFY2(qAbs(rect.center().y() - middle) <= 1.0,
               qPrintable(button->property("label").toString() + QStringLiteral(" off centre")));
      QVERIFY2(rect.left() >= previousRight, qPrintable(button->property("label").toString()));
      QVERIFY2(rect.right() <= width - 19, qPrintable(button->property("label").toString()));
      previousRight = rect.right();
    }
    // A selection keeps Actions, Trash and Clear; the rest only where they fit.
    QCOMPARE(item("selectionActionsButton")->isVisible(), selection);
    if (selection) {
      QVERIFY(pill(m_window->contentItem(), QStringLiteral("Trash 6")));
      QVERIFY(pill(m_window->contentItem(), QStringLiteral("Clear")));
      if (width >= 1280) QVERIFY(pill(m_window->contentItem(), QStringLiteral("Organize")));
      if (width >= 1920) QVERIFY(pill(m_window->contentItem(), QStringLiteral("Hide")));
    }
    // The gear is a line icon, never the colour emoji.
    QQuickItem* settings = item("settingsButton");
    QCOMPARE(settings->property("icon").toString(), QStringLiteral("settings"));
    QCOMPARE(settings->property("accessibleName").toString(), QStringLiteral("Settings"));

    // Kind: full names in a segmented control, or a menu when narrow.
    QQuickItem* segments = item("kindSegments");
    QQuickItem* kindMenu = item("kindMenuButton");
    QVERIFY(segments->isVisible() != kindMenu->isVisible());
    if (width >= 1280) QVERIFY(segments->isVisible());
    const QStringList full{QStringLiteral("All"), QStringLiteral("Screenshots"), QStringLiteral("Recordings"),
                           QStringLiteral("Pictures"), QStringLiteral("Videos"), QStringLiteral("Downloads"),
                           QStringLiteral("PDFs")};
    if (segments->isVisible()) {
      QStringList labels;
      find(segments, [&labels](QQuickItem* candidate) {
        if (candidate->property("segment").toBool() && candidate->isVisible())
          labels.append(candidate->property("label").toString());
        return false;
      });
      QCOMPARE(labels, full);
      QVERIFY(segments->mapRectToScene(segments->boundingRect()).right() <= width - 19);
    } else {
      QVERIFY(kindMenu->property("label").toString().startsWith(QStringLiteral("All")));
    }
    QQuickItem* browse = item("libraryBrowseButton");
    QVERIFY(browse->mapRectToScene(browse->boundingRect()).right() <= width - 19);
  }

  void openFolderPinsFromTheChipRow() {
    const QString folder = QFileInfo(pathAt(0)).absolutePath();
    const auto restore = qScopeGuard([&] {
      m_settings->unpinFolder(folder);
      m_library->setFolderFilter({});
      m_window->resize(1280, 820);
    });
    m_window->resize(941, 760);
    m_library->setFolderFilter(folder);
    waitForLayout(QSize(941, 760));
    QQuickItem* pin = item("pinCurrentFolder");
    QQuickItem* chips = item("activeFilterChips");
    QTRY_VERIFY(pin->isVisible() && chips->isVisible());
    // The pin button shares the folder chip's row instead of adding one.
    const QRectF pinRect = pin->mapRectToScene(pin->boundingRect());
    const QRectF chipRect = chips->mapRectToScene(chips->boundingRect());
    QVERIFY(pinRect.top() < chipRect.bottom() && chipRect.top() < pinRect.bottom());
    QVERIFY(pinRect.left() >= chipRect.right());
    QCOMPARE(item("pinnedFolders")->isVisible(), false);
    const qreal gridTop = item("library")->mapToScene(QPointF()).y();
    QVERIFY2(gridTop < 58 + 40 + 40 + 20, qPrintable(QString::number(gridTop)));
    click(pin);
    QTRY_VERIFY(m_settings->isFolderPinned(folder));
    QTRY_COMPARE(pin->property("label").toString(), QStringLiteral("Unpin folder"));
  }

  void longLibraryNamesAndStatusFitNarrowWindows_data() {
    QTest::addColumn<int>("width");
    for (int width : {560, 700, 900})
      QTest::newRow(qPrintable(QString::number(width))) << width;
  }

  void longLibraryNamesAndStatusFitNarrowWindows() {
    QFETCH(int, width);
    const bool downloads = m_settings->scanDownloads();
    const QString album = QStringLiteral("A very long collection name ").repeated(4).left(60);
    QVERIFY(m_settings->createAlbum(album));
    m_library->setAlbumFilter(album, {});
    m_settings->setScanDownloads(true);
    m_window->resize(width, 420);
    const auto restore = qScopeGuard([&] {
      m_library->setAlbumFilter({}, {});
      m_settings->deleteAlbum(album);
      m_settings->setScanDownloads(downloads);
      m_window->resize(1280, 820);
    });
    waitForLayout(QSize(width, 420));
    QQuickItem* browse = item("libraryBrowseButton");
    QTRY_VERIFY(browse->mapRectToScene(browse->boundingRect()).right() <= width - 19);
    if (width >= 700) QTRY_COMPARE(browse->property("toolTip").toString(), album + QStringLiteral("  ▾"));
    QQuickItem* status = item("libraryStatus");
    const auto clearOfSettings = [&] {
      QQuickItem* settings = pill(m_window->contentItem(), QStringLiteral("Settings"));
      return settings && status->mapRectToScene(status->boundingRect()).right()
                             < settings->mapRectToScene(settings->boundingRect()).left();
    };
    QTRY_VERIFY(clearOfSettings());
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

  void backgroundRetryIsClearedWhenTheCompositionChanges() {
    QQuickItem* sheet = item("matteSheet");
    perform(QStringLiteral("matte"), pathAt(0));
    QTRY_VERIFY(sheet->isVisible());
    sheet->setProperty("retryCopy", true);
    sheet->setProperty("saveMessage", QStringLiteral("Retry copy"));
    sheet->setProperty("aspect", (sheet->property("aspect").toInt() + 1) % 3);
    QVERIFY(!sheet->property("retryCopy").toBool());
    QVERIFY(sheet->property("saveMessage").toString().isEmpty());
    sheet->setProperty("retryCopy", true);
    sheet->setProperty("selected", (sheet->property("selected").toInt() + 1) % 7);
    QVERIFY(!sheet->property("retryCopy").toBool());
    sheet->setProperty("retryCopy", true);
    sheet->setProperty("paddingPercent", sheet->property("paddingPercent").toInt() == 12 ? 16 : 12);
    QVERIFY(!sheet->property("retryCopy").toBool());
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

  // Very large pictures compare from copies the size of the screen, and a
  // JPEG zoomed past its copy loads only the part on screen.
  void compareSheetOpensVeryLargePicturesFromCopies() {
    ImageBudget::setLimits({.displayBytes = 1 * ImageBudget::kMiB});
    QQuickItem* sheet = item("compareSheet");
    const auto restore = qScopeGuard([sheet] {
      ImageBudget::setLimits({});
      QMetaObject::invokeMethod(sheet, "close");
    });
    QStringList pair;
    for (const QString& name : {QStringLiteral("compare-a.jpg"), QStringLiteral("compare-b.jpg")}) {
      const QString path = m_scratch.filePath(name);
      QImage image(4000, 3000, QImage::Format_RGB32);
      image.fill(QColor(60, 120, 180));
      QVERIFY(image.save(path, nullptr, 85));
      pair << path;
    }
    QVERIFY(QMetaObject::invokeMethod(sheet, "open", Q_ARG(QVariant, QVariant(pair))));
    QTRY_VERIFY(sheet->isVisible());
    QQuickItem* image = find(sheet, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("compareImage");
    });
    QVERIFY(image);
    QTRY_VERIFY(image->property("source").toString().startsWith(QStringLiteral("image://large/")));
    QTRY_COMPARE(image->property("status").toInt(), 1);
    QQuickItem* area = find(sheet, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("compareArea");
    });
    QVERIFY(area);
    QVERIFY(!area->isVisible());
    sheet->setProperty("zoom", 8.0);
    QTRY_VERIFY(area->isVisible());
    QCOMPARE(area->property("status").toInt(), 1);
    const QRectF shown = area->property("shown").toRectF();
    QVERIFY(shown.width() > 0 && shown.width() < 4000);
    QVERIFY(QMetaObject::invokeMethod(sheet, "reset"));
    QTRY_VERIFY(!area->isVisible());
  }

  void compareSheetShowsTheSetAndSharesOneZoom() {
    QQuickItem* sheet = item("compareSheet");
    QStringList pair{pathAt(0), pathAt(1)};
    QVERIFY(QMetaObject::invokeMethod(sheet, "open", Q_ARG(QVariant, QVariant(pair))));
    QTRY_VERIFY(sheet->isVisible());
    QTRY_COMPARE(sheet->width(), qreal(m_window->width()));
    QTRY_COMPARE(sheet->height(), qreal(m_window->height()));
    QCOMPARE(sheet->property("imageCount").toInt(), 2);
    QSignalSpy frames(m_window, &QQuickWindow::frameSwapped);
    m_window->update();
    QTRY_VERIFY(!frames.isEmpty());

    QQuickItem* repeater = find(sheet, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("compareRepeater");
    });
    QVERIFY(repeater);
    QCOMPARE(repeater->property("count").toInt(), 2);

    QQuickItem* image = find(sheet, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("compareImage");
    });
    QVERIFY(image);
    const QPointF pointer(image->width() * 0.65, image->height() * 0.6);
    const QPointF at = image->mapToScene(pointer);
    QWheelEvent wheel(at, m_window->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(m_window, &wheel);
    QTRY_VERIFY(sheet->property("zoom").toDouble() > 1.0);
    QVERIFY(QLineF(pointer, image->mapFromScene(at)).length() < 0.01);

    const double wheelZoom = sheet->property("zoom").toDouble();
    frames.clear();
    m_window->update();
    QTRY_VERIFY(!frames.isEmpty());
    pinchAt(at.toPoint());
    QTRY_VERIFY(sheet->property("zoom").toDouble() > wheelZoom);
    QVERIFY(QLineF(pointer, image->mapFromScene(at)).length() < 2.0);

    // One zoom property drives both pictures; reset returns them to fit.
    sheet->setProperty("zoom", 2.0);
    QCOMPARE(sheet->property("zoom").toDouble(), 2.0);
    QVERIFY(QMetaObject::invokeMethod(sheet, "reset"));
    QCOMPARE(sheet->property("zoom").toDouble(), 1.0);
    QCOMPARE(sheet->property("panX").toDouble(), 0.0);

    invoke("dismissTopLayer");
    QTRY_VERIFY(!sheet->isVisible());
  }

  void batchCorrectionSheetAppliesToTheSelection() {
    const QString first = m_scratch.filePath(QStringLiteral("batch-a.png"));
    const QString second = m_scratch.filePath(QStringLiteral("batch-b.png"));
    QVERIFY(QImage(80, 40, QImage::Format_RGB32).save(first));
    QVERIFY(QImage(60, 30, QImage::Format_RGB32).save(second));
    const QString firstCopy = m_scratch.filePath(QStringLiteral("batch-a-edited.png"));
    const QString secondCopy = m_scratch.filePath(QStringLiteral("batch-b-edited.png"));
    m_disposablePaths.append(firstCopy);
    m_disposablePaths.append(secondCopy);
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(first);
      QFile::remove(second);
      QFile::remove(firstCopy);
      QFile::remove(secondCopy);
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
      QTRY_COMPARE(m_captures->rowOf(firstCopy), -1);
      QTRY_COMPARE(m_captures->rowOf(secondCopy), -1);
      m_window->resize(1280, 820);
    });

    QQuickItem* sheet = item("batchCorrectionSheet");
    QVERIFY(QMetaObject::invokeMethod(sheet, "open",
                                      Q_ARG(QVariant, QVariant(QStringList{first, second}))));
    QTRY_VERIFY(sheet->isVisible());
    click(pill(sheet, QStringLiteral("Rotate right")));
    QQuickItem* apply = item("batchApply");
    QVERIFY(apply);
    click(apply);
    QTRY_VERIFY_WITH_TIMEOUT(!sheet->isVisible(), 20000);

    QVERIFY(QFileInfo::exists(firstCopy));
    QCOMPARE(QImage(firstCopy).size(), QSize(40, 80));
    QVERIFY(QFileInfo::exists(secondCopy));
    QCOMPARE(QImage(secondCopy).size(), QSize(30, 60));
  }

  void animatedCopiesAreLabeledBeforeSaving() {
    const QString path = QFINDTESTDATA("fixtures/viewer/animated.webp");
    const auto cleanup = qScopeGuard([&] { invoke("dismissTopLayer"); m_window->resize(1280, 820); });
    m_window->resize(560, 420);
    QQuickItem* single = item("correctionSheet");
    perform(QStringLiteral("corrections"), path);
    QTRY_VERIFY(single->isVisible());
    QVERIFY(single->property("animated").toBool());
    QCOMPARE(item("correctionSave")->property("label").toString(), QStringLiteral("Save first frame"));
    QVERIFY(QMetaObject::invokeMethod(single, "close"));
    QQuickItem* batch = item("batchCorrectionSheet");
    QVERIFY(QMetaObject::invokeMethod(batch, "open", Q_ARG(QVariant, QVariant(QStringList{path}))));
    QVERIFY(batch->property("hasAnimation").toBool());
    QCOMPARE(item("batchApply")->property("label").toString(), QStringLiteral("Save first frames"));
    QVERIFY(QMetaObject::invokeMethod(batch, "close"));
    QQuickItem* matte = item("matteSheet");
    matte->setProperty("path", path);
    QVERIFY(QMetaObject::invokeMethod(matte, "open"));
    QVERIFY(matte->property("animated").toBool());
    QVERIFY(pill(matte, QStringLiteral("Save first frame")));
    QVERIFY(QMetaObject::invokeMethod(matte, "close"));
    QQuickItem* exportSheet = item("exportSheet");
    QVERIFY(QMetaObject::invokeMethod(exportSheet, "open",
        Q_ARG(QVariant, QVariant(QStringList{path})), Q_ARG(QVariant, false)));
    QVERIFY(exportSheet->property("hasAnimation").toBool());
    QVERIFY(QMetaObject::invokeMethod(exportSheet, "close"));
  }

  void correctionSheetsIgnoreStaleJobsAndShowFailures() {
    const QString path = m_scratch.filePath(QStringLiteral("generation.png"));
    QVERIFY(QImage(120, 80, QImage::Format_RGB32).save(path));
    const QString missing = m_scratch.filePath(QStringLiteral("generation-missing.png"));
    m_disposablePaths.append(path);
    m_disposablePaths.append(missing);
    m_disposablePaths.append(ImageEditor::outputPathFor(missing));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer"); QFile::remove(path); QFile::remove(missing);
      QFile::remove(ImageEditor::outputPathFor(missing));
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
      QTRY_COMPARE(m_captures->rowOf(path), -1);
      QTRY_COMPARE(m_captures->rowOf(ImageEditor::outputPathFor(missing)), -1);
    });
    QQuickItem* single = item("correctionSheet");
    perform(QStringLiteral("corrections"), path);
    const int old = single->property("jobGeneration").toInt();
    QVERIFY(QMetaObject::invokeMethod(single, "close"));
    perform(QStringLiteral("corrections"), path);
    const int current = single->property("jobGeneration").toInt();
    QVERIFY(old != current);
    m_imageEditor->saved(path, old);
    m_imageEditor->failed(QStringLiteral("old error"), old);
    m_imageEditor->copied(old);
    QVERIFY(single->isVisible());
    QVERIFY(single->property("errorText").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(single, "close"));
    QQuickItem* batch = item("batchCorrectionSheet");
    QVERIFY(QMetaObject::invokeMethod(batch, "open", Q_ARG(QVariant, QVariant(QStringList{path}))));
    const int oldBatch = batch->property("jobGeneration").toInt();
    QVERIFY(QMetaObject::invokeMethod(batch, "close"));
    QVERIFY(QMetaObject::invokeMethod(batch, "open", Q_ARG(QVariant, QVariant(QStringList{path}))));
    const int batchId = batch->property("jobGeneration").toInt();
    m_imageEditor->batchProgress(1, 1, oldBatch);
    m_imageEditor->batchFinished(1, 0, {}, oldBatch);
    QVERIFY(batch->isVisible());
    QCOMPARE(batch->property("doneCount").toInt(), 0);
    m_imageEditor->batchFinished(0, 1,
        {QVariantMap{{QStringLiteral("source"), missing}, {QStringLiteral("output"), QString()},
                     {QStringLiteral("error"), QStringLiteral("Could not read")}}}, batchId);
    QVERIFY(batch->isVisible());
    QVERIFY(batch->property("errorText").toString().contains(missing));
    QCOMPARE(batch->property("failedPaths").value<QJSValue>().toVariant().toStringList(), QStringList{missing});
    QVERIFY(pill(batch, QStringLiteral("Retry failed"))->isVisible());
    QVERIFY(QImage(40, 20, QImage::Format_RGB32).save(missing));
    QSignalSpy retried(m_imageEditor, &ImageEditor::batchFinished);
    clickSettled(pill(batch, QStringLiteral("Retry failed")));
    QTRY_COMPARE_WITH_TIMEOUT(retried.size(), 1, 15000);
    QCOMPARE(retried.first().at(0).toInt(), 1);
    QCOMPARE(retried.first().at(1).toInt(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!batch->isVisible(), 15000);
    QVERIFY(QFileInfo::exists(ImageEditor::outputPathFor(missing)));
    QVERIFY(!QFileInfo::exists(ImageEditor::outputPathFor(path)));
  }

  void correctionStraightenSliderDrivesThePreview() {
    const QString path = m_scratch.filePath(QStringLiteral("straighten-disposable.png"));
    QVERIFY(QImage(120, 80, QImage::Format_RGB32).save(path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      m_window->resize(1280, 820);
    });

    QQuickItem* sheet = item("correctionSheet");
    perform(QStringLiteral("corrections"), path);
    QTRY_VERIFY(sheet->isVisible());
    QTRY_COMPARE(item("correctionPreview")->property("status").toInt(), 1);

    QQuickItem* slider = item("straightenSlider");
    QVERIFY(slider);
    slider->setProperty("value", 7.5);
    QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
    QCOMPARE(sheet->property("straighten").toDouble(), 7.5);
    // The preview id carries the angle in tenths of a degree.
    QVERIFY(item("correctionPreview")->property("source").toUrl().toString().contains(".75"));

    click(pill(sheet, QStringLiteral("Reset")));
    QCOMPARE(sheet->property("straighten").toDouble(), 0.0);
    QCOMPARE(slider->property("value").toDouble(), 0.0);
  }

  void correctionCropAspectPresets() {
    const QString path = m_scratch.filePath(QStringLiteral("aspect-disposable.png"));
    QVERIFY(QImage(120, 80, QImage::Format_RGB32).save(path));
    const QString portrait = m_scratch.filePath(QStringLiteral("aspect-portrait.png"));
    QVERIFY(QImage(80, 120, QImage::Format_RGB32).save(portrait));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      QFile::remove(portrait);
      m_window->resize(1280, 820);
    });

    QQuickItem* sheet = item("correctionSheet");
    perform(QStringLiteral("corrections"), path);
    QTRY_VERIFY(sheet->isVisible());
    QQuickItem* preview = item("correctionPreview");
    QTRY_COMPARE(preview->property("status").toInt(), 1);

    const auto cropRatio = [&] {
      const double pw = preview->property("paintedWidth").toDouble();
      const double ph = preview->property("paintedHeight").toDouble();
      const double w = sheet->property("cropW").toDouble() * pw;
      const double h = sheet->property("cropH").toDouble() * ph;
      return h > 0 ? w / h : 0.0;
    };

    click(pill(sheet, QStringLiteral("1:1")));
    QCOMPARE(sheet->property("cropAspectLabel").toString(), QStringLiteral("1:1"));
    QVERIFY2(qAbs(cropRatio() - 1.0) < 0.02, qPrintable(QString::number(cropRatio())));

    click(pill(sheet, QStringLiteral("16:9")));
    QVERIFY2(qAbs(cropRatio() - 16.0 / 9.0) < 0.02, qPrintable(QString::number(cropRatio())));

    // Portrait presets on a landscape picture: the frame is height-bound.
    click(pill(sheet, QStringLiteral("9:16")));
    QCOMPARE(sheet->property("cropAspectLabel").toString(), QStringLiteral("9:16"));
    QVERIFY2(qAbs(cropRatio() - 9.0 / 16.0) < 0.02, qPrintable(QString::number(cropRatio())));
    QCOMPARE(sheet->property("cropH").toDouble(), 1.0);

    click(pill(sheet, QStringLiteral("2:3")));
    QVERIFY2(qAbs(cropRatio() - 2.0 / 3.0) < 0.02, qPrintable(QString::number(cropRatio())));

    click(pill(sheet, QStringLiteral("3:4")));
    QVERIFY2(qAbs(cropRatio() - 3.0 / 4.0) < 0.02, qPrintable(QString::number(cropRatio())));

    for (int turn = 0; turn < 4; ++turn) {
      QVERIFY(QMetaObject::invokeMethod(sheet, "rotate", Q_ARG(QVariant, 1)));
      QTRY_VERIFY(qAbs(cropRatio() - 3.0 / 4.0) < 0.02);
    }
    QVERIFY(QMetaObject::invokeMethod(sheet, "close"));
    perform(QStringLiteral("corrections"), portrait);
    QCOMPARE(sheet->property("cropAspectLabel").toString(), QStringLiteral("Free"));
    QCOMPARE(sheet->property("cropW").toDouble(), 1.0);
    QCOMPARE(sheet->property("cropH").toDouble(), 1.0);

    click(pill(sheet, QStringLiteral("Free")));
    QCOMPARE(sheet->property("cropAspect").toDouble(), 0.0);
    QCOMPARE(sheet->property("cropW").toDouble(), 1.0);
    QCOMPARE(sheet->property("cropH").toDouble(), 1.0);
  }

  void correctionControlsKeepClearOfEachOther() {
    const QString path = m_scratch.filePath(QStringLiteral("layout-disposable.png"));
    QVERIFY(QImage(120, 80, QImage::Format_RGB32).save(path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      m_window->resize(1280, 820);
    });

    QQuickItem* sheet = item("correctionSheet");
    perform(QStringLiteral("corrections"), path);
    QTRY_VERIFY(sheet->isVisible());
    QTRY_COMPARE(item("correctionPreview")->property("status").toInt(), 1);
    QQuickItem* controls = item("correctionControls");

    // Wide, the result ends the ratio row and the actions end the resize row;
    // in the smallest window both drop to a line of their own. Whether a
    // window is wide enough depends on the font, so only the smallest one is
    // fixed. Either way no control may cover another or leave the panel.
    for (const QSize size : {QSize(1280, 820), QSize(560, 420)}) {
      m_window->resize(size);
      if (size.width() < 720) {
        QTRY_VERIFY(!controls->property("wide").toBool());
      }
      waitForLayout(size);
      // Rows can still be easing into place after the layout pass. Wait for a
      // layout with no overlaps, then check it in detail for the message.
      const auto overlaps = [&] {
        QList<QQuickItem*> found;
        find(controls, [&found](QQuickItem* candidate) {
          if ((candidate->property("floating").isValid() ||
               candidate->objectName() == QStringLiteral("correctionResult") ||
               candidate->objectName() == QStringLiteral("straightenSlider") ||
               candidate->objectName().startsWith(QStringLiteral("correctionWidth")) ||
               candidate->objectName().startsWith(QStringLiteral("correctionHeight"))) &&
              candidate->isVisible() && candidate->width() > 0) found.append(candidate);
          return false;
        });
        const QRectF panel = controls->parentItem()->mapRectToScene(
            QRectF(0, 0, controls->parentItem()->width(), controls->parentItem()->height()));
        for (qsizetype i = 0; i < found.size(); ++i) {
          const QRectF a = found[i]->mapRectToScene(QRectF(0, 0, found[i]->width(), found[i]->height()));
          if (!panel.contains(a)) return true;
          for (qsizetype j = i + 1; j < found.size(); ++j) {
            const QRectF overlap = a.intersected(
                found[j]->mapRectToScene(QRectF(0, 0, found[j]->width(), found[j]->height())));
            if (overlap.width() >= 1 && overlap.height() >= 1) return true;
          }
        }
        return false;
      };
      QTRY_VERIFY(!overlaps());

      QList<QQuickItem*> parts;
      find(controls, [&parts](QQuickItem* candidate) {
        const bool control = candidate->property("floating").isValid() ||
                             candidate->objectName() == QStringLiteral("correctionResult") ||
                             candidate->objectName() == QStringLiteral("straightenSlider") ||
                             candidate->objectName().startsWith(QStringLiteral("correctionWidth")) ||
                             candidate->objectName().startsWith(QStringLiteral("correctionHeight"));
        if (control && candidate->isVisible() && candidate->width() > 0) {
          parts.append(candidate);
        }
        return false;
      });
      QVERIFY(parts.size() >= 20);

      const auto rect = [](QQuickItem* part) {
        return part->mapRectToScene(QRectF(0, 0, part->width(), part->height()));
      };
      const QRectF panel = rect(controls->parentItem());
      for (qsizetype i = 0; i < parts.size(); ++i) {
        const QRectF a = rect(parts[i]);
        QVERIFY2(panel.contains(a), qPrintable(parts[i]->property("label").toString()));
        for (qsizetype j = i + 1; j < parts.size(); ++j) {
          const QRectF overlap = a.intersected(rect(parts[j]));
          const auto name = [&rect](QQuickItem* part) {
            const QRectF r = rect(part);
            return part->objectName() + QLatin1Char(' ') + part->property("label").toString() +
                   QStringLiteral(" %1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
          };
          QVERIFY2(overlap.width() < 1 || overlap.height() < 1,
                   qPrintable(QStringLiteral("%1x%2: ").arg(size.width()).arg(size.height()) +
                              name(parts[i]) + QStringLiteral(" / ") + name(parts[j])));
        }
      }
    }
  }

  void correctionSheetCopiesTheSelectedRegion() {
    const QString path = m_scratch.filePath(QStringLiteral("region-disposable.png"));
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(Qt::darkGreen);
    QVERIFY(source.save(path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      m_window->resize(1280, 820);
    });

    QQuickItem* sheet = item("correctionSheet");
    QSignalSpy copied(sheet, SIGNAL(copied()));
    perform(QStringLiteral("corrections"), path);
    QTRY_VERIFY(sheet->isVisible());
    QTRY_COMPARE(item("correctionPreview")->property("status").toInt(), 1);

    QQuickItem* copyButton = item("correctionCopyRegion");
    QVERIFY(copyButton);
    click(copyButton);
    QTRY_COMPARE_WITH_TIMEOUT(copied.size(), 1, 15000);
    // Copying does not write a file, so the sheet stays open for another pick.
    QVERIFY(sheet->isVisible());
  }

  void correctionControlsApplyAndSaveACopy() {
    const QString path = m_scratch.filePath(QStringLiteral("correction-disposable.png"));
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      for (int suffix = 0; suffix < 4; ++suffix) {
        const QString out = suffix == 0
                                ? m_scratch.filePath(QStringLiteral("correction-disposable-edited.png"))
                                : m_scratch.filePath(
                                      QStringLiteral("correction-disposable-edited-%1.png").arg(suffix + 1));
        // A tile was built for the saved copy; removing it makes that tile's
        // thumbnail request fail while it is still on screen.
        m_disposablePaths.append(out);
        QFile::remove(out);
      }
      // The saved copy joined the shared library model. Drop it again before
      // the next test, or pathAt(0) hands it a file that no longer exists.
      QMetaObject::invokeMethod(m_captures, "refresh");
      // A follow-up scan can still be pending once this one stops.
      waitForScan();
      QTRY_COMPARE(m_captures->rowOf(ImageEditor::outputPathFor(path)), -1);
      m_window->resize(1280, 820);
    });

    QQuickItem* sheet = item("correctionSheet");
    perform(QStringLiteral("corrections"), path);
    QTRY_VERIFY(sheet->isVisible());
    QTRY_COMPARE(item("correctionPreview")->property("status").toInt(), 1);

    QCOMPARE(sheet->property("quarterTurns").toInt(), 0);
    click(pill(sheet, QStringLiteral("Rotate right")));
    QCOMPARE(sheet->property("quarterTurns").toInt(), 1);
    QVERIFY(sheet->isVisible());

    click(pill(sheet, QStringLiteral("Flip H")));
    QVERIFY(sheet->property("flipHorizontal").toBool());
    QVERIFY(sheet->isVisible());

    click(pill(sheet, QStringLiteral("50%")));
    QVERIFY(sheet->property("targetWidth").toInt() > 0);

    QQuickItem* save = item("correctionSave");
    QVERIFY(save);
    click(save);
    QTRY_VERIFY_WITH_TIMEOUT(!sheet->isVisible(), 15000);

    const QString output = m_scratch.filePath(QStringLiteral("correction-disposable-edited.png"));
    QVERIFY2(QFileInfo::exists(output), qPrintable(output));
    QVERIFY(QFileInfo::exists(path));
    const QImage copy(output);
    QVERIFY(!copy.isNull());
    // Rotated from 80x60 to 60x80, then scaled to half: 30x40.
    QCOMPARE(copy.size(), QSize(30, 40));
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

    // Mouse side buttons navigate media without taking normal clicks.
    const QPoint at = centre(detail);
    QTest::mouseClick(m_window, Qt::ForwardButton, Qt::NoModifier, at);
    QTRY_VERIFY(detail->property("path").toString() != shown);
    QTest::mouseClick(m_window, Qt::BackButton, Qt::NoModifier, at);
    QTRY_COMPARE(detail->property("path").toString(), shown);

    // Two quick clicks on Rotate: two rotations, and never the double-tap that
    // used to open whatever tile sat under the button.
    QQuickItem* rotate = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((rotate = pill(detail, QStringLiteral("Rotate right"))) != nullptr, 5000);
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

  void videoPosterOnlyLoadsForAnOpenVideo() {
    QQuickItem* poster = item("detailVideoPoster");
    QVERIFY(poster);
    openDetail(m_library->rowOf(QFileInfo(m_oddPath).canonicalFilePath()));
    QVERIFY(poster->property("source").toUrl().isEmpty());

    int documentRow = -1;
    int videoRow = -1;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isDocumentAt(row)) documentRow = row;
      if (m_library->isVideoAt(row)) videoRow = row;
    }
    QVERIFY(documentRow >= 0);
    QVERIFY(videoRow >= 0);
    invoke("dismissTopLayer");
    openDetail(documentRow);
    QVERIFY(poster->property("source").toUrl().isEmpty());
    invoke("dismissTopLayer");
    openDetail(videoRow);
    QVERIFY(!poster->property("source").toUrl().isEmpty());
    QCOMPARE(poster->property("sourceSize").toSize(),
             QSize(qRound(poster->width()), qRound(poster->height())));
    invoke("dismissTopLayer");
    QTRY_VERIFY(poster->property("source").toUrl().isEmpty());
  }

  void startupReadinessExcludesPlaceholdersAndFailedImages() {
    QQuickItem* grid = item("library");
    const auto viewportReady = [grid] {
      QVariant ready;
      QMetaObject::invokeMethod(grid, "viewportReady", Q_RETURN_ARG(QVariant, ready));
      return ready.toBool();
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewportReady(), 15000);
    QQuickItem* card = cardFor(pathAt(0));
    QVERIFY(card);
    // A decoded thumbnail which is still faded out is not a useful tile.
    card->setProperty("thumbnailReady", false);
    QTRY_VERIFY(!card->property("thumbnailPresented").toBool());
    QVERIFY(!viewportReady());
    card->setProperty("thumbnailReady", true);
    QTRY_VERIFY(viewportReady());

    openDetail(m_library->rowOf(QFileInfo(m_oddPath).canonicalFilePath()));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->property("imageReady").toBool());
    const QString missing = m_scratch.filePath(QStringLiteral("missing-image.png"));
    // Only the still decoder runs; no hidden video poster requests this path.
    m_expectedQmlWarnings = {QStringLiteral("Cannot open: ") + QUrl::fromLocalFile(missing).toString()};
    detail->setProperty("path", missing);
    QTRY_VERIFY(!detail->property("playbackError").toString().isEmpty());
    QVERIFY(detail->property("stillReady").toBool());
    QVERIFY(!detail->property("imageReady").toBool());
    QTRY_VERIFY(m_expectedQmlWarnings.isEmpty());
    invoke("dismissTopLayer");
    QVERIFY(!detail->property("imageReady").toBool());
  }

  void galleryThumbnailSizingRecyclingAndScrolledModelChanges() {
    // Synthetic rows must not react to background layout/reset signals from
    // the main test window's unrelated library.
    CaptureFilterModel thumbnailCaptures;
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    auto* provider = new HeldThumbnailProvider;
    engine.addImageProvider(QStringLiteral("thumbs"), provider);
    auto* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("Theme"), m_theme);
    context->setContextProperty(QStringLiteral("Settings"), m_settings);
    context->setContextProperty(QStringLiteral("Captures"), &thumbnailCaptures);
    context->setContextProperty(QStringLiteral("Library"), m_captures);
    context->setContextProperty(QStringLiteral("MediaMetadata"), m_mediaMetadata);
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, &engine, [&warnings](const QList<QQmlError>& errors) {
      for (const auto& error : errors) warnings.append(error.toString());
    });
    engine.loadData(R"QML(
      import QtQuick
      import Omaroll
      Window {
        width: Settings.tileWidth * 2 + 12
        height: Math.round(Settings.tileWidth * 0.68) * 2
        ListModel { id: captures }
        CaptureGrid { objectName: "sizingGrid"; anchors.fill: parent; model: captures }
        CaptureCard {
          objectName: "recycledCard"; width: Settings.tileWidth; height: parent.height / 2
          visible: false; z: 100
        }
        // Holds decoded tiles in Qt's pixmap cache, as the same source, size
        // and fill mode share one entry there.
        property var keptThumbnails: []
        Repeater {
          model: keptThumbnails
          Image {
            objectName: "keptThumbnail"
            visible: false; asynchronous: true; fillMode: Image.PreserveAspectCrop
            source: modelData.source; sourceSize: modelData.size
          }
        }
        function insertRowAbove() {
          for (let i = 0; i < 2; ++i) {
            captures.insert(i, { path: "/thumbnail-test/" + (2000 + i) + ".png", fileName: "",
                                 kindLabel: "", timeLabel: "", dateLabel: "", sizeLabel: "", isVideo: false,
                                 isDocument: false, favorite: false, rating: 0, hidden: false,
                                 stamp: 1, thumbnailVersion: "1", ocrSnippet: "", caption: "", rawFormat: "" })
          }
        }
        function removeRowAbove() { captures.remove(0, 2) }
        function populate() {
          for (let i = 0; i < 100; ++i) {
            captures.append({ path: "/thumbnail-test/" + i + ".png", fileName: "",
                              kindLabel: "", timeLabel: "", dateLabel: "", sizeLabel: "", isVideo: false,
                              isDocument: false, favorite: false, rating: 0, hidden: false,
                              stamp: 1, thumbnailVersion: "1", ocrSnippet: "", caption: "", rawFormat: "" })
          }
        }
      }
    )QML");
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    QVERIFY(window);
    // Ensure even an early assertion releases held responses before teardown.
    const auto release = qScopeGuard([&] { provider->completeAll(); });
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto* gallery = window->findChild<QQuickItem*>(QStringLiteral("sizingGrid"));
    QVERIFY(gallery);
    auto* view = find(gallery, [](QQuickItem* candidate) {
      return candidate->property("cellHeight").isValid();
    });
    QVERIFY(view);
    const auto diagnostics = qScopeGuard([&] {
      if (!QTest::currentTestFailed()) return;
      qInfo() << "Gallery geometry" << window->size() << view->size()
              << "columns" << gallery->property("columns")
              << "cell" << view->property("cellWidth") << view->property("cellHeight")
              << "contentY" << view->property("contentY") << "originY" << view->property("originY");
      provider->dump();
    });
    // Wait for geometry and its pending relayout, rather than racing an
    // independent timer against window exposure on the OpenGL backend.
    QTRY_COMPARE(gallery->width(), qreal(window->width()));
    QTRY_COMPARE(view->property("cellWidth").toInt(), m_settings->tileWidth());
    QObject* relayout = nullptr;
    for (QObject* child : gallery->findChildren<QObject*>()) {
      if (child->property("interval").toInt() == 80 && child->property("running").isValid()) {
        relayout = child;
        break;
      }
    }
    QVERIFY(relayout);
    QTRY_VERIFY(!relayout->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(window, "populate"));
    const qreal cellHeight = view->property("cellHeight").toReal();
    QCOMPARE(gallery->property("columns").toInt(), 2);
    const auto viewportReady = [&] {
      QVariant ready;
      QMetaObject::invokeMethod(gallery, "viewportReady", Q_RETURN_ARG(QVariant, ready));
      return ready.toBool();
    };
    // Qt's software scene graph can round the final fade by one channel value.
    // Keep the comparison tight while distinguishing these red/blue sources.
    const auto pixelMatches = [](const QColor& actual, const QColor& expected) {
      return actual.alpha() == expected.alpha()
             && qAbs(actual.red() - expected.red()) <= 2
             && qAbs(actual.green() - expected.green()) <= 2
             && qAbs(actual.blue() - expected.blue()) <= 2;
    };
    // Visible and buffered cards use their final size for the first request,
    // rather than decoding at fallback size and immediately requesting again.
    QTRY_VERIFY(provider->rows().contains(0) && provider->rows().contains(1)
                && provider->rows().contains(2) && provider->rows().contains(3));
    QTest::qWait(240);
    QVERIFY(!provider->hasEmptySizeRequest());
    for (int row : {0, 1, 2, 3}) QCOMPARE(provider->rows().count(row), 1);
    provider->completeAll();
    QTRY_VERIFY(viewportReady());
    // Qt drops thumbnails nothing shows from its shared cache, a quarter of
    // them every 30 seconds or the oldest once the main window's push it past
    // its budget. Keep the first rows' while the grid scrolls away, so coming
    // back checks the tiles rather than how long this test took.
    {
      QVariantList kept;
      for (int row : {0, 1, 2, 3}) {
        const QString path = QStringLiteral("/thumbnail-test/%1.png").arg(row);
        QQuickItem* tile = find(view, [&path](QQuickItem* candidate) {
          return candidate->property("thumbnailLayoutReady").isValid()
                 && candidate->property("path").toString() == path;
        });
        QVERIFY(tile);
        QQuickItem* image = find(tile, [](QQuickItem* candidate) {
          return candidate->property("sourceSize").isValid();
        });
        QVERIFY(image);
        kept.append(QVariantMap{{QStringLiteral("source"), image->property("source")},
                                {QStringLiteral("size"), image->property("sourceSize")}});
      }
      window->setProperty("keptThumbnails", kept);
      const auto keptReady = [&] {
        const QList<QQuickItem*> children = window->contentItem()->childItems();
        return std::count_if(children.cbegin(), children.cend(), [](QQuickItem* candidate) {
          return candidate->objectName() == QStringLiteral("keptThumbnail")
                 && candidate->property("status").toInt() == 1; // Image.Ready.
        });
      };
      QTRY_COMPARE(keptReady(), 4);
    }
    // Keep selection in the synthetic viewport: changing an offscreen current
    // index during insertion lets Qt scroll toward that unrelated selection.
    view->setProperty("currentIndex", 62);
    view->setProperty("contentY", cellHeight * 31);
    QTRY_VERIFY(provider->rows().contains(62) && provider->rows().contains(63)
                && provider->rows().contains(64) && provider->rows().contains(65));
    provider->completeAll();
    QTRY_VERIFY(viewportReady());
    // GridView preserves the on-screen cells when a full row changes above
    // them by shifting originY. Index arithmetic must follow that origin.
    const auto visiblePaths = [&] {
      QSet<QString> paths;
      const QRectF viewport = view->mapRectToScene(view->boundingRect());
      std::function<void(QQuickItem*)> collect = [&](QQuickItem* parent) {
        for (QQuickItem* child : parent->childItems()) {
          if (child->property("thumbnailLayoutReady").isValid() && child->isVisible() &&
              child->mapRectToScene(child->boundingRect()).intersects(viewport)) {
            paths.insert(child->property("path").toString());
          }
          collect(child);
        }
      };
      collect(view);
      return paths;
    };
    const QSet<QString> beforeInsertion = visiblePaths();
    QCOMPARE(beforeInsertion, (QSet<QString>{QStringLiteral("/thumbnail-test/62.png"),
                                             QStringLiteral("/thumbnail-test/63.png"),
                                             QStringLiteral("/thumbnail-test/64.png"),
                                             QStringLiteral("/thumbnail-test/65.png")}));
    const qreal originalOrigin = view->property("originY").toReal();
    QVERIFY(QMetaObject::invokeMethod(window, "insertRowAbove"));
    QTRY_VERIFY(view->property("originY").toReal() != originalOrigin);
    QTRY_COMPARE(visiblePaths(), beforeInsertion);
    QTRY_VERIFY(viewportReady());
    const auto cardForRow = [&](int row) {
      const QString path = QStringLiteral("/thumbnail-test/%1.png").arg(row);
      return find(view, [&path](QQuickItem* candidate) {
        return candidate->property("thumbnailLayoutReady").isValid()
               && candidate->property("path").toString() == path
               && candidate->parentItem()->property("index").toInt() >= 0;
      });
    };
    // The unadjusted index range includes the preceding buffered row. Make
    // that row unready so it cannot accidentally let incorrect arithmetic pass.
    auto* above = cardForRow(60);
    QVERIFY(above);
    above->setProperty("thumbnailReady", false);
    QTRY_VERIFY(!above->property("thumbnailPresented").toBool());
    QVERIFY(viewportReady());
    auto* visibleCard = cardForRow(64);
    QVERIFY(visibleCard);
    visibleCard->setProperty("thumbnailReady", false);
    QTRY_VERIFY(!visibleCard->property("thumbnailPresented").toBool());
    QVERIFY(!viewportReady());
    visibleCard->setProperty("thumbnailReady", true);
    QTRY_VERIFY(viewportReady());
    above->setProperty("thumbnailReady", true);
    for (int row : {62, 63, 64, 65}) QCOMPARE(provider->rows().count(row), 1);
    QVERIFY(QMetaObject::invokeMethod(window, "removeRowAbove"));
    QTRY_COMPARE(view->property("originY").toReal(), originalOrigin);
    QTRY_COMPARE(visiblePaths(), beforeInsertion);
    QTRY_VERIFY(viewportReady());
    for (int row : {62, 63, 64, 65}) QCOMPARE(provider->rows().count(row), 1);

    view->setProperty("contentY", 0);
    QTest::qWait(240);
    for (int row : {0, 1, 2, 3}) QCOMPARE(provider->rows().count(row), 1);
    QTRY_VERIFY(viewportReady());

    // Simulate a recycled card's path reassignment while its old pixmap is
    // ready and the replacement is held. The retained old pixels must vanish
    // immediately, rather than fade over the new file's placeholder.
    auto* card = window->findChild<QQuickItem*>(QStringLiteral("recycledCard"));
    QVERIFY(card);
    card->setVisible(true);
    card->setProperty("path", QStringLiteral("/thumbnail-test/1000.png"));
    QTRY_VERIFY(provider->rows().contains(1000));
    provider->completeAll();
    QTRY_VERIFY(card->property("thumbnailPresented").toBool());
    const QPoint pixel(qRound(card->width() / 2), qRound(card->height() / 2));
    QTRY_VERIFY(pixelMatches(window->grabWindow().pixelColor(pixel), QColor(Qt::red)));
    card->setProperty("path", QStringLiteral("/thumbnail-test/1001.png"));
    QTRY_VERIFY(provider->rows().contains(1001));
    QVERIFY(!card->property("thumbnailPresented").toBool());
    auto* thumbnail = find(card, [](QQuickItem* candidate) {
      return candidate->property("sourceSize").isValid();
    });
    QVERIFY(thumbnail);
    QVERIFY(!thumbnail->isVisible());
    QVERIFY(window->grabWindow().pixelColor(pixel) != QColor(Qt::red));
    QTRY_COMPARE(thumbnail->opacity(), 0.0);
    provider->completeAll();
    QTRY_VERIFY(card->property("thumbnailPresented").toBool());
    QTRY_VERIFY(pixelMatches(window->grabWindow().pixelColor(pixel), QColor(Qt::blue)));
    QCOMPARE(thumbnail->property("status").toInt(), 1); // Image.Ready.
    QVERIFY(thumbnail->opacity() >= 0.999);
    QCOMPARE(card->property("readyPath").toString(), QStringLiteral("/thumbnail-test/1001.png"));
    QCOMPARE(card->property("readyIdentity").toString(), QStringLiteral("0"));
    // A same-path rewrite must also hide the previous version while decoding.
    card->setProperty("stamp", 1);
    QTRY_COMPARE(provider->count(1001, 1), 1);
    QVERIFY(!card->property("thumbnailPresented").toBool());
    QVERIFY(!card->property("thumbnailReady").toBool());
    QVERIFY(!thumbnail->isVisible());
    QVERIFY(window->grabWindow().pixelColor(pixel) != QColor(Qt::blue));
    // Hold the replacement through the old fade-out. Reversing an animation
    // within its first tick can leave a one-level alpha rounding difference in
    // the software renderer; the identity checks above still run immediately.
    QTRY_COMPARE(thumbnail->opacity(), 0.0);
    provider->completeAll();
    QTRY_VERIFY(card->property("thumbnailPresented").toBool());
    QTRY_VERIFY(pixelMatches(window->grabWindow().pixelColor(pixel), QColor(Qt::red)));
    QCOMPARE(thumbnail->property("status").toInt(), 1);
    QVERIFY(thumbnail->opacity() >= 0.999);
    QCOMPARE(card->property("readyPath").toString(), QStringLiteral("/thumbnail-test/1001.png"));
    QCOMPARE(card->property("readyIdentity").toString(), QStringLiteral("1"));
    // A replacement identity changes the URL even when mtime stays put.
    card->setProperty("thumbnailVersion", QStringLiteral("2"));
    QTRY_COMPARE(provider->count(1001, 2), 1);
    QVERIFY(!thumbnail->isVisible());
    provider->completeAll();
    QTRY_VERIFY(card->property("thumbnailPresented").toBool());
    QTRY_VERIFY(pixelMatches(window->grabWindow().pixelColor(pixel), QColor(Qt::blue)));
    const QSize oldSize = thumbnail->property("sourceSize").toSize();
    card->setWidth(card->width() + 1);
    QTRY_VERIFY(thumbnail->property("sourceSize").toSize() != oldSize);
    QTRY_COMPARE(provider->count(1001, 2), 2);
    provider->completeAll();
    QTRY_VERIFY(card->property("thumbnailPresented").toBool());

    const QString identity = card->property("thumbnailIdentity").toString();
    const int requests = provider->count(1001, 2);
    card->setProperty("thumbnailDpr", 2.0);
    QTRY_COMPARE(provider->count(1001, 2), requests + 1);
    QCOMPARE(card->property("thumbnailIdentity").toString(), identity);
    QVERIFY(thumbnail->isVisible());
    QVERIFY(card->property("thumbnailReady").toBool());
    provider->completeAll();
    QTRY_VERIFY(card->property("thumbnailPresented").toBool());
    QVERIFY(!provider->hasEmptySizeRequest());
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }

  // A reused grid delegate gets its new row's roles one at a time, in an order
  // Qt does not fix. When the file's thumbnail version arrives before its
  // mtime and Qt still caches that thumbnail, the load finishes in between.
  // The mtime that follows does not change the URL and must not hide it.
  void reusedCardKeepsACachedThumbnailWhateverTheRoleOrder() {
    CaptureFilterModel cardCaptures;
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    auto* provider = new HeldThumbnailProvider;
    engine.addImageProvider(QStringLiteral("thumbs"), provider);
    auto* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("Theme"), m_theme);
    context->setContextProperty(QStringLiteral("Settings"), m_settings);
    context->setContextProperty(QStringLiteral("Captures"), &cardCaptures);
    context->setContextProperty(QStringLiteral("Library"), m_captures);
    context->setContextProperty(QStringLiteral("MediaMetadata"), m_mediaMetadata);
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, &engine, [&warnings](const QList<QQmlError>& errors) {
      for (const auto& error : errors) warnings.append(error.toString());
    });
    engine.loadData(R"QML(
      import QtQuick
      import Omaroll
      Window {
        width: 320
        height: 220
        CaptureCard { objectName: "reusedCard"; anchors.fill: parent }
      }
    )QML");
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    QVERIFY(window);
    const auto release = qScopeGuard([&] { provider->completeAll(); });
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto* card = window->findChild<QQuickItem*>(QStringLiteral("reusedCard"));
    QVERIFY(card);
    auto* snippet = window->findChild<QQuickItem*>(QStringLiteral("captureSnippetLabel"));
    QVERIFY(snippet);
    const auto assign = [&](int row, int version, double stamp, bool versionFirst) {
      card->setProperty("path", QStringLiteral("/thumbnail-test/%1.png").arg(row));
      if (versionFirst) card->setProperty("thumbnailVersion", QString::number(version));
      card->setProperty("stamp", stamp);
      if (!versionFirst) card->setProperty("thumbnailVersion", QString::number(version));
    };
    const auto presented = [&](int row, int version) {
      return card->property("thumbnailPresented").toBool()
             && card->property("thumbnailReady").toBool()
             && card->property("readyPath").toString()
                    == QStringLiteral("/thumbnail-test/%1.png").arg(row)
             && card->property("readyIdentity").toString() == QString::number(version);
    };

    for (const bool versionFirst : {true, false}) {
      const int base = versionFirst ? 1 : 101;
      assign(base, 11, 100, versionFirst);
      QTRY_COMPARE(provider->count(base, 11), 1);
      provider->completeAll();
      QTRY_VERIFY(presented(base, 11));
      assign(base + 1, 22, 200, versionFirst);
      QTRY_COMPARE(provider->count(base + 1, 22), 1);
      QVERIFY(!card->property("thumbnailReady").toBool());
      provider->completeAll();
      QTRY_VERIFY(presented(base + 1, 22));
      // Back to the first file, whose thumbnail Qt still has.
      assign(base, 11, 100, versionFirst);
      QTRY_VERIFY2(presented(base, 11),
                   qPrintable(QStringLiteral("versionFirst %1 ready %2 status %3")
                                  .arg(versionFirst)
                                  .arg(card->property("thumbnailReady").toBool())
                                  .arg(find(card, [](QQuickItem* candidate) {
                                         return candidate->property("sourceSize").isValid();
                                       })->property("status").toInt())));
      QCOMPARE(provider->count(base, 11), 1);
      provider->completeAll();
    }

    // The matched text shows while the thumbnail is still being made.
    QVERIFY(!snippet->isVisible());
    assign(3, 33, 300, true);
    card->setProperty("ocrSnippet", QStringLiteral("ACME invoice 7781"));
    QTRY_COMPARE(provider->count(3, 33), 1);
    QVERIFY(!card->property("thumbnailReady").toBool());
    QVERIFY(snippet->isVisible());
    QCOMPARE(snippet->property("text").toString(), QStringLiteral("ACME invoice 7781"));
    provider->completeAll();
    QTRY_VERIFY(presented(3, 33));
    QVERIFY(snippet->isVisible());
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }

  // In a large library a search that hides or brings back thousands of rows
  // applies in one go, as a layout change, so every visible delegate is
  // reused for another row. The selected file, the scroll anchor and every
  // visible thumbnail come back, each tile showing its own file.
  void largeSearchChangesKeepTheGridPlaceAndThumbnails_data() {
    QTest::addColumn<bool>("sectioned");
    QTest::newRow("date sections") << true;
    // Unrated, the rating sort keeps newest first in one flat grid.
    QTest::newRow("flat grid") << false;
  }

  void largeSearchChangesKeepTheGridPlaceAndThumbnails() {
    QFETCH(bool, sectioned);
    CaptureModel captures(m_settings);
    QTRY_VERIFY_WITH_TIMEOUT(!captures.scanning(), 15000);
    constexpr int kRows = 6000;
    QList<CaptureRecord> records;
    records.reserve(kRows);
    const QDateTime newest(QDate(2026, 1, 1), QTime(12, 0));
    for (int row = 0; row < kRows; ++row) {
      CaptureRecord record;
      record.fileName = QStringLiteral("%1.png").arg(row);
      record.path = QStringLiteral("/large-grid-test/") + record.fileName;
      record.kind = CaptureRecord::Picture;
      record.captured = newest.addSecs(-qint64(row) * 60);
      record.modified = record.captured.toMSecsSinceEpoch();
      record.bytes = 1000;
      records.append(record);
    }
    captures.setRecordsForTesting(records);
    CaptureFilterModel library;
    library.setSourceModel(&captures);
    if (!sectioned) library.setSortMode(CaptureFilterModel::RatingFirst);
    QCOMPARE(library.rowCount(), kRows);
    QCOMPARE(library.pathAt(10), records.at(10).path);

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    auto* provider = new HeldThumbnailProvider;
    engine.addImageProvider(QStringLiteral("thumbs"), provider);
    auto* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("Theme"), m_theme);
    context->setContextProperty(QStringLiteral("Settings"), m_settings);
    context->setContextProperty(QStringLiteral("Captures"), &library);
    context->setContextProperty(QStringLiteral("Library"), &captures);
    context->setContextProperty(QStringLiteral("MediaMetadata"), m_mediaMetadata);
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, &engine, [&warnings](const QList<QQmlError>& errors) {
      for (const auto& error : errors) warnings.append(error.toString());
    });
    engine.loadData(R"QML(
      import QtQuick
      import Omaroll
      Window {
        width: Settings.tileWidth * 3 + 24
        height: Math.round(Settings.tileWidth * 0.68) * 3
        CaptureGrid { objectName: "largeGrid"; anchors.fill: parent; model: Captures }
      }
    )QML");
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    QVERIFY(window);
    const auto release = qScopeGuard([&] { provider->completeAll(); });
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto* grid = window->findChild<QQuickItem*>(QStringLiteral("largeGrid"));
    QVERIFY(grid);
    // The flat GridView, or the sections' ListView of header and tile rows.
    auto* view = find(grid, [sectioned](QQuickItem* candidate) {
      return sectioned ? candidate->objectName() == QStringLiteral("reviewSetsList")
                       : candidate->objectName() == QStringLiteral("flatGrid");
    });
    QVERIFY(view);
    QTRY_VERIFY(view->isVisible());
    QTRY_COMPARE(grid->width(), qreal(window->width()));
    auto* relayout = window->findChild<QObject*>(QStringLiteral("gridRelayoutTimer"));
    QVERIFY(relayout);
    QTRY_VERIFY(!relayout->property("running").toBool());
    QTRY_VERIFY(grid->property("layoutReady").toBool());
    const int columns = grid->property("columns").toInt();
    QVERIFY(columns >= 2);
    const auto settled = [&] {
      provider->completeAll();
      QVariant ready;
      QMetaObject::invokeMethod(grid, "viewportReady", Q_RETURN_ARG(QVariant, ready));
      return ready.toBool();
    };
    // The list rows on screen, top to bottom.
    const auto shownRows = [&] {
      QList<QQuickItem*> rows;
      const qreal top = view->property("contentY").toReal();
      for (qreal y = top + 1; y < top + view->height();) {
        QQuickItem* row = nullptr;
        QMetaObject::invokeMethod(view, "itemAt", Q_RETURN_ARG(QQuickItem*, row),
                                  Q_ARG(qreal, 20), Q_ARG(qreal, y));
        if (!row) break;
        rows.append(row);
        y = row->y() + row->height() + 1;
      }
      return rows;
    };
    // The tiles of the top tile row.
    const auto topRowPaths = [&] {
      QStringList paths;
      for (QQuickItem* row : shownRows()) {
        if (row->property("header").toBool()) continue;
        const int first = row->property("first").toInt();
        for (int i = 0; i < row->property("count").toInt(); ++i) paths.append(library.pathAt(first + i));
        break;
      }
      return paths;
    };
    const auto topLeftPath = [&] {
      if (sectioned) {
        const QStringList paths = topRowPaths();
        return paths.isEmpty() ? QString() : paths.first();
      }
      int index = -1;
      QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, index),
                                Q_ARG(qreal, view->property("contentX").toReal() + 4),
                                Q_ARG(qreal, view->property("contentY").toReal() + 4));
      return index >= 0 ? library.pathAt(index) : QString();
    };
    // Every visible cell holds its own row's file, and its thumbnail is of
    // that file.
    const auto cellsMatchRows = [&] {
      if (sectioned) {
        int shown = 0;
        for (QQuickItem* row : shownRows()) {
          if (row->property("header").toBool()) continue;
          const int first = row->property("first").toInt();
          const int count = row->property("count").toInt();
          QList<QQuickItem*> cells;
          find(row, [&cells](QQuickItem* candidate) {
            if (candidate->property("used").isValid() && candidate->property("row").isValid())
              cells.append(candidate);
            return false;
          });
          int used = 0;
          for (QQuickItem* cell : cells) {
            if (!cell->property("used").toBool()) continue;
            const int index = cell->property("row").toInt();
            if (index < first || index >= first + count) return false;
            if (cell->property("path").toString() != library.pathAt(index)) return false;
            auto* card = find(cell, [](QQuickItem* candidate) {
              return candidate->property("readyPath").isValid();
            });
            if (!card || card->property("readyPath").toString() != library.pathAt(index)) return false;
            ++used;
          }
          if (used != count) return false;
          shown += used;
        }
        return shown > 0;
      }
      const qreal cellHeight = view->property("cellHeight").toReal();
      const qreal top = view->property("contentY").toReal() - view->property("originY").toReal();
      const int first = std::max(0, int(top / cellHeight)) * columns;
      const int end = std::min(library.rowCount(),
                               int(std::ceil((top + view->height()) / cellHeight)) * columns);
      for (int index = first; index < end; ++index) {
        QQuickItem* cell = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell),
                                  Q_ARG(int, index));
        if (!cell || cell->property("path").toString() != library.pathAt(index)) return false;
        auto* card = find(cell, [](QQuickItem* candidate) {
          return candidate->property("readyPath").isValid();
        });
        if (!card || card->property("readyPath").toString() != library.pathAt(index)) return false;
      }
      return end > first;
    };
    QTRY_VERIFY_WITH_TIMEOUT(settled(), 10000);

    // Deep in the library, with a tile selected.
    const qreal cellHeight = grid->property("cellWidth").toReal() * 0.68;
    QMetaObject::invokeMethod(grid, "scrollTo", Q_ARG(QVariant, std::round(cellHeight) * 700));
    QTRY_VERIFY_WITH_TIMEOUT(settled(), 10000);
    const QString anchor = topLeftPath();
    if (!sectioned) QCOMPARE(anchor, library.pathAt(700 * columns));
    QVERIFY(!anchor.isEmpty());
    // An odd row, which only stays because it is the selected one.
    const int selectedRow = 701 * columns + ((701 * columns) % 2 == 0 ? 1 : 2);
    const QString selected = library.pathAt(selectedRow);
    grid->setProperty("currentIndex", selectedRow);
    QCOMPARE(grid->property("selectedPath").toString(), selected);
    QTRY_VERIFY(cellsMatchRows());

    // Leave out every odd row but those two: thousands of changes at once.
    int dropped = 0;
    for (int row = 1; row < kRows; row += 2) {
      if (records.at(row).path != anchor && records.at(row).path != selected) {
        library.setOcrText(records.at(row).path, QStringLiteral("drop"));
        ++dropped;
      }
    }
    QSignalSpy layouts(&library, &QAbstractItemModel::layoutChanged);
    QSignalSpy removed(&library, &QAbstractItemModel::rowsRemoved);
    for (const QString& query : {QStringLiteral("-drop"), QString()}) {
      layouts.clear();
      removed.clear();
      library.setSearchText(query);
      QCOMPARE(library.rowCount(), query.isEmpty() ? kRows : kRows - dropped);
      QVERIFY2(layouts.size() >= 1 && removed.isEmpty(), qPrintable(query));
      QTRY_COMPARE(grid->property("currentIndex").toInt(), library.rowOf(selected));
      QCOMPARE(grid->property("selectedPath").toString(), selected);
      // Sections lay their rows out per day, so with rows gone the anchor
      // can sit further along its row; the row holding it stays on top.
      if (sectioned) QTRY_VERIFY2(topRowPaths().contains(anchor), qPrintable(topRowPaths().join(' ')));
      else QTRY_COMPARE(topLeftPath(), anchor);
      QTRY_VERIFY_WITH_TIMEOUT(settled(), 10000);
      QTRY_VERIFY(cellsMatchRows());
    }
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }

  // The preview has the quick viewer's icon toolbar, and each of its buttons
  // does what its name says.
  void previewToolbarDrivesThePicture() {
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
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(detail, "setSlideshow", Q_ARG(QVariant, false));
      QMetaObject::invokeMethod(detail, "setFullScreen", Q_ARG(QVariant, false));
      QMetaObject::invokeMethod(detail, "resetImageView");
      invoke("dismissTopLayer");
    });
    QTRY_VERIFY(detail->property("stillReady").toBool());
    QQuickItem* bar = item("previewToolbar");
    QTRY_VERIFY(bar->isVisible());
    // Icons, named for assistive technology; no text pills left over.
    for (const auto& [name, label] : {std::pair{"previewZoomOut", "Zoom out"}, std::pair{"previewZoomIn", "Zoom in"},
                                      std::pair{"previewRotateLeft", "Rotate left"},
                                      std::pair{"previewRotateRight", "Rotate right"},
                                      std::pair{"previewFlipHorizontal", "Flip horizontally"},
                                      std::pair{"previewFlipVertical", "Flip vertically"},
                                      std::pair{"previewFullScreenButton", "Full screen"}}) {
      QQuickItem* button = item(QString::fromLatin1(name));
      QVERIFY2(button->isVisible(), name);
      QVERIFY2(!button->property("icon").toString().isEmpty(), name);
      QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(button);
      QVERIFY(accessible);
      QCOMPARE(accessible->text(QAccessible::Name), QString::fromLatin1(label));
    }
    QVERIFY(!item("previewTrashButton")->isVisible());
    QVERIFY(!pill(detail, QStringLiteral("Flip H")));
    QVERIFY(!pill(detail, QStringLiteral("Start slideshow")));
    // The keys shown are the preview's own.
    QCOMPARE(item("previewRotateRight")->property("shortcut").toString(), QStringLiteral("R"));
    QCOMPARE(item("previewFullScreenButton")->property("shortcut").toString(), QStringLiteral("F11"));

    click(item("previewZoomIn"));
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.25);
    click(item("previewZoomOut"));
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.0);
    click(item("previewRotateRight"));
    QTRY_COMPARE(detail->property("imageRotation").toInt(), 90);
    click(item("previewRotateLeft"));
    click(item("previewRotateLeft"));
    QTRY_COMPARE(detail->property("imageRotation").toInt(), 270);
    click(item("previewFlipHorizontal"));
    QTRY_VERIFY(detail->property("imageFlipHorizontal").toBool());
    QVERIFY(item("previewFlipHorizontal")->property("active").toBool());
    click(item("previewFlipVertical"));
    QTRY_VERIFY(detail->property("imageFlipVertical").toBool());
    // The toggle follows the view: fitted offers 1:1; at actual size or any
    // other zoom it offers Fit, which also undoes turns and flips, as 0 does.
    QMetaObject::invokeMethod(detail, "resetImageView");
    QQuickItem* fit = item("previewFitToggle");
    QQuickItem* level = item("previewZoomLevel");
    QVERIFY(level->isVisible());
    const auto percent = [&] {
      return QStringLiteral("%1%").arg(qRound(detail->property("displayedImageScale").toDouble() * 100));
    };
    QTRY_COMPARE(level->property("text").toString(), percent());
    const bool fitIsActual = qAbs(detail->property("displayedImageScale").toDouble() - 1.0) < 0.01;
    if (!fitIsActual) {
      QCOMPARE(fit->property("label").toString(), QStringLiteral("1:1"));
      QCOMPARE(fit->property("shortcut").toString(), QStringLiteral("1"));
      click(fit);
      QTRY_VERIFY(qAbs(detail->property("displayedImageScale").toDouble() - 1.0) < 0.01);
      QTRY_COMPARE(level->property("text").toString(), QStringLiteral("100%"));
      QTRY_COMPARE(fit->property("label").toString(), QStringLiteral("Fit"));
      click(fit);
      QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.0);
      QTRY_COMPARE(fit->property("label").toString(), QStringLiteral("1:1"));
    } else {
      QCOMPARE(fit->property("label").toString(), QStringLiteral("Fit"));
      QVERIFY(fit->property("active").toBool());
    }
    // Zoomed past fit, the way back is Fit, not actual size.
    click(item("previewZoomIn"));
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.25);
    QTRY_COMPARE(level->property("text").toString(), percent());
    QCOMPARE(fit->property("label").toString(), QStringLiteral("Fit"));
    QCOMPARE(fit->property("shortcut").toString(), QStringLiteral("0"));
    QVERIFY(!fit->property("active").toBool());
    // A turn fits the picture again, so turn first and zoom after.
    click(fit);
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.0);
    click(item("previewRotateRight"));
    QTRY_COMPARE(detail->property("imageRotation").toInt(), 90);
    click(item("previewFlipVertical"));
    QTRY_VERIFY(detail->property("imageFlipVertical").toBool());
    click(item("previewZoomIn"));
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.25);
    QTRY_COMPARE(fit->property("label").toString(), QStringLiteral("Fit"));
    click(fit);
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.0);
    QCOMPARE(detail->property("imageRotation").toInt(), 0);
    QVERIFY(!detail->property("imageFlipHorizontal").toBool());
    QVERIFY(!detail->property("imageFlipVertical").toBool());
    click(item("previewFullScreenButton"));
    QTRY_VERIFY(detail->property("fullScreen").toBool());
    QCOMPARE(item("previewFullScreenButton")->property("icon").toString(), QStringLiteral("fullscreen-exit"));
    click(item("previewFullScreenButton"));
    QTRY_VERIFY(!detail->property("fullScreen").toBool());
    if (detail->property("canNavigate").toBool()) {
      const QString shown = detail->property("path").toString();
      click(item("previewToolbarNext"));
      QTRY_VERIFY(detail->property("path").toString() != shown);
      click(item("previewToolbarPrevious"));
      QTRY_COMPARE(detail->property("path").toString(), shown);
      click(item("previewSlideshowButton"));
      QTRY_VERIFY(detail->property("slideshowRunning").toBool());
      // While it runs, the bar steps aside for the slideshow's own controls.
      QTRY_VERIFY(!bar->isVisible());
    }
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
    // The narrow-stage checks below are about the media giving way when the
    // inspector takes its room, so ask for the inspector first.
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());
    QTRY_VERIFY(detail->property("stillReady").toBool());
    QTRY_COMPARE(item("transparencyGrid")->property("status").toInt(), 1);

    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(qAbs(detail->property("displayedImageScale").toDouble() - 1.0) < 0.01);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(detail->property("imageZoom").toDouble(), 1.0);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(qAbs(detail->property("displayedImageScale").toDouble() - 1.0) < 0.01);
    click(item("previewFlipHorizontal"));
    click(item("previewFlipVertical"));
    QVERIFY(detail->property("imageFlipHorizontal").toBool());
    QVERIFY(detail->property("imageFlipVertical").toBool());

    QVERIFY(QMetaObject::invokeMethod(detail, "resetImageView"));
    QVERIFY(QMetaObject::invokeMethod(detail, "adjustImageZoom", Q_ARG(QVariant, 10.0)));
    QVERIFY(detail->property("imageZoom").toDouble() > 4.0);
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_VERIFY(qAbs(detail->property("displayedImageScale").toDouble() - 1.0) < 0.01);
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
    detail->setProperty("isAnimatedImage", true);
    detail->setProperty("animationPlaying", true);
    QSignalSpy action(detail, SIGNAL(actionTriggered(QString)));
    QTest::keyClick(m_window, Qt::Key_Space);
    QVERIFY(!detail->property("animationPlaying").toBool());
    QCOMPARE(action.size(), 0);

    // A narrow stage keeps the toolbar whole: flip, then zoom, give way,
    // and the keys still do both.
    m_window->resize(560, 420);
    QTRY_VERIFY(!item("previewFlipHorizontal")->isVisible());
    QVERIFY(!item("previewFlipVertical")->isVisible());
    QQuickItem* bar = item("previewToolbar");
    QTRY_VERIFY(bar->isVisible());
    const QRectF barBounds = bar->mapRectToScene(bar->boundingRect());
    const QRectF stageBounds = item("previewToolbar")->parentItem()->mapRectToScene(
        item("previewToolbar")->parentItem()->boundingRect());
    QVERIFY2(barBounds.left() >= stageBounds.left() && barBounds.right() <= stageBounds.right(),
             qPrintable(QStringLiteral("%1-%2 in %3-%4").arg(barBounds.left()).arg(barBounds.right())
                            .arg(stageBounds.left()).arg(stageBounds.right())));
    QVERIFY(item("previewRotateRight")->isVisible());
    m_window->resize(1280, 820);
    QTRY_VERIFY(item("previewFitToggle")->isVisible());
    QTRY_VERIFY(item("previewFlipHorizontal")->isVisible());
  }

  // Qt on Wayland can report a mouse wheel as a touchpad. Its notches must
  // still zoom the picture in the viewer; only a phased swipe pans it.
  void imageViewerWheelZoomsWhateverQtCallsTheDevice() {
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
    QVERIFY(QMetaObject::invokeMethod(detail, "resetImageView"));
    QCOMPARE(detail->property("imageZoom").toDouble(), 1.0);

    QPointingDevice touchpad(QStringLiteral("touchpad"), 4243, QInputDevice::DeviceType::TouchPad,
                             QPointingDevice::PointerType::Finger,
                             QInputDevice::Capability::Position | QInputDevice::Capability::Scroll,
                             1, 3);
    const QPointF at(m_window->width() * 0.4, m_window->height() / 2.0);
    QWheelEvent notch(at, m_window->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false,
                      Qt::MouseEventNotSynthesized, &touchpad);
    QCoreApplication::sendEvent(m_window, &notch);
    QTRY_VERIFY(qAbs(detail->property("imageZoom").toDouble() - 1.2) < 0.001);

    // A two-finger swipe pans rather than zooming. Gesture boundaries have
    // no movement; Qt still delivers ScrollEnd while the handler is active.
    for (const Qt::ScrollPhase phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollEnd}) {
      const QPoint delta = phase == Qt::ScrollUpdate ? QPoint(0, -40) : QPoint();
      QWheelEvent swipe(at, m_window->mapToGlobal(at.toPoint()), delta, delta,
                        Qt::NoButton, Qt::NoModifier, phase, false,
                        Qt::MouseEventNotSynthesized, &touchpad);
      QCoreApplication::sendEvent(m_window, &swipe);
      QVERIFY(qAbs(detail->property("imageZoom").toDouble() - 1.2) < 0.001);
    }
    QTest::qWait(50);
    QVERIFY(qAbs(detail->property("imageZoom").toDouble() - 1.2) < 0.001);
    QVERIFY(QMetaObject::invokeMethod(detail, "resetImageView"));
  }

  void externalSidecarSubtitlesShowOnTheVideo() {
    int videoRow = -1;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) {
        videoRow = row;
        break;
      }
    }
    QVERIFY(videoRow >= 0);
    const QString video = m_library->pathAt(videoRow);
    const QFileInfo info(video);
    const QString sidecar = info.dir().filePath(info.completeBaseName() + QStringLiteral(".srt"));
    {
      QFile file(sidecar);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("1\n00:00:01,000 --> 00:00:06,000\nSidecar cue text\n\n");
    }
    const auto cleanup = qScopeGuard([&] {
      QFile::remove(sidecar);
      invoke("dismissTopLayer");
      rescan();
      m_window->resize(1280, 820);
    });

    openDetail(videoRow);
    QQuickItem* detail = item("detail");
    QTRY_VERIFY_WITH_TIMEOUT(
        detail->property("subtitleFiles").toStringList().contains(sidecar), 5000);

    // Cycle the one subtitle control until the sidecar is chosen; some videos
    // also carry an embedded track ahead of it.
    for (int step = 0; step < 6
                       && detail->property("externalSubtitle").toString() != sidecar; ++step) {
      click(item("subtitleButton"));
      QTest::qWait(50);
    }
    QCOMPARE(detail->property("externalSubtitle").toString(), sidecar);

    // The timing nudge for a sidecar sits beside the clock, not on it, and
    // leaves with the CC button when the row runs short.
    QTRY_VERIFY(item("subtitleOffsetControls")->isVisible());
    QTRY_VERIFY(item("videoClock")->isVisible());
    QCOMPARE(labelOverClock(), QString());
    m_window->resize(560, 420);
    QTRY_VERIFY(!item("subtitleOffsetControls")->isVisible());
    // Nor does it squeeze the scrub below its minimum on the way down.
    for (int width = 1280; width >= 560; width -= 20) {
      m_window->resize(width, 820);
      QCoreApplication::processEvents();
      if (item("subtitleOffsetControls")->isVisible()) {
        QVERIFY2(item("videoSeek")->width() >= 120,
                 qPrintable(QStringLiteral("scrub too narrow at %1 px").arg(width)));
      }
    }
    // At the narrowest width that shows it, a nudge neither hides the
    // buttons nor slides them out from under the pointer.
    int narrowest = 560;
    for (; narrowest <= 1280 && !item("subtitleOffsetControls")->isVisible(); narrowest += 2) {
      m_window->resize(narrowest, 820);
      QCoreApplication::processEvents();
    }
    QVERIFY(item("subtitleOffsetControls")->isVisible());
    QQuickItem* later = item("subtitleLater");
    const QPointF laterAt = later->mapToScene(QPointF(0, 0));
    click(later);
    QTRY_COMPARE(detail->property("subtitleOffsetMs").toInt(), 500);
    QCoreApplication::processEvents();
    QVERIFY(item("subtitleOffsetControls")->isVisible());
    QCOMPARE(later->mapToScene(QPointF(0, 0)), laterAt);
    detail->setProperty("subtitleOffsetMs", 0);
    m_window->resize(1280, 820);
    QTRY_VERIFY(item("subtitleOffsetControls")->isVisible());

    auto* player = m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer"));
    QVERIFY(player);
    player->setPosition(2000);
    QQuickItem* overlay = item("subtitleOverlay");
    QVERIFY(overlay);
    QTRY_VERIFY_WITH_TIMEOUT(overlay->property("text").toString().contains(
                                 QStringLiteral("Sidecar cue text")), 5000);

    // Sub + shifts the lookup past the cue; Sub − brings it back.
    detail->setProperty("subtitleOffsetMs", 5000);
    QTRY_VERIFY_WITH_TIMEOUT(overlay->property("text").toString().isEmpty(), 2000);
    detail->setProperty("subtitleOffsetMs", 0);
    QTRY_VERIFY_WITH_TIMEOUT(overlay->property("text").toString().contains(
                                 QStringLiteral("Sidecar cue text")), 2000);
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
    // A clip with one audio track and no subtitles offers neither control.
    // Their labels painted over the clock while their width was zero.
    QVERIFY(!item("audioTrackButton")->isVisible());
    QVERIFY(!item("subtitleButton")->isVisible());
    QVERIFY(!detail->property("playbackMuted").toBool());

    // A control collapsed for want of a second audio track or a subtitle must
    // not leave its label drawn over the clock.
    auto* player = m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer"));
    QVERIFY(player);
    QVERIFY2(player->audioTracks().size() < 2, "the Audio button must be collapsed here");
    QTRY_VERIFY(item("videoClock")->isVisible());
    QCOMPARE(labelOverClock(), QString());

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

    // Closing and visiting a picture must stop decoding, but retain the player
    // and its session controls when the next video is opened.
    const double retainedVolume = detail->property("playbackVolume").toDouble();
    invoke("dismissTopLayer");
    openDetail(m_library->rowOf(QFileInfo(m_oddPath).canonicalFilePath()));
    QTRY_VERIFY(detail->property("imageReady").toBool());
    QTRY_VERIFY(player->source().isEmpty());
    QCOMPARE(player->playbackState(), QMediaPlayer::StoppedState);
    invoke("dismissTopLayer");
    openDetail(videoRow);
    QCOMPARE(m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer")), player);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    QCOMPARE(detail->property("playbackVolume").toDouble(), retainedVolume);
  }

  void rightClickOpensActionsForThePointedTileAndPreservesSelection() {
    QQuickItem* grid = item("library");
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("libraryContextMenu"));
    QVERIFY(menu);
    const QString first = pathAt(0), second = pathAt(1), third = pathAt(2);
    QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, first)));
    QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, second)));
    // The grid may still be recreating delegates after the previous test.
    QTRY_VERIFY(cardFor(second) != nullptr && grid->property("layoutReady").toBool());
    click(cardFor(second), Qt::RightButton);
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(!item("detail")->isVisible());
    const auto targets = [&] { return menu->property("targets").value<QJSValue>().toVariant().toStringList(); };
    QVERIFY(targets().contains(first));
    QVERIFY(targets().contains(second));
    QCOMPARE(targets().size(), 2);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
    click(cardFor(third), Qt::RightButton);
    QTRY_VERIFY(menu->property("visible").toBool());
    QCOMPARE(targets(), QStringList{third});
    QCOMPARE(grid->property("checkedCount").toInt(), 2);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
    QTRY_VERIFY(grid->hasActiveFocus());
  }

  void contextSelectionIsCapturedAndNarrowActionsRemainReachable() {
    QQuickItem* grid = item("library");
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("libraryContextMenu"));
    const QString first = pathAt(0), second = pathAt(1);
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, first));
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, second));
    m_window->resize(560, 420);
    const auto cleanup = qScopeGuard([&] { QMetaObject::invokeMethod(menu, "close"); m_window->resize(1280, 820); });
    QTRY_VERIFY(item("selectionActionsButton")->isVisible());
    waitForLayout(QSize(560, 420));
    click(item("selectionActionsButton"));
    QTRY_VERIFY(menu->property("visible").toBool());
    const QVariantList entries = menu->property("entries").value<QJSValue>().toVariant().toList();
    QVERIFY(std::any_of(entries.begin(), entries.end(), [](const QVariant& row) {
      return row.toMap().value(QStringLiteral("id")) == QStringLiteral("tailscale");
    }));
    QMetaObject::invokeMethod(grid, "clearChecked");
    const QStringList targets = menu->property("targets").value<QJSValue>().toVariant().toStringList();
    QCOMPARE(targets.size(), 2);
    QVERIFY(targets.contains(first) && targets.contains(second));
    QMetaObject::invokeMethod(menu, "close");
  }

  void compareAppearsForImageSelectionsAndUsesTheCapturedPair() {
    QStringList images;
    QString video;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) video = pathAt(row);
      else if (!m_library->isDocumentAt(row)) images.append(pathAt(row));
    }
    QVERIFY(images.size() >= 2 && !video.isEmpty());
    const QString first = images[0], second = images[1];
    QQuickItem* grid = item("library");
    QObject* menu = m_window->findChild<QObject*>("libraryContextMenu");
    QVERIFY(menu);
    const auto hasCompare = [&] {
      const auto rows = menu->property("entries").value<QJSValue>().toVariant().toList();
      return std::any_of(rows.begin(), rows.end(), [](const QVariant& row) {
        return row.toMap().value("id").toString() == QStringLiteral("compare");
      });
    };
    QVERIFY(m_duplicates->groupPaths(first).size() < 2);
    QVERIFY(m_similarities->groupPaths(first).size() < 2);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu",
        Q_ARG(QVariant, m_library->rowOf(first)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(!hasCompare());
    QMetaObject::invokeMethod(menu, "close");
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, first));
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, second));
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu",
        Q_ARG(QVariant, m_library->rowOf(second)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(hasCompare());
    const QStringList captured = menu->property("targets").value<QJSValue>().toVariant().toStringList();
    QMetaObject::invokeMethod(grid, "clearChecked");
    // Trigger the actual MenuItem after selection changes.
    QQuickItem* action = item("contextAction_compare");
    QVERIFY(action);
    QVERIFY(QMetaObject::invokeMethod(action, "click"));
    QTRY_VERIFY(item("compareSheet")->isVisible());
    QCOMPARE(item("compareSheet")->property("paths").value<QJSValue>().toVariant().toStringList(), captured);
    invoke("dismissTopLayer");
    QMetaObject::invokeMethod(menu, "close");
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, first));
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, video));
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu",
        Q_ARG(QVariant, m_library->rowOf(first)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(!hasCompare());
  }

  void contextMenuShortcutsUseCapturedTargets() {
    QStringList images;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      const QString path = pathAt(row);
      if (path.endsWith(".jpg") || path.endsWith(".png")) images.append(path);
    }
    QVERIFY(images.size() >= 3);
    const QString a = images[0], b = images[1], c = images[2];
    const QStringList paths{a, b, c};
    const auto cleanup = qScopeGuard([&] { m_settings->setFavorite(paths, false); m_settings->setHidden(paths, false); });
    m_settings->setFavorite(paths, false);
    m_settings->setHidden(paths, false);
    QQuickItem* grid = item("library");
    QObject* menu = m_window->findChild<QObject*>("libraryContextMenu");
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, a));
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, b));
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu",
        Q_ARG(QVariant, m_library->rowOf(c)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_V);
    QTRY_VERIFY(m_settings->isFavorite(c));
    QVERIFY(!m_settings->isFavorite(a) && !m_settings->isFavorite(b));
    QTRY_VERIFY(!menu->property("visible").toBool());
    m_settings->setFavorite(paths, false);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu",
        Q_ARG(QVariant, m_library->rowOf(a)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    QMetaObject::invokeMethod(grid, "clearChecked");
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, c));
    QTest::keyClick(m_window, Qt::Key_V);
    QTRY_VERIFY(m_settings->isFavorite(a) && m_settings->isFavorite(b));
    QVERIFY(!m_settings->isFavorite(c));
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu",
        Q_ARG(QVariant, m_library->rowOf(a)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_H, Qt::ControlModifier);
    QTRY_VERIFY(m_settings->isHidden(a));
    QVERIFY(!m_settings->isHidden(b) && !m_settings->isHidden(c));
  }

  void menusOfferOnlyUsefulExistingActions() {
    QStringList images;
    QString video, document;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      const QString path = pathAt(row);
      if (m_library->isVideoAt(row)) video = path;
      else if (m_library->isDocumentAt(row)) document = path;
      else if (path.endsWith(".jpg")) images.append(path);
    }
    QVERIFY(images.size() >= 3 && !video.isEmpty() && !document.isEmpty());
    const auto ids = [&](const QStringList& paths) {
      QVariant result;
      QMetaObject::invokeMethod(m_window, "selectionEntries", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, QVariant(paths)));
      QStringList out;
      for (const QVariant& row : result.toList()) out.append(row.toMap().value("id").toString());
      return out;
    };
    QVERIFY(ids({images[0]}).contains("preview"));
    QVERIFY(ids({images[0]}).contains("organize"));
    QVERIFY(ids({images[0]}).contains("omaframe"));
    QVERIFY(ids({document}).contains("copy"));
    QVERIFY(!ids({video}).contains("frame"));
    QVERIFY(ids({images[0], images[1]}).contains("print"));
    QVERIFY(ids({images[0], document}).contains("print"));
    QVERIFY(!ids({images[0], video}).contains("print"));
    QQuickItem* grid = item("library");
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, images[0]));
    QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, images[1]));
    openDetail(m_library->rowOf(images[2]));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QVariant result;
    QVERIFY(QMetaObject::invokeMethod(m_window, "previewSelection", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, images[2])));
    QVERIFY(result.toList().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(detail, "visibleActions", Q_RETURN_ARG(QVariant, result)));
    for (const QVariant& row : result.toList()) QVERIFY(row.toMap().value("id").toString() != "correctionsbatch");
    QVERIFY(QMetaObject::invokeMethod(detail, "actionTriggered", Q_ARG(QString, QStringLiteral("compare"))));
    QVERIFY(!item("compareSheet")->isVisible());
    // Grid bulk shortcuts still use the checked pair even if another tile is current.
    perform("compare", images[2]);
    QTRY_VERIFY(item("compareSheet")->isVisible());
    QCOMPARE(item("compareSheet")->property("paths").value<QJSValue>().toVariant().toStringList(), QStringList({images[0], images[1]}));
    invoke("dismissTopLayer");
    openDetail(m_library->rowOf(video));
    QVERIFY(QMetaObject::invokeMethod(detail, "visibleActions", Q_RETURN_ARG(QVariant, result)));
    bool hasFrame = false;
    for (const QVariant& row : result.toList()) hasFrame |= row.toMap().value("id").toString() == "frame";
    QVERIFY(hasFrame);
  }

  void selectionLabelsMatchTheCapturedTargetsAndCopyDependency() {
    const QStringList paths{pathAt(0), pathAt(1)};
    const auto cleanup = qScopeGuard([&] {
      m_settings->setFavorite(paths, false);
      m_settings->setHidden(paths, false);
    });
    const auto entries = [&] {
      QVariant result;
      QMetaObject::invokeMethod(m_window, "selectionEntries", Q_RETURN_ARG(QVariant, result),
                                Q_ARG(QVariant, QVariant(paths)));
      const QVariantList rows = result.toList();
      QVariantMap byId;
      for (const QVariant& row : rows) byId.insert(row.toMap().value("id").toString(), row);
      return byId;
    };
    m_settings->setFavorite(paths, false);
    m_settings->setHidden(paths, false);
    QCOMPARE(entries().value("favorite").toMap().value("label").toString(), QStringLiteral("Favourite"));
    QCOMPARE(entries().value("hide").toMap().value("label").toString(), QStringLiteral("Hide"));
    m_settings->setFavorite(paths, true);
    m_settings->setHidden(paths, true);
    QCOMPARE(entries().value("favorite").toMap().value("label").toString(), QStringLiteral("Unfavourite"));
    QCOMPARE(entries().value("hide").toMap().value("label").toString(), QStringLiteral("Unhide"));
    QCOMPARE(entries().value("copy").toMap().value("available").toBool(), m_registry->available(QStringLiteral("copy")));
    QCOMPARE(entries().value("copy").toMap().value("hint").toString(), QStringLiteral("wl-clipboard"));
  }

  void mixedSelectionsCannotStartImageCorrection() {
    QString image, video;
    for (int row = 0; row < m_library->rowCount(); ++row) {
      if (m_library->isVideoAt(row)) video = pathAt(row);
      else if (!m_library->isDocumentAt(row)) image = pathAt(row);
    }
    QVERIFY(!image.isEmpty() && !video.isEmpty());
    const QVariantList paths{image, video};
    QVERIFY(QMetaObject::invokeMethod(m_window, "performForTargets", Q_ARG(QVariant, QStringLiteral("correctionsbatch")),
        Q_ARG(QVariant, image), Q_ARG(QVariant, false), Q_ARG(QVariant, paths)));
    QVERIFY(!item("batchCorrectionSheet")->isVisible());
  }

  void spaceCanUseNativeViewingAndLegacyFramingRemainsAvailable() {
    const QString previous = m_settings->imagePrimaryAction();
    const auto cleanup = qScopeGuard([&] { m_settings->setImagePrimaryAction(previous); });
    m_settings->setImagePrimaryAction(QStringLiteral("preview"));
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(item("detail")->isVisible());
    QVERIFY(!item("matteSheet")->isVisible());
    QVERIFY(QMetaObject::invokeMethod(item("detail"), "adjustImageZoom", Q_ARG(QVariant, 2.0)));
    const double zoom = item("detail")->property("imageZoom").toDouble();
    QTest::keyClick(m_window, Qt::Key_Space);
    QCOMPARE(item("detail")->property("imageZoom").toDouble(), zoom);
    invoke("dismissTopLayer");
    m_settings->setImagePrimaryAction(QStringLiteral("matte"));
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(item("matteSheet")->isVisible());
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

  void browseKeepsSearchAndChoicesVisibleWithManySources() {
    // Keep fixture files until suite teardown: queued decodes from a removed
    // source may still finish after this test restores the library settings.
    const QDir sources(m_scratch.filePath(QStringLiteral("browse-sources")));
    QStringList paths;
    const QSize previous = m_window->size();
    const auto restore = qScopeGuard([&] {
      invoke("dismissTopLayer");
      for (const QString& path : paths) m_settings->removeLibraryFolder(path);
      rescan();
      // Removing folders schedules its own rescan; wait for that one too.
      const bool removed = QTest::qWaitFor([&] {
        return !m_captures->scanning() && std::all_of(paths.cbegin(), paths.cend(), [&](const QString& path) {
          return m_captures->rowOf(path + QStringLiteral("/picture.png")) < 0;
        });
      }, 60000);
      m_window->resize(previous);
      QVERIFY(removed);
    });
    for (int index = 0; index < 12; ++index) {
      const QString path = sources.filePath(QStringLiteral("source-folder-%1-photographs").arg(index));
      QVERIFY(QDir().mkpath(path));
      const QString picture = path + QStringLiteral("/picture.png");
      QImage fixture(40, 30, QImage::Format_RGB32);
      fixture.fill(Qt::blue);
      QVERIFY(fixture.save(picture));
      m_disposablePaths.append(picture);
      QVERIFY(m_settings->addLibraryFolder(QUrl::fromLocalFile(path)));
      paths.append(path);
    }
    // Each added folder asks for a scan; read the library once they are done.
    rescan();
    QTRY_VERIFY(m_library->folders().contains(paths.constLast()));
    m_window->resize(560, 420);
    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(QMetaObject::invokeMethod(browser, "open"));
    QTRY_VERIFY(browser->property("visible").toBool());
    QQuickItem* controls = item("libraryControls");
    QQuickItem* choices = item("libraryChoices");
    QQuickItem* search = item("librarySearch");
    QTRY_VERIFY(controls->property("contentHeight").toReal() > controls->height());
    QTRY_VERIFY(choices->height() >= 120);
    QVERIFY(search->isVisible());
    QTRY_VERIFY(search->hasActiveFocus());
    QTRY_VERIFY(search->mapToScene(QPointF()).y() >= 0);
    QTRY_VERIFY(search->mapToScene(QPointF()).y() + search->height() <= m_window->height());
    QQuickItem* addFolder = item("browseAddFolder");
    QVERIFY(addFolder->x() + addFolder->width() <= addFolder->parentItem()->width());
    QTest::keyClick(m_window, Qt::Key_Tab, Qt::ShiftModifier);
    QTRY_VERIFY(m_window->activeFocusItem() != search);
    QQuickItem* focused = m_window->activeFocusItem();
    QVERIFY(focused);
    QTRY_VERIFY(focused->mapToItem(controls, QPointF()).y() >= 0);
    QTRY_VERIFY(focused->mapToItem(controls, QPointF()).y() + focused->height()
                <= controls->height());
    search->forceActiveFocus();
    search->setProperty("text", paths.constLast());
    QTRY_COMPARE(choices->property("count").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!browser->property("visible").toBool());
    QCOMPARE(m_library->folderFilter(), paths.constLast());
    m_library->setFolderFilter({});
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
    QTRY_VERIFY(!m_window->findChild<QObject*>(QStringLiteral("gridRelayoutTimer"))->property("running").toBool());
    QTRY_VERIFY(item("library")->property("layoutReady").toBool());
  }

  void searchTypingIsCoalescedAndExplicitActionsApplyImmediately() {
    auto* search = item("libraryFilterSearch");
    QVERIFY(search);
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(search->hasActiveFocus());
    QSignalSpy changes(m_library, &CaptureFilterModel::searchTextChanged);
    QElapsedTimer timer; timer.start();
    typeText(QStringLiteral("alpine"));
    qInfo() << "PERF search burst ms" << timer.nsecsElapsed() / 1e6
            << "updates" << changes.size();
    QCOMPARE(changes.size(), 0);
    QTRY_COMPARE(m_library->searchText(), QStringLiteral("alpine"));
    QCOMPARE(changes.size(), 1);
    QCOMPARE(m_library->count(), 1);
    search->setProperty("text", QStringLiteral("pending saved search"));
    QCOMPARE(m_library->currentView().value(QStringLiteral("search")).toString(),
             QStringLiteral("pending saved search"));
    search->setProperty("text", QStringLiteral("getter search"));
    QCOMPARE(m_library->searchText(), QStringLiteral("getter search"));
    search->setProperty("text", QStringLiteral("no-match"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QCOMPARE(m_library->searchText(), QStringLiteral("no-match"));
    QCOMPARE(m_library->count(), 0);
    search->setProperty("text", QString());
    QCOMPARE(m_library->searchText(), QString());
    search->setProperty("text", QStringLiteral("pending"));
    m_library->setSearchText(QStringLiteral("alpine"));
    QCOMPARE(search->property("text").toString(), QStringLiteral("alpine"));
    QTest::qWait(200);
    QCOMPARE(m_library->searchText(), QStringLiteral("alpine"));
    m_library->setSearchText(QString());
    search->setProperty("text", QStringLiteral("pending"));
    m_library->setSearchText(QString());
    QCOMPARE(search->property("text").toString(), QString());
    QTest::qWait(200);
    QCOMPARE(m_library->searchText(), QString());
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

  // Text read after a search starts brings its rows back on recycled
  // delegates. Each shows its thumbnail and the matched text, and the grid
  // shows every thumbnail again once the search is cleared. Which order a
  // recycled delegate receives its roles in is Qt's choice, so the card test
  // above pins the order that used to leave tiles blank.
  void textMatchesFoundMidSearchKeepTheirThumbnails() {
    QQuickItem* grid = item("library");
    const auto viewportReady = [grid] {
      QVariant ready;
      QMetaObject::invokeMethod(grid, "viewportReady", Q_RETURN_ARG(QVariant, ready));
      return ready.toBool();
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewportReady(), 15000);
    const int all = m_library->rowCount();
    const QString matched = pathAt(1);
    const QString other = pathAt(2);
    // Pictures take text from the picture index and PDFs from their own;
    // either may sit in these rows.
    const auto setText = [this](const QString& path, const QString& text) {
      m_library->setOcrText(path, text);
      m_library->setDocumentText(path, text);
    };
    const auto restore = qScopeGuard([&] {
      setText(matched, QString());
      setText(other, QString());
      m_library->setSearchText(QString());
    });
    for (int round = 0; round < 3; ++round) {
      m_library->setSearchText(QStringLiteral("zqxinvoice"));
      QTRY_COMPARE(m_library->rowCount(), 0);
      setText(matched, QStringLiteral("ACME HARDWARE ZQXINVOICE 7781"));
      QTRY_COMPARE(m_library->rowCount(), 1);
      setText(other, QStringLiteral("zqxinvoice paid"));
      QTRY_COMPARE(m_library->rowCount(), 2);
      QTRY_VERIFY(viewportReady());
      for (const QString& path : {matched, other}) {
        QQuickItem* card = cardFor(path);
        QVERIFY(card);
        QVERIFY(card->property("thumbnailReady").toBool());
        auto* snippet = find(card, [](QQuickItem* candidate) {
          return candidate->objectName() == QStringLiteral("captureSnippetLabel");
        });
        QVERIFY(snippet);
        QVERIFY(snippet->isVisible());
        QVERIFY(snippet->property("text").toString().contains(QStringLiteral("ZQXINVOICE"),
                                                                 Qt::CaseInsensitive));
      }
      m_library->setSearchText(QString());
      QTRY_COMPARE(m_library->rowCount(), all);
      QTRY_VERIFY(viewportReady());
      setText(matched, QString());
      setText(other, QString());
    }
  }

  // Up and Down walk the suggestions and wrap, Enter or Tab takes one, Esc
  // closes them before it clears the field, and Enter with nothing
  // highlighted searches for what was typed.
  void searchSuggestionsFollowTheKeyboard() {
    auto* search = item("libraryFilterSearch");
    auto* popup = m_window->findChild<QObject*>(QStringLiteral("searchSuggestions"));
    QVERIFY(popup);
    const auto restore = qScopeGuard([&] {
      search->setProperty("text", QString());
      m_library->setSearchText(QString());
    });
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(search->hasActiveFocus());
    typeText(QStringLiteral("type:"));
    QTRY_VERIFY(popup->property("visible").toBool());
    const QVariantList rows = popup->property("rows").toList();
    QCOMPARE(rows.size(), SearchQuery::typeNames().size());
    QCOMPARE(popup->property("highlighted").toInt(), -1);
    QTest::keyClick(m_window, Qt::Key_Down);
    QCOMPARE(popup->property("highlighted").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_Down);
    QCOMPARE(popup->property("highlighted").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Up);
    QTest::keyClick(m_window, Qt::Key_Up);
    QCOMPARE(popup->property("highlighted").toInt(), rows.size() - 1);
    // Enter takes the highlighted value and searches at once; the field
    // keeps the keyboard for the next word.
    QTest::keyClick(m_window, Qt::Key_Return);
    const QString last = rows.last().toMap().value(QStringLiteral("insert")).toString();
    QCOMPARE(search->property("text").toString(), last + QLatin1Char(' '));
    QCOMPARE(m_library->searchText(), last + QLatin1Char(' '));
    QVERIFY(search->hasActiveFocus());
    QTRY_VERIFY(!popup->property("visible").toBool());

    // Tab completes the first suggestion, here a field name.
    typeText(QStringLiteral("cam"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Tab);
    QCOMPARE(search->property("text").toString(), last + QStringLiteral(" camera:"));
    QVERIFY(search->hasActiveFocus());

    // Esc closes the list first and leaves the text; a second Esc clears it.
    search->setProperty("text", QString());
    typeText(QStringLiteral("type:"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCOMPARE(search->property("text").toString(), QStringLiteral("type:"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QCOMPARE(search->property("text").toString(), QString());

    // With nothing highlighted, Enter searches for the text as typed.
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(search->hasActiveFocus());
    typeText(QStringLiteral("type:v"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Return);
    QCOMPARE(m_library->searchText(), QStringLiteral("type:v"));
    QTRY_VERIFY(!popup->property("visible").toBool());
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  // Each chip clears its own facet; Clear all clears every one in a single
  // filter pass, and a saved view's chip clears its search too.
  void filterChipsClearOneFacetOrAllAtOnce() {
    const auto restore = qScopeGuard([&] {
      m_library->clearFacets(true);
    });
    const QString folder = QFileInfo(pathAt(0)).absolutePath();
    m_library->setFolderFilter(folder);
    m_library->setDateRange(QStringLiteral("2000-01-01"), QStringLiteral("2099-12-31"));
    m_library->setMinimumRating(0);
    QVERIFY(m_library->rowCount() > 0);
    const auto shown = [this](const QString& name) {
      return find(m_window->contentItem(), [&name](QQuickItem* candidate) {
               return candidate->objectName() == name && candidate->isVisible();
             }) != nullptr;
    };
    auto* chips = item("activeFilterChips");
    QTRY_VERIFY(chips->isVisible());
    QTRY_VERIFY(shown(QStringLiteral("filterChip-folder")));
    QTRY_VERIFY(shown(QStringLiteral("filterChip-date")));
    click(item("filterChipClear-date"));
    QTRY_VERIFY(m_library->dateFrom().isEmpty());
    QCOMPARE(m_library->folderFilter(), QDir::cleanPath(folder));

    m_library->setDateRange(QStringLiteral("2000-01-01"), QStringLiteral("2099-12-31"));
    m_library->setTagFilter(QStringLiteral("ui-chip-tag"), {pathAt(0)});
    QTRY_VERIFY(item("filterChipsClearAll")->isVisible());
    // Each filter pass ends by refreshing the pairing roles of every row.
    QSignalSpy changes(m_library, &QAbstractItemModel::dataChanged);
    const auto passes = [&] {
      int count = 0;
      for (const QList<QVariant>& arguments : std::as_const(changes)) {
        count += arguments.at(2).value<QList<int>>().contains(CaptureModel::CompanionPathRole) ? 1 : 0;
      }
      return count;
    };
    QQuickItem* clearAll = item("filterChipsClearAll");
    // The pointer travels there, leaving the hovered chip and its tooltip.
    QTest::mouseMove(m_window, centre(clearAll));
    QTest::qWait(50);
    click(clearAll);
    QTRY_VERIFY(m_library->folderFilter().isEmpty());
    QVERIFY(m_library->dateFrom().isEmpty());
    QVERIFY(m_library->tagFilter().isEmpty());
    QCOMPARE(passes(), 1);
    QTRY_VERIFY(!chips->isVisible());

    m_library->applyView(QStringLiteral("UI chips"),
                         {{QStringLiteral("query"), QStringLiteral("alpine")},
                          {QStringLiteral("folder"), folder}});
    QTRY_VERIFY(shown(QStringLiteral("filterChipClear-view")));
    click(item("filterChipClear-view"));
    QTRY_VERIFY(m_library->smartCollectionFilter().isEmpty());
    QVERIFY(m_library->searchText().isEmpty());
    QVERIFY(m_library->folderFilter().isEmpty());
  }

  // Clear all is reached with Tab after the chips and taken with Space or
  // Return, with a visible focus ring.
  void clearAllFiltersFromTheKeyboard() {
    const auto restore = qScopeGuard([&] { m_library->clearFacets(true); });
    const QString folder = QFileInfo(pathAt(0)).absolutePath();
    const auto narrow = [&] {
      m_library->setFolderFilter(folder);
      m_library->setDateRange(QStringLiteral("2000-01-01"), QStringLiteral("2099-12-31"));
    };
    narrow();
    QQuickItem* clearAll = item("filterChipsClearAll");
    QTRY_VERIFY(clearAll->isVisible());
    QVERIFY(clearAll->activeFocusOnTab());
    QQuickItem* lastChip = item("filterChipClear-date");
    lastChip->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(lastChip->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(clearAll->hasActiveFocus());
    QVERIFY(item("filterChipsClearAllFocus")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(m_library->folderFilter().isEmpty());
    QVERIFY(m_library->dateFrom().isEmpty());

    narrow();
    QTRY_VERIFY(clearAll->isVisible());
    clearAll->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(clearAll->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(m_library->folderFilter().isEmpty());
    QVERIFY(m_library->dateFrom().isEmpty());
  }

  // A suggestion over a tile takes the click alone: the tile is not
  // selected and the search field keeps the keyboard.
  void suggestionClicksDoNotReachTiles() {
    auto* search = item("libraryFilterSearch");
    auto* popup = m_window->findChild<QObject*>(QStringLiteral("searchSuggestions"));
    QVERIFY(popup);
    QQuickItem* grid = item("library");
    const auto restore = qScopeGuard([&] {
      search->setProperty("text", QString());
      m_library->setSearchText(QString());
      QMetaObject::invokeMethod(grid, "scrollTo", Q_ARG(QVariant, 0.0));
    });
    // Date sorts open with a day header right under the suggestions; scroll
    // it away so tiles lie there, as they do in the flat grid.
    QMetaObject::invokeMethod(grid, "scrollTo", Q_ARG(QVariant, 40.0));
    QTest::qWait(50);
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(search->hasActiveFocus());
    typeText(QStringLiteral("type:"));
    QTRY_VERIFY(popup->property("visible").toBool());
    // A point on a suggestion row with a tile right under it. Date sorts
    // show headers and short rows, so the middle of the lowest suggestion can
    // sit over a header or the empty end of a row; look along each row,
    // lowest first, for a spot over a tile the view shows.
    QList<QQuickItem*> suggestionRows;
    find(m_window->contentItem(), [&suggestionRows](QQuickItem* candidate) {
      if (candidate->objectName() == QStringLiteral("searchSuggestionRow") && candidate->isVisible())
        suggestionRows.append(candidate);
      return false;
    });
    QVERIFY(!suggestionRows.isEmpty());
    // The search sits in the header, so a short list ends above the grid.
    // Lower the list over the tiles: the point is that a click on it stays
    // on it, whatever lies underneath.
    {
      const qreal top = suggestionRows.first()->mapToScene(QPointF()).y();
      const qreal target = grid->mapToScene(QPointF(0, 60)).y();
      if (top < target) popup->setProperty("y", popup->property("y").toReal() + target - top);
      QTRY_VERIFY(suggestionRows.first()->mapToScene(QPointF()).y() >= target - 1);
    }
    std::sort(suggestionRows.begin(), suggestionRows.end(), [](QQuickItem* a, QQuickItem* b) {
      return a->mapToScene(QPointF()).y() > b->mapToScene(QPointF()).y();
    });
    QTRY_VERIFY(suggestionRows.first()->width() > 0);
    QQuickItem* lowest = nullptr;
    QQuickItem* tile = nullptr;
    QPoint at;
    for (QQuickItem* row : std::as_const(suggestionRows)) {
      for (qreal x = 8; x < row->width() - 8 && !tile; x += 8) {
        const QPoint point = row->mapToScene(QPointF(x, row->height() / 2)).toPoint();
        tile = find(grid, [&point](QQuickItem* candidate) {
          return candidate->isVisible() && candidate->property("selectionMode").isValid() &&
                 !candidate->property("path").toString().isEmpty() &&
                 candidate->contains(candidate->mapFromScene(QPointF(point))) &&
                 shownByItsView(candidate);
        });
        if (tile) at = point;
      }
      if (tile) {
        lowest = row;
        break;
      }
    }
    QVERIFY2(tile, "no tile under any suggestion");
    const QString tilePath = tile->property("path").toString();
    QVERIFY(grid->property("selectedPath").toString() != tilePath);
    const QVariantList rows = popup->property("rows").toList();
    const int index = lowest->parentItem()->property("index").toInt();
    const QString insert = rows.at(index).toMap().value(QStringLiteral("insert")).toString();

    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at);
    QTRY_COMPARE(search->property("text").toString(), insert + QLatin1Char(' '));
    // Past the double-click interval, where a tile would have been chosen.
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
    QVERIFY(search->hasActiveFocus());
    QVERIFY(grid->property("selectedPath").toString() != tilePath);
    // Nor did it reach a tile's checkbox or a day's Select control.
    QCOMPARE(grid->property("checkedCount").toInt(), 0);
  }

  void emptySearchResultsRecoverKeyboardNavigation() {
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(item("filters")->property("searchActive").toBool());
    typeText(QStringLiteral("no-media-matches-this-query"));
    QTRY_COMPARE(m_library->count(), 0);
    QTRY_COMPARE(item("library")->property("currentIndex").toInt(), -1);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(m_library->count() > 0);
    QTRY_VERIFY(item("library")->property("currentIndex").toInt() >= 0);
    QTRY_VERIFY(item("library")->hasActiveFocus());
    const QString expected = pathAt(item("library")->property("currentIndex").toInt());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(item("detail")->isVisible());
    QCOMPARE(item("detail")->property("path").toString(), expected);
  }

  // A search or filter that matches nothing says so and offers the way
  // back, rather than calling a populated folder empty.
  void libraryLoadingStateWaitsForTheScanInsteadOfTheEmptyState() {
    // The main window's library is loaded, so its loading state is hidden and
    // the empty state is the one that answers for an empty view.
    QQuickItem* mainLoading = item(QStringLiteral("libraryLoadingState"));
    QVERIFY(mainLoading);
    QVERIFY(!mainLoading->isVisible());
    QVERIFY(!mainLoading->property("waiting").toBool());

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
    engine.rootContext()->setContextProperty(QStringLiteral("Theme"), m_theme);
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, &engine, [&warnings](const QList<QQmlError>& errors) {
      for (const auto& error : errors) warnings.append(error.toString());
    });
    // No second window: it would take keyboard focus from the main one.
    engine.loadData(R"QML(
      import QtQuick
      import Omaroll
      Item {
        width: 320; height: 200
        LoadingState { objectName: "loading"; anchors.centerIn: parent; width: 280 }
      }
    )QML");
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join('\n')));
    auto* root = qobject_cast<QQuickItem*>(engine.rootObjects().constFirst());
    QVERIFY(root);
    auto* loading = root->findChild<QQuickItem*>(QStringLiteral("loading"));
    QVERIFY(loading);
    QVERIFY(!loading->isVisible());

    // A scan that answers quickly never shows it.
    loading->setProperty("waiting", true);
    QTest::qWait(60);
    QVERIFY(!loading->isVisible());
    loading->setProperty("waiting", false);
    QTest::qWait(300);
    QVERIFY(!loading->isVisible());

    // A slow one does, until it answers, and starts over the next time.
    loading->setProperty("waiting", true);
    QTRY_VERIFY_WITH_TIMEOUT(loading->isVisible(), 2000);
    QVERIFY(loading->property("text").toString().startsWith(QStringLiteral("Looking for")));
    loading->setProperty("waiting", false);
    QVERIFY(!loading->isVisible());
    loading->setProperty("waiting", true);
    QVERIFY(!loading->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(loading->isVisible(), 2000);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }

  void emptyStatesSayWhyNothingIsShownAndOfferTheWayOut() {
    const auto named = [this](const QString& name) {
      return find(m_window->contentItem(), [&name](QQuickItem* candidate) {
        return candidate->objectName() == name && candidate->isVisible();
      });
    };
    const auto title = [this] { return item(QStringLiteral("libraryEmptyTitle"))->property("text").toString(); };
    // A filter change arrives in several notifications, and each rebuilds the
    // buttons; click one that is still there once they stop.
    const auto clickAction = [this, &named](const QString& name) {
      for (int attempt = 0; attempt < 40; ++attempt) {
        QPointer<QQuickItem> target = named(name);
        QTest::qWait(120);
        if (target && target->isVisible() && centre(target).x() > 0
            && centre(target).x() < m_window->width()) {
          click(target);
          return true;
        }
      }
      return false;
    };
    const QString folder = QFileInfo(pathAt(0)).absolutePath();
    QVERIFY(m_library->folderItemCount(folder) > 0);
    QCOMPARE(m_library->emptyReason(), QString());

    // The baseline repro: a populated folder and a search for nothing.
    m_library->setFolderFilter(folder);
    m_library->setSearchText(QStringLiteral("zzz_no_matching_photo_zz"));
    QTRY_COMPARE(m_library->count(), 0);
    QTRY_VERIFY(item(QStringLiteral("libraryEmptyState"))->isVisible());
    QCOMPARE(m_library->emptyReason(), QStringLiteral("search"));
    QTRY_COMPARE(title(), QStringLiteral("Nothing matches \u201czzz_no_matching_photo_zz\u201d in this folder"));
    QVERIFY(!title().contains(QStringLiteral("No media")));
    // Behind a sheet the actions are off, so a sheet button over one cannot
    // press it too (Restore last good backup sat over Set as default viewer).
    QVERIFY(QMetaObject::invokeMethod(item("settingsSheet"), "open"));
    QTRY_VERIFY(named(QStringLiteral("emptyAction_clear-search")) &&
                !named(QStringLiteral("emptyAction_clear-search"))->isEnabled());
    invoke("dismissTopLayer");
    QTRY_VERIFY(named(QStringLiteral("emptyAction_clear-search"))->isEnabled());
    QVERIFY(clickAction(QStringLiteral("emptyAction_clear-search")));
    QTRY_VERIFY(m_library->searchText().isEmpty());
    QTRY_VERIFY(m_library->count() > 0);
    QCOMPARE(m_library->folderFilter(), folder);
    m_library->setFolderFilter(QString());

    // Filters, and a search on top of them, clear together.
    m_library->setCameraFilter(QStringLiteral("No Such Camera"));
    m_library->setSearchText(QStringLiteral("alpine"));
    QTRY_COMPARE(m_library->count(), 0);
    QCOMPARE(m_library->emptyReason(), QStringLiteral("filters"));
    QTRY_COMPARE(title(), QStringLiteral("Nothing matches these filters"));
    const QString detail = item(QStringLiteral("libraryEmptyDetail"))->property("text").toString();
    QVERIFY2(detail.contains(QStringLiteral("taken with No Such Camera")), qPrintable(detail));
    QVERIFY2(detail.contains(QStringLiteral("alpine")), qPrintable(detail));
    QVERIFY(clickAction(QStringLiteral("emptyAction_clear-filters")));
    QTRY_VERIFY(m_library->count() > 0);
    QVERIFY(m_library->cameraFilter().isEmpty());
    QVERIFY(m_library->searchText().isEmpty());

    // A saved view's filters are the view: Clear filters leaves it, keeping
    // its folder, and the words say so.
    const auto leaveView = qScopeGuard([this] {
      m_library->clearRefinements();
      m_library->clearSmartCollection();
      m_library->setFolderFilter(QString());
    });
    QVariantMap view = m_library->currentView();
    view.insert(QStringLiteral("folder"), folder);
    view.insert(QStringLiteral("camera"), QStringLiteral("No Such Camera"));
    m_library->applyView(QStringLiteral("Old camera"), view, {});
    QTRY_COMPARE(m_library->count(), 0);
    QCOMPARE(m_library->emptyReason(), QStringLiteral("filters"));
    QTRY_COMPARE(title(), QStringLiteral("Nothing matches this saved view's filters in this folder"));
    QVERIFY(clickAction(QStringLiteral("emptyAction_clear-filters")));
    QTRY_VERIFY(m_library->count() > 0);
    QVERIFY(m_library->cameraFilter().isEmpty());
    QVERIFY(m_library->smartCollectionFilter().isEmpty());
    QCOMPARE(m_library->folderFilter(), folder);

    // An empty album is empty, whatever the search.
    m_library->setAlbumFilter(QStringLiteral("Nothing yet"), {});
    QTRY_COMPARE(m_library->count(), 0);
    QCOMPARE(m_library->emptyReason(), QStringLiteral("scope"));
    QTRY_COMPARE(title(), QStringLiteral("This album is empty"));
    QVERIFY(clickAction(QStringLiteral("emptyAction_show-all")));
    QTRY_VERIFY(m_library->count() > 0);
    QVERIFY(m_library->albumFilter().isEmpty());
    QTRY_VERIFY(!item(QStringLiteral("libraryEmptyState"))->isVisible());

    // Nothing found at all: the first run.
    CaptureFilterModel nothing;
    QCOMPARE(nothing.emptyReason(), QStringLiteral("library"));
    m_mimeDefaults->refresh();
    QTRY_VERIFY(m_mimeDefaults->known());
    QVERIFY(!m_mimeDefaults->imagesDefault());
    QVariant state;
    QVERIFY(QMetaObject::invokeMethod(m_window, "emptyState", Q_RETURN_ARG(QVariant, state),
                                      Q_ARG(QVariant, QStringLiteral("library"))));
    if (state.metaType() == QMetaType::fromType<QJSValue>()) state = state.value<QJSValue>().toVariant();
    const QVariantMap first = state.toMap();
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("No media yet"));
    QVERIFY(first.value(QStringLiteral("detail")).toString().contains(QStringLiteral("stay where they are")));
    const QStringList actions = first.value(QStringLiteral("actions")).toStringList();
    QVERIFY2(actions.contains(QStringLiteral("add-folder")), qPrintable(actions.join(u',')));
    QVERIFY(actions.contains(QStringLiteral("open-file")));
    QVERIFY(actions.contains(QStringLiteral("set-default")));
    // A demo window does not offer the demo.
    QVERIFY(!actions.contains(QStringLiteral("demo")));
  }

  void externalOpenKeepsTheSearchFieldInSync() {
    QQuickItem* search = item("libraryFilterSearch");
    QVERIFY(search);
    m_library->setSearchText(QStringLiteral("alpine"));
    QTRY_COMPARE(search->property("text").toString(), QStringLiteral("alpine"));
    QTRY_COMPARE(m_library->count(), 1);
    const QString path = pathAt(0);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPath", Q_ARG(QVariant, path)));
    QTRY_VERIFY(item("detail")->isVisible());
    QCOMPARE(m_library->searchText(), QString());
    QCOMPARE(search->property("text").toString(), QString());
    invoke("dismissTopLayer");
    QVERIFY(QMetaObject::invokeMethod(m_window, "openFolder", Q_ARG(QVariant, QString())));
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

  void tabCyclesTheSectionsAndWraps() {
    QTest::keyClick(m_window, Qt::Key_1);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), -1);
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Tab, Qt::ShiftModifier);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), -1);
    // Backwards from All wraps to the last section, PDFs.
    QTest::keyClick(m_window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 5);
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), -1);

    // Downloads joins the cycle only while it is scanned.
    const bool downloads = m_settings->scanDownloads();
    m_settings->setScanDownloads(true);
    QTest::keyClick(m_window, Qt::Key_5);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 3);
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 4);
    m_settings->setScanDownloads(false);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), -1);
    QTest::keyClick(m_window, Qt::Key_5);
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_COMPARE(m_library->property("kindFilter").toInt(), 5);
    m_settings->setScanDownloads(downloads);

    // The search field keeps Tab for itself.
    QTest::keyClick(m_window, Qt::Key_1);
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTRY_VERIFY(item("filters")->property("searchActive").toBool());
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTest::qWait(50);
    QCOMPARE(m_library->property("kindFilter").toInt(), -1);
    // Tab has traversed out of the field, so hand the grid its focus back
    // rather than sending an Escape that would now close the window.
    item("library")->forceActiveFocus();
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  void altDigitsRateTheHighlightedTileAndTheViewer() {
    item("library")->setProperty("currentIndex", 0);
    item("library")->forceActiveFocus();
    const QString first = pathAt(0);
    QTest::keyClick(m_window, Qt::Key_3, Qt::AltModifier);
    QTRY_COMPARE(m_settings->rating(first), 3);
    QTRY_COMPARE(m_library->data(m_library->index(0, 0), CaptureRoles::RatingRole).toInt(), 3);

    // The viewer shows the same stars and Alt+0 clears them there.
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QTRY_COMPARE(detail->property("rating").toInt(), 3);
    QTest::keyClick(m_window, Qt::Key_5, Qt::AltModifier);
    QTRY_COMPARE(m_settings->rating(first), 5);
    QTRY_COMPARE(detail->property("rating").toInt(), 5);
    QTest::keyClick(m_window, Qt::Key_0, Qt::AltModifier);
    QTRY_COMPARE(m_settings->rating(first), 0);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!detail->isVisible());
  }

  void viewerCaptionSavesOnEnterAndShowsOnTheTile() {
    QQuickItem* detail = item("detail");
    const QString first = pathAt(0);
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    // The caption editor lives in the on-demand inspector.
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());
    QQuickItem* field = detail->findChild<QQuickItem*>(QStringLiteral("viewerCaption"));
    QVERIFY(field);
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QTRY_VERIFY(field->hasActiveFocus());
    typeText(QStringLiteral("Trip notes"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(m_settings->caption(first), QStringLiteral("Trip notes"));
    QTRY_COMPARE(detail->property("caption").toString(), QStringLiteral("Trip notes"));
    QVERIFY(!field->hasActiveFocus());

    // Arrow to the next file and caption that one too. Enter must save on
    // its own, whatever focus the sheet held before the field was clicked.
    QTest::keyClick(m_window, Qt::Key_Right);
    const QString second = detail->property("path").toString();
    QTRY_VERIFY(second != first);
    QTRY_VERIFY(detail->property("caption").toString().isEmpty());
    QVERIFY(field->property("text").toString().isEmpty());
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QTRY_VERIFY(field->hasActiveFocus());
    typeText(QStringLiteral("Second one"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(m_settings->caption(second), QStringLiteral("Second one"));
    QVERIFY(!field->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(detail->property("path").toString(), first);
    QTRY_COMPARE(field->property("text").toString(), QStringLiteral("Trip notes"));
    m_settings->setCaption(second, QString());
    // Enter handed focus back to the picture, so the viewer's own keys work.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!detail->isVisible());
    QTRY_COMPARE(m_library->data(m_library->index(0, 0), CaptureRoles::CaptionRole).toString(),
                 QStringLiteral("Trip notes"));
    // The grid may still be rebuilding delegates from an earlier filter
    // change, so give the tile a moment to pick the caption up.
    QTRY_VERIFY(cardFor(first) != nullptr);
    QTRY_COMPARE(cardFor(first)->property("caption").toString(), QStringLiteral("Trip notes"));
    m_settings->setCaption(first, QString());
    QTRY_VERIFY(m_settings->caption(first).isEmpty());
  }

  // The preview opens clean: media and the header, with the inspector and the
  // action menu hidden until they are asked for.
  void viewerOpensWithTheInspectorAndActionMenuClosed() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QVERIFY(!detail->property("showInfo").toBool());
    QVERIFY(!detail->property("actionNavigationActive").toBool());
    QTRY_VERIFY(item("viewerInspector")->property("width").toReal() == 0);
    QVERIFY(!item("viewerActionPopup")->isVisible());
    QVERIFY(!item("viewerPrimaryActions")->isVisible());
    // The header carries the file name and the way in to everything else.
    QVERIFY(item("viewerHeader")->isVisible());
    QVERIFY(item("viewerInfoButton")->isVisible());
    QVERIFY(item("viewerOverflowButton")->isVisible());
    QCOMPARE(item("viewerHeaderName")->property("text").toString(),
             detail->property("fileName").toString());

    // The info toggle brings the inspector in and out without closing preview.
    click(item("viewerInfoButton"));
    QTRY_VERIFY(detail->property("showInfo").toBool());
    QTRY_VERIFY(item("viewerInspector")->width() > 0);
    click(item("viewerInfoButton"));
    QTRY_VERIFY(!detail->property("showInfo").toBool());
    QVERIFY(detail->isVisible());
  }

  // Tab opens the otherwise hidden action menu; arrows move through available
  // rows, Escape closes it and hands focus back without closing the preview.
  void hiddenActionMenuNavigatesAndEscapeReturnsToPreview() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QVERIFY(!item("viewerActionPopup")->isVisible());

    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(detail->property("actionNavigationActive").toBool());
    QTRY_VERIFY(item("viewerActionPopup")->isVisible());
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));

    QTest::keyClick(m_window, Qt::Key_Down);
    QTRY_VERIFY(activeViewerAction() && activeViewerAction()->objectName() != QStringLiteral("viewerAction_copy"));
    QTest::keyClick(m_window, Qt::Key_Up);
    QTRY_COMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!detail->property("actionNavigationActive").toBool());
    QVERIFY(detail->isVisible());
    QTRY_VERIFY(detail->hasActiveFocus());
    QVERIFY(!item("viewerActionPopup")->isVisible());

    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(item("viewerActionPopup")->isVisible());
    const bool favourite = detail->property("favorite").toBool();
    click(item("viewerFavouriteButton"));
    QTRY_VERIFY(!item("viewerActionPopup")->isVisible());
    QCOMPARE(detail->property("favorite").toBool(), favourite);
    QVERIFY(detail->isVisible());

    QVERIFY(QMetaObject::invokeMethod(detail, "focusActionById",
                                    Q_ARG(QVariant, QStringLiteral("favorite"))));
    QTRY_VERIFY(activeViewerAction() && activeViewerAction()->objectName() ==
                                       QStringLiteral("viewerAction_favorite"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!item("viewerActionPopup")->isVisible());
    QTRY_COMPARE(detail->property("favorite").toBool(), !favourite);
    QVERIFY(detail->isVisible());
    m_settings->setFavorite({pathAt(0)}, favourite);
  }

  // Escape while editing a caption reverts the text and keeps the preview open.
  void viewerCaptionEscapeCancelsTheEdit() {
    QQuickItem* detail = item("detail");
    const QString first = pathAt(0);
    m_settings->setCaption(first, QString());
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());
    QQuickItem* field = detail->findChild<QQuickItem*>(QStringLiteral("viewerCaption"));
    QVERIFY(field);
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QTRY_VERIFY(field->hasActiveFocus());
    typeText(QStringLiteral("Do not keep"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!field->hasActiveFocus());
    QVERIFY(detail->isVisible());
    QVERIFY(m_settings->caption(first).isEmpty());
    QCOMPARE(field->property("text").toString(), QString());
  }

  void viewerCaptionSavesBeforeFileSwitchAndInspectorClose() {
    QQuickItem* detail = item("detail");
    const QString first = pathAt(0);
    const QString second = pathAt(1);
    const QString firstCaption = m_settings->caption(first);
    const QString secondCaption = m_settings->caption(second);
    openDetail(0);
    detail->setProperty("showInfo", true);
    QQuickItem* field = item("viewerCaption");
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QTRY_VERIFY(field->hasActiveFocus());
    field->setProperty("text", QStringLiteral("First file notes"));
    openDetail(1);
    QTRY_COMPARE(m_settings->caption(first), QStringLiteral("First file notes"));
    QCOMPARE(m_settings->caption(second), secondCaption);
    QTRY_COMPARE(field->property("text").toString(), secondCaption);
    QTest::keyClick(m_window, Qt::Key_C, Qt::AltModifier);
    QTRY_VERIFY(field->hasActiveFocus());
    field->setProperty("text", QStringLiteral("Second file notes"));
    click(item("viewerInfoButton"));
    QTRY_VERIFY(!detail->property("showInfo").toBool());
    QTRY_COMPARE(m_settings->caption(second), QStringLiteral("Second file notes"));
    QVERIFY(detail->isVisible());
    QTRY_VERIFY(detail->hasActiveFocus());
    m_settings->setCaption(first, firstCaption);
    m_settings->setCaption(second, secondCaption);
  }

  // Additional file metadata stays hidden until the disclosure is expanded.
  void viewerTechnicalDetailsExpandOnDemand() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(m_mediaInfo->path() == pathAt(0) && !m_mediaInfo->loading(), 5000);
    QVERIFY(!detail->property("technicalExpanded").toBool());
    QVERIFY(!item("viewerTechnicalDetails")->isVisible());
    click(item("viewerTechnicalToggle"));
    QTRY_VERIFY(detail->property("technicalExpanded").toBool());
    QVERIFY(item("viewerTechnicalDetails")->isVisible());
    QQuickItem* line = find(item("viewerTechnicalDetails"), [](QQuickItem* candidate) {
      return candidate->property("text").toString().startsWith(QStringLiteral("JPEG"));
    });
    QVERIFY(line);
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
    clickPlaced(keep);
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
    QTRY_VERIFY(!m_window->findChild<QObject*>(QStringLiteral("gridRelayoutTimer"))->property("running").toBool());
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
    rescan();
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
    rescan();
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
    QCOMPARE(item("libraryFilterSearch")->property("text").toString(), QStringLiteral("alpine"));
    QCOMPARE(m_library->sortMode(), CaptureFilterModel::NameAscending);
    QCOMPARE(m_library->count(), 1);

    m_settings->deleteSmartCollection(QStringLiteral("Alpine pictures"));
    m_library->setSearchText({});
    m_library->setSortMode(CaptureFilterModel::NewestFirst);
    m_library->clearSmartCollection();
  }

  void selectionKeysActOnTheWholeSelection() {
    QQuickItem* grid = item("library");
    // Start from the first tile, whatever the previous test left current.
    // The saved path too: a layout restore still pending from the previous
    // test's sort change puts the current tile back from it.
    QTRY_VERIFY(!grid->property("restoringLayout").toBool());
    grid->setProperty("selectedPath", pathAt(0));
    grid->setProperty("currentIndex", 0);
    QTest::qWait(200);
    QTRY_COMPARE(grid->property("currentIndex").toInt(), 0);
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
    QCOMPARE(focused->objectName(), QStringLiteral("viewerAction_copy"));
    QVERIFY(focused->property("usable").toBool());
    QCOMPARE(focused->property("shortcut").toString(),
             m_registry->shortcutFor(QStringLiteral("copy")));
    QCOMPARE(focused->property("toolTipText").toString(), QStringLiteral("Copy image  ·  Y"));

    QTest::keyClick(m_window, Qt::Key_Down);
    QQuickItem* next = activeViewerAction();
    QTRY_VERIFY(next != nullptr);
    QVERIFY(next != focused);
    QVERIFY(next->property("usable").toBool());
    QTest::keyClick(m_window, Qt::Key_Up);
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));

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
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));
    QMetaObject::invokeMethod(detail, "focusPreview");
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(activeViewerAction() != nullptr);
    QCOMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_copy"));
    QVERIFY(QMetaObject::invokeMethod(detail, "focusActionById", Q_ARG(QVariant, QStringLiteral("matte"))));
    QTRY_COMPARE(activeViewerAction()->objectName(), QStringLiteral("viewerAction_matte"));
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
    QQuickItem* rotate = pill(item("detail"), QStringLiteral("Rotate right"));
    QTRY_VERIFY(rotate != nullptr);
    QCOMPARE(rotate->property("shortcut").toString(), QStringLiteral("R"));
    QCOMPARE(rotate->property("resolvedToolTip").toString(), QStringLiteral("Rotate right  ·  R"));
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
    const QString negativePath = pathAt(0);
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
    const auto removeFixture = qScopeGuard([&] {
      if (QObject* popup = m_window->findChild<QObject*>("libraryContextMenu"))
        QMetaObject::invokeMethod(popup, "close");
      QMetaObject::invokeMethod(detail, "close");
      QFile::remove(qrPath);
      rescan();
    });
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(qrPath) >= 0, 5000);
    QObject* menu = m_window->findChild<QObject*>("libraryContextMenu");
    QVERIFY(menu);
    const auto openMenu = [&](const QString& path) {
      return QMetaObject::invokeMethod(m_window, "openContextMenu",
          Q_ARG(QVariant, m_library->rowOf(path)), Q_ARG(QVariant, 100), Q_ARG(QVariant, 100));
    };
    QVERIFY(openMenu(qrPath));
    QTRY_VERIFY_WITH_TIMEOUT(menu->property("conditionalEntriesVisible").toBool(), 3000);
    QQuickItem* rename = item("contextAction_rename");
    QVERIFY(rename);
    QQuickItem* contextQrAction = item("contextAction_qr");
    QVERIFY(contextQrAction);
    QQuickItem* qrSeparator = item("contextAction_conditionalSeparator");
    QVERIFY(qrSeparator);
    QTRY_VERIFY(contextQrAction->isVisible() && qrSeparator->isVisible());
    QCOMPARE(contextQrAction->height(), 30.0);
    QCOMPARE(qrSeparator->height(), 9.0);
    const double renameY = rename->property("y").toDouble();
    m_qr->inspect(negativePath);
    QTRY_VERIFY(!menu->property("conditionalEntriesVisible").toBool());
    QTRY_VERIFY(!contextQrAction->isVisible() && !qrSeparator->isVisible());
    QCOMPARE(contextQrAction->height(), 0.0);
    QCOMPARE(qrSeparator->height(), 0.0);
    m_qr->inspect(qrPath);
    QTRY_VERIFY(menu->property("conditionalEntriesVisible").toBool());
    QCOMPARE(item("contextAction_rename"), rename);
    QCOMPARE(rename->property("y").toDouble(), renameY);
    QMetaObject::invokeMethod(menu, "close");
    QVERIFY(openMenu(negativePath));
    QTRY_VERIFY_WITH_TIMEOUT(!m_qr->checking(), 3000);
    QVERIFY(!menu->property("conditionalEntriesVisible").toBool());
    // A completion for another image must not reveal an action on this target.
    m_qr->inspect(qrPath);
    QTRY_VERIFY(m_qr->detected());
    QVERIFY(!menu->property("conditionalEntriesVisible").toBool());
    QMetaObject::invokeMethod(menu, "close");
    openDetail(m_library->rowOf(qrPath));
    QTRY_COMPARE(m_qr->path(), qrPath);
    QTRY_VERIFY_WITH_TIMEOUT(!m_qr->checking(), 3000);
    QVERIFY(m_qr->detected());
    m_qr->inspect(negativePath);
    QTRY_VERIFY(!detail->property("qrDetected").toBool());
    QVERIFY(QMetaObject::invokeMethod(detail, "openImageContextMenu", Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QObject* previewMenu = m_window->findChild<QObject*>("detailContextMenu");
    QTRY_VERIFY(previewMenu->property("visible").toBool());
    QQuickItem* previewRename = item("detailContextAction_rename");
    QQuickItem* previewQr = item("detailContextAction_qr");
    QVERIFY(!previewQr->isVisible());
    QCOMPARE(previewQr->height(), 0.0);
    m_qr->inspect(qrPath);
    QTRY_VERIFY(previewQr->isVisible());
    QCOMPARE(item("detailContextAction_rename"), previewRename);
    QMetaObject::invokeMethod(previewMenu, "close");

    // Inserting or removing the asynchronous QR row must not move keyboard
    // focus onto a different action farther down the list.
    QVERIFY(QMetaObject::invokeMethod(detail, "focusActionById",
                                      Q_ARG(QVariant, QStringLiteral("rename"))));
    QTRY_VERIFY(find(detail, [](QQuickItem* candidate) {
                  return candidate->objectName() == QStringLiteral("viewerAction_rename") &&
                         candidate->hasActiveFocus();
                }) != nullptr);
    m_qr->inspect(negativePath);
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

    // The menu scrolls to whatever is focused, so ask for the QR row before
    // reaching for it; the row only exists once zbar has decoded the file.
    QVERIFY(QMetaObject::invokeMethod(detail, "focusActionById",
                                      Q_ARG(QVariant, QStringLiteral("qr"))));
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
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(qrPath) < 0, 5000);
  }

  void viewerShowsUsefulMediaMetadata() {
    QQuickItem* detail = item("detail");
    openDetail(0);
    QTRY_VERIFY(detail->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("stillReady").toBool(), 10000);
    // The inspector is on demand now; ask for it before reading the details.
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());
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
    m_captures->applyMetadata(
        {{path, record.modified, record.bytes, enriched, record.device, record.inode}});
    QTRY_COMPARE(detail->property("dayLabel").toString(),
                 m_captures->dayLabelAt(m_captures->rowOf(path)));
    QTRY_COMPARE(detail->property("timeLabel").toString(),
                 m_library->timeLabelAt(m_library->rowOf(path)));

    m_captures->applyMetadata(
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
    m_captures->applyMetadata(
        {{selectedPath, record.modified, record.bytes, moved, record.device, record.inode}});

    QTRY_VERIFY(!reordered.isEmpty());
    QTRY_COMPARE(m_library->rowOf(selectedPath), 0);
    QTRY_COMPARE(pathAt(grid->property("currentIndex").toInt()), selectedPath);

    m_captures->applyMetadata({{selectedPath, record.modified, record.bytes, record.captured,
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
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(item("viewerActionPopup")->isVisible());
    QTest::keyClick(m_window, Qt::Key_F5);
    QTRY_VERIFY(detail->property("slideshowRunning").toBool());
    QVERIFY(detail->property("fullScreen").toBool());
    QVERIFY(!detail->property("showInfo").toBool());
    QVERIFY(!detail->property("actionNavigationActive").toBool());
    QVERIFY(!item("viewerActionPopup")->isVisible());
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

  void shuffledFolderPreviewStaysInTheExactFolder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("child"))));
    const QString first = dir.filePath(QStringLiteral("first.png"));
    const QString second = dir.filePath(QStringLiteral("second.png"));
    const QString child = dir.filePath(QStringLiteral("child/third.png"));
    QImage fixture(80, 60, QImage::Format_RGB32);
    fixture.fill(Qt::blue);
    for (const QString& path : {first, second, child}) {
      QVERIFY(fixture.save(path));
      m_disposablePaths.append(path);
    }
    const bool previousShuffle = m_settings->slideshowShuffle();
    const auto restore = qScopeGuard([&] {
      invoke("dismissTopLayer");
      m_settings->setSlideshowShuffle(previousShuffle);
      QMetaObject::invokeMethod(m_window, "openFolder", Q_ARG(QVariant, QString()));
      rescan();
    });
    m_captures->addExtraFiles({first, second, child});
    // The viewer opens once the scan has listed the file.
    waitForScan();
    QVERIFY(m_captures->rowOf(first) >= 0);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPath", Q_ARG(QVariant, first)));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QVERIFY(prop("viewerFolderOnly").toBool());
    m_settings->setSlideshowShuffle(true);
    QVERIFY(QMetaObject::invokeMethod(detail, "setSlideshow", Q_ARG(QVariant, true)));
    for (int index = 0; index < 12; ++index) {
      const QString before = detail->property("path").toString();
      QVERIFY(QMetaObject::invokeMethod(m_window, "navigateDetail", Q_ARG(QVariant, 1)));
      QCOMPARE(detail->property("path").toString(), before == first ? second : first);
      QVERIFY(detail->property("slideshowRunning").toBool());
    }
  }

  void explicitSelectionNavigatesInOrderAcrossFolders() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath("other folder")));
    const QString first = dir.filePath(QStringLiteral(".first # 雪.png"));
    const QString second = dir.filePath(QStringLiteral("other folder/second.png"));
    const QString third = dir.filePath(QStringLiteral("third.png"));
    QImage fixture(80, 60, QImage::Format_RGB32);
    fixture.fill(Qt::green);
    for (const QString& path : {first, second, third}) {
      QVERIFY(fixture.save(path));
      m_disposablePaths.append(path);
    }
    m_disposablePaths.append(dir.filePath(QStringLiteral("other folder/renamed selection.png")));
    const QStringList selection{second, first, third};
    m_captures->addExtraFiles(selection);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPaths", Q_ARG(QVariant, selection)));
    QQuickItem* detail = item("detail");
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(m_window, "openFolder", Q_ARG(QVariant, QString()));
      rescan();
    });
    // The viewer opens once the library has rescanned with every filter cleared.
    // Under ASan with the other heavy suites running in parallel that takes 6 to
    // 10 seconds in CI, so allow the same budget as the suite's other scan waits.
    QTRY_VERIFY_WITH_TIMEOUT(detail->isVisible(), 15000);
    QTRY_COMPARE(detail->property("path").toString(), second);
    QTRY_VERIFY(detail->property("canNavigate").toBool());
    QCOMPARE(detail->property("selectionLabel").toString(), QStringLiteral("1 of 3 selected files"));
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("stillReady").toBool(), 5000);
    for (const QString& expected : {first, third, second}) {
      QTest::keyClick(m_window, Qt::Key_Right);
      QTRY_COMPARE(detail->property("path").toString(), expected);
    }
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(detail->property("path").toString(), third);
    QVERIFY(QFile::remove(first));
    rescan();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowOf(first), -1, 15000);
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(detail->property("path").toString(), second);
    // A later Open With request replaces the previous selection.
    const QStringList replacement{third, second};
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPaths", Q_ARG(QVariant, replacement)));
    QTRY_COMPARE(detail->property("path").toString(), third);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(detail->property("path").toString(), second);
    // Renaming a directly added file must keep its result discoverable.
    perform(QStringLiteral("rename"), second);
    QTRY_VERIFY(item("renameSheet")->isVisible());
    item("renameInput")->setProperty("text", QStringLiteral("renamed selection"));
    click(pill(item("renameSheet"), QStringLiteral("Rename")));
    QTRY_VERIFY(!item("renameSheet")->isVisible());
    const QString renamed = dir.filePath(QStringLiteral("other folder/renamed selection.png"));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(renamed) >= 0, 5000);
    openDetail(m_library->rowOf(renamed));
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(detail->property("path").toString(), third);
    // A single-file request restores the folder navigation behavior.
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPath", Q_ARG(QVariant, renamed)));
    QVERIFY(prop("viewerPaths").value<QJSValue>().toVariant().toList().isEmpty());
    QVERIFY(prop("viewerFolderOnly").toBool());
    QVERIFY(detail->property("selectionLabel").toString().isEmpty());
  }

  void pdfPreviewPagesInPlaceAndOffersDocumentActions() {
    const QSize previousSize = widenForDocumentChrome();
    const auto restoreSize = qScopeGuard([&] { m_window->resize(previousSize); });
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
    rescan();
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
    // The action menu is hidden until asked for; open it to prove the document
    // action is offered (present even when its program is missing), then put
    // the menu away again.
    detail->setProperty("actionNavigationActive", true);
    QTRY_VERIFY(find(detail, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("viewerAction_open-document") &&
             candidate->isVisible();
    }) != nullptr);
    QMetaObject::invokeMethod(detail, "focusPreview");
    QTRY_VERIFY(!detail->property("actionNavigationActive").toBool());
    QCOMPARE(detail->property("pdfPage").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_PageDown);
    QTRY_COMPARE(detail->property("pdfPage").toInt(), 2);

    // Search needs pdftotext and a fixture with an extractable text layer. A
    // headless build image can rasterise the QPdfWriter output without one, so
    // probe the file first rather than assert on text that is not there.
    const QString pdftotext = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    QString extractedText;
    if (!pdftotext.isEmpty()) {
      QProcess probe;
      probe.start(pdftotext, {QStringLiteral("-layout"), m_pdfPath, QStringLiteral("-")});
      if (probe.waitForFinished(10000)) {
        extractedText = QString::fromUtf8(probe.readAllStandardOutput());
      }
    }
    if (!extractedText.trimmed().isEmpty()) {
      QQuickItem* search = item("pdfSearchInput");
      QVERIFY(search);
      search->setProperty("text", QStringLiteral("e"));
      QVERIFY(QMetaObject::invokeMethod(detail, "findInPdf"));
      QTRY_COMPARE_WITH_TIMEOUT(m_pdfInfo->matchCount(), 2, 8000);
      QTRY_COMPARE(detail->property("pdfPage").toInt(), 1);
      QVERIFY(QMetaObject::invokeMethod(detail, "stepPdfMatch", Q_ARG(QVariant, 1)));
      QTRY_COMPARE(detail->property("pdfPage").toInt(), 2);
      QVERIFY(QMetaObject::invokeMethod(detail, "stepPdfMatch", Q_ARG(QVariant, 1)));
      QTRY_COMPARE(detail->property("pdfPage").toInt(), 1);
    }

    // Copy page text reports an outcome; whether the text extracts depends on
    // the environment, so only the outcome is asserted.
    if (!pdftotext.isEmpty()) {
      QSignalSpy copied(m_pdfInfo, &PdfInspector::textCopied);
      QSignalSpy copyFailed(m_pdfInfo, &PdfInspector::textCopyFailed);
      click(item("pdfCopyPageText"));
      QTRY_VERIFY_WITH_TIMEOUT(copied.size() + copyFailed.size() >= 1, 8000);
    }

    // Jump straight to a page by number.
    QQuickItem* pageInput = item("pdfPageInput");
    QVERIFY(pageInput);
    pageInput->setProperty("text", QStringLiteral("2"));
    QVERIFY(QMetaObject::invokeMethod(pageInput, "accepted"));
    QTRY_COMPARE(detail->property("pdfPage").toInt(), 2);

    // Continuous fit-width is the default; fit-page swaps the surfaces.
    QVERIFY(detail->property("pdfFitWidth").toBool());
    QQuickItem* list = item("pdfPageList");
    QCOMPARE(list->property("count").toInt(), 2);
    QVERIFY(item("pdfScroll")->isVisible());
    click(item("pdfFitPage"));
    QVERIFY(!detail->property("pdfFitWidth").toBool());
    QVERIFY(!item("pdfScroll")->isVisible());
    click(item("pdfFitWidth"));
    QVERIFY(detail->property("pdfFitWidth").toBool());
    QVERIFY(item("pdfScroll")->isVisible());

    invoke("dismissTopLayer");
    QVERIFY(QFile::remove(m_pdfPath));
    rescan();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowOf(m_pdfPath), -1, 5000);
  }

  void pdfTextSelectionPicksTheWordsDraggedOver() {
    const QSize previousSize = widenForDocumentChrome();
    const auto restoreSize = qScopeGuard([&] { m_window->resize(previousSize); });
    // Letter, not A4: its aspect differs from the placeholder the list draws
    // while a page renders, so the page-cell geometry below is measured rather
    // than assumed. The file is checked in, because a build image without fonts
    // cannot write a text layer for a drag to find.
    const QString source = QFINDTESTDATA("fixtures/pdf/letter-pages.pdf");
    QVERIFY(!source.isEmpty());
    QVERIFY(QFile::copy(source, m_pdfPath));
    if (!PdfSupport::textAvailable()) {
      QSKIP("pdftotext is not installed");
    }

    // The clipboard the app is handed is a stub script, so the log starts empty
    // and proves what a copy actually put there.
    QFile::remove(m_scratch.filePath(QStringLiteral("clipboard.log")));
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(m_pdfPath) >= 0, 5000);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPath", Q_ARG(QVariant, m_pdfPath)));
    const auto restoreFilter = qScopeGuard([&] { m_library->setFolderFilter(QString()); });
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(m_pdfInfo->path(), m_pdfPath, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(m_pdfInfo->pageCount(), 2, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("stillReady").toBool(), 10000);
    QCOMPARE(detail->property("pdfSelectMode").toBool(), false);

    QQuickItem* list = item("pdfPageList");
    QQuickItem* firstPage = item("pdfPageImage");
    QVERIFY(firstPage);
    QVERIFY(list->property("interactive").toBool());
    QQuickItem* layer = [&] {
      for (QQuickItem* page : list->childItems()) {
        if (QQuickItem* found = find(page, [](QQuickItem* candidate) {
              return candidate->objectName() == QStringLiteral("pdfSelectionScroll");
            })) {
          return found;
        }
      }
      return static_cast<QQuickItem*>(nullptr);
    }();
    QVERIFY(layer);
    QVERIFY(!layer->property("selecting").toBool());
    QCOMPARE(layer->property("page").toInt(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(firstPage->property("status").toInt(), 1, 10000);
    QTRY_VERIFY(firstPage->property("implicitWidth").toDouble() > 0 &&
                firstPage->height() > firstPage->width() * 1.2);
    // The page cell follows the loaded page, not the requested size: a
    // regression here collapses the continuous view to a sliver per page.
    QVERIFY2(firstPage->height() > firstPage->width() * 1.2,
             qPrintable(QStringLiteral("page cell %1x%2")
                            .arg(firstPage->width())
                            .arg(firstPage->height())));
    QVERIFY(firstPage->property("implicitWidth").toDouble() > 0);
    QCOMPARE(firstPage->height(),
             firstPage->width() * firstPage->property("implicitHeight").toDouble()
                 / firstPage->property("implicitWidth").toDouble());
    QVERIFY(list->property("contentHeight").toDouble() > list->height() * 1.5);

    // The text controls live in the search row, which has room beside the find
    // box, rather than in the page row. Both rows keep their width when a
    // selection appears, so no control moves under the pointer mid-click. The
    // rows themselves are not measured against the stage: their width follows
    // the installed fonts, which a test image need not share.
    QQuickItem* searchRow = item("pdfSearchRow");
    QVERIFY(find(searchRow, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("pdfSelectText");
    }));
    QVERIFY(find(searchRow, [](QQuickItem* candidate) {
      return candidate->objectName() == QStringLiteral("pdfCopySelection");
    }));
    const qreal pageRowWidth = item("pdfPageRow")->property("implicitWidth").toReal();
    const qreal searchRowWidth = searchRow->property("implicitWidth").toReal();
    // The copy control is in place before there is anything to copy, dimmed
    // rather than hidden.
    QQuickItem* copySelection = item("pdfCopySelection");
    QVERIFY(copySelection->isVisible());
    QVERIFY(!copySelection->property("enabled").toBool());

    // The words sit high on the page, so a drag down the page crosses them.
    const auto dragRect = [](QQuickItem* target) {
      return std::pair<QPointF, QPointF>(
          QPointF(target->width() * 0.04, target->height() * 0.08),
          QPointF(target->width() * 0.62, target->height() * 0.18));
    };

    // With the mode off the drag belongs to the list, and nothing is selected.
    const auto [offFrom, offTo] = dragRect(firstPage);
    dragBetween(firstPage, offFrom, offTo);
    QVERIFY(!m_pdfInfo->hasSelection());
    settle(list);

    clickSettled(item("pdfSelectText"));
    QCOMPARE(detail->property("pdfSelectMode").toBool(), true);
    QCOMPARE(layer->property("selecting").toBool(), true);

    // A press that never moves is a click, not a selection.
    click(firstPage, Qt::LeftButton,
          QPoint(static_cast<int>(firstPage->width() * 0.05),
                 static_cast<int>(firstPage->height() * 0.10)));
    QVERIFY(!m_pdfInfo->hasSelection());

    // One that does move picks the words under it, and the page does not
    // flick away from under the drag: selecting is not scrolling.
    settle(list);
    const qreal beforeDrag = list->property("contentY").toReal();
    const auto [from, to] = dragRect(firstPage);
    dragBetween(firstPage, from, to);
    QTRY_VERIFY_WITH_TIMEOUT(m_pdfInfo->hasSelection(), 8000);
    QVERIFY(!list->property("moving").toBool());
    QCOMPARE(list->property("contentY").toReal(), beforeDrag);
    QCOMPARE(m_pdfInfo->selectionPage(), 1);
    QVERIFY2(m_pdfInfo->selectionText().contains(QStringLiteral("Omaroll")),
             qPrintable(m_pdfInfo->selectionText()));
    QVERIFY(!m_pdfInfo->selectionRects().isEmpty());

    // The copy control wakes up with the selection, without the row moving.
    QTRY_VERIFY_WITH_TIMEOUT(copySelection->property("enabled").toBool(), 3000);
    QCOMPARE(item("pdfPageRow")->property("implicitWidth").toReal(), pageRowWidth);
    QCOMPARE(searchRow->property("implicitWidth").toReal(), searchRowWidth);
    QSignalSpy copied(m_pdfInfo, &PdfInspector::selectionCopied);
    QSignalSpy failed(m_pdfInfo, &PdfInspector::selectionFailed);
    clickSettled(copySelection);
    QTRY_VERIFY_WITH_TIMEOUT(!copied.isEmpty(), 8000);
    QCOMPARE(failed.size(), 0);

    // The clipboard the app was handed received the words themselves.
    QFile clipboard(m_scratch.filePath(QStringLiteral("clipboard.log")));
    QVERIFY(clipboard.open(QIODevice::ReadOnly));
    const QByteArray copiedText = clipboard.readAll();
    QVERIFY2(copiedText.contains("Omaroll"), copiedText.constData());

    // The wheel still moves through the document while the mode is on, so the
    // rest of a page and the pages below it stay reachable.
    const qreal beforeWheel = list->property("contentY").toReal();
    wheelOver(list, 3);
    QTRY_VERIFY_WITH_TIMEOUT(list->property("contentY").toReal() > beforeWheel, 3000);
    // And the selection survives a scroll.
    QVERIFY(m_pdfInfo->hasSelection());

    // Escape leaves selection mode and closes nothing.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(detail->property("pdfSelectMode").toBool(), false);
    QVERIFY(detail->isVisible());
    QVERIFY(!m_pdfInfo->hasSelection());

    // The fitted page carries a layer of its own, over the same words.
    clickSettled(item("pdfFitPage"));
    QVERIFY(!detail->property("pdfFitWidth").toBool());
    clickSettled(item("pdfSelectText"));
    QCOMPARE(detail->property("pdfSelectMode").toBool(), true);
    QQuickItem* stillLayer = item("pdfSelectionStill");
    QVERIFY(stillLayer);
    QCOMPARE(stillLayer->property("selecting").toBool(), true);
    QCOMPARE(stillLayer->property("page").toInt(), detail->property("pdfPage").toInt());
    QQuickItem* stillPage = stillLayer->parentItem();
    QVERIFY(stillPage);
    const auto [stillFrom, stillTo] = dragRect(stillPage);
    dragBetween(stillPage, stillFrom, stillTo);
    QTRY_VERIFY_WITH_TIMEOUT(m_pdfInfo->hasSelection(), 8000);
    QCOMPARE(m_pdfInfo->selectionPage(), detail->property("pdfPage").toInt());

    // Turning the mode off leaves the mode and the page behind as they were.
    clickSettled(item("pdfSelectText"));
    QCOMPARE(detail->property("pdfSelectMode").toBool(), false);
    QVERIFY(!m_pdfInfo->hasSelection());
    clickSettled(item("pdfFitWidth"));

    invoke("dismissTopLayer");
    QTRY_VERIFY_WITH_TIMEOUT(!m_pdfInfo->hasSelection(), 3000);
    QVERIFY(QFile::remove(m_pdfPath));
    rescan();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowOf(m_pdfPath), -1, 5000);
  }

  void narrowWindowKeepsTheDocumentControlsThatFit() {
    // The document rows carry words, so they give way as the window narrows.
    // What a reader still needs stays: the page, its navigation and the way into
    // text selection. The pills that need more room or repeat the action list
    // step aside. The assertions follow the rows' own flags rather than absolute
    // widths, because the label metrics differ between this desktop and CI.
    const QString source = QFINDTESTDATA("fixtures/pdf/letter-pages.pdf");
    QVERIFY(!source.isEmpty());
    // The window belongs to the suite, and rows shrink their labels when it is
    // narrow, so put back exactly the size this test found.
    const QSize previousSize = m_window->size();
    const QString path =
        QFileInfo(m_pdfPath).absolutePath() + QStringLiteral("/narrow-pages.pdf");
    QVERIFY(QFile::copy(source, path));
    // The tiles and the viewer keep asking for this file's thumbnail after it is
    // removed, which is a disposable fixture rather than a QML warning.
    m_disposablePaths.append(path);
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(path) >= 0, 5000);
    QVERIFY(QMetaObject::invokeMethod(m_window, "openPath", Q_ARG(QVariant, path)));
    // Leave nothing behind on any exit path: opening one file narrows the library
    // to its folder, which would hide the videos from later tests, and a leftover
    // file would too. The window belongs to the suite and rows shrink their
    // labels when it is narrow, so put back exactly the size this test found.
    const auto restoreState = qScopeGuard([&] {
      m_window->resize(previousSize);
      invoke("dismissTopLayer");
      m_library->setFolderFilter(QString());
      QFile::remove(path);
      rescan();
    });
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(m_pdfInfo->path(), path, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!m_pdfInfo->loading(), 5000);
    // The document rows give way against the stage; with the inspector open
    // the stage is as narrow here as it was when the rows were first built.
    detail->setProperty("showInfo", true);
    QTRY_VERIFY(item("viewerInspector")->isVisible());

    // A page in the fit-width list carries an edge of its own: white paper on a
    // light theme has nothing else to separate it from the stage behind it.
    QQuickItem* edge = item("pdfPageEdge");
    QVERIFY(edge);
    QVERIFY(edge->isVisible());

    QQuickItem* copyPageText = item("pdfCopyPageText");
    QQuickItem* pageInput = item("pdfPageInput");
    QQuickItem* selectText = item("pdfSelectText");
    QQuickItem* copySelection = item("pdfCopySelection");
    QVERIFY(copyPageText && pageInput && selectText && copySelection);

    // The smallest window the app allows. Each row gives way on its own width:
    // the search row narrows and drops the pill Ctrl+C stands in for, the page
    // row shortens its labels and drops what needs the most room.
    m_window->resize(560, 420);
    waitForLayout(QSize(560, 420));
    qInfo() << "document chrome at" << m_window->size()
            << "search row" << item("pdfSearchRow")->property("implicitWidth").toReal()
            << "page row" << item("pdfPageRow")->property("implicitWidth").toReal()
            << "search compact" << detail->property("compactPdfSearch").toBool()
            << "page compact" << detail->property("compactPdfPage").toBool()
            << "page narrow" << detail->property("narrowPdfPage").toBool();
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("compactPdfSearch").toBool(), 3000);
    QTRY_VERIFY(detail->property("compactPdfPage").toBool());
    QTRY_VERIFY(!copyPageText->isVisible());
    QTRY_VERIFY(!pageInput->isVisible());
    QVERIFY(!copySelection->isVisible());
    QVERIFY(item("pdfPageRow")->isVisible());
    QVERIFY(selectText->isVisible());

    // A wide window keeps every control, in any font metric: the labels are the
    // first thing to shorten, and nothing is dropped until even those do not fit.
    m_window->resize(1600, 900);
    QTRY_VERIFY_WITH_TIMEOUT(copyPageText->isVisible(), 3000);
    QTRY_VERIFY(pageInput->isVisible());
    QVERIFY(selectText->isVisible());
    QVERIFY(copySelection->isVisible());

    // The fitted page carries the same edge over the page sheet.
    clickSettled(item("pdfFitPage"));
    QVERIFY(!detail->property("pdfFitWidth").toBool());
    QQuickItem* fittedEdge = item("pdfFittedPageEdge");
    QVERIFY(fittedEdge);
    QVERIFY(fittedEdge->isVisible());

    invoke("dismissTopLayer");
    QVERIFY(QFile::remove(path));
    rescan();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowOf(path), -1, 5000);
  }

  void albumFromASelection() {
    QTest::keyClick(m_window, Qt::Key_X);
    QTRY_COMPARE(item("library")->property("checkedCount").toInt(), 1);
    // The header row re-lays itself out in the next polish pass once the
    // selection pills appear; click only once the pill has its place.
    QQuickItem* albumPill = pill(m_window->contentItem(), QStringLiteral("Organize"));
    QVERIFY(albumPill);
    QTRY_VERIFY(centre(albumPill).x() > 0 && centre(albumPill).x() < m_window->width());
    waitForLayout();
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
    waitForLayout();
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
    clickPlaced(pill(m_window->contentItem(), QStringLiteral("Keep selected")));
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

  void keepSelectedSkipsACopyReplacedWhileThePromptIsOpen() {
    QTemporaryDir dir;
    const QString kept = dir.filePath(QStringLiteral("kept.png"));
    const QString copy = dir.filePath(QStringLiteral("copy.png"));
    m_disposablePaths.append(kept);
    m_disposablePaths.append(copy);
    QImage picture(35, 29, QImage::Format_RGB32);
    picture.fill(QColor(17, 29, 143));
    QVERIFY(picture.save(kept));
    QVERIFY(QFile::copy(kept, copy));
    m_captures->addExtraFiles({kept, copy});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(copy) >= 0, 10000);
    m_library->setDuplicatesOnly(true);
    QTRY_COMPARE_WITH_TIMEOUT(m_duplicates->otherCopies(kept), QStringList{copy}, 15000);
    QVERIFY(QMetaObject::invokeMethod(m_window, "keepSelectedDuplicate", Q_ARG(QVariant, kept)));
    QTRY_VERIFY(item("confirm")->isVisible());
    QVERIFY(QFile::rename(copy, dir.filePath(QStringLiteral("old.png"))));
    picture.fill(Qt::yellow);
    QVERIFY(picture.save(copy));
    QSignalSpy finished(m_actions, &ActionLauncher::duplicateCleanupFinished);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(finished.first().at(0).toInt(), 0);
    QCOMPARE(finished.first().at(1).toStringList(), QStringList{copy});
    QCOMPARE(QImage(copy).pixelColor(0, 0), QColor(Qt::yellow));
    QVERIFY(QFileInfo::exists(kept));
    m_library->setDuplicatesOnly(false);
    QVERIFY(dir.remove());
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_captures->rowOf(kept) < 0 && m_captures->rowOf(copy) < 0, 10000);
  }

  void libraryFilePromptsPreserveSymlinkTargetsAndRejectReplacements_data() {
    QTest::addColumn<bool>("rename");
    QTest::addColumn<bool>("replace");
    QTest::newRow("rename-alias") << true << false;
    QTest::newRow("trash-alias") << false << false;
    QTest::newRow("rename-replacement") << true << true;
    QTest::newRow("trash-replacement") << false << true;
  }

  void libraryFilePromptsPreserveSymlinkTargetsAndRejectReplacements() {
    QFETCH(bool, rename);
    QFETCH(bool, replace);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("original.png"));
    const QString alias = dir.filePath(QStringLiteral("alias.jpg"));
    m_disposablePaths.append(media);
    QImage picture(32, 24, QImage::Format_RGB32);
    picture.fill(Qt::cyan);
    QVERIFY(picture.save(media));
    if (!replace) QVERIFY(QFile::link(media, alias));
    m_captures->addExtraFiles({replace ? media : alias});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(media) >= 0, 10000);
    QCOMPARE(m_library->deletionPathAt(m_library->rowOf(media)), replace ? media : alias);
    m_settings->toggleFavorite(media);
    const auto clearFavorite = qScopeGuard([&] {
      if (m_settings->isFavorite(media)) m_settings->toggleFavorite(media);
    });
    perform(rename ? QStringLiteral("rename") : QStringLiteral("trash"), media);
    QQuickItem* sheet = item(rename ? "renameSheet" : "confirm");
    QTRY_VERIFY(sheet->isVisible());
    if (rename) {
      QCOMPARE(sheet->property("path").toString(), replace ? media : alias);
      item("renameInput")->setProperty("text", QStringLiteral("renamed"));
    } else {
      QCOMPARE(sheet->property("detail").toString(), replace ? media : alias);
    }
    if (replace) {
      QVERIFY(QFile::rename(media, dir.filePath(QStringLiteral("old.png"))));
      picture.fill(Qt::magenta);
      QVERIFY(picture.save(media));
    }
    if (rename) click(pill(sheet, QStringLiteral("Rename")));
    else QTest::keyClick(m_window, Qt::Key_Return);
    if (replace && rename) {
      QVERIFY(sheet->isVisible());
      QVERIFY(sheet->property("errorMessage").toString().contains(QStringLiteral("changed")));
      invoke("dismissTopLayer");
    } else {
      QTRY_VERIFY(!sheet->isVisible());
    }
    QVERIFY(QFileInfo::exists(media));
    QVERIFY(m_settings->isFavorite(media));
    QCOMPARE(QImage(media).pixelColor(0, 0), replace ? QColor(Qt::magenta) : QColor(Qt::cyan));
    if (!replace) {
      QVERIFY(!QFileInfo(alias).isSymLink());
      if (rename) {
        const QString renamed = dir.filePath(QStringLiteral("renamed.jpg"));
        QVERIFY(QFileInfo(renamed).isSymLink());
        QCOMPARE(QFileInfo(renamed).canonicalFilePath(), media);
      }
    } else {
      QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("renamed.png"))));
    }
    QVERIFY(dir.remove());
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_captures->rowOf(media) < 0, 10000);
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

  // The library's Trash, single, batch and duplicate cleanup, is one undo
  // step each, from the footer's Undo or Ctrl+Z.
  void libraryTrashUndoBringsFilesBack() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("undo-a.png"));
    const QString second = dir.filePath(QStringLiteral("undo-b.png"));
    const QString copy = dir.filePath(QStringLiteral("undo-copy.png"));
    QImage picture(37, 23, QImage::Format_RGB32);
    picture.fill(QColor(201, 33, 77));
    QVERIFY(picture.save(first));
    picture.fill(QColor(11, 133, 77));
    QVERIFY(picture.save(second));
    QVERIFY(QFile::copy(second, copy));
    for (const QString& path : {first, second, copy}) m_disposablePaths.append(path);
    m_captures->addExtraFiles({first, second, copy});
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      m_library->setDuplicatesOnly(false);
      dir.remove();
      rescan();
      QTRY_VERIFY(!m_captures->scanning());
    });
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(first) >= 0 && m_library->rowOf(second) >= 0 &&
                             m_library->rowOf(copy) >= 0, 10000);
    QQuickItem* grid = item("library");

    // One file, put back with the footer's Undo.
    grid->setProperty("currentIndex", m_library->rowOf(first));
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(item("confirm")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!item("confirm")->isVisible());
    QTRY_VERIFY(!QFileInfo::exists(first));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(first) < 0, 10000);
    QQuickItem* undo = item("footerUndoButton");
    QTRY_VERIFY(undo->isVisible());
    click(undo);
    QTRY_VERIFY(QFileInfo::exists(first));
    QCOMPARE(QImage(first).pixelColor(0, 0), QColor(201, 33, 77));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(first) >= 0, 10000);
    QVERIFY(!undo->isVisible());

    // A batch is one step.
    for (const QString& path : {first, second})
      QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, path)));
    QVERIFY(QMetaObject::invokeMethod(m_window, "requestDeleteBatch",
                                    Q_ARG(QVariant, QVariant(QStringList{first, second})),
                                    Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
    QTRY_VERIFY(item("confirm")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!item("confirm")->isVisible());
    QTRY_VERIFY(!QFileInfo::exists(first) && !QFileInfo::exists(second));
    QTRY_VERIFY(undo->isVisible());
    QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(QFileInfo::exists(first) && QFileInfo::exists(second));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(first) >= 0 && m_library->rowOf(second) >= 0, 10000);

    // Duplicate cleanup, which checks the bytes on a worker, is one step too.
    m_library->setDuplicatesOnly(true);
    QTRY_COMPARE_WITH_TIMEOUT(m_duplicates->otherCopies(second), QStringList{copy}, 15000);
    QSignalSpy finished(m_actions, &ActionLauncher::duplicateCleanupFinished);
    QVERIFY(QMetaObject::invokeMethod(m_window, "keepSelectedDuplicate", Q_ARG(QVariant, second)));
    QTRY_VERIFY(item("confirm")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!QFileInfo::exists(copy));
    QTRY_VERIFY(undo->isVisible());
    QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(QFileInfo::exists(copy));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(copy) >= 0, 10000);
  }

  // Similar sets show as sections; Compare lists the facts for choosing a
  // keeper and marks the largest; keeping one moves the rest to Trash as a
  // single step that one Undo puts back.
  void similarSetsShowAsSectionsAndKeepOneIsOneUndoStep() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage scene(960, 640, QImage::Format_RGB32);
    {
      QPainter painter(&scene);
      QLinearGradient gradient(0, 0, scene.width(), scene.height());
      gradient.setColorAt(0, QColor(30, 90, 160));
      gradient.setColorAt(1, QColor(240, 170, 60));
      painter.fillRect(scene.rect(), gradient);
      painter.setBrush(QColor(250, 240, 220));
      painter.setPen(Qt::NoPen);
      painter.drawEllipse(QPoint(300, 260), 140, 140);
      painter.drawEllipse(QPoint(700, 420), 90, 90);
    }
    const QString original = dir.filePath(QStringLiteral("keep-scene.png"));
    const QString small = dir.filePath(QStringLiteral("keep-scene-small.png"));
    const QString large = dir.filePath(QStringLiteral("keep-scene-large.jpg"));
    QVERIFY(scene.save(original));
    QVERIFY(scene.scaled(480, 320, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(small));
    QVERIFY(scene.scaled(1440, 960, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(large, "JPG", 80));
    for (const QString& path : {original, small, large}) m_disposablePaths.append(path);
    m_captures->addExtraFiles({original, small, large});
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      item("compareSheet")->setProperty("paths", QStringList());
      dir.remove();
      m_captures->refresh();
      waitForScan();
      // With the view still on, the similar index drops the deleted set, so
      // a later test that turns it on doesn't start from it.
      QTRY_VERIFY_WITH_TIMEOUT(!m_similarities->scanning() && m_similarities->groupPaths(original).isEmpty() &&
                                   m_similarities->groupPaths(small).isEmpty() &&
                                   m_similarities->groupPaths(large).isEmpty(),
                               15000);
      m_library->setSimilarOnly(false);
    });
    // The added files arrive with the scan they start, which takes over ten
    // seconds on an emulated aarch64 runner.
    waitForScan();
    QTRY_VERIFY(m_library->rowOf(original) >= 0 && m_library->rowOf(small) >= 0 &&
                m_library->rowOf(large) >= 0);

    m_library->setSimilarOnly(true);
    QTRY_COMPARE_WITH_TIMEOUT(m_similarities->groupPaths(original),
                              QStringList({large, small, original}), 15000);
    QQuickItem* sections = item("reviewSets");
    QTRY_VERIFY(sections->isVisible());
    QVERIFY(!m_library->reviewSections().isEmpty());
    const auto headers = [&] {
      QStringList labels;
      auto* rows = qobject_cast<SectionRows*>(m_library->sectionRows());
      for (int row = 0; rows && row < rows->rowCount(); ++row) {
        const QModelIndex at = rows->index(row);
        if (rows->data(at, SectionRows::HeaderRole).toBool()) {
          labels.append(rows->data(at, SectionRows::LabelRole).toString() + QLatin1Char(':') +
                        QString::number(rows->data(at, SectionRows::SectionCountRole).toInt()));
        }
      }
      return labels;
    };
    // The test's set is one section of three among any the demo library has.
    QTRY_VERIFY2(headers().filter(QRegularExpression(QStringLiteral("^Similar set \\d+:3$"))).size() >= 1,
                 qPrintable(headers().join(u' ')));
    QCOMPARE(item("library")->property("currentDayLabel").toString(), QString());

    QQuickItem* compare = item("compareSheet");
    QVERIFY(QMetaObject::invokeMethod(compare, "open",
                                    Q_ARG(QVariant, QVariant(QStringList{original, small, large}))));
    QTRY_VERIFY(compare->isVisible());
    QCOMPARE(compare->property("largestPath").toString(), large);
    const auto factLine = [&](const QString& path) {
      QVariant line;
      QMetaObject::invokeMethod(compare, "factLine", Q_RETURN_ARG(QVariant, line),
                                Q_ARG(QVariant, path));
      return line.toString();
    };
    QVERIFY2(factLine(large).startsWith(QStringLiteral("1440 × 960  ·  ")), qPrintable(factLine(large)));
    QVERIFY2(factLine(small).startsWith(QStringLiteral("480 × 320  ·  ")), qPrintable(factLine(small)));
    QCOMPARE(compare->property("facts").value<QJSValue>().property(small).property("folder").toString(),
             dir.path());

    QVERIFY(QMetaObject::invokeMethod(compare, "requestKeep", Q_ARG(QVariant, large)));
    QQuickItem* confirm = item("compareKeepConfirm");
    QTRY_VERIFY(confirm->isVisible());
    QVERIFY(confirm->property("title").toString().contains(QStringLiteral("the other 2")));
    // The prompt's targets were captured; files stay until it is accepted.
    QVERIFY(QFileInfo::exists(original) && QFileInfo::exists(small));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!confirm->isVisible());
    QTRY_VERIFY(!compare->isVisible());
    QTRY_VERIFY(!QFileInfo::exists(original) && !QFileInfo::exists(small));
    QVERIFY(QFileInfo::exists(large));
    QQuickItem* undo = item("footerUndoButton");
    QTRY_VERIFY(undo->isVisible());
    QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(QFileInfo::exists(original) && QFileInfo::exists(small));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(original) >= 0 && m_library->rowOf(small) >= 0,
                             10000);
  }

  void demoRendersShowRealSimilarSets() {
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      item("compareSheet")->setProperty("paths", QStringList());
      m_window->setProperty("renderingCompare", false);
      m_library->setSimilarOnly(false);
    });
    // What --render-view compare opens: a set the similar scan found.
    // --render grabs once renderReady says so, which is never before the
    // sheet is up, however long the scan takes.
    const auto ready = [&] {
      QVariant value;
      QMetaObject::invokeMethod(m_window, "renderReady", Q_RETURN_ARG(QVariant, value));
      return value.toBool();
    };
    QVERIFY(ready());
    QVERIFY(QMetaObject::invokeMethod(m_window, "openViewForRender", Q_ARG(QVariant, QStringLiteral("compare"))));
    // Ready only once the sheet is up with its pictures loaded.
    QVERIFY(!ready() || item("compareSheet")->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(item("compareSheet")->isVisible(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(ready(), 10000);
    QVariant pathsValue = item("compareSheet")->property("paths");
    if (pathsValue.metaType() == QMetaType::fromType<QJSValue>()) pathsValue = pathsValue.value<QJSValue>().toVariant();
    const QStringList shown = pathsValue.toStringList();
    QVERIFY2(shown.size() >= 2, qPrintable(shown.join(u' ')));
    QStringList sorted = shown;
    sorted.sort();
    QCOMPARE(m_similarities->groupPaths(shown.first()), sorted);
    // And --render-view similar has the demo's resized copy and its burst.
    QVERIFY(m_library->similarOnly());
    const auto setOf = [&](const QString& name) {
      QStringList names;
      for (const QString& path : m_similarities->groupPaths(m_demoPictures + QLatin1Char('/') + name))
        names << QFileInfo(path).fileName();
      return names;
    };
    QTRY_COMPARE_WITH_TIMEOUT(setOf(QStringLiteral("winter-cabin.jpg")),
                              QStringList({QStringLiteral("winter-cabin-small.jpg"),
                                           QStringLiteral("winter-cabin.jpg")}),
                              20000);
    QCOMPARE(setOf(QStringLiteral("coastal-wave.jpg")),
             QStringList({QStringLiteral("coastal-wave-2.jpg"), QStringLiteral("coastal-wave-3.jpg"),
                          QStringLiteral("coastal-wave.jpg")}));
    // New pictures in the sheet keep the render waiting until they load, so
    // it never saves a half-drawn sheet.
    QVERIFY(item("compareSheet")->isVisible());
    // The other set, so the sheet has not drawn these before: the same
    // pictures again could come straight from the image cache.
    const bool showedCabin = std::any_of(shown.cbegin(), shown.cend(), [](const QString& path) {
      return QFileInfo(path).fileName().startsWith(QStringLiteral("winter-cabin"));
    });
    const QStringList otherNames = showedCabin
        ? QStringList({QStringLiteral("coastal-wave.jpg"), QStringLiteral("coastal-wave-2.jpg")})
        : QStringList({QStringLiteral("winter-cabin.jpg"), QStringLiteral("winter-cabin-small.jpg")});
    QStringList other;
    for (const QString& name : otherNames) other << m_demoPictures + QLatin1Char('/') + name;
    item("compareSheet")->setProperty("paths", other);
    QVERIFY(!ready());
    QTRY_VERIFY_WITH_TIMEOUT(ready(), 10000);
  }

  void permanentDeleteUsesCapturedSelectionAndConfirmsInTheDetailViewer() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.png"));
    const QString second = dir.filePath(QStringLiteral("b.png"));
    const QString third = dir.filePath(QStringLiteral("c.png"));
    QImage picture(200, 150, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    for (const auto& path : {first, second, third}) QVERIFY(picture.save(path));
    m_captures->addExtraFiles({first, second, third});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(third) >= 0, 10000);
    m_settings->setConfirmPermanentDelete(true);
    const auto restore = qScopeGuard([&] { m_settings->setConfirmPermanentDelete(true); });
    QQuickItem* grid = item("library");
    grid->setProperty("currentIndex", m_library->rowOf(first));
    QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, first)));
    QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, second)));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QQuickItem* confirm = item("permanentConfirm");
    QTRY_VERIFY(confirm->isVisible());
    QVERIFY(confirm->property("title").toString().contains(QStringLiteral("2 files")));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(QFile::exists(first) && QFile::exists(second));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(confirm->isVisible());
    // Changing the selection behind the prompt must not change its targets.
    QVERIFY(QMetaObject::invokeMethod(grid, "clearChecked"));
    QVERIFY(QMetaObject::invokeMethod(grid, "toggleChecked", Q_ARG(QVariant, third)));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(first) && !QFile::exists(second));
    QVERIFY(QFile::exists(third));
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(first) < 0, 10000);
    openDetail(m_library->rowOf(third));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(confirm->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QVERIFY(QFile::exists(third));
    m_settings->setConfirmPermanentDelete(false);
    grid->setProperty("currentIndex", m_library->rowOf(third));
    grid->forceActiveFocus();
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Delete, Qt::ShiftModifier, QString(), true);
    QCoreApplication::sendEvent(m_window, &repeat);
    QVERIFY(QFile::exists(third));
    QVERIFY(!confirm->isVisible());
    QVERIFY(!item("confirm")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(!QFile::exists(third));
    QVERIFY(!confirm->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(third) < 0, 10000);
  }

  void permanentDeleteReportsPartialFailureWithoutRemovingADirectory() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("delete me.png"));
    const QString second = dir.filePath(QStringLiteral("keep directory.png"));
    QImage picture(200, 150, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(first));
    QVERIFY(picture.save(second));
    m_captures->addExtraFiles({first, second});
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(second) >= 0, 10000);
    // Let the tile's thumbnail load before the file becomes a directory, or
    // its late request logs a warning the final check would count.
    const auto thumbnailSettled = [&] {
      QQuickItem* card = cardFor(second);
      if (!card) return true; // not on screen, so never requested
      QQuickItem* thumbnail = find(card, [](QQuickItem* candidate) {
        return candidate->property("source").toUrl().toString().startsWith(QStringLiteral("image://thumbs/"));
      });
      const int status = thumbnail ? thumbnail->property("status").toInt() : 1;
      return status == 1 || status == 3; // Image.Ready or Image.Error
    };
    QTRY_VERIFY(thumbnailSettled());
    QVERIFY(QMetaObject::invokeMethod(m_window, "requestPermanentDelete",
                                     Q_ARG(QVariant, QVariant(QStringList{first, second}))));
    QTRY_VERIFY(item("permanentConfirm")->isVisible());
    QVERIFY(QFile::remove(second));
    QVERIFY(QDir().mkpath(second));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!QFile::exists(first));
    QVERIFY(QFileInfo(second).isDir());
    QVERIFY(item("detail")->property("status").toString().contains(QStringLiteral("1 of 2")));
  }

  void permanentDeleteFromAnAliasTileRemovesTheLinkAndKeepsTheOriginal() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("original"))));
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("links"))));
    const QString target = dir.filePath(QStringLiteral("original/same.png"));
    const QString alias = dir.filePath(QStringLiteral("links/same.png"));
    QImage picture(200, 150, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(target));
    QVERIFY(QFile::link(target, alias));
    m_captures->addExtraFiles({alias});
    m_captures->setExtraRoot(QFileInfo(target).canonicalPath());
    QTRY_VERIFY_WITH_TIMEOUT(!m_captures->scanning(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(target) >= 0, 10000);
    QCOMPARE(m_library->deletionPathAt(m_library->rowOf(target)), alias);
    m_settings->setConfirmPermanentDelete(false);
    const auto restore = qScopeGuard([&] { m_settings->setConfirmPermanentDelete(true); });
    item("library")->setProperty("currentIndex", m_library->rowOf(target));
    QTest::keyClick(m_window, Qt::Key_Delete, Qt::ShiftModifier);
    QTRY_VERIFY(!QFileInfo(alias).isSymLink());
    QVERIFY(QFile::exists(target));
    QVERIFY(!item("permanentConfirm")->isVisible());
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
    QTest::addColumn<bool>("extensionless");
    QTest::newRow("gif") << QStringLiteral("animated.gif") << false;
    QTest::newRow("webp") << QStringLiteral("animated.webp") << false;
    QTest::newRow("extensionless-gif") << QStringLiteral("animated.gif") << true;
    QTest::newRow("extensionless-webp") << QStringLiteral("animated.webp") << true;
  }

  void realAnimationsPauseResumeAndKeepTheirFrame() {
    QFETCH(QString, name);
    QFETCH(bool, extensionless);
    const QString source = QFINDTESTDATA(qPrintable("fixtures/viewer/" + name));
    QVERIFY(!source.isEmpty());
    const QString path = QFileInfo(m_oddPath).dir().filePath(
        extensionless ? QStringLiteral("without-extension-") + QFileInfo(name).baseName()
                       + (name.endsWith(u"gif") ? QStringLiteral("-gif") : QStringLiteral("-webp"))
                      : name);
    QVERIFY(QFile::copy(source, path));
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
    });
    rescan();
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
    rescan();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    openDetail(m_library->rowOf(path));
    QTRY_COMPARE(item("animatedImage")->property("status").toInt(), 1);
    QCOMPARE(item("animatedImage")->property("frameCount").toInt(), 1);
    QVERIFY(item("detail")->property("playbackError").toString().isEmpty());
  }

  // A camera raw previews from its embedded JPEG, measures as the raw, and
  // offers only the actions that can open it.
  void cancellingTheRawEditorChooserKeepsTheSavedCommand() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    QVERIFY(configureRawEditorRecorder());
    auto* editors = qobject_cast<ExternalEditors*>(m_engine->rootContext()
        ->contextProperty(QStringLiteral("Editors")).value<QObject*>());
    QVERIFY(editors);
    const QString saved = editors->customCommand();
    editors->setCustomCommand(QString());
    const QString raw = m_scratch.filePath(QStringLiteral("cancel-command.dng"));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/camera.dng"), raw));
    QObject* chooser = m_window->findChild<QObject*>(QStringLiteral("editorChooser"));
    QVERIFY(chooser);
    QVERIFY(QMetaObject::invokeMethod(chooser, "request", Q_ARG(QVariant, raw),
                                     Q_ARG(QVariant, true)));
    QTRY_VERIFY(chooser->property("visible").toBool());
    QQuickItem* input = item("editorChooserCustomCommand");
    input->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    typeText(QStringLiteral("unavailable-draft-editor {path}"));
    QCOMPARE(chooser->property("customDraft").toString(), QStringLiteral("unavailable-draft-editor {path}"));
    QCOMPARE(chooser->property("selectedId").toString(), QStringLiteral("custom"));
    QVERIFY(QMetaObject::invokeMethod(chooser, "openSelected"));
    QCOMPARE(editors->customCommand(), QString());
    QVERIFY(chooser->property("visible").toBool());
    QCOMPARE(editors->customCommand(), QString());
    QVERIFY(!item("editorChooserDefault_custom")->isEnabled());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!chooser->property("visible").toBool());
    QCOMPARE(editors->customCommand(), QString());
    QVERIFY(!QFileInfo::exists(raw + QStringLiteral(".editor-open")));
    QVERIFY(QMetaObject::invokeMethod(chooser, "request", Q_ARG(QVariant, raw),
                                     Q_ARG(QVariant, true)));
    QTRY_VERIFY(chooser->property("visible").toBool());
    input->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    typeText(saved);
    QCOMPARE(editors->customCommand(), QString());
    QQuickItem* makeDefault = item("editorChooserDefault_custom");
    QTRY_VERIFY(makeDefault->isEnabled());
    QCOMPARE(chooser->property("hint").toString(), QString());
    click(makeDefault);
    QCOMPARE(editors->customCommand(), saved);
    QCOMPARE(editors->preferredId(), QStringLiteral("custom"));
    QCOMPARE(chooser->property("hint").toString(), QString());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!chooser->property("visible").toBool());
    for (const QString& id : {QStringLiteral("develop"), QStringLiteral("choose-editor")}) {
      perform(id, pathAt(0));
      QCOMPARE(item("detail")->property("status").toString(), QStringLiteral("That action is for camera raws"));
    }
  }

  void rawEditorButtonsRemainClickableAtNormalAndNarrowSizes() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    QVERIFY(configureRawEditorRecorder());
    QObject* chooser = m_window->findChild<QObject*>(QStringLiteral("editorChooser"));
    QVERIFY(chooser);
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(chooser, "close");
      m_window->resize(1280, 820);
    });
    int index = 0;
    for (const QSize& size : {QSize(1280, 820), QSize(560, 420)}) {
      m_window->resize(size);
      const QString raw = m_scratch.filePath(QStringLiteral("footer%1.dng").arg(index++));
      QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/camera.dng"), raw));
      for (const QString& buttonName : {QStringLiteral("editorChooserCancel"),
                                        QStringLiteral("editorChooserOpen")}) {
        QVERIFY(QMetaObject::invokeMethod(chooser, "request", Q_ARG(QVariant, raw),
                                         Q_ARG(QVariant, true)));
        QTRY_VERIFY(chooser->property("visible").toBool());
        QQuickItem* scroller = item("editorChooserScroll");
        // A resize updates popup geometry on the next polish pass. Scroll
        // using the settled viewport instead of the previous window's height.
        waitForLayout(size);
        scroller->setProperty("contentY", qMax(0.0, scroller->property("contentHeight").toDouble()
                                                   - scroller->height()));
        QQuickItem* button = item(buttonName);
        const auto inside = [&] {
          const QRectF rect = button->mapRectToItem(scroller, button->boundingRect());
          return rect.bottom() <= scroller->height() + 1 && rect.left() >= 0 &&
                 rect.right() <= scroller->width() + 1;
        };
        QTRY_VERIFY2(inside(), qPrintable(QStringLiteral("%1 lies outside the chooser").arg(buttonName)));
        click(button);
        QTRY_VERIFY(!chooser->property("visible").toBool());
      }
      QTRY_VERIFY(QFileInfo::exists(raw + QStringLiteral(".editor-open")));
    }
  }

  void rawDetailShortcutsChooseWithoutLaunchingAndOpenThePreferredEditor() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    QVERIFY(configureRawEditorRecorder());
    const QString raw = QFileInfo(m_oddPath).absolutePath() + QStringLiteral("/shortcut.dng");
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/camera.dng"), raw));
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(raw) >= 0, 5000);
    QQuickItem* detail = item("detail");
    QObject* chooser = m_window->findChild<QObject*>(QStringLiteral("editorChooser"));
    QVERIFY(chooser);
    for (Qt::Key key : {Qt::Key_D, Qt::Key_O}) {
      // External actions close detail. Each shortcut must start from the
      // RAW preview again, rather than exercising the grid on the next key.
      openDetail(m_library->rowOf(raw));
      QTRY_VERIFY(detail->isVisible());
      QCOMPARE(detail->property("path").toString(), raw);
      QVERIFY(QMetaObject::invokeMethod(detail, "focusPreview"));
      QTRY_VERIFY(detail->hasActiveFocus());
      QTest::keyClick(m_window, key, Qt::ShiftModifier);
      QTRY_VERIFY(chooser->property("visible").toBool());
      QVERIFY(!QFileInfo::exists(raw + QStringLiteral(".editor-open")));
      QVERIFY(QMetaObject::invokeMethod(chooser, "close"));
      QTRY_VERIFY(!chooser->property("visible").toBool());
    }
    openDetail(m_library->rowOf(raw));
    QTRY_VERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), raw);
    QVERIFY(QMetaObject::invokeMethod(detail, "focusPreview"));
    QTRY_VERIFY(detail->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_O);
    QTRY_VERIFY(QFileInfo::exists(raw + QStringLiteral(".editor-open")));
    QVERIFY(!chooser->property("visible").toBool());
  }

  void rawDetailCompanionActionsRefreshAfterScansAndPairingChanges() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString folder = QFileInfo(m_oddPath).absolutePath();
    const QString raw = folder + QStringLiteral("/live-pair.dng");
    const QString jpeg = folder + QStringLiteral("/live-pair.jpg");
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/camera.dng"), raw));
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(raw) >= 0, 5000);
    openDetail(m_library->rowOf(raw));
    QObject* actions = item("detail")->findChild<QObject*>(QStringLiteral("viewerActions"));
    QVERIFY(actions);
    const int original = actions->property("count").toInt();
    QImage image(48, 64, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(jpeg, "JPEG"));
    rescan();
    QTRY_COMPARE(actions->property("count").toInt(), original + 1);
    m_library->setPairRawJpeg(false);
    QTRY_COMPARE(actions->property("count").toInt(), original);
    m_library->setPairRawJpeg(true);
    QTRY_COMPARE(actions->property("count").toInt(), original + 1);
    QVERIFY(QFile::remove(jpeg));
    rescan();
    QTRY_COMPARE(actions->property("count").toInt(), original);
    QVERIFY(image.save(jpeg, "JPEG"));
    const QString ambiguous = folder + QStringLiteral("/LIVE-PAIR.jpeg");
    QVERIFY(image.save(ambiguous, "JPEG"));
    rescan();
    QTRY_COMPARE(m_captures->companionPathAt(m_captures->rowOf(raw)), QString());
    QTRY_COMPARE(actions->property("count").toInt(), original);
  }

  void theRawEditorChooserBelongsToItsRequestingWindowAndFile() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString first = QFileInfo(m_oddPath).absolutePath() + QStringLiteral("/first.dng");
    const QString second = QFileInfo(m_oddPath).absolutePath() + QStringLiteral("/second.dng");
    const QString fixture = QFINDTESTDATA("fixtures/raw/camera.dng");
    QVERIFY(QFile::copy(fixture, first));
    QVERIFY(QFile::copy(fixture, second));
    QObject* chooser = m_window->findChild<QObject*>(QStringLiteral("editorChooser"));
    QVERIFY(chooser);
    QQmlComponent component(m_engine);
    component.loadFromModule("Omaroll", "EditorChooser");
    std::unique_ptr<QObject> other(component.createWithInitialProperties(
        {{QStringLiteral("parent"), QVariant::fromValue(m_window->contentItem())}},
        m_engine->rootContext()));
    QVERIFY2(other, qPrintable(component.errorString()));
    QVERIFY(QMetaObject::invokeMethod(chooser, "request", Q_ARG(QVariant, first),
                                     Q_ARG(QVariant, true)));
    QTRY_VERIFY(chooser->property("visible").toBool());
    QVERIFY(!other->property("visible").toBool());
    QCOMPARE(chooser->property("path").toString(), first);
    QVERIFY(QMetaObject::invokeMethod(other.get(), "request", Q_ARG(QVariant, second),
                                     Q_ARG(QVariant, true)));
    QTRY_VERIFY(other->property("visible").toBool());
    QCOMPARE(chooser->property("path").toString(), first);
    QCOMPARE(other->property("path").toString(), second);
    QVERIFY(QMetaObject::invokeMethod(other.get(), "close"));
    QVERIFY(QMetaObject::invokeMethod(chooser, "close"));
  }

  void folderSidebarDefaultsTogglesAndPersists() {
    QVERIFY(!m_settings->showFolderSidebar());
    QQuickItem* sidebar = item("folderSidebar");
    QVERIFY(sidebar);
    QVERIFY(!sidebar->isVisible());
    const auto restore = qScopeGuard([&] { m_settings->setShowFolderSidebar(false); });
    QTest::keyClick(m_window, Qt::Key_F9);
    QTRY_VERIFY(sidebar->isVisible());
    QVERIFY(m_settings->showFolderSidebar());
    QVERIFY(AppSettings().showFolderSidebar());
    click(item("folderSidebarToggle"));
    QTRY_VERIFY(!sidebar->isVisible());
    QVERIFY(!AppSettings().showFolderSidebar());
    click(item("folderSidebarToggle"));
    QTRY_VERIFY(sidebar->isVisible());
    QTest::keyClick(m_window, Qt::Key_F9);
    QTRY_VERIFY(!sidebar->isVisible());
  }

  void folderSidebarUsesBrowseFiltersAndAllMedia() {
    const QString folder = QFileInfo(m_oddPath).absolutePath();
    m_settings->setShowFolderSidebar(true);
    QVERIFY(m_settings->pinFolderPath(folder));
    const auto restore = qScopeGuard([&] {
      m_settings->setShowFolderSidebar(false);
      m_settings->unpinFolder(folder);
      m_library->setFolderFilter({});
      m_library->setKindFilter(-1);
      m_library->setSearchText({});
      m_library->setFavoritesOnly(false);
    });
    QTRY_VERIFY(item("folderSidebar")->isVisible());
    QTRY_VERIFY(!item("pinnedFolders")->isVisible());
    m_library->setKindFilter(2);
    m_library->setSearchText("alpine");
    m_library->setFavoritesOnly(true);
    m_library->setAlbumFilter("temporary", {});
    m_library->setDateRange("2026-01-01", "2026-01-31", 0);
    QQuickItem* pin = nullptr;
    QTRY_VERIFY((pin = find(item("folderSidebar"), [&](QQuickItem* candidate) {
      return candidate->activeFocusOnTab() && candidate->property("path").toString() == folder;
    })) != nullptr);
    QTRY_VERIFY(pin->isVisible() && pin->width() > 100 && pin->height() > 20);
    clickSettled(pin);
    QTRY_COMPARE(m_library->folderFilter(), folder);
    QCOMPARE(m_library->kindFilter(), 2);
    QCOMPARE(m_library->searchText(), QStringLiteral("alpine"));
    QVERIFY(m_library->favoritesOnly());
    // Browse facets combine, so a folder narrows the album and date in view.
    QCOMPARE(m_library->albumFilter(), QStringLiteral("temporary"));
    QCOMPARE(m_library->dateFrom(), QStringLiteral("2026-01-01"));
    m_library->setAlbumFilter({}, {});
    m_library->clearDateRange();
    click(item("sidebarAllMedia"));
    QTRY_VERIFY(m_library->folderFilter().isEmpty());
    QCOMPARE(m_library->kindFilter(), 2);
    QCOMPARE(m_library->searchText(), QStringLiteral("alpine"));
    QVERIFY(m_library->favoritesOnly());
    m_settings->setShowFolderSidebar(false);
    QTRY_VERIFY(item("pinnedFolders")->isVisible());
  }

  void folderSidebarAutoHidesWithoutChangingPreference() {
    m_settings->setShowFolderSidebar(true);
    const auto restore = qScopeGuard([&] {
      m_settings->setShowFolderSidebar(false);
      m_window->resize(1280, 820);
    });
    QTRY_VERIFY(item("folderSidebar")->isVisible());
    m_window->resize(759, 600);
    QTRY_VERIFY(!item("folderSidebar")->isVisible());
    QVERIFY(m_settings->showFolderSidebar());
    m_window->resize(560, 420);
    QTRY_VERIFY(!item("folderSidebar")->isVisible());
    QVERIFY(AppSettings().showFolderSidebar());
    m_window->resize(760, 600);
    QTRY_VERIFY(item("folderSidebar")->isVisible());
    QVERIFY(item("library")->width() > 450);
  }

  void folderSidebarResizesAndSavesOnRelease() {
    m_settings->setShowFolderSidebar(true);
    m_settings->setFolderSidebarWidth(240);
    const auto restore = qScopeGuard([&] {
      m_settings->setShowFolderSidebar(false);
      m_settings->setFolderSidebarWidth(240);
    });
    QQuickItem* sidebar = item("folderSidebar");
    QQuickItem* handle = item("folderSidebarResizeHandle");
    QTRY_COMPARE(sidebar->width(), 240.0);
    const QPoint start = centre(handle);
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(m_window, start + QPoint(60, 0));
    QTRY_COMPARE(sidebar->width(), 300.0);
    QCOMPARE(m_settings->folderSidebarWidth(), 240);
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, start + QPoint(60, 0));
    QTRY_COMPARE(m_settings->folderSidebarWidth(), 300);
    QCOMPARE(AppSettings().folderSidebarWidth(), 300);
  }

  void folderSidebarKeyboardExpandsCollapsesAndOpens() {
    m_library->setFolderFilter({});
    const QString folder = QFileInfo(m_oddPath).absolutePath();
    const QVariantList sources{QVariantMap{{"path", folder}, {"label", "Test library"}}};
    // Use an unbound projection: a pending scan or queued live rebuild must
    // never replace the synthetic tree while keyboard input is being tested.
    FolderTreeModel fixture;
    fixture.setFolders(sources, {folder + "/b/leaf", folder + "/bc"}, {});
    m_engine->rootContext()->setContextProperty(QStringLiteral("FolderTree"), &fixture);
    const auto restore = qScopeGuard([&] {
      m_settings->setShowFolderSidebar(false);
      m_library->setFolderFilter({});
      m_engine->rootContext()->setContextProperty(QStringLiteral("FolderTree"), m_folderTree);
    });
    m_settings->setShowFolderSidebar(true);
    QQuickItem* tree = item("folderSidebarTree");
    QTRY_COMPARE(tree->property("rows").toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(item("folderSidebar"), "focusTree"));
    QTRY_VERIFY(tree->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(tree->property("rows").toInt(), 3);
    QTest::keyClick(m_window, Qt::Key_Down);
    QTRY_COMPARE(tree->property("currentRow").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Right);
    QTRY_COMPARE(tree->property("rows").toInt(), 4);
    QTest::keyClick(m_window, Qt::Key_Down);
    QTRY_COMPARE(tree->property("currentRow").toInt(), 2);
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(tree->property("currentRow").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(m_library->folderFilter(), folder + "/b");
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(tree->property("rows").toInt(), 3);
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(tree->property("currentRow").toInt(), 0);
    QTest::keyClick(m_window, Qt::Key_Left);
    QTRY_COMPARE(tree->property("rows").toInt(), 1);
    m_library->setFolderFilter(folder + "/b/leaf");
    QTRY_COMPARE(tree->property("rows").toInt(), 4);
    QTRY_COMPARE(tree->property("currentRow").toInt(), 2);
    QTest::keyClick(m_window, Qt::Key_Up);
    QTRY_COMPARE(tree->property("currentRow").toInt(), 1);
    m_library->setFolderFilter({});
    item("library")->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(tree->hasActiveFocus());
    item("sidebarAddFolder")->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Tab);
    QTRY_VERIFY(item("library")->hasActiveFocus());
  }

  void folderSidebarPreservesExpansionAcrossCountAndStructureChanges() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString branch = dir.filePath("b");
    const QString leaf = dir.filePath("b/leaf");
    const QString current = dir.filePath("bc");
    QVERIFY(QDir().mkpath(leaf));
    QVERIFY(QDir().mkpath(current));
    QImage image(12, 12, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(leaf + "/first.png"));
    QVERIFY(image.save(current + "/current.png"));
    const auto restore = qScopeGuard([&] {
      m_settings->setShowFolderSidebar(false);
      m_library->setFolderFilter({});
      m_settings->removeLibraryFolder(dir.path());
      QTRY_VERIFY(!m_folderTree->indexForPath(dir.path()).isValid());
    });
    QVERIFY(m_settings->addLibraryFolder(QUrl::fromLocalFile(dir.path())));
    QTRY_COMPARE(m_folderTree->data(m_folderTree->indexForPath(leaf), FolderTreeModel::MediaCountRole).toInt(), 1);
    m_library->setFolderFilter(current);
    m_settings->setShowFolderSidebar(true);
    QQuickItem* tree = item("folderSidebarTree");
    QVERIFY(QMetaObject::invokeMethod(tree, "expandToIndex", Q_ARG(QModelIndex, m_folderTree->indexForPath(branch))));
    const auto branchRow = [&] {
      int row = -1;
      QMetaObject::invokeMethod(tree, "rowAtIndex", Q_RETURN_ARG(int, row),
                                Q_ARG(QModelIndex, m_folderTree->indexForPath(branch)));
      return row;
    };
    QTRY_VERIFY(branchRow() >= 0);
    QVERIFY(QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, branchRow())));
    const auto branchExpanded = [&] {
      bool expanded = false;
      QMetaObject::invokeMethod(tree, "isExpanded", Q_RETURN_ARG(bool, expanded), Q_ARG(int, branchRow()));
      return expanded;
    };
    QTRY_VERIFY(branchExpanded());
    QSignalSpy aboutToReset(m_folderTree, &QAbstractItemModel::modelAboutToBeReset);
    QVERIFY(image.save(leaf + "/second.png"));
    rescan();
    QTRY_COMPARE(m_folderTree->data(m_folderTree->indexForPath(leaf), FolderTreeModel::MediaCountRole).toInt(), 2);
    QCOMPARE(aboutToReset.size(), 0);
    QVERIFY(branchExpanded());
    QVERIFY(QDir().mkpath(dir.filePath("a")));
    QVERIFY(image.save(dir.filePath("a/sibling.png")));
    rescan();
    QTRY_VERIFY(m_folderTree->indexForPath(dir.filePath("a")).isValid());
    QCOMPARE(aboutToReset.size(), 1);
    QTRY_VERIFY(branchExpanded());
    QCOMPARE(m_library->folderFilter(), current);
  }

  void folderSidebarShortcutRespectsSearchSheetsAndMenus() {
    QVERIFY(!m_settings->showFolderSidebar());
    QTest::keyClick(m_window, Qt::Key_Slash);
    QTest::keyClick(m_window, Qt::Key_F9);
    QVERIFY(!m_settings->showFolderSidebar());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QObject* browser = m_window->findChild<QObject*>(QStringLiteral("libraryBrowser"));
    QVERIFY(QMetaObject::invokeMethod(browser, "open"));
    QTRY_VERIFY(browser->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_F9);
    QVERIFY(!m_settings->showFolderSidebar());
    QVERIFY(QMetaObject::invokeMethod(browser, "close"));
    m_settings->setShowFolderSidebar(true);
    const auto restore = qScopeGuard([&] {
      QMetaObject::invokeMethod(m_window->findChild<QObject*>(QStringLiteral("sidebarFolderMenu")), "close");
      m_settings->setShowFolderSidebar(false);
    });
    const QString folder = QFileInfo(m_oddPath).absolutePath();
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = find(item("folderSidebar"), [&](QQuickItem* candidate) {
      return candidate->property("path").toString() == folder && candidate->property("folderPath").isValid();
    })) != nullptr);
    click(row, Qt::RightButton);
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("sidebarFolderMenu"));
    QTRY_VERIFY(menu->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_F9);
    QVERIFY(m_settings->showFolderSidebar());
  }

  // Dragged in from another app: files open as they would from the file
  // manager, folders are offered as library folders first, and anything
  // that is not a local file is named.
  void filesAndFoldersDroppedOnTheLibrary() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    const QString picture = root + QStringLiteral("/dropped.jpg");
    const QString first = root + QStringLiteral("/first");
    const QString second = root + QStringLiteral("/second");
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(picture));
    QVERIFY(QDir().mkpath(first));
    QVERIFY(QDir().mkpath(second));
    const auto cleanup = qScopeGuard([this, first, second] {
      m_settings->removeLibraryFolder(first);
      m_settings->removeLibraryFolder(second);
    });
    QSignalSpy opened(m_actions, &ActionLauncher::openRequested);
    QQuickItem* confirm = item("folderDropConfirm");

    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(picture)}, [this] {
      QVERIFY(item("libraryDropHighlight")->property("shown").toBool());
    }));
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.last().first().toStringList(), QStringList{picture});
    QVERIFY(!confirm->isVisible());
    QTRY_VERIFY(!item("libraryDropHighlight")->property("shown").toBool());

    // A folder is a question first, and Escape leaves it out.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(first)}));
    QTRY_VERIFY(confirm->isVisible());
    QCOMPARE(confirm->property("title").toString(), QStringLiteral("Add this folder to Omaroll?"));
    QCOMPARE(opened.size(), 1);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!confirm->isVisible());
    QVERIFY(!m_settings->libraryFolders().contains(first));

    // Mixed: both folders are asked about together and the web address is
    // named. The file opens once that is answered, so the viewer does not
    // come up over the question.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(picture), QUrl::fromLocalFile(first),
                              QUrl::fromLocalFile(second),
                              QUrl(QStringLiteral("https://example.com/cat.png"))}));
    QTRY_VERIFY(confirm->isVisible());
    QCOMPARE(opened.size(), 1);
    QCOMPARE(confirm->property("title").toString(), QStringLiteral("Add these 2 folders to Omaroll?"));
    QCOMPARE(item("footerStatus")->property("text").toString(), QStringLiteral("Skipped: cat.png"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!confirm->isVisible());
    QTRY_COMPARE(opened.size(), 2);
    QCOMPARE(opened.last().first().toStringList(), QStringList{picture});
    QVERIFY(m_settings->libraryFolders().contains(first));
    QVERIFY(m_settings->libraryFolders().contains(second));
    QCOMPARE(item("footerStatus")->property("text").toString(),
             QStringLiteral("Added 2 folders. Omaroll is scanning them now."));

    // Added already: said, not added twice.
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}));
    QTRY_VERIFY(confirm->isVisible());
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!confirm->isVisible());
    QCOMPARE(item("footerStatus")->property("text").toString(),
             QStringLiteral("Those folders are already added, unavailable, or your home folder"));
    QCOMPARE(m_settings->libraryFolders().count(first), 1);
    QTRY_VERIFY(!m_captures->scanning());
  }

  // A linked folder is added as the folder it links to, and the library
  // then shows that folder rather than an empty view of the link's name.
  void droppedLinkedFolderShowsTheFolderItAdds() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    const QString real = root + QStringLiteral("/real");
    const QString link = root + QStringLiteral("/link");
    QVERIFY(QDir().mkpath(real));
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkMagenta);
    QVERIFY(image.save(real + QStringLiteral("/inside.png")));
    QVERIFY(QFile::link(real, link));
    const auto cleanup = qScopeGuard([this, real] {
      m_settings->removeLibraryFolder(real);
      QMetaObject::invokeMethod(m_window, "showAllMedia", Q_ARG(QVariant, QString()));
    });
    QQuickItem* confirm = item("folderDropConfirm");
    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(link)}));
    QTRY_VERIFY(confirm->isVisible());
    QVERIFY2(confirm->property("detail").toString().startsWith(real),
             qPrintable(confirm->property("detail").toString()));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!confirm->isVisible());
    QVERIFY(m_settings->libraryFolders().contains(real));
    QTRY_COMPARE(m_library->folderFilter(), real);
    // A scan of the new folder, which can be slow beside a busy suite.
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowCount(), 1, 20000);
    QTRY_VERIFY(!m_captures->scanning());
  }

  // Many skipped names are counted, not listed without end.
  void droppedSkippedNamesAreCapped() {
    QList<QUrl> urls;
    for (int i = 1; i <= 9; ++i) urls.append(QUrl(QStringLiteral("https://example.com/%1.png").arg(i)));
    QVERIFY(dropOn(m_window, urls));
    QTRY_COMPARE(item("footerStatus")->property("text").toString(),
                 QStringLiteral("Skipped: 1.png, 2.png, 3.png, 4.png, 5.png and 4 more"));
  }

  // Under an open menu or the detail view, a drop does not land behind it.
  void dropsWaitForMenusAndTheDetailView() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString picture = QFileInfo(dir.path()).canonicalFilePath() + QStringLiteral("/over.png");
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkYellow);
    QVERIFY(image.save(picture));
    QSignalSpy opened(m_actions, &ActionLauncher::openRequested);
    QObject* menu = m_window->findChild<QObject*>(QStringLiteral("libraryContextMenu"));
    QVERIFY(QMetaObject::invokeMethod(m_window, "openContextMenu", Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, 100), Q_ARG(QVariant, 100)));
    QTRY_VERIFY(menu->property("visible").toBool());
    dropOn(m_window, {QUrl::fromLocalFile(picture)});
    QTest::qWait(50);
    QCOMPARE(opened.size(), 0);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());

    openDetail(0);
    QTRY_VERIFY(item("detail")->isVisible());
    dropOn(m_window, {QUrl::fromLocalFile(picture)});
    QTest::qWait(50);
    QCOMPARE(opened.size(), 0);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!item("detail")->isVisible());

    QVERIFY(dropOn(m_window, {QUrl::fromLocalFile(picture)}));
    QTRY_COMPARE(opened.size(), 1);
  }

  void pinnedFoldersRemainShortcutsWithoutRemovingTheirSource() {
    QVERIFY(m_settings->pinFolderPath(QFileInfo(m_oddPath).absolutePath()));
    QQuickItem* pins = item("pinnedFolders");
    QTRY_VERIFY(pins->isVisible());
    const QString folder = m_settings->pinnedFolders().first();
    QVERIFY(QMetaObject::invokeMethod(pins, "chosen", Q_ARG(QString, folder)));
    QCOMPARE(m_library->folderFilter(), folder);
    m_settings->unpinFolder(folder);
    QTRY_VERIFY(!pins->isVisible());
    QVERIFY(m_settings->libraryFolders().contains(folder));
    QCOMPARE(m_library->folderFilter(), folder);
  }

  void theLibraryCompanionActionOpensTheRequestedOriginal() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString folder = QFileInfo(m_oddPath).absolutePath();
    const QString raw = folder + QStringLiteral("/paired.dng");
    const QString jpeg = folder + QStringLiteral("/paired.jpg");
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/raw/camera.dng"), raw));
    QImage image(48, 64, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(jpeg, "JPEG"));
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(m_library->rowOf(raw) >= 0, 5000);
    QTRY_COMPARE(m_library->rowOf(jpeg), -1);
    openDetail(m_library->rowOf(raw));
    QVERIFY(QMetaObject::invokeMethod(m_window, "perform", Q_ARG(QVariant, "companion"),
                                     Q_ARG(QVariant, raw), Q_ARG(QVariant, false)));
    QQuickItem* detail = item("detail");
    QTRY_COMPARE(detail->property("path").toString(), jpeg);
    QVERIFY(m_library->rowOf(jpeg) >= 0);
    QCOMPARE(m_library->rowOf(raw), -1);
    QVERIFY(QMetaObject::invokeMethod(m_window, "perform", Q_ARG(QVariant, "companion"),
                                     Q_ARG(QVariant, jpeg), Q_ARG(QVariant, false)));
    QTRY_COMPARE(detail->property("path").toString(), raw);
    QVERIFY(QFileInfo::exists(raw) && QFileInfo::exists(jpeg));
  }

  void aCameraRawPreviewsAtItsOwnSizeWithItsOwnActions() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) {
      QSKIP("Qt has no camera raw decoder here (kimageformats with LibRaw)");
    }
    const QString source = QFINDTESTDATA("fixtures/raw/camera.dng");
    QVERIFY(!source.isEmpty());
    const QString path = QFileInfo(m_oddPath).dir().filePath(QStringLiteral("DSC00042.DNG"));
    QVERIFY(QFile::copy(source, path));
    m_disposablePaths.append(path);
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      rescan();
    });
    rescan();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    QTRY_VERIFY_WITH_TIMEOUT(cardFor(path) != nullptr, 5000);
    QCOMPARE(cardFor(path)->property("rawFormat").toString(), QStringLiteral("DNG"));
    QQuickItem* badge = cardFor(path)->findChild<QQuickItem*>(QStringLiteral("rawBadge"));
    QVERIFY(badge);
    QVERIFY(badge->isVisible());
    openDetail(m_library->rowOf(path));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY_WITH_TIMEOUT(detail->property("imageReady").toBool(), 10000);
    QVERIFY(detail->property("playbackError").toString().isEmpty());
    // The preview is 24x32 once upright; the raw is 48x64.
    QTRY_COMPARE(detail->property("mediaWidth").toInt(), 48);
    QTRY_COMPARE(detail->property("mediaHeight").toInt(), 64);
    // A late size result for another version of this path must be ignored.
    m_captures->rawSizeRead(path, QUrl(QStringLiteral("image://raw/old-version")),
                           QSize(480, 640));
    QCOMPARE(detail->property("mediaWidth").toInt(), 48);
    QCOMPARE(detail->property("mediaHeight").toInt(), 64);
    QVariant rows;
    QVERIFY(QMetaObject::invokeMethod(detail, "visibleActions", Q_RETURN_ARG(QVariant, rows)));
    QStringList ids;
    for (const QVariant& row : rows.toList()) {
      ids << row.toMap().value(QStringLiteral("id")).toString();
    }
    QVERIFY(ids.contains(QStringLiteral("develop")));
    QVERIFY(ids.contains(QStringLiteral("matte")));
    QVERIFY(ids.contains(QStringLiteral("export")));
    QVERIFY(!ids.contains(QStringLiteral("corrections")));
    QVERIFY(!ids.contains(QStringLiteral("annotate")));
    QVERIFY(!ids.contains(QStringLiteral("print")));
    // Asked for anyway, from the grid's shortcut, it declines with a reason.
    QVERIFY(QMetaObject::invokeMethod(m_window, "perform", Q_ARG(QVariant, "corrections"),
                                      Q_ARG(QVariant, path), Q_ARG(QVariant, QVariant())));
    QTRY_COMPARE(detail->property("status").toString(),
                 QStringLiteral("That one cannot open camera raws"));
    QVERIFY(!item("correctionSheet")->isVisible());
  }

  void anExtensionlessLibraryPreviewFollowsAChangeOfMedium() {
    const QString path = QFileInfo(m_oddPath).dir().filePath(QStringLiteral("same-name-media"));
    // Replaced under the tiles and the open preview, which may still ask.
    m_disposablePaths.append(path);
    const auto cleanup = qScopeGuard([&] {
      invoke("dismissTopLayer");
      QFile::remove(path);
      rescan();
    });
    const QString still = QFINDTESTDATA("fixtures/viewer/transparent.png");
    const QString video = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    QVERIFY(QFile::copy(still, path));
    rescan();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    openDetail(m_library->rowOf(path));
    QQuickItem* detail = item("detail");
    QTRY_VERIFY(detail->property("imageReady").toBool());
    QVERIFY(!detail->property("isVideo").toBool());
    QVERIFY(QFile::remove(path));
    QVERIFY(QFile::copy(video, path));
    rescan();
    QTRY_VERIFY(detail->property("isVideo").toBool());
    QVERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), path);
    QVERIFY(QFile::remove(path));
    QVERIFY(QFile::copy(still, path));
    rescan();
    QTRY_VERIFY(!detail->property("isVideo").toBool());
    QTRY_VERIFY(detail->property("imageReady").toBool());
    QVERIFY(detail->isVisible());
  }

  void previewSeekBarShowsStoryboardFrames() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
      QSKIP("ffmpeg is not installed");
    }
    const QString path = QFileInfo(m_oddPath).dir().filePath(QStringLiteral("storyboard.mkv"));
    QVERIFY(!makeStoryboardVideo(path).isEmpty());
    const auto cleanup = qScopeGuard([&] {
      m_window->resize(1280, 820);
      invoke("dismissTopLayer");
      QFile::remove(path);
      rescan();
    });
    rescan();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    openDetail(m_library->rowOf(path));
    auto* player = m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer"));
    QVERIFY(player);
    QTRY_VERIFY(player->duration() > 0);
    auto* storyboard = m_window->findChild<Storyboard*>(QStringLiteral("videoStoryboard"));
    QVERIFY(storyboard);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard->complete(), 30000);
    QVERIFY(storyboard->frameCount() >= 2);
    player->pause();

    QQuickItem* seek = item("videoSeek");
    QQuickItem* preview = item("videoSeekPreview");
    QVERIFY(!preview->isVisible());
    QQuickItem* frame = item("seekPreviewFrame");
    QQuickItem* time = item("seekPreviewTime");
    const auto along = [seek](qreal fraction) {
      return seek->mapToScene(QPointF(seek->width() * fraction, seek->height() / 2)).toPoint();
    };
    QTest::mouseMove(m_window, along(0.5));
    QTRY_VERIFY(preview->isVisible());
    QTRY_COMPARE(frame->property("status").toInt(), 1); // Image.Ready
    QCOMPARE(time->property("text").toString(), QStringLiteral("0:06"));
    const QRectF frameBounds = frame->mapRectToScene(frame->boundingRect());
    const QRectF timeBounds = time->mapRectToScene(time->boundingRect());
    QVERIFY(frameBounds.bottom() <= timeBounds.top());
    QVERIFY(preview->mapRectToScene(preview->boundingRect()).bottom() < seek->mapToScene(QPointF()).y());
    const QString middle = frame->property("source").toString();
    QTest::mouseMove(m_window, along(0.95));
    QTRY_VERIFY(frame->property("source").toString() != middle);

    // A tap or a key seeks too, and holds new decodes for a moment, from
    // the press on.
    QVERIFY(!storyboard->paused());
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, along(0.25));
    QVERIFY(storyboard->paused());
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, along(0.25));
    QVERIFY(storyboard->paused());
    QTRY_VERIFY_WITH_TIMEOUT(!storyboard->paused(), 3000);
    QTest::keyClick(m_window, Qt::Key_Home);
    QVERIFY(storyboard->paused());
    QTRY_COMPARE(player->position(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!storyboard->paused(), 3000);

    // Behind another window, such as a quick viewer, the sheet leaves the
    // decoder to it.
    QVERIFY(storyboard->active());
    {
      QQuickWindow other;
      other.resize(200, 200);
      other.show();
      other.requestActivate();
      QTRY_VERIFY(other.isActive());
      QTRY_VERIFY(!storyboard->active());
    }
    m_window->requestActivate();
    QTRY_VERIFY(m_window->isActive());
    QTRY_VERIFY(storyboard->active());

    // Minimized, it decodes nothing either.
    m_window->showMinimized();
    QTRY_VERIFY(!storyboard->active());
    m_window->showNormal();
    m_window->requestActivate();
    QTRY_VERIFY(m_window->isActive());
    QTRY_VERIFY(storyboard->active());

    // In a narrow window the preview stays inside it.
    m_window->resize(560, 420);
    QTRY_VERIFY(seek->isVisible() && seek->width() > 0
                && seek->mapToScene(QPointF(seek->width(), 0)).x() <= m_window->width());
    QTest::mouseMove(m_window, along(0.99));
    QTRY_VERIFY(preview->isVisible());
    const QRectF narrow = preview->mapRectToScene(preview->boundingRect());
    QVERIFY(narrow.left() >= 0 && narrow.right() <= m_window->width());
    QVERIFY(narrow.top() >= 0);
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
    rescan();
    QTRY_VERIFY(m_library->rowOf(path) >= 0);
    m_settings->setVideoPosition(path, 7000);
    openDetail(m_library->rowOf(path));
    auto* player = m_window->findChild<QMediaPlayer*>(QStringLiteral("videoPlayer"));
    QVERIFY(player);
    QTRY_VERIFY(item("detail")->property("resumeAvailable").toBool());
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    // Offering to resume must not start playback, not even for a moment.
    QSignalSpy resumeStates(player, &QMediaPlayer::playbackStateChanged);
    QTRY_VERIFY(player->mediaStatus() == QMediaPlayer::LoadedMedia ||
                player->mediaStatus() == QMediaPlayer::BufferedMedia);
    QTest::qWait(200); // Checks that nothing starts, so there is no event to wait for.
    QVERIFY(!playedDuring(resumeStates));
    QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
    auto* mpris = qobject_cast<MprisService*>(
        m_engine->rootContext()->contextProperty(QStringLiteral("Mpris")).value<QObject*>());
    QVERIFY(mpris);
    mpris->play();
    QTRY_VERIFY(player->position() >= 6500);
    QVERIFY(!item("detail")->property("resumeAvailable").toBool());
    QVERIFY(!item("detail")->property("resumePending").toBool());
    mpris->pause();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    item("detail")->setProperty("resumeAvailable", true);
    item("detail")->setProperty("resumePending", true);
    mpris->setPosition(mpris->metadata().value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>(),
                       1000000);
    QVERIFY(!item("detail")->property("resumeAvailable").toBool());
    QVERIFY(!item("detail")->property("resumePending").toBool());
    QTRY_COMPARE(player->position(), 1001); // Enter the frame at the exact boundary.
    item("detail")->setProperty("resumeAvailable", true);
    click(item("resumeButton"));
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    player->setPosition(0);
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
    click(item("audioTrackButton"));
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
    if (qEnvironmentVariableIsSet("OMAROLL_REQUIRE_OPENGL")) {
      player->pause();
      QTest::qWait(150);
      auto* overlay = item("subtitleOverlay");
      QTRY_VERIFY(overlay->isVisible());
      const auto capture = [&] {
        auto* output = item("videoOutput");
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
      QVERIFY(themed != embeddedOnly);
      input->setSubtitleText({});
      QTest::qWait(100);
      const QImage withoutCaptions = capture();
      if (withoutCaptions != embeddedOnly && qEnvironmentVariableIsSet("OMAROLL_PIXEL_EVIDENCE")) {
        const QString dir = qEnvironmentVariable("OMAROLL_PIXEL_EVIDENCE");
        embeddedOnly.save(dir + "/embedded-only.png");
        withoutCaptions.save(dir + "/without-captions.png");
      }
      QCOMPARE(withoutCaptions, embeddedOnly);
      overlay->setVisible(true);
    }
    player->play();
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
    // Restoring the window must not resume a clip the user had paused.
    QSignalSpy restoreStates(player, &QMediaPlayer::playbackStateChanged);
    m_window->showMinimized();
    QTRY_COMPARE(m_window->visibility(), QWindow::Minimized);
    m_window->showNormal();
    QTRY_VERIFY(m_window->visibility() != QWindow::Minimized);
    QTRY_VERIFY(player->mediaStatus() == QMediaPlayer::LoadedMedia ||
                player->mediaStatus() == QMediaPlayer::BufferedMedia);
    QTest::qWait(200); // Checks that nothing starts, so there is no event to wait for.
    QVERIFY(!playedDuring(restoreStates));
    QCOMPARE(player->playbackState(), QMediaPlayer::PausedState);
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    m_window->showMinimized();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
    m_window->showNormal();
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    // A clip that reaches its end holds its first frame, paused, instead of
    // stopping on a blank stage; Space then plays it again from the start.
    player->setPosition(player->duration() - 300);
    QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PausedState, 5000);
    QTRY_VERIFY(player->position() < 500);
    QVERIFY(player->hasVideo());
    QVERIFY(item("videoOutput")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    QTRY_VERIFY(player->position() > 0 && player->position() < 3000);
    // Basic controls must remain within the narrow window.
    m_window->resize(560, 420);
    waitForLayout(QSize(560, 420));
    for (const QString& name : {QStringLiteral("videoPlayButton"),
                                QStringLiteral("videoSoundButton"),
                                QStringLiteral("playbackSpeedButton")}) {
      QQuickItem* control = item(name);
      QVERIFY(control->isVisible());
      // The transport row eases into its narrow arrangement after the layout
      // pass, so wait for the control to come to rest inside the window.
      const auto inside = [&] {
        const QRectF bounds = control->mapRectToScene(control->boundingRect());
        return bounds.left() >= 0 && bounds.right() <= m_window->width();
      };
      QTRY_VERIFY2(inside(), qPrintable(name));
    }
    // The inspector leaves much less room than the closed preview. Seeking
    // must still have a useful target rather than a negative-width hit area.
    item("detail")->setProperty("showInfo", true);
    QQuickItem* seek = item("videoSeek");
    QTRY_VERIFY(seek->isVisible() && seek->width() >= 60);
    const QRectF seekBounds = seek->mapRectToScene(seek->boundingRect());
    for (const QString& name : {QStringLiteral("videoPlayButton"),
                                QStringLiteral("videoSoundButton"),
                                QStringLiteral("playbackSpeedButton")}) {
      QQuickItem* control = item(name);
      const QRectF bounds = control->mapRectToScene(control->boundingRect());
      QVERIFY(bounds.left() >= 0 && bounds.right() <= m_window->width());
      QVERIFY(!bounds.intersects(seekBounds));
    }
    player->pause();
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                     seek->mapToScene(QPointF(seek->width() * 0.75, seek->height() / 2)).toPoint());
    QTRY_VERIFY(player->position() > player->duration() / 2);
    // A saved spot belongs below the picture, including with the inspector
    // open at minimum size. Slideshow is absent from ordinary video playback.
    QQuickItem* detail = item("detail");
    QVERIFY(!item("viewerSlideshowButton")->isVisible());
    detail->setProperty("resumePosition", 7000);
    detail->setProperty("resumeAvailable", true);
    for (const QSize size : {QSize(560, 420), QSize(1280, 820)}) {
      m_window->resize(size);
      settle();
      QQuickItem* prompt = item("resumePrompt");
      const QRectF bounds = prompt->mapRectToScene(prompt->boundingRect());
      QQuickItem* output = item("videoOutput");
      QVERIFY(prompt->isVisible());
      QVERIFY(bounds.left() >= 0 && bounds.right() <= output->mapToScene(QPointF(output->width(), 0)).x());
      QVERIFY(bounds.top() >= output->mapToScene(QPointF(0, output->height())).y());
      QVERIFY(bounds.bottom() <= seek->mapToScene(QPointF()).y());
      for (const QString& name : {QStringLiteral("resumeButton"), QStringLiteral("restartVideoButton")}) {
        QQuickItem* button = item(name);
        const QRectF buttonBounds = button->mapRectToScene(button->boundingRect());
        QVERIFY(bounds.contains(buttonBounds));
      }
    }
    click(item("resumeButton"));
    QTRY_VERIFY(!detail->property("resumeAvailable").toBool());
    QTRY_VERIFY(player->position() >= 6900 && player->playbackState() == QMediaPlayer::PlayingState);
    player->pause();
    detail->setProperty("resumeAvailable", true);
    click(item("restartVideoButton"));
    QTRY_VERIFY(!detail->property("resumeAvailable").toBool());
    QTRY_VERIFY(player->position() < 2000 && player->playbackState() == QMediaPlayer::PlayingState);
    player->pause();
    detail->setProperty("resumeAvailable", true);
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(!detail->property("resumeAvailable").toBool());
    player->pause();
    detail->setProperty("resumeAvailable", true);
    QTest::keyClick(m_window, Qt::Key_L);
    QTRY_VERIFY(!detail->property("resumeAvailable").toBool());
    detail->setProperty("resumeAvailable", true);
    click(seek);
    QTRY_VERIFY(!detail->property("resumeAvailable").toBool());
    detail->setProperty("resumeAvailable", true);
    QTest::keyClick(m_window, Qt::Key_F5);
    QTRY_VERIFY(detail->property("slideshowRunning").toBool());
    QVERIFY(!detail->property("resumeAvailable").toBool());
    QTRY_COMPARE(player->playbackState(), QMediaPlayer::PlayingState);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!detail->property("slideshowRunning").toBool());
    QVERIFY(item("detail")->property("playbackError").toString().isEmpty());
  }

  // A tiled window changes width, which rebuilds the grid layout, and new
  // captures keep arriving while it is open. Every visible tile must still
  // describe the row it sits at, and opening it must open that file.
  void tilesStayMappedAfterResizeAndRescan_data() {
    QTest::addColumn<bool>("sectioned");
    QTest::newRow("date sections") << true;
    // Unrated, the rating sort keeps newest first in one flat grid.
    QTest::newRow("flat grid") << false;
  }

  void tilesStayMappedAfterResizeAndRescan() {
    QFETCH(bool, sectioned);
    if (!sectioned) m_library->setSortMode(CaptureFilterModel::RatingFirst);
    const auto restoreSort = qScopeGuard([&] { m_library->setSortMode(CaptureFilterModel::NewestFirst); });
    auto* sectionRows = qobject_cast<SectionRows*>(m_library->sectionRows());
    QTRY_VERIFY(item(sectioned ? QStringLiteral("reviewSets") : QStringLiteral("flatGrid"))->isVisible());
    // The visible view: the flat GridView or the sections' ListView.
    const auto liveView = [&]() -> QQuickItem* {
      return item(sectioned ? QStringLiteral("reviewSetsList") : QStringLiteral("flatGrid"));
    };
    // The list or grid delegate a card sits in, and the library row it shows.
    const auto delegateOf = [&](QQuickItem* card, int* row) -> QQuickItem* {
      QQuickItem* cell = card->parentItem();
      if (!cell) return nullptr;
      if (!sectioned) {
        *row = cell->property("index").toInt();
        return cell;
      }
      *row = cell->property("row").toInt();
      return cell->parentItem() ? cell->parentItem()->parentItem() : nullptr;
    };
    m_settings->setShowGridFilenames(true);
    const auto restoreFilenames = qScopeGuard([&] { m_settings->setShowGridFilenames(false); });
    QQuickItem* grid = item("library");
    QQuickItem* detail = item("detail");
    // Earlier tests delete explicit files while their last scan is still in
    // flight. Count a fresh disk snapshot, not those departing rows.
    rescan();
    QTRY_VERIFY_WITH_TIMEOUT(!m_captures->scanning() && [&] {
      for (int row = 0; row < m_captures->rowCount(); ++row) {
        if (!QFileInfo::exists(m_captures->pathAt(row))) return false;
      }
      return true;
    }(), 15000);
    const QString pictures = QFileInfo(m_oddPath).absolutePath();
    const QString source = pictures + QStringLiteral("/alpine-dawn.jpg");
    QStringList added;
    auto cleanup = qScopeGuard([&] {
      for (const QString& path : added) {
        QFile::remove(path);
      }
      rescan();
      const bool removed = QTest::qWaitFor([&] {
        return std::all_of(added.cbegin(), added.cend(), [&](const QString& path) {
          return m_captures->rowOf(path) < 0;
        });
      }, 15000);
      m_window->resize(1280, 820);
      waitForLayout(QSize(1280, 820));
      QVERIFY(removed);
    });

    struct Stale {
      QQuickItem* card;
      int index;
      QString path;
    };
    QList<Stale> stale;
    auto checkCards = [&](const char* stage) {
      QTest::qWait(400);
      stale.clear();
      int checked = 0;
      QHash<QPoint, QString> positions;
      QQuickItem* view = liveView();
      QVERIFY(view);
      QObject* relayout = grid->findChild<QObject*>(QStringLiteral("gridRelayoutTimer"));
      QVERIFY(relayout);
      QTRY_VERIFY(!relayout->property("running").toBool()
                  && grid->property("layoutReady").toBool()
                  && view->property("reuseItems").toBool());
      view->ensurePolished();
      std::function<void(QQuickItem*)> walk = [&](QQuickItem* node) {
        for (QQuickItem* child : node->childItems()) {
          if (child->property("dragPaths").isValid() && child->isVisible() &&
              child->parentItem() && child->parentItem()->property("index").isValid()) {
            int index = -1;
            QQuickItem* owner = delegateOf(child, &index);
            const int viewIndex = owner ? owner->property("index").toInt() : -1;
            QQuickItem* live = nullptr;
            QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, live),
                                      Q_ARG(int, viewIndex));
            // Pooled delegates can retain visible=true and old positions,
            // but are not the tiles the view paints or sends input to.
            if (!owner || live != owner) continue;
            const QString path = child->property("path").toString();
            if (path != m_library->pathAt(index)) {
              const QPointF at = child->mapToScene(QPointF(child->width() / 2, child->height() / 2));
              qInfo() << stage << "tile" << index << "shows" << path << "but the row holds"
                      << m_library->pathAt(index) << "at" << at << "live delegate:"
                      << (live == owner);
              stale.append({child, index, path});
              ++checked;
              walk(child);
              continue;
            }
            const QPoint at = child->mapToScene(QPointF(0, 0)).toPoint();
            QVERIFY2(!positions.contains(at) || positions.value(at) == path,
                     qPrintable(QStringLiteral("%1: %2 and %3 overlap at %4,%5")
                                    .arg(QLatin1String(stage), positions.value(at), path)
                                    .arg(at.x())
                                    .arg(at.y())));
            positions.insert(at, path);
            QQuickItem* image = find(child, [](QQuickItem* candidate) {
              return candidate->property("sourceSize").isValid() &&
                     candidate->property("source").isValid();
            });
            if (image) {
              const QString url =
                  QUrl::fromPercentEncoding(image->property("source").toUrl().toString().toUtf8());
              QVERIFY2(url.isEmpty() || url.contains(path),
                       qPrintable(QStringLiteral("%1: tile %2 requests %3 for %4")
                                      .arg(QLatin1String(stage))
                                      .arg(index)
                                      .arg(url, path)));
            }
            ++checked;
          }
          walk(child);
        }
      };
      walk(m_window->contentItem());
      QVERIFY2(stale.isEmpty(),
               qPrintable(QStringLiteral("%1: %2 tile(s) show a file their row no longer holds")
                              .arg(QLatin1String(stage))
                              .arg(stale.size())));
      QVERIFY2(checked >= 4, qPrintable(QStringLiteral("%1: only %2 tiles visible")
                                            .arg(QLatin1String(stage))
                                            .arg(checked)));
    };

    // The delegate the view itself shows for a row. cardFor() can answer a
    // pooled delegate that still reports itself visible but takes no clicks.
    auto viewIndexOf = [&](int row) {
      return sectioned ? sectionRows->rowOfItem(row) : row;
    };
    auto liveCardAt = [&](int row) -> QQuickItem* {
      QQuickItem* view = liveView();
      QQuickItem* cell = nullptr;
      QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell),
                                Q_ARG(int, viewIndexOf(row)));
      const QString path = pathAt(row);
      return cell ? find(cell, [&path](QQuickItem* candidate) {
        return candidate->property("dragPaths").isValid() && candidate->property("path").toString() == path;
      }) : nullptr;
    };
    auto openByRightClick = [&](int row, const char* stage) {
      QQuickItem* view = liveView();
      const QString expectedPath = pathAt(row);
      QSignalSpy frames(m_window, &QQuickWindow::afterAnimating);
      // Position first: an offscreen row may not have a live delegate yet.
      // GridView.Contain and ListView.Contain
      QTRY_VERIFY(viewIndexOf(row) >= 0);
      QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, viewIndexOf(row)), Q_ARG(int, 4));
      m_window->update();
      QTRY_VERIFY(!frames.isEmpty());
      QMetaObject::invokeMethod(view, "forceLayout");
      view->ensurePolished();
      QQuickItem* card = nullptr;
      QTRY_VERIFY_WITH_TIMEOUT((card = liveCardAt(row)) != nullptr, 5000);
      QCOMPARE(card->property("path").toString(), expectedPath);
      // Relayout may destroy the delegate while the wait processes events.
      QTRY_VERIFY_WITH_TIMEOUT((card = liveCardAt(row)) != nullptr
                                   && !view->property("moving").toBool()
                                   && view->mapRectToScene(view->boundingRect()).contains(centre(card)),
                              5000);
      const QPoint clickPosition = centre(card);
      const QSizeF cardSize = card->size();
      QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, clickPosition);
      QObject* menu = m_window->findChild<QObject*>(QStringLiteral("libraryContextMenu"));
      QTRY_VERIFY(menu && menu->property("visible").toBool());
      const QStringList targets = menu->property("targets").value<QJSValue>().toVariant().toStringList();
      QCOMPARE(targets, QStringList{expectedPath});
      QTest::keyClick(m_window, Qt::Key_Escape);
      QTRY_VERIFY(!menu->property("visible").toBool());
      QTRY_COMPARE(pathAt(grid->property("currentIndex").toInt()), expectedPath);
      QTest::keyClick(m_window, Qt::Key_Return);
      settle();
      QTRY_VERIFY2_WITH_TIMEOUT(
          detail->isVisible(),
          qPrintable(QStringLiteral("%1: right click on row %2 at %3,%4 (card %5x%6, window %7x%8, "
                                    "grid enabled %9, sheet open %10, popup %11) opened nothing")
                         .arg(QLatin1String(stage))
                         .arg(row)
                         .arg(clickPosition.x())
                         .arg(clickPosition.y())
                         .arg(cardSize.width())
                         .arg(cardSize.height())
                         .arg(m_window->width())
                         .arg(m_window->height())
                         .arg(grid->isEnabled())
                         .arg(prop("anySheetOpen").toBool())
                         .arg(prop("popupOpen").toBool())),
          5000);
      QVERIFY2(detail->property("path").toString() == expectedPath,
               qPrintable(QStringLiteral("%1: clicked %2, viewer opened %3")
                              .arg(QLatin1String(stage), expectedPath,
                                   detail->property("path").toString())));
      invoke("dismissTopLayer");
      QTRY_VERIFY(!detail->isVisible());
    };

    // Tiled beside another window: fewer columns, layout rebuilt.
    m_window->resize(1005, 545);
    checkCards("after narrowing");
    if (QTest::currentTestFailed()) return;
    openByRightClick(2, "after narrowing");
    if (QTest::currentTestFailed()) return;

    // New screenshots land while the window is open and sort to the top.
    const int before = m_library->rowCount();
    for (int i = 1; i <= 3; ++i) {
      const QString target =
          pictures + QStringLiteral("/screenshot-2099-01-0%1_10-00-00.jpg").arg(i);
      QVERIFY(QFile::copy(source, target));
      added.append(QFileInfo(target).canonicalFilePath());
      m_disposablePaths.append(added.last());
    }
    rescan();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowCount(), before + 3, 15000);
    checkCards("after rescan");
    if (QTest::currentTestFailed()) return;
    openByRightClick(0, "after rescan");
    if (QTest::currentTestFailed()) return;
    openByRightClick(4, "after rescan");
    if (QTest::currentTestFailed()) return;

    // Back to a wide window, more columns.
    m_window->resize(1280, 820);
    checkCards("after widening");
    if (QTest::currentTestFailed()) return;
    openByRightClick(3, "after widening");
    if (QTest::currentTestFailed()) return;

    // Background metadata moves a row to the top, re-sorting in place.
    int movable = -1;
    for (int candidate = 2; candidate < m_library->rowCount(); ++candidate) {
      const int sourceRow = m_captures->rowOf(pathAt(candidate));
      if (sourceRow >= 0 && !m_captures->recordAt(sourceRow).hasProducerTimestamp) {
        movable = candidate;
        break;
      }
    }
    QVERIFY(movable > 0);
    const QString movedPath = pathAt(movable);
    const CaptureRecord movedRecord = m_captures->recordAt(m_captures->rowOf(movedPath));
    const auto restoreDate = qScopeGuard([&] {
      m_captures->applyMetadata({{movedPath, movedRecord.modified, movedRecord.bytes,
                                       movedRecord.captured, movedRecord.device,
                                       movedRecord.inode}});
    });
    m_captures->applyMetadata({{movedPath, movedRecord.modified, movedRecord.bytes,
                                     QDateTime::currentDateTime().addYears(80), movedRecord.device,
                                     movedRecord.inode}});
    QTRY_COMPARE(m_library->rowOf(movedPath), 0);
    checkCards("after date reorder");
    if (QTest::currentTestFailed()) return;
    openByRightClick(0, "after date reorder");
    if (QTest::currentTestFailed()) return;
    openByRightClick(movable, "after date reorder");
    if (QTest::currentTestFailed()) return;

    // Ctrl+wheel tile size change.
    const int tileWidth = m_settings->tileWidth();
    const auto restoreTiles = qScopeGuard([&] { m_settings->setTileWidth(tileWidth); });
    m_settings->setTileWidth(tileWidth + 120);
    checkCards("after larger tiles");
    if (QTest::currentTestFailed()) return;
    openByRightClick(1, "after larger tiles");
    if (QTest::currentTestFailed()) return;

    // Scroll to the end, let the library change while the top rows are
    // pooled, then scroll back so those pooled delegates are reused.
    QQuickItem* view = liveView();
    QVERIFY(view);
    QMetaObject::invokeMethod(view, "positionViewAtEnd");
    QTest::qWait(300);
    checkCards("after scrolling down");
    if (QTest::currentTestFailed()) return;
    const int beforeMore = m_library->rowCount();
    for (int i = 4; i <= 6; ++i) {
      const QString target =
          pictures + QStringLiteral("/screenshot-2099-01-0%1_10-00-00.jpg").arg(i);
      QVERIFY(QFile::copy(source, target));
      added.append(QFileInfo(target).canonicalFilePath());
      m_disposablePaths.append(added.last());
    }
    rescan();
    QTRY_COMPARE_WITH_TIMEOUT(m_library->rowCount(), beforeMore + 3, 15000);
    QMetaObject::invokeMethod(view, "positionViewAtBeginning");
    QTRY_COMPARE(view->property("contentY").toReal(), view->property("originY").toReal());
    checkCards("after scrolling back");
    if (QTest::currentTestFailed()) return;
    openByRightClick(2, "after scrolling back");
    if (QTest::currentTestFailed()) return;
    grid->setProperty("currentIndex", 0);
  }

  void gridActivationDuringLayoutKeepsTheChosenFile() {
    // The flat grid's relayout; date sorts lay out sections instead.
    m_library->setSortMode(CaptureFilterModel::RatingFirst);
    const auto restoreSort = qScopeGuard([&] { m_library->setSortMode(CaptureFilterModel::NewestFirst); });
    QTRY_VERIFY(item("flatGrid")->isVisible());
    QQuickItem* grid = item("library");
    QQuickItem* view = find(grid, [](QQuickItem* candidate) {
      return candidate->property("cellWidth").isValid();
    });
    QVERIFY(view);
    QQuickItem* cell = nullptr;
    QTRY_VERIFY((QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell),
                                          Q_ARG(int, 1)), cell != nullptr));
    QQuickItem* card = find(cell, [](QQuickItem* candidate) {
      return candidate->property("dragPaths").isValid();
    });
    QVERIFY(card);
    const QString chosen = card->property("path").toString();
    grid->setProperty("currentIndex", 0);
    QVERIFY(grid->property("selectedPath").toString() != chosen);
    const auto restore = qScopeGuard([&] {
      view->setProperty("reuseItems", true);
      grid->setProperty("restoringLayout", false);
    });
    // The model reattach suppresses automatic selection changes. A user's
    // activation during that interval must still replace the saved path.
    grid->setProperty("restoringLayout", true);
    view->setProperty("reuseItems", false);
    QVERIFY(QMetaObject::invokeMethod(card, "activated"));
    QCOMPARE(grid->property("selectedPath").toString(), chosen);
    QVERIFY(QMetaObject::invokeMethod(grid, "restoreLayout"));
    QCOMPARE(grid->property("selectedPath").toString(), chosen);
    view->setProperty("reuseItems", true);
    QVERIFY(QMetaObject::invokeMethod(grid, "restoreLayout"));
    QCOMPARE(pathAt(grid->property("currentIndex").toInt()), chosen);
  }

  // A small tiled window with large tiles cuts the visible rows off at the
  // bottom. Double-clicking a partly visible tile must open that tile, not
  // whatever the view scrolled under the pointer after the first click.
  void doubleClickOnAPartlyVisibleTileOpensThatTile() {
    // The flat grid's relayout; date sorts lay out sections instead.
    m_library->setSortMode(CaptureFilterModel::RatingFirst);
    const auto restoreSort = qScopeGuard([&] { m_library->setSortMode(CaptureFilterModel::NewestFirst); });
    QTRY_VERIFY(item("flatGrid")->isVisible());
    QQuickItem* grid = item("library");
    QQuickItem* detail = item("detail");
    const int tileWidth = m_settings->tileWidth();
    const auto restore = qScopeGuard([&] {
      invoke("dismissTopLayer");
      m_settings->setTileWidth(tileWidth);
      m_window->resize(1280, 820);
      waitForLayout(QSize(1280, 820));
    });
    m_window->resize(1005, 545);
    m_settings->setTileWidth(440);
    QQuickItem* view = find(grid, [](QQuickItem* candidate) {
      return candidate->property("cellWidth").isValid();
    });
    QVERIFY(view);
    // Measure only once the relayout has settled with the first row at the
    // top, since the rows below are counted from there. A slow machine can
    // still be resizing or scrolled after any fixed wait.
    QVariantList measured;
    bool settled = false;
    for (int attempt = 0; attempt < 30 && !settled; ++attempt) {
      if (view->property("contentY").toReal() != view->property("originY").toReal()) {
        QMetaObject::invokeMethod(view, "positionViewAtBeginning");
      }
      QTest::qWait(200);
      const QVariantList now{view->height(), view->property("cellWidth"),
                             view->property("cellHeight"), grid->property("columns"),
                             view->property("contentY"), view->property("originY")};
      settled = now == measured && grid->property("layoutReady").toBool() &&
                now.at(4).toReal() == now.at(5).toReal();
      measured = now;
    }
    QVERIFY2(settled, "grid geometry never settled at the top");
    // The first tile in the row that is cut off at the bottom of the viewport,
    // located from the view's own geometry so pooled delegates cannot mislead.
    const qreal cellHeight = view->property("cellHeight").toReal();
    const qreal cellWidth = view->property("cellWidth").toReal();
    const int columns = grid->property("columns").toInt();
    const int cutRow = static_cast<int>(std::floor(view->height() / cellHeight));
    const int index = cutRow * columns;
    QVERIFY2(index < m_library->rowCount(), "not enough rows to cut one off");
    QVERIFY2(cutRow * cellHeight < view->height() && (cutRow + 1) * cellHeight > view->height(),
             "row is not partly visible at this size");
    const QString path = pathAt(index);
    // Middle of the strip of the row that is still visible; the strip's height
    // depends on the platform's fonts, so do not assume a depth.
    const qreal strip = view->height() - cutRow * cellHeight;
    QVERIFY2(strip >= 24, qPrintable(QStringLiteral("visible strip is only %1 px").arg(strip)));
    const QPointF inView(cellWidth / 2, cutRow * cellHeight + strip / 2);
    int hit = -1;
    QVERIFY(QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, hit),
                                      Q_ARG(qreal, inView.x()),
                                      Q_ARG(qreal, inView.y() +
                                                       view->property("contentY").toReal())));
    QCOMPARE(hit, index);
    // Wait for the relayout after the size changes to settle: the live
    // delegate for the row must be under the click point and carry its file.
    QQuickItem* cell = nullptr;
    const auto cardPath = [&](QQuickItem* node) -> QString {
      QQuickItem* card = node ? find(node, [](QQuickItem* candidate) {
        return candidate->property("dragPaths").isValid();
      }) : nullptr;
      return card ? card->property("path").toString() : QString();
    };
    QTRY_VERIFY_WITH_TIMEOUT(
        (QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell),
                                   Q_ARG(int, index)),
         cell != nullptr && cell->mapRectToItem(view, QRectF(0, 0, cell->width(), cell->height()))
                                .contains(inView) &&
         cardPath(cell) == path),
        5000);
    // Under xcb the resize arrives asynchronously and can trigger a second
    // relayout, which detaches the model for a moment. Click only once the
    // same live delegate has stayed put for a while.
    bool stable = false;
    for (int attempt = 0; attempt < 20 && !stable; ++attempt) {
      QQuickItem* seen = cell;
      QTest::qWait(300);
      QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, cell),
                                Q_ARG(int, index));
      stable = cell && cell == seen && grid->property("layoutReady").toBool() &&
               grid->isEnabled() &&
               cell->mapRectToItem(view, QRectF(0, 0, cell->width(), cell->height()))
                   .contains(inView);
    }
    QVERIFY2(stable, "grid layout never settled");
    const qreal before = view->property("contentY").toReal();
    const QPoint at = view->mapToScene(inView).toPoint();
    // A human double-click: the first click's side effects get time to land
    // before the second click arrives.
    humanDoubleClick(at, 100);
    qInfo() << "after double-click: currentIndex" << grid->property("currentIndex").toInt()
            << "detail visible" << detail->isVisible() << "path"
            << detail->property("path").toString() << "window" << m_window->size() << "view"
            << view->size() << "at" << at << "layoutReady" << grid->property("layoutReady")
            << "enabled" << grid->isEnabled() << "sheet" << prop("anySheetOpen")
            << "popup" << prop("popupOpen") << "count" << grid->property("count");
    QTest::qWait(400);
    const qreal after = view->property("contentY").toReal();
    qInfo() << "contentY before" << before << "after" << after << "tile" << index << path;
    QTRY_VERIFY(detail->isVisible());
    QCOMPARE(detail->property("path").toString(), path);
  }

  void pendingBackgroundCannotAttachToAnotherImagesSheet() {
    const QString first = m_scratch.filePath(QStringLiteral("pending-background-a.png"));
    const QString second = m_scratch.filePath(QStringLiteral("pending-background-b.png"));
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(first));
    source.fill(Qt::blue);
    QVERIFY(source.save(second));
    QSignalSpy finished(m_matte, &MatteComposer::finished);
    perform(QStringLiteral("matte"), first);
    QTRY_COMPARE(item("mattePreview")->property("status").toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(item("matteSheet"), "save"));
    QVERIFY(m_matte->busy());
    // Synchronous calls occur before completion delivery, without racing a sleep.
    invoke("dismissTopLayer");
    perform(QStringLiteral("matte"), second);
    QVERIFY(!item("matteSheet")->isVisible());
    QCOMPARE(item("matteSheet")->property("path").toString(), first);
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    QVERIFY(!item("matteSheet")->isVisible());
    // The library indexes the saved copy. Keep it until the disposable profile
    // is destroyed, so an active delegate never points at a deleted fixture.
    perform(QStringLiteral("matte"), second);
    QTRY_VERIFY(item("matteSheet")->isVisible());
    QCOMPARE(item("matteSheet")->property("path").toString(), second);
    QVERIFY(!item("matteSheet")->property("retryCopy").toBool());
  }

  void emptyStateReadsTheAppliedSearchOnly() {
    // The empty state's text names the search. Reading it must not apply
    // typing still waiting on the debounce: that changed the count while the
    // empty state was being worked out, a binding loop on slow machines.
    const QString match = QFileInfo(pathAt(0)).completeBaseName();
    QVERIFY(!match.isEmpty());
    const auto restore = qScopeGuard([&] { m_library->setSearchText({}); });
    m_library->setSearchText(QStringLiteral("zz-nothing-matches-this"));
    QQuickItem* empty = item("libraryEmptyState");
    QTRY_VERIFY(empty->isVisible());
    const qsizetype before = m_warnings.size();
    m_library->queueSearchText(match);
    // Anything that redraws the empty state while the typing waits.
    QMetaObject::invokeMethod(m_mimeDefaults, "stateChanged");
    QCoreApplication::processEvents();
    const QStringList added = m_warnings.mid(before);
    QVERIFY2(added.filter(QStringLiteral("Binding loop")).isEmpty(), qPrintable(added.join(QLatin1Char('\n'))));
    // The typing still lands once the debounce runs.
    QTRY_VERIFY(m_library->count() > 0);
    QTRY_VERIFY(!empty->isVisible());
  }

  void theWindowRaisedNoQmlWarnings() {
    QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
  }

private:
  QVariant prop(const char* name) const { return m_window->property(name); }

  void invoke(const char* method) { QMetaObject::invokeMethod(m_window, method); }

  bool configureRawEditorRecorder() {
    const QString script = m_scratch.filePath(QStringLiteral("raw-editor-recorder"));
    QFile handler(script);
    if (!handler.open(QIODevice::WriteOnly)) return false;
    handler.write("#!/bin/sh\nprintf '%s' \"$1\" > \"$1.editor-open\"\n");
    handler.close();
    if (!handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner)) return false;
    auto* editors = qobject_cast<ExternalEditors*>(m_engine->rootContext()
        ->contextProperty(QStringLiteral("Editors")).value<QObject*>());
    if (!editors) return false;
    editors->setCustomCommand(QLatin1Char('"') + script + QLatin1Char('"'));
    return editors->setPreferred(QStringLiteral("custom"));
  }

  void perform(const QString& id, const QString& path) {
    QMetaObject::invokeMethod(m_window, "perform", Q_ARG(QVariant, id), Q_ARG(QVariant, path),
                              Q_ARG(QVariant, QVariant()));
  }

  // Waits until a resize has reached the window and the items it moved have
  // been laid out again, rather than sleeping through a guessed polish pass.
  // It does not cover timers or animations that move items afterwards, so a
  // geometry check that follows it uses QTRY.
  void waitForLayout(const QSize& size = QSize()) {
    if (size.isValid()) QTRY_COMPARE(m_window->size(), size);
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
  }

  // Rescans the library and waits for that scan to finish, however slow the
  // machine (an emulated aarch64 runner or a sanitizer build), so the checks
  // that follow see its result instead of racing it.
  // A refresh during a scan is folded into a follow-up that starts after a
  // debounce, so the scan in flight can finish before the files changed here
  // are read. Wait that scan out first, then scan afresh.
  // A refresh scheduled by the file watcher waits out its debounce first, and
  // that pending one is folded into this scan.
  void rescan() {
    QVERIFY(QTest::qWaitFor([&] { return !m_captures->scanning(); }, 60000));
    m_captures->refresh();
    QVERIFY(m_captures->scanning());
    waitForScan();
  }

  // Waits until no scan is running or waiting on its debounce.
  void waitForScan() {
    QVERIFY(QTest::qWaitFor([&] { return !m_captures->scanning() && !m_captures->refreshPending(); }, 60000));
  }

  static bool playedDuring(const QSignalSpy& states) {
    return std::any_of(states.cbegin(), states.cend(), [](const QList<QVariant>& change) {
      return change.at(0).value<QMediaPlayer::PlaybackState>() == QMediaPlayer::PlayingState;
    });
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

  QQuickItem* item(const QString& objectName) const {
    QQuickItem* found = find(m_window->contentItem(), [&objectName](QQuickItem* candidate) {
      return candidate->objectName() == objectName;
    });
    if (!found) {
      qFatal("no item named %s", qPrintable(objectName));
    }
    return found;
  }

  // The label of any visible video control drawn over the clock, or an empty
  // string when the clock stands clear.
  QString labelOverClock() const {
    QQuickItem* clock = item(QStringLiteral("videoClock"));
    const QRectF clockBounds = clock->mapRectToScene(clock->boundingRect());
    QString covering;
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* node) {
      for (QQuickItem* child : node->childItems()) {
        if (!child->isVisible() || !covering.isEmpty()) continue;
        const QString text = child->property("text").toString();
        if (!text.isEmpty() &&
            child->mapRectToScene(child->boundingRect()).intersects(clockBounds)) {
          covering = text;
        }
        walk(child);
      }
    };
    walk(item(QStringLiteral("subtitleOffsetControls")));
    walk(item(QStringLiteral("videoMediaControls")));
    return covering;
  }

  // A visible PillButton by its label or its compact-mode tooltip.
  // A text button, or an icon button by its name.
  static QQuickItem* pill(QQuickItem* within, const QString& label) {
    return find(within, [&label](QQuickItem* candidate) {
      return (candidate->property("floating").isValid() || candidate->property("raised").isValid()) &&
             (candidate->property("label").toString() == label ||
              candidate->property("toolTip").toString() == label) &&
             candidate->isVisible() && candidate->width() > 0;
    });
  }

  // The selection's count matches the files it holds, all of them shown.
  bool selectionConsistent(QQuickItem* grid, QString* why) const {
    QVariant paths;
    QMetaObject::invokeMethod(grid, "checkedPaths", Q_RETURN_ARG(QVariant, paths));
    const QStringList list = paths.value<QJSValue>().toVariant().toStringList();
    const int count = grid->property("checkedCount").toInt();
    if (count != list.size()) {
      *why = QStringLiteral("checkedCount %1 but %2 checked paths").arg(count).arg(list.size());
      return false;
    }
    for (const QString& path : list) {
      if (m_library->rowOf(path) < 0) {
        *why = QStringLiteral("%1 is checked but not shown").arg(path);
        return false;
      }
    }
    return true;
  }

  QQuickItem* cardFor(const QString& path) const {
    return find(m_window->contentItem(), [&path](QQuickItem* candidate) {
      // A relayout releases the previous delegates a moment after the new
      // ones appear; those keep a -1 index and must not answer for the tile.
      return candidate->property("dragPaths").isValid() &&
             candidate->property("path").toString() == path && candidate->isVisible() &&
             candidate->parentItem() && candidate->parentItem()->property("index").toInt() >= 0 &&
             shownByItsView(candidate);
    });
  }

  // Recycled delegates wait in the view's pool still marked visible, with
  // the file they showed last. Only the delegate the view holds for its
  // index is on screen and takes clicks.
  static bool shownByItsView(QQuickItem* item) {
    for (QQuickItem* delegate = item; delegate && delegate->parentItem();
         delegate = delegate->parentItem()) {
      QQuickItem* view = delegate->parentItem()->parentItem();
      if (!view || !view->inherits("QQuickItemView") || !delegate->property("index").isValid()) continue;
      QQuickItem* live = nullptr;
      QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, live),
                                Q_ARG(int, delegate->property("index").toInt()));
      return live == delegate;
    }
    return true;
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

  // Two clicks a human interval apart, with the event loop running between
  // them so whatever the first click triggers has happened by the second.
  // QTest::mouseClick only pushes its timestamps past the double-click
  // interval when no delay is given, so explicit delays keep the pair a
  // double-click as far as Qt is concerned.
  // The gap is applied before the second press and again before its release,
  // so it stays well under a quarter of the platform's double-click interval.
  void humanDoubleClick(const QPoint& at, int gapMs) {
    const int gap =
        qMin(gapMs, QGuiApplication::styleHints()->mouseDoubleClickInterval() / 4);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at, 1);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at, gap);
    QTest::qWait(30);
  }

  // The document rows drop their wordier pills when the stage is narrow, and the
  // label metrics differ between this desktop and CI, so a test that needs those
  // pills gives the window room first and puts the size back afterwards.
  QSize widenForDocumentChrome() {
    const QSize previous = m_window->size();
    m_window->resize(qMax(m_window->width(), 1600), qMax(m_window->height(), 900));
    return previous;
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

  // A pill in a row that re-centres when its contents change moves under the
  // pointer: click only once it has its place, as the header pills need.
  void clickSettled(QQuickItem* target) {
    QVERIFY(target);
    QTRY_VERIFY(centre(target).x() > 0 && centre(target).x() < m_window->width());
    QTest::qWait(120);
    click(target);
  }

  // A pill that has just appeared in a row has its size before the row has
  // placed it, and a click at its old place lands on a neighbour. Click once
  // the window's pending layout has run.
  void clickPlaced(QQuickItem* target) {
    QVERIFY(target);
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    click(target);
  }

  // Waits for a flickable to stop, so a position read is not taken mid-flick.
  void settle(QQuickItem* flickable) {
    QVERIFY(flickable);
    QTRY_VERIFY_WITH_TIMEOUT(!flickable->property("moving").toBool(), 5000);
    QTest::qWait(50);
  }

  // A press, a few moves and a release in the target's own coordinates, the
  // way a drag arrives. QTest sends the moves through the window, so the item
  // under the pointer gets them in the order a hand would produce.
  void dragBetween(QQuickItem* target, QPointF from, QPointF to) {
    QVERIFY(target);
    const QPoint start = target->mapToScene(from).toPoint();
    const QPoint end = target->mapToScene(to).toPoint();
    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::qWait(20);
    constexpr int kSteps = 4;
    for (int step = 1; step <= kSteps; ++step) {
      const QPointF at = QPointF(start)
                         + (QPointF(end) - QPointF(start))
                               * (static_cast<qreal>(step) / static_cast<qreal>(kSteps));
      QTest::mouseMove(m_window, at.toPoint());
      QTest::qWait(20);
    }
    QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, end);
    QTest::qWait(30);
  }

  void pinchAt(const QPoint& at) {
    static auto* device = [] {
      auto* touch = QTest::createTouchDevice();
      touch->setParent(qApp); // The test owns this synthetic device.
      return touch;
    }();
    QTest::touchEvent(m_window, device).press(0, at - QPoint(40, 0), m_window)
                                      .press(1, at + QPoint(40, 0), m_window);
    QTest::qWait(30);
    for (int distance : {50, 60, 80}) {
      QTest::touchEvent(m_window, device).move(0, at - QPoint(distance, 0), m_window)
                                        .move(1, at + QPoint(distance, 0), m_window);
      QTest::qWait(30);
    }
    QTest::touchEvent(m_window, device).release(0, at - QPoint(80, 0), m_window)
                                      .release(1, at + QPoint(80, 0), m_window);
    QTest::qWait(30);
  }

  // One wheel notch over the target: negative notches scroll the content down.
  void wheelOver(QQuickItem* target, int notches) {
    QVERIFY(target);
    const QPointF at = target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
    QWheelEvent event(at, m_window->mapToGlobal(at.toPoint()), QPoint(),
                      QPoint(0, -120 * notches), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(m_window, &event);
    QTest::qWait(80);
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
  MimeDefaults* m_mimeDefaults = nullptr;
  FolderTreeModel* m_folderTree = nullptr;
  ActionLauncher* m_actions = nullptr;
  ActionRegistry* m_registry = nullptr;
  TailscalePeers* m_tailscale = nullptr;
  MatteComposer* m_matte = nullptr;
  ImageEditor* m_imageEditor = nullptr;
  OcrIndex* m_textIndex = nullptr;
  QrDetector* m_qr = nullptr;
  SubtitleIndex* m_subtitles = nullptr;
  DuplicateIndex* m_duplicates = nullptr;
  SimilarityIndex* m_similarities = nullptr;
  MediaInspector* m_mediaInfo = nullptr;
  PdfInspector* m_pdfInfo = nullptr;
  MediaMetadataIndex* m_mediaMetadata = nullptr;
  ThumbnailProvider* m_thumbnails = nullptr;
  MatteProvider* m_mattes = nullptr;
  EditProvider* m_edits = nullptr;
  PdfProvider* m_pdfs = nullptr;
  QQmlApplicationEngine* m_engine = nullptr;
  QQuickWindow* m_window = nullptr;
  QStringList m_warnings;
  QStringList m_expectedQmlWarnings;
  QStringList m_disposablePaths;
  RawImageProvider* m_raws = nullptr;
  LargeImageProvider* m_large = nullptr;
  QString m_oddPath;
  QString m_demoPictures;
  QString m_pdfPath;
};

// Offscreen unconditionally, not just under ctest. Run by hand on a live
// session this would open a real window on the desktop, be resized by the
// tiling compositor, and click the wrong things.
int main(int argc, char* argv[]) {
  configureVideoPlayback();
  disableHeadlessAudio();
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication application(argc, argv);
  if (!QMediaDevices::audioOutputs().isEmpty()) {
    QTextStream(stderr) << "Refusing playback tests with audio outputs available\n";
    return 1;
  }
  UiTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}
#include "tst_ui.moc"
