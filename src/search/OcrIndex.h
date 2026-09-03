#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTimer>

#include <optional>

class CaptureModel;

// A local, lazy text index for still images.
//
// Nothing is read until a search has at least two characters. At that point
// cached text is restored and missing entries are sent to one Tesseract
// process at a time. Clearing the search pauses the queue after the current
// image, keeping background CPU use predictable.
class OcrIndex final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ available CONSTANT)
  Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)
  Q_PROPERTY(int completed READ completed NOTIFY progressChanged)
  Q_PROPERTY(int total READ total NOTIFY progressChanged)
  Q_PROPERTY(QString reviewPath READ reviewPath NOTIFY reviewChanged)
  Q_PROPERTY(QString reviewText READ reviewText NOTIFY reviewChanged)
  Q_PROPERTY(QString reviewError READ reviewError NOTIFY reviewChanged)
  Q_PROPERTY(bool reviewing READ reviewing NOTIFY reviewChanged)

public:
  explicit OcrIndex(CaptureModel* model, QObject* parent = nullptr);
  ~OcrIndex() override;

  [[nodiscard]] bool available() const { return !m_program.isEmpty(); }
  [[nodiscard]] bool indexing() const { return m_indexing; }
  [[nodiscard]] int completed() const { return m_completed; }
  [[nodiscard]] int total() const { return m_total; }
  [[nodiscard]] QString reviewPath() const { return m_reviewPath; }
  [[nodiscard]] QString reviewText() const { return m_reviewText; }
  [[nodiscard]] QString reviewError() const { return m_reviewError; }
  [[nodiscard]] bool reviewing() const { return m_reviewing; }

  void setSearchText(const QString& text);
  Q_INVOKABLE void recognize(const QString& path, bool refresh = false);
  Q_INVOKABLE void cancelReview();
  Q_INVOKABLE int clearCache();

  [[nodiscard]] static QString cacheDirectory();

signals:
  void textReady(const QString& path, const QString& text);
  void indexingChanged();
  void progressChanged();
  void reviewChanged();

private:
  struct Candidate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
  };

  struct Entry {
    qint64 modified = 0;
    qint64 bytes = 0;
    QString text;
  };

  void sync();
  void processNext();
  void startCurrentProcess(bool sparseText);
  void interruptCurrent(bool requeue);
  void finishCurrent(bool successful);
  void setIndexing(bool value);
  void resetProgress(int total);
  void advanceProgress();
  [[nodiscard]] bool stillCurrent(const Candidate& candidate) const;
  [[nodiscard]] QString cachePath(const QString& path) const;
  [[nodiscard]] bool readCache(const Candidate& candidate, QString* text) const;
  void writeCache(const Candidate& candidate, const QString& text) const;
  static void pruneCache();
  void adopt(const Candidate& candidate, const QString& text);
  void publishReview(const Candidate& candidate, const QString& text,
                     const QString& error = {});
  [[nodiscard]] QString failureKey(const Candidate& candidate) const;

  CaptureModel* m_model = nullptr;
  QString m_program;
  QString m_languages;
  QHash<QString, Entry> m_entries;
  QList<Candidate> m_queue;
  QSet<QString> m_failed;
  std::optional<Candidate> m_current;
  std::optional<Candidate> m_reviewCandidate;
  QProcess m_process;
  QTimer m_timeout;
  bool m_active = false;
  bool m_indexing = false;
  int m_completed = 0;
  int m_total = 0;
  bool m_currentForReview = false;
  bool m_sparseTextPass = false;
  bool m_interruptCurrent = false;
  bool m_requeueInterrupted = false;
  QString m_firstPassText;
  QString m_reviewPath;
  qint64 m_reviewModified = 0;
  qint64 m_reviewBytes = 0;
  QString m_reviewText;
  QString m_reviewError;
  bool m_reviewing = false;
};
