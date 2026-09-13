#pragma once

#include <QImage>
#include <QList>
#include <QSize>
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

} // namespace PdfSupport
