#include "edit/ClipboardText.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>

namespace ClipboardText {

bool offer(const QString& text) {
  if (text.isEmpty()) {
    return false;
  }

  const QByteArray utf8 = text.toUtf8();
  const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
  if (!wlCopy.isEmpty()) {
    QProcess process;
    process.start(wlCopy, {QStringLiteral("--type"), QStringLiteral("text/plain;charset=utf-8")});
    process.write(utf8);
    process.closeWriteChannel();
    if (process.waitForFinished(3000)) {
      return true;
    }
  }

  if (QGuiApplication::clipboard()) {
    QGuiApplication::clipboard()->setText(text);
    return true;
  }
  return false;
}

} // namespace ClipboardText
