#include "app/DemoLibrary.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <unistd.h>

namespace {

QString demoRoot() {
  // One private tree per process. Two simultaneous renders must not clear or
  // rewrite each other's fictional library.
  static QTemporaryDir directory([] {
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString base = runtime.isEmpty()
                             ? QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("omaroll-%1").arg(getuid()))
                             : QDir(runtime).filePath(QStringLiteral("omaroll"));
    QDir().mkpath(base);
    return QDir(base).filePath(QStringLiteral("demo-XXXXXX"));
  }());
  return directory.path();
}

void stampFile(const QString& path, const QDateTime& stamp) {
  QFile file(path);
  if (file.open(QIODevice::ReadWrite)) {
    file.setFileTime(stamp, QFileDevice::FileModificationTime);
    file.close();
  }
}

bool copyMedia(const QString& resource, const QString& path, const QDateTime& stamp) {
  if (!QFile::copy(resource, path)) {
    return false;
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther);
  stampFile(path, stamp);
  return true;
}

} // namespace

namespace DemoLibrary {

Layout build() {
  const QString root = demoRoot();

  Layout layout;
  layout.root = root;
  layout.pictures = root + QStringLiteral("/Pictures");
  layout.videos = root + QStringLiteral("/Videos");

  QDir().mkpath(layout.pictures);
  QDir().mkpath(layout.videos);

  // Clear anything from a previous run so the library is exactly what this
  // build generates.
  for (const QString& directory : {layout.pictures, layout.videos}) {
    QDir dir(directory);
    for (const QString& name : dir.entryList(QDir::Files)) {
      dir.remove(name);
    }
  }

  const QDateTime base = QDateTime::currentDateTime();
  const QStringList names = {QStringLiteral("alpine-dawn"),
                             QStringLiteral("desert-curves"),
                             QStringLiteral("green-road"),
                             QStringLiteral("botanical-still-life"),
                             QStringLiteral("rain-courtyard"),
                             QStringLiteral("coastal-wave"),
                             QStringLiteral("forest-falls"),
                             QStringLiteral("blue-hour-street"),
                             QStringLiteral("winter-cabin"),
                             QStringLiteral("canyon-river"),
                             QStringLiteral("rain-reading-room"),
                             QStringLiteral("storm-meadow"),
                             QStringLiteral("black-sand-coast"),
                             QStringLiteral("sunlit-stair"),
                             QStringLiteral("dew-ferns")};

  for (int index = 0; index < names.size(); ++index) {
    const QString& name = names.at(index);
    copyMedia(QStringLiteral(":/omaroll/demo/%1.jpg").arg(name),
              layout.pictures + QLatin1Char('/') + name + QStringLiteral(".jpg"),
              base.addSecs(-(index * 11 + 2) * 3600));
  }

  const QStringList videoNames = {QStringLiteral("clouds-over-canyon"),
                                  QStringLiteral("ocean-surface"),
                                  QStringLiteral("rain-window")};
  for (int index = 0; index < videoNames.size(); ++index) {
    const QString& name = videoNames.at(index);
    copyMedia(QStringLiteral(":/omaroll/demo/%1.mp4").arg(name),
              layout.videos + QLatin1Char('/') + name + QStringLiteral(".mp4"),
              base.addSecs(-(index * 17 + 8) * 3600));
  }

  return layout;
}

} // namespace DemoLibrary
