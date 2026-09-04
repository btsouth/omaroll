#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>

class CaptureModel;

// Finds visually similar still images on demand. A small perceptual difference
// hash survives resizing and ordinary JPEG recompression. Results are review
// suggestions only: no file is ever changed or removed automatically.
class SimilarityIndex final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY activeChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
  Q_PROPERTY(int completed READ completed NOTIFY progressChanged)
  Q_PROPERTY(int total READ total NOTIFY progressChanged)
  Q_PROPERTY(int similarCount READ similarCount NOTIFY groupsChanged)
  Q_PROPERTY(int groupCount READ groupCount NOTIFY groupsChanged)

public:
  explicit SimilarityIndex(CaptureModel* model, QObject* parent = nullptr);
  ~SimilarityIndex() override;

  [[nodiscard]] bool active() const { return m_active; }
  [[nodiscard]] bool scanning() const { return m_scanning; }
  [[nodiscard]] int completed() const { return m_completed; }
  [[nodiscard]] int total() const { return m_total; }
  [[nodiscard]] int similarCount() const { return m_groups.size(); }
  [[nodiscard]] int groupCount() const;
  [[nodiscard]] const QHash<QString, QString>& groups() const { return m_groups; }

  void setActive(bool active);
  Q_INVOKABLE void refresh();

signals:
  void activeChanged();
  void scanningChanged();
  void progressChanged();
  void groupsChanged();

private:
  struct Candidate {
    QString path;
    qint64 bytes = 0;
    qint64 modified = 0;
  };
  struct CachedHash {
    qint64 bytes = 0;
    qint64 modified = 0;
    quint64 hash = 0;
    int red = 0;
    int green = 0;
    int blue = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
  };
  struct Result {
    quint64 generation = 0;
    QHash<QString, QString> groups;
    QHash<QString, CachedHash> cache;
  };

  void markDirty();
  void start();
  void setScanning(bool scanning);
  void setProgress(int completed, int total);
  [[nodiscard]] QList<Candidate> candidates() const;
  [[nodiscard]] static CachedHash hashFile(const Candidate& candidate);

  CaptureModel* m_model = nullptr;
  QFutureWatcher<Result> m_watcher;
  QTimer m_refreshTimer;
  std::shared_ptr<std::atomic_bool> m_cancel;
  QHash<QString, CachedHash> m_cache;
  QHash<QString, QString> m_groups;
  quint64 m_generation = 0;
  bool m_active = false;
  bool m_scanning = false;
  bool m_dirty = true;
  bool m_restartQueued = false;
  int m_completed = 0;
  int m_total = 0;
};
