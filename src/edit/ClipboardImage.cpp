#include "edit/ClipboardImage.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

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

  auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
  if (!application || QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
    return false;
  }
  bool offered = false;
  const auto fallback = [&] {
    if (QGuiApplication::clipboard()) {
      QGuiApplication::clipboard()->setImage(image);
      offered = true;
    }
  };
  if (QThread::currentThread() == application->thread()) {
    fallback();
  } else {
    QMetaObject::invokeMethod(application, fallback, Qt::BlockingQueuedConnection);
  }
  return offered;
}

} // namespace ClipboardImage
