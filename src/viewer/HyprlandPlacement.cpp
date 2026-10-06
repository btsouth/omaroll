#include "viewer/HyprlandPlacement.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

namespace {

QString hyprctl() { return QStandardPaths::findExecutable(QStringLiteral("hyprctl")); }

class PlacementQuery final : public QObject {
public:
  PlacementQuery(QObject* context, HyprlandPlacement::Reply reply)
      : QObject(context), m_reply(std::move(reply)) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(300);
    connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) finish({});
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
      m_timeout.stop();
      if (code != 0 || status != QProcess::NormalExit) {
        finish({});
        return;
      }
      const QByteArray output = m_process.readAllStandardOutput();
      if (m_step == 0) {
        m_workspace = output;
        ++m_step;
        ask(QStringLiteral("clients"));
      } else if (m_step == 1) {
        m_clients = output;
        ++m_step;
        ask(QStringLiteral("activeworkspace"));
      } else {
        const QJsonValue before = QJsonDocument::fromJson(m_workspace).object().value(u"id");
        const QJsonValue after = QJsonDocument::fromJson(output).object().value(u"id");
        finish(before.isDouble() && before == after
                   ? HyprlandPlacement::plan(m_clients, output, QCoreApplication::applicationPid())
                   : HyprlandPlacement::Plan{});
      }
    });
    ask(QStringLiteral("activeworkspace"));
  }

private:
  void ask(const QString& what) {
    m_process.start(hyprctl(), {QStringLiteral("-j"), what});
    m_timeout.start();
  }
  void finish(HyprlandPlacement::Plan plan) {
    if (!m_reply) return;
    auto reply = std::exchange(m_reply, {});
    deleteLater();
    reply(std::move(plan));
  }
  QProcess m_process;
  QTimer m_timeout;
  HyprlandPlacement::Reply m_reply;
  QByteArray m_workspace;
  QByteArray m_clients;
  int m_step = 0;
};

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

void query(QObject* context, Reply reply) {
  if (!available()) {
    reply({});
    return;
  }
  new PlacementQuery(context, std::move(reply));
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
