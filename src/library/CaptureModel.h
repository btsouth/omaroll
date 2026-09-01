#pragma once

#include "library/CaptureRecord.h"
#include "sources/CaptureScanner.h"

#include <QAbstractListModel>
#include <QDate>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QList>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <memory>

class AppSettings;

// The library.
//
// Scanning runs on a worker thread. Once Pictures and Videos recurse, a cold
// scan of a large tree is long enough to drop frames if it happens on the GUI
// thread, and the window has to stay live while it works.
//
// A scan result is applied as a diff, never as a reset: rows that vanished are
// removed, rows that changed are updated in place, and new files are inserted
// at the top. So a screenshot taken while omaroll is open slides in without
// the grid losing its scroll position, its selection or the current item.
class CaptureModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)

public:
  explicit CaptureModel(AppSettings* settings, QObject* parent = nullptr);
  ~CaptureModel() override;

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] bool empty() const { return m_records.isEmpty(); }
  [[nodiscard]] bool scanning() const { return m_scanning; }

  // Direct access for the proxy's sort and filter, which would otherwise pay
  // for a QVariant per role per comparison across the whole library.
  [[nodiscard]] const CaptureRecord& recordAt(int row) const { return m_records.at(row); }

  // A directory handed to omaroll on the command line or by "Open with",
  // scanned alongside the usual roots for this session only.
  void setExtraRoot(const QString& directory);

  Q_INVOKABLE void refresh();
  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString dayLabelAt(int row) const;

  // "Today" and "Yesterday" are relative to the day the labels were built.
  // Called at midnight and whenever the window comes back, since a laptop
  // that slept through midnight fires no timer.
  Q_INVOKABLE void checkDayRollover();

  // For Image.source. Concatenating "file://" onto a path breaks on any name
  // with a '#' or '?' in it; this goes through the proper encoder.
  Q_INVOKABLE QUrl fileUrl(const QString& path) const;

  // text/uri-list for a drag: one fully encoded file: URL per line, CRLF
  // terminated as RFC 2483 asks, which is what Nautilus, browsers and Electron
  // apps parse on drop.
  Q_INVOKABLE QString uriList(const QStringList& paths) const;

  // Marks whose files are gone. Paths the scan found are trusted without a
  // stat; the rest are checked on disk, because a root that is unmounted or
  // switched off in settings is not the same as a file that was deleted.
  // Static and pure so the worker can run it and a test can check it.
  [[nodiscard]] static QStringList missingMarks(const QStringList& marks,
                                                const QSet<QString>& livePaths);

signals:
  void countChanged();
  void scanningChanged();
  // "Today" became "Yesterday": every day label is stale at once.
  void dayLabelsChanged();

private:
  struct ScanResult {
    QList<CaptureRecord> records;
    QStringList directories;
    QStringList deadMarks;
  };

  [[nodiscard]] QList<CaptureScanner::Root> roots() const;
  void rewatch(const QStringList& scannedDirectories = {});
  void scheduleRefresh();
  void scheduleMidnight();
  void applyMarks();
  void adoptResults(ScanResult result);

  AppSettings* m_settings = nullptr;
  QStringList m_extraRoots;
  QList<CaptureRecord> m_records;
  QFileSystemWatcher m_watcher;
  QFutureWatcher<ScanResult> m_scanWatcher;
  // Set on teardown so a scan in flight stops walking instead of finishing a
  // large tree nobody will look at.
  std::shared_ptr<std::atomic_bool> m_cancel;
  // omarchy-capture-* writes and the compositor's own churn both arrive as
  // bursts; coalesce so a single screenshot does not trigger several rescans.
  QTimer m_refreshTimer;
  QTimer m_midnightTimer;
  QDate m_labelDate;
  bool m_scanning = false;
  // A scan requested while one is already running, so the newest state is not
  // lost to a rescan that started a moment too early.
  bool m_rescanQueued = false;
};
