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
#include "sources/DecodeJournal.h"
#include "sources/FileVersion.h"
#include "sources/ImageBudget.h"
#include "sources/PinnedImageReader.h"
#include "thumbs/LargeImageProvider.h"
#include "thumbs/ThumbnailCache.h"
#include "viewer/PictureDiagnosis.h"
#include "viewer/ViewerSession.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QSet>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <csignal>
#include <functional>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

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

// The 24-byte PlayStation TIM file CI's image fuzzer found: KImageFormats'
// TIM plugin (kimg_tim, 6.31) crashes reading it. Qt's own format guessing
// offered it to that plugin under any picture name whose decoder refused it,
// so in a folder it crashed the library at launch and the viewer's preload.
QByteArray timCrashInput() {
  return QByteArray::fromHex("10000000d93135ffefffffffff055031203421ff14000004");
}

// A real fault, not a raised signal: a store into a page with no access.
// Neither sanitizer stops it first.
void faultHere() {
  void* page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  *static_cast<volatile int*>(page) = 1;
}

// How often a run-once handler from before the journal ran, in a page the
// forked child shares with the test.
std::atomic<int>* g_runOnceCalls = nullptr;

// What a recursive parser does to a crafted file: runs out of stack.
__attribute__((noinline)) int recurse(int depth) {
  volatile char frame[1024];
  frame[0] = char(depth);
  return depth > (1 << 30) ? frame[0] : recurse(depth + 1) + frame[0];
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
        QStringLiteral("truncated.mkv"),   QStringLiteral("tim.png"),
        QStringLiteral("tim.jpg"),         QStringLiteral("tim.tga"),
        QStringLiteral("tim.gif"),
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

  // Every reader is pinned to a listed format, so a TIM file named as a
  // picture only ever meets that picture's decoder, which refuses it. Before,
  // Qt offered it to the TIM plugin, which crashed this process.
  void timFilesNamedAsPicturesNeverReachTheirPlugin() {
    const bool timKnown =
        QMimeDatabase().mimeTypeForData(timCrashInput()).name() == u"image/x-tim";
    for (const char* name : {"tim.png", "tim.jpg", "tim.tga", "tim.gif"}) {
      const QString path = m_dir.filePath(QLatin1String(name));
      PinnedImageReader reader(path);
      QVERIFY2(!reader.canRead(), name);
      QVERIFY2(!reader.pinned(), name);
      QVERIFY2(!reader.renamed(), name);
      // No decoder stays attached to parse what it refused.
      QVERIFY2(reader.device() == nullptr, name);
      QVERIFY2(reader.read().isNull(), name);
      QVERIFY2(ImageFormats::listed().contains(reader.format()), name);
      const ImageBudget::Estimate estimate = ImageBudget::estimate(path);
      QVERIFY2(estimate.unreadable(), name);
      QVERIFY2(!ImageBudget::plan(estimate).error.isEmpty(), name);
      QVERIFY2(ThumbnailCache::thumbnail(path, QSize(64, 64), 1.0).isNull(), name);
      // The viewer's error card says the name is wrong rather than calling
      // the file damaged.
      const PictureDiagnosis::Finding finding = PictureDiagnosis::failure(path);
      if (timKnown) {
        QCOMPARE(finding.cause, QStringLiteral("wrong-type"));
        QCOMPARE(finding.title, QStringLiteral("This file isn't what its name says"));
        const QString kind = QMimeDatabase().mimeTypeForName(QStringLiteral("image/x-tim")).comment();
        QVERIFY2(finding.detail.contains(kind) && finding.detail.contains(u"PNG") ==
                                                      QByteArray(name).endsWith(".png"),
                 qPrintable(finding.detail));
        // Its capitals kept, with the article for how it sounds.
        QVERIFY2(finding.detail.startsWith(QStringLiteral("Its contents look like a ") + kind),
                 qPrintable(finding.detail));
      } else {
        QVERIFY(!finding.cause.isEmpty());
      }
    }
    // A .gif plays only when it is one; anything else is read as a still.
    QVERIFY(!ImageFormats::looksAnimated(m_dir.filePath(QStringLiteral("tim.gif")),
                                         QStringLiteral("gif")));
    QVERIFY(ImageFormats::looksAnimated(m_dir.filePath(QStringLiteral("bomb.gif")),
                                        QStringLiteral("gif")));
    // A listed format under another listed name still opens, read pinned to
    // what it is.
    const QString renamed = m_dir.filePath(QStringLiteral("png-inside.jpg"));
    PinnedImageReader reader(renamed);
    QVERIFY(reader.canRead());
    QVERIFY(reader.renamed());
    QCOMPARE(reader.format(), QByteArray("png"));
    QCOMPARE(reader.read().size(), QSize(24, 18));
    const ImageBudget::Estimate estimate = ImageBudget::estimate(renamed);
    QVERIFY(estimate.valid());
    QVERIFY(!estimate.direct);
    QVERIFY(!ThumbnailCache::thumbnail(renamed, QSize(64, 64), 1.0).isNull());
    // Also after a decoder that reads into the file before refusing it.
    for (const char* name : {"jpeg-inside.gif", "jpeg-inside.webp", "png-inside.gif"}) {
      PinnedImageReader other(m_dir.filePath(QLatin1String(name)));
      QVERIFY2(other.canRead() && other.renamed(), name);
      QVERIFY2(other.read().size() == QSize(24, 18), name);
    }
    // Bytes from elsewhere, under a type that is wrong.
    QBuffer bytes;
    bytes.setData(jpegBytes(QSize(24, 18)));
    QVERIFY(bytes.open(QIODevice::ReadOnly));
    PinnedImageReader fromBytes(&bytes, "gif");
    QVERIFY(fromBytes.canRead() && fromBytes.renamed());
    QCOMPARE(fromBytes.read().size(), QSize(24, 18));
    // So does a picture whose name is right, read directly.
    QVERIFY(ImageBudget::estimate(m_dir.filePath(QStringLiteral("tiny.jpg"))).direct);
  }

  // The viewer's neighbour preload probed the TIM file and crashed. Now it
  // skips it, preloads a renamed PNG through the pinned provider, and shows
  // the TIM file's error without loading it.
  void viewerSkipsTimFilesAndReadsRenamedOnesPinned() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    QVERIFY(writeFile(folder.filePath(QStringLiteral("a.jpg")), jpegBytes(QSize(24, 18))));
    QVERIFY(writeFile(folder.filePath(QStringLiteral("b-tim.png")), timCrashInput()));
    QVERIFY(writeFile(folder.filePath(QStringLiteral("c-png.jpg")), pngBytes(QSize(24, 18))));
    ViewerSession session;
    session.open({folder.filePath(QStringLiteral("a.jpg"))});
    QTRY_COMPARE(session.count(), 3);
    QCOMPARE(session.fileNameAt(1), QStringLiteral("b-tim.png"));
    // Both neighbours are probed in one task, the next one first.
    QTRY_VERIFY(!session.previousPreloadUrl().isEmpty());
    QVERIFY(session.nextPreloadUrl().isEmpty());
    QVERIFY(session.previousPreloadUrl().toString().startsWith(u"image://large/"));

    QVERIFY(session.step(1));
    QCOMPARE(session.fileName(), QStringLiteral("b-tim.png"));
    QTRY_VERIFY(!session.pictureError().isEmpty());
    QVERIFY(session.imageUrl().isEmpty());
    session.diagnosePicture();
    QTRY_VERIFY(!session.pictureFailure().isEmpty());

    QVERIFY(session.step(1));
    QCOMPARE(session.fileName(), QStringLiteral("c-png.jpg"));
    QTRY_VERIFY(!session.imageUrl().isEmpty());
    QVERIFY(session.imageUrl().toString().startsWith(u"image://large/"));
    QVERIFY(session.pictureError().isEmpty());
  }

  // What a crash leaves behind: the next launch skips that file for
  // thumbnails and decodes, names it once, and reads it again once it
  // changes.
  void leftoverJournalEntrySkipsTheFileAndNamesIt() {
    const QString journal = m_dir.filePath(QStringLiteral("journal-leftover"));
    const QString victim = m_dir.filePath(QStringLiteral("crashed-last-time.jpg"));
    const QString bystander = m_dir.filePath(QStringLiteral("tiny.jpg"));
    QVERIFY(writeFile(victim, jpegBytes(QSize(24, 18))));
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    QVERIFY(DecodeJournal::open(journal));
    {
      // Decoding when the process died.
      DecodeJournal::Scope scope(victim);
      DecodeJournal::simulateCrash();
      DecodeJournal::close();
    }

    QVERIFY(DecodeJournal::open(journal));
    QVERIFY(DecodeJournal::isBlocked(victim));
    QVERIFY(!DecodeJournal::isBlocked(bystander));
    QCOMPARE(DecodeJournal::blocked(), QStringList{QFileInfo(victim).canonicalFilePath()});
    QVERIFY(ThumbnailCache::thumbnail(victim, QSize(64, 64), 1.0).isNull());
    // By whatever name it is reached.
    const QString link = m_dir.filePath(QStringLiteral("link-to-crashed.jpg"));
    QVERIFY(QFile::link(victim, link));
    QVERIFY(DecodeJournal::isBlocked(link));
    QVERIFY(!ThumbnailCache::thumbnail(bystander, QSize(64, 64), 1.0).isNull());
    PinnedImageReader reader(victim);
    QVERIFY(reader.blocked());
    QVERIFY(!reader.canRead());
    const ImageBudget::Estimate estimate = ImageBudget::estimate(victim);
    QVERIFY(estimate.blocked);
    QVERIFY(estimate.unreadable());
    const PictureDiagnosis::Finding finding = PictureDiagnosis::failure(victim);
    QCOMPARE(finding.cause, QStringLiteral("crashed"));
    QCOMPARE(finding.title, QStringLiteral("This file crashed Omaroll"));

    // The notice names it, once.
    DecodeCrashNotice notice;
    QCOMPARE(notice.path(), QFileInfo(victim).canonicalFilePath());
    QCOMPARE(notice.fileName(), QStringLiteral("crashed-last-time.jpg"));
    notice.shown();
    QVERIFY(DecodeJournal::pendingNotice().isEmpty());
    QSignalSpy changed(&notice, &DecodeCrashNotice::changed);
    notice.dismiss();
    QCOMPARE(changed.count(), 1);
    QVERIFY(notice.path().isEmpty());

    // A launch after that still skips it, without saying so again.
    DecodeJournal::close();
    QVERIFY(DecodeJournal::open(journal));
    QVERIFY(DecodeJournal::isBlocked(victim));
    QVERIFY(DecodeJournal::pendingNotice().isEmpty());
    QVERIFY(DecodeCrashNotice().path().isEmpty());

    // Changed, it gets another chance, and stays unblocked.
    QFile file(victim);
    QVERIFY(file.open(QIODevice::Append));
    QCOMPARE(file.write("\0", 1), qint64(1));
    file.close();
    QVERIFY(!DecodeJournal::isBlocked(victim));
    QVERIFY(!ThumbnailCache::thumbnail(victim, QSize(64, 64), 1.0).isNull());
    DecodeJournal::close();
    QVERIFY(DecodeJournal::open(journal));
    QVERIFY(DecodeJournal::blocked().isEmpty());
    DecodeJournal::close();

    // A blocked file that is gone is dropped from the list when it is read,
    // but not one whose folder is gone too, as on a drive that is not
    // mounted: that one is still blocked when the drive is back.
    const QString drive = m_dir.filePath(QStringLiteral("drive"));
    const QString unplugged = m_dir.filePath(QStringLiteral("drive-unplugged"));
    QVERIFY(QDir().mkpath(drive));
    const QString away = drive + QStringLiteral("/on-a-drive.jpg");
    QVERIFY(writeFile(away, jpegBytes(QSize(24, 18))));
    const QString awayKey = QFileInfo(away).canonicalFilePath();
    for (const QString& crashed : {victim, away}) {
      QVERIFY(DecodeJournal::open(journal));
      DecodeJournal::Scope scope(crashed);
      DecodeJournal::simulateCrash();
      DecodeJournal::close();
    }
    QVERIFY(DecodeJournal::open(journal));
    QCOMPARE(DecodeJournal::blocked().size(), 2);
    DecodeJournal::close();
    QVERIFY(QFile::remove(link));
    QVERIFY(QFile::remove(victim));
    QVERIFY(QDir().rename(drive, unplugged));
    QVERIFY(DecodeJournal::open(journal));
    QCOMPARE(DecodeJournal::blocked(), QStringList{awayKey});
    DecodeJournal::close();
    QVERIFY(DecodeJournal::open(journal));
    QCOMPARE(DecodeJournal::blocked(), QStringList{awayKey});
    DecodeJournal::close();
    QVERIFY(QDir().rename(unplugged, drive));
    QVERIFY(DecodeJournal::open(journal));
    QVERIFY(DecodeJournal::isBlocked(away));
    QVERIFY(ThumbnailCache::thumbnail(away, QSize(64, 64), 1.0).isNull());
    DecodeJournal::close();
  }

  // The real path: a fatal signal on a decoding thread marks its file, and
  // only that thread's file.
  void fatalSignalWhileDecodingBlocksTheFile() {
    const QString victim = QFileInfo(m_dir.filePath(QStringLiteral("tiny.jpg"))).canonicalFilePath();
    const QString bystander = QFileInfo(m_dir.filePath(QStringLiteral("tiny.png"))).canonicalFilePath();
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    // What a crash in each shape leaves for the next launch.
    const auto blamed = [](const QString& journal, int signal, const std::function<void()>& crash) {
      DecodeJournal::open(journal);
      const pid_t child = ::fork();
      if (child == 0) {
        // No core dump and no QtTest crash report for a crash the test
        // asked for: the journal's handler hands it to the default.
        ::prctl(PR_SET_DUMPABLE, 0);
        // A child that hangs on a lock some parent thread held at fork time
        // fails the check instead of the suite.
        ::alarm(20);
        ::signal(signal, SIG_DFL);
        DecodeJournal::installCrashHandler();
        crash();
        ::_exit(0);
      }
      int status = 0;
      const bool died = child > 0 && ::waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
                        WTERMSIG(status) == signal;
      DecodeJournal::close();
      DecodeJournal::open(journal);
      const QStringList files = DecodeJournal::blocked();
      DecodeJournal::close();
      return died ? files : QStringList{QStringLiteral("<the child did not die of the signal>")};
    };

    // abort() inside nested scopes blames the inner file.
    QCOMPARE(blamed(m_dir.filePath(QStringLiteral("journal-abort")), SIGABRT,
                    [&] {
                      DecodeJournal::Scope outer(bystander);
                      DecodeJournal::Scope inner(victim);
                      std::abort();
                    }),
             QStringList{victim});
    // A real fault on a pool thread while it decodes.
    QCOMPARE(blamed(m_dir.filePath(QStringLiteral("journal-fault")), SIGSEGV,
                    [&] {
                      QThreadPool pool;
                      pool.start([&] {
                        DecodeJournal::Scope scope(victim);
                        faultHere();
                      });
                      pool.waitForDone();
                    }),
             QStringList{victim});
    // A fault on a thread that decodes nothing blames nothing, even while
    // another thread is decoding.
    QCOMPARE(blamed(m_dir.filePath(QStringLiteral("journal-elsewhere")), SIGSEGV,
                    [&] {
                      QThreadPool pool;
                      QSemaphore decoding;
                      QSemaphore never;
                      pool.start([&] {
                        DecodeJournal::Scope scope(bystander);
                        decoding.release();
                        never.acquire();
                      });
                      decoding.acquire();
                      faultHere();
                    }),
             QStringList{});
    // A stack overflow in a recursive parser is recorded on the thread's own
    // alternate stack.
    QCOMPARE(blamed(m_dir.filePath(QStringLiteral("journal-overflow")), SIGSEGV,
                    [&] {
                      QThreadPool pool;
                      pool.setStackSize(256 * 1024);
                      pool.start([&] {
                        DecodeJournal::Scope scope(victim);
                        (void)recurse(0);
                      });
                      pool.waitForDone();
                    }),
             QStringList{victim});

    // A process that ends without a crash leaves nothing to block.
    const QString clean = m_dir.filePath(QStringLiteral("journal-clean"));
    QVERIFY(DecodeJournal::open(clean));
    {
      DecodeJournal::Scope scope(bystander);
    }
    DecodeJournal::close();
    QVERIFY(DecodeJournal::open(clean));
    QVERIFY(DecodeJournal::blocked().isEmpty());
    DecodeJournal::close();
  }

  // A handler from before the journal that asked to run once (QtTest's does)
  // runs once for a crash outside any decode, and then the default ends the
  // process, instead of the fault calling it again forever.
  void aRunOnceHandlerFromBeforeRunsOnce() {
    const QString journal = m_dir.filePath(QStringLiteral("journal-run-once"));
    void* shared = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    QVERIFY(shared != MAP_FAILED);
    const auto unmap = qScopeGuard([shared] { ::munmap(shared, 4096); });
    g_runOnceCalls = new (shared) std::atomic<int>(0);
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    QVERIFY(DecodeJournal::open(journal));
    const pid_t child = ::fork();
    if (child == 0) {
      ::prctl(PR_SET_DUMPABLE, 0);
      // The old loop ends in SIGALRM rather than hanging the suite.
      ::alarm(10);
      struct sigaction once {};
      once.sa_handler = [](int) { g_runOnceCalls->fetch_add(1); };
      once.sa_flags = SA_RESETHAND;
      sigemptyset(&once.sa_mask);
      ::sigaction(SIGSEGV, &once, nullptr);
      DecodeJournal::installCrashHandler();
      faultHere();
      ::_exit(0);
    }
    QVERIFY(child > 0);
    int status = 0;
    QCOMPARE(::waitpid(child, &status, 0), child);
    QVERIFY2(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
             qPrintable(QStringLiteral("status %1").arg(status)));
    QCOMPARE(g_runOnceCalls->load(), 1);
    DecodeJournal::close();
    QVERIFY(DecodeJournal::open(journal));
    QVERIFY(DecodeJournal::blocked().isEmpty());
  }

  // Files blocked before are skipped even when this process keeps no
  // journal: another Omaroll holds it, or its folder cannot be made.
  void filesBlockedBeforeStaySkippedWithoutAJournal() {
    const QString store = m_dir.filePath(QStringLiteral("journal-store-first"));
    const QString victim = m_dir.filePath(QStringLiteral("blocked-store-first.jpg"));
    QVERIFY(writeFile(victim, jpegBytes(QSize(24, 18))));
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    QVERIFY(DecodeJournal::open(store));
    {
      DecodeJournal::Scope scope(victim);
      DecodeJournal::simulateCrash();
      DecodeJournal::close();
    }
    QVERIFY(DecodeJournal::open(store));
    QVERIFY(DecodeJournal::isBlocked(victim));
    DecodeJournal::close();

    // Another Omaroll holds the journal.
    const int held = ::open(QFile::encodeName(store + QStringLiteral("/decoding")).constData(),
                            O_RDWR | O_CLOEXEC);
    QVERIFY(held >= 0);
    const auto release = qScopeGuard([held] { ::close(held); });
    QCOMPARE(::flock(held, LOCK_EX | LOCK_NB), 0);
    QVERIFY(!DecodeJournal::open(store));
    QVERIFY(!DecodeJournal::active());
    QVERIFY(DecodeJournal::isBlocked(victim));
    QVERIFY(ThumbnailCache::thumbnail(victim, QSize(64, 64), 1.0).isNull());
    DecodeJournal::close();
    QCOMPARE(::flock(held, LOCK_UN), 0);

    // The journal's own folder cannot be made: it goes beside the list.
    const QString notAFolder = m_dir.filePath(QStringLiteral("journal-not-a-folder"));
    QVERIFY(writeFile(notAFolder, QByteArray("x")));
    QVERIFY(DecodeJournal::open(store, notAFolder + QStringLiteral("/omaroll")));
    QVERIFY(DecodeJournal::active());
    QVERIFY(DecodeJournal::isBlocked(victim));
  }

  // On a full disk there is no room for the journal. Omaroll goes on without
  // it, still skipping files blocked before, instead of dying of SIGBUS on
  // the first store into the mapping.
  void aJournalWithoutRoomIsOffAndDecodingGoesOn() {
    const QString journal = m_dir.filePath(QStringLiteral("journal-full"));
    const QString victim = m_dir.filePath(QStringLiteral("blocked-before.jpg"));
    const QString picture = m_dir.filePath(QStringLiteral("tiny.jpg"));
    QVERIFY(writeFile(victim, jpegBytes(QSize(24, 18))));
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    QVERIFY(DecodeJournal::open(journal));
    {
      DecodeJournal::Scope scope(victim);
      DecodeJournal::simulateCrash();
      DecodeJournal::close();
    }
    DecodeJournal::failNextReservation();
    QVERIFY(!DecodeJournal::open(journal));
    QVERIFY(!DecodeJournal::active());
    QVERIFY(DecodeJournal::isBlocked(victim));
    {
      DecodeJournal::Scope scope(picture);
    }
    QVERIFY(!PinnedImageReader(picture).read().isNull());
    QVERIFY(!ThumbnailCache::thumbnail(picture, QSize(64, 64), 1.0).isNull());
    QVERIFY(ThumbnailCache::thumbnail(victim, QSize(64, 64), 1.0).isNull());
    DecodeJournal::close();
    // Room again: journaling is back.
    QVERIFY(DecodeJournal::open(journal));
    QVERIFY(DecodeJournal::active());
  }

  // The same on a real full filesystem: a small tmpfs, filled. Containers
  // give /dev/shm one of 64 MiB; a desktop's is half its memory and skipped.
  void aJournalOnAFullTmpfsIsOff() {
    struct statvfs space {};
    if (::statvfs("/dev/shm", &space) != 0 || ::access("/dev/shm", W_OK) != 0 ||
        quint64(space.f_blocks) * space.f_frsize > 256ull * 1024 * 1024) {
      QSKIP("No small writable tmpfs at /dev/shm to fill");
    }
    const QString folder = QStringLiteral("/dev/shm/omaroll-journal-test-%1").arg(::getpid());
    const QByteArray filler = QFile::encodeName(folder + QStringLiteral("/filler"));
    const auto cleanup = qScopeGuard([&] { QDir(folder).removeRecursively(); });
    QVERIFY(QDir().mkpath(folder));
    // Unlinked at once: its space comes back when the last process holding
    // it exits, however this test ends.
    const int fill = ::open(filler.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    QVERIFY(fill >= 0);
    const auto release = qScopeGuard([fill] { ::close(fill); });
    QCOMPARE(::unlink(filler.constData()), 0);
    const QByteArray chunk(1024 * 1024, '\x5a');
    while (::write(fill, chunk.constData(), size_t(chunk.size())) == chunk.size()) {
    }
    const QByteArray page(4096, '\x5a');
    while (::write(fill, page.constData(), size_t(page.size())) == page.size()) {
    }
    // In a child, so the old SIGBUS shows as a failure rather than ending
    // the suite.
    const QString store = m_dir.filePath(QStringLiteral("journal-tmpfs-store"));
    const QString picture = m_dir.filePath(QStringLiteral("tiny.jpg"));
    const pid_t child = ::fork();
    if (child == 0) {
      ::prctl(PR_SET_DUMPABLE, 0);
      ::alarm(20);
      const bool opened = DecodeJournal::open(store, folder);
      DecodeJournal::Scope scope(picture);
      const bool decoded = !PinnedImageReader(picture).read().isNull();
      ::_exit(opened ? 3 : decoded ? 0 : 4);
    }
    QVERIFY(child > 0);
    int status = 0;
    QCOMPARE(::waitpid(child, &status, 0), child);
    QVERIFY2(WIFEXITED(status), qPrintable(QStringLiteral("signal %1").arg(WTERMSIG(status))));
    QCOMPARE(WEXITSTATUS(status), 0);
  }

  // A blocked GIF is not played: Qt's movie reader would read it again with
  // nothing recorded. The viewer shows why instead.
  void aBlockedAnimationShowsItsErrorInsteadOfPlaying() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString gif = QFileInfo(folder.path()).canonicalFilePath() + QStringLiteral("/crashed.gif");
    QVERIFY(writeFile(gif, QByteArray::fromHex(
                               "47494638396101000100800000000000ffffff2c00000000010001000002024401003b")));
    QVERIFY(ImageFormats::looksAnimated(gif, QStringLiteral("gif")));
    const QString journal = m_dir.filePath(QStringLiteral("journal-gif"));
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    QVERIFY(DecodeJournal::open(journal));
    {
      DecodeJournal::Scope scope(gif);
      DecodeJournal::simulateCrash();
      DecodeJournal::close();
    }
    QVERIFY(DecodeJournal::open(journal));
    const QList<CaptureRecord> records = CaptureScanner::scan({{folder.path()}});
    QCOMPARE(records.size(), 1);
    QVERIFY(!records.first().animated);
    ViewerSession session;
    session.open({gif});
    QTRY_COMPARE(session.count(), 1);
    QVERIFY(!session.isAnimated());
    QTRY_VERIFY(!session.pictureError().isEmpty());
    QVERIFY(session.imageUrl().isEmpty());
    session.diagnosePicture();
    QTRY_COMPARE(session.pictureFailure().value(QStringLiteral("cause")).toString(),
                 QStringLiteral("crashed"));
    DecodeJournal::close();
    // Not blocked, it plays.
    const QList<CaptureRecord> again = CaptureScanner::scan({{folder.path()}});
    QVERIFY(again.first().animated);
  }

  // Recording is a copy into shared memory. Printed for the record, and
  // bounded loosely so a sanitizer build still passes.
  void journalCostsNothingNextToADecode() {
    const QString journal = m_dir.filePath(QStringLiteral("journal-cost"));
    const QString picture = m_dir.filePath(QStringLiteral("tiny.jpg"));
    const auto closeJournal = qScopeGuard([] { DecodeJournal::close(); });
    QVERIFY(DecodeJournal::open(journal));
    QElapsedTimer timer;
    constexpr int kScopes = 200000;
    timer.start();
    for (int i = 0; i < kScopes; ++i) {
      DecodeJournal::Scope scope(picture);
    }
    const double scopeNs = double(timer.nsecsElapsed()) / kScopes;
    // The alternative the journal avoids: a marker file written and removed.
    constexpr int kMarkers = 2000;
    const QByteArray marker = QFile::encodeName(journal + QStringLiteral("/marker"));
    timer.restart();
    for (int i = 0; i < kMarkers; ++i) {
      const int fd = ::open(marker.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      QVERIFY(fd >= 0);
      QVERIFY(::write(fd, picture.toUtf8().constData(), size_t(picture.toUtf8().size())) > 0);
      ::close(fd);
      ::unlink(marker.constData());
    }
    const double markerNs = double(timer.nsecsElapsed()) / kMarkers;
    // A pinned header read and a plain one, and a whole tile.
    constexpr int kReads = 2000;
    timer.restart();
    for (int i = 0; i < kReads; ++i) (void)QImageReader(picture).size();
    const double plainNs = double(timer.nsecsElapsed()) / kReads;
    timer.restart();
    for (int i = 0; i < kReads; ++i) (void)PinnedImageReader(picture).size();
    const double pinnedNs = double(timer.nsecsElapsed()) / kReads;
    timer.restart();
    for (int i = 0; i < kReads; ++i) {
      PinnedImageReader reader(picture);
      reader.setScaledSize(QSize(12, 9));
      (void)reader.read();
    }
    const double tileNs = double(timer.nsecsElapsed()) / kReads;
    DecodeJournal::close();
    qInfo("decode journal: %.0f ns per recorded decode, %.0f ns per marker file written and "
          "removed; header read %.0f ns plain, %.0f ns pinned and recorded; a 24x18 JPEG tile "
          "%.0f ns",
          scopeNs, markerNs, plainNs, pinnedNs, tileNs);
    QVERIFY2(scopeNs < 20000, qPrintable(QString::number(scopeNs)));
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
    // Another picture format than the name: one Omaroll does not list (the
    // fuzzer's TIM crash), and one it does.
    for (const char* name : {"tim.png", "tim.jpg", "tim.tga", "tim.gif"}) {
      if (!put(name, timCrashInput())) return false;
    }
    if (!put("png-inside.jpg", pngBytes(QSize(24, 18)))) return false;
    if (!put("jpeg-inside.gif", jpegBytes(QSize(24, 18)))) return false;
    if (!put("jpeg-inside.webp", jpegBytes(QSize(24, 18)))) return false;
    if (!put("png-inside.gif", pngBytes(QSize(24, 18)))) return false;
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
