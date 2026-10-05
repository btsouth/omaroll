#include "pdf/PdfSupport.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <cmath>
#include <sys/resource.h>

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

QByteArray toolOutput(const QString& executable, const QStringList& arguments, int timeout) {
  QProcess process;
  PdfSupport::limitProcess(process);
  process.setStandardErrorFile(QProcess::nullDevice());
  QByteArray output("");
  bool exceeded = false;
  const auto drain = [&] {
    const QByteArray chunk = process.readAllStandardOutput();
    if (output.size() + chunk.size() > 64 * 1024) {
      exceeded = true;
      process.kill();
    } else if (!exceeded) {
      output += chunk;
    }
  };
  QObject::connect(&process, &QProcess::readyReadStandardOutput, drain);
  process.start(executable, arguments);
  if (!process.waitForFinished(timeout)) {
    process.kill();
    process.waitForFinished(1000);
    return {};
  }
  drain();
  if (exceeded || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    return {};
  }
  return output;
}

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
    // The first word on a line fixes it, so the grouping does not drift as the
    // selection grows.
    const bool newLine =
        lines.isEmpty()
        || qAbs(box.center().y() - lineCenter) > kLineTolerance * qMax(box.height(), lineHeight);
    if (newLine) {
      lines.append(QList<int>{index});
      lineCenter = box.center().y();
      lineHeight = box.height();
      continue;
    }
    lineHeight = qMax(lineHeight, box.height());
    lines.last().append(index);
  }
  return lines;
}

} // namespace

