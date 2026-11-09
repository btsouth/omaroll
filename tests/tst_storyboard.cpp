#include "thumbs/Storyboard.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#include <memory>

namespace {

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeFile(const QString& path, const QByteArray& contents) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

QString shellQuote(QString text) {
  text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + text + QLatin1Char('\'');
}

QByteArray jpeg(const QColor& color, const QSize& size = QSize(64, 36)) {
  QImage image(size, QImage::Format_RGB32);
  image.fill(color);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "JPG", 80);
  return bytes;
}

// Executed in place of ffmpeg. The mode file says what to do: "wait" holds
// until it changes, "success" writes a frame 0.5 s before the target, and
// anything else fails the way ffmpeg does for a file with no video stream.
int runHelper(const QStringList& arguments) {
  const int seek = arguments.indexOf(QStringLiteral("-ss"));
  const int input = arguments.indexOf(QStringLiteral("-i"));
  QFile log(qEnvironmentVariable("OMAROLL_TEST_STORYBOARD_LOG"));
  if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 2;
  log.write(QStringLiteral("%1 %2 %3\n")
                .arg(QCoreApplication::applicationPid())
                .arg(seek >= 0 ? arguments.value(seek + 1) : QString())
                .arg(input >= 0 ? arguments.value(input + 1) : QString())
                .toUtf8());
  log.close();
  const QString modePath = qEnvironmentVariable("OMAROLL_TEST_STORYBOARD_MODE");
  QElapsedTimer elapsed;
  elapsed.start();
  while (readFile(modePath).startsWith("wait") && elapsed.elapsed() < 30000) {
    QThread::msleep(10);
  }
  if (readFile(modePath) != "success") {
    QFile err;
    (void)err.open(stderr, QIODevice::WriteOnly);
    err.write("Stream map '0:v:0' matches no streams.\n");
    return 1;
  }
  QFile err;
  (void)err.open(stderr, QIODevice::WriteOnly);
  err.write("[Parsed_showinfo_0 @ 0x1] n:   0 pts:  -64 pts_time:-0.5 duration: 1\n");
  QFile out;
  (void)out.open(stdout, QIODevice::WriteOnly);
  out.write(jpeg(QColor(40, 120, 200)));
  return 0;
}

bool alive(qint64 pid) { return ::kill(pid_t(pid), 0) == 0; }

// Twelve seconds of moving pattern with a keyframe every two seconds, so a
// storyboard has several distinct frames. The checked-in fixture has one.
QString makeStoryboardVideo(const QString& path) {
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

} // namespace

class StoryboardTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    m_oldPath = qgetenv("PATH");
  }

  void init() {
    // A storyboard that cannot be made degrades quietly to the time alone.
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
    m_oldCache = qgetenv("XDG_CACHE_HOME");
    m_scratch = std::make_unique<QTemporaryDir>();
    QVERIFY(m_scratch->isValid());
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch->filePath(QStringLiteral("cache")).toUtf8()));
  }

  void cleanup() {
    restoreEnvironment("PATH", m_oldPath);
    restoreEnvironment("XDG_CACHE_HOME", m_oldCache);
    // Nothing outlives its storyboard.
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 5000);
    m_scratch.reset();
  }

  void planSizesFramesByDuration() {
    QVERIFY(StoryboardPlan::sampleTimes(0).isEmpty());
    QVERIFY(StoryboardPlan::sampleTimes(StoryboardPlan::kMinimumDurationMs - 1).isEmpty());

    const QList<qint64> short_ = StoryboardPlan::sampleTimes(10000);
    QCOMPARE(short_, (QList<qint64>{1000, 3000, 5000, 7000, 9000}));

    const QList<qint64> minute = StoryboardPlan::sampleTimes(60000);
    QCOMPARE(minute.size(), 30);

    // Two hours: the frame limit, evenly spread, all inside the clip.
    const qint64 twoHours = 2LL * 60 * 60 * 1000;
    const QList<qint64> film = StoryboardPlan::sampleTimes(twoHours);
    QCOMPARE(film.size(), StoryboardPlan::kMaximumFrames);
    QCOMPARE(film.constFirst(), 36000);
    QCOMPARE(film.at(1) - film.at(0), 72000);
    QVERIFY(film.constLast() < twoHours);
    QVERIFY(std::is_sorted(film.cbegin(), film.cend()));

    for (const qint64 duration : {5000LL, 5001LL, 199999LL, 200001LL, 86400000LL}) {
      const QList<qint64> times = StoryboardPlan::sampleTimes(duration);
      QVERIFY(!times.isEmpty());
      QVERIFY(times.size() <= StoryboardPlan::kMaximumFrames);
      QVERIFY(times.constLast() < duration);
      for (qsizetype index = 1; index < times.size(); ++index) {
        QVERIFY(times.at(index) - times.at(index - 1) >= StoryboardPlan::kMinimumIntervalMs);
      }
    }

    // Two sizes, so a fractional scale shares its neighbour's cache.
    QCOMPARE(StoryboardPlan::pixelBox(1.0), QSize(192, 108));
    QCOMPARE(StoryboardPlan::pixelBox(0.0), QSize(192, 108));
    QCOMPARE(StoryboardPlan::pixelBox(1.25), QSize(384, 216));
    QCOMPARE(StoryboardPlan::pixelBox(2.0), QSize(384, 216));
    QCOMPARE(StoryboardPlan::pixelBox(3.0), QSize(384, 216));
  }

  void extractionGoesCoarseToFine() {
    QCOMPARE(StoryboardPlan::extractionOrder(0), QList<int>{});
    QCOMPARE(StoryboardPlan::extractionOrder(1), QList<int>{0});
    QCOMPARE(StoryboardPlan::extractionOrder(5), (QList<int>{0, 4, 2, 1, 3}));
    for (int count = 1; count <= 130; ++count) {
      QList<int> order = StoryboardPlan::extractionOrder(count);
      QCOMPARE(order.size(), count);
      // The first handful already reach across the clip.
      if (count >= 8) {
        const int reach = *std::max_element(order.cbegin(), order.cbegin() + 4);
        QVERIFY2(reach >= count / 2, qPrintable(QString::number(count)));
      }
      std::sort(order.begin(), order.end());
      for (int index = 0; index < count; ++index) QCOMPARE(order.at(index), index);
    }
  }

  void nearestFrameLookup() {
    QCOMPARE(StoryboardPlan::nearest({}, 1000), -1);
    const QList<qint64> times{0, 10000, 20000};
    QCOMPARE(StoryboardPlan::nearest(times, -5), 0);
    QCOMPARE(StoryboardPlan::nearest(times, 4999), 0);
    QCOMPARE(StoryboardPlan::nearest(times, 5000), 0);
    QCOMPARE(StoryboardPlan::nearest(times, 5001), 1);
    QCOMPARE(StoryboardPlan::nearest(times, 10000), 1);
    QCOMPARE(StoryboardPlan::nearest(times, 16000), 2);
    QCOMPARE(StoryboardPlan::nearest(times, 999999), 2);

    // Two samples on one keyframe are one frame, and frames stay in order.
    StoryboardFrames frames;
    QVERIFY(frames.insert(20000, jpeg(Qt::red)));
    QVERIFY(frames.insert(0, jpeg(Qt::green)));
    QVERIFY(!frames.insert(20000, jpeg(Qt::blue)));
    QVERIFY(!frames.insert(5000, {}));
    QVERIFY(frames.insert(10000, jpeg(Qt::blue)));
    QCOMPARE(frames.times, times);
    QCOMPARE(frames.frameSize, QSize(64, 36));
  }

  void cacheRoundTripsAndInvalidates() {
    const QString video = m_scratch->filePath(QStringLiteral("clip.mp4"));
    QVERIFY(writeFile(video, "not really a video"));
    const QSize box = StoryboardPlan::pixelBox(1);
    const QString key = StoryboardCache::key(video, box);
    QVERIFY(!key.isEmpty());
    QCOMPARE(StoryboardCache::key(video, box), key);
    QVERIFY(StoryboardCache::key(video, StoryboardPlan::pixelBox(2)) != key);
    QVERIFY(StoryboardCache::key(m_scratch->filePath(QStringLiteral("missing.mp4")), box).isEmpty());
    QVERIFY(!StoryboardCache::load(key));

    StoryboardFrames frames;
    frames.insert(1000, jpeg(Qt::red));
    frames.insert(3000, jpeg(Qt::blue));
    QVERIFY(StoryboardCache::save(key, frames));
    const std::optional<StoryboardFrames> loaded = StoryboardCache::load(key);
    QVERIFY(loaded);
    QCOMPARE(loaded->times, frames.times);
    QCOMPARE(loaded->jpegs, frames.jpegs);
    QCOMPARE(loaded->frameSize, frames.frameSize);

    // A rewrite is a different file, even at the same size.
    QTest::qWait(10);
    QVERIFY(writeFile(video, "not really a video"));
    QVERIFY(StoryboardCache::key(video, box) != key);

    // A damaged entry is a miss, not a crash or a wrong frame.
    const QString entry = StoryboardCache::directory() + QLatin1Char('/') + key + QStringLiteral(".storyboard");
    QByteArray bytes = readFile(entry);
    QVERIFY(bytes.size() > 40);
    QVERIFY(writeFile(entry, bytes.left(bytes.size() - 7)));
    QVERIFY(!StoryboardCache::load(key));
    bytes[0] = 'X';
    QVERIFY(writeFile(entry, bytes));
    QVERIFY(!StoryboardCache::load(key));
  }

  void cacheEvictsLeastRecentlyUsed() {
    StoryboardFrames frames;
    frames.insert(1000, jpeg(Qt::red, QSize(320, 180)));
    QStringList keys;
    for (const QString& name : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}) {
      const QString key = QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Md5).toHex());
      QVERIFY(StoryboardCache::save(key, frames));
      keys.append(key);
    }
    const auto entry = [](const QString& key) {
      return StoryboardCache::directory() + QLatin1Char('/') + key + QStringLiteral(".storyboard");
    };
    const auto age = [&](const QString& key, qint64 seconds) {
      const struct timespec when[2] = {{seconds, 0}, {seconds, 0}};
      return ::utimensat(AT_FDCWD, QFile::encodeName(entry(key)).constData(), when, 0) == 0;
    };
    QVERIFY(age(keys.at(0), 1000000));
    QVERIFY(age(keys.at(1), 2000000));
    QVERIFY(age(keys.at(2), 3000000));
    // Opening the oldest makes it the newest.
    QVERIFY(StoryboardCache::load(keys.at(0)));

    const qint64 one = QFileInfo(entry(keys.at(0))).size();
    StoryboardCache::prune(one * 2);
    QVERIFY(QFile::exists(entry(keys.at(0))));
    QVERIFY(!QFile::exists(entry(keys.at(1))));
    QVERIFY(QFile::exists(entry(keys.at(2))));
    StoryboardCache::prune(one);
    QVERIFY(QFile::exists(entry(keys.at(0))));
    QVERIFY(!QFile::exists(entry(keys.at(2))));
  }

  void changingFileCancelsItsDecoders() {
    useHelper("wait");
    const QString first = fakeVideo(QStringLiteral("first.mp4"));
    const QString second = fakeVideo(QStringLiteral("second.mp4"));
    auto storyboard = std::make_unique<Storyboard>();
    storyboard->setDuration(600000);
    storyboard->setSource(first);
    // Nothing runs before the first frame is on screen.
    QTest::qWait(Storyboard::kStartDelayMs + 200);
    QCOMPARE(helperStarts().size(), 0);
    storyboard->setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), Storyboard::kParallelProcesses, 5000);
    QTest::qWait(200);
    // Never more than two at once.
    QCOMPARE(Storyboard::runningProcesses(), Storyboard::kParallelProcesses);
    const QList<QStringList> firstStarts = helperStarts();
    for (const QStringList& start : firstStarts) QCOMPARE(start.value(2), first);
    QVERIFY(storyboard->running());

    storyboard->setSource(second);
    for (const QStringList& start : firstStarts) {
      QTRY_VERIFY_WITH_TIMEOUT(!alive(start.value(0).toLongLong()), 3000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), 2 * Storyboard::kParallelProcesses, 5000);
    for (const QStringList& start : helperStarts().mid(firstStarts.size())) {
      QCOMPARE(start.value(2), second);
    }
    QCOMPARE(storyboard->frameCount(), 0);

    // Closing the viewer stops the rest.
    const QList<QStringList> secondStarts = helperStarts().mid(firstStarts.size());
    storyboard.reset();
    for (const QStringList& start : secondStarts) {
      QTRY_VERIFY_WITH_TIMEOUT(!alive(start.value(0).toLongLong()), 3000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 3000);
  }

  void seekingPausesExtraction() {
    useHelper("wait");
    const QString video = fakeVideo(QStringLiteral("paused.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(600000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), Storyboard::kParallelProcesses, 5000);
    storyboard.setPaused(true);
    QVERIFY(writeFile(modePath(), "success"));
    // The frames in flight land; nothing new starts while the user seeks.
    QTRY_COMPARE_WITH_TIMEOUT(storyboard.frameCount(), Storyboard::kParallelProcesses, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 5000);
    QTest::qWait(300);
    QCOMPARE(helperStarts().size(), Storyboard::kParallelProcesses);
    storyboard.setPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 20000);
    QCOMPARE(helperStarts().size(), StoryboardPlan::sampleTimes(600000).size());
    // Every sample came back 0.5 s before its target, so each is a frame.
    QCOMPARE(storyboard.frameCount(), StoryboardPlan::sampleTimes(600000).size());
    QCOMPARE(storyboard.frameTime(0), 3000 - 500);
  }

  void shortClipsAndAudioOnlyFallBackToTime() {
    useHelper("fail");
    // Too short for a storyboard: nothing starts at all.
    const QString clip = fakeVideo(QStringLiteral("short.mp4"));
    Storyboard brief;
    brief.setSource(clip);
    brief.setDuration(StoryboardPlan::kMinimumDurationMs - 1);
    brief.setActive(true);
    QTest::qWait(Storyboard::kStartDelayMs + 300);
    QCOMPARE(helperStarts().size(), 0);
    QVERIFY(!brief.running());
    QCOMPARE(brief.frameCount(), 0);
    QCOMPARE(brief.frameAt(1000), -1);
    QCOMPARE(brief.frameUrl(0), QString());

    // No video stream: a few attempts, then quietly nothing, and the same
    // file is not tried again until it changes.
    const QString audio = fakeVideo(QStringLiteral("song.m4a"));
    {
      Storyboard storyboard;
      storyboard.setSource(audio);
      storyboard.setDuration(180000);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.failed(), 10000);
      QCOMPARE(storyboard.frameCount(), 0);
      QVERIFY(helperStarts().size() <= 4);
    }
    const qsizetype attempts = helperStarts().size();
    Storyboard again;
    again.setSource(audio);
    again.setDuration(180000);
    again.setActive(true);
    QTRY_VERIFY(again.failed());
    QCOMPARE(helperStarts().size(), attempts);
  }

  void realVideoExtractsCachesAndInvalidates() {
    if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    const QString video = makeStoryboardVideo(m_scratch->filePath(QStringLiteral("pattern.mkv")));
    QVERIFY(!video.isEmpty());

    const int launched = Storyboard::launchedProcesses();
    QElapsedTimer elapsed;
    elapsed.start();
    QList<qint64> times;
    {
      Storyboard storyboard;
      storyboard.setSource(video);
      storyboard.setDuration(12000);
      storyboard.setDevicePixelRatio(2);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 30000);
      qInfo("12 s fixture: %d frames in %lld ms", storyboard.frameCount(), elapsed.elapsed());
      QVERIFY(!storyboard.fromCache());
      QVERIFY(storyboard.frameCount() >= 2);
      QVERIFY(qAbs(storyboard.aspectRatio() - 16.0 / 9.0) < 0.05);
      // One frame per keyframe at or before each sample, at its real time.
      times = framesOf(storyboard);
      QCOMPARE(times, (QList<qint64>{0, 2000, 4000, 6000, 8000, 10000}));

      // The provider serves the frame nearest a time, sized for the box.
      QQmlEngine engine;
      Storyboard::install(engine);
      auto* provider = static_cast<QQuickImageProvider*>(engine.imageProvider(QStringLiteral("storyboard")));
      QVERIFY(provider);
      const QString url = storyboard.frameUrl(storyboard.frameAt(6000));
      QVERIFY(url.startsWith(QStringLiteral("image://storyboard/")));
      QSize size;
      QElapsedTimer decode;
      decode.start();
      const QImage image = provider->requestImage(url.mid(19), &size, {});
      qInfo("frame decode for hover: %lld us", decode.nsecsElapsed() / 1000);
      QVERIFY(!image.isNull());
      QCOMPARE(image.size(), size);
      // Down to the box, never up: this source is smaller than the 2x box.
      QCOMPARE(image.size(), QSize(320, 180));
    }
    QVERIFY(Storyboard::launchedProcesses() > launched);

    // Reopened: straight from the cache, no decoder.
    const int afterFirst = Storyboard::launchedProcesses();
    {
      Storyboard storyboard;
      storyboard.setSource(video);
      storyboard.setDuration(12000);
      storyboard.setDevicePixelRatio(2);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 5000);
      QVERIFY(storyboard.fromCache());
      QCOMPARE(framesOf(storyboard), times);
      QCOMPARE(Storyboard::launchedProcesses(), afterFirst);
    }

    // Changed on disk: extracted again.
    QFile file(video);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-60), QFileDevice::FileModificationTime));
    file.close();
    {
      Storyboard storyboard;
      storyboard.setSource(video);
      storyboard.setDuration(12000);
      storyboard.setDevicePixelRatio(2);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 30000);
      QVERIFY(!storyboard.fromCache());
      QVERIFY(Storyboard::launchedProcesses() > afterFirst);
    }
  }

  void realAudioOnlyFileFails() {
    if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    const QString audio = m_scratch->filePath(QStringLiteral("tone.m4a"));
    QProcess make;
    make.start(m_ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                          QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                          QStringLiteral("sine=frequency=440:duration=8"), QStringLiteral("-c:a"),
                          QStringLiteral("aac"), audio});
    QVERIFY(make.waitForFinished(20000));
    QCOMPARE(make.exitCode(), 0);
    Storyboard storyboard;
    storyboard.setSource(audio);
    storyboard.setDuration(8000);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.failed(), 15000);
    QCOMPARE(storyboard.frameCount(), 0);
  }

