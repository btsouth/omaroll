// Malformed, truncated and lying media: what a hostile file does to the
// scanner, the image budget, the thumbnail cache, the large-picture provider
// and the PDF helpers.
//
// Exit values matter less than reach. This suite runs under AddressSanitizer
// and UndefinedBehaviorSanitizer in CI, so a header that overflows a size, a
// container that lies about its dimensions, or a decoder that reads past a
// truncated buffer is reported. Every hostile file is driven through the same
// guarded reads the library and the viewers use, so a missing guard shows up
// as a crash or an allocation rather than as a quietly wrong tile.
//
// Its own process, like tst_large_images: the image budget and Qt's allocation
// limit are process-wide.

#include "library/MediaInspector.h"
#include "pdf/PdfSupport.h"
#include "sources/CaptureScanner.h"
#include "sources/FileVersion.h"
#include "sources/ImageBudget.h"
#include "thumbs/LargeImageProvider.h"
#include "thumbs/ThumbnailCache.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMediaPlayer>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <optional>

namespace {

bool writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// The PNG chunk CRC. Qt has no public CRC-32, and the decoder checks it, so a
// lying IHDR still has to be well formed to reach the size handling.
quint32 crc32(const QByteArray& data) {
  static quint32 table[256] = {};
  static bool ready = false;
  if (!ready) {
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    ready = true;
  }
  quint32 c = 0xffffffffu;
  for (const char byte : data) {
    c = table[(c ^ quint8(byte)) & 0xff] ^ (c >> 8);
  }
  return c ^ 0xffffffffu;
}

void appendBE16(QByteArray& out, quint16 value) {
  out += char(value >> 8);
  out += char(value & 0xff);
}

void appendBE32(QByteArray& out, quint32 value) {
  out += char((value >> 24) & 0xff);
  out += char((value >> 16) & 0xff);
  out += char((value >> 8) & 0xff);
  out += char(value & 0xff);
}

void appendLE16(QByteArray& out, quint16 value) {
  out += char(value & 0xff);
  out += char(value >> 8);
}

void appendLE32(QByteArray& out, quint32 value) {
  out += char(value & 0xff);
  out += char((value >> 8) & 0xff);
  out += char((value >> 16) & 0xff);
  out += char((value >> 24) & 0xff);
}

QByteArray pngChunk(const char* type, const QByteArray& payload) {
  QByteArray body(type, 4);
  body += payload;
  QByteArray chunk;
  appendBE32(chunk, quint32(payload.size()));
  chunk += body;
  appendBE32(chunk, crc32(body));
  return chunk;
}

// A PNG whose IHDR claims a gigapixel canvas with an IDAT far too short to
// hold a single row of it. The allocation limit is the only thing between this
// header and a multi-gigabyte reservation.
QByteArray pngWithClaimedSize(quint32 width, quint32 height) {
  QByteArray png = QByteArray::fromHex("89504e470d0a1a0a");
  QByteArray ihdr;
  appendBE32(ihdr, width);
  appendBE32(ihdr, height);
  ihdr += char(8); // bit depth
  ihdr += char(6); // colour type: RGBA
  ihdr += char(0);
  ihdr += char(0);
  ihdr += char(0);
  png += pngChunk("IHDR", ihdr);
  png += pngChunk("IDAT", QByteArray::fromHex("789c030000000001"));
  png += pngChunk("IEND", QByteArray());
  return png;
}

// A logical screen of the largest a GIF can claim, with an image descriptor
// that repeats it and no image data behind that: what Qt reports before it can
// allocate anything.
QByteArray gifWithClaimedSize(quint16 width, quint16 height) {
  QByteArray gif = "GIF89a";
  appendLE16(gif, width);
  appendLE16(gif, height);
  gif += char(0x00); // no global colour table
  gif += char(0x00); // background colour index
  gif += char(0x00); // pixel aspect ratio
  // The frame Qt would draw, so the claimed size survives to the estimate
  // rather than being rejected as a header-parse error.
  gif += char(0x2c); // image separator
  appendLE16(gif, 0);
  appendLE16(gif, 0);
  appendLE16(gif, width);
  appendLE16(gif, height);
  gif += char(0x00); // no local colour table
  gif += char(0x3b); // trailer, with the image data intentionally absent
  return gif;
}

// A WebP VP8X header claiming a huge canvas, with no image data.
QByteArray webpWithClaimedSize(quint32 width, quint32 height) {
  QByteArray payload;
  payload += char(0x00);         // no alpha, no animation
  payload += QByteArray(3, '\0'); // reserved
  const quint32 w = width - 1;
  const quint32 h = height - 1;
  payload += char(w & 0xff);
  payload += char((w >> 8) & 0xff);
  payload += char((w >> 16) & 0xff);
  payload += char(h & 0xff);
  payload += char((h >> 8) & 0xff);
  payload += char((h >> 16) & 0xff);
  QByteArray chunk = "VP8X";
  appendLE32(chunk, quint32(payload.size()));
  chunk += payload;
  if (chunk.size() % 2 != 0) {
    chunk += char(0);
  }
  QByteArray riff = "RIFF";
  appendLE32(riff, quint32(4 + chunk.size()));
  riff += "WEBP";
  riff += chunk;
  return riff;
}

// An ISO base media file with a brand and nothing else: what a truncated
// download of any modern photo or video looks like.
QByteArray ftypOnly(const char* brand) {
  QByteArray payload(brand, 4);
  payload += QByteArray(4, '\0');
  payload += QByteArray(brand, 4);
  payload += "mif1";
  QByteArray box;
  appendBE32(box, quint32(8 + payload.size()));
  box += "ftyp";
  box += payload;
  return box;
}

QByteArray jpegBytes(const QSize& size) {
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::darkCyan);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "JPEG", 90);
  return bytes;
}

