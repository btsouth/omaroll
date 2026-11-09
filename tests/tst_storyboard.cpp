#include "thumbs/Storyboard.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QProcess>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

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
// until it changes, "success" writes a frame 0.5 s before the target,
// "limit N" succeeds for the first N launches and then waits like "wait",
// "slow S" waits at -ss S and succeeds elsewhere, "failat S" fails at -ss S
// and succeeds elsewhere, "exponent" succeeds with a tiny offset written in
// scientific notation, "crash" dies, "cover" plays a file whose first video
// stream is cover art (red) ahead of the video (blue), and anything else
// fails the way ffmpeg does for a file with no video stream.
int runHelper(const QStringList& arguments) {
  const int seek = arguments.indexOf(QStringLiteral("-ss"));
  const int input = arguments.indexOf(QStringLiteral("-i"));
  const QString at = seek >= 0 ? arguments.value(seek + 1) : QString();
  const QString path = input >= 0 ? arguments.value(input + 1) : QString();
  const QString logPath = qEnvironmentVariable("OMAROLL_TEST_STORYBOARD_LOG");
  QFile log(logPath);
  if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 2;
  log.write(QString::number(QCoreApplication::applicationPid()).toUtf8() + ' ' + at.toUtf8() + ' ' +
            path.toUtf8() + '\n');
  log.close();
  // Which launch this is, by this process's own line, since two run at once.
  qsizetype launch = 0;
  const QByteArray self = QByteArray::number(QCoreApplication::applicationPid()) + ' ';
  for (const QByteArray& line : readFile(logPath).split('\n')) {
    ++launch;
    if (line.startsWith(self)) break;
  }
  const QString modePath = qEnvironmentVariable("OMAROLL_TEST_STORYBOARD_MODE");
  const auto holds = [&] {
    const QByteArray mode = readFile(modePath);
    if (mode.startsWith("wait")) return true;
    if (mode.startsWith("limit ")) return launch > mode.mid(6).toInt();
    if (mode.startsWith("slow ")) return at.toUtf8() == mode.mid(5);
    return false;
  };
  QElapsedTimer elapsed;
  elapsed.start();
  while (holds() && elapsed.elapsed() < 30000) {
    QThread::msleep(10);
  }
  const QByteArray mode = readFile(modePath);
  if (mode == "crash") {
    ::raise(SIGKILL);
  }
  const bool declines = mode.startsWith("failat ") && at.toUtf8() == mode.mid(7);
  if (declines || (mode != "success" && mode != "exponent" && mode != "cover" && !mode.startsWith("limit ") &&
                   !mode.startsWith("slow ") && !mode.startsWith("failat "))) {
    QFile err;
    (void)err.open(stderr, QIODevice::WriteOnly);
    err.write("Stream map '0:v:0' matches no streams.\n");
    return 1;
  }
  QFile err;
  (void)err.open(stderr, QIODevice::WriteOnly);
  // As ffmpeg logs it: the input's path first, then showinfo for the frame
  // written and for frames the filter saw after it.
  err.write("Input #0, matroska,webm, from '" + path.toUtf8() + "':\n");
  err.write(mode == "exponent"
                ? "[Parsed_showinfo_0 @ 0x1a2b] n:   0 pts:   0 pts_time:-1e-05 duration: 1\n"
                : "[Parsed_showinfo_0 @ 0x1a2b] n:   0 pts:  -64 pts_time:-0.5 duration: 1\n");
  err.write("[Parsed_showinfo_0 @ 0x1a2b] n:   1 pts:   64 pts_time:1.5 duration: 1\n");
  QFile out;
  (void)out.open(stdout, QIODevice::WriteOnly);
  if (mode == "cover") {
    const QString stream = arguments.value(arguments.indexOf(QStringLiteral("-map")) + 1);
    // "v" is any video stream, cover art included; "V" leaves it out.
    out.write(jpeg(stream == QStringLiteral("0:V:0") ? QColor(Qt::blue) : QColor(Qt::red)));
    return 0;
  }
  out.write(jpeg(QColor(40, 120, 200)));
  return 0;
}

bool alive(qint64 pid) { return ::kill(pid_t(pid), 0) == 0; }

