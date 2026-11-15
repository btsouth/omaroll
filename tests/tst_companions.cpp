#include "app/AppSettings.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/SectionRows.h"
#include "sources/CameraRaw.h"
#include "sources/CaptureScanner.h"
#include "sources/RawJpegPairs.h"
#include "viewer/ViewerSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSaveFile>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtTest>

#include <memory>

namespace {
// Keep queued listing/header work behind one known worker, without production
// test hooks or timing guesses about how quickly a small fixture is read.
class HeldWorker final {
public:
  HeldWorker() : m_pool(QThreadPool::globalInstance()), m_threads(m_pool->maxThreadCount()) {
    m_pool->setMaxThreadCount(1);
    m_future = QtConcurrent::run([this] {
      m_started.release();
      m_release.acquire();
    });
  }
  ~HeldWorker() {
    release();
    m_future.waitForFinished();
    m_pool->waitForDone();
    m_pool->setMaxThreadCount(m_threads);
  }
  bool started() { return m_started.tryAcquire(1, 5000); }
  void release() {
    if (!m_released) {
      m_released = true;
      m_release.release();
    }
  }
private:
  QThreadPool* m_pool;
  int m_threads;
  QSemaphore m_started;
  QSemaphore m_release;
  QFuture<void> m_future;
  bool m_released = false;
};
} // namespace

