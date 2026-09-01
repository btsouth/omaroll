#pragma once

#include "library/CaptureRecord.h"
#include "sources/CaptureScanner.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QList>
#include <QTimer>

class AppSettings;

// The library, with each row knowing whether it opens a new day.
//
// Scanning runs on a worker thread. Once Pictures and Videos recurse, a cold
// scan of a large tree is long enough to drop frames if it happens on the GUI
// thread, and the window has to stay live while it works.
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

  Q_INVOKABLE void refresh();
  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString dayLabelAt(int row) const;

signals:
  void countChanged();
  void scanningChanged();

private:
  [[nodiscard]] QList<CaptureScanner::Root> roots() const;
  void rewatch();
  void scheduleRefresh();
  void applyMarks();
  void adoptResults(QList<CaptureRecord> scanned);

  AppSettings* m_settings = nullptr;
  QList<CaptureRecord> m_records;
  QFileSystemWatcher m_watcher;
  QFutureWatcher<QList<CaptureRecord>> m_scanWatcher;
  // omarchy-capture-* writes and the compositor's own churn both arrive as
  // bursts; coalesce so a single screenshot does not trigger several rescans.
  QTimer m_refreshTimer;
  bool m_scanning = false;
  // A scan requested while one is already running, so the newest state is not
  // lost to a rescan that started a moment too early.
  bool m_rescanQueued = false;
};