QByteArray pngBytes(const QSize& size) {
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::darkGreen);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

QByteArray jpegWithApp1(const QByteArray& payload, const QByteArray& base) {
  const int length = payload.size() + 2;
  QByteArray out = base.left(2);
  out += QByteArray::fromHex("ffe1");
  out += char(length >> 8);
  out += char(length & 0xff);
  out += payload;
  out += base.mid(2);
  return out;
}

// A TIFF directory claiming 65535 entries with none behind it, wrapped in the
// EXIF segment Qt cannot write itself.
QByteArray exifWithHugeIfd() {
  QByteArray tiff = "MM";
  appendBE16(tiff, 42);
  appendBE32(tiff, 8);
  appendBE16(tiff, 0xffff);
  appendBE32(tiff, 0);
  return QByteArray("Exif\0\0", 6) + tiff;
}

// Deep nesting and a canvas far past any screen, to reach both the SVG
// parser's recursion and its reported size.
QByteArray nestedSvg() {
  QByteArray svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"99999999\" height=\"99999999\">";
  for (int i = 0; i < 64; ++i) {
    svg += "<g>";
  }
  svg += "<rect width=\"100%\" height=\"100%\" fill=\"red\"/>";
  for (int i = 0; i < 64; ++i) {
    svg += "</g>";
  }
  svg += "</svg>";
  return svg;
}

// A single-page PDF whose MediaBox is far larger than any renderer should
// rasterize at the requested size.
QByteArray pdfPageWithMediaBox(int width, int height) {
  QByteArray pdf = "%PDF-1.4\n";
  QList<int> offsets(5, 0);
  const auto addObject = [&](int number, const QByteArray& body) {
    offsets[number] = int(pdf.size());
    pdf += QByteArray::number(number) + " 0 obj\n" + body + "\nendobj\n";
  };
  addObject(1, "<< /Type /Catalog /Pages 2 0 R >>");
  addObject(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  addObject(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + QByteArray::number(width) + " " +
                   QByteArray::number(height) + "] /Resources << >> /Contents 4 0 R >>");
  addObject(4, "<< /Length 0 >>\nstream\nendstream");
  const int xref = int(pdf.size());
  pdf += "xref\n0 5\n0000000000 65535 f \n";
  for (int i = 1; i <= 4; ++i) {
    pdf += QStringLiteral("%1 00000 n \n").arg(offsets[i], 10, 10, QLatin1Char('0')).toLatin1();
  }
  pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) +
         "\n%%EOF\n";
  return pdf;
}

// A little-endian TIFF header with a directory claiming 65535 entries and none
// behind it, followed by noise: a hostile camera raw.
QByteArray rawWithHugeIfd() {
  QByteArray raw("II*\0", 4);
  appendLE32(raw, 8);
  appendLE16(raw, 0xffff);
  appendLE32(raw, 0);
  raw += QByteArray(64, '\xff');
  return raw;
}

