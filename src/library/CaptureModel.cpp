#include "library/CaptureModel.h"

#include "app/AppSettings.h"
#include "library/CaptureRoles.h"
#include "sources/CaptureLocations.h"

#include <QDir>
#include <QLocale>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

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

CaptureModel::CaptureModel(AppSettings* settings, QObject* parent)
    : QAbstractListModel(parent), m_settings(settings) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(180);
  connect(&m_refreshTimer, &QTimer::timeout, this, &CaptureModel::refresh);

  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          [this] { scheduleRefresh(); });

  connect(&m_scanWatcher, &QFutureWatcher<QList<CaptureRecord>>::finished, this,
          [this] { adoptResults(m_scanWatcher.result()); });

  if (m_settings) {
    // A source setting change means the roots moved; rescan rather than filter.
    connect(m_settings, &AppSettings::scanDownloadsChanged, this, &CaptureModel::refresh);
    connect(m_settings, &AppSettings::recursionDepthChanged, this, &CaptureModel::refresh);
    // A mark change only repaints flags, so never pay for a rescan.
    connect(m_settings, &AppSettings::marksChanged, this, &CaptureModel::applyMarks);
  }

  rewatch();
  refresh();
}

CaptureModel::~CaptureModel() {
  // The worker holds no reference to this object, but waiting keeps the future
  // from delivering into a destroyed watcher.
  m_scanWatcher.waitForFinished();
}

QList<CaptureScanner::Root> CaptureModel::roots() const {
  const int depth = m_settings ? m_settings->recursionDepth() : 4;

  // Captures land flat where Omarchy writes them, so depth 1 there. The general
  // media roots recurse, because people foreseeably keep photos in subfolders
  // and a library that cannot see them looks broken.
  QList<CaptureScanner::Root> list = {
      {CaptureLocations::screenshots(), 1, CaptureRecord::Picture, CaptureRecord::Video},
      {CaptureLocations::recordings(), 1, CaptureRecord::Picture, CaptureRecord::Video},
      {CaptureLocations::pictures(), depth, CaptureRecord::Picture, CaptureRecord::Video},
      {CaptureLocations::videos(), depth, CaptureRecord::Picture, CaptureRecord::Video},
  };

  if (!m_settings || m_settings->scanDownloads()) {
    // Anything in Downloads is a download whatever its medium, unless a
    // producer name says otherwise.
    list.append(
        {CaptureLocations::downloads(), 1, CaptureRecord::Download, CaptureRecord::Download});
  }
  return list;
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
  case CaptureRoles::FavoriteRole:
    return record.favorite;
  case CaptureRoles::HiddenRole:
    return record.hidden;
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
      {CaptureRoles::FavoriteRole, "favorite"},
      {CaptureRoles::HiddenRole, "hidden"},
      {CaptureRoles::IsDayStartRole, "isDayStart"},
  };
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
  if (m_scanning) {
    // Fold repeat requests into one follow-up scan rather than queueing many.
    m_rescanQueued = true;
    return;
  }

  m_scanning = true;
  emit scanningChanged();

  const QList<CaptureScanner::Root> scanRoots = roots();
  m_scanWatcher.setFuture(
      QtConcurrent::run([scanRoots] { return CaptureScanner::scan(scanRoots); }));
}

void CaptureModel::adoptResults(QList<CaptureRecord> scanned) {
  if (m_settings) {
    QSet<QString> live;
    live.reserve(scanned.size());
    for (CaptureRecord& record : scanned) {
      record.favorite = m_settings->isFavorite(record.path);
      record.hidden = m_settings->isHidden(record.path);
      live.insert(record.path);
    }
    m_settings->pruneMissing(live);
  }

  beginResetModel();
  m_records = std::move(scanned);
  endResetModel();

  m_scanning = false;
  emit scanningChanged();
  emit countChanged();

  rewatch();

  if (m_rescanQueued) {
    m_rescanQueued = false;
    scheduleRefresh();
  }
}

void CaptureModel::applyMarks() {
  if (!m_settings || m_records.isEmpty()) {
    return;
  }

  for (qsizetype row = 0; row < m_records.size(); ++row) {
    CaptureRecord& record = m_records[row];
    const bool favorite = m_settings->isFavorite(record.path);
    const bool hidden = m_settings->isHidden(record.path);
    if (favorite == record.favorite && hidden == record.hidden) {
      continue;
    }
    record.favorite = favorite;
    record.hidden = hidden;
    const QModelIndex changed = index(static_cast<int>(row), 0);
    emit dataChanged(changed, changed, {CaptureRoles::FavoriteRole, CaptureRoles::HiddenRole});
  }
}

void CaptureModel::rewatch() {
  if (!m_watcher.directories().isEmpty()) {
    m_watcher.removePaths(m_watcher.directories());
  }

  // Watch the roots only, not every subdirectory. A deep tree would blow past
  // the inotify watch limit, and a change anywhere under a root still lands as
  // a change to something we are watching often enough to be useful.
  QStringList existing;
  for (const CaptureScanner::Root& root : roots()) {
    if (!existing.contains(root.path) && QDir(root.path).exists()) {
      existing.append(root.path);
    }
  }
  if (!existing.isEmpty()) {
    m_watcher.addPaths(existing);
  }
}

void CaptureModel::scheduleRefresh() { m_refreshTimer.start(); }
