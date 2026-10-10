// Very large pictures: what they cost to decode, which work is allowed to
// decode them, and the reduced copy the viewers show.
//
// Its own process, because the budgets and Qt's allocation limit are global.
// Small pictures and small limits stand in for 150-megapixel scans.

#include "edit/ImageEditor.h"
#include "sources/FileVersion.h"
#include "sources/ImageBudget.h"
#include "thumbs/LargeImageProvider.h"
#include "thumbs/ThumbnailCache.h"

#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>

#include <atomic>
#include <memory>

namespace {

constexpr qint64 kKiB = 1024;

// A header that claims |size| at 32 bits per pixel, with no pixels behind it:
// enough to be measured, never decoded.
bool writeBitmapHeader(const QString& path, const QSize& size) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream << quint16(0x4d42) << quint32(54) << quint16(0) << quint16(0) << quint32(54)
         << quint32(40) << qint32(size.width()) << qint32(size.height()) << quint16(1)
         << quint16(32) << quint32(0) << quint32(0) << qint32(0) << qint32(0) << quint32(0)
         << quint32(0);
  return stream.status() == QDataStream::Ok;
}

// A single EXIF Orientation tag after the SOI marker, which Qt cannot write;
// the same segment tst_omaroll assembles.
QByteArray withExifOrientation(const QByteArray& jpeg, quint16 orientation) {
  QByteArray tiff = QByteArray::fromHex("4d4d002a00000008000101120003000000010000000000000000");
  tiff[18] = char(orientation >> 8);
  tiff[19] = char(orientation & 0xFF);
  const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
  const int length = int(payload.size()) + 2;
  return jpeg.left(2) + QByteArray::fromHex("ffe1") + char(length >> 8) + char(length & 0xFF) +
         payload + jpeg.mid(2);
}

// A brighter left half, so a turned decode can be told from a plain one.
// |orientation| is an EXIF value for a JPEG.
bool writeImage(const QString& path, const QSize& size, QImage::Format format, int orientation = 1) {
  QImage image(size, format);
  image.fill(Qt::darkCyan);
  for (int y = 0; y < size.height(); ++y) {
    for (int x = 0; x < size.width() / 2; ++x) image.setPixelColor(x, y, Qt::yellow);
  }
  if (orientation == 1) return image.save(path);
  QByteArray jpeg;
  QBuffer buffer(&jpeg);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG")) return false;
  QFile file(path);
  const QByteArray tagged = withExifOrientation(jpeg, quint16(orientation));
  return file.open(QIODevice::WriteOnly) && file.write(tagged) == tagged.size();
}

ImageBudget::Estimate made(const QSize& size, qint64 bytes, bool scalable) {
  ImageBudget::Estimate estimate;
  estimate.stored = size;
  estimate.oriented = size;
  estimate.bytes = bytes;
  estimate.formatKnown = true;
  estimate.scalable = scalable;
  return estimate;
}

} // namespace

class LargeImageTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    QVERIFY(m_dir.isValid());
    QVERIFY(qputenv("XDG_CACHE_HOME", m_dir.filePath(QStringLiteral("cache")).toUtf8()));
    QCOMPARE(QImageReader::allocationLimit(), 256);
  }

  void cleanup() {
    ImageBudget::setLimits({});
    QImageReader::setAllocationLimit(256);
  }

  void theStartupLimitFollowsMemoryUnlessQtIsToldOtherwise() {
    constexpr qint64 gib = 1024 * ImageBudget::kMiB;
    QCOMPARE(ImageBudget::automaticLimitMb(0), 256);
    QCOMPARE(ImageBudget::automaticLimitMb(1 * gib), 256);
    QCOMPARE(ImageBudget::automaticLimitMb(4 * gib), 512);
    QCOMPARE(ImageBudget::automaticLimitMb(8 * gib), 1024);
    QCOMPARE(ImageBudget::automaticLimitMb(16 * gib), 2048);
    QCOMPARE(ImageBudget::automaticLimitMb(128 * gib), 2048);
    // QT_IMAGEIO_MAXALLOC, as Qt parses it, is left for Qt to apply.
    QCOMPARE(ImageBudget::startupLimitMb("1024", 16 * gib), -1);
    QCOMPARE(ImageBudget::startupLimitMb(" 0 ", 16 * gib), -1);
    QCOMPARE(ImageBudget::startupLimitMb("0x400", 16 * gib), -1);
    QCOMPARE(ImageBudget::startupLimitMb("", 16 * gib), 2048);
    QCOMPARE(ImageBudget::startupLimitMb("lots", 8 * gib), 1024);
    QCOMPARE(ImageBudget::startupLimitMb("-1", 8 * gib), 1024);
  }

  void anEstimateIsWhatQtAllocates() {
    // The reporter's scan: 14172 x 10261 at 32 bits, about 555 MiB.
    const QString scan = m_dir.filePath(QStringLiteral("scan.bmp"));
    QVERIFY(writeBitmapHeader(scan, QSize(14172, 10261)));
    const ImageBudget::Estimate large = ImageBudget::estimate(scan);
    QVERIFY(large.valid());
    QVERIFY(large.formatKnown);
    QVERIFY(!large.scalable);
    QCOMPARE(large.stored, QSize(14172, 10261));
    QCOMPARE(large.bytes, qint64(581675568));
    QCOMPARE(ImageBudget::formatBytes(large.bytes), QStringLiteral("555 MiB"));
    QCOMPARE(ImageBudget::formatBytes(3300 * ImageBudget::kMiB), QStringLiteral("3.2 GiB"));

    // Rows are padded to 32 bits, as Qt pads them.
    const QString grey = m_dir.filePath(QStringLiteral("grey.png"));
    QVERIFY(writeImage(grey, QSize(301, 200), QImage::Format_Grayscale8));
    QCOMPARE(ImageBudget::estimate(grey).bytes, qint64(304 * 200));
    const QString colour = m_dir.filePath(QStringLiteral("colour.png"));
    QVERIFY(writeImage(colour, QSize(301, 200), QImage::Format_RGB32));
    QCOMPARE(ImageBudget::estimate(colour).bytes, qint64(301 * 4 * 200));

    const QString photo = m_dir.filePath(QStringLiteral("photo.jpg"));
    QVERIFY(writeImage(photo, QSize(320, 200), QImage::Format_RGB32));
    QVERIFY(ImageBudget::estimate(photo).scalable);

    // A turned JPEG is shown on its side.
    const QString turned = m_dir.filePath(QStringLiteral("turned.jpg"));
    QVERIFY(writeImage(turned, QSize(320, 200), QImage::Format_RGB32, 6));
    const ImageBudget::Estimate sideways = ImageBudget::estimate(turned);
    QCOMPARE(sideways.stored, QSize(320, 200));
    QCOMPARE(sideways.oriented, QSize(200, 320));

    // Qt's JPEG decoder reduces by eighths, to the first size at least the
    // one asked for, so a target over half the picture decodes most of it.
    ImageBudget::Estimate jpeg = made(QSize(3000, 2000), 3000 * 2000 * 4, true);
    jpeg.jpeg = true;
    QCOMPARE(ImageBudget::bytesAt(jpeg, QSize(2048, 1365)), qint64(2250 * 1500 * 4));
    QCOMPARE(ImageBudget::bytesAt(jpeg, QSize(300, 200)), qint64(375 * 250 * 4));
    QCOMPARE(ImageBudget::bytesAt(jpeg, QSize(2990, 1990)), jpeg.bytes);
    QCOMPARE(ImageBudget::bytesAt(jpeg, QSize()), jpeg.bytes);
    const ImageBudget::Estimate png = made(QSize(3000, 2000), 3000 * 2000 * 4, false);
    QCOMPARE(ImageBudget::bytesAt(png, QSize(300, 200)), png.bytes);
    QCOMPARE(ImageBudget::bytesAt(made(QSize(3000, 2000), 3000 * 2000 * 4, true), QSize(300, 200)),
             qint64(300 * 200 * 4));

    QVERIFY(!ImageBudget::estimate(m_dir.filePath(QStringLiteral("missing.png"))).valid());
    // A hostile header saturates instead of wrapping.
    const QString hostile = m_dir.filePath(QStringLiteral("hostile.bmp"));
    QVERIFY(writeBitmapHeader(hostile, QSize(2000000000, 2000000000)));
    const ImageBudget::Estimate huge = ImageBudget::estimate(hostile);
    if (huge.valid()) QVERIFY(huge.bytes > 1024 * 1024 * ImageBudget::kMiB);
  }

  // Thumbnails, similarity and the sheets' previews run several at a time.
  // Past the background budget, whole decodes take turns; past Qt's limit
  // they are refused. A JPEG decoded at a reduced size never waits.
  void largeBackgroundDecodesTakeTurns() {
    ImageBudget::setLimits({.backgroundBytes = 64 * kKiB});
    const QString png = m_dir.filePath(QStringLiteral("budget.png"));
    const QString other = m_dir.filePath(QStringLiteral("budget-2.png"));
    const QString jpeg = m_dir.filePath(QStringLiteral("budget.jpg"));
    QVERIFY(writeImage(png, QSize(320, 200), QImage::Format_RGB32));
    QVERIFY(writeImage(other, QSize(320, 200), QImage::Format_RGB32));
    QVERIFY(writeImage(jpeg, QSize(320, 200), QImage::Format_RGB32));

    QImageReader whole(png);
    whole.setScaledSize(QSize(32, 20));
    QVERIFY(!ImageBudget::fitsBudget(whole, ImageBudget::limits().backgroundBytes));
    QImageReader fitting(jpeg);
    fitting.setScaledSize(QSize(32, 20));
    QVERIFY(ImageBudget::fitsBudget(fitting, ImageBudget::limits().backgroundBytes));

    {
      QImageReader holder(png);
      const ImageBudget::LargeDecode held(holder);
      QVERIFY(held.allowed());
      // The next very large decode waits for the slot, and gives up when its
      // request is cancelled.
      std::atomic_bool cancel{false};
      QFuture<QImage> waiting = QtConcurrent::run([&other, &cancel] {
        QImageReader reader(other);
        return ImageBudget::readBounded(reader, [&cancel] { return cancel.load(); });
      });
      QTest::qWait(150);
      QVERIFY(!waiting.isFinished());
      QImageReader reduced(jpeg);
      reduced.setScaledSize(QSize(32, 20));
      QCOMPARE(ImageBudget::readBounded(reduced).size(), QSize(32, 20));
      cancel.store(true);
      waiting.waitForFinished();
      QVERIFY(waiting.result().isNull());
    }
    QImageReader after(other);
    QCOMPARE(ImageBudget::readBounded(after).size(), QSize(320, 200));
    // A tile for the PNG too, from its turn at a whole decode.
    QVERIFY(!ThumbnailCache::thumbnail(png, QSize(48, 30), 1.0).isNull());

    // Past Qt's limit nothing waits: it is refused.
    const QString scan = m_dir.filePath(QStringLiteral("budget-scan.png"));
    QVERIFY(writeImage(scan, QSize(1200, 800), QImage::Format_RGB32));
    QImageReader::setAllocationLimit(1);
    QImageReader refused(scan);
    QVERIFY(!ImageBudget::LargeDecode(refused).allowed());
    QVERIFY(ImageBudget::readBounded(refused).isNull());

    ImageEditor editor;
    QCOMPARE(editor.previewProblem(scan),
             QStringLiteral("This picture needs about 4 MiB; the limit on this computer is 1 MiB"));
    QVERIFY(editor.previewProblem(png).isEmpty());
    QVERIFY(editor.previewProblem(jpeg).isEmpty());
  }

  void aLargePictureIsShownFromAReducedCopy() {
    ImageBudget::setLimits({.displayBytes = 1 * ImageBudget::kMiB, .textureEdge = 4000});
    const QSize size(1000, 1000);

    // Small enough: shown as it is.
    QVERIFY(!ImageBudget::plan(made(size, 512 * kKiB, false)).reduced);
    QVERIFY(!ImageBudget::plan({}).reduced);

    // Large but within Qt's limit: a reduced copy, then full resolution.
    ImageBudget::Plan plan = ImageBudget::plan(made(size, 4 * ImageBudget::kMiB, false));
    QVERIFY(plan.reduced);
    QCOMPARE(plan.size, size);
    QVERIFY(plan.detail);
    QVERIFY(plan.note.isEmpty());
    QVERIFY(plan.error.isEmpty());

    // Longer than a texture can be: a reduced copy without detail, however
    // little memory it needs.
    plan = ImageBudget::plan(made(QSize(5000, 10), 200 * kKiB, false));
    QVERIFY(plan.reduced);
    QVERIFY(!plan.detail);
    QVERIFY(plan.note.contains(QStringLiteral("graphics card")));
    // Its copy is as long as a texture can be, not as long as the screen.
    QCOMPARE(plan.edge, 4000);
    QCOMPARE(ImageBudget::reducedEdge(plan), 4000);
    QVERIFY(ImageBudget::reducedEdge() >= 2048);
    QCOMPARE(ImageBudget::plan(made(size, 4 * ImageBudget::kMiB, false)).edge, 0);

    // Past Qt's limit: a JPEG still opens reduced; anything else cannot.
    QImageReader::setAllocationLimit(2);
    plan = ImageBudget::plan(made(size, 4 * ImageBudget::kMiB, true));
    QVERIFY(plan.reduced);
    QVERIFY(!plan.detail);
    QVERIFY(plan.error.isEmpty());
    QCOMPARE(plan.note, QStringLiteral("Showing a reduced copy. Full resolution needs about 4 MiB; "
                                       "the limit on this computer is 2 MiB"));
    plan = ImageBudget::plan(made(size, 4 * ImageBudget::kMiB, false));
    QVERIFY(plan.reduced);
    QCOMPARE(plan.error,
             QStringLiteral("This picture needs about 4 MiB; the limit on this computer is 2 MiB"));

    // A limit lower than the display threshold makes a picture large too.
    ImageBudget::setLimits({.displayBytes = 64 * ImageBudget::kMiB});
    QVERIFY(ImageBudget::plan(made(size, 3 * ImageBudget::kMiB, true)).reduced);
    QImageReader::setAllocationLimit(0);
    QVERIFY(!ImageBudget::plan(made(size, 3 * ImageBudget::kMiB, false)).reduced);
    QVERIFY(ImageBudget::decodeRefusal(made(size, 3 * ImageBudget::kMiB, false)).isEmpty());
  }

  void theReducedCopyIsUprightAndBounded() {
    const QString png = m_dir.filePath(QStringLiteral("copy.png"));
    const QString turned = m_dir.filePath(QStringLiteral("copy-turned.jpg"));
    QVERIFY(writeImage(png, QSize(1200, 800), QImage::Format_RGB32));
    QVERIFY(writeImage(turned, QSize(1200, 800), QImage::Format_RGB32, 6));

    LargeImageProvider provider;
    QCOMPARE(read(provider, png, 300).size(), QSize(300, 200));
    const QImage upright = read(provider, turned, 300);
    QCOMPARE(upright.size(), QSize(200, 300));
    // The yellow half of the stored picture is on top once turned.
    QVERIFY(upright.pixelColor(100, 20).red() > 180);
    QVERIFY(upright.pixelColor(100, 280).red() < 80);
    // Never enlarged.
    QCOMPARE(read(provider, png, 4096).size(), QSize(1200, 800));
    QVERIFY(read(provider, m_dir.filePath(QStringLiteral("gone.png")), 300).isNull());

    // A whole decode Qt refuses is reported, not crashed on.
    QImageReader::setAllocationLimit(1);
    QVERIFY(read(provider, png, 300).isNull());
    // A JPEG's reduced decode shrinks until the limit allows it: 2048 wide
    // would decode at 2250 x 1500, past 10 MiB.
    const QString photo = m_dir.filePath(QStringLiteral("copy-photo.jpg"));
    QVERIFY(writeImage(photo, QSize(3000, 2000), QImage::Format_RGB32));
    QImageReader::setAllocationLimit(10);
    QCOMPARE(read(provider, photo, 2048).size(), QSize(1536, 1024));
    QImageReader::setAllocationLimit(256);
    QCOMPARE(read(provider, photo, 2048).size(), QSize(2048, 1365));
    provider.shutdown();
  }

private:
  QImage read(LargeImageProvider& provider, const QString& path, int edge) {
    const QString url = ImageBudget::reducedUrl(path, FileVersion::key(path), edge).toString();
    const QString prefix = QStringLiteral("image://large/");
    if (!url.startsWith(prefix)) return {};
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(url.mid(prefix.size()), QSize()));
    QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
    if (!finished.wait(10000) && finished.isEmpty()) return {};
    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    return factory ? factory->image() : QImage();
  }

  QTemporaryDir m_dir;
};

int main(int argc, char** argv) {
  // Qt prefers this over any limit set in code; the tests set their own.
  qunsetenv("QT_IMAGEIO_MAXALLOC");
  QGuiApplication app(argc, argv);
  LargeImageTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "tst_large_images.moc"
