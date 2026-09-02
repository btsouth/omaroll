#pragma once

#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <optional>

class CaptureModel;

// Checks only the image currently open in the viewer. Detection retains a
// yes/no value keyed by file identity, never the decoded QR payload.
class QrDetector final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ available CONSTANT)
  Q_PROPERTY(QString path READ path NOTIFY stateChanged)
  Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
  Q_PROPERTY(bool detected READ detected NOTIFY stateChanged)

public:
  explicit QrDetector(CaptureModel* model, QObject* parent = nullptr);
  ~QrDetector() override;

  [[nodiscard]] bool available() const { return !m_program.isEmpty(); }
  [[nodiscard]] QString path() const { return m_path; }
  [[nodiscard]] bool checking() const { return m_checking; }
  [[nodiscard]] bool detected() const { return m_detected; }

  Q_INVOKABLE void inspect(const QString& path);
  Q_INVOKABLE void clear();

signals:
  void stateChanged();

private:
  struct Candidate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
  };

  struct Entry {
    qint64 modified = 0;
    qint64 bytes = 0;
    bool detected = false;
  };

  void startRequested();
  void finishCurrent(bool completedNormally, bool successful);
  [[nodiscard]] bool stillCurrent(const Candidate& candidate) const;
  [[nodiscard]] static bool sameIdentity(const Candidate& left, const Candidate& right);

  CaptureModel* m_model = nullptr;
  QString m_program;
  QHash<QString, Entry> m_cache;
  std::optional<Candidate> m_requested;
  std::optional<Candidate> m_current;
  QProcess m_process;
  QTimer m_timeout;
  QString m_path;
  bool m_checking = false;
  bool m_detected = false;
};
