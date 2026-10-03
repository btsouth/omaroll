// Optional diagnostics, run only in the runner's headless isolated container.
// Frame submission is not physical presentation. Jumps are not wheel latency;
// "cold" means a fresh application cache, not a cold OS filesystem cache.
#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/HeadlessAudio.h"
#include "edit/EditProvider.h"
#include "edit/ImageEditor.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/DuplicateIndex.h"
#include "library/MediaInspector.h"
#include "library/MediaMetadataIndex.h"
#include "library/SimilarityIndex.h"
#include "matte/MatteComposer.h"
#include "matte/MatteProvider.h"
#include "pdf/PdfInspector.h"
#include "pdf/PdfProvider.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "subtitles/SubtitleIndex.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailProvider.h"
#include "viewer/ViewerSession.h"
#include "sources/CaptureScanner.h"

#include <QAudioDevice>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iterator>
#include <memory>

using namespace Qt::StringLiterals;

static qint64 rssKb() {
  QFile file(u"/proc/self/status"_s);
  if (file.open(QIODevice::ReadOnly)) {
    for (const QByteArray& line : file.readAll().split('\n')) {
      if (line.startsWith("VmRSS:")) return line.simplified().split(' ').value(1).toLongLong();
    }
  }
  return -1;
}

static QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
  for (auto* child : root->childItems()) {
    if (match(child)) return child;
    if (auto* found = findItem(child, match)) return found;
  }
  return nullptr;
}

static double frameDistance(const QImage& left, const QImage& right) {
  qint64 total = 0;
  for (int y = 0; y < left.height(); ++y) {
    for (int x = 0; x < left.width(); ++x) {
      const QRgb a = left.pixel(x, y), b = right.pixel(x, y);
      total += qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
    }
  }
  return double(total) / (left.width() * left.height() * 3);
}

// All QML reads stay on the GUI thread. Like StartupTrace, latch readiness
// during sync and label only that frame at afterFrameEnd, with an operation ID
// so a queued completion can never label the next operation's previous frame.
class Measurements : public QObject {
public:
  QJsonArray samples, diagnostics;
  QJsonObject sample;
  QElapsedTimer clock;
  double initialGuiEventloopLag = -1;
  bool failed = false;
  QString fatal;
  quint64 generation = 0;
  QQuickWindow* window = nullptr;

