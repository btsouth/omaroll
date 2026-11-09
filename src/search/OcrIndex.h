#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class CaptureModel;

// A local text index for still images.
//
// By default nothing is read until a search has at least two characters. At
// that point cached text is restored and missing entries are sent to one
// Tesseract process at a time. Clearing the search pauses the queue after the
// current image, keeping background CPU use predictable.
//
// Background indexing is opt-in. Once the library scan has settled it reads
// the rest of the library with two to four Tesseract processes at the lowest
// CPU and I/O priority, screenshots first or screenshots only, and pauses
// while the computer runs on battery.
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
  Q_PROPERTY(bool background READ background WRITE setBackground NOTIFY backgroundChanged)
  Q_PROPERTY(bool screenshotsOnly READ screenshotsOnly WRITE setScreenshotsOnly NOTIFY
                 backgroundChanged)
  // Why background indexing is waiting, or empty while it runs or is off.
  Q_PROPERTY(QString pauseReason READ pauseReason NOTIFY backgroundStateChanged)
  Q_PROPERTY(bool backgroundRunning READ backgroundRunning NOTIFY backgroundStateChanged)
  // Pictures the background indexer covers, and how many of those have been
  // read (or could not be). Counted while a search or the indexer is active.
  Q_PROPERTY(int indexedCount READ indexedCount NOTIFY coverageChanged)
  Q_PROPERTY(int eligibleCount READ eligibleCount NOTIFY coverageChanged)
  // Stills whose text has not been read yet, whatever the mode. Search shows
  // a notice while this is above zero.
  Q_PROPERTY(int unreadCount READ unreadCount NOTIFY coverageChanged)
  Q_PROPERTY(int running READ running NOTIFY runningChanged)

public:
  explicit OcrIndex(CaptureModel* model, QObject* parent = nullptr);
  ~OcrIndex() override;
  void requestStop();

  [[nodiscard]] bool available() const { return !m_program.isEmpty(); }
  [[nodiscard]] bool indexing() const { return m_indexing; }
  [[nodiscard]] int completed() const { return m_completed; }
  [[nodiscard]] int total() const { return m_total; }
  [[nodiscard]] QString reviewPath() const { return m_reviewPath; }
  [[nodiscard]] QString reviewText() const { return m_reviewText; }
  [[nodiscard]] QString reviewError() const { return m_reviewError; }
  [[nodiscard]] bool reviewing() const { return m_reviewing; }
  [[nodiscard]] bool background() const { return m_background; }
  void setBackground(bool enabled);
  [[nodiscard]] bool screenshotsOnly() const { return m_screenshotsOnly; }
  void setScreenshotsOnly(bool value);
  [[nodiscard]] QString pauseReason() const;
  [[nodiscard]] bool backgroundRunning() const;
  [[nodiscard]] int indexedCount() const { return m_indexedCount; }
  [[nodiscard]] int eligibleCount() const { return m_eligibleCount; }
  [[nodiscard]] int unreadCount() const { return m_unreadCount; }
  [[nodiscard]] int running() const;

  // Background indexing: how many Tesseract processes may run at once
  // (default two to four, from the processor count), how long the library
  // must stay unchanged after a scan, and where power supplies are read.
  // The setters exist for tests.
  [[nodiscard]] int parallelLimit() const { return m_parallelLimit; }
  void setParallelLimit(int limit);
  void setSettleDelay(int milliseconds);
  void setPowerSupplyDirectory(const QString& directory);
  // Reads the power supplies now rather than at the next poll.
  Q_INVOKABLE void checkPower();
  // True when a mains or USB supply reports offline and a battery is
  // discharging. Unknown hardware counts as mains power.
  [[nodiscard]] static bool onBattery(const QString& directory);

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
  void backgroundChanged();
  void backgroundStateChanged();
  void coverageChanged();
  void runningChanged();

private:
  struct Candidate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
    bool screenshot = false;
  };

  struct Version {
    qint64 modified = 0;
    qint64 bytes = 0;
  };

  struct Entry {
    qint64 modified = 0;
    qint64 bytes = 0;
    QString text;
  };

  struct Worker {
    QProcess process;
    QTimer timeout;
    std::optional<Candidate> current;
    bool forReview = false;
    bool sparseTextPass = false;
    bool interrupt = false;
    bool requeue = false;
    QString firstPassText;
  };

  void sync();
  void processNext();
  [[nodiscard]] bool queueRunning() const;
  [[nodiscard]] int workerLimit() const;
  [[nodiscard]] Worker* idleWorker();
  [[nodiscard]] Worker* workerFor(const Candidate& candidate) const;
  void start(Worker* worker, const Candidate& candidate, bool forReview);
  void startProcess(Worker* worker, bool sparseText);
  void interrupt(Worker* worker, bool requeue);
  void finish(Worker* worker, bool successful);
  void reset(Worker* worker);
  void setIndexing(bool value);
  void resetProgress(int total);
  void advanceProgress();
  void updateBackgroundState();
  void scheduleCoverage();
  void updateCoverage();
  void noteCoverage(const Candidate& candidate, bool wasKnown);
  [[nodiscard]] bool stillCurrent(const Candidate& candidate) const;
  [[nodiscard]] QString cachePath(const QString& path) const;
  [[nodiscard]] bool readCache(const Candidate& candidate, QString* text) const;
  void writeCache(const Candidate& candidate, const QString& text) const;
  static void pruneCache();
  void adopt(const Candidate& candidate, const QString& text);
  void publishReview(const Candidate& candidate, const QString& text,
                     const QString& error = {});
  [[nodiscard]] bool failed(const Candidate& candidate) const;
  [[nodiscard]] bool known(const Candidate& candidate) const;

  CaptureModel* m_model = nullptr;
  QString m_program;
  QString m_languages;
  QString m_legacyLanguages;
  QHash<QString, Entry> m_entries;
  QList<Candidate> m_queue;
  // The version of each picture Tesseract could not read.
  QHash<QString, Version> m_failed;
  std::vector<std::unique_ptr<Worker>> m_workers;
  std::optional<Candidate> m_reviewCandidate;
  bool m_active = false;
  bool m_indexing = false;
  int m_completed = 0;
  int m_total = 0;
  QString m_reviewPath;
  qint64 m_reviewModified = 0;
  qint64 m_reviewBytes = 0;
  QString m_reviewText;
  QString m_reviewError;
  bool m_reviewing = false;

  bool m_background = false;
  bool m_screenshotsOnly = false;
  bool m_settled = false;
  bool m_onBattery = false;
  bool m_wasRunning = false;
  QString m_lastPauseReason;
  int m_parallelLimit = 2;
  QString m_powerDirectory = QStringLiteral("/sys/class/power_supply");
  QTimer m_settleTimer;
  QTimer m_powerTimer;
  QTimer m_coverageTimer;
  QList<Candidate> m_stills;
  QHash<QString, int> m_stillRows;
  bool m_coverageRecount = false;
  bool m_coverageSignal = false;
  int m_indexedCount = 0;
  int m_eligibleCount = 0;
  int m_unreadCount = 0;
};
