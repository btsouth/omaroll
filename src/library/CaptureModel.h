#pragma once

#include "library/CaptureRecord.h"
#include "sources/CaptureScanner.h"

#include <QAbstractListModel>
#include <QDate>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <memory>

class AppSettings;

// The library.
//
// Scanning runs on a worker thread. Once Pictures and Videos recurse, a cold
// scan of a large tree is long enough to drop frames if it happens on the GUI
// thread, and the window has to stay live while it works.
//
// Ordinary scan results are applied as a diff: rows that vanished are removed,
// rows that changed are updated in place, and new files are inserted at the
// top. Highly fragmented mass removals use one bounded reset instead of
// thousands of row operations; the grid preserves its place by path across it.
class CaptureModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
  Q_PROPERTY(QVariantList automaticFolders READ automaticFolders
                 NOTIFY automaticFoldersChanged)

public:
  explicit CaptureModel(AppSettings* settings, QObject* parent = nullptr);
  ~CaptureModel() override;

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] bool empty() const { return m_records.isEmpty(); }
  [[nodiscard]] bool scanning() const { return m_scanning; }
  [[nodiscard]] QVariantList automaticFolders() const;
  Q_INVOKABLE [[nodiscard]] bool folderAvailable(const QString& path) const;

  // Direct access for the proxy's sort and filter, which would otherwise pay
  // for a QVariant per role per comparison across the whole library.
  [[nodiscard]] const CaptureRecord& recordAt(int row) const { return m_records.at(row); }

  struct MetadataUpdate {
    QString path;
    qint64 modified = 0;
    qint64 bytes = 0;
    QDateTime captured;
    quint64 device = 0;
    quint64 inode = 0;
    QString camera;
    QString lens;
  };

  // Replaces mtime fallbacks with dates read from embedded media metadata and
  // records the camera and lens. File identity is checked again here because
  // extraction is asynchronous.
  void applyMetadata(const QList<MetadataUpdate>& updates);

  // A directory handed to omaroll on the command line or by "Open with",
  // scanned alongside the usual roots for this session only.
  void setExtraRoot(const QString& directory);
  Q_INVOKABLE void addExtraFiles(const QStringList& paths);

  Q_INVOKABLE void refresh();

  // A tool omaroll launched is still writing this file. Scans leave it out
  // until it is released, so a transcode's zero-byte output never shows up as
  // a broken library entry mid-write.
  void holdPath(const QString& path);
  void releasePath(const QString& path);

  Q_INVOKABLE QString pathAt(int row) const;
  // The source row for a path, -1 when no scan has brought it in yet. The
  // proxy's rowOf answers "visible under the current filters?"; this answers
  // "in the library at all?".
  Q_INVOKABLE int rowOf(const QString& path) const;
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
  void automaticFoldersChanged();
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
  void rebuildRowIndex();

  AppSettings* m_settings = nullptr;
  QStringList m_extraRoots;
  QSet<QString> m_extraFiles;
  QSet<QString> m_heldPaths;
  QList<CaptureRecord> m_records;
  QHash<QString, int> m_rowsByPath;
  bool m_rowIndexValid = true;
  QHash<QString, MetadataUpdate> m_metadata;
  QFileSystemWatcher m_watcher;
  QFutureWatcher<ScanResult> m_scanWatcher;
  // Set on teardown so a scan in flight stops walking instead of finishing a
  // large tree nobody will look at.
  std::shared_ptr<std::atomic_bool> m_cancel;
  // omarchy-capture-* writes and the compositor's own churn both arrive as
  // bursts; coalesce so a single screenshot does not trigger several rescans.
  QTimer m_refreshTimer;
  // Used only when a very broad tree exceeds the bounded inotify watch set.
  // The scan itself stays on the worker thread.
  QTimer m_fallbackRefreshTimer;
  QTimer m_midnightTimer;
  QDate m_labelDate;
  bool m_scanning = false;
  // A scan requested while one is already running, so the newest state is not
  // lost to a rescan that started a moment too early.
  bool m_rescanQueued = false;
};
