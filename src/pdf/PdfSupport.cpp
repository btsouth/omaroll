#include "pdf/PdfSupport.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QXmlStreamReader>

namespace {

QString normalized(const QString& text) {
  QString out = text;
  out.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  return out.trimmed();
}

// A word joins the line above it while its vertical centre stays within half
// the taller of the two boxes, which keeps a heading and a footnote apart
// without measuring fonts.
constexpr qreal kLineTolerance = 0.5;

// Splits the selected words into the lines they were written on, keeping the
// reading order pdftotext emits.
QList<QList<int>> groupLines(const PdfSupport::PdfPageText& page, const QList<int>& indexes) {
  QList<QList<int>> lines;
  qreal lineCenter = 0;
  qreal lineHeight = 0;
  for (const int index : indexes) {
    if (index < 0 || index >= page.words.size()) {
      continue;
    }
    const QRectF box = page.words.at(index).box;
    const bool newLine =
        lines.isEmpty()
        || qAbs(box.center().y() - lineCenter) > kLineTolerance * qMax(box.height(), lineHeight);
    if (newLine) {
      lines.append(QList<int>{index});
      lineCenter = box.center().y();
      lineHeight = box.height();
      continue;
    }
    const qsizetype count = lines.last().size();
    lineCenter = (lineCenter * count + box.center().y()) / (count + 1);
    lineHeight = qMax(lineHeight, box.height());
    lines.last().append(index);
  }
  return lines;
}

} // namespace

namespace PdfSupport {

bool available() {
  return !QStandardPaths::findExecutable(QStringLiteral("pdftoppm")).isEmpty();
}

bool textAvailable() {
  return !QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty();
}

QList<int> findPages(const QString& documentText, const QString& query) {
  QList<int> pages;
  const QString needle = normalized(query).toCaseFolded();
  if (needle.isEmpty()) {
    return pages;
  }
  const QStringList pageTexts = documentText.split(QChar(0x0C));
  for (qsizetype index = 0; index < pageTexts.size(); ++index) {
    const QString& page = pageTexts.at(index);
    if (page.trimmed().isEmpty()) {
      continue;
    }
    if (normalized(page).toCaseFolded().contains(needle)) {
      pages.append(static_cast<int>(index) + 1);
    }
  }
  return pages;
}

QImage renderPage(const QString& path, int page, const QSize& target) {
  const QString executable = QStandardPaths::findExecutable(QStringLiteral("pdftoppm"));
  if (executable.isEmpty() || !QFileInfo(path).isFile()) {
    return {};
  }
  QTemporaryDir temporary;
  if (!temporary.isValid()) {
    return {};
  }
  const QString prefix = temporary.filePath(QStringLiteral("page"));
  const int edge = qBound(128, qMax(target.width(), target.height()), 3840);
  QProcess process;
  process.start(executable,
                {QStringLiteral("-f"), QString::number(qMax(1, page)),
                 QStringLiteral("-l"), QString::number(qMax(1, page)),
                 QStringLiteral("-singlefile"), QStringLiteral("-scale-to"),
                 QString::number(edge), QStringLiteral("-png"), path, prefix});
  if (!process.waitForFinished(12000) || process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0) {
    return {};
  }
  return QImage(prefix + QStringLiteral(".png"));
}

PdfPageText parsePageWords(const QByteArray& xml) {
  PdfPageText page;
  QXmlStreamReader reader(xml);
  bool readPage = false;
  while (!reader.atEnd()) {
    reader.readNext();
    if (!reader.isStartElement()) {
      continue;
    }
    const QStringView name = reader.name();
    if (name == QStringLiteral("page")) {
      if (readPage) {
        break; // Only the first page of the document is read.
      }
      const QXmlStreamAttributes attributes = reader.attributes();
      page.pageSize = QSizeF(attributes.value(QStringLiteral("width")).toDouble(),
                             attributes.value(QStringLiteral("height")).toDouble());
      if (page.pageSize.width() <= 0 || page.pageSize.height() <= 0) {
        return {};
      }
      readPage = true;
      continue;
    }
    if (name != QStringLiteral("word") || !readPage) {
      continue;
    }
    const QXmlStreamAttributes attributes = reader.attributes();
    const QPointF topLeft(attributes.value(QStringLiteral("xMin")).toDouble(),
                          attributes.value(QStringLiteral("yMin")).toDouble());
    const QPointF bottomRight(attributes.value(QStringLiteral("xMax")).toDouble(),
                              attributes.value(QStringLiteral("yMax")).toDouble());
    PdfWord word;
    word.box = QRectF(topLeft, bottomRight);
    word.text = reader.readElementText();
    if (!word.text.isEmpty() && word.box.width() > 0 && word.box.height() > 0) {
      page.words.append(word);
    }
  }
  if (reader.hasError()) {
    return {};
  }
  return page;
}

QList<int> wordsTouched(const PdfPageText& page, const QRectF& area) {
  QList<int> indexes;
  const QRectF wanted = area.normalized();
  if (page.pageSize.width() <= 0 || page.pageSize.height() <= 0 || wanted.isEmpty()) {
    return indexes;
  }
  const QRectF box(wanted.x() * page.pageSize.width(), wanted.y() * page.pageSize.height(),
                   wanted.width() * page.pageSize.width(),
                   wanted.height() * page.pageSize.height());
  for (qsizetype index = 0; index < page.words.size(); ++index) {
    const QRectF overlap = box.intersected(page.words.at(index).box);
    if (overlap.width() > 0 && overlap.height() > 0) {
      indexes.append(static_cast<int>(index));
    }
  }
  return indexes;
}

QString wordsText(const PdfPageText& page, const QList<int>& indexes) {
  QStringList lines;
  for (const QList<int>& line : groupLines(page, indexes)) {
    QStringList words;
    words.reserve(line.size());
    for (const int index : line) {
      words.append(page.words.at(index).text);
    }
    lines.append(words.join(QLatin1Char(' ')));
  }
  return lines.join(QLatin1Char('\n'));
}

QList<QRectF> wordLines(const PdfPageText& page, const QList<int>& indexes) {
  QList<QRectF> lines;
  if (page.pageSize.width() <= 0 || page.pageSize.height() <= 0) {
    return lines;
  }
  for (const QList<int>& line : groupLines(page, indexes)) {
    QRectF box;
    for (const int index : line) {
      const QRectF word = page.words.at(index).box;
      box = box.isNull() ? word : box.united(word);
    }
    lines.append(QRectF(box.x() / page.pageSize.width(), box.y() / page.pageSize.height(),
                        box.width() / page.pageSize.width(),
                        box.height() / page.pageSize.height()));
  }
  return lines;
}

} // namespace PdfSupport
