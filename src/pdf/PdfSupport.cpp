#include "pdf/PdfSupport.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace PdfSupport {

bool available() {
  return !QStandardPaths::findExecutable(QStringLiteral("pdftoppm")).isEmpty();
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
