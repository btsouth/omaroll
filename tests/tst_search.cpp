#include "app/AppSettings.h"
#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"
#include "search/DocumentTextIndex.h"
#include "search/SearchQuery.h"
#include "sources/FileVersion.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
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
#include <atomic>
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
    QString folded = SearchText::fold(QStringLiteral("ﬁ Café"), &sources);
    QCOMPARE(folded, QStringLiteral("fi cafe"));
    QCOMPARE(sources, (QList<qsizetype>{0, 0, 1, 2, 3, 4, 5}));

    // Letters NFKD leaves whole are spelled out, in either case.
    QCOMPARE(SearchText::fold(QStringLiteral("Straße STRASSE ẞ")), QStringLiteral("strasse strasse ss"));
    QCOMPARE(SearchText::fold(QStringLiteral("Łódź")), QStringLiteral("lodz"));
    QCOMPARE(SearchText::fold(QStringLiteral("Øresund")), QStringLiteral("oresund"));
    QCOMPARE(SearchText::fold(QStringLiteral("Đà Nẵng")), QStringLiteral("da nang"));
    QCOMPARE(SearchText::fold(QStringLiteral("æble Æble Œuvre Þing Ðið")),
             QStringLiteral("aeble aeble oeuvre thing did"));
    QCOMPARE(SearchText::fold(QStringLiteral("Iİı Ħal Ŧ")), QStringLiteral("iii hal t"));
    // Marks go after case folding in both forms, so the Greek ypogegrammeni,
    // which folds to iota, is kept the same way in each.
    QCOMPARE(SearchText::fold(QStringLiteral("ᾳ")), QStringLiteral("αι"));
    QCOMPARE(SearchText::fold(QStringLiteral("ᾳ"), &sources), QStringLiteral("αι"));
    QCOMPARE(sources, (QList<qsizetype>{0, 0}));
    const QString mixed = QStringLiteral("Straße — Łódź ﬁle 𝐀B Ǖ ᾳ é");
    QCOMPARE(SearchText::fold(mixed, &sources), SearchText::fold(mixed));
    QCOMPARE(sources.size(), SearchText::fold(mixed).size());

    QVERIFY(SearchText::containsWordStart(QStringLiteral("fujifilm x-t4"), QStringLiteral("x-t")));
    QVERIFY(SearchText::containsWordStart(QStringLiteral("travel/japan"), QStringLiteral("jap")));
    QVERIFY(!SearchText::containsWordStart(QStringLiteral("party"), QStringLiteral("art")));
    QVERIFY(SearchText::containsWordStart(QStringLiteral("party art"), QStringLiteral("art")));
  }

  void longTextSnippetsComeFromTheOriginal() {
    // Long enough for many pieces, with the match on a piece boundary, after
    // decomposed letters that fold shorter and ligatures that fold longer.
    QString text;
    for (int line = 0; line < 400; ++line) {
      text += QStringLiteral("Line %1 ﬁne café Straße 𝐀 ").arg(line);
    }
    qsizetype boundary = 2048 * 3 - 3;
    if (text.at(boundary).isLowSurrogate()) ++boundary;
    text.insert(boundary, QStringLiteral("ŁÓDŹ Harbour"));
    const SearchText::FoldedText kept = SearchText::FoldedText::make(text);
    QCOMPARE(QString::fromUtf8(kept.original), text);
    QCOMPARE(QString::fromUtf8(kept.folded), SearchText::fold(text));
    QVERIFY(kept.marks.size() > 3);
    const QString snippet = kept.snippet(QByteArrayLiteral("lodz harbour"), 10, 10);
    QVERIFY2(snippet.contains(QStringLiteral("ŁÓDŹ Harbour")), qPrintable(snippet));
    QVERIFY(snippet.startsWith(QChar(0x2026)));
    QVERIFY(snippet.endsWith(QChar(0x2026)));
    QVERIFY(snippet.size() <= 1 + 10 + 12 + 10 + 1);
    // The first and last characters, with nothing to cut on that side.
    const QString start = kept.snippet(QByteArrayLiteral("line 0 fine"), 10, 4);
    QVERIFY2(start.startsWith(QStringLiteral("Line 0 ﬁne")), qPrintable(start));
    QVERIFY(!start.startsWith(QChar(0x2026)));
    const QString end = kept.snippet(QByteArrayLiteral("line 399 fine cafe strasse"), 4, 10);
    QVERIFY2(end.endsWith(QStringLiteral("Straße 𝐀")), qPrintable(end));
    QVERIFY(kept.snippet(QByteArrayLiteral("nowhere"), 10, 10).isEmpty());
    // Matched text that folds to letters the original spells differently.
    QVERIFY(kept.snippet(QByteArrayLiteral("strasse a line 12 "), 0, 0).contains(QStringLiteral("Straße 𝐀 Line 12")));
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
    // Picture text and PDF text each come from their own index.
    proxy.setOcrText(path("Shots/screenshot-2023-03-01.png"),
                     QStringLiteral("Invoice 42 total due at CAFÉ Nord"));
    proxy.setDocumentText(path("Docs/report.pdf"), QStringLiteral("Quarterly résumé summary"));

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
    proxy.setOcrText(path("Clips/clip.mp4"), QStringLiteral("Łódź Straße Øresund Đà Nẵng"));
    for (const char* word : {"lodz", "strasse", "STRASSE", "oresund", "\"da nang\"", "straße"}) {
      expect(QString::fromUtf8(word), {QStringLiteral("clip.mp4")});
    }
    proxy.setOcrText(path("Clips/clip.mp4"), {});
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

  void plainWordsMatchFoldersBelowTheLibraryRoots() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    const QString root = QFileInfo(m_media).canonicalFilePath();
    QVERIFY(!root.isEmpty());
    const auto at = [&](const QString& relative) {
      CaptureRecord record;
      record.path = relative.startsWith(QLatin1Char('/')) ? relative : root + QLatin1Char('/') + relative;
      record.fileName = QFileInfo(record.path).fileName();
      record.kind = CaptureRecord::Picture;
      record.captured = QDateTime(QDate(2024, 1, 1), QTime(12, 0));
      return record;
    };
    model.setRecordsForTesting({at(QStringLiteral("loose.jpg")),
                                at(QStringLiteral("2024/Road trip/a.jpg")),
                                at(QStringLiteral("Videos-old/b.jpg")),
                                at(m_files + QStringLiteral("/Elsewhere/c.jpg"))});
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    const auto found = [&](const QString& query) {
      proxy.setSearchText(query);
      return names(proxy);
    };
    // The root's own path is not searched by plain words; what is below it is.
    const QString rootName = SearchText::fold(QFileInfo(root).fileName());
    QVERIFY(found(rootName).isEmpty());
    QVERIFY(found(SearchText::fold(QFileInfo(root).dir().dirName())).isEmpty());
    QCOMPARE(found(QStringLiteral("road")), QStringList{QStringLiteral("a.jpg")});
    QCOMPARE(found(QStringLiteral("2024")), QStringList{QStringLiteral("a.jpg")});
    QCOMPARE(found(QStringLiteral("trip")), QStringList{QStringLiteral("a.jpg")});
    // At a word start only, so "eos" does not find "videos".
    QCOMPARE(found(QStringLiteral("old")), QStringList{QStringLiteral("b.jpg")});
    QVERIFY(found(QStringLiteral("ideos")).isEmpty());
    QVERIFY(found(QStringLiteral("rip")).isEmpty());
    // Outside every root, a file's own folder name counts.
    QCOMPARE(found(QStringLiteral("elsewhere")), QStringList{QStringLiteral("c.jpg")});
    QVERIFY(found(SearchText::fold(QFileInfo(m_files).fileName())).isEmpty());
    // in: still takes the whole path.
    QCOMPARE(found(QStringLiteral("in:") + root + QStringLiteral("/2024")),
             QStringList{QStringLiteral("a.jpg")});
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
    // A value typed in full behind an open quote still offers the closing
    // one; once closed there is nothing to add.
    QCOMPARE(inserts(QStringLiteral("album:\"Weekend Café")),
             QStringList{QStringLiteral("album:\"Weekend Café\"")});
    QVERIFY(inserts(QStringLiteral("album:\"Weekend Café\"")).isEmpty());
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
    // "search" keeps only plain words, which is what 1.18 reads.
    QVariantMap view = proxy.currentView();
    QCOMPARE(view.value(QStringLiteral("query")).toString(),
             QStringLiteral("-type:animated after:2023"));
    QVERIFY(view.value(QStringLiteral("search")).toString().isEmpty());
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

    // Plain words and tokens together: the words alone go in "search".
    proxy.setSearchText(QStringLiteral("Café -draft tag:x \"Dinner with\""));
    view = proxy.currentView();
    QCOMPARE(view.value(QStringLiteral("search")).toString(), QStringLiteral("Café Dinner with"));

    // A view saved by 1.18 has plain words only, and they keep that meaning.
    proxy.setAlbumFilter({}, {});
    proxy.setFolderFilter({});
    proxy.applyView(QStringLiteral("Old"), {{QStringLiteral("search"), QStringLiteral("draft")}});
    QCOMPARE(names(proxy), QStringList{QStringLiteral("draft copy.jpg")});
    proxy.applyView(QStringLiteral("Old"),
                    {{QStringLiteral("search"), QStringLiteral("-paris type:photo")}});
    QVERIFY(proxy.searchText().contains(QStringLiteral("\"-paris\"")));
    QCOMPARE(proxy.count(), 0);
    proxy.setOcrText(path("Clips/clip.mp4"), QStringLiteral("notes on -paris type:photo"));
    QTRY_COMPARE(names(proxy), QStringList{QStringLiteral("clip.mp4")});
    proxy.setOcrText(path("Clips/clip.mp4"), {});

    // Deleting the album deletes the views that open it, and no others.
    QVERIFY(settings.saveSmartCollection(QStringLiteral("No album"),
                                         {{QStringLiteral("query"), QStringLiteral("cafe")}}));
    QCOMPARE(settings.deleteAlbum(QStringLiteral("Paris")), 1);
    QVERIFY(settings.smartCollection(QStringLiteral("Paris trip")).isEmpty());
    QVERIFY(!settings.smartCollection(QStringLiteral("No album")).isEmpty());
    QVERIFY(settings.createTag(QStringLiteral("Travel/Japan")));
    QVERIFY(settings.saveSmartCollection(QStringLiteral("Japan"),
                                         {{QStringLiteral("tag"), QStringLiteral("Travel/Japan")}}));
    QCOMPARE(settings.deleteTag(QStringLiteral("Travel")), 1);
    QVERIFY(settings.smartCollection(QStringLiteral("Japan")).isEmpty());
    QCOMPARE(settings.smartCollectionNames(), QStringList{QStringLiteral("No album")});
  }

  void clearingEveryFacetIsOneFilterPass() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(settings.createAlbum(QStringLiteral("Trip")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Trip"), {path("Iceland/café-paris.jpg")}));
    proxy.setFolderFilter(m_files + QStringLiteral("/Iceland"));
    proxy.setAlbumFilter(QStringLiteral("Trip"), settings.albumPaths(QStringLiteral("Trip")));
    proxy.setDateRange(QStringLiteral("2024-01-01"), QStringLiteral("2024-12-31"));
    proxy.setCameraFilter(QStringLiteral("FUJIFILM X-T4"));
    proxy.setLensFilter(QStringLiteral("XF35mmF1.4 R"));
    proxy.setMinimumRating(0);
    proxy.setSearchText(QStringLiteral("jpg"));
    proxy.setKindFilter(CaptureRecord::Picture);
    QCOMPARE(proxy.count(), 1);

    // Each filter pass ends by refreshing every row's pairing roles once.
    QSignalSpy passes(&proxy, &QAbstractItemModel::dataChanged);
    QSignalSpy resets(&proxy, &QAbstractItemModel::modelReset);
    QSignalSpy layouts(&proxy, &QAbstractItemModel::layoutChanged);
    proxy.clearFacets(false);
    QCOMPARE(passes.size(), 1);
    QCOMPARE(resets.size() + layouts.size(), 0);
    QVERIFY(proxy.folderFilter().isEmpty());
    QVERIFY(proxy.albumFilter().isEmpty());
    QVERIFY(proxy.dateFrom().isEmpty());
    QVERIFY(proxy.cameraFilter().isEmpty());
    QVERIFY(proxy.lensFilter().isEmpty());
    // The search and kind are not facets.
    QCOMPARE(proxy.searchText(), QStringLiteral("jpg"));
    QCOMPARE(proxy.kindFilter(), int(CaptureRecord::Picture));
    QCOMPARE(names(proxy), sorted({QStringLiteral("café-paris.jpg"), QStringLiteral("waterfall.jpg"),
                                   QStringLiteral("draft copy.jpg")}));
    proxy.setTagFilter(QStringLiteral("x"), {});
    passes.clear();
    proxy.clearFacets(true);
    // One pass, and one refresh of the search snippets.
    QCOMPARE(passes.size(), 2);
    QVERIFY(proxy.searchText().isEmpty());
    QVERIFY(proxy.tagFilter().isEmpty());
  }

  // Large libraries take a filter change in one go once enough rows change;
  // smaller changes go row by row. Both must leave the same rows in order.
  void bulkAndIncrementalFilterChangesAgree() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    constexpr int kItems = 6000;
    QList<CaptureRecord> records;
    for (int index = 0; index < kItems; ++index) {
      CaptureRecord record;
      record.fileName = QStringLiteral("item-%1-%2.jpg").arg(index).arg(index % 7 == 0 ? "seven" : "x");
      record.path = QStringLiteral("/library/set %1/").arg(index % 13) + record.fileName;
      record.kind = CaptureRecord::Picture;
      record.captured = QDateTime(QDate(2020, 1, 1), QTime(9, 0)).addSecs(qint64(index) * 997 % 50000 * 60);
      record.bytes = 1000 + (index * 31) % 4000;
      records.append(record);
    }
    model.setRecordsForTesting(records);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QSignalSpy layouts(&proxy, &QAbstractItemModel::layoutChanged);
    QSignalSpy inserts(&proxy, &QAbstractItemModel::rowsInserted);
    QSignalSpy removes(&proxy, &QAbstractItemModel::rowsRemoved);
    const auto rows = [](const CaptureFilterModel& model) {
      QStringList paths;
      for (int row = 0; row < model.rowCount(); ++row) paths.append(model.pathAt(row));
      return paths;
    };
    // Each step against a model that starts with the same filter, which Qt
    // maps from scratch.
    const QStringList steps{QStringLiteral("seven"),            // thousands removed: bulk
                            QStringLiteral("seven -item-7-"),   // one row: incremental
                            QStringLiteral("seven -item-1"),    // over a hundred: incremental
                            QStringLiteral("item"),             // thousands back: bulk
                            QStringLiteral("in:\"set 3\" -seven"),
                            QString()};
    int bulk = 0;
    int incremental = 0;
    for (const QString& step : steps) {
      layouts.clear();
      inserts.clear();
      removes.clear();
      proxy.setSearchText(step);
      bulk += layouts.isEmpty() ? 0 : 1;
      incremental += inserts.size() + removes.size() > 0 && layouts.isEmpty() ? 1 : 0;
      CaptureFilterModel fresh;
      fresh.setSearchText(step);
      fresh.setSourceModel(&model);
      QCOMPARE(proxy.rowCount(), fresh.rowCount());
      QVERIFY2(rows(proxy) == rows(fresh), qPrintable(step));
    }
    QVERIFY(bulk >= 2);
    QVERIFY(incremental >= 2);
    // The sort holds through both too.
    proxy.setSortMode(CaptureFilterModel::LargestFirst);
    proxy.setSearchText(QStringLiteral("seven"));
    CaptureFilterModel fresh;
    fresh.setSortMode(CaptureFilterModel::LargestFirst);
    fresh.setSearchText(QStringLiteral("seven"));
    fresh.setSourceModel(&model);
    QCOMPARE(rows(proxy), rows(fresh));
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
      connect(&index, &DocumentTextIndex::textReady, &proxy, &CaptureFilterModel::setDocumentText);
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
    proxy.setDocumentText(path("Docs/report.pdf"), {});
    {
      DocumentTextIndex index(&model);
      connect(&index, &DocumentTextIndex::textReady, &proxy, &CaptureFilterModel::setDocumentText);
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
      QTRY_COMPARE(names(proxy), QStringList{QStringLiteral("report.pdf")});

      QVERIFY(index.clearCache() >= 1);
      QTRY_COMPARE(proxy.count(), 0);
      QVERIFY(QDir(DocumentTextIndex::cacheDirectory())
                  .entryList({QStringLiteral("*.txt")}, QDir::Files)
                  .isEmpty());
    }
  }

  void pdfReadsAreBoundedAndFailuresAreRemembered() {
    const QString bin = m_scratch->filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    const QString log = m_scratch->filePath(QStringLiteral("calls"));
    QFile helper(bin + QStringLiteral("/pdftotext"));
    QVERIFY(helper.open(QIODevice::WriteOnly));
    helper.write("#!/bin/sh\necho \"$*\" >> '" + log.toUtf8() + "'\n"
                 "case \"$*\" in\n"
                 "  *slow.pdf*) exec sleep 30 ;;\n"
                 "  *late.pdf*) sleep 1; printf 'late words' ;;\n"
                 "  *big.pdf*) head -c 3000000 /dev/zero | tr '\\0' 'a' ;;\n"
                 "  *) printf 'plain words' ;;\n"
                 "esac\n");
    helper.close();
    QVERIFY(helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    const QByteArray oldPath = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + oldPath));
    const auto calls = [&](const char* name) {
      QFile file(log);
      if (!file.open(QIODevice::ReadOnly)) return 0;
      int count = 0;
      for (const QByteArray& line : file.readAll().split('\n')) count += line.contains(name) ? 1 : 0;
      return count;
    };

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    const auto document = [&](const char* relative, qint64 shift = 0) {
      CaptureRecord result = record(relative, CaptureRecord::Document, {2024, 1, 1});
      result.document = true;
      result.modified += shift;
      return result;
    };
    QList<CaptureRecord> records{document("Docs/big.pdf"), document("Docs/slow.pdf")};
    model.setRecordsForTesting(records);

    {
      DocumentTextIndex index(&model);
      index.setTimeoutMs(1500);
      QSignalSpy ready(&index, &DocumentTextIndex::textReady);
      const auto textFor = [&](const QString& file) {
        QString text;
        for (const QList<QVariant>& arguments : std::as_const(ready)) {
          if (arguments.at(0).toString() == file) text = arguments.at(1).toString();
        }
        return text;
      };
      index.setSearchText(QStringLiteral("words"));
      QTRY_COMPARE(calls("slow.pdf"), 1);
      QTRY_VERIFY(!index.indexing());
      // Output past the limit is cut there, and the tool stopped.
      QCOMPARE(calls("big.pdf"), 1);
      QCOMPARE(textFor(path("Docs/big.pdf")).size(), DocumentTextIndex::kTextLimit);
      QVERIFY(textFor(path("Docs/slow.pdf")).isEmpty());
      // The cache holds the text compressed.
      const QFileInfoList cached = QDir(DocumentTextIndex::cacheDirectory())
                                       .entryInfoList({QStringLiteral("*.txt")}, QDir::Files);
      QCOMPARE(cached.size(), 1);
      QVERIFY2(cached.first().size() < DocumentTextIndex::kTextLimit / 8,
               qPrintable(QString::number(cached.first().size())));

      // The document that timed out is not read again on the next sync...
      model.setRecordsForTesting(records);
      QTest::qWait(800);
      QVERIFY(!index.indexing());
      QCOMPARE(calls("slow.pdf"), 1);
      QCOMPARE(calls("big.pdf"), 1);
      // ...until it changes.
      records[1] = document("Docs/slow.pdf", 1000);
      model.setRecordsForTesting(records);
      QTRY_COMPARE(calls("slow.pdf"), 2);
      QTRY_VERIFY(!index.indexing());

      // Clearing the cache while a document is read drops what it returns.
      index.setTimeoutMs(5000);
      records.prepend(document("Docs/late.pdf"));
      model.setRecordsForTesting(records);
      QTRY_COMPARE(calls("late.pdf"), 1);
      QVERIFY(index.indexing());
      ready.clear();
      QVERIFY(index.clearCache() >= 1);
      QTRY_VERIFY_WITH_TIMEOUT(!index.indexing(), 5000);
      QTest::qWait(100);
      QVERIFY(textFor(path("Docs/late.pdf")).isEmpty());
      QVERIFY(QDir(DocumentTextIndex::cacheDirectory())
                  .entryList({QStringLiteral("*.txt")}, QDir::Files)
                  .isEmpty());
    }

    // Closing while a document is read stops the tool rather than waiting
    // for it.
    records = {document("Docs/slow.pdf", 2000)};
    model.setRecordsForTesting(records);
    auto index = std::make_unique<DocumentTextIndex>(&model);
    index->setTimeoutMs(30'000);
    index->setSearchText(QStringLiteral("words"));
    QTRY_COMPARE(calls("slow.pdf"), 3);
    QVERIFY(index->indexing());
    QElapsedTimer closing;
    closing.start();
    index.reset();
    QVERIFY2(closing.elapsed() < 3000, qPrintable(QString::number(closing.elapsed())));
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
    // Only the first attempt's first keystroke builds the keys; later
    // attempts find them warm.
    qint64 coldFirst = -1;
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
      if (attempt == 0) coldFirst = run.first();
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
    const qint64 first = coldFirst;
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

  // Snippets are read for every visible tile after each keystroke and each
  // text that arrives, so long PDF text must not be refolded per read.
  void snippetsOfLongDocumentsStayCheap() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    constexpr int kDocuments = 24;
    QList<CaptureRecord> records;
    for (int index = 0; index < kDocuments; ++index) {
      CaptureRecord record;
      record.fileName = QStringLiteral("report-%1.pdf").arg(index);
      record.path = QStringLiteral("/library/Documents/") + record.fileName;
      record.kind = CaptureRecord::Document;
      record.document = true;
      record.captured = QDateTime(QDate(2024, 1, 1), QTime(9, 0)).addSecs(index);
      records.append(record);
    }
    model.setRecordsForTesting(records);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);

    // Half a million characters of ordinary text with one em dash, as most
    // PDFs have, and the match near the end.
    QString body;
    body.reserve(512 * 1024);
    const QString line = QStringLiteral("Quarterly figures for the northern region, page break. ");
    while (body.size() < 512 * 1024 - 200) body += line;
    body.insert(1000, QChar(0x2014));
    for (int index = 0; index < kDocuments; ++index) {
      proxy.setDocumentText(records.at(index).path,
                          body + QStringLiteral(" Final total for Zürich %1 closes it.").arg(index));
    }
    proxy.setSearchText(QStringLiteral("zurich"));
    QCOMPARE(proxy.count(), kDocuments);
    const QString first = proxy.ocrSnippetAt(0);
    QVERIFY2(first.contains(QStringLiteral("Zürich")), qPrintable(first));
    QVERIFY(first.startsWith(QChar(0x2026)));

    // CPU time stretches with the machine's load and with sanitizers, so
    // the budget is set by work timed alongside it in the same build:
    // folding one document's text, which a snippet read that refolded would
    // repeat for every tile. All the reads together cost less than that one
    // fold. Each attempt times both back to back, and the best ratio counts.
    double ratio = std::numeric_limits<double>::max();
    qint64 reads = 0;
    qint64 folds = 0;
    qsizetype folded = 0;
    for (int attempt = 0; attempt < 7; ++attempt) {
      qint64 cpu = threadMicros();
      folded += SearchText::fold(body).size();
      const qint64 fold = std::max<qint64>(1, threadMicros() - cpu);
      cpu = threadMicros();
      for (int row = 0; row < proxy.count(); ++row) (void)proxy.ocrSnippetAt(row);
      const qint64 read = threadMicros() - cpu;
      if (double(read) / double(fold) < ratio) {
        ratio = double(read) / double(fold);
        reads = read;
        folds = fold;
      }
    }
    QVERIFY(folded >= 7 * body.size());
    qInfo().noquote() << QStringLiteral("%1 snippets of 512K-character text: %2 ms CPU, %3x one fold (%4 ms)")
                             .arg(kDocuments)
                             .arg(reads / 1000.0, 0, 'f', 2)
                             .arg(ratio, 0, 'f', 2)
                             .arg(folds / 1000.0, 0, 'f', 2);
    QVERIFY2(ratio < 4, qPrintable(QString::number(ratio)));
  }

  // A supplementary character at either end of the context, or ending the
  // match, stays whole.
  void snippetsKeepSurrogatePairsWhole() {
    const QString face = QString::fromUcs4(U"\U0001F600");
    QString text;
    for (int index = 0; index < 40; ++index) text += face;
    text += QStringLiteral(" Match") + face + QLatin1Char(' ');
    for (int index = 0; index < 40; ++index) text += face;
    const SearchText::FoldedText kept = SearchText::FoldedText::make(text);
    const auto wellFormed = [](const QString& value) {
      for (qsizetype at = 0; at < value.size(); ++at) {
        if (value.at(at).isHighSurrogate() &&
            (at + 1 >= value.size() || !value.at(at + 1).isLowSurrogate())) {
          return false;
        }
        if (value.at(at).isLowSurrogate() && (at == 0 || !value.at(at - 1).isHighSurrogate())) {
          return false;
        }
      }
      return true;
    };
    const QByteArray needle = (QStringLiteral("match") + face).toUtf8();
    for (int before = 0; before < 5; ++before) {
      for (int after = 0; after < 5; ++after) {
        const QString snippet = kept.snippet(needle, before, after);
        QVERIFY2(wellFormed(snippet), qPrintable(QStringLiteral("%1 %2").arg(before).arg(after)));
        QVERIFY2(snippet.contains(QStringLiteral("Match") + face),
                 qPrintable(QStringLiteral("%1 %2").arg(before).arg(after)));
      }
    }
  }

  void quotesInsideValuesAreEscaped() {
    const QString name = QStringLiteral("Road \"Trip\"");
    QCOMPARE(SearchQuery::quote(name), QStringLiteral("\"Road \\\"Trip\\\"\""));
    QCOMPARE(SearchQuery::quote(QStringLiteral("plain")), QStringLiteral("plain"));
    QCOMPARE(SearchQuery::quote(QStringLiteral("a\\b c")), QStringLiteral("\"a\\\\b c\""));
    const SearchQuery album = SearchQuery::parse(QStringLiteral("album:") + SearchQuery::quote(name));
    QCOMPARE(album.terms.size(), 1);
    QCOMPARE(album.terms.first().field, SearchQuery::Album);
    QCOMPARE(album.terms.first().value, QStringLiteral("road \"trip\""));
    const SearchQuery phrase = SearchQuery::parse(QStringLiteral("\"say \\\"hi\\\"\" next"));
    QCOMPARE(phrase.terms.size(), 2);
    QCOMPARE(phrase.terms.first().value, QStringLiteral("say \"hi\""));

    // A view saved before field tokens searched for its words as written,
    // quotes included.
    const SearchQuery legacy =
        SearchQuery::parse(SearchQuery::fromPlainWords(QStringLiteral("He\"llo -draft C:\\x")));
    QCOMPARE(legacy.terms.size(), 3);
    QCOMPARE(legacy.terms.at(0).value, QStringLiteral("he\"llo"));
    QCOMPARE(legacy.terms.at(1).value, QStringLiteral("-draft"));
    QVERIFY(!legacy.terms.at(1).negated);
    QCOMPARE(legacy.terms.at(2).value, QStringLiteral("c:\\x"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(settings.createAlbum(name));
    QVERIFY(settings.addToAlbum(name, {path("Iceland/waterfall.jpg")}));
    const auto inserts = [&](const QString& text) {
      QStringList values;
      for (const QVariant& row : proxy.searchSuggestions(text, int(text.size()))) {
        values.append(row.toMap().value(QStringLiteral("insert")).toString());
      }
      return values;
    };
    const QString accepted = QStringLiteral("album:\"Road \\\"Trip\\\"\"");
    QCOMPARE(inserts(QStringLiteral("album:road")), QStringList{accepted});
    // An escaped quote does not end the value being typed.
    QCOMPARE(inserts(QStringLiteral("album:\"Road \\\"Tr")), QStringList{accepted});
    proxy.setSearchText(accepted);
    QCOMPARE(proxy.textSearch(), QString());
    QCOMPARE(names(proxy), QStringList{QStringLiteral("waterfall.jpg")});
    // A space after an escaped quote is still inside the value.
    QVERIFY(settings.createAlbum(QStringLiteral("Bob\"s Trip")));
    QCOMPARE(inserts(QStringLiteral("album:\"Bob\\\"s Tr")),
             QStringList{QStringLiteral("album:\"Bob\\\"s Trip\"")});
  }

  void inNamesTheHomeFolderItself() {
    const QString home = QDir::homePath();
    QCOMPARE(SearchQuery::parse(QStringLiteral("in:") + home).terms.first().value,
             QStringLiteral("~"));
    QCOMPARE(SearchQuery::parse(QStringLiteral("in:") + home + QLatin1Char('/')).terms.first().value,
             QStringLiteral("~"));
    QCOMPARE(SearchQuery::parse(QStringLiteral("in:") + home + QStringLiteral("/Pictures"))
                 .terms.first()
                 .value,
             QStringLiteral("~/pictures"));

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    const auto picture = [](const QString& file) {
      CaptureRecord record;
      record.path = file;
      record.fileName = QFileInfo(file).fileName();
      record.kind = CaptureRecord::Picture;
      record.captured = QDateTime(QDate(2024, 1, 1), QTime(9, 0));
      return record;
    };
    model.setRecordsForTesting({picture(home + QStringLiteral("/top.jpg")),
                                picture(home + QStringLiteral("/Pictures/deep.jpg")),
                                picture(QStringLiteral("/srv/elsewhere/out.jpg"))});
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    proxy.setSearchText(QStringLiteral("in:") + home);
    QCOMPARE(names(proxy), QStringList({QStringLiteral("deep.jpg"), QStringLiteral("top.jpg")}));
  }

  // A scan that inserts a file moves the rows below it; none of them may
  // keep the text of the file that was there before.
  void rowsThatMoveDoNotKeepAnotherFilesText() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    const auto document = [&](const char* relative) {
      CaptureRecord result = record(relative, CaptureRecord::Document, {2024, 1, 1});
      result.document = true;
      return result;
    };
    const CaptureRecord alpha = document("Docs/alpha.pdf");
    const CaptureRecord beta = document("Docs/beta.pdf");
    model.setRecordsForTesting({alpha, beta});
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    proxy.setDocumentText(alpha.path, QStringLiteral("ledger words"));
    proxy.setDocumentText(beta.path, QStringLiteral("receipt words"));
    proxy.setSearchText(QStringLiteral("ledger"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("alpha.pdf")});

    model.setRecordsForTesting({document("Docs/gamma.pdf"), alpha, beta});
    QCOMPARE(names(proxy), QStringList{QStringLiteral("alpha.pdf")});
    proxy.setSearchText(QStringLiteral("receipt"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("beta.pdf")});
  }

  // A PDF replaced by a picture at the same path: the picture's text stays
  // when the PDF index later drops the document's.
  void textProducersKeepTheirOwnText() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    CaptureRecord document = record("Docs/scan", CaptureRecord::Document, {2024, 1, 1});
    document.document = true;
    model.setRecordsForTesting({document});
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    proxy.setDocumentText(document.path, QStringLiteral("ledger totals"));
    proxy.setSearchText(QStringLiteral("ledger"));
    QCOMPARE(proxy.count(), 1);

    CaptureRecord picture = document;
    picture.document = false;
    picture.kind = CaptureRecord::Picture;
    model.setRecordsForTesting({picture});
    // The PDF's text is not the picture's.
    QCOMPARE(proxy.count(), 0);
    proxy.setOcrText(picture.path, QStringLiteral("bakery receipt"));
    proxy.setDocumentText(picture.path, {});
    proxy.setSearchText(QStringLiteral("bakery"));
    QTRY_COMPARE(proxy.count(), 1);
    // And the other way round.
    proxy.setOcrText(picture.path, {});
    QTRY_COMPARE(proxy.count(), 0);
  }

  void savedViewsSurviveAlbumMembershipChanges() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    model.setRecordsForTesting(library());
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    QVERIFY(settings.createAlbum(QStringLiteral("Trip")));
    QVERIFY(settings.addToAlbum(QStringLiteral("Trip"), {path("Iceland/waterfall.jpg")}));
    proxy.applyView(QStringLiteral("Trip view"), {{QStringLiteral("album"), QStringLiteral("Trip")}});
    QCOMPARE(names(proxy), QStringList{QStringLiteral("waterfall.jpg")});

    // A file joins the album: the view stays open and shows it.
    QVERIFY(settings.addToAlbum(QStringLiteral("Trip"), {path("Iceland/café-paris.jpg")}));
    proxy.setAlbumFilter(QStringLiteral("Trip"), settings.albumPaths(QStringLiteral("Trip")));
    QCOMPARE(proxy.smartCollectionFilter(), QStringLiteral("Trip view"));
    QCOMPARE(names(proxy).size(), 2);
    // Another album is another view.
    QVERIFY(settings.createAlbum(QStringLiteral("Other")));
    proxy.setAlbumFilter(QStringLiteral("Other"), {});
    QVERIFY(proxy.smartCollectionFilter().isEmpty());
  }

  void rowLookupsAreCurrentWhenAResetEnds() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    const QList<CaptureRecord> records = library();
    model.setRecordsForTesting(records);
    QList<CaptureRecord> reversed = records;
    std::reverse(reversed.begin(), reversed.end());
    int seen = -2;
    connect(&model, &QAbstractItemModel::modelReset, this,
            [&] { seen = model.rowOf(reversed.first().path); });
    model.setRecordsForTesting(reversed);
    QCOMPARE(seen, 0);
  }

  // Past its budget, PDF text leaves memory least recently used first and
  // is matched from disk off the GUI thread, so every document stays
  // searchable, with snippets.
  // A damaged cache entry whose size header claims more than any saved text
  // is a miss, so the document is read again; it is never decompressed into
  // the gigabytes the header asks for (found by fuzz_doctext).
  void documentTextCacheRejectsAnImpossibleSize() {
    const QString pdf = m_scratch->filePath(QStringLiteral("claimed.pdf"));
    {
      QFile file(pdf);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("%PDF-1.4\n");
    }
    const QString entryPath = DocumentTextIndex::cacheDirectory() + QLatin1Char('/') +
        QString::fromLatin1(QCryptographicHash::hash(pdf.toUtf8(), QCryptographicHash::Sha256).toHex()) +
        QStringLiteral(".txt");
    QVERIFY(QDir().mkpath(DocumentTextIndex::cacheDirectory()));
    const auto writeEntry = [&](quint32 claimed) {
      QByteArray compressed = qCompress(QByteArrayLiteral("invoice total"));
      compressed[0] = char(claimed >> 24);
      compressed[1] = char(claimed >> 16);
      compressed[2] = char(claimed >> 8);
      compressed[3] = char(claimed);
      QFile file(entryPath);
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QDataStream out(&file);
      out.setVersion(QDataStream::Qt_6_0);
      out << quint32(0x50445431) << quint16(3) << pdf << FileVersion::diskKey(pdf) << compressed;
    };
    writeEntry(13);
    QCOMPARE(DocumentTextIndex::cachedText(pdf), QStringLiteral("invoice total"));
    writeEntry(quint32(DocumentTextIndex::kTextLimit * 4 + 1));
    QVERIFY(DocumentTextIndex::cachedText(pdf).isNull());
    writeEntry(0x9cf3cc2b);
    QVERIFY(DocumentTextIndex::cachedText(pdf).isNull());
    // A size header within the cap on text that inflates past it is a miss too.
    {
      const QByteArray big(DocumentTextIndex::kTextLimit * 4 + 1, 'a');
      QByteArray compressed = qCompress(big);
      compressed[0] = 0;
      compressed[1] = 0;
      compressed[2] = 0;
      compressed[3] = 16;
      QFile file(entryPath);
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QDataStream out(&file);
      out.setVersion(QDataStream::Qt_6_0);
      out << quint32(0x50445431) << quint16(3) << pdf << FileVersion::diskKey(pdf) << compressed;
    }
    QVERIFY(DocumentTextIndex::cachedText(pdf).isNull());
  }

  void documentTextStaysBoundedAndSearchable() {
    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    constexpr int kDocuments = 48;
    constexpr qsizetype kBudget = 2 * 1024 * 1024;
    const auto documentPath = [](int index) {
      return QStringLiteral("/library/Documents/doc-%1.pdf").arg(index);
    };
    // Each about 64K characters: the disk cache stand-in rebuilds it.
    const auto documentText = [](int index) {
      QString text;
      text.reserve(66 * 1024);
      const QString line = QStringLiteral("Filler paragraph about the northern region figures. ");
      while (text.size() < 64 * 1024) text += line;
      return text + QStringLiteral(" Uniqueword%1x ends it.").arg(index);
    };
    QList<CaptureRecord> records;
    for (int index = 0; index < kDocuments; ++index) {
      CaptureRecord record;
      record.path = documentPath(index);
      record.fileName = QFileInfo(record.path).fileName();
      record.kind = CaptureRecord::Document;
      record.document = true;
      record.captured = QDateTime(QDate(2024, 1, 1), QTime(9, 0)).addSecs(index);
      records.append(record);
    }
    model.setRecordsForTesting(records);

    // Without a loader nothing leaves memory: the cost the bound replaces.
    qsizetype unbounded = 0;
    {
      CaptureFilterModel all;
      all.setSourceModel(&model);
      for (int index = 0; index < kDocuments; ++index) {
        all.setDocumentText(documentPath(index), documentText(index));
      }
      unbounded = all.documentTextBytes();
      QCOMPARE(all.residentDocumentCount(), kDocuments);
    }

    auto lost = std::make_shared<std::atomic_int>(-1);
    auto loads = std::make_shared<std::atomic_int>(0);
    CaptureFilterModel proxy;
    proxy.setSourceModel(&model);
    proxy.setDocumentTextBudget(kBudget);
    proxy.setDocumentTextLoader([lost, loads, documentText](const QString& file) {
      ++*loads;
      const int index = QFileInfo(file).completeBaseName().mid(4).toInt();
      return index == lost->load() ? QString() : documentText(index);
    });
    for (int index = 0; index < kDocuments; ++index) {
      proxy.setDocumentText(documentPath(index), documentText(index));
    }
    qInfo().noquote() << QStringLiteral("%1 documents: %2 KiB in memory unbounded, %3 KiB with the "
                                        "bound (%4 kept, %5 on disk)")
                             .arg(kDocuments)
                             .arg(unbounded / 1024)
                             .arg(proxy.documentTextBytes() / 1024)
                             .arg(proxy.residentDocumentCount())
                             .arg(proxy.storedDocumentCount());
    QVERIFY(unbounded > kBudget * 2);
    QVERIFY(proxy.documentTextBytes() <= kBudget);
    QVERIFY(proxy.storedDocumentCount() > kDocuments / 2);
    QCOMPARE(proxy.residentDocumentCount() + proxy.storedDocumentCount(), kDocuments);
    QCOMPARE(loads->load(), 0);

    const auto search = [&](const QString& text) {
      proxy.setSearchText(text);
      QTRY_VERIFY(!proxy.matchingStoredText());
      QCoreApplication::processEvents();
      QTest::qWait(100);
    };
    // Every document, wherever its text is.
    search(QStringLiteral("northern"));
    QCOMPARE(proxy.count(), kDocuments);
    QVERIFY(loads->load() >= proxy.storedDocumentCount());
    // One of the first, long out of memory, with its snippet.
    search(QStringLiteral("uniqueword7x"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("doc-7.pdf")});
    QVERIFY2(proxy.ocrSnippetAt(0).contains(QStringLiteral("Uniqueword7x")),
             qPrintable(proxy.ocrSnippetAt(0)));
    // And one still in memory.
    search(QStringLiteral("uniqueword47x"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("doc-47.pdf")});
    // Exclusions see the text on disk too.
    search(QStringLiteral("northern -uniqueword7x"));
    QCOMPARE(proxy.count(), kDocuments - 1);
    QVERIFY(proxy.documentTextBytes() <= kBudget);

    // Text gone from disk is reported so it can be read again.
    QSignalSpy gone(&proxy, &CaptureFilterModel::documentTextLost);
    lost->store(3);
    search(QStringLiteral("uniqueword3x"));
    QCOMPARE(gone.size(), 1);
    QCOMPARE(gone.first().first().toString(), documentPath(3));
    QCOMPARE(proxy.count(), 0);
    lost->store(-1);
    proxy.setDocumentText(documentPath(3), documentText(3));
    QTRY_COMPARE(proxy.count(), 1);

    // Text the cache does not hold never leaves memory.
    proxy.setDocumentText(documentPath(5), documentText(5) + QStringLiteral(" unsaved"), false);
    proxy.setDocumentTextBudget(0);
    QCOMPARE(proxy.residentDocumentCount(), 1);
    search(QStringLiteral("unsaved"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("doc-5.pdf")});
    search(QStringLiteral("uniqueword3x"));
    QCOMPARE(names(proxy), QStringList{QStringLiteral("doc-3.pdf")});
  }

  void pdfIndexingPausesOnBatteryAndKnowsReplacedFiles() {
    const QString bin = m_scratch->filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    const QString log = m_scratch->filePath(QStringLiteral("calls"));
    QFile helper(bin + QStringLiteral("/pdftotext"));
    QVERIFY(helper.open(QIODevice::WriteOnly));
    helper.write("#!/bin/sh\necho \"$@\" >> '" + log.toUtf8() + "'\nprintf 'ledger words'\n");
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
    // A laptop on battery, then on mains.
    const QString power = m_scratch->filePath(QStringLiteral("power"));
    const auto supply = [&](const char* name, const QByteArray& type, const char* key,
                            const QByteArray& value) {
      QVERIFY(QDir().mkpath(power + QLatin1Char('/') + QLatin1String(name)));
      QFile typeFile(power + QLatin1Char('/') + QLatin1String(name) + QStringLiteral("/type"));
      QVERIFY(typeFile.open(QIODevice::WriteOnly));
      typeFile.write(type + '\n');
      typeFile.close();
      QFile valueFile(power + QLatin1Char('/') + QLatin1String(name) + QLatin1Char('/') +
                      QLatin1String(key));
      QVERIFY(valueFile.open(QIODevice::WriteOnly));
      valueFile.write(value + '\n');
    };
    supply("BAT0", "Battery", "status", "Discharging");
    supply("AC", "Mains", "online", "0");

    AppSettings settings;
    settings.setScanDownloads(false);
    CaptureModel model(&settings);
    QTRY_VERIFY(!model.scanning());
    CaptureRecord document = record("Docs/ledger.pdf", CaptureRecord::Document, {2024, 1, 1});
    document.document = true;
    document.thumbnailVersion = QStringLiteral("v1");
    model.setRecordsForTesting({document});

    {
      DocumentTextIndex index(&model);
      QSignalSpy ready(&index, &DocumentTextIndex::textReady);
      const auto reads = [&] {
        int count = 0;
        for (const QList<QVariant>& arguments : std::as_const(ready)) {
          count += arguments.at(1).toString().isEmpty() ? 0 : 1;
        }
        return count;
      };
      index.setPowerSupplyDirectory(power);
      QVERIFY(index.onBattery());
      index.setBackground(true);
      QTest::qWait(300);
      QCOMPARE(calls(), 0);
      // Mains power resumes it.
      supply("AC", "Mains", "online", "1");
      index.checkPower();
      QTRY_COMPARE(calls(), 1);
      QTRY_VERIFY(!index.indexing());
      QCOMPARE(reads(), 1);

      // Back on battery, a search still reads.
      supply("AC", "Mains", "online", "0");
      index.checkPower();
      QVERIFY(index.onBattery());
      index.setSearchText(QStringLiteral("ledger"));
      // The scanner's identity changed with the size and mtime kept: the
      // document is read again (here from the cache, whose own key is the
      // file's size, mtime and ctime).
      document.thumbnailVersion = QStringLiteral("v2");
      model.setRecordsForTesting({document});
      QTRY_COMPARE(reads(), 2);
      QTRY_VERIFY(!index.indexing());

      // Clearing the cache cancels a sync that was already scheduled.
      document.thumbnailVersion = QStringLiteral("v3");
      model.setRecordsForTesting({document});
      QVERIFY(index.clearCache() >= 1);
      QTest::qWait(700);
      QCOMPARE(calls(), 1);
      QCOMPARE(reads(), 2);
      QVERIFY(QDir(DocumentTextIndex::cacheDirectory())
                  .entryList({QStringLiteral("*.txt")}, QDir::Files)
                  .isEmpty());
    }

    {
      DocumentTextIndex index(&model);
      index.setSearchText(QStringLiteral("ledger"));
      QTRY_COMPARE(calls(), 2);
      QTRY_VERIFY(!index.indexing());
      QCOMPARE(DocumentTextIndex::cachedText(document.path), QStringLiteral("ledger words"));
    }
    // Replaced with a file of the same size and its old mtime: the cache no
    // longer answers for it, here or after a restart.
    const QDateTime mtime = QFileInfo(document.path).lastModified();
    {
      QFile file(document.path);
      QVERIFY(file.open(QIODevice::ReadWrite));
      QByteArray bytes = file.readAll();
      bytes[0] = bytes[0] == 'X' ? 'Y' : 'X';
      file.seek(0);
      file.write(bytes);
      file.close();
      QVERIFY(file.open(QIODevice::ReadWrite));
      QVERIFY(file.setFileTime(mtime, QFileDevice::FileModificationTime));
    }
    QVERIFY(DocumentTextIndex::cachedText(document.path).isNull());
    {
      DocumentTextIndex index(&model);
      index.setSearchText(QStringLiteral("ledger"));
      QTRY_COMPARE(calls(), 3);
      QTRY_VERIFY(!index.indexing());
    }
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
