#pragma once

#include "library/CaptureRecord.h"
#include "sources/CaptureScanner.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QList>
#include <QTimer>

// The library, newest first, with each row knowing whether it opens a new day.
class CaptureModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
  Q_PROPERTY(bool empty READ empty NOTIFY countChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)

public:
  explicit CaptureModel(QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] bool empty() const { return m_records.isEmpty(); }
  [[nodiscard]] bool scanning() const { return m_scanning; }

  void setRoots(const QList<CaptureScanner::Root>& roots);

  Q_INVOKABLE void refresh();
  Q_INVOKABLE QString pathAt(int row) const;
  Q_INVOKABLE QString dayLabelAt(int row) const;

signals:
  void countChanged();
  void scanningChanged();

private:
  void rewatch();
  void scheduleRefresh();

  QList<CaptureScanner::Root> m_roots;
  QList<CaptureRecord> m_records;
  QFileSystemWatcher m_watcher;
  // omarchy-capture-* writes and the compositor's own churn both arrive as
  // bursts; coalesce so a single screenshot does not trigger several rescans.
  QTimer m_refreshTimer;
  bool m_scanning = false;
};
