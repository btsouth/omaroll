#include "edit/ClipboardImage.h"

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
      process.start(wlCopy, {QStringLiteral("--type"), QStringLiteral("image/png")});
      process.write(png);
      process.closeWriteChannel();
      if (process.waitForFinished(3000)) {
        return true;
      }
    }
  }

  if (QGuiApplication::clipboard()) {
    QGuiApplication::clipboard()->setImage(image);
    return true;
  }
  return false;
}

} // namespace ClipboardImage