class CompanionTest final : public QObject {
  Q_OBJECT

private slots:
  void init() {
    m_scratch = std::make_unique<QTemporaryDir>();
    QVERIFY(m_scratch->isValid());
    m_media = m_scratch->filePath(QStringLiteral("media"));
    QVERIFY(QDir().mkpath(m_media));
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_scratch->filePath(QStringLiteral("config")));
    // AppSettings writes organization snapshots under the data folder.
    QVERIFY(qputenv("XDG_DATA_HOME", m_scratch->filePath(QStringLiteral("data")).toUtf8()));
    for (const char* variable : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                                 "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR"}) {
      QVERIFY(qputenv(variable, m_media.toUtf8()));
    }
  }

  void matchesOnlyUniqueSameDirectoryStems() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = QStringLiteral("/one/IMG.01.DNG");
    const QString jpeg = QStringLiteral("/one/img.01.JpEg");
    const QStringList paths{raw, jpeg, QStringLiteral("/two/img.01.jpg"),
                            QStringLiteral("/One/img.01.jpg"), QStringLiteral("/one/img.01.png")};
    const auto pairs = RawJpegPairs::find(paths);
    QCOMPARE(pairs.size(), 2);
    QCOMPARE(pairs.value(raw), jpeg);
    QCOMPARE(pairs.value(jpeg), raw);
    QCOMPARE(RawJpegPairs::find({raw, QStringLiteral("/two/img.01.jpg")}).size(), 0);
    QCOMPARE(RawJpegPairs::find({raw, jpeg, QStringLiteral("/one/img.01.jpg")}).size(), 0);
    // Even the same RAW format twice, with case variants, is ambiguous.
    QCOMPARE(RawJpegPairs::find({raw, jpeg, QStringLiteral("/one/img.01.dng")}).size(), 0);
    QCOMPARE(RawJpegPairs::find({raw, raw, jpeg, jpeg}), pairs);
    const auto grouped = RawJpegPairs::grouped({jpeg, raw}, pairs);
    QCOMPARE(grouped, QStringList{raw});
    QCOMPARE(RawJpegPairs::grouped({jpeg, raw}, pairs, jpeg), QStringList{jpeg});
  }

  // A pair whose two files carry different dates is one tile dated by the
  // RAW: the sidebar's and Browse's month counts (dateBuckets) and the
  // months Jump to date and the grid's sections list (SectionRows::months)
  // agree, and a date filter keeps or drops both files together. When the
  // pair is not folded, because a filter leaves only the JPEG or pairing is
  // off, the JPEG is its own tile and is filtered by its own date.
  void pairIsDatedOnceByItsRaw() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Pair.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("pair.jpg"));
    const QString other = makeJpeg(QStringLiteral("other.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty() && !other.isEmpty());
    const auto touch = [](const QString& path, const QDateTime& when) {
      QFile file(path);
      return file.open(QIODevice::ReadWrite) && file.setFileTime(when, QFileDevice::FileModificationTime);
    };
    QVERIFY(touch(raw, QDateTime(QDate(2024, 3, 10), QTime(12, 0))));
    QVERIFY(touch(jpeg, QDateTime(QDate(2025, 6, 15), QTime(12, 0))));
    QVERIFY(touch(other, QDateTime(QDate(2023, 1, 5), QTime(12, 0))));
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 5000);
    QTRY_COMPARE(proxy.count(), 2);
    QTRY_VERIFY(!model.metadataIndexing());
    const QDate rawDate = model.recordAt(model.rowOf(raw)).captured.date();
    const QDate jpegDate = model.recordAt(model.rowOf(jpeg)).captured.date();
    const QString rawMonth = rawDate.toString(QStringLiteral("yyyy-MM"));
    const QString jpegMonth = jpegDate.toString(QStringLiteral("yyyy-MM"));
    QVERIFY2(rawMonth != jpegMonth, "the pair's files must fall in different months");

    // Sidebar and Browse counts: the library's months.
    const auto bucketCounts = [&] {
      QMap<QString, int> counts;
      for (const QVariant& bucket : proxy.dateBuckets()) {
        const QVariantMap month = bucket.toMap();
        counts.insert(month.value(QStringLiteral("key")).toString(),
                      month.value(QStringLiteral("count")).toInt());
      }
      return counts;
    };
    // Jump to date and the grid: the sections of what is shown.
    auto* sections = qobject_cast<SectionRows*>(proxy.sectionRows());
    QVERIFY(sections);
    sections->setActive(true);
    const auto sectionCounts = [&] {
      sections->rebuildNow();
      QMap<QString, int> counts;
      for (const QVariant& entry : sections->months()) {
        const QVariantMap month = entry.toMap();
        counts[month.value(QStringLiteral("key")).toString()] +=
            month.value(QStringLiteral("count")).toInt();
      }
      return counts;
    };
    QTRY_COMPARE(bucketCounts().value(rawMonth), 1);
    QVERIFY(!bucketCounts().contains(jpegMonth));
    QCOMPARE(bucketCounts().size(), 2);
    QCOMPARE(sectionCounts(), bucketCounts());

    // Filtering to the RAW's month keeps the pair together, with its badge;
    // the JPEG's own month has nothing.
    proxy.setDateRange(rawDate.toString(Qt::ISODate), rawDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), raw);
    QCOMPARE(proxy.companionPathAt(0), jpeg);
    proxy.setDateRange(jpegDate.toString(Qt::ISODate), jpegDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 0);
    proxy.clearDateRange();

    // Only the JPEG passes another filter: it is its own tile, in its own
    // month, so the date filter tests its own date, never the RAW's.
    const auto shownMonthOf = [&](const QString& path) {
      sections->rebuildNow();
      const QVariantMap section = sections->sectionOfItem(proxy.rowOf(path));
      const int header = section.value(QStringLiteral("headerRow")).toInt();
      for (const QVariant& entry : sections->months()) {
        if (entry.toMap().value(QStringLiteral("headerRow")).toInt() == header)
          return entry.toMap().value(QStringLiteral("key")).toString();
      }
      return QString();
    };
    settings.setFavorite({jpeg}, true);
    proxy.setFavoritesOnly(true);
    QTRY_COMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    QVERIFY(proxy.companionPathAt(0).isEmpty());
    QCOMPARE(shownMonthOf(jpeg), jpegMonth);
    proxy.setDateRange(jpegDate.toString(Qt::ISODate), jpegDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    proxy.setDateRange(rawDate.toString(Qt::ISODate), rawDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 0);
    proxy.clearDateRange();
    proxy.setFavoritesOnly(false);
    settings.setFavorite({jpeg}, false);
    QTRY_COMPARE(proxy.count(), 2);

    // Opened as the JPEG alone: the JPEG stands for the pair and the tile is
    // dated by it, in the filter and the counts alike.
    proxy.setExplicitPaths({jpeg});
    QCOMPARE(proxy.rowOf(raw), -1);
    QCOMPARE(proxy.companionPathAt(proxy.rowOf(jpeg)), raw);
    QCOMPARE(shownMonthOf(jpeg), jpegMonth);
    QCOMPARE(bucketCounts().value(jpegMonth), 1);
    QVERIFY(!bucketCounts().contains(rawMonth));
    QCOMPARE(sectionCounts(), bucketCounts());
    proxy.setDateRange(jpegDate.toString(Qt::ISODate), jpegDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    proxy.setDateRange(rawDate.toString(Qt::ISODate), rawDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 0);
    proxy.clearDateRange();
    proxy.setExplicitPaths({});
    QCOMPARE(bucketCounts().value(rawMonth), 1);

    // The RAW's date read later, with a date range on: the JPEG follows
    // its RAW in and out of the range without anything else changing.
    const QDate later = rawDate.addMonths(-2);
    const QString laterMonth = later.toString(QStringLiteral("yyyy-MM"));
    QVERIFY(laterMonth != jpegMonth && laterMonth != rawMonth);
    proxy.setDateRange(later.toString(Qt::ISODate), later.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 0);
    const CaptureRecord& rawRecord = model.recordAt(model.rowOf(raw));
    model.applyMetadata({{raw, rawRecord.modified, rawRecord.bytes, QDateTime(later, QTime(9, 0)),
                          rawRecord.device, rawRecord.inode, {}, {}}});
    QTRY_COMPARE(model.recordAt(model.rowOf(raw)).captured.date(), later);
    QTRY_COMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), raw);
    QCOMPARE(proxy.companionPathAt(0), jpeg);
    QCOMPARE(proxy.rowOf(jpeg), -1);
    QTRY_COMPARE(bucketCounts().value(laterMonth), 1);
    QVERIFY(!bucketCounts().contains(rawMonth));
    proxy.clearDateRange();
    QCOMPARE(sectionCounts(), bucketCounts());
    const QString rawMonthNow = laterMonth;

    // Pairing off: two items, each by its own date, everywhere.
    proxy.setPairRawJpeg(false);
    QCOMPARE(proxy.count(), 3);
    QCOMPARE(bucketCounts().value(jpegMonth), 1);
    QCOMPARE(bucketCounts().value(rawMonthNow), 1);
    QCOMPARE(sectionCounts(), bucketCounts());
    proxy.setDateRange(jpegDate.toString(Qt::ISODate), jpegDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    proxy.clearDateRange();
    proxy.setPairRawJpeg(true);
    QCOMPARE(bucketCounts().value(rawMonthNow), 1);
    QVERIFY(!bucketCounts().contains(jpegMonth));

    // A hidden file is counted only while hidden files are shown.
    settings.setHidden({other}, true);
    QTRY_COMPARE(proxy.count(), 1);
    QTRY_COMPARE(bucketCounts().size(), 1);
    QCOMPARE(sectionCounts(), bucketCounts());
    proxy.setShowHidden(true);
    QCOMPARE(bucketCounts().size(), 2);
    QCOMPARE(sectionCounts(), bucketCounts());
  }

  // A RAW and a JPEG with one name in different folders are not a pair:
  // each is its own item, counted and filtered by its own date.
  void filesSplitAcrossFoldersKeepTheirOwnDates() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    QVERIFY(QDir().mkpath(m_media + QStringLiteral("/raws")));
    QVERIFY(QDir().mkpath(m_media + QStringLiteral("/jpegs")));
    const QString raw = makeRaw(QStringLiteral("raws/Split.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("jpegs/split.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty());
    const auto touch = [](const QString& path, const QDateTime& when) {
      QFile file(path);
      return file.open(QIODevice::ReadWrite) && file.setFileTime(when, QFileDevice::FileModificationTime);
    };
    QVERIFY(touch(raw, QDateTime(QDate(2024, 3, 10), QTime(12, 0))));
    QVERIFY(touch(jpeg, QDateTime(QDate(2025, 6, 15), QTime(12, 0))));
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 5000);
    QTRY_VERIFY(!model.metadataIndexing());
    QVERIFY(model.companionPathAt(model.rowOf(jpeg)).isEmpty());
    QTRY_COMPARE(proxy.count(), 2);
    const QDate jpegDate = model.recordAt(model.rowOf(jpeg)).captured.date();
    QTRY_VERIFY(!proxy.dateBuckets().isEmpty());
    int counted = 0;
    for (const QVariant& bucket : proxy.dateBuckets()) counted += bucket.toMap().value(QStringLiteral("count")).toInt();
    QCOMPARE(counted, 2);
    proxy.setDateRange(jpegDate.toString(Qt::ISODate), jpegDate.toString(Qt::ISODate), 0);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
  }

  void scannerKeepsBothPhysicalRowsAndRejectsAmbiguity() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty());
    auto records = CaptureScanner::scan({{m_media, 1}});
    QCOMPARE(records.size(), 2);
    for (const CaptureRecord& record : records) {
      QCOMPARE(record.companionPath, record.path == raw ? jpeg : raw);
    }
    QVERIFY(!makeJpeg(QStringLiteral("SHOT.jpeg")).isEmpty());
    records = CaptureScanner::scan({{m_media, 1}});
    QCOMPARE(records.size(), 3);
    for (const CaptureRecord& record : records) QVERIFY(record.companionPath.isEmpty());
  }

  void filtersChooseAnEligibleRepresentativeAndReviewsKeepBoth() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty());
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 5000);
    QTRY_COMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), raw);
    QCOMPARE(proxy.companionPathAt(0), jpeg);
    QCOMPARE(model.companionPathAt(model.rowOf(jpeg)), raw);
    const auto badge = [&](const QString& path) {
      return proxy.data(proxy.index(proxy.rowOf(path), 0), CaptureRoles::RawFormatRole).toString();
    };
    QCOMPARE(badge(raw), QStringLiteral("RAW+JPG"));
    QCOMPARE(model.data(model.index(model.rowOf(raw), 0), CaptureRoles::RawFormatRole).toString(),
             QStringLiteral("DNG")); // Source roles always describe the physical file.

    proxy.setExplicitPaths({jpeg});
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    QVERIFY(proxy.rowOf(jpeg) >= 0);
    QCOMPARE(proxy.rowOf(raw), -1);
    QCOMPARE(proxy.companionPathAt(0), raw);
    QCOMPARE(badge(jpeg), QStringLiteral("RAW+JPG"));
    proxy.setExplicitPaths({raw, jpeg});
    QCOMPARE(proxy.count(), 2);
    QVERIFY(proxy.rowOf(raw) >= 0 && proxy.rowOf(jpeg) >= 0);
    QCOMPARE(badge(raw), QStringLiteral("DNG"));
    QVERIFY(badge(jpeg).isEmpty());
    proxy.setExplicitPaths({raw});
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), raw);
    proxy.setExplicitPaths({jpeg});
    // An explicit JPEG that fails a filter must not hide a matching RAW.
    proxy.setSearchText(QStringLiteral("dng"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), raw);
    QCOMPARE(badge(raw), QStringLiteral("DNG"));
    proxy.setSearchText({});
    QCOMPARE(proxy.pathAt(0), jpeg);
    QCOMPARE(badge(jpeg), QStringLiteral("RAW+JPG"));
    proxy.setExplicitPaths({});
    QCOMPARE(proxy.pathAt(0), raw);

    proxy.setPairRawJpeg(false);
    QCOMPARE(proxy.count(), 2);
    QVERIFY(proxy.companionPathAt(0).isEmpty());
    QCOMPARE(badge(raw), QStringLiteral("DNG"));
    QVERIFY(badge(jpeg).isEmpty());
    proxy.setPairRawJpeg(true);
    QCOMPARE(proxy.count(), 1);

    settings.setFavorite({jpeg}, true);
    proxy.setFavoritesOnly(true);
    QTRY_COMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    QVERIFY(badge(jpeg).isEmpty());
    settings.setFavorite({raw}, true);
    QTRY_COMPARE(proxy.pathAt(0), raw);
    settings.setFavorite({raw}, false);
    QTRY_COMPARE(proxy.pathAt(0), jpeg);
    proxy.setFavoritesOnly(false);
    QCOMPARE(proxy.pathAt(0), raw);

    settings.setHidden({raw}, true);
    QTRY_COMPARE(proxy.pathAt(0), jpeg);
    proxy.setShowHidden(true);
    QCOMPARE(proxy.pathAt(0), raw);
    proxy.setShowHidden(false);
    QCOMPARE(proxy.pathAt(0), jpeg);
    settings.setHidden({raw}, false);
    QTRY_COMPARE(proxy.pathAt(0), raw);

    proxy.setAlbumFilter(QStringLiteral("JPEG album"), {jpeg});
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    proxy.setAlbumFilter({}, {});
    proxy.setSearchText(QStringLiteral("jpg"));
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(proxy.pathAt(0), jpeg);
    proxy.setSearchText({});
    settings.setCaption(jpeg, QStringLiteral("selected caption"));
    proxy.setSearchText(QStringLiteral("selected"));
    QTRY_COMPARE(proxy.pathAt(0), jpeg);
    proxy.setSearchText({});
    proxy.setTagFilter(QStringLiteral("JPEG tag"), {jpeg});
    QCOMPARE(proxy.pathAt(0), jpeg);
    proxy.setTagFilter({}, {});

    const QHash<QString, QString> groups{{raw, QStringLiteral("pair")},
                                        {jpeg, QStringLiteral("pair")}};
    proxy.setDuplicateGroups(groups);
    proxy.setDuplicatesOnly(true);
    QCOMPARE(proxy.count(), 2);
    QVERIFY(proxy.companionPathAt(0).isEmpty());
    QCOMPARE(badge(raw), QStringLiteral("DNG"));
    QVERIFY(badge(jpeg).isEmpty());
    proxy.setSimilarGroups(groups);
    proxy.setSimilarOnly(true);
    QCOMPARE(proxy.count(), 2);
    proxy.setSimilarOnly(false);
    QCOMPARE(proxy.count(), 1);
    QCOMPARE(model.rowCount(), 2);
  }

  void rescanClearsAndRestoresCompanions() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 5000);
    QTRY_COMPARE(proxy.count(), 1);
    const QString ambiguous = makeJpeg(QStringLiteral("SHOT.jpeg"));
    QVERIFY(!ambiguous.isEmpty());
    model.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 5000);
    QTRY_COMPARE(proxy.count(), 3);
    QVERIFY(model.companionPathAt(model.rowOf(raw)).isEmpty());
    QVERIFY(QFile::remove(ambiguous));
    model.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 2, 5000);
    QTRY_COMPARE(proxy.count(), 1);
    QVERIFY(QFile::remove(raw));
    model.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1, 5000);
    QTRY_COMPARE(proxy.pathAt(0), jpeg);
    QVERIFY(proxy.companionPathAt(0).isEmpty());
  }

  void viewerPreservesOpenedJpegToggleSwitchAndExplicitSelection() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    const QString other = makeJpeg(QStringLiteral("next.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty() && !other.isEmpty());
    ViewerSession viewer;
    viewer.open({jpeg});
    QCOMPARE(viewer.path(), jpeg);
    QTRY_COMPARE_WITH_TIMEOUT(viewer.count(), 2, 5000);
    QCOMPARE(viewer.path(), jpeg);
    QCOMPARE(viewer.companionPath(), raw);
    const quint64 revision = viewer.sequenceRevision();
    QVERIFY(viewer.switchCompanion());
    QCOMPARE(viewer.path(), raw);
    QCOMPARE(viewer.count(), 2);
    QCOMPARE(viewer.rawFormatAt(viewer.index()), QStringLiteral("DNG"));
    QVERIFY(viewer.sequenceRevision() > revision);
    viewer.setPairRawJpeg(false);
    QCOMPARE(viewer.count(), 3);
    QCOMPARE(viewer.path(), raw);
    QVERIFY(viewer.companionPath().isEmpty());
    viewer.jump(int(viewer.sequence().indexOf(jpeg)));
    viewer.setPairRawJpeg(true);
    QCOMPARE(viewer.count(), 2);
    QCOMPARE(viewer.path(), jpeg);

    const QStringList selected{jpeg, other, raw, jpeg};
    viewer.open(selected);
    QVERIFY(viewer.selection());
    QCOMPARE(viewer.sequence(), selected);
    QVERIFY(viewer.companionPath().isEmpty());
    QVERIFY(!viewer.switchCompanion());
    viewer.setPairRawJpeg(false);
    viewer.setPairRawJpeg(true);
    QCOMPARE(viewer.sequence(), selected);
    QCOMPARE(viewer.path(), jpeg);
  }

  void viewerHandlesAmbiguityRemovalAndRewrite() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    ViewerSession viewer;
    viewer.open({jpeg});
    QTRY_COMPARE_WITH_TIMEOUT(viewer.companionPath(), raw, 5000);
    QCOMPARE(viewer.count(), 1);
    const QString ambiguous = makeJpeg(QStringLiteral("SHOT.jpeg"));
    QVERIFY(!ambiguous.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(viewer.count(), 3, 5000);
    QCOMPARE(viewer.path(), jpeg);
    QVERIFY(viewer.companionPath().isEmpty());
    QVERIFY(QFile::remove(ambiguous));
    QTRY_COMPARE_WITH_TIMEOUT(viewer.count(), 1, 5000);
    QVERIFY(viewer.switchCompanion());
    const quint64 revision = viewer.sequenceRevision();
    const QString before = viewer.contentVersion();
    QFile replacement(raw);
    QVERIFY(replacement.open(QIODevice::Append));
    QCOMPARE(replacement.write("rewrite"), qint64(7));
    replacement.close();
    QTRY_VERIFY_WITH_TIMEOUT(viewer.contentVersion() != before, 5000);
    QVERIFY(viewer.sequenceRevision() > revision);
    QVERIFY(QFile::remove(raw));
    viewer.forget(raw);
    QCOMPARE(viewer.path(), jpeg);
    QCOMPARE(viewer.count(), 1);
    QVERIFY(viewer.companionPath().isEmpty());
    QVERIFY(QFile::exists(jpeg));
    QTest::qWait(600);
    QCOMPARE(viewer.path(), jpeg);
    QVERIFY(!viewer.sequence().contains(raw));
  }

  void viewerRejectsListingsFromEarlierOpens() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    QVERIFY(QDir().mkpath(m_media + QStringLiteral("/elsewhere")));
    const QString other = makeJpeg(QStringLiteral("elsewhere/alone.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty() && !other.isEmpty());
    ViewerSession viewer;
    QSignalSpy revisions(&viewer, &ViewerSession::sequenceRevisionChanged);
    viewer.open({jpeg});
    viewer.open({other});
    QTRY_VERIFY_WITH_TIMEOUT(revisions.count() >= 3, 5000);
    QCOMPARE(viewer.sequence(), QStringList{other});
    QVERIFY(viewer.companionPath().isEmpty());
    viewer.open({jpeg});
    const QStringList selected{other, jpeg, raw};
    viewer.open(selected);
    QTest::qWait(600);
    QCOMPARE(viewer.sequence(), selected);
    QCOMPARE(viewer.path(), other);
    QVERIFY(viewer.selection());
  }

  void viewerCoalescesOverlappingRelistsWithinOneOpen() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty());
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    HeldWorker held;
    QVERIFY(held.started());
    ViewerSession viewer;
    QSignalSpy revisions(&viewer, &ViewerSession::sequenceRevisionChanged);
    viewer.open({jpeg}); // Initial listing is queued, not completed.
    const QString ambiguous = makeJpeg(QStringLiteral("SHOT.jpeg"));
    QVERIFY(!ambiguous.isEmpty());
    QTest::qWait(650); // Directory watch asks to relist during the first request.
    QVERIFY(QFile::remove(ambiguous));
    QTest::qWait(650); // A third request supersedes that relist.
    QCOMPARE(revisions.count(), 1);
    held.release();
    QTRY_COMPARE_WITH_TIMEOUT(viewer.companionPath(), raw, 5000);
    QCOMPARE(viewer.count(), 1);
    QCOMPARE(viewer.path(), jpeg);
    QCOMPARE(revisions.count(), 2); // Only the newest request was applied.
  }

  void rawHeaderCompletionAfterNavigationIsDiscarded() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty());
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    HeldWorker held;
    QVERIFY(held.started());
    ViewerSession viewer;
    QSignalSpy sizes(&viewer, &ViewerSession::rawSizeChanged);
    viewer.open({raw, jpeg});
    QVERIFY(viewer.jump(1));
    held.release();
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::processEvents();
    QCOMPARE(viewer.path(), jpeg);
    QVERIFY(!viewer.rawSize().isValid());
    QCOMPARE(sizes.count(), 0);
  }

  void rawHeaderCompletionAfterReplacementUsesOnlyNewVersion() {
    if (!CameraRaw::isRaw(QStringLiteral("dng"))) QSKIP("Qt RAW support is unavailable");
    const QString raw = makeRaw(QStringLiteral("Shot.DNG"));
    const QString jpeg = makeJpeg(QStringLiteral("shot.jpg"));
    QVERIFY(!raw.isEmpty() && !jpeg.isEmpty());
    const QSize expected = CameraRaw::fullSize(raw);
    QVERIFY(expected.isValid());
    QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    HeldWorker held;
    QVERIFY(held.started());
    ViewerSession viewer;
    QSignalSpy sizes(&viewer, &ViewerSession::rawSizeChanged);
    viewer.open({raw, jpeg});
    const QString before = viewer.contentVersion();
    QFile original(raw);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray bytes = original.readAll();
    original.close();
    QSaveFile replacement(raw);
    QVERIFY(replacement.open(QIODevice::WriteOnly));
    QCOMPARE(replacement.write(bytes), qint64(bytes.size()));
    QCOMPARE(replacement.write("replacement"), qint64(11));
    QVERIFY(replacement.commit());
    viewer.open({raw, jpeg});
    QVERIFY(viewer.contentVersion() != before);
    held.release();
    QTRY_COMPARE_WITH_TIMEOUT(viewer.rawSize(), expected, 5000);
    QCOMPARE(sizes.count(), 1); // The obsolete probe did not publish a size.
  }

private:
  QString makeRaw(const QString& name) const {
    const QString fixture = QFINDTESTDATA("fixtures/raw/camera.dng");
    const QString path = m_media + QLatin1Char('/') + name;
    return QFile::copy(fixture, path) ? path : QString();
  }

  QString makeJpeg(const QString& name) const {
    QImage image(8, 6, QImage::Format_RGB32);
    image.fill(Qt::green);
    const QString path = m_media + QLatin1Char('/') + name;
    return image.save(path, "JPEG") ? path : QString();
  }

  std::unique_ptr<QTemporaryDir> m_scratch;
  QString m_media;
};

int main(int argc, char* argv[]) {
  QTemporaryDir config;
  if (!config.isValid()) return 1;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QCoreApplication application(argc, argv);
  CompanionTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}

#include "tst_companions.moc"
