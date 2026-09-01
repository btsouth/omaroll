#include "app/AppSettings.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "matte/HueExtractor.h"
#include "matte/MatteComposer.h"
#include "sources/CaptureScanner.h"

#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class OmarollTest : public QObject {
  Q_OBJECT

private slots:
  // --- Classification ---------------------------------------------------

  void classifiesOmarchyScreenshot() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(
        QStringLiteral("screenshot-2026-08-31_23-26-39.png"), kind, captured));
    QCOMPARE(kind, CaptureRecord::Screenshot);
    QCOMPARE(captured.date(), QDate(2026, 8, 31));
    QCOMPARE(captured.time(), QTime(23, 26, 39));
  }

  void classifiesOmarchyRecording() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(
        QStringLiteral("screenrecording-2026-08-31_23-26-39.mp4"), kind, captured));
    QCOMPARE(kind, CaptureRecord::Recording);
    QCOMPARE(captured.time(), QTime(23, 26, 39));
  }

  void classifiesThirdPartyProducers_data() {
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<int>("expected");

    QTest::newRow("grim") << "20260831_23h26m39s_grim.png" << int(CaptureRecord::Screenshot);
    QTest::newRow("flameshot") << "flameshot_2026-08-31.png" << int(CaptureRecord::Screenshot);
    QTest::newRow("gnome style") << "Screenshot from 2026-08-31.png"
                                 << int(CaptureRecord::Screenshot);
    QTest::newRow("obs") << "2026-08-31 23-26-39.mkv" << int(CaptureRecord::Recording);
    QTest::newRow("gsr") << "Video_2026-08-31.mp4" << int(CaptureRecord::Recording);
  }

  void classifiesThirdPartyProducers() {
    QFETCH(QString, fileName);
    QFETCH(int, expected);

    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY2(CaptureScanner::classifyByName(fileName, kind, captured),
             qPrintable(QStringLiteral("no pattern matched %1").arg(fileName)));
    QCOMPARE(int(kind), expected);
  }

  void ignoresUnrelatedNames() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(!CaptureScanner::classifyByName(QStringLiteral("holiday-photo.jpg"), kind, captured));
    QVERIFY(!CaptureScanner::classifyByName(QStringLiteral("IMG_4821.jpeg"), kind, captured));
  }

  // --- Traversal --------------------------------------------------------

  void scanAppliesDepthAndSkipRules() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto write = [&](const QString& relative) {
      const QString full = dir.filePath(relative);
      QDir().mkpath(QFileInfo(full).absolutePath());
      QImage image(4, 4, QImage::Format_RGB32);
      image.fill(Qt::red);
      QVERIFY(image.save(full, "PNG"));
    };

    write(QStringLiteral("top.png"));
    write(QStringLiteral("nested/one.png"));
    write(QStringLiteral("nested/deeper/two.png"));
    write(QStringLiteral(".hidden/secret.png"));
    write(QStringLiteral("thumbnails/cached.png"));

    // A dotfile at the top level is skipped too.
    write(QStringLiteral(".dotfile.png"));

    // Depth 1: only the top-level file.
    auto shallow = CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture,
                                          CaptureRecord::Video}});
    QCOMPARE(shallow.size(), 1);
    QCOMPARE(shallow.first().fileName, QStringLiteral("top.png"));

    // Depth 2 reaches nested/ but not nested/deeper/, and never the skipped
    // directories at any depth.
    auto deeper = CaptureScanner::scan({{dir.path(), 2, CaptureRecord::Picture,
                                         CaptureRecord::Video}});
    QCOMPARE(deeper.size(), 2);

    QStringList names;
    for (const auto& record : deeper) {
      names << record.fileName;
    }
    std::sort(names.begin(), names.end());
    QCOMPARE(names, QStringList({QStringLiteral("one.png"), QStringLiteral("top.png")}));
  }

  void scanDeduplicatesAcrossOverlappingRoots() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(dir.filePath(QStringLiteral("shot.png")), "PNG"));

    // The same directory listed twice, which is exactly what happens when
    // OMARCHY_SCREENSHOT_DIR and XDG_PICTURES_DIR resolve to the same place.
    const auto records = CaptureScanner::scan({
        {dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video},
        {dir.path(), 4, CaptureRecord::Picture, CaptureRecord::Video},
    });
    QCOMPARE(records.size(), 1);
  }

  void videoNameOnImageFileStaysAnImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::green);
    // A recording name on a PNG: the medium has to win, or the grid offers
    // "Trim" on something omacut cannot open.
    QVERIFY(image.save(dir.filePath(QStringLiteral("screenrecording-2026-08-31_10-00-00.png")),
                       "PNG"));

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Screenshot);
  }

  // --- Filtering and sorting -------------------------------------------

  void filterModelSortsAndFilters() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", dir.path().toUtf8()));

    // mtime is set explicitly. A file with a producer name takes its timestamp
    // from the name, but an unnamed one falls back to mtime, and a file written
    // during the test would otherwise always be the newest thing in the library.
    const auto write = [&](const QString& name, int edge, const QDateTime& stamp) {
      QImage image(edge, edge, QImage::Format_RGB32);
      image.fill(Qt::gray);
      const QString path = dir.filePath(name);
      QVERIFY(image.save(path, "PNG"));
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadWrite));
      QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
      file.close();
    };

    write(QStringLiteral("screenshot-2026-08-30_10-00-00.png"), 8,
          QDateTime(QDate(2026, 8, 30), QTime(10, 0)));
    write(QStringLiteral("screenshot-2026-08-31_10-00-00.png"), 64,
          QDateTime(QDate(2026, 8, 31), QTime(10, 0)));
    write(QStringLiteral("beach.png"), 16, QDateTime(QDate(2026, 8, 20), QTime(9, 0)));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);

    QSignalSpy spy(&model, &CaptureModel::countChanged);
    QVERIFY(spy.wait(5000));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 3);

    // Newest first by default.
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("screenshot-2026-08-31_10-00-00.png"));

    proxy.setSortMode(CaptureFilterModel::OldestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    // Size sorting reads the file, not the name.
    proxy.setSortMode(CaptureFilterModel::LargestFirst);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("screenshot-2026-08-31_10-00-00.png"));

    proxy.setSortMode(CaptureFilterModel::NameAscending);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    // Only the two named screenshots are Screenshots; beach.png falls back.
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    proxy.setKindFilter(CaptureRecord::Screenshot);
    QCOMPARE(proxy.count(), 2);

    proxy.setKindFilter(CaptureFilterModel::kAllKinds);
    proxy.setSearchText(QStringLiteral("beach"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.fileNameAt(0), QStringLiteral("beach.png"));

    proxy.setSearchText({});
    QCOMPARE(proxy.count(), 3);
  }

  void hiddenIsExcludedUntilAskedFor() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QString path = dir.filePath(QStringLiteral("screenshot-2026-08-31_10-00-00.png"));
    QVERIFY(image.save(path, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy spy(&model, &CaptureModel::countChanged);
    QVERIFY(spy.wait(5000));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 1);

    // Marks are keyed on the canonical path the scanner produced.
    settings.toggleHidden(proxy.pathAt(0));
    QCOMPARE(proxy.count(), 0);

    proxy.setShowHidden(true);
    QCOMPARE(proxy.count(), 1);

    settings.toggleHidden(proxy.pathAt(0));
    proxy.setShowHidden(false);
    QCOMPARE(proxy.count(), 1);
  }

  // --- Mattes -----------------------------------------------------------

  void hueExtractorFindsDominantColour() {
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(QColor::fromHsv(200, 200, 200));
    const int hue = HueExtractor::dominantHue(image);
    QVERIFY(hue >= 190 && hue <= 210);
  }

  void hueExtractorReportsGreyscaleHonestly() {
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(QColor(120, 120, 120));
    QCOMPARE(HueExtractor::dominantHue(image), -1);
    // Still hands back something usable rather than an invalid colour.
    QVERIFY(HueExtractor::dominantColor(image).isValid());
  }

  void composeGrowsTheCanvasAndKeepsTheCapture() {
    QImage source(200, 100, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(30, 200, 200));

    const QImage result =
        MatteComposer::compose(source, MatteComposer::Adaptive, MatteComposer::Original, 0.10);
    QVERIFY(!result.isNull());
    // Padding is a fraction of the longest edge, applied on both sides.
    QCOMPARE(result.width(), 200 + 2 * 20);
    QCOMPARE(result.height(), 100 + 2 * 20);
  }

  void composeNoneReturnsTheOriginal() {
    QImage source(40, 30, QImage::Format_RGB32);
    source.fill(Qt::magenta);
    const QImage result =
        MatteComposer::compose(source, MatteComposer::None, MatteComposer::Square, 0.2);
    QCOMPARE(result.size(), source.size());
  }

  void composeHonoursAspectWithoutCropping() {
    QImage source(100, 400, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(280, 180, 180));

    const QImage result =
        MatteComposer::compose(source, MatteComposer::Deep, MatteComposer::Wide, 0.05);
    QVERIFY(!result.isNull());

    const qreal ratio = qreal(result.width()) / result.height();
    QVERIFY2(qAbs(ratio - 16.0 / 9.0) < 0.02, qPrintable(QString::number(ratio)));
    // The capture is tall, so the canvas had to grow sideways rather than crop.
    QVERIFY(result.width() >= source.width());
    QVERIFY(result.height() >= source.height());
  }

  void composeStaysWithinTheOutputBudget() {
    // A very tall capture forced into a wide aspect is the case that could
    // otherwise ask for a multi-gigabyte canvas.
    QImage source(400, 12000, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(90, 180, 180));

    const QImage result =
        MatteComposer::compose(source, MatteComposer::Slate, MatteComposer::Social, 0.05);
    QVERIFY(!result.isNull());
    const qint64 pixels = qint64(result.width()) * result.height();
    QVERIFY2(pixels <= MatteComposer::kMaxOutputPixels, qPrintable(QString::number(pixels)));
  }

  void everyMatteProducesSomething() {
    QImage source(80, 60, QImage::Format_RGB32);
    source.fill(QColor::fromHsv(15, 190, 190));

    for (int matte = 0; matte < MatteComposer::MatteCount; ++matte) {
      const QImage result = MatteComposer::compose(
          source, static_cast<MatteComposer::Matte>(matte), MatteComposer::Original, 0.08);
      QVERIFY2(!result.isNull(), qPrintable(QStringLiteral("matte %1 was null").arg(matte)));
    }
  }
};

QTEST_MAIN(OmarollTest)
#include "tst_omaroll.moc"
