#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <memory>

class CaptureModel;

// Finds visually similar still images on demand. Results are review
// suggestions only: no file is ever changed or removed automatically.
//
// A 64-bit difference hash only nominates candidate pairs. Each pair is then
// checked against a 32x32 greyscale thumbnail, which survives resizing,
// recompression and brightness changes but tells apart two screenshots of
// different text. Low-colour, mostly flat pictures (terminals, documents,
// chat windows) need a much closer match, and when both have indexed text
// that text must agree. Sets use complete linkage: a picture joins a set only
// when it matches every member, so A~B and B~C never put A with C.
//
// Signatures are kept in ~/.cache/omaroll, keyed by path, size and
// modification time, so a relaunch does not decode every still again.
class SimilarityIndex final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY activeChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
  Q_PROPERTY(int completed READ completed NOTIFY progressChanged)
  Q_PROPERTY(int total READ total NOTIFY progressChanged)
  Q_PROPERTY(int similarCount READ similarCount NOTIFY groupsChanged)
  Q_PROPERTY(int groupCount READ groupCount NOTIFY groupsChanged)
  Q_PROPERTY(int strictness READ strictness WRITE setStrictness NOTIFY strictnessChanged)

public:
  enum Strictness { Strict = 0, Normal = 1, Loose = 2 };
  Q_ENUM(Strictness)

  explicit SimilarityIndex(CaptureModel* model, QObject* parent = nullptr);
  ~SimilarityIndex() override;

  [[nodiscard]] bool active() const { return m_active; }
  [[nodiscard]] bool scanning() const { return m_scanning; }
  [[nodiscard]] int completed() const { return m_completed; }
  [[nodiscard]] int total() const { return m_total; }
  [[nodiscard]] int similarCount() const { return m_groups.size(); }
  [[nodiscard]] int groupCount() const;
  [[nodiscard]] const QHash<QString, QString>& groups() const { return m_groups; }
  [[nodiscard]] int strictness() const { return m_strictness; }
  void setStrictness(int strictness);
  // Pictures decoded by the last finished scan, rather than taken from the
  // memory or disk cache. For tests and diagnostics.
  [[nodiscard]] int lastDecoded() const { return m_lastDecoded; }

  // Every path in this picture's similar set, itself included, sorted. Empty
  // when it is not part of a set.
  Q_INVOKABLE [[nodiscard]] QStringList groupPaths(const QString& path) const;

  void requestStop();
  void setActive(bool active);
  Q_INVOKABLE void refresh();
  // Recognized text for a picture, from the text index. Empty clears it.
  void setText(const QString& path, const QString& text);

  [[nodiscard]] static QString cachePath();

signals:
  void activeChanged();
  void scanningChanged();
  void progressChanged();
  void groupsChanged();
  void strictnessChanged();

private:
  struct Candidate {
    QString path;
    qint64 bytes = 0;
    qint64 modified = 0;
  };

public:
  struct Signature {
    qint64 bytes = 0;
    qint64 modified = 0;
    quint64 hash = 0;
    int red = 0;
    int green = 0;
    int blue = 0;
    int width = 0;
    int height = 0;
    // Share of the picture within a narrow band of its most common grey,
    // and its average colourfulness, both scaled to 0..255.
    int flatness = 0;
    int chroma = 0;
    // 32x32 greyscale, row by row.
    QByteArray thumbnail;
    // Derived from the thumbnail, not stored.
    qint64 sum = 0;
    qint64 sumSquares = 0;
    bool valid = false;

    void prepare();
    [[nodiscard]] bool textLike() const;
    [[nodiscard]] QByteArray serialize() const;
    [[nodiscard]] static Signature deserialize(qint64 bytes, qint64 modified,
                                               const QByteArray& data);
  };

  // Whether two signatures describe the same picture at a strictness, and
  // how closely (higher is closer). The word lists come from recognized text
  // and may be empty. Public for tests.
  [[nodiscard]] static bool matches(const Signature& left, const Signature& right,
                                    const QStringList& leftWords,
                                    const QStringList& rightWords, int strictness,
                                    double* score = nullptr);
  // Sets of indexes into `signatures`, each of two or more pictures that all
  // match one another. `words` is parallel to `signatures`. A set flag stops
  // early and returns nothing.
  [[nodiscard]] static QList<QList<int>> group(const QList<Signature>& signatures,
                                               const QList<QStringList>& words, int strictness,
                                               const std::atomic_bool* cancel = nullptr);
  [[nodiscard]] static Signature signatureOf(const QString& path,
                                             const std::atomic_bool* cancel = nullptr);
  [[nodiscard]] static QStringList wordsOf(const QString& text);

private:
  struct Result {
    quint64 generation = 0;
    QHash<QString, QString> groups;
    QHash<QString, Signature> cache;
    int decoded = 0;
    bool complete = false;
  };

  void markDirty();
  void start();
  void setScanning(bool scanning);
  void setProgress(int completed, int total);
  [[nodiscard]] QList<Candidate> candidates() const;

  CaptureModel* m_model = nullptr;
  QFutureWatcher<Result> m_watcher;
  QTimer m_refreshTimer;
  std::shared_ptr<std::atomic_bool> m_cancel;
  QHash<QString, Signature> m_cache;
  QHash<QString, QString> m_groups;
  QHash<QString, QStringList> m_words;
  quint64 m_generation = 0;
  int m_strictness = Normal;
  int m_lastDecoded = 0;
  bool m_active = false;
  bool m_scanning = false;
  bool m_dirty = true;
  bool m_restartQueued = false;
  // The disk cache is read by the first scan after activation and again
  // after the memory copy is released.
  bool m_diskLoaded = false;
  int m_completed = 0;
  int m_total = 0;
};