namespace PdfSupport {

void limitProcess(QProcess& process) {
  process.setStandardInputFile(QProcess::nullDevice());
  process.setChildProcessModifier([&process] {
    const rlimit memory{512 * 1024 * 1024, 512 * 1024 * 1024};
    const rlimit file{64 * 1024 * 1024, 64 * 1024 * 1024};
    const rlimit cpu{30, 30};
    if (::setrlimit(RLIMIT_AS, &memory) != 0 || ::setrlimit(RLIMIT_FSIZE, &file) != 0 ||
        ::setrlimit(RLIMIT_CPU, &cpu) != 0) {
      process.failChildProcessModifier("Could not limit PDF resources");
    }
  });
}

bool available() {
  return !QStandardPaths::findExecutable(QStringLiteral("pdftoppm")).isEmpty();
}

bool textAvailable() {
  return !QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty();
}

QList<int> findPages(const QString& documentText, const QString& query, bool* tooManyPages) {
  if (tooManyPages) *tooManyPages = false;
  QList<int> pages;
  const QString needle = normalized(query).toCaseFolded();
  if (needle.isEmpty()) return pages;
  qsizetype start = 0;
  int pageNumber = 1;
  while (start < documentText.size()) {
    if (pageNumber > 100000) {
      if (tooManyPages) *tooManyPages = true;
      return {};
    }
    const qsizetype separator = documentText.indexOf(QChar(0x0C), start);
    const qsizetype end = separator < 0 ? documentText.size() : separator;
    const QString page = documentText.mid(start, end - start);
    if (normalized(page).toCaseFolded().contains(needle)) {
      pages.append(pageNumber);
    }
    if (separator < 0) break;
    start = separator + 1;
    ++pageNumber;
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
  QStringList arguments{QStringLiteral("-f"), QString::number(qMax(1, page)),
                        QStringLiteral("-l"), QString::number(qMax(1, page)),
                        QStringLiteral("-singlefile")};
  const QByteArray info = toolOutput(QStandardPaths::findExecutable(QStringLiteral("pdfinfo")),
      {QStringLiteral("-f"), QString::number(qMax(1, page)), QStringLiteral("-l"),
       QString::number(qMax(1, page)), path}, 10000);
  static const QRegularExpression geometry(
      QStringLiteral(R"(^(?:Page\s+\d+\s+size|Page size):\s+([\d.]+)\s+x\s+([\d.]+))"),
      QRegularExpression::MultilineOption);
  const auto match = geometry.match(QString::fromLocal8Bit(info));
  qreal width = match.captured(1).toDouble();
  qreal height = match.captured(2).toDouble();
  static const QRegularExpression rotation(
      QStringLiteral(R"(^(?:Page\s+\d+\s+rot|Page rot):\s+(-?\d+))"),
      QRegularExpression::MultilineOption);
  const int degrees = rotation.match(QString::fromLocal8Bit(info)).captured(1).toInt();
  if (qAbs(degrees % 180) == 90) std::swap(width, height);
  if (!match.hasMatch() || !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
    return {};
  }
  qreal scale;
  if (target.width() > 0 && target.height() > 0) {
    scale = qBound(128, qMax(target.width(), target.height()), 3840) / qMax(width, height);
  } else if (target.width() > 0) {
    scale = qBound(128, target.width(), 3840) / width;
  } else {
    scale = qBound(128, target.height(), 3840) / height;
  }
  scale = qMin(scale, 3840.0 / qMax(width, height));
  scale = qMin(scale, std::sqrt(qreal(kPixelLimit - 2 * 3840) / width / height));
  const int rasterWidth = qMax(1, int(std::floor(width * scale + 0.000001)));
  const int rasterHeight = qMax(1, int(std::floor(height * scale + 0.000001)));
  arguments << QStringLiteral("-scale-dimension-before-rotation")
            << QStringLiteral("-scale-to-x") << QString::number(rasterWidth)
            << QStringLiteral("-scale-to-y") << QString::number(rasterHeight)
            << QStringLiteral("-png") << path << prefix;
  // Both raster edges and their product are bounded before Poppler allocates.
  if (toolOutput(executable, arguments, 12000).isNull()) {
    return {};
  }
  return QImage(prefix + QStringLiteral(".png"));
}

PdfPageText parsePageWords(const QByteArray& xml) {
  PdfPageText page;
  if (xml.size() > kPageByteLimit) return page;
  QXmlStreamReader reader(xml);
  bool readPage = false;
  qsizetype textSize = 0;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isDTD() && !reader.entityDeclarations().isEmpty()) return {};
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
      if (!std::isfinite(page.pageSize.width()) || !std::isfinite(page.pageSize.height()) ||
          page.pageSize.width() <= 0 || page.pageSize.height() <= 0) {
        return {};
      }
      readPage = true;
      continue;
    }
    if (name != QStringLiteral("word") || !readPage) {
      continue;
    }
    const QXmlStreamAttributes attributes = reader.attributes();
    // A word without its box cannot be placed, so it is left out rather than
    // pinned to the page's corner by a zero default.
    const bool placed =
        attributes.hasAttribute(QStringLiteral("xMin"))
        && attributes.hasAttribute(QStringLiteral("yMin"))
        && attributes.hasAttribute(QStringLiteral("xMax"))
        && attributes.hasAttribute(QStringLiteral("yMax"));
    const QPointF topLeft(attributes.value(QStringLiteral("xMin")).toDouble(),
                          attributes.value(QStringLiteral("yMin")).toDouble());
    const QPointF bottomRight(attributes.value(QStringLiteral("xMax")).toDouble(),
                              attributes.value(QStringLiteral("yMax")).toDouble());
    PdfWord word;
    word.box = QRectF(topLeft, bottomRight);
    word.text = reader.readElementText();
    textSize += word.text.size();
    if (textSize > kPageByteLimit / 2 || page.words.size() >= 100000) return {};
    if (placed && !word.text.isEmpty() && word.box.width() > 0 && word.box.height() > 0) {
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
  const QRectF sheet(0, 0, page.pageSize.width(), page.pageSize.height());
  for (const QList<int>& line : groupLines(page, indexes)) {
    QRectF box;
    for (const int index : line) {
      const QRectF word = page.words.at(index).box;
      box = box.isNull() ? word : box.united(word);
    }
    // A highlight belongs on the page: poppler can report a box that reaches
    // past the edge, so it is clipped rather than drawn outside the sheet.
    box = box.intersected(sheet);
    if (box.isEmpty()) {
      continue;
    }
    lines.append(QRectF(box.x() / page.pageSize.width(), box.y() / page.pageSize.height(),
                        box.width() / page.pageSize.width(),
                        box.height() / page.pageSize.height()));
  }
  return lines;
}

} // namespace PdfSupport
