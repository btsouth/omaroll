#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

// The machines on the user's tailnet that can take a Taildrop, for the "Send
// to a machine" picker.
//
// omarchy-tailscale-send takes the machine as its first argument and there is
// no house picker for one outside the shell panel, so this is the one piece
// omaroll has to supply itself. It asks `tailscale status --json` and keeps
// only what the answer says can receive a file right now.
class TailscalePeers final : public QObject {
  Q_OBJECT
  // Rows for QML: name (short host name), machine (what to hand the sender),
  // os.
  Q_PROPERTY(QVariantList peers READ peers NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  // Why the list is empty, in the user's words. Empty while it is not.
  Q_PROPERTY(QString error READ error NOTIFY changed)

public:
  explicit TailscalePeers(QObject* parent = nullptr);

  [[nodiscard]] QVariantList peers() const { return m_peers; }
  [[nodiscard]] bool busy() const { return m_busy; }
  [[nodiscard]] QString error() const { return m_error; }

  // Ask tailscale again. Asynchronous; `changed` fires when the answer lands.
  Q_INVOKABLE void refresh();

  // Pure, so a test can feed it a status document. Returns false and sets
  // error when the tailnet is not usable at all; true with an empty list and
  // a set error when it is up but no machine can take a file.
  [[nodiscard]] static bool parse(const QByteArray& statusJson, QVariantList* peers,
                                  QString* error);

signals:
  void changed();

private:
  QVariantList m_peers;
  QString m_error;
  bool m_busy = false;
};
