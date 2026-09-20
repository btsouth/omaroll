#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>

namespace PdfSupport {

[[nodiscard]] bool available();
[[nodiscard]] QImage renderPage(const QString& path, int page, const QSize& target);

// Whether pdftotext is installed, which search needs.
[[nodiscard]] bool textAvailable();

// The 1-based pages of a document whose text contains query, ignoring case and
// runs of whitespace (so a phrase split across lines still matches). The text
// is what pdftotext emits: pages separated by a form feed.
[[nodiscard]] QList<int> findPages(const QString& documentText, const QString& query);

// One recognized word of a page. The box is in page points, the space
// pdftotext reports and the space a page is rendered in.
struct PdfWord {
  QRectF box;
  QString text;
};

// One page's words in reading order with the page size in points.
struct PdfPageText {
  QSizeF pageSize;
  QList<PdfWord> words;
};

// Reads the XHTML that `pdftotext -bbox` writes for one page. Only the first
// page of the input is read, so a caller that passes a whole document still
// gets one page. Anything unreadable yields an empty page rather than a
// partial guess, because a half-read page would place the words wrongly.
[[nodiscard]] PdfPageText parsePageWords(const QByteArray& xml);

// The words the normalized (0..1) area covers, in reading order. The area has
// to overlap a word's own box, so a careless drag along a margin selects
// nothing.
[[nodiscard]] QList<int> wordsTouched(const PdfPageText& page, const QRectF& area);

// The touched words as a reader expects them: a space within a line, a newline
// between lines, in reading order.
[[nodiscard]] QString wordsText(const PdfPageText& page, const QList<int>& indexes);

// One rectangle per selected line, in normalized page coordinates, for the
// highlight drawn over the page.
[[nodiscard]] QList<QRectF> wordLines(const PdfPageText& page, const QList<int>& indexes);

} // namespace PdfSupport
