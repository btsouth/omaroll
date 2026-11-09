// Seek-preview extraction cost for one video: wall time, decoder CPU, frames,
// cache size, and the cache-hit reopen. Run explicitly, never a CI gate:
//   omaroll_benchmark_storyboard <video> [device pixel ratio]
#include "thumbs/Storyboard.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <sys/resource.h>

namespace {

qint64 childCpuMs() {
  struct rusage usage {};
  getrusage(RUSAGE_CHILDREN, &usage);
  return qint64(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000 +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000;
}

qint64 durationMs(const QString& path) {
  QProcess probe;
  probe.start(QStringLiteral("ffprobe"),
              {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
               QStringLiteral("format=duration"), QStringLiteral("-of"),
               QStringLiteral("default=nw=1:nk=1"), path});
  probe.waitForFinished(30000);
  return qint64(QString::fromLatin1(probe.readAllStandardOutput()).trimmed().toDouble() * 1000);
}

// A run that has not finished by then is reported as such, not waited on.
constexpr int kRunDeadlineMs = 30 * 60 * 1000;

struct Run {
  bool finished = false;
  qint64 wallMs = 0;
  qint64 firstFrameMs = -1;
  qint64 cpuMs = 0;
  int frames = 0;
  int peakProcesses = 0;
  int launched = 0;
  bool fromCache = false;
};

Run run(const QString& path, qint64 duration, qreal ratio) {
  Run result;
  const qint64 cpu = childCpuMs();
  const int launched = Storyboard::launchedProcesses();
  Storyboard storyboard;
  QElapsedTimer elapsed;
  QEventLoop loop;
  QObject::connect(&storyboard, &Storyboard::framesChanged, &loop, [&] {
    if (result.firstFrameMs < 0) result.firstFrameMs = elapsed.elapsed();
  });
  // Done when complete or failed, or when a run ends short, with samples
  // that timed out.
  bool started = false;
  QObject::connect(&storyboard, &Storyboard::stateChanged, &loop, [&] {
    started = started || storyboard.running();
    if (storyboard.complete() || storyboard.failed() || (started && !storyboard.running())) {
      result.finished = true;
      loop.quit();
    }
  });
  QTimer::singleShot(kRunDeadlineMs, &loop, &QEventLoop::quit);
  QTimer sampler;
  QObject::connect(&sampler, &QTimer::timeout, &loop, [&] {
    result.peakProcesses = std::max(result.peakProcesses, Storyboard::runningProcesses());
  });
  sampler.start(5);
  storyboard.setSource(path);
  storyboard.setDuration(qreal(duration));
  storyboard.setDevicePixelRatio(ratio);
  storyboard.setActive(true);
  // The start delay is the viewer's settle time, not extraction.
  elapsed.start();
  loop.exec();
  result.wallMs = elapsed.elapsed() - Storyboard::kStartDelayMs;
  if (result.firstFrameMs >= 0) result.firstFrameMs -= Storyboard::kStartDelayMs;
  result.cpuMs = childCpuMs() - cpu;
  result.frames = storyboard.frameCount();
  result.launched = Storyboard::launchedProcesses() - launched;
  result.fromCache = storyboard.fromCache();
  return result;
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  const QStringList arguments = app.arguments();
  if (arguments.size() < 2) {
    QTextStream(stderr) << "usage: omaroll_benchmark_storyboard <video> [ratio]\n";
    return 2;
  }
  const QString path = QFileInfo(arguments.at(1)).absoluteFilePath();
  const qreal ratio = arguments.size() > 2 ? arguments.at(2).toDouble() : 1.0;
  QTemporaryDir cache;
  qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
  const qint64 duration = durationMs(path);
  if (duration < StoryboardPlan::kMinimumDurationMs) {
    // No storyboard is made for it, so there is nothing to measure.
    QTextStream(stderr) << "omaroll_benchmark_storyboard: " << path << " has no duration ffprobe "
                        << "can read, or is under " << StoryboardPlan::kMinimumDurationMs / 1000
                        << " s\n";
    return 2;
  }

  const Run cold = run(path, duration, ratio);
  const Run warm = run(path, duration, ratio);
  qint64 bytes = 0;
  for (const QFileInfo& entry : QDir(StoryboardCache::directory()).entryInfoList(QDir::Files)) {
    bytes += entry.size();
  }
  QTextStream out(stdout);
  out << "file " << QFileInfo(path).fileName() << " duration_s " << duration / 1000 << " ratio "
      << ratio << '\n'
      << "cold finished " << cold.finished << " wall_ms " << cold.wallMs << " first_frame_ms " << cold.firstFrameMs
      << " decoder_cpu_ms " << cold.cpuMs << " cpu_core_pct "
      << (cold.wallMs > 0 ? 100.0 * cold.cpuMs / cold.wallMs : 0) << " frames " << cold.frames
      << " processes " << cold.launched << " peak_parallel " << cold.peakProcesses << '\n'
      << "warm finished " << warm.finished << " wall_ms " << warm.wallMs << " from_cache " << warm.fromCache << " processes "
      << warm.launched << '\n'
      << "cache_bytes " << bytes << '\n';
  Storyboard::shutdown();
  Storyboard::waitForWorkers();
  return cold.finished && cold.frames > 0 ? 0 : 1;
}
