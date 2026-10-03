#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/TailscalePeers.h"
#include "app/AppSettings.h"
#include "app/DemoLibrary.h"
#include "app/HeadlessAudio.h"
#include "app/SingleInstance.h"
#include "app/OpenRequest.h"
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/DuplicateIndex.h"
#include "library/MediaMetadataIndex.h"
#include "library/MediaInspector.h"
#include "library/SimilarityIndex.h"
#include "edit/ImageEditor.h"
#include "edit/JpegTransform.h"
#include "matte/HueExtractor.h"
#include "matte/MatteComposer.h"
#include "pdf/PdfInspector.h"
#include "pdf/PdfSupport.h"
#include "search/OcrIndex.h"
#include "search/QrDetector.h"
#include "subtitles/SubtitleIndex.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureLocations.h"
#include "sources/CaptureScanner.h"
#include "theme/OmarchyTheme.h"
#include "thumbs/ThumbnailCache.h"
#include "viewer/HyprlandPlacement.h"
#include "viewer/ViewerSession.h"

#include <QClipboard>
#include <QColorSpace>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageIOHandler>
#include <QImageReader>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QScopeGuard>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTransform>
#include <QTimer>
#include <QThreadPool>
#include <QtTest>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <memory>

namespace {

// A JPEG carrying a single EXIF Orientation tag. Qt cannot write one, and the
// alternatives (ImageMagick, exiftool) are not guaranteed on a build machine,
// so the APP1 segment is assembled by hand and inserted after the SOI marker.
QByteArray withExifOrientation(const QByteArray& jpeg, quint16 orientation) {
  if (jpeg.size() < 2 || static_cast<quint8>(jpeg.at(0)) != 0xFF ||
      static_cast<quint8>(jpeg.at(1)) != 0xD8) {
    return jpeg;
  }
  QByteArray payload;
  payload.append("Exif\0\0", 6);
  QByteArray tiff;
  const quint16 length = static_cast<quint16>(2 + 6 + 8 + 18);
  QByteArray app1;
  app1.append(static_cast<char>(0xFF));
  app1.append(static_cast<char>(0xE1));
  app1.append(static_cast<char>((length >> 8) & 0xFF));
  app1.append(static_cast<char>(length & 0xFF));
  tiff.append("MM", 2);                        // big-endian
  tiff.append("\x00\x2A", 2);                  // 42
  tiff.append("\x00\x00\x00\x08", 4);          // IFD0 at offset 8
  tiff.append("\x00\x01", 2);                  // one entry
  tiff.append("\x01\x12", 2);                  // Orientation
  tiff.append("\x00\x03", 2);                  // SHORT
  tiff.append("\x00\x00\x00\x01", 4);          // count 1
  tiff.append(static_cast<char>((orientation >> 8) & 0xFF));
  tiff.append(static_cast<char>(orientation & 0xFF));
  tiff.append("\x00\x00", 2);                  // value padded to 4 bytes
  tiff.append("\x00\x00\x00\x00", 4);          // no next IFD
  app1.append(payload);
  app1.append(tiff);
  return jpeg.left(2) + app1 + jpeg.mid(2);
}

// One TIFF entry for cameraTiff(): ASCII text, or unsigned integers, or a
// single rational as numerator and denominator.
struct TiffField {
  quint16 tag = 0;
  quint16 type = 0;
  QList<quint32> values;
  QByteArray text;
};

// A TIFF structure the way cameras write one: IFD0, and when |exif| is given,
// an Exif IFD it points to. Values longer than four bytes follow the IFDs.
QByteArray cameraTiff(bool bigEndian, const QList<TiffField>& first,
                      const QList<TiffField>& exif = {}) {
  const auto put16 = [bigEndian](QByteArray& out, quint32 value) {
    const char bytes[] = {char(value >> (bigEndian ? 8 : 0)), char(value >> (bigEndian ? 0 : 8))};
    out.append(bytes, 2);
  };
  const auto put32 = [bigEndian](QByteArray& out, quint32 value) {
    for (int index = 0; index < 4; ++index) {
      out.append(char(value >> (bigEndian ? 24 - 8 * index : 8 * index)));
    }
  };
  QList<TiffField> zeroth = first;
  const quint32 firstAt = 8;
  const quint32 exifAt = firstAt + 2 + 12 * quint32(zeroth.size() + (exif.isEmpty() ? 0 : 1)) + 4;
  if (!exif.isEmpty()) {
    zeroth.append({0x8769, 4, {exifAt}, {}});
  }
  quint32 dataAt = exifAt + (exif.isEmpty() ? 0 : 2 + 12 * quint32(exif.size()) + 4);

  QByteArray out(bigEndian ? "MM" : "II", 2);
  put16(out, 42);
  put32(out, firstAt);
  QByteArray data;
  const auto directory = [&](const QList<TiffField>& fields) {
    put16(out, quint32(fields.size()));
    for (const TiffField& field : fields) {
      put16(out, field.tag);
      put16(out, field.type);
      if (field.type == 2) {
        const QByteArray text = field.text + '\0';
        put32(out, quint32(text.size()));
        if (text.size() <= 4) {
          out.append(text.leftJustified(4, '\0'));
        } else {
          put32(out, dataAt + quint32(data.size()));
          data.append(text);
        }
      } else if (field.type == 5) {
        put32(out, 1);
        put32(out, dataAt + quint32(data.size()));
        put32(data, field.values.value(0));
        put32(data, field.values.value(1));
      } else {
        put32(out, quint32(field.values.size()));
        if (field.type == 3) {
          put16(out, field.values.value(0));
          put16(out, 0);
        } else {
          put32(out, field.values.value(0));
        }
      }
    }
    put32(out, 0);
  };
  directory(zeroth);
  if (!exif.isEmpty()) {
    directory(exif);
  }
  return out + data;
}

QList<TiffField> rawCameraFields(quint16 orientation) {
  return {{0x010F, 2, {}, "SONY"}, {0x0110, 2, {}, "ILCE-7CM2"}, {0x0112, 3, {orientation}, {}}};
}

QList<TiffField> rawShotFields() {
  return {{0x829A, 5, {1, 160}, {}},
          {0x829D, 5, {32, 10}, {}},
          {0x8827, 3, {3200}, {}},
          {0x9003, 2, {}, "2026:10:01 08:32:17"},
          {0x920A, 5, {280, 10}, {}},
          {0xA434, 2, {}, "E 28-200mm F2.8-5.6 A071"}};
}

// An ISO base media box.
QByteArray box(const QByteArray& type, const QByteArray& payload) {
  const quint32 size = quint32(8 + payload.size());
  QByteArray out;
  for (int index = 0; index < 4; ++index) {
    out.append(char(size >> (24 - 8 * index)));
  }
  return out + type + payload;
}

QString rawFixture(const QString& name) {
  return QFINDTESTDATA(QStringLiteral("fixtures/raw/") + name);
}

QByteArray fileHash(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly)
             ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
             : QByteArray();
}

} // namespace

class OmarollTest : public QObject {
  Q_OBJECT

private slots:
  // Settings and the thumbnail cache both write to the home directory. Point
  // them at a scratch tree so a test run cannot touch a real library's marks.
  void initTestCase() {
    QVERIFY(m_scratch.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_scratch.filePath(QStringLiteral("config")));
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch.filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME", m_scratch.filePath(QStringLiteral("data")).toUtf8()));
  }