// A 32-bit BMP header claiming |width| x |height| with no pixel data. Qt's BMP
// handler accepts the dimensions, so this is the lying header the estimator
// has to saturate rather than wrap.
QByteArray bitmapHeader(int width, int height) {
  QByteArray bmp = "BM";
  appendLE32(bmp, 54);                       // file size, unused by the reader
  appendLE16(bmp, 0);
  appendLE16(bmp, 0);
  appendLE32(bmp, 54);                       // pixel data offset
  appendLE32(bmp, 40);                       // DIB header size
  appendLE32(bmp, quint32(width));
  appendLE32(bmp, quint32(height));
  appendLE16(bmp, 1);                        // planes
  appendLE16(bmp, 32);                       // bits per pixel
  appendLE32(bmp, 0);                        // compression
  appendLE32(bmp, 0);
  appendLE32(bmp, 0);
  appendLE32(bmp, 0);
  appendLE32(bmp, 0);
  appendLE32(bmp, 0);
  return bmp;
}

} // namespace

class HostileMediaTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    QVERIFY(m_dir.isValid());
    QVERIFY(qputenv("XDG_CACHE_HOME", m_dir.filePath(QStringLiteral("cache")).toUtf8()));
    // Qt's own default, the one a fresh launch raises before decoding
    // anything large.
    QCOMPARE(QImageReader::allocationLimit(), 256);
    QVERIFY(buildFixtures());
  }

  void cleanup() {
    ImageBudget::setLimits({});
    QImageReader::setAllocationLimit(256);
  }

  // Admission is by extension and header only: a hostile file is listed and
  // the library still opens. A text file with a media extension is admitted by
  // its name; the decode is what fails, later.
  void scannerListsHostileFilesWithoutDecoding() {
    QStringList traversed;
    const QList<CaptureRecord> records =
        CaptureScanner::scan({{m_dir.path()}}, nullptr, &traversed);
    QVERIFY(!traversed.isEmpty());
    QSet<QString> names;
    for (const CaptureRecord& record : records) {
      names.insert(record.fileName);
      QVERIFY(QFileInfo::exists(record.path));
    }
    for (const char* name :
         {"bomb.bmp", "bomb.png", "bomb.gif", "bomb.webp", "bomb.pdf", "broken.mp4",
          "truncated.mkv", "huge.svg", "truncated.jpg", "truncated.gif", "text.png",
          "text.mp4", "empty.png", "empty.jpg", "wrapped.heic", "tiny.png",
          "truncated.avif"}) {
      QVERIFY2(names.contains(QLatin1String(name)), name);
    }
  }

  // Extensionless files are read from their headers, so the sniffing path has
  // to survive a header that lies about its size just as a decode would.
  void extensionlessHostileFilesSniffSafely() {
    QCOMPARE(CaptureScanner::mediaSuffix(m_dir.filePath(QStringLiteral("no-extension-image"))),
             QStringLiteral("jpg"));
    QCOMPARE(CaptureScanner::mediaSuffix(m_dir.filePath(QStringLiteral("no-extension-bomb"))),
             QStringLiteral("png"));
    QVERIFY(CaptureScanner::mediaSuffix(m_dir.filePath(QStringLiteral("no-extension-text")))
                .isEmpty());
    QVERIFY(CaptureScanner::mediaSuffix(m_dir.filePath(QStringLiteral("no-extension-pdf")))
                .isEmpty());
    // A repeated ask gives the same answer.
    QCOMPARE(CaptureScanner::mediaSuffix(m_dir.filePath(QStringLiteral("no-extension-bomb"))),
             QStringLiteral("png"));
  }

  // Estimating reads only a header, so it must never trust one into an
  // overflow: an accepted gigapixel header saturates or stays enormous; a
  // wrapped size would read as small and sail past the allocation limit. Some
  // decoders reject the header outright, which is also safe.
  void imageBudgetNeverWrapsOnLyingHeaders() {
    for (const char* name : {"bomb.bmp", "bomb.png", "bomb.gif", "bomb.webp"}) {
      const ImageBudget::Estimate estimate =
          ImageBudget::estimate(m_dir.filePath(QLatin1String(name)));
      QVERIFY2(!estimate.valid() || estimate.bytes >= 64 * ImageBudget::kMiB, name);
    }
    // The completed GIF header carries its canvas into the frame Qt reports,
    // so the estimator really sees 65535 x 65535 rather than rejecting it.
    const ImageBudget::Estimate gif =
        ImageBudget::estimate(m_dir.filePath(QStringLiteral("bomb.gif")));
    QVERIFY(gif.valid());
    QVERIFY(gif.bytes >= 64 * ImageBudget::kMiB);
    // The WebP handler wants image data before it reports a canvas, so this
    // header-only file is rejected before the estimate; that rejection is the
    // safe outcome the decode check below asserts.
    // The SVG handler scales, so its surface is measured rather than trusted;
    // it is only read at the size it is drawn at.
    const ImageBudget::Estimate svg =
        ImageBudget::estimate(m_dir.filePath(QStringLiteral("huge.svg")));
    if (svg.valid()) {
      QVERIFY(svg.bytes > 0);
    }
    // An EXIF segment larger than the file behind it must not be believed
    // either; the frame itself is a tiny JPEG.
    const ImageBudget::Estimate exif =
        ImageBudget::estimate(m_dir.filePath(QStringLiteral("exif-huge.jpg")));
    QVERIFY(exif.valid());
    QVERIFY(exif.bytes > 0);

    const ImageBudget::Estimate tiny =
        ImageBudget::estimate(m_dir.filePath(QStringLiteral("tiny.jpg")));
    QVERIFY(tiny.valid());
    QVERIFY(tiny.bytes > 0 && tiny.bytes < 4 * ImageBudget::kMiB);

    // Every lying header is refused a decode rather than allocated.
    for (const char* name : {"bomb.bmp", "bomb.png", "bomb.gif", "bomb.webp"}) {
      QImageReader reader(m_dir.filePath(QLatin1String(name)));
      QVERIFY2(ImageBudget::readBounded(reader).isNull(), name);
    }
    QImageReader readable(m_dir.filePath(QStringLiteral("tiny.jpg")));
    QVERIFY(!ImageBudget::readBounded(readable).isNull());
  }

  // The grid asks for a tile of every file it lists. A broken file fails once
  // and is remembered, and never yields a tile larger than the request.
  void thumbnailsOfHostileFilesFailCleanly() {
    const QStringList hostile = {
        QStringLiteral("bomb.bmp"),        QStringLiteral("bomb.png"),
        QStringLiteral("bomb.gif"),        QStringLiteral("bomb.webp"),
        QStringLiteral("huge.svg"),        QStringLiteral("truncated.jpg"),
        QStringLiteral("scan-starved.jpg"), QStringLiteral("garbage.jpg"),
        QStringLiteral("truncated.png"),   QStringLiteral("truncated.gif"),
        QStringLiteral("truncated.webp"),  QStringLiteral("truncated.avif"),
        QStringLiteral("exif-huge.jpg"),   QStringLiteral("text.png"),
        QStringLiteral("empty.png"),       QStringLiteral("binary.pdf"),
        QStringLiteral("bomb.pdf"),        QStringLiteral("broken.mp4"),
        QStringLiteral("truncated.mkv"),
    };
    for (const QString& name : hostile) {
      const QString path = m_dir.filePath(name);
      const QImage first = ThumbnailCache::thumbnail(path, QSize(64, 64), 1.0);
      if (!first.isNull()) {
        QVERIFY2(first.width() <= 128 && first.height() <= 128, qPrintable(name));
      }
      // The second ask repeats the outcome (from the negative cache for a
      // failure) rather than crashing or flipping.
      const QImage second = ThumbnailCache::thumbnail(path, QSize(64, 64), 1.0);
      QCOMPARE(second.isNull(), first.isNull());
    }
    QVERIFY(!ThumbnailCache::thumbnail(m_dir.filePath(QStringLiteral("tiny.jpg")), QSize(64, 64),
                                       1.0)
                 .isNull());
  }

  // The viewer's reduced copy is the last decoder a file reaches. Lying
  // dimensions must be refused there too, not allocated.
  void largePictureReadsRefuseLyingDimensions() {
    LargeImageProvider provider;
    // std::nullopt means the request never completed, so a missing result is
    // told apart from a decode that completed and refused the picture.
    const auto read = [&provider](const QString& path) -> std::optional<QImage> {
      const QString url = ImageBudget::reducedUrl(path, FileVersion::key(path), 512).toString();
      const QString prefix = QStringLiteral("image://large/");
      if (!url.startsWith(prefix)) {
        return std::nullopt;
      }
      std::unique_ptr<QQuickImageResponse> response(
          provider.requestImageResponse(url.mid(prefix.size()), QSize()));
      if (!response) {
        return std::nullopt;
      }
      QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
      if (finished.isEmpty() && !finished.wait(15000)) {
        return std::nullopt;
      }
      std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
      return factory ? factory->image() : QImage();
    };
    const auto tiny = read(m_dir.filePath(QStringLiteral("tiny.jpg")));
    QVERIFY(tiny.has_value());
    QVERIFY(!tiny->isNull());
    for (const char* name : {"bomb.bmp", "bomb.png", "bomb.gif", "bomb.webp"}) {
      const auto image = read(m_dir.filePath(QLatin1String(name)));
      QVERIFY2(image.has_value(), name);
      QVERIFY2(image->isNull(), name);
    }
    provider.shutdown();
  }

  void pdfHelpersRejectMalformedDocuments() {
    if (!PdfSupport::available()) {
      QSKIP("Poppler tools are not installed");
    }
    QVERIFY(PdfSupport::renderPage(m_dir.filePath(QStringLiteral("binary.pdf")), 1, QSize(200, 200))
                .isNull());
    const QImage bomb =
        PdfSupport::renderPage(m_dir.filePath(QStringLiteral("bomb.pdf")), 1, QSize(200, 200));
    if (!bomb.isNull()) {
      QVERIFY(bomb.width() <= 200 && bomb.height() <= 200);
    }
    QVERIFY(PdfSupport::findPages(QStringLiteral("some document text"),
                                  QStringLiteral("absent"))
                .isEmpty());
    QVERIFY(PdfSupport::findPages(QString(), QStringLiteral("query")).isEmpty());
  }

  // The details panel and the raw header reader take bytes from outside tools
  // and hostile files, so their parsers must stop at the ends of their input.
  void mediaParsersSurviveGarbage() {
    QVERIFY(MediaInspector::parseImage(QByteArray()).isEmpty());
    QVERIFY(MediaInspector::parseVideo(QByteArray(), QStringLiteral("mp4")).isEmpty());
    QVERIFY(MediaInspector::parseVideo(QByteArray(4096, '\xff'), QStringLiteral("mkv")).isEmpty());
    // Content is not asserted: only that a byte soup does not read past its
    // end while the field parser walks it.
    (void)MediaInspector::parseImage(QByteArray(4096, '\xff'));
    (void)MediaInspector::parseImage(QByteArray(4096, '\n'));
    (void)MediaInspector::parseImage(QByteArray("Format: \n1/0\n-\n") + QByteArray("\xff\xfe", 2));
    (void)MediaInspector::describeRaw(m_dir.filePath(QStringLiteral("broken.arw")));
  }

  // A corrupt download opened from the file manager reaches the player the
  // same way. It must settle rather than leaving the viewer loading forever.
  void brokenVideoSettlesWithoutCrashing() {
    QAudioOutput audio;
    audio.setVolume(0);
    QMediaPlayer player;
    player.setAudioOutput(&audio);
    player.setSource(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("broken.mp4"))));
    player.play();
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 10000 && player.mediaStatus() == QMediaPlayer::LoadingMedia) {
      QTest::qWait(50);
    }
    QVERIFY2(player.mediaStatus() != QMediaPlayer::LoadingMedia,
             qPrintable(QStringLiteral("still loading after %1 ms").arg(elapsed.elapsed())));
    player.stop();

    // A truncated but real container is opened briefly, then abandoned; the
    // point is the decode path, not a particular outcome.
    QMediaPlayer truncated;
    truncated.setAudioOutput(&audio);
    truncated.setSource(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("truncated.mkv"))));
    truncated.play();
    QTest::qWait(500);
    truncated.stop();
  }