// Run in a child process: puts a FIFO where the video's cache entry goes, so
// the cache lane blocks opening it, then shuts down. Exits only if shutdown
// and the process's own exit did not wait for that read.
int runShutdownCheck(const QString& video) {
  const QString key = StoryboardCache::key(video, StoryboardPlan::pixelBox(1));
  const QString entry = StoryboardCache::directory() + QLatin1Char('/') + key + QStringLiteral(".storyboard");
  if (key.isEmpty() || !QDir().mkpath(StoryboardCache::directory()) ||
      ::mkfifo(QFile::encodeName(entry).constData(), 0600) != 0) {
    return 3;
  }
  {
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QEventLoop loop;
    QTimer::singleShot(Storyboard::kStartDelayMs + 700, &loop, &QEventLoop::quit);
    loop.exec();
    if (!storyboard.running()) {
      return 4;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    Storyboard::shutdown();
    if (elapsed.elapsed() > 5000) {
      return 5;
    }
  }
  return 0;
}

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
    // The helper is this binary, slow to start under a sanitizer on a busy
    // machine; only the timeout test wants a sample to miss its deadline.
    Storyboard::setFrameTimeoutMs(30000);
  }

  void cleanup() {
    // Every storyboard is gone by now. Their workers may still be saving
    // what they decoded, into this test's cache, so they finish before the
    // environment points anywhere else.
    Storyboard::waitForWorkers();
    // Nothing outlives its storyboard.
    QCOMPARE(Storyboard::runningProcesses(), 0);
    restoreEnvironment("PATH", m_oldPath);
    restoreEnvironment("XDG_CACHE_HOME", m_oldCache);
    Storyboard::setFailureMemoryMs(10 * 60 * 1000);
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
    frames.markSampled(3000);
    frames.markSampled(1000);
    frames.markSampled(5000);
    frames.markSampled(1000);
    QCOMPARE(frames.sampled, (QList<qint64>{1000, 3000, 5000}));
    QVERIFY(StoryboardCache::save(key, frames));
    std::optional<StoryboardFrames> loaded = StoryboardCache::load(key);
    QVERIFY(loaded);
    QCOMPARE(loaded->times, frames.times);
    QCOMPARE(loaded->jpegs, frames.jpegs);
    QCOMPARE(loaded->frameSize, frames.frameSize);
    // Partial unless saved as complete.
    QCOMPARE(loaded->sampled, frames.sampled);
    QVERIFY(!loaded->complete);
    frames.complete = true;
    QVERIFY(StoryboardCache::save(key, frames));
    loaded = StoryboardCache::load(key);
    QVERIFY(loaded && loaded->complete);

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
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), Storyboard::kParallelProcesses, 60000);
    storyboard.setPaused(true);
    QVERIFY(writeFile(modePath(), "success"));
    // The frames in flight land; nothing new starts while the user seeks.
    QTRY_COMPARE_WITH_TIMEOUT(storyboard.frameCount(), Storyboard::kParallelProcesses, 60000);
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 60000);
    QTest::qWait(300);
    QCOMPARE(helperStarts().size(), Storyboard::kParallelProcesses);
    storyboard.setPaused(false);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
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
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.failed(), 60000);
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

  // A sample that times out leaves the storyboard partial. The cache keeps
  // what was decoded, and the next open decodes only the missing sample.
  void timedOutSamplesAreTriedAgainNextTime() {
    // Long enough for the helper to start under a sanitizer, short enough
    // for the test.
    Storyboard::setFrameTimeoutMs(5000);
    useHelper("slow 5.000");
    const QString video = fakeVideo(QStringLiteral("slow.mp4"));
    const qsizetype samples = StoryboardPlan::sampleTimes(20000).size();
    QCOMPARE(samples, 10);
    {
      Storyboard storyboard;
      storyboard.setDuration(20000);
      storyboard.setSource(video);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(!storyboard.running() && storyboard.frameCount() > 0, 60000);
      QCOMPARE(storyboard.frameCount(), samples - 1);
      QVERIFY(!storyboard.complete());
      QVERIFY(!storyboard.failed());
    }
    QCOMPARE(helperStarts().size(), samples);

    QVERIFY(writeFile(modePath(), "success"));
    {
      Storyboard storyboard;
      storyboard.setDuration(20000);
      storyboard.setSource(video);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
      QCOMPARE(storyboard.frameCount(), samples);
      QVERIFY(!storyboard.fromCache());
    }
    QCOMPARE(helperStarts().size(), samples + 1);
    QCOMPARE(helperStarts().constLast().value(1), QStringLiteral("5.000"));

    // Now complete on disk: no decoder at all.
    Storyboard storyboard;
    storyboard.setDuration(20000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 3000);
    QVERIFY(storyboard.fromCache());
    QCOMPARE(storyboard.frameCount(), samples);
    QCOMPARE(helperStarts().size(), samples + 1);
  }

  // Going inactive keeps the frames decoded so far, and the next run picks
  // up the samples still missing instead of starting over.
  void cancelledWorkIsKeptAndResumed() {
    useHelper("limit 6");
    const QString video = fakeVideo(QStringLiteral("toggle.mp4"));
    const qsizetype samples = StoryboardPlan::sampleTimes(60000).size();
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(storyboard.frameCount(), 6, 60000);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), 6 + Storyboard::kParallelProcesses, 60000);
    storyboard.setActive(false);
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 3000);
    QCOMPARE(storyboard.frameCount(), 6);

    QVERIFY(writeFile(modePath(), "success"));
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), samples);
    // The two cut off were tried again; none of the six was.
    QCOMPARE(helperStarts().size(), Storyboard::kParallelProcesses + samples);

    // Closed part way, a second video's frames are in the cache for the next
    // viewer, which decodes the rest.
    QVERIFY(writeFile(logPath(), {}));
    QVERIFY(writeFile(modePath(), "limit 4"));
    const QString other = fakeVideo(QStringLiteral("other.mp4"));
    {
      Storyboard closing;
      closing.setDuration(60000);
      closing.setSource(other);
      closing.setActive(true);
      QTRY_COMPARE_WITH_TIMEOUT(closing.frameCount(), 4, 60000);
      QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), 4 + Storyboard::kParallelProcesses, 60000);
    }
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 3000);
    QVERIFY(writeFile(modePath(), "success"));
    Storyboard reopened;
    reopened.setDuration(60000);
    reopened.setSource(other);
    reopened.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(reopened.complete() || reopened.failed() ||
                                 (reopened.frameCount() > 0 && !reopened.running()), 60000);
    QVERIFY2(reopened.complete(), qPrintable(QStringLiteral("%1 frames, %2 starts")
                                                 .arg(reopened.frameCount())
                                                 .arg(helperStarts().size())));
    QCOMPARE(reopened.frameCount(), samples);
    QCOMPARE(helperStarts().size(), Storyboard::kParallelProcesses + samples);
  }

  // Frames kept across a pause belong to the file as it was: rewritten in
  // between, it starts over instead of mixing old frames with new.
  void rewrittenFileStartsOver() {
    useHelper("limit 4");
    const QString video = fakeVideo(QStringLiteral("rewritten.mp4"));
    const qsizetype samples = StoryboardPlan::sampleTimes(60000).size();
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(storyboard.frameCount(), 4, 60000);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), 4 + Storyboard::kParallelProcesses, 60000);
    storyboard.setActive(false);
    QTRY_COMPARE_WITH_TIMEOUT(Storyboard::runningProcesses(), 0, 3000);
    const QString oldUrl = storyboard.frameUrl(0);
    QTest::qWait(10);
    QVERIFY(writeFile(video, "a different video"));
    QVERIFY(writeFile(modePath(), "success"));
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), samples);
    // The same frame time in the new version is a different image, so it
    // has a different URL: Qt caches images by URL.
    QCOMPARE(storyboard.frameTime(0), 1000 - 500);
    QVERIFY(storyboard.frameUrl(0) != oldUrl);
    QCOMPARE(helperStarts().size(), 4 + Storyboard::kParallelProcesses + samples);
  }

  // A cached video gets its previews while another is still decoding.
  void cacheHitWhileAnotherVideoExtracts() {
    useHelper("wait");
    const QString busy = fakeVideo(QStringLiteral("busy.mp4"));
    const QString cached = fakeVideo(QStringLiteral("cached.mp4"));
    StoryboardFrames frames;
    frames.insert(1000, jpeg(Qt::red));
    frames.insert(9000, jpeg(Qt::blue));
    for (const qint64 target : StoryboardPlan::sampleTimes(10000)) frames.markSampled(target);
    frames.complete = true;
    QVERIFY(StoryboardCache::save(StoryboardCache::key(cached, StoryboardPlan::pixelBox(1)), frames));

    Storyboard extracting;
    extracting.setDuration(600000);
    extracting.setSource(busy);
    extracting.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), Storyboard::kParallelProcesses, 60000);

    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setSource(cached);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 3000);
    QVERIFY(storyboard.fromCache());
    QCOMPARE(storyboard.frameCount(), 2);
    QVERIFY(extracting.running());
    QCOMPARE(Storyboard::runningProcesses(), Storyboard::kParallelProcesses);
    QCOMPARE(helperStarts().size(), Storyboard::kParallelProcesses);
  }

  // A path is only a path: "%2" in it is not a placeholder, and "pts_time:"
  // in a name is not the frame's time.
  void percentInAPathIsNotAPlaceholder() {
    useHelper("success");
    const QString percent = fakeVideo(QStringLiteral("a%2b.mp4"));
    const QString lookalike = fakeVideo(QStringLiteral("a192b.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setActive(true);
    storyboard.setSource(percent);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameTime(0), 1000 - 500);
    storyboard.setSource(lookalike);
    QCOMPARE(storyboard.frameCount(), 0);
    QVERIFY(!storyboard.complete());
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(helperStarts().constLast().value(2), lookalike);
  }

  void frameTimesComeFromShowinfoNotThePath() {
    useHelper("success");
    const QString tricky = fakeVideo(QStringLiteral("clip pts_time:9.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setActive(true);
    storyboard.setSource(tricky);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    // The showinfo line's offset, not the path's, and the first frame's.
    QCOMPARE(storyboard.frameTime(0), 1000 - 500);
    QCOMPARE(storyboard.frameTime(4), 9000 - 500);
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

  // Flipping quickly between videos, with partial work saved at every
  // switch, never mixes up their frames or runs more than two decoders.
  void rapidSwitchingKeepsEachVideosOwnFrames() {
    if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    const QList<QColor> colors{Qt::red, Qt::green, Qt::blue, Qt::yellow};
    QStringList videos;
    for (qsizetype index = 0; index < colors.size(); ++index) {
      const QString path = m_scratch->filePath(QStringLiteral("flip%1.mkv").arg(index));
      QProcess make;
      make.start(m_ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                            QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                            QStringLiteral("-i"),
                            QStringLiteral("color=c=%1:size=320x180:rate=10:duration=40").arg(colors.at(index).name()),
                            QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-g"),
                            QStringLiteral("20"), path});
      QVERIFY(make.waitForFinished(30000) && make.exitCode() == 0);
      videos.append(path);
    }
    const auto matches = [](const QByteArray& bytes, const QColor& expected) {
      const QImage image = QImage::fromData(bytes, "JPG");
      const QColor actual = image.pixelColor(image.width() / 2, image.height() / 2);
      return !image.isNull() && qAbs(actual.red() - expected.red()) < 48 &&
             qAbs(actual.green() - expected.green()) < 48 && qAbs(actual.blue() - expected.blue()) < 48;
    };
    int most = 0;
    QTimer sampler;
    connect(&sampler, &QTimer::timeout, this, [&most] { most = std::max(most, Storyboard::runningProcesses()); });
    sampler.start(2);
    QQmlEngine engine;
    Storyboard::install(engine);
    auto* provider = static_cast<QQuickImageProvider*>(engine.imageProvider(QStringLiteral("storyboard")));
    Storyboard storyboard;
    storyboard.setDuration(40000);
    storyboard.setActive(true);
    for (int round = 0; round < 24; ++round) {
      storyboard.setSource(videos.at(round % videos.size()));
      QTest::qWait((round * 97) % 450);
    }
    storyboard.setSource(videos.constLast());
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QVERIFY(storyboard.frameCount() >= 2);
    for (int index = 0; index < storyboard.frameCount(); ++index) {
      QSize size;
      const QImage image = provider->requestImage(storyboard.frameUrl(index).mid(19), &size, {});
      QByteArray bytes;
      QBuffer buffer(&bytes);
      QVERIFY(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "JPG"));
      QVERIFY(matches(bytes, colors.constLast()));
    }
    for (qsizetype index = 0; index < videos.size(); ++index) {
      const auto cached = StoryboardCache::load(StoryboardCache::key(videos.at(index), StoryboardPlan::pixelBox(1)));
      if (!cached) continue;
      for (const QByteArray& frame : cached->jpegs) QVERIFY2(matches(frame, colors.at(index)), qPrintable(videos.at(index)));
    }
    QVERIFY2(most <= Storyboard::kParallelProcesses, qPrintable(QString::number(most)));
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

  // Another local user can read neither the frames nor the folder they are
  // in, whatever the umask, and an entry made before that becomes private.
  void cacheIsOwnerOnly() {
    const mode_t old = ::umask(022);
    const auto restore = qScopeGuard([old] { ::umask(old); });
    StoryboardFrames frames;
    frames.insert(1000, jpeg(Qt::red));
    const QString key = QString::fromLatin1(QCryptographicHash::hash("private", QCryptographicHash::Md5).toHex());
    QVERIFY(StoryboardCache::save(key, frames));
    const auto mode = [](const QString& path) {
      struct stat info {};
      return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? int(info.st_mode & 0777) : -1;
    };
    const QString entry = StoryboardCache::directory() + QLatin1Char('/') + key + QStringLiteral(".storyboard");
    QCOMPARE(mode(StoryboardCache::directory()), 0700);
    QCOMPARE(mode(entry), 0600);
    // Nothing is left beside it.
    QCOMPARE(QDir(StoryboardCache::directory()).entryList(QDir::Files), QStringList{QFileInfo(entry).fileName()});

    QVERIFY(::chmod(QFile::encodeName(entry).constData(), 0644) == 0);
    QVERIFY(::chmod(QFile::encodeName(StoryboardCache::directory()).constData(), 0755) == 0);
    QVERIFY(StoryboardCache::load(key));
    QCOMPARE(mode(entry), 0600);
    QVERIFY(StoryboardCache::save(key, frames));
    QCOMPARE(mode(StoryboardCache::directory()), 0700);
  }

  // A frame that is too long to be one is turned down before it is read,
  // and one that does not decode makes the entry a miss.
  void cacheRejectsOversizedAndUndecodableFrames() {
    const QString key = QString::fromLatin1(QCryptographicHash::hash("broken", QCryptographicHash::Md5).toHex());
    const QString entry = StoryboardCache::directory() + QLatin1Char('/') + key + QStringLiteral(".storyboard");
    QVERIFY(QDir().mkpath(StoryboardCache::directory()));
    const auto write = [&](quint32 length, const QByteArray& bytes) {
      QByteArray contents;
      QDataStream out(&contents, QIODevice::WriteOnly);
      out.setVersion(QDataStream::Qt_6_5);
      out << quint32(0x4f4d5342) << quint16(3) << QSize(64, 36) << qint32(1) << qint64(1000) << length;
      out.writeRawData(bytes.constData(), int(bytes.size()));
      out << qint32(1) << true << qint64(1000);
      return writeFile(entry, contents);
    };
    const QByteArray good = jpeg(Qt::red);
    QVERIFY(write(quint32(good.size()), good));
    QVERIFY(StoryboardCache::load(key));
    // Far beyond the file and any frame: not allocated, not read.
    QVERIFY(write(0xfffffff0u, good));
    QVERIFY(!StoryboardCache::load(key));
    QVERIFY(write(3u * 1024 * 1024, QByteArray(3 * 1024 * 1024, 'x')));
    QVERIFY(!StoryboardCache::load(key));

    // JPEG markers at both ends and nothing a decoder can use between them.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.gui.imageio.jpeg*=false"));
    const auto quiet = qScopeGuard([] { QLoggingCategory::setFilterRules({}); });
    QByteArray damaged = good;
    for (qsizetype index = 2; index < damaged.size() - 2; ++index) damaged[index] = char(0);
    QVERIFY(write(quint32(damaged.size()), damaged));
    QVERIFY(!StoryboardCache::load(key));
    QVERIFY(write(quint32(good.size() / 2), good.left(good.size() / 2)));
    QVERIFY(!StoryboardCache::load(key));

    // So a video whose entry went bad is decoded again, not shown blank.
    useHelper("success");
    const QString video = fakeVideo(QStringLiteral("damaged.mp4"));
    StoryboardFrames frames;
    frames.insert(500, jpeg(Qt::red));
    frames.insert(2500, damaged);
    for (const qint64 target : StoryboardPlan::sampleTimes(10000)) frames.markSampled(target);
    frames.complete = true;
    QVERIFY(StoryboardCache::save(StoryboardCache::key(video, StoryboardPlan::pixelBox(1)), frames));
    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QVERIFY(!storyboard.fromCache());
    QCOMPARE(helperStarts().size(), StoryboardPlan::sampleTimes(10000).size());
  }

  // Staging files left by a process that died while saving go, once they
  // are old enough not to be another process's save in progress.
  void pruneRemovesAbandonedStagingFiles() {
    StoryboardFrames frames;
    frames.insert(1000, jpeg(Qt::red));
    const QString key = QString::fromLatin1(QCryptographicHash::hash("kept", QCryptographicHash::Md5).toHex());
    QVERIFY(StoryboardCache::save(key, frames));
    const QString entry = StoryboardCache::directory() + QLatin1Char('/') + key + QStringLiteral(".storyboard");
    const QString abandoned = entry + QStringLiteral(".4242.99.tmp");
    const QString saving = entry + QStringLiteral(".4243.99.tmp");
    QVERIFY(writeFile(abandoned, QByteArray(4096, 'a')));
    QVERIFY(writeFile(saving, QByteArray(4096, 'b')));
    const qint64 old = QDateTime::currentSecsSinceEpoch() - StoryboardCache::kAbandonedStagingMs / 1000 - 60;
    const struct timespec when[2] = {{old, 0}, {old, 0}};
    QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(abandoned).constData(), when, 0) == 0);
    StoryboardCache::prune();
    QVERIFY(!QFile::exists(abandoned));
    QVERIFY(QFile::exists(saving));
    QVERIFY(QFile::exists(entry));
    // A save in progress still counts toward the limit.
    StoryboardCache::prune(QFileInfo(entry).size() + 1024);
    QVERIFY(!QFile::exists(entry));
    QVERIFY(QFile::exists(saving));
  }

  // FFmpeg 6.1 can give a tiny offset in scientific notation.
  void frameTimesReadScientificNotation() {
    useHelper("exponent");
    const QString video = fakeVideo(QStringLiteral("exponent.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), 5);
    QCOMPARE(storyboard.frameTime(0), 1000);
    QCOMPARE(storyboard.frameTime(4), 9000);
  }

  // Cover art stored as a video stream ahead of the real one is not what
  // the seek bar shows.
  void attachedCoverArtIsSkipped() {
    useHelper("cover");
    const QString covered = fakeVideo(QStringLiteral("covered.mkv"));
    {
      Storyboard storyboard;
      storyboard.setDuration(10000);
      storyboard.setSource(covered);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    }
    Storyboard::waitForWorkers();
    const auto frames = StoryboardCache::load(StoryboardCache::key(covered, StoryboardPlan::pixelBox(1)));
    QVERIFY(frames && !frames->jpegs.isEmpty());
    for (const QByteArray& bytes : frames->jpegs) {
      const QImage image = QImage::fromData(bytes, "JPG");
      const QColor colour = image.pixelColor(image.width() / 2, image.height() / 2);
      QVERIFY2(colour.blue() > 160 && colour.red() < 80, qPrintable(colour.name()));
    }
  }

  // And with ffmpeg itself, on a file that carries cover art.
  void realCoverArtFileShowsItsVideo() {
    if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg is not installed");
    const QString cover = m_scratch->filePath(QStringLiteral("cover.png"));
    const QString video = m_scratch->filePath(QStringLiteral("covered.mp4"));
    QProcess make;
    make.start(m_ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                          QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                          QStringLiteral("-i"), QStringLiteral("color=c=red:size=320x180"),
                          QStringLiteral("-frames:v"), QStringLiteral("1"), cover});
    QVERIFY(make.waitForFinished(30000) && make.exitCode() == 0);
    make.start(m_ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                          QStringLiteral("-y"), QStringLiteral("-i"), cover, QStringLiteral("-f"),
                          QStringLiteral("lavfi"), QStringLiteral("-i"),
                          QStringLiteral("color=c=blue:size=320x180:rate=10:duration=12"),
                          QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-map"), QStringLiteral("1"),
                          QStringLiteral("-c:v:0"), QStringLiteral("png"), QStringLiteral("-c:v:1"),
                          QStringLiteral("mpeg4"), QStringLiteral("-g"), QStringLiteral("20"),
                          QStringLiteral("-disposition:v:0"), QStringLiteral("attached_pic"), video});
    QVERIFY(make.waitForFinished(30000));
    QVERIFY2(make.exitCode() == 0, make.readAllStandardError().constData());
    Storyboard storyboard;
    storyboard.setDuration(12000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(!storyboard.running() && (storyboard.complete() || storyboard.failed()), 30000);
    QVERIFY(storyboard.complete());
    const auto cached = StoryboardCache::load(StoryboardCache::key(video, StoryboardPlan::pixelBox(1)));
    QVERIFY(cached && !cached->jpegs.isEmpty());
    for (const QByteArray& bytes : cached->jpegs) {
      const QImage image = QImage::fromData(bytes, "JPG");
      const QColor colour = image.pixelColor(image.width() / 2, image.height() / 2);
      QVERIFY2(colour.blue() > 160 && colour.red() < 80, qPrintable(colour.name()));
    }
  }

  // One sample ffmpeg fails on, with the rest fine, may have been a passing
  // problem: it is left for the next open to try again.
  void failedSamplesAreTriedAgainNextTime() {
    useHelper("failat 5.000");
    const QString video = fakeVideo(QStringLiteral("gap.mp4"));
    const qsizetype samples = StoryboardPlan::sampleTimes(20000).size();
    {
      Storyboard storyboard;
      storyboard.setDuration(20000);
      storyboard.setSource(video);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(!storyboard.running() && storyboard.frameCount() > 0, 60000);
      QCOMPARE(storyboard.frameCount(), samples - 1);
      QVERIFY(!storyboard.complete());
      QVERIFY(!storyboard.failed());
    }
    QCOMPARE(helperStarts().size(), samples);
    Storyboard::waitForWorkers();
    QVERIFY(writeFile(modePath(), "success"));
    Storyboard storyboard;
    storyboard.setDuration(20000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), samples);
    QCOMPARE(helperStarts().size(), samples + 1);
    QCOMPARE(helperStarts().constLast().value(1), QStringLiteral("5.000"));
  }

  // Every first sample timing out says the machine was busy, not that the
  // file is bad: back in front, the viewer tries again.
  void allTimeoutsStayRetryable() {
    Storyboard::setFrameTimeoutMs(1500);
    useHelper("wait");
    const QString video = fakeVideo(QStringLiteral("busy.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(helperStarts().size() >= 3 && !storyboard.running(), 60000);
    QVERIFY(!storyboard.failed());
    QCOMPARE(storyboard.frameCount(), 0);

    Storyboard::setFrameTimeoutMs(30000);
    QVERIFY(writeFile(modePath(), "success"));
    storyboard.setActive(false);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), StoryboardPlan::sampleTimes(60000).size());
  }

  // A decoder that crashes says nothing about the file, so the file is not
  // set aside as one without video.
  void crashedDecodersAreNotFailures() {
    useHelper("crash");
    const QString video = fakeVideo(QStringLiteral("crash.mp4"));
    {
      Storyboard storyboard;
      storyboard.setDuration(60000);
      storyboard.setSource(video);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(helperStarts().size() >= 3 && !storyboard.running(), 60000);
      QVERIFY(!storyboard.failed());
    }
    Storyboard::waitForWorkers();
    QVERIFY(writeFile(modePath(), "success"));
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
  }

  // A file ffmpeg turns down is left alone for a while, not for good.
  void declinedFilesAreTriedAgainLater() {
    Storyboard::setFailureMemoryMs(300);
    useHelper("fail");
    const QString video = fakeVideo(QStringLiteral("later.mp4"));
    {
      Storyboard storyboard;
      storyboard.setDuration(60000);
      storyboard.setSource(video);
      storyboard.setActive(true);
      QTRY_VERIFY_WITH_TIMEOUT(storyboard.failed(), 60000);
    }
    QTest::qWait(400);
    QVERIFY(writeFile(modePath(), "success"));
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
  }

  // A new duration is a new plan: the run for the old one stops and the
  // new one covers the whole clip, reusing the samples that still fit.
  void durationChangeRebuildsThePlan() {
    useHelper("wait");
    const QString video = fakeVideo(QStringLiteral("growing.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), Storyboard::kParallelProcesses, 60000);
    const QList<QStringList> first = helperStarts();
    storyboard.setDuration(20000);
    for (const QStringList& start : first) {
      QTRY_VERIFY_WITH_TIMEOUT(!alive(start.value(0).toLongLong()), 3000);
    }
    QVERIFY(writeFile(modePath(), "success"));
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), StoryboardPlan::sampleTimes(20000).size());
    QCOMPARE(storyboard.frameTime(storyboard.frameCount() - 1), 19000 - 500);
    QCOMPARE(helperStarts().size(), first.size() + StoryboardPlan::sampleTimes(20000).size());

    // A duration a few milliseconds off is the same plan near enough.
    const qsizetype starts = helperStarts().size();
    storyboard.setDuration(20033);
    QVERIFY(storyboard.complete());
    QTest::qWait(Storyboard::kStartDelayMs + 200);
    QCOMPARE(helperStarts().size(), starts);
  }

  // A complete entry for a shorter duration is a start, not the whole job.
  void cachedPlanForAnotherDurationIsNotComplete() {
    useHelper("success");
    const QString video = fakeVideo(QStringLiteral("replanned.mp4"));
    StoryboardFrames frames;
    for (const qint64 target : StoryboardPlan::sampleTimes(10000)) {
      frames.insert(target - 500, jpeg(Qt::red));
      frames.markSampled(target);
    }
    frames.complete = true;
    QVERIFY(StoryboardCache::save(StoryboardCache::key(video, StoryboardPlan::pixelBox(1)), frames));
    Storyboard storyboard;
    storyboard.setDuration(20000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QVERIFY(!storyboard.fromCache());
    QCOMPARE(storyboard.frameCount(), StoryboardPlan::sampleTimes(20000).size());
    QCOMPARE(helperStarts().size(), StoryboardPlan::sampleTimes(20000).size() - frames.sampled.size());
  }

  // A file rewritten while it is being decoded starts over. Nothing from
  // the run that saw both versions is kept under the old version's key.
  void fileChangedDuringExtractionStartsOver() {
    useHelper("wait");
    const QString video = fakeVideo(QStringLiteral("changing.mp4"));
    const QString oldKey = StoryboardCache::key(video, StoryboardPlan::pixelBox(1));
    const qsizetype samples = StoryboardPlan::sampleTimes(60000).size();
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts().size(), Storyboard::kParallelProcesses, 60000);
    QTest::qWait(10);
    QVERIFY(writeFile(video, "the same name, another video"));
    const QString newKey = StoryboardCache::key(video, StoryboardPlan::pixelBox(1));
    QVERIFY(newKey != oldKey);
    QVERIFY(writeFile(modePath(), "success"));
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QCOMPARE(storyboard.frameCount(), samples);
    Storyboard::waitForWorkers();
    QVERIFY(!StoryboardCache::load(oldKey));
    const auto saved = StoryboardCache::load(newKey);
    QVERIFY(saved && saved->complete);
    QCOMPARE(helperStarts().size(), Storyboard::kParallelProcesses + samples);
  }

  // The library says when a file changed; that starts over too, with frame
  // URLs of its own so Qt's image cache cannot serve the old frames.
  void versionChangeStartsOver() {
    useHelper("success");
    const QString video = fakeVideo(QStringLiteral("versioned.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(10000);
    storyboard.setVersion(QStringLiteral("1"));
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    const QString before = storyboard.frameUrl(0);
    storyboard.setVersion(QStringLiteral("2"));
    QCOMPARE(storyboard.frameCount(), 0);
    QVERIFY(!storyboard.complete());
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.complete(), 60000);
    QVERIFY(storyboard.frameUrl(0) != before);
  }

  // `running` is a property QML binds to, so it says when it stops.
  void cancellingNotifiesRunning() {
    useHelper("wait");
    const QString video = fakeVideo(QStringLiteral("notify.mp4"));
    Storyboard storyboard;
    storyboard.setDuration(60000);
    storyboard.setSource(video);
    storyboard.setActive(true);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.running(), 5000);
    QSignalSpy state(&storyboard, &Storyboard::stateChanged);
    storyboard.setSource(QString());
    QVERIFY(!storyboard.running());
    QVERIFY(state.size() >= 1);

    storyboard.setSource(video);
    QTRY_VERIFY_WITH_TIMEOUT(storyboard.running(), 5000);
    state.clear();
    storyboard.setDuration(StoryboardPlan::kMinimumDurationMs - 1);
    QVERIFY(!storyboard.running());
    QVERIFY(state.size() >= 1);
  }

  // Exit does not wait on cache I/O that never returns, as on a stalled
  // mount; here a FIFO the cache lane blocks opening.
  void shutdownDoesNotWaitForStalledCacheIo() {
    const QString video = fakeVideo(QStringLiteral("stalled.mp4"));
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--storyboard-shutdown-check"), video});
    const bool exited = child.waitForFinished(20000);
    if (!exited) {
      child.kill();
      child.waitForFinished(3000);
    }
    QVERIFY2(exited, "exit waited on the stalled cache read");
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
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
  if (argc > 2 && qstrcmp(argv[1], "--storyboard-shutdown-check") == 0) {
    QCoreApplication app(argc, argv);
    return runShutdownCheck(app.arguments().at(2));
  }
  QGuiApplication app(argc, argv);
  StoryboardTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "tst_storyboard.moc"
