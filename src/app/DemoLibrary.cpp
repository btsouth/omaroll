#include "app/DemoLibrary.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
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

// Writes a picture made from another demo picture, so the Similar view has
// real sets to show: a smaller recompressed copy, and frames of a burst.
bool saveDerived(const QImage& image, const QString& path, int quality, const QDateTime& stamp) {
  if (image.isNull() || !image.save(path, "JPG", quality)) {
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

  // A resized, recompressed copy of the cabin, as a picture sent through a
  // chat app comes back.
  const QImage cabin(QStringLiteral(":/omaroll/demo/winter-cabin.jpg"));
  saveDerived(cabin.scaled(cabin.size() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
              layout.pictures + QStringLiteral("/winter-cabin-small.jpg"), 70,
              base.addSecs(-(names.indexOf(QStringLiteral("winter-cabin")) * 11 + 2) * 3600 - 60));

  // A short burst of the wave: the camera drifts a little between frames.
  const QImage wave(QStringLiteral(":/omaroll/demo/coastal-wave.jpg"));
  const QDateTime waveStamp =
      base.addSecs(-(names.indexOf(QStringLiteral("coastal-wave")) * 11 + 2) * 3600);
  for (int frame = 2; frame <= 3; ++frame) {
    const int dx = wave.width() * (frame - 1) * 15 / 1000;
    const int dy = wave.height() * (frame - 1) * 8 / 1000;
    const QRect window(dx, dy, wave.width() * 96 / 100, wave.height() * 96 / 100);
    saveDerived(wave.copy(window).scaled(wave.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                layout.pictures + QStringLiteral("/coastal-wave-%1.jpg").arg(frame), 88,
                waveStamp.addSecs(-(frame - 1)));
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

  // One document, so the PDF surfaces are rendered and reviewed like the rest of
  // the window: the page list, the find row and the page controls. It sits in
  // the last day with the other demo media, but not at the top of the grid: the
  // first tile stays a picture. The name is deliberately not one a test builds.
  copyMedia(QStringLiteral(":/omaroll/demo/quiet-horizons-guide.pdf"),
            layout.pictures + QStringLiteral("/quiet-horizons-guide.pdf"),
            base.addSecs(-3 * 3600));

  return layout;
}

} // namespace DemoLibrary