private:
  bool buildFixtures() {
    const auto put = [this](const char* name, const QByteArray& bytes) {
      return writeFile(m_dir.filePath(QLatin1String(name)), bytes);
    };

    // A control that decodes.
    if (!put("tiny.jpg", jpegBytes(QSize(24, 18)))) return false;
    if (!put("tiny.png", pngBytes(QSize(24, 18)))) return false;

    // Truncated containers: a writer cut off mid-file.
    const QByteArray jpeg = jpegBytes(QSize(240, 180));
    const QByteArray png = pngBytes(QSize(120, 90));
    if (!put("truncated.jpg", jpeg.left(jpeg.size() * 3 / 5))) return false;
    if (!put("scan-starved.jpg", jpeg.left(400))) return false;
    if (!put("garbage.jpg", QByteArray("\xff\xd8\xff", 3) + QByteArray(4096, '\xff'))) return false;
    if (!put("truncated.png", png.left(png.size() / 3))) return false;
    const QString animatedGif = QFINDTESTDATA("fixtures/viewer/animated.gif");
    const QString animatedWebp = QFINDTESTDATA("fixtures/viewer/animated.webp");
    const QString tracks = QFINDTESTDATA("fixtures/viewer/tracks.mkv");
    if (animatedGif.isEmpty() || animatedWebp.isEmpty() || tracks.isEmpty()) return false;
    if (!put("truncated.gif", readFile(animatedGif).left(256))) return false;
    if (!put("truncated.webp", readFile(animatedWebp).left(256))) return false;

    // Lying headers: dimensions no allocation should ever honour.
    if (!put("bomb.bmp", bitmapHeader(2000000000, 2000000000))) return false;
    if (!put("bomb.png", pngWithClaimedSize(0x7fffffffu, 0x7fffffffu))) return false;
    if (!put("bomb.gif", gifWithClaimedSize(65535, 65535))) return false;
    if (!put("bomb.webp", webpWithClaimedSize(0x00ffffffu, 0x00ffffffu))) return false;
    if (!put("huge.svg", nestedSvg())) return false;
    if (!put("exif-huge.jpg", jpegWithApp1(exifWithHugeIfd(), jpegBytes(QSize(16, 16)))))
      return false;

    // Truncated modern photo containers, header only.
    if (!put("truncated.avif", ftypOnly("avif"))) return false;
    if (!put("wrapped.heic", ftypOnly("heic"))) return false;

    // Mislabeled and empty files.
    if (!put("text.png", "This is not a PNG, it is a README.\n")) return false;
    if (!put("text.jpg", "Not a photograph either.\n")) return false;
    if (!put("text.mp4", "Not a movie.\n")) return false;
    if (!put("text.pdf", "Not a document.\n")) return false;
    if (!put("empty.png", QByteArray())) return false;
    if (!put("empty.jpg", QByteArray())) return false;
    if (!put("empty.mp4", QByteArray())) return false;
    if (!put("empty.pdf", QByteArray())) return false;

    // Documents: garbage behind a PDF header, and a page no renderer should
    // rasterize at its own size.
    if (!put("binary.pdf", QByteArray("%PDF-1.7\n") + QByteArray(4096, '\xa5'))) return false;
    if (!put("bomb.pdf", pdfPageWithMediaBox(200000, 200000))) return false;

    // Video: an ISO header with nothing behind it, and a real clip cut short.
    if (!put("broken.mp4", ftypOnly("isom") + QByteArray(2048, '\x00'))) return false;
    const QByteArray realVideo = readFile(tracks);
    if (!put("truncated.mkv", realVideo.left(realVideo.size() / 4))) return false;

    // A camera raw header with a lying directory.
    if (!put("broken.arw", rawWithHugeIfd())) return false;

    // Extensionless: a valid image, a lying one, plain text and a PDF.
    if (!put("no-extension-image", jpegBytes(QSize(24, 18)))) return false;
    if (!put("no-extension-bomb", pngWithClaimedSize(0x7fffffffu, 0x7fffffffu))) return false;
    if (!put("no-extension-text", "Just words.\n")) return false;
    if (!put("no-extension-pdf", "%PDF-1.4\n%%EOF\n")) return false;
    return true;
  }

  QTemporaryDir m_dir;
};

int main(int argc, char** argv) {
  qunsetenv("QT_IMAGEIO_MAXALLOC");
  QGuiApplication app(argc, argv);
  HostileMediaTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "tst_hostile_media.moc"
