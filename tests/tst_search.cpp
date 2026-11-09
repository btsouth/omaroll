#include "app/AppSettings.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "search/DocumentTextIndex.h"
#include "search/SearchQuery.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <ctime>
#include <limits>
#include <memory>

#if defined(__SANITIZE_ADDRESS__)
#define OMAROLL_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OMAROLL_SANITIZED 1
#endif
#endif
#ifndef OMAROLL_SANITIZED
#define OMAROLL_SANITIZED 0
#endif

namespace {

QStringList names(const CaptureFilterModel& proxy) {
  QStringList result;
  for (int row = 0; row < proxy.count(); ++row) result.append(proxy.fileNameAt(row));
  result.sort();
  return result;
}

// CPU time of this thread, so a busy machine that delays the test does not
// count against the search.
qint64 threadMicros() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return qint64(now.tv_sec) * 1'000'000 + now.tv_nsec / 1000;
}

QStringList sorted(QStringList values) {
  values.sort();
  return values;
}

} // namespace

class SearchTest final : public QObject {
  Q_OBJECT

private slots:
  void init() {
    m_scratch = std::make_unique<QTemporaryDir>();
    QVERIFY(m_scratch->isValid());
    // Scanned roots stay empty; the library comes from records, and the files
    // they name live elsewhere so no rescan replaces them.
    m_media = m_scratch->filePath(QStringLiteral("media"));
    m_files = m_scratch->filePath(QStringLiteral("files"));
    QVERIFY(QDir().mkpath(m_media));
    QVERIFY(QDir().mkpath(m_files));
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_scratch->filePath(QStringLiteral("config")));
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch->filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("XDG_DATA_HOME", m_scratch->filePath(QStringLiteral("data")).toUtf8()));
    for (const char* variable : {"OMARCHY_SCREENSHOT_DIR", "OMARCHY_SCREENRECORD_DIR",
                                 "XDG_PICTURES_DIR", "XDG_VIDEOS_DIR", "XDG_DOWNLOAD_DIR"}) {
      QVERIFY(qputenv(variable, m_media.toUtf8()));
    }
  }

  void foldingIgnoresCaseAndAccents() {
    QCOMPARE(SearchText::fold(QStringLiteral("Café-Paris.JPG")), QStringLiteral("cafe-paris.jpg"));
    QCOMPARE(SearchText::fold(QStringLiteral("CAFÉ")), QStringLiteral("cafe"));
    QCOMPARE(SearchText::fold(QStringLiteral("Zoë Ångström")), QStringLiteral("zoe angstrom"));
    // Compatibility forms fold too: full-width letters and ligatures.
    QCOMPARE(SearchText::fold(QStringLiteral("ＦＵＪＩ ﬁle")), QStringLiteral("fuji file"));
    // Already decomposed input loses its combining marks.
    QCOMPARE(SearchText::fold(QStringLiteral("café")), QStringLiteral("cafe"));
    // Nothing to fold: the same string comes back, shared.
    const QString plain = QStringLiteral("already-plain.png");
    QCOMPARE(SearchText::fold(plain).constData(), plain.constData());

    QList<qsizetype> sources;
    const QString folded = SearchText::fold(QStringLiteral("ﬁ Café"), &sources);
    QCOMPARE(folded, QStringLiteral("fi cafe"));
    QCOMPARE(sources, (QList<qsizetype>{0, 0, 1, 2, 3, 4, 5}));

    QVERIFY(SearchText::containsWordStart(QStringLiteral("fujifilm x-t4"), QStringLiteral("x-t")));
    QVERIFY(SearchText::containsWordStart(QStringLiteral("travel/japan"), QStringLiteral("jap")));
    QVERIFY(!SearchText::containsWordStart(QStringLiteral("party"), QStringLiteral("art")));
    QVERIFY(SearchText::containsWordStart(QStringLiteral("party art"), QStringLiteral("art")));
  }

  void queryParsesTokensPhrasesAndExclusions() {
    const SearchQuery query = SearchQuery::parse(
        QStringLiteral("tag:Travel \"Road  Trip\" -draft -tag:work KIND:PDF before:2024 "
                       "after:2023-06 note:x before:soon camera:\"Sony A7\" tag: -\"old copy\""));
    QCOMPARE(query.terms.size(), 11);
    const auto& terms = query.terms;
    QCOMPARE(terms[0].field, SearchQuery::Tag);
    QCOMPARE(terms[0].value, QStringLiteral("travel"));
    QCOMPARE(terms[1].field, SearchQuery::Text);
    QCOMPARE(terms[1].value, QStringLiteral("road trip"));
    QVERIFY(terms[1].phrase);
    QCOMPARE(terms[2].value, QStringLiteral("draft"));
    QVERIFY(terms[2].negated);
    QCOMPARE(terms[3].field, SearchQuery::Tag);
    QVERIFY(terms[3].negated);
    QCOMPARE(terms[4].field, SearchQuery::Type);
    QCOMPARE(terms[4].type, SearchQuery::Pdf);
    QCOMPARE(terms[5].field, SearchQuery::Before);
    QCOMPARE(terms[5].date, QDate(2024, 1, 1));
    QCOMPARE(terms[6].field, SearchQuery::After);
    QCOMPARE(terms[6].date, QDate(2023, 6, 30));
    // Unknown fields and values that do not parse are plain text.
    QCOMPARE(terms[7].field, SearchQuery::Text);
    QCOMPARE(terms[7].value, QStringLiteral("note:x"));
    QCOMPARE(terms[8].field, SearchQuery::Text);
    QCOMPARE(terms[8].value, QStringLiteral("before:soon"));
    QCOMPARE(terms[9].field, SearchQuery::Camera);
    QCOMPARE(terms[9].value, QStringLiteral("sony a7"));
    // "tag:" with no value yet narrows nothing and is left out.
    QCOMPARE(terms[10].value, QStringLiteral("old copy"));
    QVERIFY(terms[10].negated);
    QCOMPARE(query.textValues(), QStringList({QStringLiteral("road trip"), QStringLiteral("note:x"),
                                              QStringLiteral("before:soon")}));
    QCOMPARE(query.textValues(true).size(), 5);

    QCOMPARE(SearchQuery::parse(QStringLiteral("after:2024-02")).terms.first().date,
             QDate(2024, 2, 29));
    QCOMPARE(SearchQuery::parse(QStringLiteral("before:2024/06/15")).terms.first().date,
             QDate(2024, 6, 15));
    QCOMPARE(SearchQuery::parse(QStringLiteral("after:2024-13")).terms.first().field,
             SearchQuery::Text);
    QCOMPARE(SearchQuery::parse(QStringLiteral("type:banana")).terms.first().field,
             SearchQuery::Text);
    QVERIFY(SearchQuery::parse(QStringLiteral("  -  ")).isEmpty());
    QVERIFY(!SearchQuery::parse(QStringLiteral("type:video")).hasText());
    const SearchQuery home =
        SearchQuery::parse(QStringLiteral("in:") + QDir::homePath() + QStringLiteral("/Pictures"));
    QCOMPARE(home.terms.first().value, QStringLiteral("~/pictures"));
    // An unclosed quote runs to the end, as it does while being typed.
    QCOMPARE(SearchQuery::parse(QStringLiteral("\"half typed")).terms.first().value,
             QStringLiteral("half typed"));
  }

  void searchMatchesEveryFieldAndToken() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), 7);

    QVERIFY(settings.createTag(QStringLiteral("Wanderlust/Fjords")));
    QVERIFY(settings.addTag(QStringLiteral("Wanderlust/Fjords"), {path("Iceland/waterfall.jpg")}));
    QVERIFY(settings.createAlbum(QStringLiteral("Weekend Café")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Weekend Café"),
                                {path("Iceland/café-paris.jpg"), path("Iceland/draft copy.jpg")}));
    // Picture text and PDF text arrive the same way.
    proxy.setOcrText(path("Shots/screenshot-2023-03-01.png"),
                     QStringLiteral("Invoice 42 total due at CAFÉ Nord"));
    proxy.setOcrText(path("Docs/report.pdf"), QStringLiteral("Quarterly résumé summary"));

    const auto expect = [&](const QString& query, const QStringList& expected) {
      proxy.setSearchText(query);
      const QStringList actual = names(proxy);
      if (actual != sorted(expected)) {
        qWarning().noquote() << "query" << query << "gave" << actual;
      }
      QCOMPARE(actual, sorted(expected));
    };

    // Folding on both sides, and every plain field.
    expect(QStringLiteral("cafe"), {QStringLiteral("café-paris.jpg"),
                                    QStringLiteral("screenshot-2023-03-01.png"),
                                    QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("CAFÉ"), {QStringLiteral("café-paris.jpg"),
                                    QStringLiteral("screenshot-2023-03-01.png"),
                                    QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("zoe"), {QStringLiteral("café-paris.jpg")});
    expect(QStringLiteral("iceland"), {QStringLiteral("café-paris.jpg"),
                                       QStringLiteral("waterfall.jpg"),
                                       QStringLiteral("draft copy.jpg"),
                                       QStringLiteral("IMG_0001.dng")});
    expect(QStringLiteral("fujifilm"), {QStringLiteral("café-paris.jpg")});
    expect(QStringLiteral("24-70"), {QStringLiteral("waterfall.jpg")});
    expect(QStringLiteral("fjords"), {QStringLiteral("waterfall.jpg")});
    expect(QStringLiteral("resume"), {QStringLiteral("report.pdf")});
    expect(QStringLiteral("iceland fuji"), {QStringLiteral("café-paris.jpg")});

    // Field tokens match the start of a word in their own field only.
    expect(QStringLiteral("tag:wanderlust"), {QStringLiteral("waterfall.jpg")});
    expect(QStringLiteral("tag:fjords"), {QStringLiteral("waterfall.jpg")});
    expect(QStringLiteral("tag:ords"), {});
    expect(QStringLiteral("tag:iceland"), {});
    expect(QStringLiteral("album:weekend"), {QStringLiteral("café-paris.jpg"),
                                             QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("album:\"weekend cafe\""), {QStringLiteral("café-paris.jpg"),
                                                      QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("in:iceland"), {QStringLiteral("café-paris.jpg"),
                                          QStringLiteral("waterfall.jpg"),
                                          QStringLiteral("draft copy.jpg"),
                                          QStringLiteral("IMG_0001.dng")});
    expect(QStringLiteral("in:shots"), {QStringLiteral("screenshot-2023-03-01.png")});
    expect(QStringLiteral("camera:fuji"), {QStringLiteral("café-paris.jpg")});
    expect(QStringLiteral("camera:sony lens:24-70"), {QStringLiteral("waterfall.jpg")});
    expect(QStringLiteral("lens:xf35"), {QStringLiteral("café-paris.jpg")});
    expect(QStringLiteral("camera:invoice"), {});
    expect(QStringLiteral("type:video"), {QStringLiteral("clip.mp4")});
    expect(QStringLiteral("kind:pdf"), {QStringLiteral("report.pdf")});
    expect(QStringLiteral("type:screenshot"), {QStringLiteral("screenshot-2023-03-01.png")});
    expect(QStringLiteral("type:raw"), {QStringLiteral("IMG_0001.dng")});
    expect(QStringLiteral("type:animated"), {QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("type:photo"), {QStringLiteral("café-paris.jpg"),
                                          QStringLiteral("waterfall.jpg"),
                                          QStringLiteral("draft copy.jpg"),
                                          QStringLiteral("IMG_0001.dng")});

    // Dates: before is earlier than the whole period, after is later.
    expect(QStringLiteral("before:2024"), {QStringLiteral("screenshot-2023-03-01.png"),
                                           QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("after:2024"), {QStringLiteral("waterfall.jpg")});
    expect(QStringLiteral("after:2023 before:2025"), {QStringLiteral("café-paris.jpg"),
                                                      QStringLiteral("report.pdf"),
                                                      QStringLiteral("clip.mp4"),
                                                      QStringLiteral("IMG_0001.dng")});
    expect(QStringLiteral("after:2024-02-28 before:2024-03"), {QStringLiteral("report.pdf")});
    expect(QStringLiteral("before:2024-06-15"), {QStringLiteral("screenshot-2023-03-01.png"),
                                                 QStringLiteral("report.pdf"),
                                                 QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("after:2024-06-15"), {QStringLiteral("waterfall.jpg"),
                                                QStringLiteral("clip.mp4"),
                                                QStringLiteral("IMG_0001.dng")});

    // Phrases are contiguous; single words are not.
    expect(QStringLiteral("\"invoice 42\""), {QStringLiteral("screenshot-2023-03-01.png")});
    expect(QStringLiteral("\"42 invoice\""), {});
    expect(QStringLiteral("42 invoice"), {QStringLiteral("screenshot-2023-03-01.png")});
    expect(QStringLiteral("\"draft copy\""), {QStringLiteral("draft copy.jpg")});

    // Exclusion, of words, phrases and fields.
    expect(QStringLiteral("-iceland"), {QStringLiteral("screenshot-2023-03-01.png"),
                                        QStringLiteral("report.pdf"),
                                        QStringLiteral("clip.mp4")});
    expect(QStringLiteral("in:iceland -tag:wanderlust"), {QStringLiteral("café-paris.jpg"),
                                                          QStringLiteral("draft copy.jpg"),
                                                          QStringLiteral("IMG_0001.dng")});
    expect(QStringLiteral("in:iceland -\"draft copy\""), {QStringLiteral("café-paris.jpg"),
                                                          QStringLiteral("waterfall.jpg"),
                                                          QStringLiteral("IMG_0001.dng")});
    expect(QStringLiteral("in:iceland -type:raw"), {QStringLiteral("café-paris.jpg"),
                                                    QStringLiteral("waterfall.jpg"),
                                                    QStringLiteral("draft copy.jpg")});
    expect(QStringLiteral("-cafe -invoice -iceland"), {QStringLiteral("report.pdf"),
                                                       QStringLiteral("clip.mp4")});

    // Unknown tokens are plain text; a field without a value narrows nothing.
    expect(QStringLiteral("note:x"), {QStringLiteral("clip.mp4")});
    expect(QStringLiteral("before:soon"), {});
    proxy.setSearchText(QStringLiteral("tag:"));
    QCOMPARE(proxy.count(), 7);

    // Only plain words start text indexing.
    proxy.setSearchText(QStringLiteral("tag:x type:video cafe -draft"));
    QCOMPARE(proxy.textSearch(), QStringLiteral("cafe draft"));
    proxy.setSearchText(QStringLiteral("type:video after:2020"));
    QVERIFY(proxy.textSearch().isEmpty());

    // The snippet shows the recognized text where the folded match was.
    proxy.setSearchText(QStringLiteral("cafe"));
    const int shot = proxy.rowOf(path("Shots/screenshot-2023-03-01.png"));
    QVERIFY(shot >= 0);
    QVERIFY2(proxy.ocrSnippetAt(shot).contains(QStringLiteral("CAFÉ Nord")),
             qPrintable(proxy.ocrSnippetAt(shot)));

    // Organization changes reach an active search.
    proxy.setSearchText(QStringLiteral("tag:work"));
    QCOMPARE(proxy.count(), 0);
    QVERIFY(settings.createTag(QStringLiteral("Work")));
    QVERIFY(settings.addTag(QStringLiteral("Work"), {path("Clips/clip.mp4")}));
    QTRY_COMPARE(names(proxy), QStringList{QStringLiteral("clip.mp4")});
    proxy.setSearchText(QStringLiteral("work"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("clip.mp4")});
    settings.removeTag(QStringLiteral("Work"), {path("Clips/clip.mp4")});
    QTRY_COMPARE(proxy.count(), 0);

    // A caption edit reaches it as well.
    settings.setCaption(path("Docs/report.pdf"), QStringLiteral("Board meeting"));
    proxy.setSearchText(QStringLiteral("board"));
    QTRY_COMPARE(names(proxy), QStringList{QStringLiteral("report.pdf")});
  }

  void suggestionsOfferValuesForTheTokenAtTheCursor() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(settings.createTag(QStringLiteral("Wanderlust/Fjords")));
    QVERIFY(settings.createAlbum(QStringLiteral("Weekend Café")));
    QTRY_VERIFY(!proxy.cameras().isEmpty());

    const auto inserts = [&](const QString& text, int cursor = -1) {
      QStringList values;
      for (const QVariant& row :
           proxy.searchSuggestions(text, cursor < 0 ? int(text.size()) : cursor)) {
        values.append(row.toMap().value(QStringLiteral("insert")).toString());
      }
      return values;
    };
    QCOMPARE(inserts(QStringLiteral("tag:wan")),
             QStringList({QStringLiteral("tag:Wanderlust"), QStringLiteral("tag:Wanderlust/Fjords")}));
    QCOMPARE(inserts(QStringLiteral("tag:fjo")), QStringList{QStringLiteral("tag:Wanderlust/Fjords")});
    // Values with spaces come back quoted; accents and case are ignored.
    QCOMPARE(inserts(QStringLiteral("album:\"weekend ca")),
             QStringList{QStringLiteral("album:\"Weekend Café\"")});
    QCOMPARE(inserts(QStringLiteral("-camera:FU")),
             QStringList{QStringLiteral("-camera:\"FUJIFILM X-T4\"")});
    QCOMPARE(inserts(QStringLiteral("lens:fe")),
             QStringList{QStringLiteral("lens:\"FE 24-70mm F2.8 GM\"")});
    QCOMPARE(inserts(QStringLiteral("type:v")), QStringList{QStringLiteral("type:video")});
    QVERIFY(inserts(QStringLiteral("type:video")).isEmpty());
    QVERIFY(inserts(QStringLiteral("in:icel")).first().endsWith(QStringLiteral("/Iceland")));
    QVERIFY(inserts(QStringLiteral("after:")).contains(QStringLiteral("after:2024")));
    QVERIFY(inserts(QStringLiteral("before:2024-")).contains(QStringLiteral("before:2024-06")));
    // A word that starts a field name offers the field itself.
    const QVariantList fields = proxy.searchSuggestions(QStringLiteral("cam"), 3);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.first().toMap().value(QStringLiteral("insert")).toString(),
             QStringLiteral("camera:"));
    QVERIFY(!fields.first().toMap().value(QStringLiteral("complete")).toBool());
    QVERIFY(inserts(QStringLiteral("zz")).isEmpty());
    QVERIFY(inserts(QStringLiteral("note:x")).isEmpty());

    // Mid-text: the token under the cursor is replaced as a whole.
    const QString text = QStringLiteral("cafe tag:wa fuji");
    const QVariantMap middle = proxy.searchSuggestions(text, 11).first().toMap();
    QCOMPARE(middle.value(QStringLiteral("start")).toInt(), 5);
    QCOMPARE(middle.value(QStringLiteral("end")).toInt(), 11);
    QCOMPARE(inserts(text, 9).first(), QStringLiteral("tag:Wanderlust"));
  }

  void browseFacetsCombineAndSavedViewsKeepTokens() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(settings.createAlbum(QStringLiteral("Weekend Café")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Weekend Café"),
                                {path("Iceland/café-paris.jpg"), path("Iceland/draft copy.jpg"),
                                 path("Clips/clip.mp4")}));

    // Folder, date and camera narrow together; clearing one leaves the rest.
    proxy.setFolderFilter(m_files + QStringLiteral("/Iceland"));
    QCOMPARE(proxy.count(), 4);
    proxy.setDateRange(QStringLiteral("2024-01-01"), QStringLiteral("2024-12-31"));
    QCOMPARE(names(proxy), sorted({QStringLiteral("café-paris.jpg"), QStringLiteral("IMG_0001.dng")}));
    proxy.setCameraFilter(QStringLiteral("FUJIFILM X-T4"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("café-paris.jpg")});
    proxy.clearDateRange();
    QCOMPARE(names(proxy), QStringList{QStringLiteral("café-paris.jpg")});
    QCOMPARE(proxy.folderFilter(), m_files + QStringLiteral("/Iceland"));
    proxy.setCameraFilter({});
    QCOMPARE(proxy.count(), 4);

    // An album combines with the folder and with a token query.
    proxy.setAlbumFilter(QStringLiteral("Weekend Café"),
                         settings.albumPaths(QStringLiteral("Weekend Café")));
    QCOMPARE(names(proxy), sorted({QStringLiteral("café-paris.jpg"), QStringLiteral("draft copy.jpg")}));
    proxy.setSearchText(QStringLiteral("-type:animated after:2023"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("café-paris.jpg")});

    // A saved view keeps the token query and every facet, album included.
    const QVariantMap view = proxy.currentView();
    QCOMPARE(view.value(QStringLiteral("search")).toString(),
             QStringLiteral("-type:animated after:2023"));
    QCOMPARE(view.value(QStringLiteral("album")).toString(), QStringLiteral("Weekend Café"));
    QVERIFY(settings.saveSmartCollection(QStringLiteral("Paris trip"), view));
    proxy.setSearchText({});
    proxy.setAlbumFilter({}, {});
    proxy.setFolderFilter({});
    QCOMPARE(proxy.count(), 7);
    proxy.applyView(QStringLiteral("Paris trip"),
                    settings.smartCollection(QStringLiteral("Paris trip")));
    QCOMPARE(proxy.smartCollectionFilter(), QStringLiteral("Paris trip"));
    QCOMPARE(proxy.searchText(), QStringLiteral("-type:animated after:2023"));
    QCOMPARE(proxy.albumFilter(), QStringLiteral("Weekend Café"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("café-paris.jpg")});
    // Album membership is read when the view opens, so it follows additions.
    QVERIFY(settings.addToAlbum(QStringLiteral("Weekend Café"), {path("Iceland/waterfall.jpg")}));
    proxy.applyView(QStringLiteral("Paris trip"),
                    settings.smartCollection(QStringLiteral("Paris trip")));
    QCOMPARE(names(proxy), sorted({QStringLiteral("café-paris.jpg"), QStringLiteral("waterfall.jpg")}));
    // Renaming the album carries the saved view along.
    QVERIFY(settings.renameAlbum(QStringLiteral("Weekend Café"), QStringLiteral("Paris")));
    QCOMPARE(settings.smartCollection(QStringLiteral("Paris trip")).value(QStringLiteral("album")).toString(),
             QStringLiteral("Paris"));
  }

  void pdfTextIsExtractedOffTheGuiThreadAndCached() {
    // A stand-in pdftotext that logs each call, so caching is observable.
    const QString bin = m_scratch->filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    const QString log = m_scratch->filePath(QStringLiteral("calls"));
    QFile helper(bin + QStringLiteral("/pdftotext"));
    QVERIFY(helper.open(QIODevice::WriteOnly));
    helper.write("#!/bin/sh\necho \"$@\" >> '" + log.toUtf8() +
                 "'\nprintf 'Quarterly r\\303\\251sum\\303\\251\\fsecond   page'\n");
    helper.close();
    QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    const QByteArray oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + oldPath));
    const auto calls = [&] {
      QFile file(log);
      return file.open(QIODevice::ReadOnly) ? file.readAll().split('\n').size() - 1 : 0;
    };

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);

    {
      DocumentTextIndex index(&model);
      connect(&index, &DocumentTextIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);
      // No search text, no background: nothing is read.
      QTest::qWait(100);
      QCOMPARE(calls(), 0);
      proxy.setSearchText(QStringLiteral("resume second page"));
      index.setSearchText(proxy.textSearch());
      QTRY_COMPARE(names(proxy), QStringList{QStringLiteral("report.pdf")});
      QTRY_VERIFY(!index.indexing());
      // Only the document was read, once, and the page count is bounded.
      QCOMPARE(calls(), 1);
      QFile file(log);
      QVERIFY(file.open(QIODevice::ReadOnly));
      QVERIFY(file.readAll().contains("-l " + QByteArray::number(DocumentTextIndex::kPageLimit)));
    }

    // A fresh index reads the cache rather than the document.
    proxy.setSearchText({});
    proxy.setOcrText(path("Docs/report.pdf"), {});
    {
      DocumentTextIndex index(&model);
      connect(&index, &DocumentTextIndex::textReady, &proxy, &CaptureFilterModel::setOcrText);
      index.setBackground(true);
      proxy.setSearchText(QStringLiteral("quarterly"));
      QTRY_COMPARE(names(proxy), QStringList{QStringLiteral("report.pdf")});
      QCOMPARE(calls(), 1);

      // A changed document is read again, and its old text dropped first.
      QList<CaptureRecord> records = library();
      for (CaptureRecord& record : records) {
        if (record.document) record.modified += 1000;
      }
      model.setRecordsForTesting(records);
      QTRY_COMPARE(calls(), 2);
      QTRY_VERIFY(!index.indexing());
      QCOMPARE(names(proxy), QStringList{QStringLiteral("report.pdf")});

      QVERIFY(index.clearCache() >= 1);
      QTRY_COMPARE(proxy.count(), 0);
      QVERIFY(QDir(DocumentTextIndex::cacheDirectory())
                  .entryList({QStringLiteral("*.txt")}, QDir::Files)
                  .isEmpty());
    }
  }

  void searchStaysFastInAFiftyThousandItemLibrary() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());

    constexpr int kItems = 50'000;
    QList<CaptureRecord> records;
    records.reserve(kItems);
    const QDateTime start(QDate(2020, 1, 1), QTime(9, 0));
    const QStringList cameras{QStringLiteral("Sony ILCE-7M3"), QStringLiteral("FUJIFILM X-T4"),
                              QStringLiteral("Canon EOS R6")};
    for (int index = 0; index < kItems; ++index) {
      CaptureRecord record;
      const bool shot = index % 3 == 0;
      record.fileName = shot ? QStringLiteral("Screenshot %1.png").arg(index)
                             : QStringLiteral("IMG_%1.JPG").arg(index, 5, 10, QLatin1Char('0'));
      record.path = QStringLiteral("/library/Pictures/Trip %1/").arg(index / 500) + record.fileName;
      record.kind = shot ? CaptureRecord::Screenshot : CaptureRecord::Picture;
      record.captured = start.addSecs(qint64(index) * 3600);
      record.modified = record.captured.toMSecsSinceEpoch();
      record.bytes = 1000 + index;
      if (!shot) record.camera = cameras.at((index / 3) % cameras.size());
      if (index % 50 == 0) record.caption = QStringLiteral("Café stop number %1").arg(index);
      records.append(record);
    }
    model.setRecordsForTesting(records);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QCOMPARE(proxy.count(), kItems);
    for (int index = 0; index < kItems; index += 3) {
      proxy.setOcrText(records.at(index).path,
                       QStringLiteral("Terminal output line %1 build passed").arg(index));
    }
    proxy.setOcrText(records.at(300).path, QStringLiteral("Invoice 4242 overdue"));
    QCoreApplication::processEvents();

    // Type a word one character at a time, then take it back again. The
    // best of three runs is kept, so a busy machine does not fail the check.
    const QString word = QStringLiteral("invoice");
    QStringList steps;
    for (int length = 1; length <= word.size(); ++length) steps.append(word.left(length));
    for (int length = word.size() - 1; length >= 0; --length) steps.append(word.left(length));
    steps.append({QStringLiteral("camera:sony"), QStringLiteral("camera:sony after:2023"),
                  QStringLiteral("camera:sony after:2023 -cafe"),
                  QStringLiteral("\"trip 1\" type:photo"), QString()});
    QList<qint64> times;
    qint64 bestMedian = std::numeric_limits<qint64>::max();
    for (int attempt = 0; attempt < 3; ++attempt) {
      QList<qint64> run;
      QElapsedTimer timer;
      for (const QString& step : std::as_const(steps)) {
        timer.start();
        const qint64 cpu = threadMicros();
        proxy.setSearchText(step);
        // A reset maps rows lazily; the grid asks for them at once.
        (void)proxy.index(0, 0);
        run.append(threadMicros() - cpu);
        if (attempt == 0) {
          qInfo().noquote() << QStringLiteral("  %1 ms CPU  %2 ms wall  %3 rows  \"%4\"")
                                   .arg(run.last() / 1000.0, 6, 'f', 1)
                                   .arg(timer.nsecsElapsed() / 1e6, 6, 'f', 1)
                                   .arg(proxy.count())
                                   .arg(step);
        }
      }
      QList<qint64> ordered = run.mid(1);
      std::sort(ordered.begin(), ordered.end());
      if (ordered.at(ordered.size() / 2) < bestMedian) {
        bestMedian = ordered.at(ordered.size() / 2);
        times = run;
      }
    }
    proxy.setSearchText(QStringLiteral("invoice"));
    QCOMPARE(proxy.count(), 1);
    proxy.setSearchText(QStringLiteral("camera:sony after:2023 -cafe"));
    QVERIFY(proxy.count() > 0);
    proxy.setSearchText({});

    // The first keystroke also folds every row's fields once.
    const qint64 first = times.first();
    QList<qint64> rest = times.mid(1);
    std::sort(rest.begin(), rest.end());
    const qint64 median = rest.at(rest.size() / 2);
    const qint64 slowest = rest.last();
    qInfo().noquote() << QStringLiteral("50k search CPU: first %1 ms, median %2 ms, slowest %3 ms over %4 keystrokes")
                             .arg(first / 1000.0, 0, 'f', 1)
                             .arg(median / 1000.0, 0, 'f', 1)
                             .arg(slowest / 1000.0, 0, 'f', 1)
                             .arg(times.size());
    // Release builds target 30 ms per keystroke; sanitizers are several
    // times slower, and debug builds without them sit in between.
