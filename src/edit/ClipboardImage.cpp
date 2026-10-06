#include "edit/ClipboardImage.h"
#include "edit/ClipboardFallback.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>

namespace ClipboardImage {

bool offer(const QImage& image) {
  if (image.isNull()) {
    return false;
  }

  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  if (image.save(&buffer, "PNG")) {
    const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
    if (!wlCopy.isEmpty()) {
      QProcess process;
      process.setStandardOutputFile(QProcess::nullDevice());
      process.setStandardErrorFile(QProcess::nullDevice());
      process.start(wlCopy, {QStringLiteral("--type"), QStringLiteral("image/png")});
      process.write(png);
      process.closeWriteChannel();
      const bool finished = process.waitForFinished(3000);
      if (!finished) {
        process.kill();
        process.waitForFinished(500);
      }
      return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }
  }

  return ClipboardFallback::onGuiThread([image] {
    if (!QGuiApplication::clipboard()) return false;
    QGuiApplication::clipboard()->setImage(image);
    return true;
  });
}

} // namespace ClipboardImage