private:
  void useHelper(const QByteArray& mode) {
    const QString bin = m_scratch->filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    const QString helper = bin + QStringLiteral("/ffmpeg");
    const QByteArray script = (QStringLiteral("#!/bin/sh\nexec ")
        + shellQuote(QCoreApplication::applicationFilePath())
        + QStringLiteral(" --storyboard-test-helper \"$@\"\n")).toUtf8();
    QVERIFY(writeFile(helper, script));
    QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + m_oldPath));
    QVERIFY(qputenv("OMAROLL_TEST_STORYBOARD_LOG", logPath().toUtf8()));
    QVERIFY(qputenv("OMAROLL_TEST_STORYBOARD_MODE", modePath().toUtf8()));
    QVERIFY(writeFile(modePath(), mode));
  }

  QString fakeVideo(const QString& name) {
    const QString path = m_scratch->filePath(name);
    return writeFile(path, name.toUtf8()) ? path : QString();
  }

  QString logPath() const { return m_scratch->filePath(QStringLiteral("helper.log")); }
  QString modePath() const { return m_scratch->filePath(QStringLiteral("helper.mode")); }

  QList<QStringList> helperStarts() const {
    QList<QStringList> starts;
    for (const QByteArray& line : readFile(logPath()).split('\n')) {
      if (!line.isEmpty()) starts.append(QString::fromUtf8(line).split(QLatin1Char(' ')));
    }
    return starts;
  }

  static QList<qint64> framesOf(const Storyboard& storyboard) {
    QList<qint64> times;
    for (int index = 0; index < storyboard.frameCount(); ++index) {
      times.append(qint64(storyboard.frameTime(index)));
    }
    return times;
  }

  static void restoreEnvironment(const char* name, const QByteArray& value) {
    if (value.isNull()) {
      qunsetenv(name);
    } else {
      qputenv(name, value);
    }
  }

  std::unique_ptr<QTemporaryDir> m_scratch;
  QString m_ffmpeg;
  QByteArray m_oldPath;
  QByteArray m_oldCache;
};

int main(int argc, char** argv) {
  if (argc > 1 && qstrcmp(argv[1], "--storyboard-test-helper") == 0) {
    QCoreApplication app(argc, argv);
    QStringList arguments = app.arguments().mid(2);
    return runHelper(arguments);
  }
  QGuiApplication app(argc, argv);
  StoryboardTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "tst_storyboard.moc"