#if OMAROLL_SANITIZED
    const qint64 budget = 600'000;
#elif defined(NDEBUG)
    const qint64 budget = 30'000;
#else
    const qint64 budget = 150'000;
#endif
    QVERIFY2(median < budget, qPrintable(QString::number(median)));
    QVERIFY2(slowest < budget * 3, qPrintable(QString::number(slowest)));
    QVERIFY2(first < budget * 3, qPrintable(QString::number(first)));
  }

private:
  QString path(const char* relative) const {
    return m_files + QLatin1Char('/') + QString::fromUtf8(relative);
  }

  CaptureRecord record(const char* relative, CaptureRecord::Kind kind, QDate date) const {
    CaptureRecord result;
    result.path = path(relative);
    result.fileName = QFileInfo(result.path).fileName();
    result.kind = kind;
    result.captured = QDateTime(date, QTime(12, 0));
    // Albums and tags identify files by their content, so each one exists.
    QDir().mkpath(QFileInfo(result.path).absolutePath());
    QFile file(result.path);
    if (file.open(QIODevice::WriteOnly)) file.write(QByteArray(relative));
    file.close();
    const QFileInfo info(result.path);
    result.bytes = info.size();
    result.modified = info.lastModified().toMSecsSinceEpoch();
    return result;
  }

  QList<CaptureRecord> library() const {
    QList<CaptureRecord> records;
    CaptureRecord cafe = record("Iceland/café-paris.jpg", CaptureRecord::Picture, {2024, 6, 15});
    cafe.camera = QStringLiteral("FUJIFILM X-T4");
    cafe.lens = QStringLiteral("XF35mmF1.4 R");
    cafe.caption = QStringLiteral("Dinner with Zoë");
    records.append(cafe);
    CaptureRecord waterfall = record("Iceland/waterfall.jpg", CaptureRecord::Picture, {2025, 1, 10});
    waterfall.camera = QStringLiteral("Sony ILCE-7M3");
    waterfall.lens = QStringLiteral("FE 24-70mm F2.8 GM");
    records.append(waterfall);
    records.append(record("Shots/screenshot-2023-03-01.png", CaptureRecord::Screenshot, {2023, 3, 1}));
    CaptureRecord report = record("Docs/report.pdf", CaptureRecord::Document, {2024, 2, 29});
    report.document = true;
    records.append(report);
    CaptureRecord clip = record("Clips/clip.mp4", CaptureRecord::Recording, {2024, 12, 31});
    clip.video = true;
    clip.caption = QStringLiteral("note:x reminder");
    records.append(clip);
    CaptureRecord draft = record("Iceland/draft copy.jpg", CaptureRecord::Picture, {2022, 5, 5});
    draft.animated = true;
    records.append(draft);
    CaptureRecord raw = record("Iceland/IMG_0001.dng", CaptureRecord::Picture, {2024, 7, 1});
    raw.raw = true;
    records.append(raw);
    return records;
  }

  std::unique_ptr<QTemporaryDir> m_scratch;
  QString m_media;
  QString m_files;
};

int main(int argc, char* argv[]) {
  QTemporaryDir config;
  if (!config.isValid()) return 1;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QGuiApplication application(argc, argv);
  SearchTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}

#include "tst_search.moc"
