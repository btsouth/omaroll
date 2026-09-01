#include "app/DemoLibrary.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace {

// One fixed seed, so the fictional library is identical on every machine and
// every run. A demo whose contents drift is useless for comparing screenshots.
constexpr quint32 kSeed = 0x0A11CE;

QString demoRoot() {
  static const QString root =
      QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
          .filePath(QStringLiteral("omaroll-demo"));
  return root;
}

// Draws something that reads as a captured window: a title bar, a sidebar and a
// few content blocks. Abstract enough to be obviously invented, structured
// enough to look like a real screenshot in a grid.
QImage fakeWindow(int width, int height, int hue, quint32 variant) {
  QImage image(width, height, QImage::Format_RGB32);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);

  const QColor background = QColor::fromHsv(hue, 40, 26);
  const QColor panel = QColor::fromHsv(hue, 35, 38);
  const QColor accent = QColor::fromHsv((hue + 40) % 360, 170, 210);
  const QColor text = QColor::fromHsv(hue, 20, 190);

  painter.fillRect(image.rect(), background);

  // Title bar
  painter.fillRect(QRect(0, 0, width, 34), QColor::fromHsv(hue, 45, 20));
  painter.setBrush(accent);
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(QPointF(20, 17), 5, 5);
  painter.setBrush(text);
  painter.drawRoundedRect(QRectF(40, 13, width * 0.22, 8), 4, 4);

  // Sidebar
  const int sidebar = static_cast<int>(width * 0.22);
  painter.fillRect(QRect(0, 34, sidebar, height - 34), panel);

  QRandomGenerator random(kSeed + variant);
  for (int row = 0; row < 8; ++row) {
    const int y = 56 + row * 26;
    if (y > height - 20) {
      break;
    }
    QColor bar = text;
    bar.setAlphaF(0.25f + (random.bounded(40) / 100.0f));
    painter.setBrush(bar);
    painter.drawRoundedRect(QRectF(16, y, sidebar - 34 - random.bounded(20), 9), 4, 4);
  }

  // Content blocks
  const int contentX = sidebar + 22;
  const int contentWidth = width - contentX - 22;

  QLinearGradient hero(contentX, 56, contentX + contentWidth, 56 + height * 0.3);
  hero.setColorAt(0.0, accent.darker(140));
  hero.setColorAt(1.0, QColor::fromHsv((hue + 90) % 360, 150, 120));
  painter.setBrush(hero);
  painter.drawRoundedRect(QRectF(contentX, 56, contentWidth, height * 0.3), 8, 8);

  int y = static_cast<int>(56 + height * 0.3 + 22);
  while (y < height - 30) {
    const int blockHeight = 14 + random.bounded(8);
    QColor block = panel.lighter(120 + random.bounded(50));
    painter.setBrush(block);
    painter.drawRoundedRect(
        QRectF(contentX, y, contentWidth * (0.5 + random.bounded(50) / 100.0), blockHeight), 5, 5);
    y += blockHeight + 14;
  }

  painter.end();
  return image;
}

// A single frame standing in for a recording. Mostly a still, with a timecode
// strip so it is recognisably video content in a grid.
QImage fakeFrame(int width, int height, int hue, quint32 variant) {
  QImage image = fakeWindow(width, height, hue, variant);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);

  painter.setBrush(QColor(0, 0, 0, 150));
  painter.setPen(Qt::NoPen);
  painter.drawRect(QRect(0, height - 26, width, 26));

  QColor progress = QColor::fromHsv((hue + 40) % 360, 180, 220);
  painter.setBrush(progress);
  const int played = static_cast<int>(width * (0.2 + (variant % 6) * 0.12));
  painter.drawRect(QRect(0, height - 26, played, 3));

  painter.end();
  return image;
}

// Encodes a generated frame into a short real clip, so demo recordings produce
// genuine thumbnails and hover-scrub rather than a placeholder. ffmpeg ships as
// an omacut dependency on Omarchy; without it the demo falls back to stills.
bool encodeClip(const QImage& frame, const QString& outputPath) {
  const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty()) {
    return false;
  }

  const QString framePath = outputPath + QStringLiteral(".frame.png");
  if (!frame.save(framePath, "PNG")) {
    return false;
  }

  QProcess process;
  process.start(ffmpeg, {
                            QStringLiteral("-y"),
                            QStringLiteral("-loglevel"), QStringLiteral("error"),
                            QStringLiteral("-loop"), QStringLiteral("1"),
                            QStringLiteral("-i"), framePath,
                            QStringLiteral("-t"), QStringLiteral("3"),
                            QStringLiteral("-r"), QStringLiteral("12"),
                            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                            outputPath,
                        });
  const bool ok = process.waitForFinished(20000) &&
                  process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
  QFile::remove(framePath);
  return ok;
}

void stampFile(const QString& path, const QDateTime& stamp) {
  QFile file(path);
  if (file.open(QIODevice::ReadWrite)) {
    file.setFileTime(stamp, QFileDevice::FileModificationTime);
    file.close();
  }
}

void writeCapture(const QString& path, const QImage& image, const QDateTime& stamp) {
  image.save(path, "PNG");
  // Timestamps come from the filename for these, but set mtime too so sorting
  // by date is right even if a name pattern ever stops matching.
  QFile file(path);
  if (file.open(QIODevice::ReadWrite)) {
    file.setFileTime(stamp, QFileDevice::FileModificationTime);
    file.close();
  }
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

  // Spread across several days so day grouping and the day header have
  // something real to do.
  const QDateTime base = QDateTime::currentDateTime();
  const QList<int> hourOffsets = {1,   2,   4,   6,   8,   10,  13,  16,
                                  26,  28,  30,  33,  36,  40,  44,  47,
                                  51,  54,  58,  62,  66,  70,  76,  80,
                                  99,  103, 108, 112, 126, 131, 150, 156};

  int index = 0;
  for (int offset : hourOffsets) {
    const QDateTime stamp = base.addSecs(-offset * 3600);
    const int hue = (index * 47) % 360;
    const bool video = index % 3 == 2;

    const QString name =
        video ? QStringLiteral("screenrecording-%1.mp4")
                    .arg(stamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")))
              : QStringLiteral("screenshot-%1.png")
                    .arg(stamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));

    if (video) {
      const QString clipPath = layout.videos + QLatin1Char('/') + name;
      const QImage frame = fakeFrame(1280, 800, hue, static_cast<quint32>(index));
      if (encodeClip(frame, clipPath)) {
        stampFile(clipPath, stamp);
      } else {
        // No encoder available. Fall back to a still under a screenshot name so
        // the demo library is still coherent rather than showing broken tiles.
        const QString stillName = QStringLiteral("screenshot-%1.png")
                                      .arg(stamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
        writeCapture(layout.pictures + QLatin1Char('/') + stillName, frame, stamp);
      }
    } else {
      writeCapture(layout.pictures + QLatin1Char('/') + name,
                   fakeWindow(1440, 900, hue, static_cast<quint32>(index)), stamp);
    }
    ++index;
  }

  return layout;
}

} // namespace DemoLibrary
