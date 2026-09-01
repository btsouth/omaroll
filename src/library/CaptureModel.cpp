#include "library/CaptureModel.h"

#include "library/CaptureRoles.h"

#include <QDir>
#include <QLocale>

namespace {

QString kindLabel(CaptureRecord::Kind kind) {
  switch (kind) {
  case CaptureRecord::Screenshot:
    return QStringLiteral("Screenshot");
  case CaptureRecord::Recording:
    return QStringLiteral("Recording");
  case CaptureRecord::Picture:
    return QStringLiteral("Picture");
  case CaptureRecord::Video:
    return QStringLiteral("Video");
  case CaptureRecord::Download:
    return QStringLiteral("Download");
  }
  return {};
}

// "Today" and "Yesterday" read better than a date for the rows people look at
// most; everything older gets a real date.
QString dayLabel(const QDate& date) {
  const QDate today = QDate::currentDate();
  if (date == today) {
    return QStringLiteral("Today");
  }
  if (date == today.addDays(-1)) {
    return QStringLiteral("Yesterday");
  }
  if (date.year() == today.year()) {
    return QLocale::system().toString(date, QStringLiteral("dddd, d MMMM"));
  }
  return QLocale::system().toString(date, QStringLiteral("d MMMM yyyy"));
}

QString sizeLabel(qint64 bytes) {
  return QLocale::system().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

CaptureModel::CaptureModel(QObject* parent) : QAbstractListModel(parent) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(180);
  connect(&m_refreshTimer, &QTimer::timeout, this, &CaptureModel::refresh);

  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          [this] { scheduleRefresh(); });
}

int CaptureModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(m_records.size());
}

QVariant CaptureModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= m_records.size()) {
    return {};
  }

  const CaptureRecord& record = m_records.at(index.row());

  switch (role) {
  case CaptureRoles::PathRole:
    return record.path;
  case CaptureRoles::FileNameRole:
    return record.fileName;
  case CaptureRoles::KindRole:
    return static_cast<int>(record.kind);
  case CaptureRoles::KindLabelRole:
    return kindLabel(record.kind);
  case CaptureRoles::CapturedRole:
    return record.captured;
  case CaptureRoles::DayKeyRole:
    return record.captured.date().toString(Qt::ISODate);
  case CaptureRoles::DayLabelRole:
    return dayLabel(record.captured.date());
  case CaptureRoles::TimeLabelRole:
    return QLocale::system().toString(record.captured.time(), QLocale::ShortFormat);
  case CaptureRoles::SizeLabelRole:
    return sizeLabel(record.bytes);
  case CaptureRoles::BytesRole:
    return record.bytes;
  case CaptureRoles::IsVideoRole:
    return record.isVideo();
  case CaptureRoles::IsDayStartRole:
    return index.row() == 0 ||
           m_records.at(index.row() - 1).captured.date() != record.captured.date();
  default:
    return {};
  }
}

QHash<int, QByteArray> CaptureModel::roleNames() const {
  return {
      {CaptureRoles::PathRole, "path"},
      {CaptureRoles::FileNameRole, "fileName"},
      {CaptureRoles::KindRole, "kind"},
      {CaptureRoles::KindLabelRole, "kindLabel"},
      {CaptureRoles::CapturedRole, "captured"},
      {CaptureRoles::DayKeyRole, "dayKey"},
      {CaptureRoles::DayLabelRole, "dayLabel"},
      {CaptureRoles::TimeLabelRole, "timeLabel"},
      {CaptureRoles::SizeLabelRole, "sizeLabel"},
      {CaptureRoles::BytesRole, "bytes"},
      {CaptureRoles::IsVideoRole, "isVideo"},
      {CaptureRoles::IsDayStartRole, "isDayStart"},
  };
}

void CaptureModel::setRoots(const QList<CaptureScanner::Root>& roots) {
  m_roots = roots;
  rewatch();
  refresh();
}

QString CaptureModel::pathAt(int row) const {
  if (row < 0 || row >= m_records.size()) {
    return {};
  }
  return m_records.at(row).path;
}

QString CaptureModel::dayLabelAt(int row) const {
  if (row < 0 || row >= m_records.size()) {
    return {};
  }
  return dayLabel(m_records.at(row).captured.date());
}

void CaptureModel::refresh() {
  m_scanning = true;
  emit scanningChanged();

  QList<CaptureRecord> scanned = CaptureScanner::scan(m_roots);

  beginResetModel();
  m_records = std::move(scanned);
  endResetModel();

  m_scanning = false;
  emit scanningChanged();
  emit countChanged();
}

void CaptureModel::rewatch() {
  if (!m_watcher.directories().isEmpty()) {
    m_watcher.removePaths(m_watcher.directories());
  }

  QStringList existing;
  for (const CaptureScanner::Root& root : m_roots) {
    if (QDir(root.path).exists()) {
      existing.append(root.path);
    }
  }
  if (!existing.isEmpty()) {
    m_watcher.addPaths(existing);
  }
}

void CaptureModel::scheduleRefresh() { m_refreshTimer.start(); }
