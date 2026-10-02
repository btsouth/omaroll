#include "viewer/HyprlandPlacement.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

QString hyprctl() { return QStandardPaths::findExecutable(QStringLiteral("hyprctl")); }

QByteArray ask(const QString& executable, const QString& what) {
  QProcess process;
  process.start(executable, {QStringLiteral("-j"), what});
  if (!process.waitForFinished(300) || process.exitCode() != 0) {
    process.kill();
    process.waitForFinished(100);
    return {};
  }
  return process.readAllStandardOutput();
}

// Hyprland 0.56 reads dispatches as Lua; earlier releases take the classic
// dispatcher, and reject the Lua form with a non-zero exit.
void dispatchTiled(const QString& executable, const QString& address, bool classic) {
  auto* process = new QProcess(QCoreApplication::instance());
  QObject::connect(process, &QProcess::finished, process,
                   [process, executable, address, classic](int exitCode) {
                     process->deleteLater();
                     if (exitCode != 0 && !classic) {
                       dispatchTiled(executable, address, true);
                     }
                   });
  QObject::connect(process, &QProcess::errorOccurred, process, &QObject::deleteLater);
  process->start(executable,
                 classic ? QStringList{QStringLiteral("dispatch"), QStringLiteral("settiled"),
                                       QStringLiteral("address:") + address}
                         : QStringList{QStringLiteral("dispatch"),
                                       QStringLiteral("hl.dsp.window.float({ action = \"disable\", "
                                                      "window = \"address:%1\" })")
                                           .arg(address)});
}

} // namespace

namespace HyprlandPlacement {

Plan plan(const QByteArray& clients, const QByteArray& activeWorkspace, qint64 pid) {
  Plan result;
  const QJsonValue workspace = QJsonDocument::fromJson(activeWorkspace).object().value(u"id");
  if (!workspace.isDouble()) {
    return result;
  }
  const QString viewerTitle = QStringLiteral(" · Omaroll");
  for (const QJsonValue& value : QJsonDocument::fromJson(clients).array()) {
    const QJsonObject client = value.toObject();
    if (client.value(u"pid").toInteger(-1) != pid ||
        client.value(u"class").toString() != QLatin1String(kWindowClass) ||
        !client.value(u"title").toString().endsWith(viewerTitle) ||
        !client.value(u"mapped").toBool(true) ||
        client.value(u"workspace").toObject().value(u"id") != workspace) {
      continue;
    }
    ++result.viewers;
    const QString address = client.value(u"address").toString();
    // Only ever a hex address goes into a dispatch.
    static const QRegularExpression hex(QStringLiteral("^0x[0-9a-fA-F]+$"));
    if (client.value(u"floating").toBool() && hex.match(address).hasMatch()) {
      result.floating.append(address);
    }
  }
  return result;
}

bool available() {
  // Offscreen runs (tests, renders) never reach a compositor, even with a
  // Hyprland session in the environment.
  return QGuiApplication::platformName() == u"wayland" &&
         !qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE").isEmpty() && !hyprctl().isEmpty();
}

Plan query() {
  if (!available()) {
    return {};
  }
  const QString executable = hyprctl();
  const QByteArray workspace = ask(executable, QStringLiteral("activeworkspace"));
  if (workspace.isEmpty()) {
    return {};
  }
  return plan(ask(executable, QStringLiteral("clients")), workspace,
              QCoreApplication::applicationPid());
}

void tile(const QStringList& addresses) {
  if (addresses.isEmpty() || !available()) {
    return;
  }
  const QString executable = hyprctl();
  for (const QString& address : addresses) {
    dispatchTiled(executable, address, false);
  }
}

} // namespace HyprlandPlacement
