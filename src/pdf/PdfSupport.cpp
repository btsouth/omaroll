#include "pdf/PdfSupport.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

QString normalized(const QString& text) {
  QString out = text;
  out.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  return out.trimmed();
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

} // namespace PdfSupport