  void primaryDefaultsPreserveExistingProfilesAndFreshRestarts() {
    QTemporaryDir profiles;
    QVERIFY(profiles.isValid());
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profiles.filePath("fresh"));
    {
      AppSettings fresh;
      QCOMPARE(fresh.imagePrimaryAction(), QStringLiteral("preview"));
      QCOMPARE(fresh.videoPrimaryAction(), QStringLiteral("preview"));
      fresh.setTileWidth(320);
    }
    {
      AppSettings restarted;
      QCOMPARE(restarted.imagePrimaryAction(), QStringLiteral("preview"));
      QCOMPARE(restarted.videoPrimaryAction(), QStringLiteral("preview"));
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profiles.filePath("existing"));
    {
      QSettings stored(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("omaroll"), QStringLiteral("omaroll"));
      stored.setValue(QStringLiteral("view/tileWidth"), 320);
    }
    {
      AppSettings existing;
      QCOMPARE(existing.imagePrimaryAction(), QStringLiteral("matte"));
      QCOMPARE(existing.videoPrimaryAction(), QStringLiteral("trim"));
      existing.setImagePrimaryAction(QStringLiteral("edit"));
      existing.setVideoPrimaryAction(QStringLiteral("play"));
    }
    {
      AppSettings explicitPreferences;
      QCOMPARE(explicitPreferences.imagePrimaryAction(), QStringLiteral("edit"));
      QCOMPARE(explicitPreferences.videoPrimaryAction(), QStringLiteral("play"));
    }
  }

  void singleInstanceForwardsTheOpenedPath() {
    const QString server = QStringLiteral("omaroll-test-%1-%2")
                               .arg(QCoreApplication::applicationPid())
                               .arg(QRandomGenerator::global()->generate());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);

    SingleInstance second(server);
    const QString path = QStringLiteral("/tmp/a strange # capture.png");
    QVERIFY(!second.claimOrNotify({path}));
    QTRY_COMPARE_WITH_TIMEOUT(activation.size(), 1, 1000);
    QCOMPARE(activation.first().first().toStringList(), QStringList{path});
  }

  void singleInstanceForwardsMultiplePaths() {
    const QString server = QStringLiteral("omaroll-selection-%1").arg(QCoreApplication::applicationPid());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);
    const QStringList paths{QStringLiteral("/tmp/one folder/a # 雪.png"),
                            QStringLiteral("/tmp/other folder/b.webp"),
                            QStringLiteral("/tmp/one folder/c.gif")};
    SingleInstance second(server);
    QVERIFY(!second.claimOrNotify(paths));
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().first().toStringList(), paths);
    SingleInstance third(server);
    QVERIFY(!third.claimOrNotify());
    QTRY_COMPARE(activation.size(), 2);
    QVERIFY(activation.last().first().toStringList().isEmpty());
  }

  void openRequestValidatesAndPreservesSelection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a space # 雪.png"));
    const QString second = dir.filePath(QStringLiteral(".hidden.webp"));
    for (const QString& path : {first, second}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("fixture");
    }
    const QString alias = dir.filePath(QStringLiteral("alias.png"));
    QVERIFY(QFile::link(first, alias));
    const OpenRequest selection = OpenRequest::fromPaths({second, first, alias});
    QVERIFY(selection.error.isEmpty());
    QCOMPARE(selection.files, (QStringList{second, first}));
    QVERIFY(selection.folder.isEmpty());
    const OpenRequest folder = OpenRequest::fromPaths({dir.path()});
    QVERIFY(folder.error.isEmpty());
    QCOMPARE(folder.folder, dir.path());
    for (const QStringList& paths : {QStringList{first, dir.filePath("missing.png")},
                                     QStringList{first, dir.path()},
                                     QStringList{QDir::homePath()}}) {
      const OpenRequest rejected = OpenRequest::fromPaths(paths);
      QVERIFY(!rejected.error.isEmpty());
      QVERIFY(rejected.files.isEmpty());
      QVERIFY(rejected.folder.isEmpty());
    }
    // Explicit files do not pull in neighbors and may include dotfiles.
    const auto records = CaptureScanner::scan({{second, 1}, {first, 1}, {alias, 1}});
    QCOMPARE(records.size(), 2);
    QSet<QString> found;
    for (const auto& record : records) found.insert(record.path);
    QCOMPARE(found, (QSet<QString>{first, second}));
  }

  void singleInstanceAcceptsLegacyAndFragmentedRequests() {
    const QString server = QStringLiteral("omaroll-fragments-%1").arg(QCoreApplication::applicationPid());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);
    QLocalSocket sender;
    sender.connectToServer(server);
    QVERIFY(sender.waitForConnected());
    const QString path = QStringLiteral("/tmp/space # 雪.png");
    const QByteArray message = QJsonDocument(QJsonObject{{QStringLiteral("path"), path}}).toJson(QJsonDocument::Compact);
    sender.write(message.left(message.size() / 2));
    sender.flush();
    QTest::qWait(30);
    QCOMPARE(activation.size(), 0);
    sender.write(message.mid(message.size() / 2) + "\n");
    sender.flush();
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().first().toStringList(), QStringList{path});
  }

  void executableForwardsCanonicalSelectionAndRejectsInvalidBatch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("--render=雪.png"));
    const QString second = dir.filePath(QStringLiteral("a space #.webp"));
    for (const QString& path : {first, second}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("fixture");
    }
    const QByteArray oldDisplay = qgetenv("WAYLAND_DISPLAY");
    qputenv("WAYLAND_DISPLAY", QFileInfo(dir.path()).fileName().toUtf8());
    SingleInstance receiver;
    oldDisplay.isNull() ? qunsetenv("WAYLAND_DISPLAY") : qputenv("WAYLAND_DISPLAY", oldDisplay);
    QVERIFY(receiver.claimOrNotify());
    QSignalSpy activation(&receiver, &SingleInstance::activationRequested);
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QString());
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QFileInfo(dir.path()).fileName());
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(dir.path());
    const QString binary = QCoreApplication::applicationDirPath() + QStringLiteral("/omaroll");
    process.start(binary, {QStringLiteral("--"), QFileInfo(first).fileName(), second, first});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().first().toStringList(), (QStringList{first, second}));
    activation.clear();
    process.start(binary, {first, dir.filePath(QStringLiteral("missing.png"))});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 2);
    QVERIFY(process.readAllStandardError().contains("does not exist"));
    QTest::qWait(30);
    QVERIFY(activation.isEmpty());
  }

  void singleInstanceForwardsTheLibraryRequest() {
    const QString server = QStringLiteral("omaroll-library-%1").arg(QCoreApplication::applicationPid());
    SingleInstance first(server);
    QVERIFY(first.claimOrNotify());
    QSignalSpy activation(&first, &SingleInstance::activationRequested);
    const QString path = QStringLiteral("/tmp/a picture # 雪.png");
    SingleInstance second(server);
    QVERIFY(!second.claimOrNotify({path}, true));
    QTRY_COMPARE(activation.size(), 1);
    QCOMPARE(activation.first().at(0).toStringList(), QStringList{path});
    QVERIFY(activation.first().at(1).toBool());
    SingleInstance third(server);
    QVERIFY(!third.claimOrNotify({path}));
    QTRY_COMPARE(activation.size(), 2);
    QVERIFY(!activation.last().at(1).toBool());
  }

  void viewerOpensOnlyPicturesAndVideos() {
    QVERIFY(ViewerSession::canOpen({QStringLiteral("/x/a.png"), QStringLiteral("/x/b.MP4"),
                                    QStringLiteral("/x/c.webp")}));
    QVERIFY(!ViewerSession::canOpen({QStringLiteral("/x/a.png"), QStringLiteral("/x/doc.pdf")}));
    QVERIFY(!ViewerSession::canOpen({QStringLiteral("/x/doc.PDF")}));
    QVERIFY(!ViewerSession::canOpen({}));
  }

  void extensionlessMediaOpensNavigatesAndAppearsInTheLibrary() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = dir.filePath(QStringLiteral("pictures.with.dots"));
    QVERIFY(QDir().mkpath(folder));
    const DemoLibrary::Layout demo = DemoLibrary::build();
    const QStringList originals{
        QFINDTESTDATA("fixtures/viewer/transparent.png"),
        demo.pictures + QStringLiteral("/alpine-dawn.jpg"),
        QFINDTESTDATA("fixtures/viewer/animated.gif"),
        QFINDTESTDATA("fixtures/viewer/animated.webp"),
        demo.videos + QStringLiteral("/ocean-surface.mp4"),
        QFINDTESTDATA("fixtures/viewer/tracks.mkv")};
    QStringList paths;
    for (int i = 0; i < originals.size(); ++i) {
      QVERIFY(!originals.at(i).isEmpty());
      const QString path = folder + QStringLiteral("/media %1 # 雪").arg(i);
      QVERIFY(QFile::copy(originals.at(i), path));
      paths.append(path);
    }
    const OpenRequest opened = OpenRequest::fromPaths(paths);
    QVERIFY2(opened.error.isEmpty(), qPrintable(opened.error));
    QCOMPARE(opened.files, paths);
    QVERIFY(ViewerSession::canOpen(paths));
    QCOMPARE(ViewerSession::siblings(folder), paths);
    const auto records = CaptureScanner::scan({{folder, 1}});
    QCOMPARE(records.size(), paths.size());
    ViewerSession viewer;
    viewer.open(paths);
    AppSettings settings;
    CaptureModel model(&settings);
    model.addExtraFiles(paths);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    for (int i = 0; i < paths.size(); ++i) {
      const bool video = i >= 4;
      const bool animated = i == 2 || i == 3;
      QVERIFY(viewer.jump(i) || i == 0);
      QCOMPARE(viewer.isVideo(), video);
      QCOMPARE(viewer.isAnimated(), animated);
      const int next = (i + 1) % paths.size();
      QCOMPARE(viewer.neighbourIsVideo(1), next >= 4);
      QCOMPARE(viewer.neighbourIsAnimated(1), next == 2 || next == 3);
      QTRY_VERIFY_WITH_TIMEOUT(model.rowOf(paths.at(i)) >= 0, 5000);
      const auto& record = model.recordAt(model.rowOf(paths.at(i)));
      QCOMPARE(record.fileName, QFileInfo(paths.at(i)).fileName());
      QCOMPARE(record.isVideo(), video);
      QCOMPARE(record.animated, animated);
      QCOMPARE(proxy.isAnimatedAt(proxy.rowOf(paths.at(i))), animated);
      QVERIFY2(!ThumbnailCache::thumbnail(paths.at(i), QSize(64, 64), 1).isNull(),
               qPrintable(paths.at(i)));
      if (!video) {
        QImageReader reader(paths.at(i));
        QVERIFY2(!reader.read().isNull(), qPrintable(reader.errorString()));
      }
    }
    QVERIFY(!proxy.isAnimatedAt(-1));
    QVERIFY(!proxy.isAnimatedAt(proxy.rowCount()));
    // Edited copies keep the detected format rather than falling back to PNG.
    QCOMPARE(ImageEditor::outputPathFor(paths.at(1)), paths.at(1) + QStringLiteral("-edited.jpg"));
    QCOMPARE(ImageEditor::outputPathFor(paths.at(0)), paths.at(0) + QStringLiteral("-edited.png"));
  }

  // A minimal Matroska header: one track of the given type and codec, in a
  // segment of unknown size as a live recorder writes it.
  static QByteArray matroska(char trackType, const QByteArray& codec) {
    const QByteArray entry = QByteArray::fromHex("8381") + QByteArray(1, trackType) +
                             QByteArray(1, '\x86') + QByteArray(1, char(0x80 | codec.size())) + codec;
    const QByteArray tracks = QByteArray(1, '\xae') + QByteArray(1, char(0x80 | entry.size())) + entry;
    return QByteArray::fromHex("1a45dfa38b4282886d6174726f736b61") +
           QByteArray::fromHex("1853806701ffffffffffffff") + QByteArray::fromHex("1654ae6b") +
           QByteArray(1, char(0x80 | tracks.size())) + tracks;
  }

  void extensionlessDetectionRejectsNonMediaAndKeepsExtensionsAuthoritative() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const auto& [type, codec] : {std::pair{char(1), QByteArray("V_VP9")},
                                      std::pair{char(1), QByteArray("V_MPEG4/ISO/AVC")}}) {
      const QString video = dir.filePath(QStringLiteral("live-recording"));
      QFile file(video);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(matroska(type, codec));
      file.close();
      QCOMPARE(CaptureScanner::mediaSuffix(video), QStringLiteral("mkv"));
      QVERIFY(QFile::remove(video));
    }
    const QList<QByteArray> contents{
        {}, "ordinary text", QByteArray(1024, '\x01'),
        QByteArray::fromHex("7f454c4602010100000000000000000002003e00"),
        QByteArray::fromHex("504b03041400000008000000000000000000"),
        "<?xml version=\"1.0\"?><document>not an image</document>",
        QByteArray("RIFF\x24\x00\x00\x00WAVEfmt ", 16),
        // Audio-only Matroska: generic container type, no V_ codec.
        QByteArray::fromHex("1a45dfa3934282886d6174726f736b61") +
            QByteArray::fromHex("8685") + "A_AAC",
        // Audio-only again, with "86 82 V_" in a Void element after the track.
        // Only a CodecID inside a track entry counts.
        matroska(2, "A_AAC") + QByteArray::fromHex("ec848682565f")};
    for (int i = 0; i < contents.size(); ++i) {
      const QString path = dir.filePath(QStringLiteral("not-media-%1").arg(i));
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(contents.at(i)), contents.at(i).size());
      file.close();
      QVERIFY(!OpenRequest::fromPaths({path}).error.isEmpty());
      QVERIFY(!ViewerSession::canOpen({path}));
    }
    const QString disguised = dir.filePath(QStringLiteral("image.txt"));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/viewer/transparent.png"), disguised));
    QVERIFY(!OpenRequest::fromPaths({disguised}).error.isEmpty());
    QVERIFY(!ViewerSession::canOpen({disguised}));
    QVERIFY(CaptureScanner::scan({{dir.path(), 1}}).isEmpty());
    QVERIFY(CaptureScanner::mediaSuffix(dir.path()).isEmpty());
    QVERIFY(CaptureScanner::mediaSuffix(dir.filePath("missing")).isEmpty());
  }

  void extensionlessViewerRefreshesReplacedFilesOnReopenAndRelist() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("same-name"));
    QImage picture(24, 16, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    QVERIFY(picture.save(path, "PNG"));
    ViewerSession viewer;
    viewer.open({path});
    QTRY_COMPARE(viewer.count(), 1);
    QVERIFY(!viewer.isVideo());
    QVERIFY(!viewer.isAnimated());
    const auto replaceWith = [&](const QString& source) {
      QFile input(source);
      QVERIFY(input.open(QIODevice::ReadOnly));
      QSaveFile output(path);
      QVERIFY(output.open(QIODevice::WriteOnly));
      const QByteArray bytes = input.readAll();
      QCOMPARE(output.write(bytes), bytes.size());
      QVERIFY(output.commit());
    };
    const auto demo = DemoLibrary::build();
    replaceWith(demo.videos + QStringLiteral("/ocean-surface.mp4"));
    viewer.open({path});
    QVERIFY(viewer.isVideo());
    QVERIFY(!viewer.isAnimated());
    replaceWith(QFINDTESTDATA("fixtures/viewer/animated.gif"));
    QTRY_VERIFY_WITH_TIMEOUT(viewer.isAnimated(), 5000);
    QVERIFY(!viewer.isVideo());
    viewer.clear();
    QVERIFY(!viewer.isVideo());
    QVERIFY(!viewer.isAnimated());
  }

  void viewerListsPicturesAndVideosInNaturalNameOrder_data() {
    QTest::addColumn<QString>("localeName");
    QTest::newRow("minimal-environment") << QStringLiteral("C");
    QTest::newRow("english") << QStringLiteral("en_US");
    QTest::newRow("german") << QStringLiteral("de_DE");
  }

  void viewerListsPicturesAndVideosInNaturalNameOrder() {
    QFETCH(QString, localeName);
    const QLocale previousLocale;
    const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(previousLocale); });
    QLocale::setDefault(QLocale(localeName));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const QString& name :
         {QStringLiteral("shot 10.png"), QStringLiteral("shot 2.png"), QStringLiteral("Shot 1.jpg"),
          QStringLiteral("clip.mp4"), QStringLiteral("notes.pdf"), QStringLiteral("readme.txt"),
          QStringLiteral(".hidden.png"), QStringLiteral("no-suffix")}) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("fixture");
    }
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("nested.png")));
    const QStringList expected{dir.filePath(QStringLiteral("clip.mp4")),
                               dir.filePath(QStringLiteral("Shot 1.jpg")),
                               dir.filePath(QStringLiteral("shot 2.png")),
                               dir.filePath(QStringLiteral("shot 10.png"))};
    QCOMPARE(ViewerSession::siblings(dir.path(), dir.filePath(QStringLiteral("shot 2.png"))),
             expected);
    // The dotfile shows only when it is the one opened.
    const QString hidden = dir.filePath(QStringLiteral(".hidden.png"));
    const QStringList withHidden = ViewerSession::siblings(dir.path(), hidden);
    QCOMPARE(withHidden.size(), expected.size() + 1);
    QVERIFY(withHidden.contains(hidden));
  }

  void viewerStepsWrapsAndFollowsTheFolder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto make = [&dir](const QString& name) {
      QFile file(dir.filePath(name));
      if (file.open(QIODevice::WriteOnly)) {
        file.write("fixture");
      }
      return dir.filePath(name);
    };
    const QString a = make(QStringLiteral("a.png"));
    const QString b = make(QStringLiteral("b.png"));
    const QString c = make(QStringLiteral("c.mp4"));
    QVERIFY(QFile::exists(a) && QFile::exists(b) && QFile::exists(c));

    ViewerSession session;
    session.open({b});
    // The opened file shows before the folder has been read.
    QCOMPARE(session.path(), b);
    QCOMPARE(session.fileName(), QStringLiteral("b.png"));
    QVERIFY(!session.isVideo());
    QTRY_COMPARE(session.count(), 3);
    QCOMPARE(session.index(), 1);
    QVERIFY(!session.selection());

    QVERIFY(session.step(1));
    QCOMPARE(session.path(), c);
    QVERIFY(session.isVideo());
    QVERIFY(session.step(1));
    QCOMPARE(session.path(), a);
    QVERIFY(session.step(-1));
    QCOMPARE(session.path(), c);
    QCOMPARE(session.neighbourUrl(1), QUrl::fromLocalFile(a));
    QCOMPARE(session.neighbourUrl(-1), QUrl::fromLocalFile(b));
    QVERIFY(session.neighbourIsVideo(0) == false);
    QVERIFY(session.jump(0));
    QCOMPARE(session.path(), a);
    QVERIFY(!session.jump(3));

    // A file landing in the folder joins; the one on screen going away is
    // replaced by whatever now sits in its place.
    const QString d = make(QStringLiteral("d.png"));
    QVERIFY(QFile::exists(d));
    QTRY_COMPARE_WITH_TIMEOUT(session.count(), 4, 5000);
    QCOMPARE(session.path(), a);
    QVERIFY(QFile::remove(a));
    QTRY_COMPARE_WITH_TIMEOUT(session.count(), 3, 5000);
    QCOMPARE(session.path(), b);
    QVERIFY(session.step(-1));
    QCOMPARE(session.path(), d);

    session.clear();
    QCOMPARE(session.count(), 0);
    QVERIFY(session.path().isEmpty());
    QVERIFY(!session.step(1));
  }

  void viewerKeepsASelectionAndMovesOnFromATrashedFile() {
    ViewerSession session;
    QSignalSpy emptied(&session, &ViewerSession::emptied);
    const QStringList paths{QStringLiteral("/x/3.png"), QStringLiteral("/x/1.png"),
                            QStringLiteral("/y/2.mp4")};
    session.open(paths);
    QVERIFY(session.selection());
    QCOMPARE(session.count(), 3);
    QCOMPARE(session.path(), paths.at(0));
    QVERIFY(session.jump(1));
    session.forget(paths.at(1));
    QCOMPARE(session.path(), paths.at(2));
    QCOMPARE(session.index(), 1);
    session.forget(paths.at(2));
    QCOMPARE(session.path(), paths.at(0));
    session.forget(QStringLiteral("/not/in/the/list.png"));
    QCOMPARE(session.count(), 1);
    QCOMPARE(emptied.size(), 0);
    session.forget(paths.at(0));
    QCOMPARE(session.count(), 0);
    QCOMPARE(emptied.size(), 1);

    QSignalSpy library(&session, &ViewerSession::libraryRequested);
    session.openInLibrary();
    QCOMPARE(library.size(), 0);
    session.open({QStringLiteral("/x/1.png"), QStringLiteral("/x/3.png")});
    session.openInLibrary();
    QCOMPARE(library.size(), 1);
    QCOMPARE(library.first().first().toString(), QStringLiteral("/x/1.png"));
  }

  // Once the last file is gone the session stops watching, so a file that
  // lands in the folder later cannot refill a viewer that has closed.
  void viewerStopsWatchingOnceItsLastFileIsGone() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString only = dir.filePath(QStringLiteral("only.png"));
    QVERIFY(QImage(4, 4, QImage::Format_RGB32).save(only));
    ViewerSession session;
    QSignalSpy emptied(&session, &ViewerSession::emptied);
    session.open({only});
    QTRY_VERIFY(!session.path().isEmpty());
    QTest::qWait(100);
    QVERIFY(QFile::remove(only));
    session.forget(only);
    QCOMPARE(session.count(), 0);
    QCOMPARE(emptied.size(), 1);
    QVERIFY(QImage(4, 4, QImage::Format_RGB32).save(dir.filePath(QStringLiteral("later.png"))));
    QTest::qWait(900);
    QCOMPARE(session.count(), 0);
    QCOMPARE(emptied.size(), 1);
  }

  // Only this process's viewers on the active workspace decide placement:
  // the library, other processes and other workspaces are left alone, and
  // nothing but a hex address ever reaches a dispatch.
  void hyprlandTilesOnlyThisProcessesViewersOnTheActiveWorkspace() {
    const auto client = [](const char* address, qint64 pid, const char* title, int workspace,
                           bool floating) {
      return QJsonObject{{"address", address},
                         {"pid", pid},
                         {"class", HyprlandPlacement::kWindowClass},
                         {"title", QString::fromUtf8(title)},
                         {"mapped", true},
                         {"floating", floating},
                         {"workspace", QJsonObject{{"id", workspace}, {"name", "x"}}}};
    };
    QJsonArray clients{
        client("0xa1", 42, "first.jpg · Omaroll", 1, true),
        client("0xa2", 42, "second.jpg · Omaroll", 1, false),
        client("0xa3", 42, "Omaroll", 1, true),
        client("0xa4", 7, "other.jpg · Omaroll", 1, true),
        client("0xa5", 42, "elsewhere.jpg · Omaroll", 2, true),
        client("0xzz\"; rm", 42, "odd.jpg · Omaroll", 1, true)};
    QJsonObject foreign = client("0xa6", 42, "foreign.jpg · Omaroll", 1, true);
    foreign.insert("class", "org.gnome.Loupe");
    clients.append(foreign);
    const QByteArray list = QJsonDocument(clients).toJson();
    const auto workspace = [](int id) { return QJsonDocument(QJsonObject{{"id", id}}).toJson(); };

    HyprlandPlacement::Plan plan = HyprlandPlacement::plan(list, workspace(1), 42);
    QCOMPARE(plan.viewers, 3);
    QVERIFY(plan.tileNew());
    QCOMPARE(plan.floating, QStringList{QStringLiteral("0xa1")});

    plan = HyprlandPlacement::plan(list, workspace(2), 42);
    QCOMPARE(plan.viewers, 1);
    QCOMPARE(plan.floating, QStringList{QStringLiteral("0xa5")});

    // Only the library here, or nothing of ours: the new viewer floats.
    QVERIFY(!HyprlandPlacement::plan(list, workspace(3), 42).tileNew());
    QVERIFY(!HyprlandPlacement::plan(list, workspace(1), 99).tileNew());
    QVERIFY(!HyprlandPlacement::plan(list, "not json", 42).tileNew());
    QVERIFY(!HyprlandPlacement::plan("[", workspace(1), 42).tileNew());
  }

  void viewerWindowIsTheSameNormalSizeForEveryFile() {
    // Most of a 16:9 screen, in its shape.
    QCOMPARE(ViewerSession::preferredWindowSize(QSize(1920, 1080)), QSize(1536, 864));
    QCOMPARE(ViewerSession::preferredWindowSize(QSize(1536, 864)), QSize(1229, 691));
    // An ultrawide gets 16:9 rather than a letterbox strip.
    QCOMPARE(ViewerSession::preferredWindowSize(QSize(3440, 1440)), QSize(2048, 1152));
    // A portrait screen still gets a landscape window, 4:3 across its width.
    QCOMPARE(ViewerSession::preferredWindowSize(QSize(1080, 1920)), QSize(864, 648));
    QCOMPARE(ViewerSession::preferredWindowSize(QSize(700, 500)), QSize(640, 480));
    QCOMPARE(ViewerSession::preferredWindowSize(QSize()), QSize(1180, 780));
  }


  void disabledXdgPictureDirectoryDoesNotScanHome() {
    const QByteArray previous = qgetenv("XDG_PICTURES_DIR");
    QVERIFY(qputenv("XDG_PICTURES_DIR", QDir::homePath().toUtf8()));
    QCOMPARE(CaptureLocations::pictures(), QDir::homePath() + QStringLiteral("/Pictures"));
    if (previous.isNull()) {
      qunsetenv("XDG_PICTURES_DIR");
    } else {
      QVERIFY(qputenv("XDG_PICTURES_DIR", previous));
    }
  }

  void automaticFoldersAreVisibleCoalescedAndReportAvailability() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pictures = dir.filePath(QStringLiteral("Pictures"));
    const QString videos = dir.filePath(QStringLiteral("Videos"));
    const QString downloads = dir.filePath(QStringLiteral("Downloads"));
    QVERIFY(QDir().mkpath(pictures));
    QVERIFY(QDir().mkpath(downloads));

    const QByteArray oldPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray oldVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray oldDownloads = qgetenv("XDG_DOWNLOAD_DIR");
    const QByteArray oldShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray oldRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("XDG_PICTURES_DIR", oldPictures);
      putBack("XDG_VIDEOS_DIR", oldVideos);
      putBack("XDG_DOWNLOAD_DIR", oldDownloads);
      putBack("OMARCHY_SCREENSHOT_DIR", oldShots);
      putBack("OMARCHY_SCREENRECORD_DIR", oldRecordings);
    });
    QVERIFY(qputenv("XDG_PICTURES_DIR", pictures.toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", videos.toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", downloads.toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", pictures.toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", videos.toUtf8()));

    AppSettings settings;
    settings.setScanDownloads(true);
    CaptureModel model(&settings);
    const QVariantList folders = model.automaticFolders();
    QCOMPARE(folders.size(), 3);

    QHash<QString, QVariantMap> byPath;
    for (const QVariant& value : folders) {
      const QVariantMap row = value.toMap();
      byPath.insert(row.value(QStringLiteral("path")).toString(), row);
    }
    QCOMPARE(byPath.value(pictures).value(QStringLiteral("label")).toString(),
             QStringLiteral("Screenshots + Pictures"));
    QVERIFY(byPath.value(pictures).value(QStringLiteral("available")).toBool());
    QCOMPARE(byPath.value(videos).value(QStringLiteral("label")).toString(),
             QStringLiteral("Screen recordings + Videos"));
    QVERIFY(!byPath.value(videos).value(QStringLiteral("available")).toBool());
    QVERIFY(byPath.value(downloads).value(QStringLiteral("available")).toBool());
    QVERIFY(model.folderAvailable(pictures));
    QVERIFY(!model.folderAvailable(videos));

    QSignalSpy changed(&model, &CaptureModel::automaticFoldersChanged);
    settings.setScanDownloads(false);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(model.automaticFolders().size(), 2);
  }

  void additionalLibraryFoldersPersistAndRejectHome() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AppSettings settings;
    QVERIFY(!settings.addLibraryFolder(QUrl::fromLocalFile(QDir::homePath())));
    QVERIFY(!settings.addLibraryFolder(QUrl::fromLocalFile(QDir::rootPath())));
    QVERIFY(settings.addLibraryFolder(QUrl::fromLocalFile(dir.path())));
    QVERIFY(!settings.addLibraryFolder(QUrl::fromLocalFile(dir.path())));
    QVERIFY(settings.libraryFolders().contains(dir.path()));
    settings.setImagePrimaryAction(QStringLiteral("view"));
    settings.setVideoPrimaryAction(QStringLiteral("play"));
    settings.setThumbnailCacheMb(512);
    settings.setTileWidth(320);
    settings.setSlideshowVideos(true);

    const QString unavailable = dir.path();
    QVERIFY(QDir(unavailable).removeRecursively());
    AppSettings restored;
    QVERIFY(restored.libraryFolders().contains(unavailable));
    QCOMPARE(restored.imagePrimaryAction(), QStringLiteral("view"));
    QCOMPARE(restored.videoPrimaryAction(), QStringLiteral("play"));
    QCOMPARE(restored.thumbnailCacheMb(), 512);
    QCOMPARE(restored.tileWidth(), 320);
    restored.setTileWidth(10);
    QCOMPARE(restored.tileWidth(), 160);
    restored.setTileWidth(5000);
    QCOMPARE(restored.tileWidth(), 480);
    restored.setTileWidth(240);
    QVERIFY(restored.slideshowVideos());
    restored.setImagePrimaryAction(QStringLiteral("invalid"));
    restored.setVideoPrimaryAction(QStringLiteral("invalid"));
    QCOMPARE(restored.imagePrimaryAction(), QStringLiteral("view"));
    QCOMPARE(restored.videoPrimaryAction(), QStringLiteral("play"));
    restored.removeLibraryFolder(unavailable);
    QVERIFY(!restored.libraryFolders().contains(unavailable));
    restored.setImagePrimaryAction(QStringLiteral("matte"));
    restored.setVideoPrimaryAction(QStringLiteral("trim"));
    restored.setThumbnailCacheMb(256);
    restored.setSlideshowVideos(false);
  }

  void themeUsesLauncherSurfaceColorAndAlpha() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("mode = \"dark\"\nbackground = \"#101820\"\n"
                 "darker_background = \"#05080a\"\nforeground = \"#f0f0f0\"\n");
    colors.close();

    QFile shell(root + QStringLiteral("/theme/shell.toml"));
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"#223344\"\nbackground-alpha = 0.73\n");
    shell.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#223344")));
    QCOMPARE(theme.surfaceAlpha(), 0.73);

    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"#445566\"\nbackground-alpha = 0.91\n");
    shell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#445566")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.91, 1500);

    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"not-a-color\"\nbackground-alpha = 4.0\n");
    shell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#05080a")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 1.0, 1500);
  }

  void themeResolvesCompactTerminalPalette() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("accent = \"#589df6\"\nselection = \"#b5d5ff\"\n"
                 "background = \"#1d2837\"\nforeground = \"#ffffff\"\n"
                 "color0 = \"#000000\"\ncolor1 = \"#f9555f\"\n"
                 "color2 = \"#21b089\"\ncolor3 = \"#fef02a\"\n"
                 "color4 = \"#589df6\"\ncolor5 = \"#944d95\"\n"
                 "color6 = \"#1f9ee7\"\ncolor7 = \"#bbbbbb\"\n"
                 "color8 = \"#555555\"\ncolor9 = \"#fa8c8f\"\n"
                 "color10 = \"#35bb9a\"\ncolor11 = \"#ffff55\"\n"
                 "color12 = \"#589df6\"\ncolor13 = \"#e75699\"\n"
                 "color14 = \"#3979bc\"\ncolor15 = \"#ffffff\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.mode(), QStringLiteral("dark"));
    QCOMPARE(theme.accent(), QColor(QStringLiteral("#589df6")));
    QCOMPARE(theme.selection(), QColor(QStringLiteral("#b5d5ff")));
    QCOMPARE(theme.background(), QColor(QStringLiteral("#1d2837")));
    QCOMPARE(theme.darkBackground(), QColor(QStringLiteral("#161e29")));
    QCOMPARE(theme.darkerBackground(), QColor(QStringLiteral("#0f141c")));
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#0f141c")));
    QCOMPARE(theme.lighterBackground(), QColor(QStringLiteral("#1d2837")));
    QCOMPARE(theme.darkForeground(), QColor(QStringLiteral("#555555")));
    QCOMPARE(theme.lightForeground(), QColor(QStringLiteral("#ffffff")));
    QCOMPARE(theme.brightForeground(), QColor(QStringLiteral("#ffffff")));
    QCOMPARE(theme.muted(), QColor(QStringLiteral("#555555")));
    QCOMPARE(theme.red(), QColor(QStringLiteral("#f9555f")));
    QCOMPARE(theme.green(), QColor(QStringLiteral("#21b089")));
    QCOMPARE(theme.yellow(), QColor(QStringLiteral("#fef02a")));
    QCOMPARE(theme.blue(), QColor(QStringLiteral("#589df6")));
  }

  void themeFallsBackAccentToTerminalBlue() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#16181d\"\nforeground = \"#c5c5d2\"\n"
                 "color4 = \"#6c9ef8\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.accent(), QColor(QStringLiteral("#6c9ef8")));
  }

  void themeResolvesLegacySemanticNames() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("mode = \"dark\"\naccent = \"#fcef0c\"\n"
                 "bg = \"#1b1d1e\"\ndark_bg = \"#353738\"\n"
                 "darker_bg = \"#1a1b1c\"\nlighter_bg = \"#1b1d1e\"\n"
                 "fg = \"#a7a8a3\"\ndark_fg = \"#8a8c89\"\n"
                 "light_fg = \"#c5c5be\"\nbright_fg = \"#dadad5\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.background(), QColor(QStringLiteral("#1b1d1e")));
    QCOMPARE(theme.darkBackground(), QColor(QStringLiteral("#353738")));
    QCOMPARE(theme.darkerBackground(), QColor(QStringLiteral("#1a1b1c")));
    QCOMPARE(theme.lighterBackground(), QColor(QStringLiteral("#1b1d1e")));
    QCOMPARE(theme.foreground(), QColor(QStringLiteral("#a7a8a3")));
    QCOMPARE(theme.darkForeground(), QColor(QStringLiteral("#8a8c89")));
    QCOMPARE(theme.lightForeground(), QColor(QStringLiteral("#c5c5be")));
    QCOMPARE(theme.brightForeground(), QColor(QStringLiteral("#dadad5")));
  }

  void themeFindsLegacyOmarchyLocation() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = config.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#202040\"\nforeground = \"#eeeeff\"\n"
                 "accent = \"#6060ff\"\n");
    colors.close();

    QFile name(root + QStringLiteral("/theme.name"));
    QVERIFY(name.open(QIODevice::WriteOnly | QIODevice::Text));
    name.write("legacy-blue\n");
    name.close();

    OmarchyTheme theme(state.path(), config.path());
    QVERIFY(theme.omarchyAvailable());
    QCOMPARE(theme.themeName(), QStringLiteral("Legacy Blue"));
    QCOMPARE(theme.background(), QColor(QStringLiteral("#202040")));
    QCOMPARE(theme.accent(), QColor(QStringLiteral("#6060ff")));
  }

  void themeFollowsAtomicDirectoryReplacement() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    const QString themePath = root + QStringLiteral("/theme");
    QVERIFY(QDir().mkpath(themePath));
    QFile colors(themePath + QStringLiteral("/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#101020\"\nforeground = \"#eeeeff\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QCOMPARE(theme.background(), QColor(QStringLiteral("#101020")));

    const QString nextPath = root + QStringLiteral("/next-theme");
    QVERIFY(QDir().mkpath(nextPath));
    QFile nextColors(nextPath + QStringLiteral("/colors.toml"));
    QVERIFY(nextColors.open(QIODevice::WriteOnly | QIODevice::Text));
    nextColors.write("background = \"#302010\"\nforeground = \"#fff0ee\"\n");
    nextColors.close();

    QVERIFY(QDir(themePath).removeRecursively());
    QVERIFY(QDir(root).rename(QStringLiteral("next-theme"), QStringLiteral("theme")));
    QTRY_COMPARE_WITH_TIMEOUT(theme.background(), QColor(QStringLiteral("#302010")), 3000);
  }

  void themeUsesMachineLauncherOverride() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());

    const QString themePath = state.filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themePath));
    QFile colors(themePath + QStringLiteral("/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("background = \"#101820\"\nforeground = \"#f0f0f0\"\n");
    colors.close();
    QFile shell(themePath + QStringLiteral("/shell.toml"));
    QVERIFY(shell.open(QIODevice::WriteOnly | QIODevice::Text));
    shell.write("[launcher]\nbackground = \"#223344\"\nbackground-alpha = 0.73\n");
    shell.close();

    const QString userRoot = config.filePath(QStringLiteral("omarchy"));
    QVERIFY(QDir().mkpath(userRoot));
    QFile userShell(userRoot + QStringLiteral("/shell.toml"));
    QVERIFY(userShell.open(QIODevice::WriteOnly | QIODevice::Text));
    userShell.write("[launcher]\nbackground = \"#445566\"\nbackground-alpha = 0.91\n");
    userShell.close();

    OmarchyTheme theme(state.path(), config.path());
    QCOMPARE(theme.surfaceBackground(), QColor(QStringLiteral("#445566")));
    QCOMPARE(theme.surfaceAlpha(), 0.91);

    QVERIFY(userShell.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    userShell.write("[launcher]\nbackground = \"#667788\"\nbackground-alpha = 0.64\n");
    userShell.close();
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#667788")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.64, 1500);

    QVERIFY(userShell.remove());
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceBackground(), QColor(QStringLiteral("#223344")), 1500);
    QTRY_COMPARE_WITH_TIMEOUT(theme.surfaceAlpha(), 0.73, 1500);
  }

  void albumsFollowMovesAndNeverGuessBetweenDuplicates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString album = QStringLiteral("Move test");
    AppSettings settings;
    settings.deleteAlbum(album);
    QVERIFY(settings.createAlbum(album));

    QImage image(12, 12, QImage::Format_RGB32);
    image.fill(Qt::magenta);
    const QString original = dir.filePath(QStringLiteral("original.png"));
    QVERIFY(image.save(original, "PNG"));
    QVERIFY(settings.addToAlbum(album, {original}));
    QCOMPARE(settings.albumPaths(album), QStringList {original});

    const QString renamed = dir.filePath(QStringLiteral("renamed.png"));
    QVERIFY(QFile::rename(original, renamed));
    CaptureRecord renamedRecord;
    renamedRecord.path = renamed;
    renamedRecord.bytes = QFileInfo(renamed).size();
    settings.reconcileAlbums({renamedRecord});
    QCOMPARE(settings.albumPaths(album), QStringList {renamed});
    AppSettings afterRename;
    QCOMPARE(afterRename.albumPaths(album), QStringList {renamed});

    // Copy then remove gives the file a new inode, like a move between
    // filesystems. The content identity still repairs the album entry.
    const QString copied = dir.filePath(QStringLiteral("copied.png"));
    QVERIFY(QFile::copy(renamed, copied));
    QVERIFY(QFile::remove(renamed));
    CaptureRecord copiedRecord;
    copiedRecord.path = copied;
    copiedRecord.bytes = QFileInfo(copied).size();
    settings.reconcileAlbums({copiedRecord});
    QCOMPARE(settings.albumPaths(album), QStringList {copied});

    const QString duplicateA = dir.filePath(QStringLiteral("duplicate-a.png"));
    const QString duplicateB = dir.filePath(QStringLiteral("duplicate-b.png"));
    QVERIFY(QFile::copy(copied, duplicateA));
    QVERIFY(QFile::copy(copied, duplicateB));
    QVERIFY(QFile::remove(copied));
    CaptureRecord a;
    a.path = duplicateA;
    a.bytes = QFileInfo(duplicateA).size();
    CaptureRecord b;
    b.path = duplicateB;
    b.bytes = QFileInfo(duplicateB).size();
    settings.reconcileAlbums({a, b});
    QVERIFY(settings.albumPaths(album).isEmpty());

    AppSettings restored;
    QVERIFY(restored.albumNames().contains(album));
    restored.reconcileAlbums({a, b});
    QVERIFY(restored.albumPaths(album).isEmpty());
    QCOMPARE(restored.albumItemCount(album), 1);
    QCOMPARE(restored.unavailableAlbumItemCount(album), 1);
    restored.removeUnavailableFromAlbum(album);
    QCOMPARE(restored.albumItemCount(album), 0);
    restored.deleteAlbum(album);
  }

  // --- Classification ---------------------------------------------------

  void albumInodeMatchNeedsTheSizeToAgree() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("screenshot-2026-09-02_12-00-00.png"));
    QImage(64, 64, QImage::Format_RGB32).save(path);

    AppSettings settings;
    QVERIFY(settings.createAlbum(QStringLiteral("Inode")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Inode"), {path}));
    QCOMPARE(settings.unavailableAlbumItemCount(QStringLiteral("Inode")), 0);

    // Pretend the inode number was handed to a different file: same path,
    // same device and inode on disk, but the entry remembers another size.
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                     QStringLiteral("omaroll"));
    QVariantMap albums = stored.value(QStringLiteral("library/albums")).toMap();
    QVariantList entries = albums.value(QStringLiteral("Inode")).toList();
    QCOMPARE(entries.size(), 1);
    QVariantMap entry = entries.first().toMap();
    entry.insert(QStringLiteral("bytes"), entry.value(QStringLiteral("bytes")).toLongLong() + 1);
    entry.insert(QStringLiteral("fingerprint"), QByteArray("not-this-file"));
    entries[0] = entry;
    albums.insert(QStringLiteral("Inode"), entries);
    stored.setValue(QStringLiteral("library/albums"), albums);
    stored.sync();

    AppSettings restored;
    QCOMPARE(restored.unavailableAlbumItemCount(QStringLiteral("Inode")), 1);

    CaptureRecord record;
    record.path = path;
    record.bytes = QFileInfo(path).size();
    record.modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    restored.reconcileAlbums({record});
    QCOMPARE(restored.unavailableAlbumItemCount(QStringLiteral("Inode")), 1);
    restored.deleteAlbum(QStringLiteral("Inode"));
  }

  void tagsAndSmartCollectionsPersistAndFollowRenames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString suffix = QString::number(QRandomGenerator::global()->generate());
    const QString tag = QStringLiteral("Tag ") + suffix;
    const QString collection = QStringLiteral("Smart ") + suffix;
    const QString original = dir.filePath(QStringLiteral("original.png"));
    const QString renamed = dir.filePath(QStringLiteral("renamed.png"));
    QImage image(24, 18, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(original, "PNG"));

    AppSettings settings;
    QVERIFY(settings.createTag(tag));
    QVERIFY(settings.addTag(tag, {original}));
    QCOMPARE(settings.tagsForPath(original), QStringList {tag});
    QCOMPARE(settings.tagItemCount(tag), 1);

    // A slash nests. The parent's paths and count include the child's, the
    // names come back in tree order, and creating a deep tag fills in the
    // levels above it. Deleting a parent takes its children with it.
    const QString child = tag + QStringLiteral("/ Japan ");
    const QString other = dir.filePath(QStringLiteral("other.png"));
    QVERIFY(image.save(other, "PNG"));
    QVERIFY(settings.createTag(child));
    const QString childName = tag + QStringLiteral("/Japan");
    QVERIFY(settings.tagNames().contains(childName));
    QVERIFY(!settings.createTag(QStringLiteral(" / / ")));
    QVERIFY(settings.addTag(childName, {other}));
    QCOMPARE(settings.tagPaths(childName), QStringList {other});
    QCOMPARE(settings.tagPaths(tag), QStringList({original, other}));
    QCOMPARE(settings.tagItemCount(tag), 2);
    QVERIFY(settings.tagNames().indexOf(tag) < settings.tagNames().indexOf(childName));
    const QString deep = QStringLiteral("Auto ") + suffix + QStringLiteral("/Level/Leaf");
    QVERIFY(settings.createTag(deep));
    QVERIFY(settings.tagNames().contains(QStringLiteral("Auto ") + suffix));
    QVERIFY(settings.tagNames().contains(QStringLiteral("Auto ") + suffix + QStringLiteral("/Level")));
    settings.deleteTag(QStringLiteral("Auto ") + suffix);
    QVERIFY(!settings.tagNames().contains(deep));

    QVariantMap view;
    view.insert(QStringLiteral("kind"), CaptureRecord::Picture);
    view.insert(QStringLiteral("favorites"), true);
    view.insert(QStringLiteral("tag"), tag);
    QVERIFY(settings.saveSmartCollection(collection, view));

    AppSettings restored;
    QCOMPARE(restored.tagPaths(tag), QStringList({original, other}));
    QCOMPARE(restored.smartCollection(collection), view);
    QVERIFY(QFile::rename(original, renamed));
    restored.relocatePath(original, renamed);
    QCOMPARE(restored.tagPaths(tag), QStringList({renamed, other}));
    QCOMPARE(restored.tagsForPath(renamed), QStringList {tag});

    AppSettings finalRestore;
    QCOMPARE(finalRestore.tagPaths(tag), QStringList({renamed, other}));
    QCOMPARE(finalRestore.smartCollection(collection), view);
    finalRestore.deleteTag(tag);
    QVERIFY(!finalRestore.tagNames().contains(childName));
    finalRestore.deleteSmartCollection(collection);
  }

  void renamingCollectionsMovesMembershipAndNests() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString suffix = QString::number(QRandomGenerator::global()->generate());
    const QString album = QStringLiteral("Album ") + suffix;
    const QString albumTarget = QStringLiteral("Album renamed ") + suffix;
    const QString otherAlbum = QStringLiteral("Other ") + suffix;

    const QString file = dir.filePath(QStringLiteral("member.png"));
    QImage(10, 10, QImage::Format_RGB32).save(file);
    const QString other = dir.filePath(QStringLiteral("other.png"));
    QImage(10, 10, QImage::Format_RGB32).save(other);

    AppSettings settings;
    settings.deleteAlbum(album);
    settings.deleteAlbum(albumTarget);
    settings.deleteAlbum(otherAlbum);
    QVERIFY(settings.createAlbum(album));
    QVERIFY(settings.addToAlbum(album, {file}));

    // Renaming keeps the membership and drops the old key.
    QVERIFY(settings.renameAlbum(album, albumTarget));
    QVERIFY(!settings.albumNames().contains(album));
    QCOMPARE(settings.albumPaths(albumTarget), QStringList {file});

    // A clash is refused, and the existing album is left whole.
    QVERIFY(settings.createAlbum(otherAlbum));
    QVERIFY(!settings.renameAlbum(albumTarget, otherAlbum));
    QCOMPARE(settings.albumPaths(albumTarget), QStringList {file});
    QCOMPARE(settings.albumPaths(otherAlbum), QStringList {});

    // Tags: renaming a parent renames every nested tag and keeps membership.
    const QString tag = QStringLiteral("Travel ") + suffix;
    const QString child = tag + QStringLiteral("/Japan");
    const QString tagTarget = QStringLiteral("Trips ") + suffix;
    settings.deleteTag(tag);
    settings.deleteTag(tagTarget);
    QVERIFY(settings.createTag(child));
    QVERIFY(settings.addTag(tag, {file}));
    QVERIFY(settings.addTag(child, {other}));
    QCOMPARE(settings.tagPaths(tag), QStringList({file, other}));

    // Saved views hold the tag by name, so a rename must follow them too.
    const QString view = QStringLiteral("View ") + suffix;
    QVariantMap viewMap;
    viewMap.insert(QStringLiteral("tag"), tag);
    QVERIFY(settings.saveSmartCollection(view, viewMap));
    QVariantMap childView;
    childView.insert(QStringLiteral("tag"), child);
    QVERIFY(settings.saveSmartCollection(view + QStringLiteral("-child"), childView));

    QVERIFY(settings.renameTag(tag, tagTarget));
    QVERIFY(!settings.tagNames().contains(tag));
    QVERIFY(!settings.tagNames().contains(child));
    QVERIFY(settings.tagNames().contains(tagTarget));
    QVERIFY(settings.tagNames().contains(tagTarget + QStringLiteral("/Japan")));
    QCOMPARE(settings.tagPaths(tagTarget), QStringList({file, other}));
    QCOMPARE(settings.tagPaths(tagTarget + QStringLiteral("/Japan")), QStringList {other});
    QCOMPARE(settings.tagsForPath(file), QStringList {tagTarget});
    QCOMPARE(settings.smartCollection(view).value(QStringLiteral("tag")).toString(), tagTarget);
    QCOMPARE(settings.smartCollection(view + QStringLiteral("-child"))
                 .value(QStringLiteral("tag"))
                 .toString(),
             tagTarget + QStringLiteral("/Japan"));

    // Renaming into its own subtree, or onto an existing tag, is refused.
    QVERIFY(!settings.renameTag(tagTarget, tagTarget + QStringLiteral("/Nested")));
    const QString existing = QStringLiteral("Existing ") + suffix;
    settings.deleteTag(existing);
    QVERIFY(settings.createTag(existing));
    QVERIFY(!settings.renameTag(tagTarget, existing));
    QCOMPARE(settings.tagPaths(tagTarget), QStringList({file, other}));

    // Both renames survive a reload.
    AppSettings restored;
    QCOMPARE(restored.albumPaths(albumTarget), QStringList {file});
    QCOMPARE(restored.tagPaths(tagTarget + QStringLiteral("/Japan")), QStringList {other});

    restored.deleteAlbum(albumTarget);
    restored.deleteAlbum(otherAlbum);
    restored.deleteTag(tagTarget);
    restored.deleteTag(existing);
    restored.deleteSmartCollection(view);
    restored.deleteSmartCollection(view + QStringLiteral("-child"));
  }

  void organizationBackupRoundTripsAndRejectsBadFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString suffix = QString::number(QRandomGenerator::global()->generate());
    const QString album = QStringLiteral("Backup album ") + suffix;
    const QString tag = QStringLiteral("Backup tag ") + suffix;
    const QString child = tag + QStringLiteral("/Japan");
    const QString collection = QStringLiteral("Backup view ") + suffix;
    const QString file = dir.filePath(QStringLiteral("a.png"));
    QImage(10, 10, QImage::Format_RGB32).save(file);

    AppSettings settings;
    settings.deleteAlbum(album);
    settings.deleteTag(tag);
    settings.deleteSmartCollection(collection);
    QVERIFY(settings.createAlbum(album));
    QVERIFY(settings.addToAlbum(album, {file}));
    QVERIFY(settings.createTag(child));
    QVERIFY(settings.addTag(tag, {file}));
    settings.setFavorite({file}, true);
    settings.setRating({file}, 4);
    settings.setCaption(file, QStringLiteral("A caption"));
    QVariantMap view;
    view.insert(QStringLiteral("favorites"), true);
    view.insert(QStringLiteral("tag"), tag);
    QVERIFY(settings.saveSmartCollection(collection, view));

    const QString backup = dir.filePath(QStringLiteral("backup.json"));
    const QVariantMap exported = settings.exportOrganization(backup);
    QVERIFY2(exported.value(QStringLiteral("ok")).toBool(),
             qPrintable(exported.value(QStringLiteral("message")).toString()));
    QVERIFY(QFileInfo::exists(backup));

    // A profile with none of it gets it all back from the file.
    AppSettings restored;
    restored.deleteAlbum(album);
    restored.deleteTag(tag);
    restored.deleteSmartCollection(collection);
    restored.setFavorite({file}, false);
    restored.setRating({file}, 0);
    restored.setCaption(file, QString());
    QVERIFY(!restored.albumNames().contains(album));
    QVERIFY(!restored.tagNames().contains(child));

    const QVariantMap imported = restored.importOrganization(backup);
    QVERIFY2(imported.value(QStringLiteral("ok")).toBool(),
             qPrintable(imported.value(QStringLiteral("message")).toString()));
    QCOMPARE(restored.albumPaths(album), QStringList {file});
    QCOMPARE(restored.tagPaths(tag), QStringList {file});
    QVERIFY(restored.isFavorite(file));
    QCOMPARE(restored.rating(file), 4);
    QCOMPARE(restored.caption(file), QStringLiteral("A caption"));
    QCOMPARE(restored.smartCollection(collection), view);

    // A malformed file is refused and changes nothing.
    const QString broken = dir.filePath(QStringLiteral("broken.json"));
    {
      QFile handle(broken);
      QVERIFY(handle.open(QIODevice::WriteOnly));
      handle.write("{not json");
    }
    QVERIFY(!restored.importOrganization(broken).value(QStringLiteral("ok")).toBool());
    QCOMPARE(restored.albumPaths(album), QStringList {file});
    QVERIFY(restored.isFavorite(file));

    // Valid JSON that is not one of ours is refused as well.
    const QString foreign = dir.filePath(QStringLiteral("foreign.json"));
    {
      QFile handle(foreign);
      QVERIFY(handle.open(QIODevice::WriteOnly));
      handle.write("{\"version\":1}");
    }
    QVERIFY(!restored.importOrganization(foreign).value(QStringLiteral("ok")).toBool());
    QCOMPARE(restored.albumPaths(album), QStringList {file});

    // One of ours but missing sections: refused rather than read as empty
    // collections, which would wipe the profile and still call it success.
    const QString incomplete = dir.filePath(QStringLiteral("incomplete.json"));
    {
      QFile handle(incomplete);
      QVERIFY(handle.open(QIODevice::WriteOnly));
      handle.write("{\"format\":\"omaroll.organization\",\"version\":1,\"albums\":{}}");
    }
    QVERIFY(!restored.importOrganization(incomplete).value(QStringLiteral("ok")).toBool());
    QCOMPARE(restored.albumPaths(album), QStringList {file});
    QVERIFY(restored.isFavorite(file));

    restored.deleteAlbum(album);
    restored.deleteTag(tag);
    restored.deleteSmartCollection(collection);
    restored.setFavorite({file}, false);
    restored.setRating({file}, 0);
    restored.setCaption(file, QString());
  }

  void importedOrganizationNormalizesNamesAndFillsAncestors() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("member.png"));
    QImage(10, 10, QImage::Format_RGB32).save(file);
    const QString backup = dir.filePath(QStringLiteral("odd.json"));
    {
      QFile handle(backup);
      QVERIFY(handle.open(QIODevice::WriteOnly));
      const QByteArray payload =
          QByteArrayLiteral("{\"format\":\"omaroll.organization\",\"version\":1,")
          + QByteArrayLiteral("\"albums\":{\"Bad/Name\":[]},\"tags\":{\"Travel//Japan\":[{\"path\":\"")
          + file.toUtf8()
          + QByteArrayLiteral("\",\"bytes\":0}]},\"favorites\":[],\"hidden\":[],\"ratings\":{},")
          + QByteArrayLiteral("\"captions\":{},\"smartCollections\":{}}");
      handle.write(payload);
    }

    AppSettings settings;
    settings.deleteTag(QStringLiteral("Travel"));
    const QVariantMap result = settings.importOrganization(backup);
    QVERIFY2(result.value(QStringLiteral("ok")).toBool(),
             qPrintable(result.value(QStringLiteral("message")).toString()));
    // A slash cannot appear in an album name anywhere else, so it is dropped.
    QVERIFY(!settings.albumNames().contains(QStringLiteral("Bad/Name")));
    // The doubled slash collapses and the required parent tag is created.
    QVERIFY(settings.tagNames().contains(QStringLiteral("Travel")));
    QVERIFY(settings.tagNames().contains(QStringLiteral("Travel/Japan")));
    QCOMPARE(settings.tagPaths(QStringLiteral("Travel/Japan")), QStringList {file});
    settings.deleteTag(QStringLiteral("Travel"));
  }

  void organizationBackupRecoversMarksAfterAMove_data() {
    QTest::addColumn<bool>("copyMove");
    QTest::newRow("inode") << false;
    QTest::newRow("fingerprint") << true;
  }

  void organizationBackupRecoversMarksAfterAMove() {
    QFETCH(bool, copyMove);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString original = dir.filePath(QStringLiteral("before.png"));
    const QString moved = dir.filePath(QStringLiteral("after.png"));
    const QString backup = dir.filePath(QStringLiteral("backup.json"));
    QImage image(10, 10, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(original, "PNG"));
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("source-profile")));
    {
      AppSettings source;
      source.setFavorite({original}, true);
      source.setHidden({original}, true);
      source.setRating({original}, 5);
      source.setCaption(original, QStringLiteral("Moved before restore"));
      QVERIFY(source.exportOrganization(backup).value(QStringLiteral("ok")).toBool());
    }
    if (copyMove) {
      QVERIFY(QFile::copy(original, moved));
      QVERIFY(QFile::remove(original));
    } else {
      QVERIFY(QFile::rename(original, moved));
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("fresh-profile")));
    {
      AppSettings restored;
      QVERIFY(restored.markedPaths().isEmpty());
      QVERIFY(restored.importOrganization(backup).value(QStringLiteral("ok")).toBool());
    }
    {
      // Recovery after restarting must use identities persisted by import,
      // without ever seeing the file at its original path in this profile.
      AppSettings reloaded;
      const auto records = CaptureScanner::scan(
          {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
      QCOMPARE(records.size(), 1);
      reloaded.reconcileMarks(records);
      QVERIFY(!reloaded.markedPaths().contains(original));
      QCOMPARE(reloaded.markedPaths(), QStringList {moved});
      QVERIFY(reloaded.isFavorite(moved));
      QVERIFY(reloaded.isHidden(moved));
      QCOMPARE(reloaded.rating(moved), 5);
      QCOMPARE(reloaded.caption(moved), QStringLiteral("Moved before restore"));
    }
  }

  void legacyOrganizationBackupDoesNotReuseOldMarkIdentities() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString original = dir.filePath(QStringLiteral("before.png"));
    const QString moved = dir.filePath(QStringLiteral("after.png"));
    const QString backup = dir.filePath(QStringLiteral("legacy.json"));
    QImage image(10, 10, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(original, "PNG"));
    AppSettings settings;
    settings.setFavorite({original}, true);
    QVERIFY(settings.exportOrganization(backup).value(QStringLiteral("ok")).toBool());
    QFile file(backup);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject legacy = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    legacy.remove(QStringLiteral("markIdentities"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray payload = QJsonDocument(legacy).toJson();
    QCOMPARE(file.write(payload), qint64(payload.size()));
    file.close();
    QVERIFY(QFile::rename(original, moved));
    QVERIFY(settings.importOrganization(backup).value(QStringLiteral("ok")).toBool());
    auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    settings.reconcileMarks(records);
    QVERIFY(settings.isFavorite(original));
    QVERIFY(!settings.isFavorite(moved));

    // Legacy marks can still acquire an identity when their file is present,
    // then follow subsequent external renames as before.
    QVERIFY(QFile::rename(moved, original));
    records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    settings.reconcileMarks(records);
    QVERIFY(QFile::rename(original, moved));
    records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    settings.reconcileMarks(records);
    QVERIFY(!settings.isFavorite(original));
    QVERIFY(settings.isFavorite(moved));
  }

  void organizationBackupAcceptsLegacyMarkIdentities_data() {
    QTest::addColumn<bool>("knownBytes");
    QTest::newRow("empty-fingerprint") << true;
    QTest::newRow("unknown-size") << false;
  }

  void organizationBackupAcceptsLegacyMarkIdentities() {
    QFETCH(bool, knownBytes);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString path = dir.filePath(QStringLiteral("unavailable.png"));
    const QString backup = dir.filePath(QStringLiteral("backup.json"));
    {
      QSettings stored(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("omaroll"), QStringLiteral("omaroll"));
      stored.setValue(QStringLiteral("library/favorites"), QStringList{path});
      stored.setValue(QStringLiteral("library/markIdentities"), QVariantMap{
          {path, QVariantMap{{QStringLiteral("bytes"), knownBytes ? 10 : -1},
                            {QStringLiteral("modified"), 1000},
                            {QStringLiteral("fingerprint"), QByteArray()},
                            {QStringLiteral("device"), QStringLiteral("1")},
                            {QStringLiteral("inode"), QStringLiteral("2")}}}});
      stored.sync();
    }
    AppSettings settings;
    QVERIFY(settings.isFavorite(path));
    QVERIFY(settings.exportOrganization(backup).value(QStringLiteral("ok")).toBool());
    QFile file(backup);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto identities = QJsonDocument::fromJson(file.readAll()).object()
                                .value(QStringLiteral("markIdentities")).toObject();
    QCOMPARE(identities.contains(path), knownBytes);
    if (knownBytes) {
      QCOMPARE(identities.value(path).toObject().value(QStringLiteral("fingerprint")).toString(),
               QString());
    }
    settings.setFavorite({path}, false);
    QVERIFY(settings.importOrganization(backup).value(QStringLiteral("ok")).toBool());
    QVERIFY(settings.isFavorite(path));
    AppSettings restored;
    QVERIFY(restored.isFavorite(path));
  }

  void invalidBackupMarkIdentitiesLeaveOrganizationUntouched() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString path = dir.filePath(QStringLiteral("marked.png"));
    const QString backup = dir.filePath(QStringLiteral("backup.json"));
    const QString broken = dir.filePath(QStringLiteral("broken.json"));
    QImage image(10, 10, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(path, "PNG"));
    AppSettings settings;
    settings.setFavorite({path}, true);
    settings.setHidden({path}, true);
    settings.setRating({path}, 4);
    settings.setCaption(path, QStringLiteral("Unchanged"));
    QVERIFY(settings.createAlbum(QStringLiteral("Keep album")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Keep album"), {path}));
    QVERIFY(settings.createTag(QStringLiteral("Keep tag")));
    QVERIFY(settings.addTag(QStringLiteral("Keep tag"), {path}));
    QVERIFY(settings.saveSmartCollection(QStringLiteral("Keep view"),
                                         {{QStringLiteral("favorites"), true}}));
    QVERIFY(settings.exportOrganization(backup).value(QStringLiteral("ok")).toBool());
    QFile file(backup);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject expected = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QJsonObject identity =
        expected.value(QStringLiteral("markIdentities")).toObject().value(path).toObject();
    QVERIFY(!identity.isEmpty());

    // Use otherwise valid replacement organization, so accepting any damaged
    // identity would visibly wipe the current profile.
    QJsonObject replacement = expected;
    replacement.insert(QStringLiteral("albums"), QJsonObject());
    replacement.insert(QStringLiteral("tags"), QJsonObject());
    replacement.insert(QStringLiteral("hidden"), QJsonArray());
    replacement.insert(QStringLiteral("ratings"), QJsonObject());
    replacement.insert(QStringLiteral("captions"), QJsonObject());
    replacement.insert(QStringLiteral("smartCollections"), QJsonObject());
    QList<QJsonValue> malformed {QJsonValue(QJsonValue::Null), QJsonArray(),
                                QJsonObject {{path, false}},
                                QJsonObject {{QStringLiteral(""), identity}},
                                QJsonObject {{dir.filePath(QStringLiteral("unmarked.png")), identity}}};
    const QList<QPair<QString, QJsonValue>> invalidFields {
        {QStringLiteral("bytes"), QJsonValue(QJsonValue::Undefined)},
        {QStringLiteral("bytes"), -1},
        {QStringLiteral("bytes"), 1.5},
        {QStringLiteral("bytes"), 1e30},
        {QStringLiteral("bytes"), QStringLiteral("10")},
        {QStringLiteral("modified"), QJsonValue(QJsonValue::Undefined)},
        {QStringLiteral("modified"), 0.5},
        {QStringLiteral("modified"), -1e30},
        {QStringLiteral("fingerprint"), QStringLiteral("not base64!")},
        {QStringLiteral("fingerprint"), QString::fromLatin1(QByteArray(31, 'x').toBase64())},
        {QStringLiteral("fingerprint"), 123},
        {QStringLiteral("device"), 1},
        {QStringLiteral("device"), QStringLiteral("-1")},
        {QStringLiteral("device"), QStringLiteral("18446744073709551616")},
        {QStringLiteral("inode"), QJsonValue(QJsonValue::Undefined)},
        {QStringLiteral("inode"), QStringLiteral("invalid")}};
    for (const auto& field : invalidFields) {
      QJsonObject row = identity;
      row.insert(field.first, field.second);
      malformed.append(QJsonObject {{path, row}});
    }

    QSignalSpy marksChanged(&settings, &AppSettings::marksChanged);
    QSignalSpy albumsChanged(&settings, &AppSettings::albumsChanged);
    QSignalSpy tagsChanged(&settings, &AppSettings::tagsChanged);
    QSignalSpy viewsChanged(&settings, &AppSettings::smartCollectionsChanged);
    QSettings stored(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("omaroll"), QStringLiteral("omaroll"));
    stored.sync();
    QVariantMap before;
    for (const QString& key : stored.allKeys()) {
      before.insert(key, stored.value(key));
    }
    expected.remove(QStringLiteral("exportedAt"));
    for (const QJsonValue& value : malformed) {
      replacement.insert(QStringLiteral("markIdentities"), value);
      QFile damaged(broken);
      QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
      const QByteArray payload = QJsonDocument(replacement).toJson();
      QCOMPARE(damaged.write(payload), qint64(payload.size()));
      damaged.close();
      QVERIFY(!settings.importOrganization(broken).value(QStringLiteral("ok")).toBool());
      QVERIFY(settings.exportOrganization(backup).value(QStringLiteral("ok")).toBool());
      QVERIFY(file.open(QIODevice::ReadOnly));
      QJsonObject after = QJsonDocument::fromJson(file.readAll()).object();
      file.close();
      after.remove(QStringLiteral("exportedAt"));
      QCOMPARE(after, expected);
      stored.sync();
      QVariantMap persisted;
      for (const QString& key : stored.allKeys()) {
        persisted.insert(key, stored.value(key));
      }
      QCOMPARE(persisted, before);
    }
    QCOMPARE(marksChanged.size(), 0);
    QCOMPARE(albumsChanged.size(), 0);
    QCOMPARE(tagsChanged.size(), 0);
    QCOMPARE(viewsChanged.size(), 0);
  }

  void externalRenameAtHashGrowthBoundaryKeepsUnrelatedMarks_data() {
    QTest::addColumn<int>("markCount");
    QTest::addColumn<bool>("markedDestination");
    QHash<QString, int> probe;
    probe.insert(QStringLiteral("first"), 1);
    const int capacity = static_cast<int>(probe.capacity());
    QTest::newRow("first-growth") << capacity << false;
    QTest::newRow("second-growth") << capacity * 2 << false;
    QTest::newRow("third-growth") << capacity * 4 << false;
    QTest::newRow("destination-collision") << capacity << true;
  }

  void externalRenameAtHashGrowthBoundaryKeepsUnrelatedMarks() {
    QFETCH(int, markCount);
    QFETCH(bool, markedDestination);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString original = dir.filePath(QStringLiteral("before.png"));
    const QString moved = dir.filePath(QStringLiteral("after.png"));
    QImage image(10, 10, QImage::Format_RGB32);
    image.fill(Qt::yellow);
    QVERIFY(image.save(original, "PNG"));
    {
      AppSettings source;
      source.setRating({original}, 3);
      source.setCaption(original, QStringLiteral("Source caption"));
    }
    QStringList ratings {QStringLiteral("3:") + original};
    QVariantMap captions {{original, QStringLiteral("Source caption")}};
    if (markedDestination) {
      ratings.append(QStringLiteral("5:") + moved);
      captions.insert(moved, QStringLiteral("Destination caption"));
    }
    QStringList unrelated;
    while (ratings.size() < markCount) {
      const QString path = dir.filePath(QStringLiteral("unrelated-%1.png").arg(ratings.size()));
      ratings.append(QStringLiteral("2:") + path);
      captions.insert(path, QStringLiteral("Caption for ") + path);
      unrelated.append(path);
    }
    {
      // Fill both hashes exactly to Qt's growth boundary without undo snapshots
      // or pre-reservation masking insertion's iterator invalidation.
      QSettings stored(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("omaroll"), QStringLiteral("omaroll"));
      stored.setValue(QStringLiteral("library/ratings"), ratings);
      stored.setValue(QStringLiteral("library/captions"), captions);
      stored.sync();
    }
    {
      AppSettings settings;
      QCOMPARE(settings.ratedCount(), markCount);
      QVERIFY(QFile::rename(original, moved));
      const auto records = CaptureScanner::scan(
          {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
      QCOMPARE(records.size(), 1);
      settings.reconcileMarks(records);
      QCOMPARE(settings.rating(original), 0);
      QVERIFY(settings.caption(original).isEmpty());
      QCOMPARE(settings.rating(moved), markedDestination ? 5 : 3);
      QCOMPARE(settings.caption(moved), markedDestination ? QStringLiteral("Destination caption")
                                                          : QStringLiteral("Source caption"));
      QCOMPARE(settings.ratedCount(), markedDestination ? markCount - 1 : markCount);
      for (const QString& path : unrelated) {
        QCOMPARE(settings.rating(path), 2);
        QCOMPARE(settings.caption(path), QStringLiteral("Caption for ") + path);
      }
    }
    AppSettings reloaded;
    QCOMPARE(reloaded.rating(original), 0);
    QVERIFY(reloaded.caption(original).isEmpty());
    QCOMPARE(reloaded.rating(moved), markedDestination ? 5 : 3);
    QCOMPARE(reloaded.caption(moved), markedDestination ? QStringLiteral("Destination caption")
                                                      : QStringLiteral("Source caption"));
    QCOMPARE(reloaded.ratedCount(), markedDestination ? markCount - 1 : markCount);
    for (const QString& path : unrelated) {
      QCOMPARE(reloaded.rating(path), 2);
      QCOMPARE(reloaded.caption(path), QStringLiteral("Caption for ") + path);
    }
  }

  void marksFollowAFileMovedOutsideOmaroll() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString original = dir.filePath(QStringLiteral("moved.png"));
    QImage image(40, 30, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(original, "PNG"));

    AppSettings settings;
    settings.setFavorite({original}, true);
    settings.setRating({original}, 5);
    settings.setCaption(original, QStringLiteral("Keep me"));
    settings.setHidden({original}, true);
    QVERIFY(settings.isFavorite(original));

    // Renamed outside the app. The inode is unchanged, so the identity match
    // finds it and every mark follows.
    const QString renamed = dir.filePath(QStringLiteral("moved-renamed.png"));
    QVERIFY(QFile::rename(original, renamed));
    CaptureRecord renamedRecord;
    renamedRecord.path = renamed;
    renamedRecord.bytes = QFileInfo(renamed).size();
    renamedRecord.modified = QFileInfo(renamed).lastModified().toMSecsSinceEpoch();
    settings.reconcileMarks({renamedRecord});

    QVERIFY(!settings.isFavorite(original));
    QVERIFY(settings.isFavorite(renamed));
    QVERIFY(settings.isHidden(renamed));
    QCOMPARE(settings.rating(renamed), 5);
    QCOMPARE(settings.caption(renamed), QStringLiteral("Keep me"));

    // A copy to a new inode with the original removed is the cross-filesystem
    // move: same size, new inode, so the content fingerprint has to match.
    const QString copied = dir.filePath(QStringLiteral("moved-copied.png"));
    QVERIFY(QFile::copy(renamed, copied));
    QVERIFY(QFile::remove(renamed));
    CaptureRecord copiedRecord;
    copiedRecord.path = copied;
    copiedRecord.bytes = QFileInfo(copied).size();
    copiedRecord.modified = QFileInfo(copied).lastModified().toMSecsSinceEpoch();
    settings.reconcileMarks({copiedRecord});

    QVERIFY(settings.isFavorite(copied));
    QCOMPARE(settings.rating(copied), 5);
    QCOMPARE(settings.caption(copied), QStringLiteral("Keep me"));

    // The recovery survives a reload.
    AppSettings reloaded;
    QVERIFY(reloaded.isFavorite(copied));
    QVERIFY(reloaded.isHidden(copied));
    QCOMPARE(reloaded.rating(copied), 5);

    // An equal-sized but different file must not steal the mark. Two
    // uncompressed BMPs of the same dimensions are byte-for-byte the same
    // length, so only the content fingerprint can tell them apart.
    QImage other(40, 30, QImage::Format_RGB32);
    other.fill(Qt::magenta);
    const QString bmpA = dir.filePath(QStringLiteral("same-size-a.bmp"));
    const QString bmpB = dir.filePath(QStringLiteral("same-size-b.bmp"));
    QVERIFY(QImage(40, 30, QImage::Format_RGB32).save(bmpA, "BMP"));
    QVERIFY(other.save(bmpB, "BMP"));
    QCOMPARE(QFileInfo(bmpA).size(), QFileInfo(bmpB).size());
    settings.setFavorite({bmpA}, true);
    QVERIFY(QFile::remove(bmpA));
    CaptureRecord decoyRecord;
    decoyRecord.path = bmpB;
    decoyRecord.bytes = QFileInfo(bmpB).size();
    decoyRecord.modified = QFileInfo(bmpB).lastModified().toMSecsSinceEpoch();
    settings.reconcileMarks({decoyRecord});
    QVERIFY(!settings.isFavorite(bmpB));
    QVERIFY(settings.isFavorite(copied));

    settings.setFavorite({copied}, false);
    settings.setRating({copied}, 0);
    settings.setCaption(copied, QString());
    settings.setHidden({copied}, false);
    settings.setFavorite({bmpA, bmpB}, false);
  }

  void reusedInodeDoesNotStealMarks_data() {
    QTest::addColumn<bool>("knownFingerprint");
    QTest::newRow("known-content") << true;
    QTest::newRow("legacy-identity") << false;
  }

  void reusedInodeDoesNotStealMarks() {
    QFETCH(bool, knownFingerprint);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString original = dir.filePath(QStringLiteral("original.bmp"));
    const QString unrelated = dir.filePath(QStringLiteral("unrelated.bmp"));
    for (const QString& path : {original, unrelated}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(QByteArray(4096, path == original ? 'a' : 'b')), qint64(4096));
    }
    const auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 2);
    CaptureRecord reused;
    for (const auto& record : records) {
      if (record.path == original) reused = record;
    }
    QVERIFY(reused.inode != 0);
    reused.path = unrelated; // Model an equal-sized file reusing the remembered inode.
    {
      AppSettings settings;
      settings.setFavorite({original}, true);
      settings.setHidden({original}, true);
      settings.setRating({original}, 4);
      settings.setCaption(original, QStringLiteral("Original caption"));
    }
    if (!knownFingerprint) {
      QSettings stored(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("omaroll"), QStringLiteral("omaroll"));
      QVariantMap identities = stored.value(QStringLiteral("library/markIdentities")).toMap();
      QVariantMap identity = identities.value(original).toMap();
      identity.remove(QStringLiteral("fingerprint"));
      identities.insert(original, identity);
      stored.setValue(QStringLiteral("library/markIdentities"), identities);
      stored.sync();
    }
    QVERIFY(QFile::remove(original));
    {
      AppSettings settings;
      // Retain the unavailable mark through a scan before the inode reappears.
      settings.reconcileMarks({});
      settings.reconcileMarks({reused});
      settings.reconcileMarks({reused});
    }
    AppSettings reloaded;
    const QString marked = knownFingerprint ? original : unrelated;
    const QString unmarked = knownFingerprint ? unrelated : original;
    QVERIFY(reloaded.isFavorite(marked));
    QVERIFY(reloaded.isHidden(marked));
    QCOMPARE(reloaded.rating(marked), 4);
    QCOMPARE(reloaded.caption(marked), QStringLiteral("Original caption"));
    QVERIFY(!reloaded.isFavorite(unmarked));
    QVERIFY(!reloaded.isHidden(unmarked));
    QCOMPARE(reloaded.rating(unmarked), 0);
    QVERIFY(reloaded.caption(unmarked).isEmpty());
  }

  void retainedMarkFingerprintCacheTracksContentAndMarkIdentity() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString first = dir.filePath(QStringLiteral("first.bmp"));
    const QString second = dir.filePath(QStringLiteral("second.bmp"));
    const QString candidate = dir.filePath(QStringLiteral("candidate.bmp"));
    for (const QString& path : {first, second}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(QByteArray(4096, path == first ? 'a' : 'b')), qint64(4096));
    }
    AppSettings settings;
    settings.setFavorite({first}, true);
    settings.setHidden({second}, true);
    QVERIFY(QFile::copy(second, candidate));
    QVERIFY(QFile::remove(first));
    QVERIFY(QFile::remove(second));
    auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    settings.reconcileMarks(records);
    // A mismatch against one mark must not suppress a match against another.
    QVERIFY(settings.isFavorite(first));
    QVERIFY(!settings.isFavorite(candidate));
    QVERIFY(settings.isHidden(candidate));
    settings.reconcileMarks(records);
    QVERIFY(settings.isFavorite(first));

    struct stat before {};
    QVERIFY(::stat(QFile::encodeName(candidate).constData(), &before) == 0);
    QTest::qWait(50);
    QFile changed(candidate);
    QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(changed.write(QByteArray(4096, 'a')), qint64(4096));
    changed.close();
    const timespec times[] = {before.st_atim, before.st_mtim};
    QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(candidate).constData(), times, 0) == 0);
    // Same path, size, inode and exact mtime: ctime must invalidate the hash.
    records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    settings.reconcileMarks(records);
    QVERIFY(!settings.isFavorite(first));
    QVERIFY(settings.isFavorite(candidate));
    QVERIFY(settings.isHidden(candidate));
  }

  void retainedMarksLeaveAmbiguousOrExcessiveFingerprintCandidatesUnresolved_data() {
    QTest::addColumn<int>("candidateCount");
    QTest::newRow("ambiguous") << 2;
    QTest::newRow("bounded-fingerprinting") << 129;
  }

  void retainedMarksLeaveAmbiguousOrExcessiveFingerprintCandidatesUnresolved() {
    QFETCH(int, candidateCount);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    const QString original = dir.filePath(QStringLiteral("original.bmp"));
    QFile file(original);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(4096, 'a')), qint64(4096));
    file.close();
    AppSettings settings;
    settings.setFavorite({original}, true);
    for (int index = 0; index < candidateCount; ++index) {
      const QString path = dir.filePath(QStringLiteral("candidate-%1.bmp").arg(index));
      QVERIFY(QFile::copy(original, path));
      QFile copy(path);
      QVERIFY(copy.open(QIODevice::ReadWrite));
      QVERIFY(copy.setFileTime(QDateTime::fromMSecsSinceEpoch(1000), QFileDevice::FileModificationTime));
    }
    QVERIFY(QFile::remove(original));
    const auto records = CaptureScanner::scan(
        {{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), candidateCount);
    settings.reconcileMarks(records);
    settings.reconcileMarks(records);
    QCOMPARE(settings.markedPaths(), QStringList{original});
  }

  void undoRestoresThePreviousMarks() {
    const QString path = QStringLiteral("/tmp/omaroll-undo-%1.png")
                             .arg(QRandomGenerator::global()->generate());
    AppSettings settings;
    settings.setFavorite({path}, false);
    settings.setRating({path}, 0);
    settings.setCaption(path, QString());
    settings.setHidden({path}, false);
    while (settings.canUndo()) {
      settings.undo();
    }
    QVERIFY(!settings.canUndo());

    settings.setRating({path}, 4);
    settings.setFavorite({path}, true);
    settings.toggleHidden(path);
    settings.setCaption(path, QStringLiteral("note"));
    QCOMPARE(settings.rating(path), 4);
    QVERIFY(settings.isFavorite(path));
    QVERIFY(settings.isHidden(path));
    QCOMPARE(settings.caption(path), QStringLiteral("note"));

    // Undo newest first, one action at a time.
    settings.undo();
    QCOMPARE(settings.caption(path), QString());
    settings.undo();
    QVERIFY(!settings.isHidden(path));
    settings.undo();
    QVERIFY(!settings.isFavorite(path));
    settings.undo();
    QCOMPARE(settings.rating(path), 0);
    QVERIFY(!settings.canUndo());

    settings.setFavorite({path}, false);
    settings.setRating({path}, 0);
    settings.setCaption(path, QString());
    settings.setHidden({path}, false);
    while (settings.canUndo()) {
      settings.undo();
    }
  }

  void marksFollowAnExternalMoveThroughAScan() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const char* name : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                             "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }
    const QString original = dir.filePath(QStringLiteral("scan-moved.png"));
    QVERIFY(QImage(48, 32, QImage::Format_RGB32).save(original, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY_WITH_TIMEOUT(model.rowCount() >= 1, 5000);
    settings.setFavorite({original}, true);

    const QString moved = dir.filePath(QStringLiteral("scan-moved-again.png"));
    QVERIFY(QFile::rename(original, moved));
    model.refresh();
    // The scan's reconcile step, not the caller, has to carry the mark.
    QTRY_VERIFY_WITH_TIMEOUT(settings.isFavorite(moved), 5000);
    QVERIFY(!settings.isFavorite(original));
    settings.setFavorite({moved}, false);
  }

  void slideshowOptionsPersistAndClamp() {
    AppSettings settings;
    settings.setSlideshowIntervalSeconds(8);
    settings.setSlideshowShuffle(true);

    AppSettings reloaded;
    QCOMPARE(reloaded.slideshowIntervalSeconds(), 8);
    QVERIFY(reloaded.slideshowShuffle());

    reloaded.setSlideshowIntervalSeconds(0);
    QCOMPARE(reloaded.slideshowIntervalSeconds(), 2);
    reloaded.setSlideshowIntervalSeconds(1000);
    QCOMPARE(reloaded.slideshowIntervalSeconds(), 20);

    reloaded.setSlideshowIntervalSeconds(4);
    reloaded.setSlideshowShuffle(false);
  }

  void videoPlaybackPreferencesAndResumePersist() {
    const QString movie = QStringLiteral("/tmp/omaroll-resume-movie-%1.mp4")
                              .arg(QRandomGenerator::global()->generate());
    AppSettings settings;
    settings.setVideoVolume(0.35);
    settings.setVideoMuted(true);
    settings.setVideoPosition(movie, 125000);
    QCOMPARE(settings.videoPosition(movie), 125000);

    // A spot at the very start is not worth offering; storing one below the
    // threshold clears an existing marker.
    const QString clip = movie + QStringLiteral(".short");
    settings.setVideoPosition(clip, 1200);
    QCOMPARE(settings.videoPosition(clip), 0);
    settings.setVideoPosition(movie, 900);
    QCOMPARE(settings.videoPosition(movie), 0);
    settings.setVideoPosition(movie, 42000);

    AppSettings reloaded;
    QVERIFY(qAbs(reloaded.videoVolume() - 0.35) < 0.001);
    QVERIFY(reloaded.videoMuted());
    QCOMPARE(reloaded.videoPosition(movie), 42000);

    reloaded.clearVideoPosition(movie);
    QCOMPARE(reloaded.videoPosition(movie), 0);

    reloaded.setVideoVolume(0.8);
    reloaded.setVideoMuted(false);
    reloaded.setVideoPosition(movie, 0);
  }

  void externalSubtitlesParseAndAnswerByPosition() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString video = dir.filePath(QStringLiteral("clip.mp4"));
    {
      QFile file(video);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("not really a video");
    }
    const QString srt = dir.filePath(QStringLiteral("clip.srt"));
    {
      QFile file(srt);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(
          "1\n00:00:01,000 --> 00:00:03,500\nHello <i>world</i>\n\n"
          "2\n00:00:04,000 --> 00:00:05,000\nSecond line\nsecond row\n\n");
    }
    const QString vtt = dir.filePath(QStringLiteral("clip.en.vtt"));
    {
      QFile file(vtt);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("WEBVTT\n\nNOTE ignore me\n\n00:01.000 --> 00:02.000\nVTT cue\n\n");
    }

    SubtitleIndex subtitles;
    const QStringList files = subtitles.files(video);
    QCOMPARE(files.size(), 2);
    QVERIFY(files.contains(srt));
    QVERIFY(files.contains(vtt));

    QCOMPARE(subtitles.label(srt), QStringLiteral("Subtitles"));
    QCOMPARE(subtitles.label(vtt), QStringLiteral("English"));
    QCOMPARE(subtitles.cueCount(srt), 2);

    // Markup is stripped, cues answer at their boundaries, and the gap between
    // cues is silent.
    QCOMPARE(subtitles.textAt(srt, 2000), QStringLiteral("Hello world"));
    QCOMPARE(subtitles.textAt(srt, 3500), QStringLiteral("Hello world"));
    QCOMPARE(subtitles.textAt(srt, 3600), QString());
    QCOMPARE(subtitles.textAt(srt, 4200), QStringLiteral("Second line\nsecond row"));

    // A WebVTT cue with no hours, and its note skipped.
    QCOMPARE(subtitles.textAt(vtt, 1500), QStringLiteral("VTT cue"));
    QCOMPARE(subtitles.textAt(vtt, 2500), QString());
  }

  void externalSubtitlesIgnoreUnrelatedSidecars() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString video = dir.filePath(QStringLiteral("movie.mp4"));
    {
      QFile file(video);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("x");
    }
    for (const char* name : {"movie2.srt", "other.srt", "movie.txt"}) {
      QFile file(dir.filePath(QLatin1String(name)));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("1\n00:00:00,000 --> 00:00:01,000\nx\n\n");
    }
    SubtitleIndex subtitles;
    QVERIFY(subtitles.files(video).isEmpty());
  }

  void pdfSearchMatchesAcrossPagesAndWhitespace() {
    const QString text = QStringLiteral("Invoice total: 42") + QChar(0x0C)
                         + QStringLiteral("Notes\nINVOICE\nnothing\nTOTAL") + QChar(0x0C)
                         + QStringLiteral("Unrelated") + QChar(0x0C);
    QCOMPARE(PdfSupport::findPages(text, QStringLiteral("invoice total")), QList<int>({1}));
    // A phrase split across a line break still matches.
    QCOMPARE(PdfSupport::findPages(text, QStringLiteral("Invoice\n   total")), QList<int>({1}));
    QCOMPARE(PdfSupport::findPages(text, QStringLiteral("invoice")), QList<int>({1, 2}));
    QCOMPARE(PdfSupport::findPages(text, QStringLiteral("unrelated")), QList<int>({3}));
    QVERIFY(PdfSupport::findPages(text, QStringLiteral("absent")).isEmpty());
    QVERIFY(PdfSupport::findPages(text, QString()).isEmpty());
  }

  void pdfPageTextCopyReportsAnOutcome() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("page.pdf"));
    {
      QPdfWriter writer(path);
      writer.setResolution(96);
      QPainter painter(&writer);
      QVERIFY(painter.isActive());
      painter.drawText(QPoint(100, 140), QStringLiteral("Hello Omaroll"));
      painter.end();
    }

    PdfInspector inspector;
    inspector.inspect(path);
    QTRY_COMPARE_WITH_TIMEOUT(inspector.pageCount(), 1, 8000);

    QSignalSpy copied(&inspector, &PdfInspector::textCopied);
    QSignalSpy failed(&inspector, &PdfInspector::textCopyFailed);
    inspector.copyPageText(1);
    // Whether the environment can extract the text or not, exactly one
    // outcome has to arrive; never both and never neither.
    QTRY_VERIFY_WITH_TIMEOUT(copied.size() + failed.size() >= 1, 8000);
    QCOMPARE(copied.size() + failed.size(), 1);

    // Without a document it refuses immediately.
    PdfInspector empty;
    QSignalSpy emptyFailed(&empty, &PdfInspector::textCopyFailed);
    empty.copyPageText(1);
    QCOMPARE(emptyFailed.size(), 1);
  }

  void pdfSelectionPicksTheWordsUnderTheArea() {
    const PdfSupport::PdfPageText page = twoLinePage();
    // Normalized areas, as the viewer sends them: the page is 600x800 points,
    // so the first line spans y 0.125 to 0.1375.
    QCOMPARE(PdfSupport::wordsTouched(page, QRectF(0.16, 0.12, 0.02, 0.03)), QList<int>({0}));
    QCOMPARE(PdfSupport::wordsTouched(page, QRectF(0.0, 0.0, 1.0, 1.0)), QList<int>({0, 1, 2, 3}));
    // A drag that ends before the second line leaves it out.
    QCOMPARE(PdfSupport::wordsTouched(page, QRectF(0.0, 0.12, 1.0, 0.006)), QList<int>({0, 1}));
    // Nothing under the pointer, an empty area, and an area that only touches
    // a word's edge select nothing at all.
    QVERIFY(PdfSupport::wordsTouched(page, QRectF(0.8, 0.8, 0.1, 0.1)).isEmpty());
    QVERIFY(PdfSupport::wordsTouched(page, QRectF()).isEmpty());
    QVERIFY(PdfSupport::wordsTouched(page, QRectF(0.0, 0.125, 0.5, 0.0)).isEmpty());
    QVERIFY(PdfSupport::wordsTouched(page, QRectF(0.0, 0.125, 0.1666, 0.001)).isEmpty());

    // Reading order comes back as given, and out-of-range indexes are ignored
    // rather than read past the end.
    QCOMPARE(PdfSupport::wordsText(page, {0, 1, 2, 3}),
             QStringLiteral("Invoice total\nPaid today"));
    QCOMPARE(PdfSupport::wordsText(page, {2, 3}), QStringLiteral("Paid today"));
    QCOMPARE(PdfSupport::wordsText(page, {1}), QStringLiteral("total"));
    QCOMPARE(PdfSupport::wordsText(page, {1, 7}), QStringLiteral("total"));
    QVERIFY(PdfSupport::wordsText(page, {}).isEmpty());

    // One highlight per line, in normalized page coordinates.
    const QList<QRectF> lines = PdfSupport::wordLines(page, {0, 1, 2, 3});
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QRectF(100.0 / 600, 100.0 / 800, 104.0 / 600, 10.0 / 800));
    QCOMPARE(lines.at(1), QRectF(100.0 / 600, 120.0 / 800, 84.0 / 600, 10.0 / 800));
    QVERIFY(PdfSupport::wordLines(page, {}).isEmpty());
  }

  void pdfSelectionKeepsLinesApartWhenTheGapIsReal() {
    PdfSupport::PdfPageText page;
    page.pageSize = QSizeF(600, 800);
    const auto add = [&page](qreal y, qreal height) {
      PdfSupport::PdfWord word;
      word.box = QRectF(100, y, 50, height);
      word.text = QStringLiteral("x");
      page.words.append(word);
    };
    // A tall heading and a word whose centre sits inside it share a line; a
    // line a whole height below does not.
    add(100, 20);
    add(104, 10);
    add(140, 10);
    QCOMPARE(PdfSupport::wordsText(page, {0, 1, 2}), QStringLiteral("x x\nx"));

    // Two columns keep their own lines. Words are joined in the document's
    // flow order, which is the order pdftotext emits and the order the file
    // itself reads, so a column-major page stays column-major here.
    PdfSupport::PdfPageText columns;
    columns.pageSize = QSizeF(600, 800);
    const auto addColumn = [&columns](qreal x, qreal y) {
      PdfSupport::PdfWord word;
      word.box = QRectF(x, y, 50, 10);
      word.text = QStringLiteral("w");
      columns.words.append(word);
    };
    addColumn(100, 100);
    addColumn(100, 120);
    addColumn(400, 100);
    QCOMPARE(PdfSupport::wordsText(columns, {0, 1, 2}), QStringLiteral("w\nw\nw"));
    // Two words on one visual line that are adjacent in flow order share it.
    QCOMPARE(PdfSupport::wordsText(columns, {2, 0, 1}), QStringLiteral("w w\nw"));
    QCOMPARE(PdfSupport::wordLines(columns, {2, 0, 1}).size(), 2);
  }

  void pdfSelectionReadsBboxOutput() {
    // The shape of what `pdftotext -bbox` writes, including the header, the
    // entities the reader has to turn back into text, and a second page that
    // must be left alone.
    const QByteArray xml =
        QStringLiteral(
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
            "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            "<head><title></title><meta name=\"Producer\" content=\"Skia/PDF m120\"/></head>\n"
            "<body><doc>\n"
            "  <page width=\"595.919980\" height=\"842.880000\">\n"
            "    <word xMin=\"143.027342\" yMin=\"53.028806\" xMax=\"198.536861\" "
            "yMax=\"68.110837\">INVOICE</word>\n"
            "    <word xMin=\"20.999999\" yMin=\"116.352534\" xMax=\"51.511228\" "
            "yMax=\"126.407221\">GitHub,</word>\n"
            "    <word xMin=\"54.011716\" yMin=\"116.352534\" xMax=\"68.518063\" "
            "yMax=\"126.407221\">Inc.</word>\n"
            "    <word xMin=\"20.999999\" yMin=\"126.852534\" xMax=\"46.521970\" "
            "yMax=\"136.907221\">Tom&amp;Jerry</word>\n"
            "  </page>\n"
            "  <page width=\"595.919980\" height=\"842.880000\">\n"
            "    <word xMin=\"1\" yMin=\"1\" xMax=\"2\" yMax=\"2\">later page</word>\n"
            "  </page>\n"
            "</doc></body></html>")
            .toUtf8();

    const PdfSupport::PdfPageText page = PdfSupport::parsePageWords(xml);
    QCOMPARE(page.pageSize, QSizeF(595.919980, 842.880000));
    QCOMPARE(page.words.size(), 4);
    QCOMPARE(page.words.at(0).text, QStringLiteral("INVOICE"));
    QCOMPARE(page.words.at(0).box, QRectF(143.027342, 53.028806, 198.536861 - 143.027342,
                                           68.110837 - 53.028806));
    QCOMPARE(page.words.at(3).text, QStringLiteral("Tom&Jerry"));
    // Only the first page of the input is read.
    for (const PdfSupport::PdfWord& word : page.words) {
      QVERIFY(word.text != QStringLiteral("later page"));
    }

    // The heading is its own line, and the address lines below it are two more.
    QCOMPARE(PdfSupport::wordsText(page, {0, 1, 2, 3}),
             QStringLiteral("INVOICE\nGitHub, Inc.\nTom&Jerry"));
    const QList<QRectF> lines = PdfSupport::wordLines(page, {0, 1, 2, 3});
    QCOMPARE(lines.size(), 3);
    // Normalized, so the highlight can be drawn at any zoom.
    QVERIFY(qAbs(lines.at(1).width() * page.pageSize.width() - (68.518063 - 20.999999)) < 1e-6);

    // A page that cannot be read is refused whole: a half-read page would place
    // every word wrongly.
    QVERIFY(PdfSupport::parsePageWords(QByteArray()).pageSize.isEmpty());
    QVERIFY(PdfSupport::parsePageWords("<html><body><doc>").pageSize.isEmpty());
    QVERIFY(PdfSupport::parsePageWords("<page width=\"0\" height=\"0\"/>").pageSize.isEmpty());
    QVERIFY(PdfSupport::parsePageWords("<page width=\"600\" height=\"800\"><word xMin=\"1\"")
                .pageSize.isEmpty());
    // A page with no words in it is a real answer: an image-only page.
    const PdfSupport::PdfPageText scanned =
        PdfSupport::parsePageWords("<page width=\"600\" height=\"800\"/>");
    QCOMPARE(scanned.pageSize, QSizeF(600, 800));
    QVERIFY(scanned.words.isEmpty());
    QVERIFY(PdfSupport::wordsText(scanned, {0}).isEmpty());
  }

  void pdfSelectionCopiesTheDraggedWords() {
    const QString page = QFINDTESTDATA("fixtures/pdf/text-page.pdf");
    // A missing fixture is a broken checkout, not a reason to report this
    // passing: the assertions below are the only coverage of the page words.
    QVERIFY(!page.isEmpty());
    if (!PdfSupport::textAvailable()) {
      QSKIP("pdftotext is not installed");
    }

    PdfInspector inspector;
    QSignalSpy changed(&inspector, &PdfInspector::selectionChanged);
    QSignalSpy failed(&inspector, &PdfInspector::selectionFailed);
    inspector.inspect(page);
    QTRY_VERIFY_WITH_TIMEOUT(!inspector.loading(), 8000);
    QCOMPARE(inspector.pageCount(), 1);
    QVERIFY(!inspector.hasSelection());

    // A drag over the page selects the words on it, which the first one on a
    // page has to wait for.
    inspector.updateSelection(1, 0.0, 0.0, 1.0, 1.0);
    QTRY_VERIFY_WITH_TIMEOUT(inspector.hasSelection(), 8000);
    QVERIFY(changed.size() >= 1);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(inspector.selectionPage(), 1);
    QVERIFY(inspector.selectionText().contains(QStringLiteral("Omaroll")));
    QVERIFY(inspector.selectionText().contains(QStringLiteral("Second")));

    const QVariantList rects = inspector.selectionRects();
    QCOMPARE(rects.size(), 2); // one highlight per line
    for (const QVariant& value : rects) {
      const QRectF box = value.toRectF();
      QVERIFY(box.width() > 0 && box.height() > 0);
      QVERIFY(box.left() >= 0 && box.top() >= 0 && box.right() <= 1 && box.bottom() <= 1);
    }
    const QString wholePage = inspector.selectionText();

    // The viewer clears the selection as a new press begins the next drag, so
    // the drag below arrives after a clear and still has to be answered from the
    // page's words rather than read again. It is strictly shorter, so it cannot
    // pass on a selection left over from the drag above.
    inspector.clearSelection();
    QVERIFY(!inspector.hasSelection());
    const QRectF firstLine = rects.first().toRectF();
    inspector.updateSelection(1, firstLine.left(), firstLine.top(), firstLine.right(),
                              firstLine.bottom());
    QVERIFY(inspector.hasSelection());
    QCOMPARE(failed.size(), 0);
    QVERIFY(inspector.selectionText().size() < wholePage.size());
    QVERIFY(wholePage.contains(inspector.selectionText()));
    QCOMPARE(inspector.selectionRects().size(), 1);

    // Copying reports one outcome, never none and never two, and the count is
    // the words that were selected rather than a number of its own.
    QSignalSpy copied(&inspector, &PdfInspector::selectionCopied);
    const int selectedWords =
        inspector.selectionText().split(QRegularExpression(QStringLiteral("\\s+")),
                                       Qt::SkipEmptyParts).size();
    QVERIFY(selectedWords >= 2);
    inspector.copySelection();
    QTRY_VERIFY_WITH_TIMEOUT(copied.size() + failed.size() >= 1, 8000);
    QCOMPARE(copied.size() + failed.size(), 1);
    if (copied.size() == 1) {
      QCOMPARE(copied.first().first().toInt(), selectedWords);
    }

    // A drag that touches no words says so and leaves nothing selected.
    failed.clear();
    copied.clear();
    inspector.updateSelection(1, 0.05, 0.90, 0.95, 0.99);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 8000);
    QVERIFY(!inspector.hasSelection());
    QVERIFY(inspector.selectionRects().isEmpty());

    // Clearing drops the words and the highlight; copying afterwards says
    // there is nothing to copy rather than putting an empty string on the
    // clipboard.
    inspector.updateSelection(1, 0.0, 0.0, 1.0, 1.0);
    QTRY_VERIFY_WITH_TIMEOUT(inspector.hasSelection(), 8000);
    inspector.clearSelection();
    QVERIFY(!inspector.hasSelection());
    QVERIFY(inspector.selectionRects().isEmpty());
    QCOMPARE(inspector.selectionPage(), 0);
    failed.clear();
    copied.clear();
    inspector.copySelection();
    QCOMPARE(failed.size(), 1);
    QCOMPARE(copied.size(), 0);

    // An empty drag is a click: it clears what is there and reports nothing.
    // The selection is made first, so this cannot pass on an empty state alone.
    inspector.updateSelection(1, 0.0, 0.0, 1.0, 1.0);
    QTRY_VERIFY_WITH_TIMEOUT(inspector.hasSelection(), 8000);
    failed.clear();
    inspector.updateSelection(1, 0.5, 0.5, 0.5, 0.5);
    QVERIFY(!inspector.hasSelection());
    QCOMPARE(failed.size(), 0);

    // A page outside the document is refused, and so is a document-less
    // inspector.
    failed.clear();
    inspector.updateSelection(2, 0.0, 0.0, 1.0, 1.0);
    QCOMPARE(failed.size(), 1);
    PdfInspector detached;
    QSignalSpy detachedFailed(&detached, &PdfInspector::selectionFailed);
    detached.updateSelection(1, 0.0, 0.0, 1.0, 1.0);
    QCOMPARE(detachedFailed.size(), 1);
  }

  void pdfSelectionCancelledReadIsNotAFailure() {
    const QString page = QFINDTESTDATA("fixtures/pdf/text-page.pdf");
    // A missing fixture is a broken checkout, not a reason to report this
    // passing: the assertions below are the only coverage of the page words.
    QVERIFY(!page.isEmpty());
    if (!PdfSupport::textAvailable()) {
      QSKIP("pdftotext is not installed");
    }

    PdfInspector inspector;
    QSignalSpy failed(&inspector, &PdfInspector::selectionFailed);
    inspector.inspect(page);
    QTRY_COMPARE_WITH_TIMEOUT(inspector.pageCount(), 1, 8000);

    // A drag that lands while the page's words are still being read, then a
    // clear: a press or a closed viewer does exactly this, and cancelling a read
    // is not something to report as a failure.
    inspector.updateSelection(1, 0.0, 0.0, 1.0, 1.0);
    inspector.clearSelection();
    QTest::qWait(500);
    QCOMPARE(failed.size(), 0);
    QVERIFY(!inspector.hasSelection());

    // The page can still be read afterwards.
    inspector.updateSelection(1, 0.0, 0.0, 1.0, 1.0);
    QTRY_VERIFY_WITH_TIMEOUT(inspector.hasSelection(), 8000);
    QCOMPARE(failed.size(), 0);
    QVERIFY(inspector.selectionText().contains(QStringLiteral("Omaroll")));
  }

  void pdfPageRenderTakesAnOpenDimension() {
    const QString page = QFINDTESTDATA("fixtures/pdf/text-page.pdf");
    // A missing fixture is a broken checkout, not a reason to report this
    // passing: the assertions below are the only coverage of the page words.
    QVERIFY(!page.isEmpty());
    if (!PdfSupport::available()) {
      QSKIP("Poppler is not installed");
    }

    // A page drawn in a column is asked for by width, and takes its own height.
    const QImage wide = PdfSupport::renderPage(page, 1, QSize(600, 0));
    QVERIFY(!wide.isNull());
    QCOMPARE(wide.width(), 600);
    QVERIFY(wide.height() > wide.width()); // the fixture page is portrait

    const QImage tall = PdfSupport::renderPage(page, 1, QSize(0, 500));
    QVERIFY(!tall.isNull());
    QCOMPARE(tall.height(), 500);
    QVERIFY(tall.width() < tall.height());

    // A request for a box still fits inside it.
    const QImage box = PdfSupport::renderPage(page, 1, QSize(400, 400));
    QVERIFY(!box.isNull());
    QVERIFY(box.width() <= 400 && box.height() <= 400);
    QCOMPARE(qMax(box.width(), box.height()), 400);
  }

  void addingOverAnUnavailableAlbumEntryReplacesIt() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("out.png"));
    QImage(32, 32, QImage::Format_RGB32).save(path);

    AppSettings settings;
    QVERIFY(settings.createAlbum(QStringLiteral("Replace")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Replace"), {path}));
    QVERIFY(QFile::remove(path));
    settings.reconcileAlbums({});
    QCOMPARE(settings.unavailableAlbumItemCount(QStringLiteral("Replace")), 1);

    // A different file lands at the same name and is added again.
    QImage(48, 48, QImage::Format_RGB32).save(path);
    QVERIFY(settings.addToAlbum(QStringLiteral("Replace"), {path}));
    QCOMPARE(settings.albumItemCount(QStringLiteral("Replace")), 1);
    QCOMPARE(settings.unavailableAlbumItemCount(QStringLiteral("Replace")), 0);
    settings.deleteAlbum(QStringLiteral("Replace"));
  }

  void themeIgnoresUnrelatedWritesBesideItsState() {
    QTemporaryDir state;
    QTemporaryDir config;
    QVERIFY(state.isValid());
    QVERIFY(config.isValid());
    const QString root = state.filePath(QStringLiteral("omarchy/current"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/theme")));
    QFile colors(root + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Text));
    colors.write("mode = \"dark\"\nbackground = \"#101820\"\nforeground = "
                 "\"#f0f0f0\"\n");
    colors.close();

    OmarchyTheme theme(state.path(), config.path());
    QSignalSpy changed(&theme, &OmarchyTheme::themeChanged);

    // What omarchy-shell does on every copy: an atomic write beside the theme.
    QFile history(state.filePath(QStringLiteral("omarchy/clipboard-history.json.tmp")));
    QVERIFY(history.open(QIODevice::WriteOnly));
    history.write("[]");
    history.close();
    QVERIFY(QFile::rename(history.fileName(),
                          state.filePath(QStringLiteral("omarchy/clipboard-history.json"))));
    QTest::qWait(400);
    QCOMPARE(changed.size(), 0);

    // A real change still lands.
    QTest::qWait(20);
    QVERIFY(colors.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    colors.write("mode = \"dark\"\nbackground = \"#202020\"\nforeground = "
                 "\"#f0f0f0\"\n");
    colors.close();
    QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, 1500);
    QCOMPARE(theme.background(), QColor(QStringLiteral("#202020")));
  }

  void classifiesOmarchyScreenshot() {
    CaptureRecord::Kind kind = CaptureRecord::Picture;
    QDateTime captured;
    QVERIFY(CaptureScanner::classifyByName(QStringLiteral("screenshot-2026-08-31_23-26-39.png"),
                                           kind, captured));
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
    auto shallow =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(shallow.size(), 1);
    QCOMPARE(shallow.first().fileName, QStringLiteral("top.png"));

    // Depth 2 reaches nested/ but not nested/deeper/, and never the skipped
    // directories at any depth.
    auto deeper =
        CaptureScanner::scan({{dir.path(), 2, CaptureRecord::Picture, CaptureRecord::Video}});
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

  void scanRecursesPastAShallowerOverlappingRoot() {
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
    write(QStringLiteral("albums/holiday.png"));

    // The default layout: the depth-1 screenshot root and the recursive
    // Pictures root are the same directory. The shallow one must not stop the
    // deep one from seeing the subfolder.
    const auto records = CaptureScanner::scan({
        {dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video},
        {dir.path(), 3, CaptureRecord::Picture, CaptureRecord::Video},
    });
    QCOMPARE(records.size(), 2);
  }

  void downloadsKeepTheirMedium() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(dir.filePath(QStringLiteral("meme.png")), "PNG"));
    // The scanner never decodes, so a stand-in is enough to classify.
    QFile clip(dir.filePath(QStringLiteral("clip.mp4")));
    QVERIFY(clip.open(QIODevice::WriteOnly));
    clip.write("not really a video");
    clip.close();
    QFile phonePhoto(dir.filePath(QStringLiteral("phone.heic")));
    QVERIFY(phonePhoto.open(QIODevice::WriteOnly));
    phonePhoto.write("not really a photo");
    phonePhoto.close();
    QFile phoneClip(dir.filePath(QStringLiteral("phone.3gp")));
    QVERIFY(phoneClip.open(QIODevice::WriteOnly));
    phoneClip.write("not really a video");
    phoneClip.close();
    QFile document(dir.filePath(QStringLiteral("manual.pdf")));
    QVERIFY(document.open(QIODevice::WriteOnly));
    document.write("not really a PDF");
    document.close();

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Download, CaptureRecord::Download}});
    QCOMPARE(records.size(), 5);
    for (const auto& record : records) {
      QCOMPARE(record.kind, CaptureRecord::Download);
      // A downloaded clip still plays, scrubs and trims like a recording.
      QCOMPARE(record.isVideo(), CaptureScanner::isVideo(QFileInfo(record.path).suffix()));
      QCOMPARE(record.isDocument(),
               CaptureScanner::isDocument(QFileInfo(record.path).suffix()));
    }
  }

  void directOpenRecognizesEveryViewerMediumAndCommonImageFormat() {
    const QStringList images = {QStringLiteral("png"),  QStringLiteral("JPG"),
                                QStringLiteral("svg"),  QStringLiteral("svgz"),
                                QStringLiteral("ico"),  QStringLiteral("jxl"),
                                QStringLiteral("jp2"),  QStringLiteral("j2k"),
                                QStringLiteral("qoi"),  QStringLiteral("psd"),
                                QStringLiteral("dds"),  QStringLiteral("exr"),
                                QStringLiteral("tga")};
    for (const QString& suffix : images) {
      QVERIFY2(CaptureScanner::isImage(suffix), qPrintable(suffix));
      QVERIFY2(CaptureScanner::isSupported(suffix), qPrintable(suffix));
    }
    QVERIFY(CaptureScanner::isSupported(QStringLiteral("mkv")));
    QVERIFY(CaptureScanner::isSupported(QStringLiteral("PDF")));
    QVERIFY(!CaptureScanner::isSupported(QStringLiteral("txt")));

    const QList<QByteArray> decoders = QImageReader::supportedImageFormats();
    for (const QByteArray format : {QByteArray("svg"), QByteArray("ico"), QByteArray("jxl"),
                                    QByteArray("jp2"), QByteArray("qoi"), QByteArray("psd"),
                                    QByteArray("dds"), QByteArray("exr"), QByteArray("tga")}) {
      QVERIFY2(decoders.contains(format),
               qPrintable(QStringLiteral("missing image decoder for %1; found: %2")
                              .arg(QString::fromLatin1(format),
                                   QString::fromLatin1(decoders.join(',')))));
    }
  }

  // The real binary, end to end: a picture opens in the viewer without the
  // library, a plain second launch brings the library up in the same process,
  // another picture opens a second viewer beside the first, and the first is
  // asked for again. Nothing along the way may raise a QML error; which
  // window each request reaches is covered by the viewer tests.
  void executableOpensPicturesInTheViewerAndTheLibraryOnRequest_data() {
    QTest::addColumn<bool>("extensionless");
    QTest::newRow("with-extensions") << false;
    QTest::newRow("without-extensions") << true;
  }

  void executableOpensPicturesInTheViewerAndTheLibraryOnRequest() {
    QFETCH(bool, extensionless);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(extensionless ? QStringLiteral("first # 1")
                                                    : QStringLiteral("first # 1.png"));
    const QString second = dir.filePath(extensionless ? QStringLiteral("second")
                                                     : QStringLiteral("second.jpg"));
    QImage picture(64, 48, QImage::Format_RGB32);
    picture.fill(Qt::darkCyan);
    QVERIFY(picture.save(first, "PNG"));
    QVERIFY(picture.save(second, "JPEG"));
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString& name : {QStringLiteral("HOME"), QStringLiteral("XDG_CONFIG_HOME"),
                                QStringLiteral("XDG_DATA_HOME"), QStringLiteral("XDG_CACHE_HOME"),
                                QStringLiteral("XDG_PICTURES_DIR"), QStringLiteral("XDG_VIDEOS_DIR"),
                                QStringLiteral("XDG_DOWNLOAD_DIR"), QStringLiteral("OMARCHY_SCREENSHOT_DIR"),
                                QStringLiteral("OMARCHY_SCREENRECORD_DIR")})
      environment.insert(name, dir.path());
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QFileInfo(dir.path()).fileName());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QString());
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    const QString binary = QCoreApplication::applicationDirPath() + QStringLiteral("/omaroll");

    QProcess viewer;
    viewer.setProcessEnvironment(environment);
    viewer.setProcessChannelMode(QProcess::SeparateChannels);
    viewer.start(binary, {QStringLiteral("--"), first});
    QVERIFY(viewer.waitForStarted());
    const auto stop = qScopeGuard([&] {
      viewer.terminate();
      if (!viewer.waitForFinished(3000)) {
        viewer.kill();
        viewer.waitForFinished();
      }
    });
    QByteArray errors;
    const auto settle = [&] {
      QVERIFY2(!viewer.waitForFinished(1500), viewer.readAllStandardError().constData());
      errors += viewer.readAllStandardError();
    };
    settle();

    // Each later launch hands its request to the running process and exits.
    const auto forward = [&](const QStringList& arguments) {
      QProcess launch;
      launch.setProcessEnvironment(environment);
      launch.start(binary, arguments);
      QVERIFY(launch.waitForFinished(5000));
      QCOMPARE(launch.exitCode(), 0);
    };
    forward({});
    settle();
    forward({QStringLiteral("--"), second});
    settle();
    forward({QStringLiteral("--"), first});
    settle();
    forward({QStringLiteral("--library"), QStringLiteral("--"), second});
    settle();

    for (const QByteArray& marker :
         {QByteArray("qrc:"), QByteArray("TypeError"), QByteArray("ReferenceError"),
          QByteArray("unsupported media file")}) {
      QVERIFY2(!errors.contains(marker), errors.constData());
    }
  }

  void executableAcceptsDirectPdfOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pdf = dir.filePath(QStringLiteral("document # 1.pdf"));
    {
      QPdfWriter writer(pdf);
      QPainter painter(&writer);
      QVERIFY(painter.isActive());
      painter.drawText(QPoint(100, 100), QStringLiteral("Direct PDF open"));
    }
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    // Keep settings, discovery, the instance socket and the window away from
    // any real user session, including when this test runs outside CTest.
    for (const QString& name : {QStringLiteral("HOME"), QStringLiteral("XDG_CONFIG_HOME"),
                                QStringLiteral("XDG_DATA_HOME"), QStringLiteral("XDG_CACHE_HOME"),
                                QStringLiteral("XDG_PICTURES_DIR"), QStringLiteral("XDG_VIDEOS_DIR"),
                                QStringLiteral("XDG_DOWNLOAD_DIR"), QStringLiteral("OMARCHY_SCREENSHOT_DIR"),
                                QStringLiteral("OMARCHY_SCREENRECORD_DIR")})
      environment.insert(name, dir.path());
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"), QFileInfo(dir.path()).fileName());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QString());
    environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/omaroll"), {pdf});
    QVERIFY(process.waitForStarted());
    const auto stop = qScopeGuard([&] {
      process.terminate();
      if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished();
      }
    });
    QVERIFY2(!process.waitForFinished(1500), process.readAllStandardError().constData());
    QVERIFY(!process.readAllStandardError().contains("unsupported media file"));
  }

  void videoNameOnImageFileStaysAnImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::green);
    // A recording name on a PNG: the medium has to win, or the grid offers
    // "Trim" on something omacut cannot open.
    QVERIFY(
        image.save(dir.filePath(QStringLiteral("screenrecording-2026-08-31_10-00-00.png")), "PNG"));

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Screenshot);
  }

  void pdfsAreScannedRenderedAndInspected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("reference.pdf"));
    {
      QPdfWriter writer(path);
      writer.setResolution(96);
      QPainter painter(&writer);
      QVERIFY(painter.isActive());
      painter.drawText(QPoint(120, 160), QStringLiteral("Omaroll PDF page one"));
      QVERIFY(writer.newPage());
      painter.drawText(QPoint(120, 160), QStringLiteral("Omaroll PDF page two"));
      painter.end();
    }

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Document);
    QVERIFY(records.first().isDocument());
    QVERIFY(!records.first().isVideo());

    if (!PdfSupport::available()) {
      QSKIP("Poppler is not installed");
    }
    const QImage preview = PdfSupport::renderPage(path, 2, QSize(420, 420));
    QVERIFY(!preview.isNull());
    QVERIFY(preview.width() > 0);
    QVERIFY(preview.height() > 0);

    PdfInspector inspector;
    QVERIFY(inspector.available());
    inspector.inspect(path);
    QTRY_VERIFY_WITH_TIMEOUT(!inspector.loading(), 5000);
    QCOMPARE(inspector.pageCount(), 2);
    QVERIFY(inspector.error().isEmpty());
  }

  // --- Filtering and sorting -------------------------------------------

  void namesSortNaturally_data() {
    QTest::addColumn<QString>("localeName");
    QTest::newRow("minimal-environment") << QStringLiteral("C");
    QTest::newRow("english") << QStringLiteral("en_US");
    QTest::newRow("german") << QStringLiteral("de_DE");
  }

  void namesSortNaturally() {
    QFETCH(QString, localeName);
    const QLocale previousLocale;
    const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(previousLocale); });
    QLocale::setDefault(QLocale(localeName));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const char* variable : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                                 "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR"}) {
      QVERIFY(qputenv(variable, dir.path().toUtf8()));
    }
    const QStringList names = {QStringLiteral("photo10.png"), QStringLiteral("photo2.png"),
                              QStringLiteral("Photo02.png"), QStringLiteral("photo1.png"),
                              QStringLiteral("旅行10.png"), QStringLiteral("旅行2.png")};
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::gray);
    for (const QString& name : names) {
      QVERIFY(image.save(dir.filePath(name)));
    }
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("other"))));
    QVERIFY(image.save(dir.filePath(QStringLiteral("other/photo2.png"))));
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    proxy.setSortMode(CaptureFilterModel::NameAscending);
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 7, 5000);
    const auto row = [&](const QString& name) { return proxy.rowOf(dir.filePath(name)); };
    QVERIFY(row(QStringLiteral("photo1.png")) < row(QStringLiteral("photo2.png")));
    QVERIFY(row(QStringLiteral("photo2.png")) < row(QStringLiteral("photo10.png")));
    QVERIFY(row(QStringLiteral("Photo02.png")) < row(QStringLiteral("photo10.png")));
    QVERIFY(row(QStringLiteral("旅行2.png")) < row(QStringLiteral("旅行10.png")));
    QVERIFY(row(QStringLiteral("other/photo2.png")) < row(QStringLiteral("photo2.png")));
    QStringList firstOrder;
    for (int i = 0; i < proxy.count(); ++i) {
      firstOrder.append(proxy.pathAt(i));
    }
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    proxy.setSortMode(CaptureFilterModel::NameAscending);
    for (int i = 0; i < proxy.count(); ++i) {
      QCOMPARE(proxy.pathAt(i), firstOrder.at(i));
    }
  }

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
    // during the test would otherwise always be the newest thing in the
    // library.
    const auto write = [&](const QString& name, int edge, const QDateTime& stamp) {
      QImage image(edge, edge, QImage::Format_RGB32);
      image.fill(Qt::gray);
      const QString path = dir.filePath(name);
      QDir().mkpath(QFileInfo(path).absolutePath());
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

    // The header's total must update even when a new source row is excluded
    // by the active filter and the proxy's visible count stays unchanged.
    QSignalSpy sourceCountChanged(&proxy, &CaptureFilterModel::countChanged);
    QSignalSpy rescanned(&model, &CaptureModel::countChanged);
    write(QStringLiteral("mountain.png"), 12, QDateTime(QDate(2026, 8, 19), QTime(9, 0)));
    model.refresh();
    QVERIFY(rescanned.wait(5000));
    QCOMPARE(proxy.sourceCount(), 4);
    QCOMPARE(proxy.count(), 1);
    QVERIFY(!sourceCountChanged.isEmpty());
    QTRY_COMPARE(proxy.folderItemCount(dir.path()), 4);

    proxy.setSearchText({});
    QCOMPARE(proxy.count(), 4);

    const QString beachPath = dir.filePath(QStringLiteral("beach.png"));
    proxy.setOcrText(beachPath, QStringLiteral("Invoice overdue total"));
    proxy.setSearchText(QStringLiteral("beach overdue"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), beachPath);
    proxy.setSearchText({});

    // A caption is searched like the filename and the picture text.
    settings.setCaption(beachPath, QStringLiteral("Sunset from the pier"));
    QTRY_COMPARE_WITH_TIMEOUT(model.recordAt(model.rowOf(beachPath)).caption,
                              QStringLiteral("Sunset from the pier"), 1000);
    proxy.setSearchText(QStringLiteral("pier"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), beachPath);
    settings.setCaption(beachPath, QString());
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 0, 1000);
    proxy.setSearchText({});

    rescanned.clear();
    write(QStringLiteral("album/forest.png"), 18, QDateTime(QDate(2026, 8, 18), QTime(9, 0)));
    model.refresh();
    QVERIFY(rescanned.wait(5000));
    QCOMPARE(proxy.count(), 5);
    QTRY_COMPARE(proxy.folders(), QStringList({dir.path(), dir.filePath(QStringLiteral("album"))}));
    QCOMPARE(proxy.folderItemCount(dir.path()), 5);
    QCOMPARE(proxy.folderItemCount(dir.filePath(QStringLiteral("album"))), 1);
    QCOMPARE(proxy.folderItemCount(dir.filePath(QStringLiteral("missing"))), 0);
    proxy.setFolderFilter(dir.path());
    QCOMPARE(proxy.count(), 5);
    proxy.setFolderFilter(dir.filePath(QStringLiteral("album")));
    QCOMPARE(proxy.count(), 1);
    proxy.setFolderFilter(dir.filePath(QStringLiteral("another-folder")));
    QCOMPARE(proxy.count(), 0);
    proxy.setFolderFilter({});
    QCOMPARE(proxy.count(), 5);
    const QString beach = dir.filePath(QStringLiteral("beach.png"));
    proxy.setAlbumFilter(QStringLiteral("Weekend"),
                         {beach, dir.filePath(QStringLiteral("album/forest.png"))});
    QCOMPARE(proxy.count(), 2);
    QCOMPARE(proxy.adjacentPath(beach, 1), dir.filePath(QStringLiteral("album/forest.png")));
    QCOMPARE(proxy.adjacentPath(beach, -1), dir.filePath(QStringLiteral("album/forest.png")));
    proxy.setAlbumFilter({}, {});
    QCOMPARE(proxy.count(), 5);

    proxy.setDateRange(QStringLiteral("2026-08-30"), QStringLiteral("2026-08-31"));
    QCOMPARE(proxy.count(), 2);
    QCOMPARE(proxy.dateFrom(), QStringLiteral("2026-08-30"));
    QCOMPARE(proxy.dateTo(), QStringLiteral("2026-08-31"));
    proxy.clearDateRange();
    QCOMPARE(proxy.count(), 5);
    proxy.setModifiedAfter(QStringLiteral("2026-08-20T09:00:00"));
    QCOMPARE(proxy.count(), 2);
    QVERIFY(!proxy.modifiedAfter().isEmpty());
    proxy.clearDateRange();
    QCOMPARE(proxy.count(), 5);
    QTRY_VERIFY_WITH_TIMEOUT(!proxy.dateBuckets().isEmpty(), 1000);
    QCOMPARE(proxy.dateDays(QStringLiteral("2026-08")).size(), 5);

    // Camera and lens filters come from the metadata index. The choice lists
    // count the library, not the current view, and the filter survives a
    // saved view round trip.
    QVERIFY(proxy.cameras().isEmpty());
    const CaptureRecord& beachRecord = model.recordAt(model.rowOf(beach));
    model.applyMetadata({{beach, beachRecord.modified, beachRecord.bytes, {}, beachRecord.device,
                          beachRecord.inode, QStringLiteral("Sony ILCE-7M3"),
                          QStringLiteral("FE 24-70mm F2.8 GM")}});
    QTRY_COMPARE_WITH_TIMEOUT(proxy.cameras().size(), 1, 1000);
    QCOMPARE(proxy.cameras().first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Sony ILCE-7M3"));
    QCOMPARE(proxy.cameras().first().toMap().value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(proxy.lenses().size(), 1);
    proxy.setCameraFilter(QStringLiteral("Sony ILCE-7M3"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), beach);
    proxy.setLensFilter(QStringLiteral("Some other lens"));
    QCOMPARE(proxy.count(), 0);
    proxy.setLensFilter(QStringLiteral("FE 24-70mm F2.8 GM"));
    QCOMPARE(proxy.count(), 1);
    const QVariantMap cameraView = proxy.currentView();
    QCOMPARE(cameraView.value(QStringLiteral("camera")).toString(), QStringLiteral("Sony ILCE-7M3"));
    proxy.setCameraFilter({});
    proxy.setLensFilter({});
    QCOMPARE(proxy.count(), 5);
    proxy.applyView(QStringLiteral("Sony shots"), cameraView);
    QCOMPARE(proxy.cameraFilter(), QStringLiteral("Sony ILCE-7M3"));
    QCOMPARE(proxy.count(), 1);
    proxy.clearSmartCollection();
    proxy.setCameraFilter({});
    proxy.setLensFilter({});

    // Ratings come from the settings marks, filter by a minimum and sort
    // highest first; a saved view keeps the minimum.
    settings.setRating({beach}, 4);
    QTRY_COMPARE_WITH_TIMEOUT(model.recordAt(model.rowOf(beach)).rating, 4, 1000);
    proxy.setMinimumRating(3);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), beach);
    proxy.setMinimumRating(5);
    QCOMPARE(proxy.count(), 0);
    const QVariantMap ratedView = proxy.currentView();
    QCOMPARE(ratedView.value(QStringLiteral("minRating")).toInt(), 5);
    proxy.setMinimumRating(0);
    QCOMPARE(proxy.count(), 5);
    proxy.setSortMode(CaptureFilterModel::RatingFirst);
    QCOMPARE(proxy.pathAt(0), beach);
    proxy.setSortMode(CaptureFilterModel::NewestFirst);
    proxy.applyView(QStringLiteral("Five stars"), ratedView);
    QCOMPARE(proxy.minimumRating(), 5);
    QCOMPARE(proxy.count(), 0);
    proxy.clearSmartCollection();
    proxy.setMinimumRating(0);
    settings.setRating({beach}, 0);
    QTRY_COMPARE_WITH_TIMEOUT(model.recordAt(model.rowOf(beach)).rating, 0, 1000);
    QCOMPARE(proxy.count(), 5);

    const QString forest = dir.filePath(QStringLiteral("album/forest.png"));
    proxy.setTagFilter(QStringLiteral("Review"), {beach, forest});
    QCOMPARE(proxy.count(), 2);
    const QVariantMap savedView = proxy.currentView();
    proxy.applyView(QStringLiteral("Review pictures"), savedView, {beach, forest});
    QCOMPARE(proxy.smartCollectionFilter(), QStringLiteral("Review pictures"));
    QCOMPARE(proxy.count(), 2);
    proxy.setSearchText(QStringLiteral("beach"));
    QVERIFY(proxy.smartCollectionFilter().isEmpty());
    QCOMPARE(proxy.count(), 1);
    proxy.setSearchText({});
    proxy.setTagFilter({}, {});
    QCOMPARE(proxy.count(), 5);

    // Navigation must visit every visible row once before wrapping, in both
    // directions. A one-step assertion misses cycles that accidentally cover
    // only part of a mixed library.
    const QString first = proxy.pathAt(0);
    for (const int direction : {1, -1}) {
      QSet<QString> visited;
      QString path = first;
      for (int step = 0; step < proxy.count(); ++step) {
        QVERIFY(!path.isEmpty());
        QVERIFY(!visited.contains(path));
        visited.insert(path);
        path = proxy.adjacentPath(path, direction);
      }
      QCOMPARE(visited.size(), proxy.count());
      QCOMPARE(path, first);
    }

    const QString next = proxy.adjacentPathInFolder(beach, 1);
    const QString previous = proxy.adjacentPathInFolder(beach, -1);
    QVERIFY(!next.isEmpty());
    QVERIFY(!previous.isEmpty());
    QCOMPARE(QFileInfo(next).absolutePath(), dir.path());
    QCOMPARE(QFileInfo(previous).absolutePath(), dir.path());
    for (const int direction : {1, -1}) {
      QSet<QString> visited;
      QString path = beach;
      do {
        QVERIFY(!visited.contains(path));
        visited.insert(path);
        path = proxy.adjacentPathInFolder(path, direction);
      } while (path != beach);
      QCOMPARE(visited.size(), 4);
    }
    QCOMPARE(proxy.adjacentPathInFolder(QStringLiteral("/missing.png"), 1), QString());
  }

  void localOcrSearchRunsOnceThenUsesItsPrivateCache() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_TEST_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_TEST_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("ocr-runs.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf 'Invoice total forty two\\n'\n"
                    "printf 'run\\n' >> \"$OMAROLL_OCR_TEST_LOG\"\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_OCR_TEST_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const QString imagePath = dir.filePath(QStringLiteral("capture-001.png"));
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::white);
    QVERIFY(image.save(imagePath, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 1);

    const auto runSearch = [&] {
      CaptureFilterModel proxy;
      proxy.setSourceModel(&model);
      OcrIndex index(&model);
      QVERIFY(index.available());
      connect(&index, &OcrIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);
      proxy.setSearchText(QStringLiteral("invoice forty"));
      index.setSearchText(proxy.searchText());
      QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 1, 3000);
      QCOMPARE(proxy.pathAt(0), imagePath);
      QCOMPARE(proxy.ocrSnippetAt(0), QStringLiteral("Invoice total forty two"));
      QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
      QCOMPARE(index.completed(), 1);
      QCOMPARE(index.total(), 1);
      proxy.setSearchText(QStringLiteral("capture"));
      QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 1, 1000);
      QVERIFY(proxy.ocrSnippetAt(0).isEmpty());
    };

    runSearch();
    runSearch();

    image = QImage(20, 14, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(imagePath, "PNG"));
    QFile changed(imagePath);
    QVERIFY(changed.open(QIODevice::ReadWrite));
    QVERIFY(changed.setFileTime(QDateTime::currentDateTime().addSecs(2),
                                QFileDevice::FileModificationTime));
    changed.close();
    scanned.clear();
    model.refresh();
    QVERIFY(scanned.wait(5000));
    runSearch();

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll(), QByteArray("run\nrun\n"));
    const QDir cache(OcrIndex::cacheDirectory());
    QCOMPARE(cache.entryList({QStringLiteral("*.ocr")}, QDir::Files).size(), 1);
    const QFileInfo entry(
        cache.filePath(cache.entryList({QStringLiteral("*.ocr")}, QDir::Files).first()));
    QVERIFY(!(entry.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    OcrIndex index(&model);
    connect(&index, &OcrIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);
    proxy.setSearchText(QStringLiteral("invoice forty"));
    index.setSearchText(proxy.searchText());
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
    QCOMPARE(index.completed(), 1);
    QCOMPARE(index.total(), 1);
    QCOMPARE(index.clearCache(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 0, 1000);
    QCOMPARE(cache.entryList({QStringLiteral("*.ocr")}, QDir::Files).size(), 0);
  }

  void sparseOcrRetryImprovesWeakRecognitionAndCachesTheResult() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_SPARSE_LOG");
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_SPARSE_LOG", previousLog);
      putBack("XDG_CACHE_HOME", previousCache);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("sparse-runs.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf '%s\\n' \"$*\" >> \"$OMAROLL_OCR_SPARSE_LOG\"\n"
                    "sparse_mode=false\n"
                    "for argument do\n"
                    "  [ \"$argument\" = 11 ] && sparse_mode=true\n"
                    "done\n"
                    "case \"$1\" in\n"
                    "  *sparse*)\n"
                    "    if $sparse_mode; then printf 'Isolated label found\\n'; "
                    "else printf 'x\\n'; fi ;;\n"
                    "  *background*) printf 'x\\n' ;;\n"
                    "  *) printf 'Enough text from the normal pass\\n' ;;\n"
                    "esac\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousPath));
    QVERIFY(qputenv("OMAROLL_OCR_SPARSE_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const auto makeImage = [&dir](const QString& name) {
      const QString path = dir.filePath(name);
      QImage image(16, 12, QImage::Format_RGB32);
      image.fill(Qt::white);
      return image.save(path, "PNG") ? path : QString();
    };
    const QString sparse = makeImage(QStringLiteral("sparse.png"));
    const QString normal = makeImage(QStringLiteral("normal.png"));
    const QString background = makeImage(QStringLiteral("background.png"));
    QVERIFY(!sparse.isEmpty() && !normal.isEmpty() && !background.isEmpty());

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 3);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    OcrIndex index(&model);
    QVERIFY(index.available());
    connect(&index, &OcrIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);

    index.recognize(sparse);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewText(), QStringLiteral("Isolated label found"));
    proxy.setSearchText(QStringLiteral("isolated label"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), sparse);
    proxy.setSearchText({});

    index.cancelReview();
    index.recognize(sparse);
    QVERIFY(!index.reviewing());
    QCOMPARE(index.reviewText(), QStringLiteral("Isolated label found"));

    index.recognize(normal);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewText(), QStringLiteral("Enough text from the normal pass"));

    // Background search remains single-pass even when its result is weak.
    index.cancelReview();
    proxy.setSearchText(QStringLiteral("not present"));
    index.setSearchText(proxy.searchText());
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
    QCOMPARE(proxy.count(), 0);

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QStringList runs =
        QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(runs.size(), 4);
    QVERIFY(!runs.at(0).contains(QStringLiteral("--psm 11")));
    QVERIFY(runs.at(1).contains(QStringLiteral("--psm 11")));
    QVERIFY(!runs.at(2).contains(QStringLiteral("--psm 11")));
    QVERIFY(!runs.at(3).contains(QStringLiteral("--psm 11")));
  }

  void clearingOcrSearchStopsTheQueuedLibraryWork() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_CANCEL_LOG");
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_CANCEL_LOG", previousLog);
      putBack("XDG_CACHE_HOME", previousCache);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("ocr-cancel.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf 'run\\n' >> \"$OMAROLL_OCR_CANCEL_LOG\"\n"
                    "/usr/bin/sleep 0.15\n"
                    "printf 'Nothing relevant here\\n'\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_OCR_CANCEL_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int index = 0; index < 16; ++index) {
      QVERIFY(image.save(dir.filePath(QStringLiteral("photo-%1.png").arg(index)), "PNG"));
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 16);

    OcrIndex index(&model);
    index.setSearchText(QStringLiteral("needle"));
    QTRY_VERIFY(index.indexing());
    QCOMPARE(index.total(), 16);
    index.setSearchText({});
    QVERIFY(!index.indexing());
    QCOMPARE(index.total(), 0);

    QTest::qWait(300);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll().count("run\n"), 1);
  }

  void ocrReviewPreservesLayoutHandlesEmptyAndRejectsStaleResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_OCR_REVIEW_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_OCR_REVIEW_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("review-runs.log"));
    QFile tesseract(dir.filePath(QStringLiteral("tesseract")));
    QVERIFY(tesseract.open(QIODevice::WriteOnly));
    tesseract.write("#!/bin/sh\n"
                    "printf 'run\\n' >> \"$OMAROLL_OCR_REVIEW_LOG\"\n"
                    "case \"$1\" in\n"
                    "  *slow*) /usr/bin/sleep 0.2; printf 'STALE\\n' ;;\n"
                    "  *empty*) ;;\n"
                    "  *fail*) exit 2 ;;\n"
                    "  *) printf 'Alpha  beta\\nSecond line\\n' ;;\n"
                    "esac\n");
    tesseract.close();
    QVERIFY(tesseract.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousPath));
    QVERIFY(qputenv("OMAROLL_OCR_REVIEW_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const auto makeImage = [&dir](const QString& name) {
      const QString path = dir.filePath(name);
      QImage image(16, 12, QImage::Format_RGB32);
      image.fill(Qt::white);
      return image.save(path, "PNG") ? path : QString();
    };
    const QString layout = makeImage(QStringLiteral("layout.png"));
    const QString empty = makeImage(QStringLiteral("empty.png"));
    const QString failure = makeImage(QStringLiteral("fail.png"));
    const QString slow = makeImage(QStringLiteral("slow.png"));
    const QString fresh = makeImage(QStringLiteral("fresh.png"));
    QVERIFY(!layout.isEmpty() && !empty.isEmpty() && !failure.isEmpty() && !slow.isEmpty() &&
            !fresh.isEmpty());

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 5);

    OcrIndex index(&model);
    QVERIFY(index.available());
    index.recognize(layout);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewPath(), layout);
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));
    QVERIFY(index.reviewError().isEmpty());

    // The second review comes from the shared private cache without another
    // process, and the layout text remains distinct from normalized search.
    index.cancelReview();
    index.recognize(layout);
    QVERIFY(!index.reviewing());
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));

    index.recognize(empty);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QVERIFY(index.reviewText().isEmpty());
    QVERIFY(index.reviewError().isEmpty());

    index.recognize(failure);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QVERIFY(index.reviewText().isEmpty());
    QCOMPARE(index.reviewError(), QStringLiteral("Could not extract text"));

    // A result from the previous image must not overwrite a newer request.
    index.recognize(slow);
    index.recognize(fresh);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 3000);
    QCOMPARE(index.reviewPath(), fresh);
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));
    QVERIFY(index.reviewError().isEmpty());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const qsizetype runs = log.readAll().count("run\n");
    // The replaced slow review may be killed before its helper reaches the
    // first instruction, or immediately after it. Either way only the fresh
    // result is published.
    QVERIFY(runs == 5 || runs == 6);
    log.close();

    // An explicit review jumps ahead of an in-flight background search. The
    // interrupted search item is then retried, rather than being recorded as
    // a failure or silently dropped.
    const auto runCount = [&] {
      QFile runsFile(logPath);
      return runsFile.open(QIODevice::ReadOnly) ? runsFile.readAll().count("run\n") : qsizetype(-1);
    };
    index.cancelReview();
    index.setSearchText(QStringLiteral("needle"));
    QTRY_VERIFY(index.indexing());
    QTRY_VERIFY(runCount() > runs);
    const qsizetype beforePriorityReview = runCount();
    index.recognize(layout, true);
    QTRY_VERIFY_WITH_TIMEOUT(!index.reviewing(), 1000);
    QCOMPARE(index.reviewText(), QStringLiteral("Alpha  beta\nSecond line"));
    QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 3000);
    QCOMPARE(runCount(), beforePriorityReview + 2);
    index.setSearchText({});
  }

  void qrDetectionCachesOnlyPresenceAndRejectsStaleResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_QR_TEST_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_QR_TEST_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("qr-runs.log"));
    QFile zbar(dir.filePath(QStringLiteral("zbarimg")));
    QVERIFY(zbar.open(QIODevice::WriteOnly));
    zbar.write("#!/bin/sh\n"
               "printf 'run\\n' >> \"$OMAROLL_QR_TEST_LOG\"\n"
               "for last do :; done\n"
               "case \"$last\" in\n"
               "  *slow*) /usr/bin/sleep 0.2; printf 'old-secret\\n' ;;\n"
               "  *positive*) printf 'otpauth://private-value\\n' ;;\n"
               "  *) exit 4 ;;\n"
               "esac\n");
    zbar.close();
    QVERIFY(zbar.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousPath));
    QVERIFY(qputenv("OMAROLL_QR_TEST_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const auto makeImage = [&dir](const QString& name) {
      const QString path = dir.filePath(name);
      QImage image(16, 12, QImage::Format_RGB32);
      image.fill(Qt::white);
      return image.save(path, "PNG") ? path : QString();
    };
    const QString positive = makeImage(QStringLiteral("positive.png"));
    const QString negative = makeImage(QStringLiteral("negative.png"));
    const QString slow = makeImage(QStringLiteral("slow.png"));
    const QString fresh = makeImage(QStringLiteral("fresh-positive.png"));
    QVERIFY(!positive.isEmpty() && !negative.isEmpty() && !slow.isEmpty() && !fresh.isEmpty());

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    QrDetector detector(&model);
    QVERIFY(detector.available());
    detector.inspect(positive);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QCOMPARE(detector.path(), positive);
    QVERIFY(detector.detected());

    // A second inspection uses the in-memory identity cache. The decoded
    // value is intentionally not exposed by the detector at all.
    detector.inspect(positive);
    QVERIFY(!detector.checking());
    QVERIFY(detector.detected());
    {
      QFile log(logPath);
      QVERIFY(log.open(QIODevice::ReadOnly));
      QCOMPARE(log.readAll().count("run\n"), 1);
    }

    detector.inspect(negative);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QVERIFY(!detector.detected());

    detector.inspect(slow);
    detector.inspect(fresh);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QCOMPARE(detector.path(), fresh);
    QVERIFY(detector.detected());

    // Cancelling the slow check must not cache a false negative for it.
    detector.inspect(slow);
    QTRY_VERIFY_WITH_TIMEOUT(!detector.checking(), 3000);
    QCOMPARE(detector.path(), slow);
    QVERIFY(detector.detected());

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const qsizetype runs = log.readAll().count("run\n");
    QVERIFY(runs == 4 || runs == 5);
    detector.clear();
    QVERIFY(detector.path().isEmpty());
    QVERIFY(!detector.checking());
    QVERIFY(!detector.detected());
  }

  void qrDetectionStopsSafelyDuringTeardown_data() {
    QTest::addColumn<int>("destructionOrder");
    QTest::newRow("parent-deletes-model-first") << 0;
    QTest::newRow("detector-dies-before-model") << 1;
    QTest::newRow("model-dies-with-detector-alive") << 2;
  }

  void qrDetectionStopsSafelyDuringTeardown() {
    QFETCH(int, destructionOrder);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QMap<QByteArray, QByteArray> previousEnvironment;
    for (const char* name : {"PATH", "OMAROLL_QR_TEST_FIFO", "OMAROLL_QR_TEST_PID",
                             "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      previousEnvironment.insert(name, qgetenv(name));
    }
    const auto restore = qScopeGuard([&] {
      for (auto it = previousEnvironment.cbegin(); it != previousEnvironment.cend(); ++it) {
        it.value().isNull() ? qunsetenv(it.key().constData())
                            : qputenv(it.key().constData(), it.value());
      }
    });

    const QString fifoPath = dir.filePath(QStringLiteral("helper-gate"));
    const QString pidPath = dir.filePath(QStringLiteral("helper.pid"));
    QCOMPARE(::mkfifo(QFile::encodeName(fifoPath).constData(), 0600), 0);
    // Holding both ends open lets the helper open its reader and guarantees
    // that read cannot finish with EOF before the destruction under test.
    const int gate = ::open(QFile::encodeName(fifoPath).constData(), O_RDWR | O_NONBLOCK);
    QVERIFY(gate >= 0);
    const auto closeGate = qScopeGuard([&] { ::close(gate); });

    QFile zbar(dir.filePath(QStringLiteral("zbarimg")));
    QVERIFY(zbar.open(QIODevice::WriteOnly));
    zbar.write("#!/bin/sh\n"
               "exec 3< \"$OMAROLL_QR_TEST_FIFO\"\n"
               "printf '%s\\n' \"$$\" > \"$OMAROLL_QR_TEST_PID\"\n"
               "IFS= read -r reply <&3\n"
               "printf 'private-value\\n'\n");
    zbar.close();
    QVERIFY(zbar.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8() + ':' + previousEnvironment.value("PATH")));
    QVERIFY(qputenv("OMAROLL_QR_TEST_FIFO", fifoPath.toUtf8()));
    QVERIFY(qputenv("OMAROLL_QR_TEST_PID", pidPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    const QString imagePath = dir.filePath(QStringLiteral("capture.png"));
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::white);
    QVERIFY(image.save(imagePath, "PNG"));
    AppSettings settings;
    settings.setScanDownloads(false);
    auto owner = std::make_unique<QObject>();
    auto* model = new CaptureModel(&settings, owner.get());
    QSignalSpy scanned(model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model->rowCount(), 1);
    auto* detector = new QrDetector(model, owner.get());
    detector->inspect(imagePath);
    QVERIFY(detector->checking());
    const auto helperPid = [&] {
      QFile file(pidPath);
      return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : qint64(0);
    };
    QTRY_VERIFY_WITH_TIMEOUT(helperPid() > 0, 3000);
    const pid_t pid = static_cast<pid_t>(helperPid());
    QCOMPARE(::kill(pid, 0), 0);
    QVERIFY(detector->checking());

    if (destructionOrder == 0) {
      // Match QObject child teardown: the model was registered first, so it
      // is deleted before the detector kills and synchronously reaps zbar.
      owner.reset();
    } else if (destructionOrder == 1) {
      QSignalSpy stateChanged(detector, &QrDetector::stateChanged);
      delete detector;
      QCOMPARE(stateChanged.size(), 0);
      QCOMPARE(model->rowCount(), 1);
    } else {
      delete model;
      QVERIFY(!detector->checking());
      QVERIFY(!detector->detected());
      QVERIFY(detector->path().isEmpty());
      // Deliver the helper's late finished callback while the detector lives.
      QTRY_VERIFY_WITH_TIMEOUT(::kill(pid, 0) == -1 && errno == ESRCH, 3000);
      QSignalSpy stateChanged(detector, &QrDetector::stateChanged);
      detector->inspect(imagePath);
      QVERIFY(!detector->checking());
      QVERIFY(!detector->detected());
      QCOMPARE(stateChanged.size(), 1);
      owner.reset();
    }
    // A killed but unreaped helper still has a PID; require full cleanup.
    QCOMPARE(::kill(pid, 0), -1);
    QCOMPARE(errno, ESRCH);
  }

  void equalSortKeysHaveStableOrder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_DOWNLOAD_DIR", dir.path().toUtf8()));

    const QDateTime stamp(QDate(2026, 8, 20), QTime(9, 0));
    const auto write = [&](const QString& relativePath) {
      const QString path = dir.filePath(relativePath);
      QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
      QImage image(16, 16, QImage::Format_RGB32);
      image.fill(Qt::gray);
      QVERIFY(image.save(path, "PNG"));
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadWrite));
      QVERIFY(file.setFileTime(stamp, QFileDevice::FileModificationTime));
    };

    // Reverse path order on disk so deterministic results cannot come from
    // insertion order. All three otherwise have the same sort keys.
    write(QStringLiteral("z/duplicate.png"));
    write(QStringLiteral("m/duplicate.png"));
    write(QStringLiteral("a/duplicate.png"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 3);
    const QStringList expected = {
        dir.filePath(QStringLiteral("a/duplicate.png")),
        dir.filePath(QStringLiteral("m/duplicate.png")),
        dir.filePath(QStringLiteral("z/duplicate.png")),
    };
    for (const int mode : {CaptureFilterModel::NewestFirst, CaptureFilterModel::OldestFirst,
                           CaptureFilterModel::LargestFirst, CaptureFilterModel::SmallestFirst,
                           CaptureFilterModel::NameAscending}) {
      proxy.setSortMode(mode);
      QStringList actual;
      for (int row = 0; row < proxy.count(); ++row) {
        actual.append(proxy.pathAt(row));
      }
      QCOMPARE(actual, expected);
    }
  }

  void exactDuplicateReviewHashesOnlyPlausibleMatchesAndUpdates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    const QByteArray same(2 * 1024 * 1024, 'a');
    const QByteArray sameSizeDifferent(2 * 1024 * 1024, 'b');
    const auto write = [&](const QString& name, const QByteArray& content) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QCOMPARE(file.write(content), content.size());
    };
    write(QStringLiteral("first.png"), same);
    write(QStringLiteral("copy.png"), same);
    write(QStringLiteral("same-size-but-different.png"), sameSizeDifferent);
    write(QStringLiteral("unique.png"), QByteArray("one of a kind"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    DuplicateIndex duplicates(&model);
    connect(&duplicates, &DuplicateIndex::groupsChanged, &proxy,
            [&] { proxy.setDuplicateGroups(duplicates.groups()); });
    proxy.setDuplicatesOnly(true);
    duplicates.setActive(true);

    QTRY_COMPARE_WITH_TIMEOUT(duplicates.groupCount(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!duplicates.scanning(), 5000);
    QCOMPARE(duplicates.total(), 3);
    QCOMPARE(duplicates.completed(), 3);
    QCOMPARE(duplicates.duplicateCount(), 2);
    QCOMPARE(proxy.count(), 2);
    const QString firstPath = dir.filePath(QStringLiteral("first.png"));
    const QString copyPath = dir.filePath(QStringLiteral("copy.png"));
    QCOMPARE(duplicates.groupPaths(firstPath), QStringList({copyPath, firstPath}));
    QCOMPARE(duplicates.otherCopies(firstPath), QStringList {copyPath});
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Exact match 1 of 1"));
    QCOMPARE(proxy.gridLabelAt(1), QStringLiteral("Exact match 1 of 1"));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("first.png"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("copy.png"))));

    // Reopening an unchanged review reuses the in-memory hashes immediately.
    duplicates.setActive(false);
    duplicates.setActive(true);
    QVERIFY(!duplicates.scanning());
    QCOMPARE(proxy.count(), 2);

    // Rewriting one copy invalidates its identity. The live library change
    // triggers a new comparison and both former matches leave the view.
    write(QStringLiteral("copy.png"), QByteArray("no longer the same"));
    model.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(duplicates.groupCount(), 0, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(proxy.count(), 0, 5000);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("first.png"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("copy.png"))));
  }

  void visuallySimilarPicturesAreSuggestedWithoutGroupingDifferentColour() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const char* name : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                             "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage source(320, 240, QImage::Format_RGB32);
    QPainter painter(&source);
    QLinearGradient gradient(0, 0, source.width(), source.height());
    gradient.setColorAt(0, QColor(24, 70, 130));
    gradient.setColorAt(1, QColor(230, 190, 80));
    painter.fillRect(source.rect(), gradient);
    painter.setPen(QPen(Qt::white, 12));
    painter.drawEllipse(QRect(70, 45, 180, 150));
    painter.drawLine(20, 210, 300, 30);
    painter.end();
    const QString original = dir.filePath(QStringLiteral("original.png"));
    const QString recompressed = dir.filePath(QStringLiteral("recompressed.jpg"));
    const QString different = dir.filePath(QStringLiteral("different.png"));
    QVERIFY(source.save(original, "PNG"));
    QVERIFY(source.scaled(640, 480, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(recompressed, "JPG", 58));
    QImage other(320, 240, QImage::Format_RGB32);
    other.fill(QColor(220, 20, 30));
    QVERIFY(other.save(different, "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 3);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    SimilarityIndex similarities(&model);
    connect(&similarities, &SimilarityIndex::groupsChanged, &proxy,
            [&] { proxy.setSimilarGroups(similarities.groups()); });
    proxy.setSimilarOnly(true);
    similarities.setActive(true);
    QTRY_COMPARE_WITH_TIMEOUT(similarities.groupCount(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!similarities.scanning(), 5000);
    QCOMPARE(similarities.similarCount(), 2);
    QCOMPARE(proxy.count(), 2);
    QVERIFY(proxy.rowOf(original) >= 0);
    QVERIFY(proxy.rowOf(recompressed) >= 0);
    QCOMPARE(proxy.rowOf(different), -1);
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Similar set 1 of 1"));
    // The compare view needs the whole set, itself included, and an unrelated
    // picture reports none.
    QCOMPARE(similarities.groupPaths(original), QStringList({original, recompressed}));
    QVERIFY(similarities.groupPaths(different).isEmpty());
  }

  void duplicateSetsStayTogetherAndFollowTheChosenSort() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    const QStringList oldSet = {
        dir.filePath(QStringLiteral("screenshot-2025-01-02_08-00-00-old-a.png")),
        dir.filePath(QStringLiteral("screenshot-2025-01-02_09-00-00-old-b.png")),
    };
    const QStringList newSet = {
        dir.filePath(QStringLiteral("screenshot-2026-08-02_08-00-00-new-a.png")),
        dir.filePath(QStringLiteral("screenshot-2026-08-02_09-00-00-new-b.png")),
    };
    for (const QString& path : oldSet + newSet) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("media");
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QHash<QString, QString> groups;
    for (const QString& path : oldSet) {
      groups.insert(path, QStringLiteral("old"));
    }
    for (const QString& path : newSet) {
      groups.insert(path, QStringLiteral("new"));
    }
    proxy.setDuplicateGroups(groups);
    proxy.setDuplicatesOnly(true);

    QCOMPARE(groups.value(proxy.pathAt(0)), QStringLiteral("new"));
    QCOMPARE(groups.value(proxy.pathAt(1)), QStringLiteral("new"));
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Exact match 1 of 2"));
    QCOMPARE(proxy.gridLabelAt(2), QStringLiteral("Exact match 2 of 2"));

    proxy.setSortMode(CaptureFilterModel::OldestFirst);
    QCOMPARE(groups.value(proxy.pathAt(0)), QStringLiteral("old"));
    QCOMPARE(groups.value(proxy.pathAt(1)), QStringLiteral("old"));
    QCOMPARE(proxy.gridLabelAt(0), QStringLiteral("Exact match 1 of 2"));
    QCOMPARE(proxy.gridLabelAt(2), QStringLiteral("Exact match 2 of 2"));
  }

  void mediaDetailsParseCameraExposureVideoAndAudio() {
    const QByteArray image = "JPEG\n2020:05:06 07:08:09\n\nTestMake\nTestMake Model X\n"
                             "Prime 50mm\n28/10\n1/125\n400\n50/1\n8\nsRGB\n";
    const QStringList imageLines = MediaInspector::parseImage(image);
    QCOMPARE(imageLines.value(0), QStringLiteral("JPEG  ·  sRGB  ·  8-bit"));
    QVERIFY(imageLines.value(1).startsWith(QStringLiteral("Taken  ·  ")));
    QCOMPARE(imageLines.value(2), QStringLiteral("Camera  ·  TestMake Model X"));
    QCOMPARE(imageLines.value(3), QStringLiteral("Lens  ·  Prime 50mm"));
    QCOMPARE(imageLines.value(4), QStringLiteral("f/2.8  ·  1/125 s  ·  ISO 400  ·  50 mm"));

    const QByteArray video = R"({
      "streams": [
        {"codec_type":"video","codec_name":"h264","profile":"High",
         "pix_fmt":"yuv420p","r_frame_rate":"30000/1001"},
        {"codec_type":"audio","codec_name":"aac","sample_rate":"48000",
         "channel_layout":"stereo"}
      ],
      "format": {"format_name":"mov,mp4,m4a,3gp,3g2,mj2","bit_rate":"3608000"}
    })";
    const QStringList videoLines = MediaInspector::parseVideo(video, QStringLiteral("mp4"));
    QCOMPARE(videoLines.value(0), QStringLiteral("MP4  ·  H.264  ·  High  ·  yuv420p"));
    QCOMPARE(videoLines.value(1), QStringLiteral("29.97 fps  ·  3.6 Mbps"));
    QCOMPARE(videoLines.value(2), QStringLiteral("AAC  ·  stereo  ·  48 kHz"));
  }

  void embeddedMediaDatesParseWithoutChangingWallClockPhotoTime() {
    QCOMPARE(MediaMetadataIndex::parseImageDate("2020:05:06 07:08:09\n2019:01:02 03:04:05\n"),
             QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
    QCOMPARE(MediaMetadataIndex::parseImageDate("\n2019:01:02 03:04:05\n"),
             QDateTime(QDate(2019, 1, 2), QTime(3, 4, 5)));
    QVERIFY(!MediaMetadataIndex::parseImageDate("undefined\n\n").isValid());

    const QByteArray video = R"({
      "streams": [{"tags":{"creation_time":"2018-04-03T02:01:00"}}],
      "format": {"tags":{"com.apple.quicktime.creationdate":"2021-08-09T10:11:12"}}
    })";
    QCOMPARE(MediaMetadataIndex::parseVideoDate(video),
             QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
    QVERIFY(!MediaMetadataIndex::parseVideoDate("not json").isValid());
  }

  void embeddedCameraNamesParseAndNormalize() {
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("Apple"), QStringLiteral("iPhone 12")),
             QStringLiteral("Apple iPhone 12"));
    // Canon and Nikon repeat the maker in the model; do not print it twice.
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("Canon"),
                                            QStringLiteral("Canon EOS R6")),
             QStringLiteral("Canon EOS R6"));
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("SONY  "), QString()),
             QStringLiteral("SONY"));
    QCOMPARE(MediaMetadataIndex::cameraName(QStringLiteral("undefined"), QStringLiteral("X-T5")),
             QStringLiteral("X-T5"));
    QVERIFY(MediaMetadataIndex::cameraName({}, {}).isEmpty());

    const MediaMetadataIndex::Details image = MediaMetadataIndex::parseImageDetails(
        "2020:05:06 07:08:09\n\nFUJIFILM\nX-T5\nXF16-55mmF2.8 R LM WR\n");
    QCOMPARE(image.captured, QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
    QCOMPARE(image.camera, QStringLiteral("FUJIFILM X-T5"));
    QCOMPARE(image.lens, QStringLiteral("XF16-55mmF2.8 R LM WR"));
    // A record cut short by the producer still parses positionally.
    const MediaMetadataIndex::Details bare = MediaMetadataIndex::parseImageDetails("\n\n");
    QVERIFY(!bare.captured.isValid());
    QVERIFY(bare.camera.isEmpty());
    QVERIFY(bare.lens.isEmpty());
    QCOMPARE(bare, MediaMetadataIndex::Details {});

    const MediaMetadataIndex::Details video = MediaMetadataIndex::parseVideoDetails(
        "{\"format\":{\"tags\":{\"creation_time\":\"2021-08-09T10:11:12\","
        "\"com.apple.quicktime.make\":\"Apple\",\"com.apple.quicktime.model\":\"iPhone 12\"}}}");
    QCOMPARE(video.captured, QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
    QCOMPARE(video.camera, QStringLiteral("Apple iPhone 12"));
    QVERIFY(MediaMetadataIndex::parseVideoDetails("not json").camera.isEmpty());
  }

  void generalMediaDatesAreEnrichedOnceAndProducerDatesAlwaysWin() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_DATE_TEST_LOG");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_DATE_TEST_LOG", previousLog);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("date-runs.log"));
    QFile magick(dir.filePath(QStringLiteral("magick")));
    QVERIFY(magick.open(QIODevice::WriteOnly));
    magick.write("#!/bin/sh\n"
                 "printf 'image\\n' >> \"$OMAROLL_DATE_TEST_LOG\"\n"
                 "index=0\n"
                 "while [ \"$#\" -gt 0 ]; do\n"
                 "  if [ \"$1\" = -format ] && [ \"$#\" -ge 3 ]; then\n"
                 "    printf '\\036OMAROLL_DATE:%s\\n' \"$index\"\n"
                 "    case \"$3\" in\n"
                 "      *undated*) printf '\\n\\n' ;;\n"
                 "      *) printf '2020:05:06 07:08:09\\n\\nApple\\niPhone 12\\n"
                 "iPhone 12 back camera\\n' ;;\n"
                 "    esac\n"
                 "    index=$((index + 1))\n"
                 "    shift 3\n"
                 "  else\n"
                 "    shift\n"
                 "  fi\n"
                 "done\n");
    magick.close();
    QVERIFY(magick.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));

    QFile ffprobe(dir.filePath(QStringLiteral("ffprobe")));
    QVERIFY(ffprobe.open(QIODevice::WriteOnly));
    ffprobe.write("#!/bin/sh\n"
                  "printf '%s\\n' "
                  "'{\"format\":{\"tags\":{\"creation_time\":\"2021-08-09T10:"
                  "11:12\",\"com.apple.quicktime.make\":\"Apple\","
                  "\"com.apple.quicktime.model\":\"iPhone 12\"}}}'\n"
                  "printf 'video\\n' >> \"$OMAROLL_DATE_TEST_LOG\"\n");
    ffprobe.close();
    QVERIFY(ffprobe.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_DATE_TEST_LOG", logPath.toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QString photo = dir.filePath(QStringLiteral("phone photo #1.jpg"));
    const QString capture = dir.filePath(QStringLiteral("screenshot-2026-08-31_10-00-00.png"));
    const QString undated = dir.filePath(QStringLiteral("undated.png"));
    QVERIFY(image.save(photo, "JPG"));
    QVERIFY(image.save(capture, "PNG"));
    QVERIFY(image.save(undated, "PNG"));
    const QString video = dir.filePath(QStringLiteral("family clip.mp4"));
    QFile clip(video);
    QVERIFY(clip.open(QIODevice::WriteOnly));
    clip.write("test video");
    clip.close();

    QFile::remove(MediaMetadataIndex::cachePath());
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), 4);

    const int captureRow = model.rowOf(capture);
    QVERIFY(captureRow >= 0);
    QVERIFY(model.recordAt(captureRow).hasProducerTimestamp);
    QCOMPARE(model.recordAt(captureRow).captured, QDateTime(QDate(2026, 8, 31), QTime(10, 0)));
    const QDateTime undatedFallback = model.recordAt(model.rowOf(undated)).captured;

    {
      MediaMetadataIndex dates(&model);
      QTRY_COMPARE_WITH_TIMEOUT(dates.total(), 3, 2000);
      QTRY_VERIFY_WITH_TIMEOUT(!dates.indexing(), 3000);
      QCOMPARE(dates.completed(), 3);
      QCOMPARE(model.recordAt(model.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
      QCOMPARE(model.recordAt(model.rowOf(video)).captured,
               QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
      QCOMPARE(model.recordAt(model.rowOf(capture)).captured,
               QDateTime(QDate(2026, 8, 31), QTime(10, 0)));
      QCOMPARE(model.recordAt(model.rowOf(undated)).captured, undatedFallback);
      // The same probe fills in the camera, so a photo library can be browsed
      // by camera without a second pass over the files.
      QCOMPARE(model.recordAt(model.rowOf(photo)).camera, QStringLiteral("Apple iPhone 12"));
      QCOMPARE(model.recordAt(model.rowOf(photo)).lens, QStringLiteral("iPhone 12 back camera"));
      QCOMPARE(model.recordAt(model.rowOf(video)).camera, QStringLiteral("Apple iPhone 12"));
      QVERIFY(model.recordAt(model.rowOf(undated)).camera.isEmpty());
      QVERIFY(model.recordAt(model.rowOf(capture)).camera.isEmpty());

      // A result for an older version of the file cannot reorder the live row.
      model.applyMetadata({{photo, 0, 0, QDateTime(QDate(1990, 1, 1), QTime(0, 0)), 0, 0}});
      QCOMPARE(model.recordAt(model.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
      // Embedded metadata can never override a producer timestamp.
      const CaptureRecord& producer = model.recordAt(model.rowOf(capture));
      model.applyMetadata(
          {{capture, producer.modified, producer.bytes, QDateTime(QDate(1990, 1, 1), QTime(0, 0)),
            producer.device, producer.inode}});
      QCOMPARE(model.recordAt(model.rowOf(capture)).captured,
               QDateTime(QDate(2026, 8, 31), QTime(10, 0)));

      // A watcher rescan keeps resolved dates in place instead of briefly
      // restoring mtimes and reshuffling the grid.
      scanned.clear();
      model.refresh();
      QVERIFY(scanned.wait(5000));
      QCOMPARE(model.recordAt(model.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
      QCOMPARE(model.recordAt(model.rowOf(video)).captured,
               QDateTime(QDate(2021, 8, 9), QTime(10, 11, 12)));
      QCOMPARE(model.recordAt(model.rowOf(photo)).camera, QStringLiteral("Apple iPhone 12"));
    }

    QFile firstLog(logPath);
    QVERIFY(firstLog.open(QIODevice::ReadOnly));
    const QByteArray firstRuns = firstLog.readAll();
    firstLog.close();
    QCOMPARE(firstRuns.count("image\n"), 1);
    QCOMPARE(firstRuns.count("video\n"), 1);
    const QFileInfo dateCache(MediaMetadataIndex::cachePath());
    QVERIFY(dateCache.isFile());
    const QFileDevice::Permissions publicPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    QCOMPARE(dateCache.permissions() & publicPermissions, QFileDevice::Permissions());

    // The second index applies its private cache and starts no process. A
    // fresh model gets its cameras from that cache too.
    {
      CaptureModel fresh(&settings);
      QSignalSpy freshScan(&fresh, &CaptureModel::countChanged);
      QVERIFY(freshScan.wait(5000));
      MediaMetadataIndex cached(&fresh);
      QTest::qWait(300);
      QVERIFY(!cached.indexing());
      QCOMPARE(fresh.recordAt(fresh.rowOf(photo)).camera, QStringLiteral("Apple iPhone 12"));
      QCOMPARE(fresh.recordAt(fresh.rowOf(photo)).captured,
               QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
    }
    QFile secondLog(logPath);
    QVERIFY(secondLog.open(QIODevice::ReadOnly));
    QCOMPARE(secondLog.readAll(), firstRuns);
  }

  void imageDatesUseBoundedBatchesAndPathIndexesStayCorrect() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousLog = qgetenv("OMAROLL_DATE_BATCH_LOG");
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    const QByteArray previousPictures = qgetenv("XDG_PICTURES_DIR");
    const QByteArray previousVideos = qgetenv("XDG_VIDEOS_DIR");
    const QByteArray previousShots = qgetenv("OMARCHY_SCREENSHOT_DIR");
    const QByteArray previousRecordings = qgetenv("OMARCHY_SCREENRECORD_DIR");
    const auto restore = qScopeGuard([&] {
      const auto putBack = [](const char* name, const QByteArray& value) {
        value.isNull() ? qunsetenv(name) : qputenv(name, value);
      };
      putBack("PATH", previousPath);
      putBack("OMAROLL_DATE_BATCH_LOG", previousLog);
      putBack("XDG_CACHE_HOME", previousCache);
      putBack("XDG_PICTURES_DIR", previousPictures);
      putBack("XDG_VIDEOS_DIR", previousVideos);
      putBack("OMARCHY_SCREENSHOT_DIR", previousShots);
      putBack("OMARCHY_SCREENRECORD_DIR", previousRecordings);
    });

    const QString logPath = dir.filePath(QStringLiteral("date-batches.log"));
    QFile magick(dir.filePath(QStringLiteral("magick")));
    QVERIFY(magick.open(QIODevice::WriteOnly));
    magick.write("#!/bin/sh\n"
                 "printf 'batch\\n' >> \"$OMAROLL_DATE_BATCH_LOG\"\n"
                 "index=0\n"
                 "status=0\n"
                 "while [ \"$#\" -gt 0 ]; do\n"
                 "  if [ \"$1\" = -format ] && [ \"$#\" -ge 3 ]; then\n"
                 "    case \"$3\" in\n"
                 "      *'photo 32 #'*) status=1 ;;\n"
                 "      *) printf '\\036OMAROLL_DATE:%s\\n2020:05:06 "
                 "07:08:09\\n\\n' \"$index\" ;;\n"
                 "    esac\n"
                 "    index=$((index + 1))\n"
                 "    shift 3\n"
                 "  else\n"
                 "    shift\n"
                 "  fi\n"
                 "done\n"
                 "exit \"$status\"\n");
    magick.close();
    QVERIFY(magick.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMAROLL_DATE_BATCH_LOG", logPath.toUtf8()));
    QVERIFY(qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8()));
    for (const char* name : {"XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "OMARCHY_SCREENSHOT_DIR",
                             "OMARCHY_SCREENRECORD_DIR"}) {
      QVERIFY(qputenv(name, dir.path().toUtf8()));
    }

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::white);
    QStringList paths;
    for (int index = 0; index < 65; ++index) {
      const QString path =
          dir.filePath(QStringLiteral("photo %1 #.png").arg(index, 2, 10, QLatin1Char('0')));
      QVERIFY(image.save(path, "PNG"));
      paths.append(path);
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy scanned(&model, &CaptureModel::countChanged);
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowCount(), paths.size());
    QSignalSpy dateChanges(&model, &QAbstractItemModel::dataChanged);

    {
      MediaMetadataIndex dates(&model);
      QTRY_COMPARE_WITH_TIMEOUT(dates.total(), paths.size(), 2000);
      QTRY_VERIFY_WITH_TIMEOUT(!dates.indexing(), 5000);
      QCOMPARE(dates.completed(), paths.size());
      for (const QString& path : std::as_const(paths)) {
        const int row = model.rowOf(path);
        QVERIFY(row >= 0);
        if (path.endsWith(QStringLiteral("photo 32 #.png"))) {
          QCOMPARE(model.recordAt(row).captured.toMSecsSinceEpoch(), model.recordAt(row).modified);
        } else {
          QCOMPARE(model.recordAt(row).captured, QDateTime(QDate(2020, 5, 6), QTime(7, 8, 9)));
        }
      }
    }
    QCOMPARE(dateChanges.count(), 1);

    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(log.readAll().count("batch\n"), 3);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(proxy.rowOf(paths.first()) >= 0);
    proxy.setSearchText(QStringLiteral("does not exist"));
    QCOMPARE(proxy.rowOf(paths.first()), -1);
    proxy.setSearchText({});

    const QString removed = paths.takeFirst();
    QVERIFY(QFile::remove(removed));
    const QString added = dir.filePath(QStringLiteral("new photo.png"));
    QVERIFY(image.save(added, "PNG"));
    scanned.clear();
    model.refresh();
    QVERIFY(scanned.wait(5000));
    QCOMPARE(model.rowOf(removed), -1);
    QVERIFY(model.rowOf(added) >= 0);
    for (const QString& path : std::as_const(paths)) {
      QVERIFY(model.rowOf(path) >= 0);
    }
  }

  void imageDetailsRunLocallyForAnOddPath() {
    if (QStandardPaths::findExecutable(QStringLiteral("magick")).isEmpty()) {
      QSKIP("ImageMagick not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("odd # photo ?.png"));
    QImage image(37, 23, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(path, "PNG"));
    const qint64 before = QFileInfo(path).size();

    MediaInspector inspector;
    inspector.inspect(path, false);
    QTRY_VERIFY_WITH_TIMEOUT(!inspector.loading(), 5000);
    QCOMPARE(inspector.path(), path);
    QVERIFY(!inspector.lines().isEmpty());
    QVERIFY(inspector.lines().first().startsWith(QStringLiteral("PNG")));
    QCOMPARE(QFileInfo(path).size(), before);
  }

  // The camera, lens, date and shot are read from the raw's own TIFF, in
  // either byte order, behind ORF's and RW2's own magic numbers, and from
  // inside a CR3's boxes or the JPEG at the front of a RAF.
  void cameraRawHeadersGiveOrientationAndExif_data() {
    QTest::addColumn<QByteArray>("file");
    const QByteArray little = cameraTiff(false, rawCameraFields(6), rawShotFields());
    const QByteArray big = cameraTiff(true, rawCameraFields(6), rawShotFields());
    QTest::newRow("little-endian") << little;
    QTest::newRow("big-endian") << big;
    QByteArray olympus = little;
    olympus.replace(2, 2, "RO");
    QTest::newRow("orf") << olympus;
    QByteArray panasonic = little;
    panasonic.replace(2, 2, QByteArray("U\0", 2));
    QTest::newRow("rw2") << panasonic;

    const QByteArray canon = QByteArray::fromHex("85c0b687820f11e08111f4ce462b6a48");
    const QByteArray cmt1 = box("CMT1", cameraTiff(false, rawCameraFields(6)));
    const QByteArray cmt2 = box("CMT2", cameraTiff(false, rawShotFields()));
    QTest::newRow("cr3") << box("ftyp", QByteArray("crx \0\0\0\1crx isom", 16)) +
                                box("moov", box("uuid", canon + box("CNCV", "CanonCR3") +
                                                            cmt1 + cmt2));

    QByteArray jpeg = QByteArray::fromHex("ffd8");
    const QByteArray exif = QByteArray("Exif\0\0", 6) + big;
    const int length = int(exif.size()) + 2;
    jpeg += QByteArray::fromHex("ffe1") + char(length >> 8) + char(length & 0xFF) + exif;
    jpeg += QByteArray::fromHex("ffd9");
    QByteArray fujifilm = QByteArray("FUJIFILMCCD-RAW 0201FF383501").leftJustified(84, '\0');
    const quint32 at = 100;
    for (int index = 0; index < 4; ++index) {
      fujifilm.append(char(at >> (24 - 8 * index)));
    }
    for (int index = 0; index < 4; ++index) {
      fujifilm.append(char(quint32(jpeg.size()) >> (24 - 8 * index)));
    }
    fujifilm = fujifilm.leftJustified(at, '\0') + jpeg;
    QTest::newRow("raf") << fujifilm;
  }

  void cameraRawHeadersGiveOrientationAndExif() {
    QFETCH(QByteArray, file);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("shot.raw"));
    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(file);
    out.close();

    const CameraRaw::Metadata metadata = CameraRaw::readMetadata(path);
    QCOMPARE(metadata.orientation, 6);
    QCOMPARE(metadata.make, QStringLiteral("SONY"));
    QCOMPARE(metadata.model, QStringLiteral("ILCE-7CM2"));
    QCOMPARE(metadata.lens, QStringLiteral("E 28-200mm F2.8-5.6 A071"));
    QCOMPARE(metadata.taken, QDateTime(QDate(2026, 10, 1), QTime(8, 32, 17)));
    QCOMPARE(metadata.exposureTime, QStringLiteral("1/160"));
    QCOMPARE(metadata.fNumber, 3.2);
    QCOMPARE(metadata.focalLength, 28.0);
    QCOMPARE(metadata.iso, 3200);
  }

  // Raws come off cards and downloads: a cut-off or foreign file reads as
  // nothing at all, never as a crash or a stray orientation.
  void cameraRawHeadersSurviveTruncationAndJunk() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cut.arw"));
    const QByteArray whole = cameraTiff(false, rawCameraFields(8), rawShotFields());
    for (qsizetype length = 0; length <= whole.size(); ++length) {
      QFile out(path);
      QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
      out.write(whole.left(length));
      out.close();
      const CameraRaw::Metadata metadata = CameraRaw::readMetadata(path);
      QVERIFY(metadata.orientation >= 1 && metadata.orientation <= 8);
    }
    QCOMPARE(CameraRaw::readMetadata(path).orientation, 8);

    // An IFD pointing past the end, and one claiming a huge entry count.
    QByteArray broken = whole;
    broken.replace(4, 4, QByteArray("\xff\xff\xff\x7f", 4));
    QByteArray crowded = whole;
    crowded.replace(8, 2, QByteArray("\xff\xff", 2));
    for (const QByteArray& junk : {broken, crowded, QByteArray("MMjunk"), QByteArray(4096, 'x')}) {
      QFile out(path);
      QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
      out.write(junk);
      out.close();
      const CameraRaw::Metadata metadata = CameraRaw::readMetadata(path);
      QCOMPARE(metadata.orientation, 1);
      QVERIFY(metadata.make.isEmpty());
    }
    QCOMPARE(CameraRaw::readMetadata(dir.filePath(QStringLiteral("missing.arw"))).orientation, 1);
  }

  // The same for the containers around the TIFF: a CR3's boxes and a RAF's
  // leading JPEG, cut off anywhere, with sizes that overflow or point past the
  // end, and with boxes repeated so the walk cannot multiply.
  void cameraRawContainersSurviveTruncationAndRepeats() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cut.raw"));
    const auto read = [&path](const QByteArray& bytes) {
      QFile out(path);
      if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return CameraRaw::Metadata{};
      out.write(bytes);
      out.close();
      return CameraRaw::readMetadata(path);
    };

    const QByteArray canon = QByteArray::fromHex("85c0b687820f11e08111f4ce462b6a48");
    const QByteArray ftyp = box("ftyp", QByteArray("crx \0\0\0\1crx isom", 16));
    const QByteArray cr3 =
        ftyp + box("moov", box("uuid", canon + box("CMT1", cameraTiff(false, rawCameraFields(6))) +
                                           box("CMT2", cameraTiff(false, rawShotFields()))));
    for (qsizetype length = 0; length <= cr3.size(); ++length) {
      const CameraRaw::Metadata metadata = read(cr3.left(length));
      QVERIFY(metadata.orientation >= 1 && metadata.orientation <= 8);
    }
    QCOMPARE(read(cr3).iso, 3200);

    // A 64-bit size near the limit, and a box that runs to the end of the file.
    QByteArray huge = QByteArray::fromHex("00000001") + "moov" + QByteArray::fromHex("7fffffffffffffff");
    QCOMPARE(read(ftyp + huge + cr3.mid(ftyp.size())).orientation, 1);
    QByteArray open = cr3;
    open.replace(ftyp.size(), 4, QByteArray(4, '\0'));
    QCOMPARE(read(open).orientation, 6);

    // Only the first moov, the first Canon box and the first CMT1 are read.
    const QByteArray first = box("CMT1", cameraTiff(false, rawCameraFields(3)));
    const QByteArray second = box("CMT1", cameraTiff(false, rawCameraFields(6)));
    QCOMPARE(read(ftyp + box("moov", box("uuid", canon + first + second)) +
                  box("moov", box("uuid", canon + second)))
                 .orientation,
             3);

    // A RAF whose JPEG pointer leads past the end, then one cut short.
    QByteArray fujifilm = QByteArray("FUJIFILMCCD-RAW 0201FF383501").leftJustified(84, '\0');
    fujifilm += QByteArray::fromHex("7fffff00") + QByteArray::fromHex("00000010");
    QCOMPARE(read(fujifilm).orientation, 1);
    QByteArray jpeg = QByteArray::fromHex("ffd8");
    const QByteArray exif = QByteArray("Exif\0\0", 6) + cameraTiff(true, rawCameraFields(6));
    const int length = int(exif.size()) + 2;
    jpeg += QByteArray::fromHex("ffe1") + char(length >> 8) + char(length & 0xFF) + exif;
    QByteArray raf = QByteArray("FUJIFILMCCD-RAW 0201FF383501").leftJustified(84, '\0');
    for (const quint32 field : {quint32(100), quint32(jpeg.size())}) {
      for (int index = 0; index < 4; ++index) raf.append(char(field >> (24 - 8 * index)));
    }
    raf = raf.leftJustified(100, '\0') + jpeg;
    for (qsizetype cut = 0; cut <= raf.size(); ++cut) {
      const CameraRaw::Metadata metadata = read(raf.left(cut));
      QVERIFY(metadata.orientation >= 1 && metadata.orientation <= 8);
    }
    QCOMPARE(read(raf).orientation, 6);
  }

  // The preview is turned exactly as Qt turns a JPEG with the same EXIF tag.
  void rawPreviewsTurnAsQtTurnsAJpeg() {
    QImage pattern(6, 4, QImage::Format_RGB32);
    for (int y = 0; y < pattern.height(); ++y) {
      for (int x = 0; x < pattern.width(); ++x) {
        pattern.setPixel(x, y, qRgb(40 * x, 60 * y, x == 0 && y == 0 ? 255 : 0));
      }
    }
    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(pattern.save(&buffer, "JPEG", 100));
    for (quint16 orientation = 1; orientation <= 8; ++orientation) {
      QByteArray jpeg = withExifOrientation(buffer.data(), orientation);
      QBuffer stored(&jpeg);
      QImageReader plain(&stored, "jpeg");
      plain.setAutoTransform(false);
      const QImage unturned = plain.read();
      QBuffer turnedBuffer(&jpeg);
      QImageReader turned(&turnedBuffer, "jpeg");
      turned.setAutoTransform(true);
      const QImage expected = turned.read().convertToFormat(QImage::Format_RGB32);
      QCOMPARE(CameraRaw::upright(unturned, orientation).convertToFormat(QImage::Format_RGB32),
               expected);
    }
  }

  // A raw is listed, measured, previewed upright, decoded upright, described
  // and dated without ImageMagick, and never written to.
  void cameraRawsAreListedPreviewedAndDescribed() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) {
      QSKIP("Qt has no camera raw decoder here (kimageformats with LibRaw)");
    }
    QVERIFY(CaptureScanner::isImage(QStringLiteral("ARW")));
    QVERIFY(CaptureScanner::isImage(QStringLiteral("cr3")));
    QVERIFY(CaptureScanner::isSupported(QStringLiteral("nef")));
    QVERIFY(!CaptureScanner::isSupported(QStringLiteral("raw")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString camera = dir.filePath(QStringLiteral("DSC00042.DNG"));
    const QString bare = dir.filePath(QStringLiteral("no-preview.dng"));
    QVERIFY(QFile::copy(rawFixture(QStringLiteral("camera.dng")), camera));
    QVERIFY(QFile::copy(rawFixture(QStringLiteral("no-preview.dng")), bare));
    const QByteArray before = fileHash(camera);

    const auto records = CaptureScanner::scan({{dir.path(), 1}});
    QCOMPARE(records.size(), 2);
    for (const CaptureRecord& record : records) {
      QVERIFY(record.isRaw());
      QVERIFY(!record.isVideo());
      QCOMPARE(record.kind, CaptureRecord::Picture);
    }
    QVERIFY(ViewerSession::canOpen({camera, bare}));

    // Tagged to turn a quarter clockwise: the bright top half ends up on
    // the right, whether the picture is the embedded preview, the plugin's
    // half-size decode of a raw without one, or the full demosaic.
    const auto brightOnTheRight = [](const QImage& image) {
      return qGray(image.pixel(image.width() - 1, image.height() / 2)) >
             qGray(image.pixel(0, image.height() / 2)) + 60;
    };
    for (const QString& path : {camera, bare}) {
      QCOMPARE(CameraRaw::fullSize(path), QSize(48, 64));
      const QImage preview = CameraRaw::readPreview(path);
      QCOMPARE(preview.size(), QSize(24, 32));
      QVERIFY2(brightOnTheRight(preview), qPrintable(path));
      const QImage full = CameraRaw::readFull(path);
      QCOMPARE(full.size(), QSize(48, 64));
      QVERIFY2(brightOnTheRight(full), qPrintable(path));
    }

    const QImage tile = ThumbnailCache::thumbnail(camera, QSize(12, 12), 1.0);
    QVERIFY(!tile.isNull());
    QVERIFY(tile.height() >= tile.width());

    AppSettings settings;
    CaptureModel model(&settings);
    const QUrl url = model.fileUrl(camera);
    QCOMPARE(url.scheme(), QStringLiteral("image"));
    QCOMPARE(url.host(), QStringLiteral("raw"));
    QVERIFY(model.fileUrl(dir.filePath(QStringLiteral("x.png"))).isLocalFile());
    QSignalSpy sized(&model, &CaptureModel::rawSizeRead);
    model.readRawSize(camera, url);
    QTRY_COMPARE(sized.count(), 1);
    QCOMPARE(sized.at(0).at(0).toString(), camera);
    QCOMPARE(sized.at(0).at(1).toUrl(), url);
    QCOMPARE(sized.at(0).at(2).toSize(), QSize(48, 64));

    // A file replaced with the same mtime, as cp -p or rsync -a leave it, is
    // a new URL, so Qt's cache cannot serve the old preview.
    const QUrl previous = CameraRaw::previewUrl(camera);
    const QDateTime stamp = QFileInfo(camera).lastModified();
    const QString replacement = dir.filePath(QStringLiteral("replacement.dng"));
    QVERIFY(QFile::copy(camera, replacement));
    QVERIFY(QFile::remove(camera));
    QVERIFY(QFile::rename(replacement, camera));
    {
      QFile touched(camera);
      QVERIFY(touched.open(QIODevice::ReadWrite));
      QVERIFY(touched.setFileTime(stamp, QFileDevice::FileModificationTime));
    }
    QCOMPARE(QFileInfo(camera).lastModified(), stamp);
    QVERIFY(CameraRaw::previewUrl(camera) != previous);

    QCOMPARE(MediaInspector::describeRaw(camera),
             QStringList({QStringLiteral("DNG"),
                          QStringLiteral("Taken  ·  ") +
                              QLocale::system().toString(
                                  QDateTime(QDate(2026, 9, 30), QTime(14, 5, 6)),
                                  QLocale::ShortFormat),
                          QStringLiteral("Camera  ·  Omaroll Test Camera"),
                          QStringLiteral("Lens  ·  Test Lens 35mm F2.8"),
                          QStringLiteral("f/2.8  ·  1/125 s  ·  ISO 400  ·  35 mm")}));
    MediaInspector inspector;
    inspector.inspect(camera, false);
    QTRY_VERIFY_WITH_TIMEOUT(!inspector.loading(), 5000);
    QCOMPARE(inspector.lines().value(2), QStringLiteral("Camera  ·  Omaroll Test Camera"));

    const MediaMetadataIndex::Details details = MediaMetadataIndex::rawDetails(camera);
    QCOMPARE(details.captured, QDateTime(QDate(2026, 9, 30), QTime(14, 5, 6)));
    QCOMPARE(details.camera, QStringLiteral("Omaroll Test Camera"));
    QCOMPARE(details.lens, QStringLiteral("Test Lens 35mm F2.8"));

    QCOMPARE(fileHash(camera), before);
  }

  // Tools that open the file themselves cannot read a raw; the actions that
  // go through Omaroll's own decoder, or only move the file, stay.
  void cameraRawsOfferOnlyActionsThatCanOpenThem() {
    if (!CameraRaw::isRaw(QStringLiteral("arw"))) {
      QSKIP("Qt has no camera raw decoder here (kimageformats with LibRaw)");
    }
    ActionRegistry registry(nullptr);
    const QString raw = QStringLiteral("/tmp/DSC00042.ARW");
    QStringList offered;
    for (const QVariant& row : registry.actionsForKind(false, false, raw)) {
      offered << row.toMap().value(QStringLiteral("id")).toString();
    }
    for (const QString& id :
         {QStringLiteral("annotate"), QStringLiteral("omaframe"), QStringLiteral("ocr"), QStringLiteral("qr"),
          QStringLiteral("edit"), QStringLiteral("view"), QStringLiteral("background"),
          QStringLiteral("print"), QStringLiteral("corrections"),
          QStringLiteral("correctionsbatch")}) {
      QVERIFY2(!offered.contains(id), qPrintable(id));
      QVERIFY2(!registry.appliesToKind(id, false, false, raw), qPrintable(id));
      QVERIFY2(registry.appliesToKind(id, false, false, QStringLiteral("/tmp/DSC00042.JPG")),
               qPrintable(id));
    }
    QVERIFY(offered.contains(QStringLiteral("develop")));
    QVERIFY(registry.appliesToKind(QStringLiteral("develop"), false, false, raw));
    QVERIFY(!registry.appliesToKind(QStringLiteral("develop"), false, false,
                                  QStringLiteral("/tmp/DSC00042.JPG")));
    for (const QString& id :
         {QStringLiteral("matte"), QStringLiteral("compare"), QStringLiteral("export"),
          QStringLiteral("copy"), QStringLiteral("send"), QStringLiteral("files"),
          QStringLiteral("rename"), QStringLiteral("favorite"), QStringLiteral("trash")}) {
      QVERIFY2(offered.contains(id), qPrintable(id));
    }
  }

  void existingImagesOpenInOmaframeWithoutChangingTheAnnotationEditor() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const bool editorWasSet = qEnvironmentVariableIsSet("OMARCHY_SCREENSHOT_EDITOR");
    const QByteArray previousEditor = qgetenv("OMARCHY_SCREENSHOT_EDITOR");
    const auto restoreEnvironment = qScopeGuard([&] {
      qputenv("PATH", previousPath);
      if (editorWasSet) qputenv("OMARCHY_SCREENSHOT_EDITOR", previousEditor);
      else qunsetenv("OMARCHY_SCREENSHOT_EDITOR");
    });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_EDITOR", "configured-annotation-editor"));

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QVERIFY(!registry.available(QStringLiteral("omaframe")));
    QVERIFY(!registry.run(QStringLiteral("omaframe"), dir.filePath(QStringLiteral("photo.png"))));
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("omaframe")));
    QVERIFY(registry.appliesToKind(QStringLiteral("omaframe"), false, false,
                                  dir.filePath(QStringLiteral("photo.png"))));
    QVERIFY(!registry.appliesToKind(QStringLiteral("omaframe"), true, false));
    QVERIFY(!registry.appliesToKind(QStringLiteral("omaframe"), false, true));

    QFile handler(dir.filePath(QStringLiteral("omaframe")));
    QVERIFY(handler.open(QIODevice::WriteOnly));
    handler.write("#!/bin/sh\nprintf '%s\\n%s\\n' \"$#\" \"$1\" > \"$1.args\"\n");
    handler.close();
    QVERIFY(handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    const QString source = dir.filePath(QStringLiteral("photo # ' ; $(touch injected) 雪.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(source, "PNG"));
    const QByteArray before = fileHash(source);
    QVERIFY(registry.available(QStringLiteral("omaframe")));
    QVERIFY(!registry.available(QStringLiteral("annotate")));
    QCOMPARE(registry.shortcutFor(QStringLiteral("annotate")), QStringLiteral("A"));
    QVERIFY(registry.run(QStringLiteral("omaframe"), source));
    const auto recordedArguments = [&source] {
      QFile arguments(source + QStringLiteral(".args"));
      return arguments.open(QIODevice::ReadOnly) ? arguments.readAll() : QByteArray();
    };
    QTRY_COMPARE(recordedArguments(), QByteArray("1\n") + source.toUtf8() + '\n');
    QCOMPARE(fileHash(source), before);
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("injected"))));
    QCOMPARE(qgetenv("OMARCHY_SCREENSHOT_EDITOR"), QByteArray("configured-annotation-editor"));
    for (const QVariant& value : registry.actionsForKind(false, false, source)) {
      const QVariantMap row = value.toMap();
      if (row.value(QStringLiteral("id")) == QStringLiteral("omaframe")) {
        QCOMPARE(row.value(QStringLiteral("label")).toString(), QStringLiteral("Open in Omaframe"));
        QVERIFY(row.value(QStringLiteral("available")).toBool());
        return;
      }
    }
    QFAIL("Existing images need an Open in Omaframe action");
  }

  // Corrections save a copy in the source's format, and a raw cannot be
  // written; nothing is flattened into a stand-in either.
  void correctionsRefuseCameraRaws() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) {
      QSKIP("Qt has no camera raw decoder here (kimageformats with LibRaw)");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString raw = dir.filePath(QStringLiteral("shot.dng"));
    QVERIFY(QFile::copy(rawFixture(QStringLiteral("camera.dng")), raw));
    const QString png = dir.filePath(QStringLiteral("plain.png"));
    QVERIFY(QImage(20, 10, QImage::Format_RGB32).save(png, "PNG"));

    ImageEditor editor;
    QSignalSpy failed(&editor, &ImageEditor::failed);
    QSignalSpy finished(&editor, &ImageEditor::batchFinished);
    editor.saveCopy(raw, 1, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("Camera raws are not corrected here"));
    editor.saveCopies({raw, png}, 1, false, false, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QCOMPARE(finished.first().at(0).toInt(), 1);
    QCOMPARE(finished.first().at(1).toInt(), 1);
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("shot-edited*")}, QDir::Files),
             QStringList());
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

  void marksSurviveARootGoingAway() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("profile")));
    const auto restoreProfile = qScopeGuard([this] {
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                         m_scratch.filePath(QStringLiteral("config")));
    });
    QMap<QByteArray, QByteArray> environment;
    const auto restoreEnvironment = qScopeGuard([&] {
      for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        it.value().isNull() ? qunsetenv(it.key().constData())
                            : qputenv(it.key().constData(), it.value());
      }
    });
    for (const char* name : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                             "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR"}) {
      environment.insert(name, qgetenv(name));
      QVERIFY(qputenv(name, dir.filePath(QStringLiteral("automatic")).toUtf8()));
    }

    const QString root = dir.filePath(QStringLiteral("mounted"));
    const QString offline = dir.filePath(QStringLiteral("offline"));
    QVERIFY(QDir().mkpath(root));
    const QString original = root + QStringLiteral("/kept.png");
    const QString backup = dir.filePath(QStringLiteral("offline-backup.json"));
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(original, "PNG"));
    {
      AppSettings settings;
      settings.setScanDownloads(false);
      QVERIFY(settings.addLibraryFolder(QUrl::fromLocalFile(root)));
      settings.setFavorite({original}, true);
      settings.setHidden({original}, true);
      settings.setRating({original}, 4);
      settings.setCaption(original, QStringLiteral("Remember offline"));
      CaptureModel model(&settings);
      QTRY_VERIFY_WITH_TIMEOUT(!model.scanning(), 5000);
      QCOMPARE(model.rowCount(), 1);

      // Simulate an unmounted volume, including failed existence checks.
      QVERIFY(QDir().rename(root, offline));
      QVERIFY(!QFileInfo::exists(original));
      model.refresh();
      QTRY_VERIFY_WITH_TIMEOUT(!model.scanning(), 5000);
      QCOMPARE(model.rowCount(), 0);
      QVERIFY(settings.isFavorite(original));
      QVERIFY(settings.isHidden(original));
      QCOMPARE(settings.rating(original), 4);
      QCOMPARE(settings.caption(original), QStringLiteral("Remember offline"));
      QVERIFY(settings.exportOrganization(backup).value(QStringLiteral("ok")).toBool());
    }
    {
      // Reload persisted organization while the volume is still unavailable.
      AppSettings restored;
      CaptureModel model(&restored);
      QTRY_VERIFY_WITH_TIMEOUT(!model.scanning(), 5000);
      QCOMPARE(model.rowCount(), 0);
      QVERIFY(restored.isFavorite(original));
      QVERIFY(restored.isHidden(original));
      QCOMPARE(restored.rating(original), 4);
      QCOMPARE(restored.caption(original), QStringLiteral("Remember offline"));
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       dir.filePath(QStringLiteral("fresh-profile")));
    QVERIFY(qputenv("XDG_PICTURES_DIR", root.toUtf8()));
    {
      AppSettings restored;
      restored.setScanDownloads(false);
      QVERIFY(restored.markedPaths().isEmpty());
      QVERIFY(restored.importOrganization(backup).value(QStringLiteral("ok")).toBool());
      CaptureModel model(&restored);
      QTRY_VERIFY_WITH_TIMEOUT(!model.scanning(), 5000);
      QCOMPARE(model.rowCount(), 0);
      QVERIFY(restored.isFavorite(original));
      QVERIFY(restored.isHidden(original));
      QCOMPARE(restored.rating(original), 4);
      QCOMPARE(restored.caption(original), QStringLiteral("Remember offline"));

      // Return under a new filename. Recovery needs the preserved identity,
      // since the original path never becomes available to backfill one.
      QVERIFY(QFile::rename(offline + QStringLiteral("/kept.png"),
                            offline + QStringLiteral("/renamed.png")));
      QVERIFY(QDir().rename(offline, root));
      const QString moved = root + QStringLiteral("/renamed.png");
      model.refresh();
      QTRY_VERIFY_WITH_TIMEOUT(!model.scanning(), 5000);
      QCOMPARE(model.rowCount(), 1);
      QVERIFY(!restored.markedPaths().contains(original));
      QVERIFY(model.rowOf(moved) >= 0);
      const CaptureRecord& record = model.recordAt(model.rowOf(moved));
      QVERIFY(record.favorite);
      QVERIFY(record.hidden);
      QCOMPARE(record.rating, 4);
      QCOMPARE(record.caption, QStringLiteral("Remember offline"));
    }
  }

  void scanSkipsTheRecorderTransients() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto touch = [&](const QString& name) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("x");
    };
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00-preview.png"));
    touch(QStringLiteral("screenrecording-2026-09-01_10-00-00-processed.mp4"));

    const auto records =
        CaptureScanner::scan({{dir.path(), 1, CaptureRecord::Picture, CaptureRecord::Video}});
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().kind, CaptureRecord::Recording);
  }

  void rescanIsADiffNotAReset() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(dir.filePath(QStringLiteral("a.png")), "PNG"));
    QVERIFY(image.save(dir.filePath(QStringLiteral("b.png")), "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 2);

    // One gone, one new: rows are removed and inserted, never reset, so the
    // grid keeps its scroll position and selection across a live update.
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removes(&model, &QAbstractItemModel::rowsRemoved);
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("a.png"))));
    QVERIFY(image.save(dir.filePath(QStringLiteral("c.png")), "PNG"));
    model.refresh();
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(inserts.size(), 1);
    QCOMPARE(removes.size(), 1);
    QCOMPARE(model.pathAt(0), dir.filePath(QStringLiteral("c.png")));
  }

  void fragmentedLargeRemovalUsesBoundedReset() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString seed = dir.filePath(QStringLiteral("item-000.png"));
    QVERIFY(image.save(seed, "PNG"));
    for (int index = 1; index < 600; ++index) {
      QVERIFY(QFile::copy(
          seed, dir.filePath(QStringLiteral("item-%1.png").arg(index, 3, 10, QLatin1Char('0')))));
    }

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 600);

    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy removes(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy folderChanges(&proxy, &CaptureFilterModel::foldersChanged);
    for (int row = 0; row < model.rowCount(); row += 2) {
      QVERIFY(QFile::remove(model.pathAt(row)));
    }

    count.clear();
    model.refresh();
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 300);
    QCOMPARE(proxy.rowCount(), 300);
    QCOMPARE(resets.size(), 1);
    QCOMPARE(removes.size(), 0);
    QTRY_COMPARE(proxy.folderItemCount(dir.path()), 300);
    QCOMPARE(folderChanges.size(), 1);
  }

  void heldPathsStayOutOfTheLibraryUntilReleased() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(dir.filePath(QStringLiteral("done.png")), "PNG"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 1);

    // A transcode's output mid-write: held, so the scan leaves it out.
    const QString partial = dir.filePath(QStringLiteral("done-720p.gif"));
    model.holdPath(partial);
    QFile file(partial);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
    file.close();
    model.refresh();
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.rowOf(partial), -1);

    // Released once the tool finishes, and the rescan it schedules brings the
    // finished file in without being told to.
    model.releasePath(partial);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 5000);
    QVERIFY(model.rowOf(partial) >= 0);
  }

  void nestedFoldersUpdateWhileTheAppIsOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString album = dir.filePath(QStringLiteral("Trips/Autumn"));
    QVERIFY(QDir().mkpath(album));
    QVERIFY(qputenv("OMARCHY_SCREENSHOT_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("OMARCHY_SCREENRECORD_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_PICTURES_DIR", dir.path().toUtf8()));
    QVERIFY(qputenv("XDG_VIDEOS_DIR", dir.path().toUtf8()));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QSignalSpy count(&model, &CaptureModel::countChanged);
    QVERIFY(count.wait(5000));
    QCOMPARE(model.rowCount(), 0);

    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::yellow);
    QVERIFY(image.save(QDir(album).filePath(QStringLiteral("new-photo.png")), "PNG"));
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1, 5000);
  }

  void uriListIsEncodedAndCrlfTerminated() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    const QString list =
        model.uriList({QStringLiteral("/tmp/a b.png"), QStringLiteral("/tmp/c#d.mp4")});
    QCOMPARE(list, QStringLiteral("file:///tmp/a%20b.png\r\nfile:///tmp/c%23d.mp4\r\n"));
  }

  // --- Actions ----------------------------------------------------------

  void registryKeysOnMedium() {
    ActionRegistry registry(nullptr);
    QCOMPARE(registry.primaryActionFor(true), QStringLiteral("trim"));
    QCOMPARE(registry.primaryActionFor(false), QStringLiteral("matte"));
    QCOMPARE(registry.shortcutFor(QStringLiteral("matte")), QStringLiteral("M"));
    QCOMPARE(registry.shortcutFor(QStringLiteral("hide")), QStringLiteral("Ctrl+H"));
    QVERIFY(registry.shortcutFor(QStringLiteral("missing")).isEmpty());
    // The public single-path overload must dispatch to the path-list overload,
    // not resolve its braced argument back to itself and recurse forever.
    QVERIFY(!registry.run(QStringLiteral("favorite"), QStringLiteral("/tmp/not-a-capture")));

    QStringList moving;
    for (const QVariant& row : registry.actionsFor(true)) {
      moving << row.toMap().value(QStringLiteral("id")).toString();
    }
    QVERIFY(moving.contains(QStringLiteral("play")));
    QVERIFY(moving.contains(QStringLiteral("frame")));
    QVERIFY(moving.contains(QStringLiteral("copy")));
    QVERIFY(moving.contains(QStringLiteral("export")));
    QVERIFY(moving.contains(QStringLiteral("rename")));
    QVERIFY(!moving.contains(QStringLiteral("gif")));
    QVERIFY(!moving.contains(QStringLiteral("shrink")));
    QVERIFY(!moving.contains(QStringLiteral("matte")));
    QVERIFY(!moving.contains(QStringLiteral("ocr")));
    QVERIFY(!moving.contains(QStringLiteral("background")));

    QStringList still;
    QString copyLabel;
    for (const QVariant& row : registry.actionsFor(false)) {
      const QVariantMap values = row.toMap();
      still << values.value(QStringLiteral("id")).toString();
      if (values.value(QStringLiteral("id")).toString() == QStringLiteral("copy")) {
        copyLabel = values.value(QStringLiteral("label")).toString();
      }
    }
    QVERIFY(still.contains(QStringLiteral("background")));
    QVERIFY(still.contains(QStringLiteral("export")));
    QVERIFY(still.contains(QStringLiteral("rename")));
    QVERIFY(!still.contains(QStringLiteral("frame")));
    QVERIFY(!still.contains(QStringLiteral("convert")));
    QCOMPARE(copyLabel, QStringLiteral("Copy image"));

    QStringList documents;
    for (const QVariant& row : registry.actionsForKind(false, true)) {
      documents << row.toMap().value(QStringLiteral("id")).toString();
    }
    QCOMPARE(registry.primaryActionForKind(false, true), QStringLiteral("open-document"));
    QVERIFY(documents.contains(QStringLiteral("open-document")));
    QVERIFY(documents.contains(QStringLiteral("send")));
    QVERIFY(documents.contains(QStringLiteral("rename")));
    QVERIFY(documents.contains(QStringLiteral("trash")));
    QVERIFY(!documents.contains(QStringLiteral("matte")));
    QVERIFY(!documents.contains(QStringLiteral("export")));
    QVERIFY(!documents.contains(QStringLiteral("copy")));
  }

  void backgroundActionUsesTheOmarchyHandlerEndToEnd() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    const QByteArray previousLog = qgetenv("OMAROLL_TEST_LOG");
    const auto restoreLog = qScopeGuard([&] {
      previousLog.isNull() ? qunsetenv("OMAROLL_TEST_LOG")
                           : qputenv("OMAROLL_TEST_LOG", previousLog);
    });

    QFile handler(dir.filePath(QStringLiteral("omarchy-theme-bg-set")));
    QVERIFY(handler.open(QIODevice::WriteOnly));
    handler.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$OMAROLL_TEST_LOG\"\n");
    handler.close();
    QVERIFY(handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    const QString logPath = dir.filePath(QStringLiteral("background.log"));
    QVERIFY(qputenv("OMAROLL_TEST_LOG", logPath.toUtf8()));

    const QString imagePath = dir.filePath(QStringLiteral("photo #1.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));

    ActionLauncher launcher;
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    ActionRegistry registry(&launcher);
    QVERIFY(registry.available(QStringLiteral("background")));
    QVERIFY(registry.appliesTo(QStringLiteral("background"), false));
    QVERIFY(!registry.appliesTo(QStringLiteral("background"), true));
    QVERIFY(registry.run(QStringLiteral("background"), imagePath));
    QCOMPARE(reported.size(), 1);
    QCOMPARE(reported.first().first().toString(), QStringLiteral("Set as current background"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(logPath), 1000);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(log.readAll()), imagePath + QLatin1Char('\n'));
  }

  void renamePreservesTheExtensionAndMovesLibraryState() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString original = dir.filePath(QStringLiteral("capture #1.PNG"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(original, "PNG"));
    const QString occupied = dir.filePath(QStringLiteral("occupied.PNG"));
    QVERIFY(image.save(occupied, "PNG"));

    const QString album = QStringLiteral("Rename test");
    AppSettings settings;
    settings.deleteAlbum(album);
    QVERIFY(settings.createAlbum(album));
    QVERIFY(settings.addToAlbum(album, {original}));
    settings.toggleFavorite(original);
    settings.toggleHidden(original);
    settings.setRating({original}, 4);
    QCOMPARE(settings.rating(original), 4);
    QCOMPARE(settings.ratedCount(), 1);

    settings.setCaption(original, QStringLiteral("  Boarding pass  "));
    QCOMPARE(settings.caption(original), QStringLiteral("Boarding pass"));

    ActionLauncher launcher;
    QVERIFY(!launcher.renameFile(original, QString()).value(QStringLiteral("ok")).toBool());
    QVERIFY(!launcher.renameFile(original, QStringLiteral("bad/name"))
                 .value(QStringLiteral("ok"))
                 .toBool());
    const QVariantMap collision = launcher.renameFile(original, QStringLiteral("occupied"));
    QVERIFY(!collision.value(QStringLiteral("ok")).toBool());
    QCOMPARE(collision.value(QStringLiteral("error")).toString(),
             QStringLiteral("A file with that name already exists"));

    const QVariantMap renamed = launcher.renameFile(original, QStringLiteral("useful name"));
    QVERIFY(renamed.value(QStringLiteral("ok")).toBool());
    const QString newPath = dir.filePath(QStringLiteral("useful name.PNG"));
    QCOMPARE(renamed.value(QStringLiteral("path")).toString(), newPath);
    QCOMPARE(renamed.value(QStringLiteral("fileName")).toString(),
             QStringLiteral("useful name.PNG"));
    QVERIFY(!QFileInfo::exists(original));
    QVERIFY(QFileInfo::exists(newPath));

    settings.relocatePath(original, newPath);
    QVERIFY(settings.isFavorite(newPath));
    QVERIFY(settings.isHidden(newPath));
    QCOMPARE(settings.rating(newPath), 4);
    QCOMPARE(settings.rating(original), 0);

    QCOMPARE(settings.caption(newPath), QStringLiteral("Boarding pass"));
    QVERIFY(settings.caption(original).isEmpty());
    QCOMPARE(settings.albumPaths(album), QStringList {newPath});
    AppSettings restored;
    QVERIFY(restored.isFavorite(newPath));
    QVERIFY(restored.isHidden(newPath));
    QCOMPARE(restored.rating(newPath), 4);
    QCOMPARE(restored.albumPaths(album), QStringList {newPath});

    // Out-of-range stars clamp, and zero forgets the entry entirely.
    settings.setRating({newPath}, 9);
    QCOMPARE(settings.rating(newPath), 5);
    settings.setRating({newPath}, 0);
    QCOMPARE(settings.rating(newPath), 0);
    QCOMPARE(settings.ratedCount(), 0);
    QVERIFY(!settings.markedPaths().contains(newPath) || settings.isFavorite(newPath));


    QCOMPARE(restored.caption(newPath), QStringLiteral("Boarding pass"));
    QCOMPARE(restored.albumPaths(album), QStringList {newPath});

    settings.setCaption(newPath, QString());
    QVERIFY(settings.caption(newPath).isEmpty());
    settings.toggleFavorite(newPath);
    settings.toggleHidden(newPath);
    settings.deleteAlbum(album);
  }

  void exportChoicesReachTheTranscoderForEverySelectedFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    QFile handler(dir.filePath(QStringLiteral("omarchy-transcode")));
    QVERIFY(handler.open(QIODevice::WriteOnly));
    handler.write("#!/bin/sh\n"
                  "printf '%s\\n%s\\n%s\\n' \"$1\" \"$2\" \"$3\" > \"$1.args\"\n"
                  "out=\"${1%.*}-$3.$2\"\n"
                  "printf converted > \"$out\"\n");
    handler.close();
    QVERIFY(handler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));

    const QString first = dir.filePath(QStringLiteral("first photo.png"));
    const QString second = dir.filePath(QStringLiteral("second # photo.webp"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(first, "PNG"));
    QVERIFY(image.save(second, "WEBP"));

    ActionLauncher launcher;
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    ActionRegistry registry(&launcher);
    QVERIFY(registry.runBatchWith(QStringLiteral("export"),
                                  {{QStringLiteral("format"), QStringLiteral("png")},
                                   {QStringLiteral("resolution"), QStringLiteral("low")}},
                                  {first, second}));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 2, 3000);

    for (const QString& path : {first, second}) {
      QFile arguments(path + QStringLiteral(".args"));
      QVERIFY(arguments.open(QIODevice::ReadOnly));
      QCOMPARE(QString::fromUtf8(arguments.readAll()), path + QStringLiteral("\npng\nlow\n"));
      const QString output = QFileInfo(path).absolutePath() + QLatin1Char('/') +
                             QFileInfo(path).completeBaseName() + QStringLiteral("-low.png");
      QFile converted(output);
      QVERIFY(converted.open(QIODevice::ReadOnly));
      QCOMPARE(converted.readAll(), QByteArray("converted"));
    }
  }

  void currentVideoFrameIsSavedAtTheChosenPosition() {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
      QSKIP("ffmpeg is not installed");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString video = dir.filePath(QStringLiteral("short clip.mp4"));
    QProcess encoder;
    encoder.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                           QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                           QStringLiteral("-i"), QStringLiteral("color=c=red:s=160x90:d=1:r=10"),
                           QStringLiteral("-c:v"), QStringLiteral("libx264"),
                           QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), video});
    QVERIFY(encoder.waitForFinished(10000));
    QCOMPARE(encoder.exitStatus(), QProcess::NormalExit);
    QCOMPARE(encoder.exitCode(), 0);

    ActionLauncher launcher;
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    ActionRegistry registry(&launcher);
    QVERIFY(registry.runBatchWith(QStringLiteral("frame"),
                                  {{QStringLiteral("seek"), QStringLiteral("0.500")},
                                   {QStringLiteral("frame"), QStringLiteral("00m00s500")}},
                                  {video}));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 5000);
    QVERIFY(settled.first().at(1).toBool());

    const QString output = dir.filePath(QStringLiteral("short clip-frame-00m00s500.png"));
    QCOMPARE(settled.first().at(0).toString(), output);
    const QImage frame(output);
    QVERIFY(!frame.isNull());
    QCOMPARE(frame.size(), QSize(160, 90));
    QVERIFY(QFileInfo(video).exists());
  }

  void clipboardFailuresAreReportedAndSecretsStayOutOfHistory() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    const auto script = [&](const QString& name, const QByteArray& body) {
      QFile file(dir.filePath(name));
      if (!file.open(QIODevice::WriteOnly)) {
        return false;
      }
      file.write("#!/bin/sh\n");
      file.write(body);
      file.close();
      return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
    };
    QVERIFY(script(QStringLiteral("wl-copy"), "exit 1\n"));
    QVERIFY(script(QStringLiteral("zbarimg"), "printf 'otpauth://private'\n"));
    QVERIFY(script(QStringLiteral("omarchy-menu-share"),
                   "printf '%s\\n' \"$@\" > \"$OMAROLL_TEST_LOG\"\n"));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    const QString logPath = dir.filePath(QStringLiteral("arguments.log"));
    const QByteArray previousLog = qgetenv("OMAROLL_TEST_LOG");
    const auto restoreLog = qScopeGuard([&] {
      previousLog.isNull() ? qunsetenv("OMAROLL_TEST_LOG")
                           : qputenv("OMAROLL_TEST_LOG", previousLog);
    });
    QVERIFY(qputenv("OMAROLL_TEST_LOG", logPath.toUtf8()));

    const QString imagePath = dir.filePath(QStringLiteral("capture.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(imagePath, "PNG"));

    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QVERIFY(launcher.copyFile(imagePath)); // accepted, not yet copied
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
    QCOMPARE(reported.size(), 0);

    failed.clear();
    QGuiApplication::clipboard()->setText(QStringLiteral("unchanged"));
    ActionRegistry registry(&launcher);
    QVERIFY(registry.run(QStringLiteral("qr"), imagePath));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1000);
    QCOMPARE(reported.size(), 0);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("unchanged"));

    const QStringList paths = {QStringLiteral("/tmp/first capture.png"),
                               QStringLiteral("/tmp/second # capture.mp4")};
    QVERIFY(registry.runBatch(QStringLiteral("send"), paths));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(logPath), 1000);
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(log.readAll()),
             QStringLiteral("file\n/tmp/first capture.png\n/tmp/second # capture.mp4\n"));
  }

  void trackedRunsReportSettleAndKeepNonemptyOutputAfterFailure() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy pending(&launcher, &ActionLauncher::outputPending);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);

    // Success: pending while the tool runs, settled saved, and the user is
    // told where the file went.
    const QString saved = dir.filePath(QStringLiteral("clip-720p.gif"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"), QStringLiteral("printf data > '%1'").arg(saved)}, {}, saved));
    QVERIFY(launcher.isPending(saved));
    QCOMPARE(pending.size(), 1);
    QVERIFY(reported.last().at(0).toString().contains(QStringLiteral("Making clip-720p.gif")));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
    QCOMPARE(settled.last().at(0).toString(), saved);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QVERIFY(!launcher.isPending(saved));
    QVERIFY(reported.last().at(0).toString().contains(
        QStringLiteral("Saved clip-720p.gif beside the original")));
    QCOMPARE(failed.size(), 0);

    // While a run is in flight, asking again says so rather than lying that
    // the partial file means the job is already done.
    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    const QString busy =
        dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00-720p.gif"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"), QStringLiteral("sleep 0.3 && printf data > '%1'").arg(busy)}, {},
        busy));
    ActionRegistry registry(&launcher);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QVERIFY(reported.last().at(0).toString().contains(QStringLiteral("Still working on")));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 2, 3000);
    QCOMPARE(settled.last().at(1).toBool(), true);

    // Failure after writing: retain the output for review and report the
    // tool's last words rather than a bare exit code.
    const QString broken = dir.filePath(QStringLiteral("clip-1080p.mp4"));
    QVERIFY(launcher.runTracked(
        QStringLiteral("sh"),
        {QStringLiteral("-c"),
         QStringLiteral("printf junk > '%1'; echo boom >&2; exit 1").arg(broken)},
        {}, broken));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 3, 3000);
    QCOMPARE(settled.last().at(1).toBool(), false);
    QVERIFY(QFileInfo::exists(broken));
    QVERIFY(failed.last().at(0).toString().contains(QStringLiteral("Kept clip-1080p.mp4")));
    QVERIFY(failed.last().at(0).toString().contains(QStringLiteral("boom")));
  }

  void completedMediaSurvivesAncillaryHelperFailure() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fixture = dir.filePath(QStringLiteral("source.png"));
    const QString output = dir.filePath(QStringLiteral("copy.png"));
    QImage image(20, 16, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(fixture));
    ActionLauncher launcher;
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QVERIFY(launcher.runTracked(QStringLiteral("sh"),
      {QStringLiteral("-c"), QStringLiteral("cp \"$1\" \"$2\"; echo clipboard-failed >&2; exit 1"),
       QStringLiteral("helper"), fixture, output}, {}, output));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
    QVERIFY(!settled.first().at(1).toBool());
    QCOMPARE(QImage(output), image);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("clipboard-failed")));
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("Kept copy.png")));
  }

  void concurrentOutputProbesReserveTheOutput_data() {
    QTest::addColumn<bool>("replaceDuringProbe");
    QTest::newRow("duplicate-request") << false;
    QTest::newRow("external-replacement") << true;
  }

  void concurrentOutputProbesReserveTheOutput() {
    QFETCH(bool, replaceDuringProbe);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    const auto script = [&](const QString& name, const QByteArray& contents) {
      QFile file(dir.filePath(name));
      if (!file.open(QIODevice::WriteOnly)) return false;
      if (file.write(contents) != contents.size()) return false;
      file.close();
      return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
    };
    QVERIFY(script(QStringLiteral("ffprobe"),
      "#!/bin/sh\nprintf started > \"${3%/*}/probe-started\"\nsleep 0.3\nexit 1\n"));
    QVERIFY(script(QStringLiteral("omarchy-transcode"),
      "#!/bin/sh\nprintf run >> \"${1%/*}/conversions\"\nprintf fresh > \"${1%.*}-1080p.mp4\"\n"));
    QVERIFY(qputenv("PATH", (dir.path().toUtf8() + ':' + previousPath)));
    const QString source = dir.filePath(QStringLiteral("clip.mp4"));
    const QString output = dir.filePath(QStringLiteral("clip-1080p.mp4"));
    QFile file(output);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("truncated");
    file.close();
    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QVERIFY(registry.run(QStringLiteral("shrink"), source));
    QVERIFY(registry.run(QStringLiteral("shrink"), source));
    QVERIFY(reported.last().first().toString().contains(QStringLiteral("Still working on")));
    // Wait until the asynchronous probe is running before replacing its input.
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath(QStringLiteral("probe-started"))), 3000);
    if (replaceDuringProbe) {
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      file.write("user replacement must survive");
      file.close();
      QTRY_VERIFY_WITH_TIMEOUT(reported.last().first().toString().contains(
        QStringLiteral("Output changed while checking")), 3000);
      QCOMPARE(settled.size(), 0);
      QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("conversions"))));
    } else {
      QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
      QFile conversions(dir.filePath(QStringLiteral("conversions")));
      QVERIFY(conversions.open(QIODevice::ReadOnly));
      QCOMPARE(conversions.readAll(), QByteArray("run"));
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), replaceDuringProbe ? QByteArray("user replacement must survive")
                                              : QByteArray("fresh"));
  }

  void rejectedOutputRemovalFailureDoesNotRevealOrLaunch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QFile probe(dir.filePath(QStringLiteral("ffprobe")));
    QVERIFY(probe.open(QIODevice::WriteOnly));
    probe.write("#!/bin/sh\nexit 1\n");
    probe.close();
    QVERIFY(probe.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    const QString source = dir.filePath(QStringLiteral("clip.mp4"));
    const QString output = dir.filePath(QStringLiteral("clip-1080p.mp4"));
    // A directory reliably makes QFile::remove fail, including under root.
    QVERIFY(QDir().mkdir(output));
    const QString child = output + QStringLiteral("/keep.txt");
    QFile marker(child);
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.write("keep");
    marker.close();
    QVERIFY(QFileInfo(output).size() > 0);
    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy alreadyDone(&launcher, &ActionLauncher::outputAlreadyDone);
    QSignalSpy pending(&launcher, &ActionLauncher::outputPending);
    QVERIFY(registry.run(QStringLiteral("shrink"), source));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("Could not remove invalid output")));
    QCOMPARE(alreadyDone.size(), 0);
    QCOMPARE(pending.size(), 0);
    QVERIFY(QFileInfo::exists(child));
    // The failed probe releases its reservation, so another request can retry.
    QVERIFY(registry.run(QStringLiteral("shrink"), source));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 2, 3000);
    QCOMPARE(alreadyDone.size(), 0);
    QCOMPARE(pending.size(), 0);
  }

  void existingOutputsAreRevealedAndEmptyCorpsesCleared() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    // A stand-in transcoder that writes the same output name the real one
    // would, so the registry's guard and launch path both run for real.
    QFile script(dir.filePath(QStringLiteral("omarchy-transcode")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nout=\"${1%.*}-720p.gif\"\nprintf fresh > \"$out\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);

    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00.mp4"));
    const QString output =
        dir.filePath(QStringLiteral("screenrecording-2026-09-01_10-00-00-720p.gif"));

    // The corpse of a transcode that died before runs were tracked: an empty
    // file that used to make every retry report "already done" and stop.
    QFile corpse(output);
    QVERIFY(corpse.open(QIODevice::WriteOnly));
    corpse.close();
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 3000);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QFile finished(output);
    QVERIFY(finished.open(QIODevice::ReadOnly));
    QCOMPARE(finished.readAll(), QByteArray("fresh"));
    finished.close();

    // A genuinely finished file is handed to the viewer without relaunching,
    // so pressing the action again always shows the result.
    reported.clear();
    QSignalSpy alreadyDone(&launcher, &ActionLauncher::outputAlreadyDone);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QCOMPARE(alreadyDone.size(), 1);
    QCOMPARE(alreadyDone.last().at(0).toString(), output);
    QCOMPARE(settled.size(), 1);
    QVERIFY(reported.last().at(0).toString().contains(QStringLiteral("Already made earlier")));
    QVERIFY(finished.open(QIODevice::ReadOnly));
    QCOMPARE(finished.readAll(), QByteArray("fresh"));
  }

  void truncatedLeftoversAreRedoneNotCalledDone() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) {
      QSKIP("ffprobe not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    QFile script(dir.filePath(QStringLiteral("omarchy-transcode")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nout=\"${1%.*}-1080p.mp4\"\nprintf fresh > \"$out\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    // The stub shadows the real transcoder, but ffprobe stays reachable.
    QVERIFY(qputenv(
        "PATH",
        (dir.path() + QStringLiteral(":") + QString::fromLocal8Bit(previousPath)).toLocal8Bit()));

    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-00-00.mp4"));
    const QString output =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-00-00-1080p.mp4"));

    // The fire-and-forget era could die mid-write: a non-empty file that no
    // player can open, which used to block every retry as "already done".
    QFile corpse(output);
    QVERIFY(corpse.open(QIODevice::WriteOnly));
    corpse.write(QByteArray(4096, 'x'));
    corpse.close();

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    QSignalSpy alreadyDone(&launcher, &ActionLauncher::outputAlreadyDone);
    QVERIFY(registry.run(QStringLiteral("shrink"), source));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 5000);
    QCOMPARE(alreadyDone.size(), 0);
    QCOMPARE(settled.last().at(1).toBool(), true);
    QFile redone(output);
    QVERIFY(redone.open(QIODevice::ReadOnly));
    QCOMPARE(redone.readAll(), QByteArray("fresh"));
  }

  void tailscalePeersAreTheMachinesThatCanTakeAFile() {
    QVariantList peers;
    QString error;

    QVERIFY(!TailscalePeers::parse(R"({"BackendState":"NeedsLogin","Self":{"UserID":0}})", &peers,
                                   &error));
    QVERIFY(error.contains(QStringLiteral("not logged in")));
    QVERIFY(!TailscalePeers::parse("not json", &peers, &error));
    QVERIFY(!error.isEmpty());

    // The daemon's own verdict wins: the offline laptop and the machine owned
    // by someone else are out, the phone is in, and a daemon too old to give
    // a verdict falls back to online and same owner.
    const QByteArray running = R"({
      "BackendState": "Running",
      "Self": {"UserID": 7, "HostName": "desk"},
      "Peer": {
        "a": {"HostName": "phone", "DNSName": "phone.tail.ts.net.", "OS": "android",
              "UserID": 7, "Online": true, "TaildropTarget": 1},
        "b": {"HostName": "laptop", "DNSName": "laptop.tail.ts.net.", "OS": "linux",
              "UserID": 7, "Online": false, "TaildropTarget": 5},
        "c": {"HostName": "shared", "DNSName": "shared.tail.ts.net.", "OS": "linux",
              "UserID": 9, "Online": true, "TaildropTarget": 9},
        "d": {"HostName": "old", "DNSName": "", "OS": "macOS",
              "UserID": 7, "Online": true, "TaildropTarget": 0},
        "e": {"HostName": "theirs", "DNSName": "theirs.tail.ts.net.", "OS": "windows",
              "UserID": 9, "Online": true, "TaildropTarget": 0}
      }})";
    QVERIFY(TailscalePeers::parse(running, &peers, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(peers.size(), 2);
    QCOMPARE(peers.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("old"));
    QCOMPARE(peers.at(0).toMap().value(QStringLiteral("machine")).toString(),
             QStringLiteral("old"));
    QCOMPARE(peers.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("phone"));
    QCOMPARE(peers.at(1).toMap().value(QStringLiteral("machine")).toString(),
             QStringLiteral("phone.tail.ts.net"));

    QVERIFY(TailscalePeers::parse(R"({"BackendState":"Running","Self":{"UserID":7},"Peer":{}})",
                                  &peers, &error));
    QVERIFY(peers.isEmpty());
    QVERIFY(error.contains(QStringLiteral("No other machine")));
  }

  void sendToMachineHandsTheMachineAndEveryFileToTheHouseSender() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });

    // A stand-in for omarchy-tailscale-send that records its argv, one per
    // line, so the test can see exactly what the real one would be handed.
    const QString record = dir.filePath(QStringLiteral("argv.txt"));
    QFile script(dir.filePath(QStringLiteral("omarchy-tailscale-send")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"" +
                 record.toUtf8() + "\"\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));

    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QVERIFY(registry.available(QStringLiteral("tailscale")));
    QVERIFY(registry.appliesTo(QStringLiteral("tailscale"), true));
    QVERIFY(registry.appliesTo(QStringLiteral("tailscale"), false));

    const QString first = dir.filePath(QStringLiteral("a shot.png"));
    const QString second = dir.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(registry.runBatchWith(
        QStringLiteral("tailscale"),
        {{QStringLiteral("machine"), QStringLiteral("laptop.tail.ts.net")}}, {first, second}));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(record) && QFileInfo(record).size() > 0, 3000);
    QFile recorded(record);
    QVERIFY(recorded.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList argv =
        QString::fromUtf8(recorded.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(argv, (QStringList {QStringLiteral("laptop.tail.ts.net"), first, second}));
  }

  void clipToGifRunsRealConversionWithoutDesktopSideEffects() {
    if (QStandardPaths::findExecutable(QStringLiteral("omarchy-transcode")).isEmpty() ||
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
      QSKIP("omarchy-transcode or ffmpeg not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Keep the real conversion, but never notify the desktop or replace the
    // user's clipboard from an automated test. The session bus stays blocked
    // when this suite runs in the local sandbox.
    const QString bin = dir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    for (const QString& name : {QStringLiteral("omarchy-notification-send"), QStringLiteral("wl-copy")}) {
      QFile helper(bin + QLatin1Char('/') + name);
      QVERIFY(helper.open(QIODevice::WriteOnly));
      helper.write(name == QStringLiteral("wl-copy") ? "#!/bin/sh\ncat >/dev/null\n"
                                                     : "#!/bin/sh\nexit 0\n");
      helper.close();
      QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner));
    }
    const QByteArray previousPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + previousPath));

    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-02_09-00-00.mp4"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("testsrc2=size=320x240:rate=10:duration=3"),
                  QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
                  QStringLiteral("yuv420p"), source});
    QVERIFY(ffmpeg.waitForFinished(30000));
    QCOMPARE(ffmpeg.exitCode(), 0);

    // The exact path the Clip to GIF button takes, with the real tool.
    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy settled(&launcher, &ActionLauncher::outputSettled);
    QVERIFY(registry.run(QStringLiteral("gif"), source));
    QTRY_COMPARE_WITH_TIMEOUT(settled.size(), 1, 60000);
    if (!failed.isEmpty()) {
      qWarning() << "failure reported:" << failed.last().at(0).toString();
    }
    QCOMPARE(settled.last().at(1).toBool(), true);
    const QString output =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_09-00-00-720p.gif"));
    QVERIFY(QFileInfo(output).size() > 0);
  }

  void trashIsRecoverableAndMissingFilesFailClearly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("recover me.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("keep this recoverable");
    file.close();

    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    const bool moved = launcher.moveToTrash(path);
    QVERIFY2(moved, failed.isEmpty() ? "moveToTrash failed without a message"
                                     : qPrintable(failed.last().first().toString()));
    QVERIFY(!QFileInfo::exists(path));

    const QDir trash(m_scratch.filePath(QStringLiteral("data/Trash/files")));
    QVERIFY2(!trash.entryList({QStringLiteral("recover me*")}, QDir::Files).isEmpty(),
             qPrintable(trash.absolutePath()));

    QVERIFY(!launcher.moveToTrash(path));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("That file is no longer there"));
  }

  // --- Thumbnails -------------------------------------------------------

  void thumbnailCoversTheTileWithoutUpscaling() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("photo.png"));
    QImage image(400, 300, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(path, "PNG"));

    // A wide tile from a 4:3 source: the short side has to meet the tile so
    // the grid's crop never stretches, and the aspect must survive.
    const QImage tile = ThumbnailCache::thumbnail(path, QSize(200, 100), 1.0);
    QVERIFY(!tile.isNull());
    QCOMPARE(tile.width(), 200);
    QCOMPARE(tile.height(), 150);

    // Provider URLs carry the screen ratio; callers supply logical sizes.
    for (const qreal ratio : {1.0, 1.5, 2.0}) {
      const QImage scaled = ThumbnailCache::thumbnail(path, QSize(100, 50), ratio);
      QCOMPARE(scaled.width(), qRound(100 * ratio));
      QVERIFY(qAbs(scaled.height() - 75 * ratio) <= 1);
    }

    // A source smaller than the tile is left alone rather than blown up.
    const QImage small = ThumbnailCache::thumbnail(path, QSize(800, 800), 1.0);
    QCOMPARE(small.size(), image.size());
  }

  void derivedFilesWearTheirSourcesTile() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty() ||
        QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer")).isEmpty()) {
      QSKIP("ffmpeg or ffmpegthumbnailer not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Dark for the opening quarter then green, the shape of a screen
    // recording that starts on an idle desktop. Short on purpose: a short
    // re-encode has a single keyframe, the case where a percent seek fails
    // and the derived file used to fall back to its dark first frame.
    const QString source = dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-40-00.mp4"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=0x0a0a0a:s=320x240:r=30:d=2"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=0x22aa66:s=320x240:r=30:d=6"),
                  QStringLiteral("-filter_complex"), QStringLiteral("[0][1]concat=n=2:v=1"),
                  QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"),
                  QStringLiteral("yuv420p"), source});
    QVERIFY(ffmpeg.waitForFinished(30000));
    QCOMPARE(ffmpeg.exitCode(), 0);

    // The derived names omarchy-transcode writes. Contents deliberately
    // unrelated (a black frame), because the tile must come from the source.
    const QString gif =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-40-00-720p.gif"));
    const QString resized =
        dir.filePath(QStringLiteral("screenrecording-2026-09-02_10-40-00-1080p.mp4"));
    QProcess black;
    black.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                 QStringLiteral("lavfi"), QStringLiteral("-i"),
                 QStringLiteral("color=black:s=320x240:r=10:d=1"), gif});
    QVERIFY(black.waitForFinished(30000));
    QCOMPARE(black.exitCode(), 0);
    QVERIFY(QFile::copy(source, resized));

    const QImage sourceTile = ThumbnailCache::thumbnail(source, QSize(80, 60), 1.0, 20);
    const QImage gifTile = ThumbnailCache::thumbnail(gif, QSize(80, 60), 1.0, 20);
    const QImage resizedTile = ThumbnailCache::thumbnail(resized, QSize(80, 60), 1.0, 20);
    QVERIFY(!sourceTile.isNull());
    QCOMPARE(gifTile, sourceTile);
    QCOMPARE(resizedTile, sourceTile);
  }

  void animatedGifThumbnailSeeksLikeAVideo() {
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
      QSKIP("ffmpeg not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // One second of red then four of blue, so frame zero and the 20% frame a
    // video's tile would show are unmistakably different colours.
    const QString path = dir.filePath(QStringLiteral("clip-720p.gif"));
    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                 {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=red:s=64x64:r=5:d=1"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("color=blue:s=64x64:r=5:d=4"), QStringLiteral("-filter_complex"),
                  QStringLiteral("[0][1]concat=n=2:v=1"), path});
    QVERIFY(ffmpeg.waitForFinished(15000));
    QCOMPARE(ffmpeg.exitCode(), 0);

    // The default grid seek is 20%: past the red opening, into the blue.
    const QImage tile = ThumbnailCache::thumbnail(path, QSize(64, 64), 1.0, 20);
    QVERIFY(!tile.isNull());
    const QColor centre = tile.pixelColor(tile.width() / 2, tile.height() / 2);
    // The GIF palette dims pure blue, so compare channels rather than expect
    // full saturation.
    QVERIFY2(
        centre.blue() > 120 && centre.red() < 80,
        qPrintable(QStringLiteral("expected the 20%% frame (blue), got %1").arg(centre.name())));
  }

  // These tools consume only temporary files, never a real selection or printer.
  void submissionsConfirmOnlyAfterSuccessfulCompletion_data() {
    QTest::addColumn<QByteArray>("ending");
    QTest::addColumn<bool>("success");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("accepted") << QByteArray("printf 'request id is mock-42\\n'\nexit 0\n")
                             << true << QStringLiteral("mock-42");
    QTest::newRow("rejected") << QByteArray("printf 'printer unavailable\\n' >&2\nexit 7\n")
                             << false << QStringLiteral("printer unavailable");
    QTest::newRow("crashed") << QByteArray("ulimit -c 0\nkill -KILL $$\n")
                            << false << QStringLiteral("did not finish");
  }

  void submissionsConfirmOnlyAfterSuccessfulCompletion() {
    QFETCH(QByteArray, ending);
    QFETCH(bool, success);
    QFETCH(QString, diagnostic);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile tool(dir.filePath(QStringLiteral("lp")));
    QVERIFY(tool.open(QIODevice::WriteOnly));
    tool.write("#!/bin/sh\n/bin/sleep 0.2\nprintf '%s\\n' \"$@\" > \"$1.args\"\n" + ending);
    tool.close();
    QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    const QString path = dir.filePath(QStringLiteral("photo ; $(touch nope) # ü.png"));
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("original");
    input.close();

    ActionLauncher launcher;
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy finished(&launcher, &ActionLauncher::submissionFinished);
    bool heartbeat = false;
    QTimer::singleShot(10, &launcher, [&] { heartbeat = true; });
    QVERIFY(launcher.runSubmission(QStringLiteral("lp"), {path}, QStringLiteral("cups"),
                                   QStringLiteral("Submitted to the print queue")));
    QCOMPARE(reported.size(), 0);
    QCOMPARE(finished.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(finished.first().at(1).toBool(), success);
    QCOMPARE(reported.size(), success ? 1 : 0);
    QCOMPARE(failed.size(), success ? 0 : 1);
    QVERIFY(finished.first().at(2).toString().contains(diagnostic));
    QFile arguments(path + QStringLiteral(".args"));
    QVERIFY(arguments.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(arguments.readAll()), path + QLatin1Char('\n'));
    QVERIFY(input.open(QIODevice::ReadOnly));
    QCOMPARE(input.readAll(), QByteArray("original"));
  }

  void submissionsReportFailedStartAndSurviveLauncherDestruction() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto writeTool = [&](const QByteArray& body) {
      QFile file(dir.filePath(QStringLiteral("lp")));
      if (!file.open(QIODevice::WriteOnly)) return false;
      file.write(body);
      file.close();
      return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
    };
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    ActionLauncher launcher;
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QSignalSpy finished(&launcher, &ActionLauncher::submissionFinished);
    QVERIFY(!launcher.runSubmission(QStringLiteral("lp"), {}));
    QCOMPARE(failed.size(), 1);
    QVERIFY(writeTool("#!/does/not/exist\n"));
    QVERIFY(launcher.runSubmission(QStringLiteral("lp"), {}));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(failed.size(), 2);
    QVERIFY(!finished.first().at(1).toBool());

    QVERIFY(writeTool("#!/bin/sh\nexec /bin/sleep 10\n"));
    auto pending = std::make_unique<ActionLauncher>();
    bool completed = false;
    connect(pending.get(), &ActionLauncher::submissionFinished, this,
            [&] { completed = true; });
    QVERIFY(pending->runSubmission(QStringLiteral("lp"), {}));
    QTest::qWait(20);
    pending.reset();
    QTest::qWait(20);
    QVERIFY(!completed);
  }

  void imageCopyTracksWlCopyEvenWithAnInstalledHouseHelper_data() {
    QTest::addColumn<bool>("helper");
    QTest::addColumn<bool>("success");
    QTest::newRow("helper-installed-success") << true << true;
    QTest::newRow("helper-installed-failure") << true << false;
    QTest::newRow("direct-success") << false << true;
    QTest::newRow("direct-failure") << false << false;
  }

  void imageCopyTracksWlCopyEvenWithAnInstalledHouseHelper() {
    QFETCH(bool, helper);
    QFETCH(bool, success);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("image # ü.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::magenta);
    QVERIFY(image.save(path, "PNG"));
    // The installed house helper masks wl-copy's failure in --copy-only mode.
    // Presence of that helper must not change which completion we trust.
    if (helper) {
      QFile house(dir.filePath(QStringLiteral("omarchy-clipboard-paste-file")));
      QVERIFY(house.open(QIODevice::WriteOnly));
      house.write("#!/bin/sh\nprintf called > '");
      house.write(path.toUtf8());
      house.write(".helper-called'\nexit 0\n");
      house.close();
      QVERIFY(house.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner));
    }
    QFile tool(dir.filePath(QStringLiteral("wl-copy")));
    QVERIFY(tool.open(QIODevice::WriteOnly));
    tool.write("#!/bin/sh\n/bin/sleep 0.2\n/bin/cat > '");
    tool.write(path.toUtf8());
    tool.write(".copied'\nprintf '%s\\n' \"$@\" > '");
    tool.write(path.toUtf8());
    tool.write(".args'\n");
    tool.write(success ? "exit 0\n" : "echo clipboard-rejected >&2\nexit 4\n");
    tool.close();
    QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    ActionLauncher launcher;
    QSignalSpy finished(&launcher, &ActionLauncher::submissionFinished);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QVERIFY(launcher.copyFile(path));
    QCOMPARE(reported.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(finished.first().at(1).toBool(), success);
    QCOMPARE(reported.size(), success ? 1 : 0);
    QCOMPARE(failed.size(), success ? 0 : 1);
    QFile copied(path + QStringLiteral(".copied"));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), original.readAll());
    QFile arguments(path + QStringLiteral(".args"));
    QVERIFY(arguments.open(QIODevice::ReadOnly));
    QCOMPARE(arguments.readAll(), QByteArray("--type\nimage/png\n"));
    QVERIFY(!QFileInfo::exists(path + QStringLiteral(".helper-called")));
  }

  void imageCopyTimeoutDoesNotConfirmSuccess() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("image.png"));
    QVERIFY(QImage(8, 8, QImage::Format_RGB32).save(path, "PNG"));
    QFile tool(dir.filePath(QStringLiteral("wl-copy")));
    QVERIFY(tool.open(QIODevice::WriteOnly));
    tool.write("#!/bin/sh\nexec /bin/sleep 10\n");
    tool.close();
    QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    ActionLauncher launcher;
    QSignalSpy finished(&launcher, &ActionLauncher::submissionFinished);
    QSignalSpy reported(&launcher, &ActionLauncher::reported);
    QSignalSpy failed(&launcher, &ActionLauncher::failed);
    QVERIFY(launcher.copyFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 8000);
    QVERIFY(!finished.first().at(1).toBool());
    QCOMPARE(reported.size(), 0);
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("Timed out")));
  }

  void matteSaveSeparatesClipboardFailureAndRetriesWithoutAnotherCopy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("photo # ü.png"));
    QImage image(120, 80, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(source, "PNG"));
    QFile original(source);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = original.readAll();
    original.close();
    const auto writeTool = [&](bool success) {
      QFile tool(dir.filePath(QStringLiteral("wl-copy")));
      if (!tool.open(QIODevice::WriteOnly)) return false;
      tool.write("#!/bin/sh\n/bin/sleep 0.2\n/bin/cat > /dev/null\n");
      tool.write(success ? "exit 0\n" : "echo selection-rejected >&2\nexit 9\n");
      tool.close();
      return tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
    };
    QVERIFY(writeTool(false));
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    MatteComposer composer;
    QSignalSpy saved(&composer, &MatteComposer::saved);
    QSignalSpy copied(&composer, &MatteComposer::copied);
    QSignalSpy composed(&composer, &MatteComposer::composed);
    QSignalSpy failed(&composer, &MatteComposer::failed);
    QSignalSpy finished(&composer, &MatteComposer::finished);
    QSignalSpy busy(&composer, &MatteComposer::busyChanged);
    bool heartbeat = false;
    QTimer::singleShot(10, &composer, [&] { heartbeat = composer.busy(); });
    composer.composeAndSave(source, MatteComposer::Slate, MatteComposer::Square, 0.07);
    QVERIFY(composer.busy());
    QCOMPARE(finished.size(), 0);
    composer.composeAndSave(source, MatteComposer::Pop, MatteComposer::Original, 0.1);
    QCOMPARE(failed.size(), 1); // duplicate request rejected while busy
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!composer.busy());
    QCOMPARE(busy.size(), 2);
    QCOMPARE(saved.size(), 1);
    QCOMPARE(copied.size(), 0);
    QCOMPARE(composed.size(), 0);
    QCOMPARE(failed.size(), 2);
    QVERIFY(failed.last().first().toString().contains(QStringLiteral("selection-rejected")));
    QVERIFY(finished.first().at(1).toBool());
    QVERIFY(!finished.first().at(2).toBool());
    const QString output = saved.first().first().toString();
    QCOMPARE(composer.lastOutputPath(), output);
    QVERIFY(!QImage(output).isNull());
    QVERIFY(writeTool(true));
    composer.retryCopy();
    QVERIFY(composer.busy());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(copied.size(), 1);
    QCOMPARE(copied.first().first().toString(), output);
    QCOMPARE(saved.size(), 1);
    QCOMPARE(composed.size(), 0); // retry does not announce a new saved file
    QVERIFY(finished.last().at(1).toBool());
    QVERIFY(finished.last().at(2).toBool());
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*-matte*.png")}, QDir::Files).size(), 1);

    composer.composeAndSave(source, MatteComposer::None, MatteComposer::Original, 0.1);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 5000);
    QCOMPARE(composed.size(), 1);
    QCOMPARE(saved.size(), 2);
    QVERIFY(saved.last().first().toString().endsWith(QStringLiteral("-matte-2.png")));
    QCOMPARE(QImage(saved.last().first().toString()), image);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), originalBytes);
    QFile::remove(composer.lastOutputPath());
    composer.retryCopy();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 5000);
    QVERIFY(!finished.last().at(1).toBool());
    QVERIFY(!finished.last().at(2).toBool());
  }

  void matteFailuresReleaseBusyAndDoNotPublishCopies() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MatteComposer composer;
    QSignalSpy failed(&composer, &MatteComposer::failed);
    QSignalSpy finished(&composer, &MatteComposer::finished);
    QSignalSpy saved(&composer, &MatteComposer::saved);
    composer.retryCopy();
    QCOMPARE(failed.size(), 1);
    QVERIFY(!composer.busy());
    composer.composeAndSave(dir.filePath(QStringLiteral("missing.png")), MatteComposer::Slate,
                            MatteComposer::Original, 0.1);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QVERIFY(!composer.busy());
    QVERIFY(!finished.first().at(1).toBool());
    QVERIFY(!finished.first().at(2).toBool());
    QFile unreadable(dir.filePath(QStringLiteral("bad.png")));
    QVERIFY(unreadable.open(QIODevice::WriteOnly));
    unreadable.write("not an image");
    unreadable.close();
    composer.composeAndSave(unreadable.fileName(), MatteComposer::Slate, MatteComposer::Original, 0.1);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 3000);
    QVERIFY(!composer.busy());
    QCOMPARE(saved.size(), 0);
    QVERIFY(composer.lastOutputPath().isEmpty());
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*-matte*.png")}, QDir::Files).size(), 0);
    composer.composeAndSave(unreadable.fileName(), -1, MatteComposer::Original, 0.1);
    QVERIFY(!composer.busy());
    QCOMPARE(failed.size(), 4);
  }

  void matteWorkersSurviveComposerDestructionAndReserveDistinctNames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("photo.png"));
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    QVERIFY(image.save(source, "PNG"));
    QFile tool(dir.filePath(QStringLiteral("wl-copy")));
    QVERIFY(tool.open(QIODevice::WriteOnly));
    tool.write("#!/bin/sh\n/bin/cat > /dev/null\nexit 0\n");
    tool.close();
    QVERIFY(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    const QByteArray previousPath = qgetenv("PATH");
    const auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
    QVERIFY(qputenv("PATH", dir.path().toUtf8()));
    auto abandoned = std::make_unique<MatteComposer>();
    bool completed = false;
    connect(abandoned.get(), &MatteComposer::finished, this, [&] { completed = true; });
    abandoned->composeAndSave(source, MatteComposer::None, MatteComposer::Original, 0.1);
    abandoned.reset();
    MatteComposer live;
    QSignalSpy finished(&live, &MatteComposer::finished);
    live.composeAndSave(source, MatteComposer::None, MatteComposer::Original, 0.1);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    const QString first = dir.filePath(QStringLiteral("photo-matte.png"));
    const QString second = dir.filePath(QStringLiteral("photo-matte-2.png"));
    QTRY_VERIFY_WITH_TIMEOUT(!QImage(first).isNull() && !QImage(second).isNull(), 5000);
    // Await the value-only worker as well before destroying its temporary files.
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QVERIFY(!completed);
    QCOMPARE(QImage(first), image);
    QCOMPARE(QImage(second), image);
    QCOMPARE(QImage(source), image);
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
    // Padding has a 24 px floor, applied on both sides.
    QCOMPARE(result.width(), 200 + 2 * 24);
    QCOMPARE(result.height(), 100 + 2 * 24);
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

  void budgetScalingKeepsTheCaptureWholeAndProportioned() {
    // A tall scrolling screenshot at the largest padding stop, forced wide:
    // the canvas is shrunk to the budget, and the padding has to shrink with
    // it or the capture is cropped top and bottom.
    QImage tall(1080, 7000, QImage::Format_RGB32);
    tall.fill(QColor(255, 0, 255));
    const QImage result =
        MatteComposer::compose(tall, MatteComposer::Slate, MatteComposer::Wide, 0.18);
    QVERIFY(!result.isNull());

    int top = result.height();
    int bottom = -1;
    for (int y = 0; y < result.height(); ++y) {
      const QRgb pixel = result.pixel(result.width() / 2, y);
      if (qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) > 200) {
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
    QVERIFY(bottom > top);
    // Not cropped: matte above and below the capture.
    QVERIFY(top > 0);
    QVERIFY(bottom < result.height() - 1);
    // Proportioned: the vertical padding is the requested 18% of the long
    // edge, which on this canvas is about 13% of its height either side.
    const qreal above = qreal(top) / result.height();
    const qreal below = qreal(result.height() - 1 - bottom) / result.height();
    QVERIFY2(above > 0.10 && above < 0.16, qPrintable(QString::number(above)));
    QVERIFY2(below > 0.10 && below < 0.16, qPrintable(QString::number(below)));
    // The capture keeps its own shape rather than becoming a sliver.
    int left = result.width();
    int right = -1;
    for (int x = 0; x < result.width(); ++x) {
      const QRgb pixel = result.pixel(x, result.height() / 2);
      if (qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) > 200) {
        left = std::min(left, x);
        right = std::max(right, x);
      }
    }
    const qreal ratio = qreal(right - left + 1) / (bottom - top + 1);
    QVERIFY2(std::abs(ratio - 1080.0 / 7000.0) < 0.01, qPrintable(QString::number(ratio)));
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
      const QImage result = MatteComposer::compose(source, static_cast<MatteComposer::Matte>(matte),
                                                   MatteComposer::Original, 0.08);
      QVERIFY2(!result.isNull(), qPrintable(QStringLiteral("matte %1 was null").arg(matte)));
    }
  }

  // --- Quick image corrections ------------------------------------------

  void correctionsRotateFlipCropAndResize() {
    QImage source(4, 2, QImage::Format_ARGB32);
    source.fill(Qt::transparent);
    // Distinct colours so a wrong turn or flip is visible.
    source.setPixelColor(0, 0, QColor(Qt::red));
    source.setPixelColor(3, 0, QColor(Qt::green));
    source.setPixelColor(0, 1, QColor(Qt::blue));
    source.setPixelColor(3, 1, QColor(Qt::yellow));

    // A clockwise quarter turn swaps the dimensions and carries the top-left
    // pixel to the top-right.
    ImageEditor::Transform turn;
    turn.quarterTurns = 1;
    const QImage turned = ImageEditor::apply(source, turn);
    QCOMPARE(turned.size(), QSize(2, 4));
    QCOMPARE(turned.pixelColor(1, 0), QColor(Qt::red));
    QCOMPARE(turned.pixelColor(1, 3), QColor(Qt::green));

    // Crop the turned frame, then resize to a fatter target: aspect is kept.
    turn.cropW = 1.0;
    turn.cropH = 0.5;
    turn.targetWidth = 10;
    turn.targetHeight = 10;
    const QImage cropped = ImageEditor::apply(source, turn);
    QCOMPARE(cropped.size(), QSize(10, 10));
    QCOMPARE(cropped.pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(cropped.pixelColor(9, 0), QColor(Qt::red));

    // A horizontal flip mirrors left to right.
    ImageEditor::Transform mirror;
    mirror.flipHorizontal = true;
    const QImage flipped = ImageEditor::apply(source, mirror);
    QCOMPARE(flipped.pixelColor(0, 0), QColor(Qt::green));
    QCOMPARE(flipped.pixelColor(3, 0), QColor(Qt::red));
  }

  void correctionsSaveAsCopyKeepsTheOriginal() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("photo.png"));
    QImage image(6, 4, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(source, "PNG"));
    QFile original(source);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = original.readAll();
    original.close();

    ImageEditor editor;
    QSignalSpy saved(&editor, &ImageEditor::saved);
    QSignalSpy failed(&editor, &ImageEditor::failed);
    editor.saveCopy(source, 1, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    QCOMPARE(failed.size(), 0);
    const QString output = saved.first().first().toString();
    QVERIFY(output.endsWith(QStringLiteral("-edited.png")));
    QVERIFY(QFileInfo::exists(output));
    // The corner was at (0,0) on a 6x4; turned clockwise it is 4x6 and the
    // top-left corner moved to the top-right.
    QCOMPARE(QImage(output).size(), QSize(4, 6));

    // The original is byte-for-byte untouched.
    QFile verify(source);
    QVERIFY(verify.open(QIODevice::ReadOnly));
    QCOMPARE(verify.readAll(), originalBytes);

    // A second correction of the same file makes its own numbered copy.
    QSignalSpy savedAgain(&editor, &ImageEditor::saved);
    editor.saveCopy(source, 0, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(savedAgain.size(), 1, 15000);
    QVERIFY(savedAgain.first().first().toString().endsWith(QStringLiteral("-edited-2.png")));
  }

  void correctionsCarryTheColourProfile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("wide.png"));
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(QColor(10, 20, 30));
    const QColorSpace space(QColorSpace::DisplayP3);
    image.setColorSpace(space);
    QVERIFY(image.save(source, "PNG"));

    ImageEditor editor;
    QSignalSpy saved(&editor, &ImageEditor::saved);
    editor.saveCopy(source, 0, true, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    QCOMPARE(saved.size(), 1);
    const QImage result(saved.first().first().toString());
    QVERIFY(!result.isNull());
    QVERIFY2(result.colorSpace().isValid(), "the copy lost its colour profile");
    QCOMPARE(result.colorSpace(), space);
  }

  void correctionsRefuseAMissingFile() {
    ImageEditor editor;
    QSignalSpy failed(&editor, &ImageEditor::failed);
    editor.saveCopy(QStringLiteral("/tmp/omaroll-does-not-exist-9e8a.png"), 0, false, false, 0, 0, 0,
                    1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
    QVERIFY(!failed.first().first().toString().isEmpty());
  }

  void correctionsReportTheOrientedSize() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("plain.png"));
    QVERIFY(QImage(7, 3, QImage::Format_RGB32).save(source, "PNG"));
    ImageEditor editor;
    QCOMPARE(editor.orientedSize(source), QSize(7, 3));
    QCOMPARE(editor.orientedSize(dir.filePath(QStringLiteral("missing.png"))), QSize());
  }

  void transparencySurvivesACorrectionCopy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("alpha.png"));
    QImage image(40, 20, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width() / 2; ++x) {
        image.setPixelColor(x, y, QColor(255, 0, 0, 255));
      }
    }
    QVERIFY(image.save(source, "PNG"));

    ImageEditor editor;
    QSignalSpy saved(&editor, &ImageEditor::saved);
    editor.saveCopy(source, 0, false, false, 0.0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    const QImage copy(saved.first().first().toString());
    QVERIFY(copy.hasAlphaChannel());
    QCOMPARE(copy.pixelColor(5, 10).alpha(), 255);
    QCOMPARE(copy.pixelColor(35, 10).alpha(), 0);
  }

  void largeImageResizeStaysWithinTheBudget() {
    // A 400-megapixel request must be clamped, and the aspect ratio kept.
    QImage large(6000, 4000, QImage::Format_RGB32);
    large.fill(Qt::black);
    ImageEditor::Transform transform;
    transform.targetWidth = 20000;
    transform.targetHeight = 20000;
    const QImage result = ImageEditor::apply(large, transform);
    QVERIFY(!result.isNull());
    const qint64 pixels = static_cast<qint64>(result.width()) * result.height();
    QVERIFY2(pixels <= ImageEditor::kMaxOutputPixels, qPrintable(QString::number(pixels)));
    const qreal ratio = qreal(result.width()) / result.height();
    QVERIFY2(qAbs(ratio - 6000.0 / 4000.0) < 0.01, qPrintable(QString::number(ratio)));
  }

  void orientationIsAppliedAndBakedIntoACopy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage source(40, 20, QImage::Format_RGB32);
    source.fill(Qt::red);
    for (int y = 0; y < source.height(); ++y) {
      for (int x = source.width() / 2; x < source.width(); ++x) {
        source.setPixelColor(x, y, Qt::blue);
      }
    }
    QByteArray jpeg;
    {
      QBuffer buffer(&jpeg);
      QVERIFY(buffer.open(QIODevice::WriteOnly));
      QVERIFY(source.save(&buffer, "JPG"));
    }
    const QString path = dir.filePath(QStringLiteral("oriented.jpg"));
    {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(withExifOrientation(jpeg, 6)); // rotate 90 clockwise
    }

    // The reader reports the tag and hands back the upright image: 40x20
    // becomes 20x40.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QVERIFY((reader.transformation() & QImageIOHandler::TransformationRotate90) != 0);
    const QImage oriented = reader.read();
    QCOMPARE(oriented.size(), QSize(20, 40));

    // The editor's size helper agrees with the reader.
    ImageEditor editor;
    QCOMPARE(editor.orientedSize(path), QSize(20, 40));

    // A saved copy bakes the orientation in: the tag is gone and the pixels
    // stay upright.
    QSignalSpy saved(&editor, &ImageEditor::saved);
    editor.saveCopy(path, 0, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    const QString output = saved.first().first().toString();
    QImageReader copyReader(output);
    QCOMPARE(copyReader.transformation(), QImageIOHandler::Transformations(
                                              QImageIOHandler::TransformationNone));
    const QImage copy = copyReader.read();
    QCOMPARE(copy.size(), QSize(20, 40));
    // A clockwise turn sends the source's left half to the top half. JPEG is
    // lossy, so check the dominant channel rather than exact colours.
    const QColor top = copy.pixelColor(copy.width() / 2, 2);
    QVERIFY2(top.red() > 200 && top.blue() < 80, qPrintable(top.name()));
    const QColor bottom = copy.pixelColor(copy.width() / 2, copy.height() - 3);
    QVERIFY2(bottom.blue() > 200 && bottom.red() < 80, qPrintable(bottom.name()));
  }

  void jpegRotationIsLosslessWhenPossible() {
    if (!JpegTransform::available()) {
      QSKIP("jpegtran is not installed");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("lossless.jpg"));
    // Dimensions that are a whole number of blocks, so -perfect can rotate.
    QImage image(240, 160, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
      for (int x = 0; x < image.width(); ++x) {
        image.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256));
      }
    }
    QVERIFY(image.save(source, "JPG", 95));
    const QImage decoded(source);
    QVERIFY(!decoded.isNull());

    ImageEditor editor;
    QSignalSpy saved(&editor, &ImageEditor::saved);
    editor.saveCopy(source, 1, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    const QString turned = saved.first().first().toString();
    const QImage turnedImage(turned);
    QCOMPARE(turnedImage.size(), QSize(160, 240));

    // The turn is the expected one (rounding of a rotated IDCT can differ by a
    // hair, so allow a small tolerance here).
    const QImage expected = decoded.transformed(QTransform().rotate(90), Qt::FastTransformation);
    int maxDifference = 0;
    for (int y = 0; y < turnedImage.height(); ++y) {
      for (int x = 0; x < turnedImage.width(); ++x) {
        const QColor a = turnedImage.pixelColor(x, y);
        const QColor b = expected.pixelColor(x, y);
        maxDifference = qMax(maxDifference,
                             qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())),
                                  qAbs(a.blue() - b.blue())));
      }
    }
    QVERIFY2(maxDifference <= 6, qPrintable(QString::number(maxDifference)));

    // Losslessness: the inverse turn returns the original pixels exactly. A
    // recompressing path would lose a little on each pass.
    QSignalSpy back(&editor, &ImageEditor::saved);
    editor.saveCopy(turned, 3, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(back.size(), 1, 15000);
    QCOMPARE(QImage(back.first().first().toString()), decoded);
  }

  void jpegLosslessIsSkippedWhenItCannotApply() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A PNG is not a JPEG, so the recompressing path runs and still rotates.
    const QString png = dir.filePath(QStringLiteral("plain.png"));
    QVERIFY(QImage(48, 24, QImage::Format_RGB32).save(png, "PNG"));
    ImageEditor editor;
    QSignalSpy saved(&editor, &ImageEditor::saved);
    editor.saveCopy(png, 1, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    QCOMPARE(QImage(saved.first().first().toString()).size(), QSize(24, 48));

    // An EXIF-oriented JPEG must not go lossless: the orientation has to be
    // baked in, which needs the decode path.
    const QString jpeg = dir.filePath(QStringLiteral("oriented.jpg"));
    QImage oriented(40, 20, QImage::Format_RGB32);
    oriented.fill(Qt::red);
    QByteArray bytes;
    {
      QBuffer buffer(&bytes);
      QVERIFY(buffer.open(QIODevice::WriteOnly));
      QVERIFY(oriented.save(&buffer, "JPG"));
    }
    {
      QFile file(jpeg);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(withExifOrientation(bytes, 6));
    }
    QSignalSpy rotated(&editor, &ImageEditor::saved);
    editor.saveCopy(jpeg, 1, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(rotated.size(), 1, 15000);
    const QImage result(rotated.first().first().toString());
    // The 40x20 source is oriented to 20x40, then turned once: 40x20.
    QCOMPARE(result.size(), QSize(40, 20));
  }

  void colourProfileSurvivesAJPEGCorrection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("profile.jpg"));
    QImage image(32, 24, QImage::Format_RGB32);
    image.fill(QColor(30, 40, 50));
    const QColorSpace space(QColorSpace::DisplayP3);
    image.setColorSpace(space);
    QVERIFY(image.save(source, "JPG", 95));

    ImageEditor editor;
    QSignalSpy saved(&editor, &ImageEditor::saved);
    editor.saveCopy(source, 0, false, false, 0, 0, 0, 1, 1, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    const QImage result(saved.first().first().toString());
    QVERIFY(!result.isNull());
    QVERIFY2(result.colorSpace().isValid(), "the copy lost its colour profile");
    // Not just valid: the same profile, so a swap to sRGB would fail too.
    QCOMPARE(result.colorSpace(), space);
  }

  void correctionsApplyToASelectionAsCopies() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.png"));
    const QString second = dir.filePath(QStringLiteral("b.png"));
    QVERIFY(QImage(80, 40, QImage::Format_RGB32).save(first, "PNG"));
    QVERIFY(QImage(60, 30, QImage::Format_RGB32).save(second, "PNG"));

    ImageEditor editor;
    QSignalSpy finished(&editor, &ImageEditor::batchFinished);
    QSignalSpy progress(&editor, &ImageEditor::batchProgress);
    editor.saveCopies({first, second}, 1, false, false, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QCOMPARE(finished.first().at(0).toInt(), 2);
    QCOMPARE(finished.first().at(1).toInt(), 0);
    QVERIFY(progress.size() >= 2);
    QCOMPARE(QImage(dir.filePath(QStringLiteral("a-edited.png"))).size(), QSize(40, 80));
    QCOMPARE(QImage(dir.filePath(QStringLiteral("b-edited.png"))).size(), QSize(30, 60));

    // A missing file counts as a failure and does not stop the run.
    editor.saveCopies({first, dir.filePath(QStringLiteral("missing.png"))}, 0, false, false, 0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 20000);
    QCOMPARE(finished.last().at(0).toInt(), 1);
    QCOMPARE(finished.last().at(1).toInt(), 1);
    // The existing copy was not overwritten; the new one is numbered.
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("a-edited-2.png"))));
  }

  void straightenFillsTheFrameWithoutEmptyCorners() {
    QImage source(80, 40, QImage::Format_RGB32);
    source.fill(Qt::darkGreen);

    ImageEditor::Transform turned;
    turned.straightenDegrees = 10.0;
    const QImage result = ImageEditor::apply(source, turned);
    // The frame keeps its size and the crop rectangle still means the same.
    QCOMPARE(result.size(), source.size());
    // Filled, not letterboxed: the corner comes from inside the picture, so it
    // is still the source colour rather than black.
    const QColor corner = result.pixelColor(1, 1);
    QVERIFY2(corner.green() > 80, qPrintable(corner.name()));

    // Zero degrees is the identity.
    QCOMPARE(ImageEditor::apply(source, ImageEditor::Transform{}), source);

    // A straighten before the crop keeps the crop's proportions.
    ImageEditor::Transform cropped;
    cropped.straightenDegrees = 5.0;
    cropped.cropW = 0.5;
    cropped.cropH = 0.5;
    QCOMPARE(ImageEditor::apply(source, cropped).size(), QSize(40, 20));
  }

  void correctionsCopyARegionToTheClipboard() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("region.png"));
    QVERIFY(QImage(64, 48, QImage::Format_RGB32).save(source, "PNG"));

    ImageEditor editor;
    QSignalSpy copied(&editor, &ImageEditor::copied);
    QSignalSpy failed(&editor, &ImageEditor::failed);
    // A quarter of the image: it has to come back without error.
    editor.copyRegion(source, 0, false, false, 0, 0.5, 0.5, 0.5, 0.5);
    QTRY_COMPARE_WITH_TIMEOUT(copied.size(), 1, 15000);
    QCOMPARE(failed.size(), 0);

    editor.copyRegion(dir.filePath(QStringLiteral("missing.png")), 0, false, false, 0, 0, 0, 1, 1);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
    QVERIFY(!failed.first().first().toString().isEmpty());
  }

  void printActionCoversPicturesAndDocuments() {
    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QVERIFY(registry.appliesToKind(QStringLiteral("print"), false, false));
    QVERIFY(registry.appliesToKind(QStringLiteral("print"), false, true));
    QVERIFY(!registry.appliesToKind(QStringLiteral("print"), true, false));
  }

  void correctionsActionIsNativeAndForStills() {
    ActionLauncher launcher;
    ActionRegistry registry(&launcher);
    QVERIFY(registry.isNative(QStringLiteral("corrections")));
    QVERIFY(registry.appliesTo(QStringLiteral("corrections"), false));
    QVERIFY(!registry.appliesTo(QStringLiteral("corrections"), true));
    QVERIFY(registry.isNative(QStringLiteral("compare")));
    QVERIFY(registry.appliesTo(QStringLiteral("compare"), false));
    QVERIFY(!registry.appliesTo(QStringLiteral("compare"), true));
  }

private:
  QTemporaryDir m_scratch;

  // A page laid out in code, in page points: two lines of two words each.
  static PdfSupport::PdfPageText twoLinePage() {
    PdfSupport::PdfPageText page;
    page.pageSize = QSizeF(600, 800);
    const auto add = [&page](qreal x, qreal y, qreal width, const char* text) {
      PdfSupport::PdfWord word;
      word.box = QRectF(x, y, width, 10);
      word.text = QString::fromLatin1(text);
      page.words.append(word);
    };
    add(100, 100, 40, "Invoice");
    add(144, 100, 60, "total");
    add(100, 120, 50, "Paid");
    add(154, 120, 30, "today");
    return page;
  }
};

int main(int argc, char* argv[]) {
  disableHeadlessAudio();
  QGuiApplication application(argc, argv);
  OmarollTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}
#include "tst_omaroll.moc"
