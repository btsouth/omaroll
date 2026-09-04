#pragma once

#include <QImage>
#include <QSize>
#include <QString>

namespace PdfSupport {

[[nodiscard]] bool available();
[[nodiscard]] QImage renderPage(const QString& path, int page, const QSize& target);

} // namespace PdfSupport
