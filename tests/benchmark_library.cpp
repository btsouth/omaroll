#include "app/AppSettings.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "sources/CaptureScanner.h"
#include "thumbs/ThumbnailCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <sys/resource.h>

// Optional benchmark, not a timing assertion in CI. Each invocation owns its
// settings, cache and generated files. Fixture creation is outside the timers.
int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  const int count = application.arguments().value(1).toInt();
  if (count < 100 || count > 100000) {
    QTextStream(stderr) << "Usage: omaroll_benchmark_library <100..100000>\n";
    return 2;
  }
  QTemporaryDir scratch;
  if (!scratch.isValid()) {
    return 1;
  }
  const QString pictures = scratch.filePath(QStringLiteral("Pictures"));
  const QString empty = scratch.filePath(QStringLiteral("empty"));
  QDir().mkpath(pictures);
  QDir().mkpath(empty);
  qputenv("XDG_CACHE_HOME", scratch.filePath(QStringLiteral("cache")).toUtf8());
  qputenv("XDG_DATA_HOME", scratch.filePath(QStringLiteral("data")).toUtf8());
  for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR",
                           "OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR"}) {
    qputenv(name, empty.toUtf8());
  }
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     scratch.filePath(QStringLiteral("config")));
  QImage image(1000, 750, QImage::Format_RGB32);
  image.fill(Qt::green);
  const QString seed = scratch.filePath(QStringLiteral("seed.png"));
  if (!image.save(seed)) {
    return 1;
  }
  QFile seedFile(seed);
  if (!seedFile.open(QIODevice::ReadOnly)) {
    return 1;
  }
  const QByteArray bytes = seedFile.readAll();
  QStringList files;
  for (int index = 0; index < count; ++index) {
    const QString folder = pictures + QStringLiteral("/folder%1").arg(index / 500);
    if (index % 500 == 0 && !QDir().mkpath(folder)) {
      return 1;
    }
    const QString path = folder + QStringLiteral("/image%1.png").arg(index);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
      return 1;
    }
    files.append(path);
  }
  QJsonObject result{{QStringLiteral("files"), count},
                     {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
                     {QStringLiteral("cache_budget_bytes"), ThumbnailCache::kMaxCacheBytes}};
  QJsonArray scans;
  QElapsedTimer timer;
  for (int run = 0; run < 5; ++run) {
    timer.start();
    const auto records = CaptureScanner::scan({{pictures, 4}});
    scans.append(timer.nsecsElapsed() / 1e6);
    if (records.size() != count) {
      return 1;
    }
  }
  result.insert(QStringLiteral("scan_ms"), scans);
  AppSettings settings;
  if (!settings.addLibraryFolder(QUrl::fromLocalFile(pictures))) {
    return 1;
  }
  timer.start();
  CaptureModel model(&settings);
  CaptureFilterModel proxy;
  proxy.setSourceModel(&model);
  QEventLoop wait;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
  QObject::connect(&model, &CaptureModel::scanningChanged, &wait, [&] {
    if (!model.scanning()) {
      wait.quit();
    }
  });
  timeout.start(60000);
  wait.exec();
  if (model.scanning() || proxy.rowCount() != count) {
    return 1;
  }
  result.insert(QStringLiteral("model_and_proxy_ms"), timer.nsecsElapsed() / 1e6);
  timer.restart();
  proxy.setSortMode(CaptureFilterModel::NameAscending);
  if (proxy.rowCount() != count || proxy.pathAt(0).isEmpty()) {
    return 1;
  }
  result.insert(QStringLiteral("natural_sort_ms"), timer.nsecsElapsed() / 1e6);
  QJsonArray thumbnails;
  for (int run = 0; run < 2; ++run) {
    timer.restart();
    for (int index = 0; index < 20; ++index) {
      if (ThumbnailCache::thumbnail(files[index], QSize(320, 240), 1).isNull()) {
        return 1;
      }
    }
    thumbnails.append(timer.nsecsElapsed() / 1e6);
  }
  result.insert(QStringLiteral("twenty_thumbnails_cold_warm_ms"), thumbnails);
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 1;
  }
  result.insert(QStringLiteral("benchmark_peak_rss_kib"), static_cast<qint64>(usage.ru_maxrss));
  QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
  return 0;
}
