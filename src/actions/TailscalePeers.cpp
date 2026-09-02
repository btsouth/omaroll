#include "actions/TailscalePeers.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <tuple>

using namespace Qt::StringLiterals;

TailscalePeers::TailscalePeers(QObject* parent) : QObject(parent) {}

bool TailscalePeers::parse(const QByteArray& statusJson, QVariantList* peers, QString* error) {
  peers->clear();
  error->clear();

  const QJsonDocument document = QJsonDocument::fromJson(statusJson);
  if (!document.isObject()) {
    *error = u"Tailscale gave an answer Omaroll could not read"_s;
    return false;
  }
  const QJsonObject status = document.object();

  const QString state = status.value(u"BackendState"_s).toString();
  if (state != u"Running"_s) {
    const QString why = state == u"NeedsLogin"_s ? u"not logged in"_s
                        : state == u"Stopped"_s  ? u"stopped"_s
                                                 : u"not running"_s;
    *error = u"Tailscale is %1. Run: sudo tailscale up"_s.arg(why);
    return false;
  }

  const qint64 self = status.value(u"Self"_s).toObject().value(u"UserID"_s).toVariant().toLongLong();
  const QJsonObject peerMap = status.value(u"Peer"_s).toObject();
  for (const QJsonValue& value : peerMap) {
    const QJsonObject peer = value.toObject();

    // TaildropTarget is the daemon's own verdict: 1 means this machine can
    // take a file right now, anything above explains why not (offline, owned
    // by someone else, no peer API). A daemon too old to report it says 0, and
    // then online and same owner is the best available guess, since Taildrop
    // only ever works between one user's own machines.
    const int target = peer.value(u"TaildropTarget"_s).toInt();
    const bool sameOwner = peer.value(u"UserID"_s).toVariant().toLongLong() == self;
    const bool canReceive =
        target == 1 || (target == 0 && sameOwner && peer.value(u"Online"_s).toBool());
    if (!canReceive) {
      continue;
    }

    const QString host = peer.value(u"HostName"_s).toString();
    QString machine = peer.value(u"DNSName"_s).toString();
    while (machine.endsWith(QLatin1Char('.'))) {
      machine.chop(1);
    }
    if (machine.isEmpty()) {
      machine = host;
    }
    if (machine.isEmpty()) {
      continue;
    }

    peers->append(QVariantMap{
        {u"name"_s, host.isEmpty() ? machine : host},
        {u"machine"_s, machine},
        {u"os"_s, peer.value(u"OS"_s).toString()},
    });
  }

  std::sort(peers->begin(), peers->end(), [](const QVariant& a, const QVariant& b) {
    return QString::compare(a.toMap().value(u"name"_s).toString(),
                            b.toMap().value(u"name"_s).toString(), Qt::CaseInsensitive) < 0;
  });

  if (peers->isEmpty()) {
    *error = u"No other machine of yours can take a file right now"_s;
  }
  return true;
}

void TailscalePeers::refresh() {
  if (m_busy) {
    return;
  }

  const QString tailscale = QStandardPaths::findExecutable(u"tailscale"_s);
  if (tailscale.isEmpty()) {
    m_peers.clear();
    m_error = u"Tailscale is not installed. Try: sudo pacman -S tailscale"_s;
    emit changed();
    return;
  }

  m_busy = true;
  m_error.clear();
  emit changed();

  // Asynchronous: the daemon answers in milliseconds when it is up, but a
  // stopped one can sit on the socket, and the sheet has to stay live.
  auto* process = new QProcess(this);
  auto* timeout = new QTimer(process);
  timeout->setSingleShot(true);
  timeout->setInterval(5000);
  connect(timeout, &QTimer::timeout, process, &QProcess::kill);

  connect(process, &QProcess::finished, this, [this, process](int, QProcess::ExitStatus status) {
    process->deleteLater();
    const QByteArray answer = process->readAllStandardOutput();
    if (status != QProcess::NormalExit || answer.trimmed().isEmpty()) {
      m_peers.clear();
      m_error = u"Tailscale did not answer. Is tailscaled running?"_s;
    } else {
      std::ignore = parse(answer, &m_peers, &m_error);
    }
    m_busy = false;
    emit changed();
  });
  connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) {
      return;
    }
    process->deleteLater();
    m_peers.clear();
    m_error = u"Could not start tailscale"_s;
    m_busy = false;
    emit changed();
  });

  process->start(tailscale, {u"status"_s, u"--json"_s});
  timeout->start();
}
