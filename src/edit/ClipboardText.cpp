#include "edit/ClipboardText.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

namespace ClipboardText {

bool offer(const QString& text) {
  if (text.isEmpty()) {
    return false;
  }

  const QByteArray utf8 = text.toUtf8();
  const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
  if (!wlCopy.isEmpty()) {
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(wlCopy, {QStringLiteral("--type"), QStringLiteral("text/plain;charset=utf-8")});
    process.write(utf8);
    process.closeWriteChannel();
    const bool finished = process.waitForFinished(3000);
    if (!finished) {
      process.kill();
      process.waitForFinished(500);
    }
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
  }

  auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
  if (!application || QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
    return false;
  }
  bool offered = false;
  const auto fallback = [&] {
    if (QGuiApplication::clipboard()) {
      QGuiApplication::clipboard()->setText(text);
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

} // namespace ClipboardText
