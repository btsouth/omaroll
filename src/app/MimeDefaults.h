#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QStringList>

// Omaroll as the desktop's default for pictures and videos, through xdg-mime.
// --set-default and --unset-default call apply() directly; Settings and the
// empty library use the same calls on a worker, since xdg-mime runs once or
// twice per type and that takes seconds for every picture and video type.
class MimeDefaults final : public QObject {
  Q_OBJECT
  // Whether JPEG pictures and MP4 videos open with Omaroll now, once
  // refresh() has asked. Unknown until then.
  Q_PROPERTY(bool known READ known NOTIFY stateChanged)
  Q_PROPERTY(bool imagesDefault READ imagesDefault NOTIFY stateChanged)
  Q_PROPERTY(bool videosDefault READ videosDefault NOTIFY stateChanged)
  // A change or a check is running.
  Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
public:
  explicit MimeDefaults(QObject* parent = nullptr);
  ~MimeDefaults() override;
  static QStringList types(const QString &desktopFile, const QString &group);
  // Returns a printable report, including any failures. No UI is required.
  Q_INVOKABLE QString apply(bool unset = false,
                            const QString &group = QStringLiteral("all"));
  bool succeeded() const { return m_succeeded; }
  // The same change on a worker. finished() says what changed in a sentence
  // and carries the full report.
  Q_INVOKABLE void applyAsync(bool unset = false,
                              const QString &group = QStringLiteral("all"));
  // Asks again whether Omaroll is the default, on a worker.
  Q_INVOKABLE void refresh();
  // One sentence for a report from apply(): how many types changed, and
  // which apps were replaced or put back, worded for the group.
  static QString summarize(bool unset, const QString &report, bool ok,
                           const QString &group = QStringLiteral("all"));

  bool known() const { return m_known; }
  bool imagesDefault() const { return m_imagesDefault; }
  bool videosDefault() const { return m_videosDefault; }
  bool busy() const { return m_applying || m_checking; }

signals:
  void stateChanged();
  void finished(bool ok, const QString &summary, const QString &report);

private:
  struct Result {
    QString report;
    bool ok = false;
  };
  struct State {
    bool images = false;
    bool videos = false;
  };
  static Result run(bool unset, const QString &group);
  static State query();

  bool m_succeeded = false;
  bool m_known = false;
  bool m_imagesDefault = false;
  bool m_videosDefault = false;
  bool m_applying = false;
  bool m_checking = false;
  bool m_recheck = false;
  bool m_unset = false;
  QString m_group;
  QFutureWatcher<Result> m_change;
  QFutureWatcher<State> m_check;
};