  Measurements() {
    clock.start();
    QTimer::singleShot(0, this, [this] { initialGuiEventloopLag = clock.nsecsElapsed() / 1e6; });
    heartbeat.setInterval(10);
    heartbeat.setTimerType(Qt::PreciseTimer);
    connect(&heartbeat, &QTimer::timeout, this, [this] {
      const qint64 now = clock.nsecsElapsed();
      maximumGap = std::max(maximumGap, now - lastBeat);
      lastBeat = now;
      if (!fatal.isEmpty()) finish(generation, now, u"failed"_s, fatal);
      else if (error) {
        const QString message = error();
        if (!message.isEmpty()) finish(generation, now, u"failed"_s, message);
      }
      if (window) window->update();
    });
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] {
      finish(generation, clock.nsecsElapsed(), u"failed"_s, u"30s readiness/frame timeout"_s);
    });
  }

  void watch(QQuickWindow* target) {
    struct State { std::atomic<quint64> ready{0}; quint64 synchronized = 0, reported = 0; };
    auto state = std::make_shared<State>();
    connect(target, &QQuickWindow::afterAnimating, this, [this, target, state] {
      state->ready.store(active && window == target && fatal.isEmpty() && ready && ready()
                             ? generation : 0);
    });
    connect(target, &QQuickWindow::beforeSynchronizing, target, [state] {
      state->synchronized = state->ready.load();
    }, Qt::DirectConnection);
    connect(target, &QQuickWindow::afterFrameEnd, this, [this, state] {
      const quint64 id = state->synchronized;
      if (!id || id == state->reported) return;
      state->reported = id;
      const qint64 submitted = clock.nsecsElapsed();
      QMetaObject::invokeMethod(this, [this, id, submitted] {
        finish(id, submitted, u"ready"_s, {});
      }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
  }

  void measure(QString operation, QString phase, std::function<void()> action,
               std::function<bool()> predicate, std::function<void()> continuation,
               std::function<QString()> decoderError = {}) {
    ++generation;
    active = true;
    ready = std::move(predicate); error = std::move(decoderError); next = std::move(continuation);
    sample = {{u"operation"_s, operation}, {u"phase"_s, phase}, {u"rss_before_kb"_s, rssKb()}};
    started = lastBeat = clock.nsecsElapsed(); maximumGap = 0;
    heartbeat.start(); deadline.start(30000);
    const quint64 id = generation;
    QTimer::singleShot(0, this, [this, id] {
      if (active && generation == id)
        sample[u"initial_gui_event_loop_lag_ms"_s] = (clock.nsecsElapsed() - started) / 1e6;
    });
    action();
    sample[u"gui_dispatch_ms"_s] = (clock.nsecsElapsed() - started) / 1e6;
    if (window) window->update();
  }

private:
  QTimer heartbeat, deadline;
  bool active = false;
  qint64 started = 0, lastBeat = 0, maximumGap = 0;
  std::function<bool()> ready;
  std::function<QString()> error;
  std::function<void()> next;

  void finish(quint64 id, qint64 submitted, const QString& status, const QString& reason) {
    if (!active || id != generation) return;
    active = false; heartbeat.stop(); deadline.stop();
    if (submitted - started > 30'000'000'000LL && fatal.isEmpty()) fatal = u"30s operation timeout"_s;
    sample[u"status"_s] = fatal.isEmpty() ? status : u"failed"_s;
    sample[u"elapsed_ms"_s] = (submitted - started) / 1e6;
    if (status == u"ready" && fatal.isEmpty()) sample[u"content_ready_submitted_frame_ms"_s] = (submitted - started) / 1e6;
    sample[u"heartbeat_max_gap_ms"_s] = std::max(maximumGap, clock.nsecsElapsed() - lastBeat) / 1e6;
    sample[u"rss_after_kb"_s] = rssKb();
    if (!reason.isEmpty() || !fatal.isEmpty()) sample[u"diagnostic"_s] = fatal.isEmpty() ? reason : fatal;
    samples.append(sample);
    if (status == u"failed" || !fatal.isEmpty()) { failed = true; QCoreApplication::quit(); }
    else QTimer::singleShot(0, this, next);
  }
};

int main(int argc, char** argv) {
  QTemporaryDir profile;
  if (!profile.isValid()) return 1;
  for (const auto& pair : {std::pair{"HOME", "home"}, {"XDG_CONFIG_HOME", "config"},
                           {"XDG_CACHE_HOME", "cache"}, {"XDG_DATA_HOME", "data"},
                           {"XDG_STATE_HOME", "state"}}) {
    const QString path = profile.filePath(QString::fromLatin1(pair.second));
    QDir().mkpath(path); qputenv(pair.first, path.toUtf8());
  }
  const QByteArray empty = profile.filePath(u"empty"_s).toUtf8();
  QDir().mkpath(QString::fromUtf8(empty));
  for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR",
                           "OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR"}) qputenv(name, empty);
  disableHeadlessAudio();
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_FORCE_STDERR_LOGGING", "1");
  QGuiApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationName(u"Omaroll"_s); app.setOrganizationName(u"Omaroll"_s);
  if (!QMediaDevices::audioOutputs().isEmpty()) {
    QTextStream(stderr) << "Refusing benchmark with exposed audio outputs\n"; return 1;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.filePath(u"config"_s));
  QQuickStyle::setStyle(u"Basic"_s);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOptions({{{u"library"_s}, u"Generated fixture root containing library/ and seeds/"_s, u"root"_s},
                     {{u"without-prefetch"_s}, u"Diagnostic comparison: disable viewer neighbor preloads"_s},
                     {{u"steps"_s}, u"Gallery page jumps per pass"_s, u"count"_s, u"12"_s},
                     {{u"burst-jumps"_s}, u"Optional rapid gallery jumps, 16 ms apart, before the normal passes"_s, u"count"_s, u"0"_s},
                     {{u"viewer-files"_s}, u"Ordered seed path, repeat for each file (default: seeds/ name order)"_s, u"file"_s}});
  parser.process(app);
  const QDir fixture(parser.value(u"library"_s));
  bool validSteps = false;
  const int steps = parser.value(u"steps"_s).toInt(&validSteps);
  bool validBurst = false;
  const int burstJumps = parser.value(u"burst-jumps"_s).toInt(&validBurst);
  if (!parser.isSet(u"library"_s) || !QFileInfo(fixture.filePath(u"library"_s)).isDir()
      || !validSteps || steps < 1 || steps > 1000 || !validBurst || burstJumps < 0
      || burstJumps > 1000 || !parser.positionalArguments().isEmpty()) parser.showHelp(2);
  QStringList seeds = parser.values(u"viewer-files"_s), files;
  QJsonArray skippedSeeds;
  if (seeds.isEmpty()) {
    for (const auto& entry : QDir(fixture.filePath(u"seeds"_s)).entryInfoList(QDir::Files, QDir::Name))
      seeds.append(entry.absoluteFilePath());
  }
  for (const QString& seed : seeds) {
    const QFileInfo info(QDir::isAbsolutePath(seed) ? seed : fixture.filePath(seed));
    const QString path = info.canonicalFilePath();
    if (!info.isReadable() || !info.isFile()) {
      QTextStream(stderr) << "Unreadable seed: " << seed << '\n'; return 2;
    }
    const QByteArray suffix = info.suffix().toLower().toLatin1();
    if (!ViewerSession::canOpen({path}) || suffix == "svg" || suffix == "svgz") {
      skippedSeeds.append(QJsonObject{{u"seed"_s, path}, {u"reason"_s, u"not a supported raster/video seed"_s}});
    } else files.append(path);
  }
  if (files.size() < 2) { QTextStream(stderr) << "Need at least two supported viewer seeds\n"; return 2; }

  // Independent first-frame references are prepared before operation timers.
  // Match pixels as well as player state, so a late frame from the previous
  // source cannot satisfy readiness. Reject ambiguous video fixtures.
  QHash<QString, QImage> videoReferences;
  for (const QString& file : files) {
    if (!CaptureScanner::isVideo(CaptureScanner::mediaSuffix(file))) continue;
    QProcess probe;
    probe.start(u"ffmpeg"_s, {u"-v"_s, u"error"_s, u"-i"_s, file, u"-frames:v"_s, u"1"_s,
                u"-f"_s, u"image2pipe"_s, u"-vcodec"_s, u"png"_s, u"pipe:1"_s});
    if (!probe.waitForFinished(10000) || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
      probe.kill(); probe.waitForFinished();
      QTextStream(stderr) << "Cannot prepare video reference: " << file << '\n'; return 2;
    }
    const QImage image = QImage::fromData(probe.readAllStandardOutput());
    if (image.isNull()) return 2;
    videoReferences.insert(file, image.scaled(32, 18, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
  }
  for (auto a = videoReferences.cbegin(); a != videoReferences.cend(); ++a) {
    for (auto b = std::next(a); b != videoReferences.cend(); ++b) {
      if (frameDistance(a.value(), b.value()) < 2) {
        QTextStream(stderr) << "Indistinguishable video references: " << a.key() << " and " << b.key() << '\n';
        return 2;
      }
    }
  }
  Measurements run;
  run.diagnostics = skippedSeeds;

  QQmlEngine engine;
  engine.addImportPath(QStringLiteral(OMAROLL_QML_IMPORT_PATH));
  auto* thumbnails = new ThumbnailProvider; auto* mattes = new MatteProvider;
  auto* edits = new EditProvider; auto* pdfs = new PdfProvider;
  engine.addImageProvider(ThumbnailProvider::kProviderId, thumbnails);
  engine.addImageProvider(MatteProvider::kProviderId, mattes);
  engine.addImageProvider(EditProvider::kProviderId, edits);
  engine.addImageProvider(PdfProvider::kProviderId, pdfs);
  OmarchyTheme theme(profile.filePath(u"state"_s), profile.filePath(u"config"_s));
  AppSettings settings; ActionLauncher actions; ActionRegistry registry(&actions);
  auto* shared = engine.rootContext();
  shared->setContextProperty(u"Theme"_s, &theme); shared->setContextProperty(u"Settings"_s, &settings);
  shared->setContextProperty(u"Actions"_s, &actions); shared->setContextProperty(u"Registry"_s, &registry);
  shared->setContextProperty(u"DemoMode"_s, false);
  std::unique_ptr<QObject> galleryServices, galleryRoot, viewerRoot;
  QQuickWindow* gallery = nullptr; QQuickWindow* viewer = nullptr;
  QQuickItem* grid = nullptr; QQuickItem* viewport = nullptr;
  CaptureModel* captures = nullptr; MediaMetadataIndex* metadata = nullptr;
  ViewerSession session; MediaInspector viewerInfo; SubtitleIndex viewerSubtitles;
  QQmlContext viewerContext(shared);
  viewerContext.setContextProperty(u"Session"_s, &session);
  viewerContext.setContextProperty(u"MediaInfo"_s, &viewerInfo);
  viewerContext.setContextProperty(u"Subtitles"_s, &viewerSubtitles);
  QObject::connect(&engine, &QQmlEngine::warnings, &run, [&](const QList<QQmlError>& warnings) {
    for (const auto& warning : warnings) {
      const QString message = warning.toString();
      run.diagnostics.append(message);
      run.fatal = message;
    }
  });
  auto create = [&](const char* type, QQmlContext* context, std::unique_ptr<QObject>& root) {
    QQmlComponent component(&engine); component.loadFromModule("Omaroll", type);
    root.reset(component.create(context));
    if (!root) run.fatal = component.errorString();
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    if (window) { window->resize(1280, 820); run.watch(window); }
    else if (run.fatal.isEmpty()) run.fatal = u"QML did not create a window"_s;
    return window;
  };
  QJsonObject result{{u"mode"_s, u"offscreen-gallery-viewer"_s}, {u"fixture_root"_s, fixture.absolutePath()},
                     {u"seed_candidates"_s, seeds.size()}, {u"viewer_files"_s, files.size()},
                     {u"steps"_s, steps}, {u"qt"_s, QString::fromLatin1(qVersion())},
                     {u"metadata_enabled"_s, true}, {u"viewer_order"_s, QJsonArray::fromStringList(files)}};
  result[u"viewer_prefetch_enabled"_s] = !parser.isSet(u"without-prefetch"_s);
  result[u"gallery_burst_jumps"_s] = burstJumps;
  result[u"video_frame_verification"_s] = u"32x18 first-frame RGB reference; mean channel error <=12 and >=2 closer than every other video; references prepared outside timers"_s;
  std::function<void()> galleryNext, viewerNext;
  int galleryStep = 0, viewerStep = 0;
  double maximumContentY = 0;
  QMetaObject::Connection frameConnection;
  bool freshVideo = false;
  auto matchesVideo = [&](const QString& expected, const QVideoFrame& frame) {
    if (!frame.isValid() || !videoReferences.contains(expected)) return false;
    const QImage image = frame.toImage().scaled(32, 18, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) return false;
    const double distance = frameDistance(image, videoReferences.value(expected));
    const QString metric = u"video_reference_min_channel_error"_s;
    if (!run.sample.contains(metric) || distance < run.sample[metric].toDouble()) run.sample[metric] = distance;
    if (distance > 12) return false;
    for (auto it = videoReferences.cbegin(); it != videoReferences.cend(); ++it) {
      if (it.key() != expected && frameDistance(image, it.value()) < distance + 2) return false;
    }
    return true;
  };
  auto attachVideo = [&](const QString& expected) {
    if (!viewer || frameConnection) return;
    auto* player = viewer->findChild<QMediaPlayer*>(u"viewerPlayer"_s);
    if (!player || !player->videoSink()) return;
    const quint64 id = run.generation;
    auto* sink = player->videoSink();
    frameConnection = QObject::connect(sink, &QVideoSink::videoFrameChanged, &run,
        [&, player, sink, expected, id](const QVideoFrame& frame) {
          if (id == run.generation && session.path() == expected
              && player->videoSink() == sink && player->source() == QUrl::fromLocalFile(expected)
              && matchesVideo(expected, frame)) {
            freshVideo = true; viewer->update();
          }
        });
  };
  viewerNext = [&] {
    if (viewerStep == files.size() * 2) { QCoreApplication::quit(); return; }
    const int step = viewerStep++;
    const int index = step < files.size() ? step : files.size() * 2 - step - 2;
    // Return walks backwards from the last seed, wrapping once to that last seed.
    const QString expected = files.at((index + files.size()) % files.size());
    run.measure(u"viewer_navigation"_s, step < files.size() ? u"first_pass"_s : u"return_pass"_s, [&] {
      QObject::disconnect(frameConnection); frameConnection = {}; freshVideo = false;
      run.sample[u"file"_s] = expected;
      if (!viewer) {
        viewer = create("Viewer", &viewerContext, viewerRoot);
        if (!viewer) return;
        if (parser.isSet(u"without-prefetch"_s)) {
          for (const int direction : {1, -1}) {
            const QString name = u"viewerPrefetch"_s + QString::number(direction);
            auto* image = findItem(viewer->contentItem(), [&](auto* item) { return item->objectName() == name; });
            if (!image || !QQmlProperty(image, u"source"_s).write(QUrl())) {
              run.fatal = u"Could not disable viewer prefetch"_s;
              return;
            }
          }
        }
        run.window = viewer; session.open(files); viewer->show(); viewer->requestActivate();
      } else {
        attachVideo(expected); // Existing sink: listen before source navigation.
        auto* keys = findItem(viewer->contentItem(), [](auto* item) { return item->objectName() == u"viewerKeys"; });
        if (keys) keys->forceActiveFocus();
        const bool backwards = step >= files.size();
        const Qt::Key key = session.isVideo() ? (backwards ? Qt::Key_PageUp : Qt::Key_PageDown)
                                             : (backwards ? Qt::Key_Left : Qt::Key_Right);
        QElapsedTimer dispatch; dispatch.start(); QTest::keyClick(viewer, key);
        run.sample[u"gui_key_dispatch_ms"_s] = dispatch.nsecsElapsed() / 1e6;
        run.sample[u"key"_s] = int(key);
        if (session.path() != expected) run.fatal = u"Synthetic key did not navigate to expected seed"_s;
      }
      run.sample[u"media_kind"_s] = session.isVideo() ? u"video"_s : session.isAnimated() ? u"animation"_s : u"image"_s;
    }, [&, expected] {
      if (!viewer || session.path() != expected) return false;
      if (!session.isVideo()) {
        auto* surface = findItem(viewer->contentItem(), [](auto* item) { return item->objectName() == u"viewerStillLoader"; });
        return viewer->property("imageReady").toBool() && surface && surface->isVisible() && surface->opacity() >= 0.999;
      }
      auto* player = viewer->findChild<QMediaPlayer*>(u"viewerPlayer"_s);
      if (!player || !player->videoSink() || player->source() != session.url()) return false;
      attachVideo(expected);
      // Covers the first frame arriving before a lazily created sink is found.
      freshVideo = freshVideo || matchesVideo(expected, player->videoSink()->videoFrame());
      return freshVideo;
    }, viewerNext, [&] { return viewer ? viewer->property("playbackError").toString() : QString(); });
  };
  galleryNext = [&] {
    if (galleryStep == steps * 2) {
      result[u"library_files"_s] = captures->rowCount();
      result[u"metadata_completed_at_gallery_end"_s] = metadata->completed();
      result[u"metadata_total_at_gallery_end"_s] = metadata->total();
      gallery->close(); galleryRoot.reset(); galleryServices.reset();
      run.window = nullptr; viewerNext(); return;
    }
    const int step = galleryStep++;
    const int page = step < steps ? step + 1 : steps * 2 - step - 1;
    const double target = page == steps ? maximumContentY : maximumContentY * page / steps;
    run.measure(u"gallery_jump"_s, step < steps ? u"cold_cache_outward"_s : u"cached_return"_s, [&] {
      const double before = viewport->property("contentY").toDouble();
      viewport->setProperty("contentY", target); QMetaObject::invokeMethod(viewport, "forceLayout");
      run.sample[u"content_y"_s] = viewport->property("contentY").toDouble();
      run.sample[u"jump_displacement_px"_s] = viewport->property("contentY").toDouble() - before;
    }, [&, target] {
      QVariant ready;
      QMetaObject::invokeMethod(grid, "viewportReady", Q_RETURN_ARG(QVariant, ready));
      return !captures->scanning() && std::abs(viewport->property("contentY").toDouble() - target) < 1 && ready.toBool();
    }, galleryNext);
  };
  QTimer::singleShot(0, &run, [&] {
    run.measure(u"gallery_initial_viewport"_s, u"cold_cache"_s, [&] {
      galleryServices = std::make_unique<QObject>();
      auto* owner = galleryServices.get(); auto* context = new QQmlContext(shared, owner);
      captures = new CaptureModel(&settings, owner); captures->setExtraRoot(fixture.filePath(u"library"_s));
      auto* filtered = new CaptureFilterModel(owner); filtered->setSourceModel(captures);
      filtered->setSortMode(settings.sortMode()); filtered->setKindFilter(settings.kindFilter());
      filtered->setShowHidden(settings.showHidden());
      QObject::connect(filtered, &CaptureFilterModel::sortModeChanged, &settings, [&, filtered] { settings.setSortMode(filtered->sortMode()); });
      QObject::connect(filtered, &CaptureFilterModel::kindFilterChanged, &settings, [&, filtered] { settings.setKindFilter(filtered->kindFilter()); });
      QObject::connect(filtered, &CaptureFilterModel::showHiddenChanged, &settings, [&, filtered] { settings.setShowHidden(filtered->showHidden()); });
      metadata = new MediaMetadataIndex(captures, owner); // Applies cached/probed updates itself, as production does.
      auto* text = new OcrIndex(captures, owner);
      auto* duplicates = new DuplicateIndex(captures, owner); auto* similar = new SimilarityIndex(captures, owner);
      QObject::connect(filtered, &CaptureFilterModel::searchTextChanged, text, [=] { text->setSearchText(filtered->searchText()); });
      QObject::connect(text, &OcrIndex::textReady, filtered, &CaptureFilterModel::setOcrText);
      QObject::connect(filtered, &CaptureFilterModel::duplicatesOnlyChanged, duplicates, [=] { duplicates->setActive(filtered->duplicatesOnly()); });
      QObject::connect(duplicates, &DuplicateIndex::groupsChanged, filtered, [=] { filtered->setDuplicateGroups(duplicates->groups()); });
      QObject::connect(filtered, &CaptureFilterModel::similarOnlyChanged, similar, [=] { similar->setActive(filtered->similarOnly()); });
      QObject::connect(similar, &SimilarityIndex::groupsChanged, filtered, [=] { filtered->setSimilarGroups(similar->groups()); });
      QObject::connect(&actions, &ActionLauncher::outputPending, captures, &CaptureModel::holdPath);
      QObject::connect(&actions, &ActionLauncher::outputSettled, captures, &CaptureModel::releasePath);
      context->setContextProperty(u"Captures"_s, filtered); context->setContextProperty(u"Library"_s, captures);
      context->setContextProperty(u"MediaMetadata"_s, metadata); context->setContextProperty(u"TextIndex"_s, text);
      context->setContextProperty(u"Duplicates"_s, duplicates); context->setContextProperty(u"Similarities"_s, similar);
      context->setContextProperty(u"MediaInfo"_s, new MediaInspector(owner)); context->setContextProperty(u"PdfInfo"_s, new PdfInspector(owner));
      context->setContextProperty(u"Matte"_s, new MatteComposer(owner)); context->setContextProperty(u"ImageEdit"_s, new ImageEditor(owner));
      context->setContextProperty(u"Qr"_s, new QrDetector(captures, owner)); context->setContextProperty(u"Subtitles"_s, new SubtitleIndex(owner));
      context->setContextProperty(u"Tailscale"_s, new TailscalePeers(owner));
      context->setContextProperty(u"InitialPaths"_s, QStringList()); context->setContextProperty(u"InitialFolderPath"_s, QString());
      gallery = create("Main", context, galleryRoot); run.window = gallery;
      if (gallery) {
        grid = findItem(gallery->contentItem(), [](auto* item) { return item->objectName() == u"library"; });
        if (grid) viewport = findItem(grid, [](auto* item) { return item->property("contentY").isValid() && item->property("cellHeight").isValid(); });
        if (!viewport) run.fatal = u"Could not find gallery GridView"_s;
      }
    }, [&] {
      QVariant ready;
      if (!grid || !viewport) return false;
      QMetaObject::invokeMethod(grid, "viewportReady", Q_RETURN_ARG(QVariant, ready));
      return !captures->scanning() && ready.toBool();
    }, [&] {
      const double pageHeight = viewport->height();
      maximumContentY = viewport->property("contentHeight").toDouble() - pageHeight;
      result[u"gallery_maximum_content_y"_s] = maximumContentY;
      if (maximumContentY < steps * pageHeight || pageHeight <= 0) {
        run.failed = true; run.fatal = u"Fixture needs at least steps+1 gallery viewports"_s; QCoreApplication::quit();
      } else if (burstJumps == 0) galleryNext();
      else {
        // Measure from the first input through a ready final viewport. Earlier
        // targets deliberately do not settle, exercising obsolete requests.
        auto* burst = new QTimer(&run);
        burst->setInterval(16);
        burst->setTimerType(Qt::PreciseTimer);
        auto dispatched = std::make_shared<int>(0);
        auto jump = [&, burst, dispatched] {
          const int number = ++*dispatched;
          const double target = number == burstJumps ? maximumContentY
              : maximumContentY * ((number * 7) % 13) / 13;
          viewport->setProperty("contentY", target);
          QMetaObject::invokeMethod(viewport, "forceLayout");
          if (number == burstJumps) burst->stop();
        };
        QObject::connect(burst, &QTimer::timeout, &run, jump);
        run.measure(u"gallery_burst"_s, u"cold_cache"_s, [&, burst, jump] {
          run.sample[u"requested_jumps"_s] = burstJumps;
          jump();
          if (burstJumps > 1) burst->start();
        }, [&, dispatched] {
          if (*dispatched != burstJumps) return false;
          QVariant ready;
          QMetaObject::invokeMethod(grid, "viewportReady", Q_RETURN_ARG(QVariant, ready));
          return !captures->scanning()
              && std::abs(viewport->property("contentY").toDouble() - maximumContentY) < 1 && ready.toBool();
        }, [&, burst] { burst->deleteLater(); galleryNext(); });
      }
    });
  });
  app.exec();
  result[u"graphics_api"_s] = run.window ? int(run.window->rendererInterface()->graphicsApi()) : -1;
  result[u"graphics_driver_details"_s] = u"See runner QSG_INFO logs"_s;
  result[u"initial_gui_event_loop_lag_ms"_s] = run.initialGuiEventloopLag;
  QObject::disconnect(frameConnection);
  if (viewer) viewer->close();
  if (galleryRoot) gallery->close();
  viewerRoot.reset(); galleryRoot.reset(); galleryServices.reset();
  thumbnails->shutdown(); mattes->shutdown(); edits->shutdown(); pdfs->shutdown();
  if (!QThreadPool::globalInstance()->waitForDone(30000)) run.fatal = u"Image worker teardown timeout"_s;
  result[u"samples"_s] = run.samples; result[u"diagnostics"_s] = run.diagnostics;
  result[u"failed"_s] = run.failed || !run.fatal.isEmpty(); result[u"error"_s] = run.fatal;
  QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
  return run.failed || !run.fatal.isEmpty() ? 1 : 0;
}
