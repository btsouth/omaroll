#include "library/CaptureModel.h"

#include "app/AppSettings.h"
#include "library/CaptureRoles.h"
#include "sources/CaptureLocations.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

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
  case CaptureRecord::Document:
    return QStringLiteral("PDF document");
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

// A rescan storm at the end of a recording (the processed file moved over the
// original, a preview written and removed) all lands inside this window.
constexpr int kRefreshDebounceMs = 600;
constexpr int kFallbackRefreshMs = 60'000;
// A few removal runs preserve the view more smoothly as a normal model diff.
// Hundreds of interleaved runs make QList shift and proxy updates quadratic,
// as when a large mounted library disappears or half a generated tree is
// removed. One reset is bounded and lets the grid restore its place by path.
constexpr int kMaximumIncrementalRemovalRuns = 256;

} // namespace

CaptureModel::CaptureModel(AppSettings* settings, QObject* parent)
    : QAbstractListModel(parent), m_settings(settings),
      m_cancel(std::make_shared<std::atomic_bool>(false)), m_labelDate(QDate::currentDate()) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(kRefreshDebounceMs);
  connect(&m_refreshTimer, &QTimer::timeout, this, &CaptureModel::refresh);

  m_fallbackRefreshTimer.setInterval(kFallbackRefreshMs);
  connect(&m_fallbackRefreshTimer, &QTimer::timeout, this, &CaptureModel::scheduleRefresh);

  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { scheduleRefresh(); });

  connect(&m_scanWatcher, &QFutureWatcher<ScanResult>::finished, this, [this] {
    if (!m_cancel->load()) {
      adoptResults(m_scanWatcher.future().takeResult());
    }
  });

  if (m_settings) {
    // A source setting change means the roots moved; rescan rather than filter.
    connect(m_settings, &AppSettings::scanDownloadsChanged, this, &CaptureModel::refresh);
    connect(m_settings, &AppSettings::scanDownloadsChanged, this,
            &CaptureModel::automaticFoldersChanged);
    connect(m_settings, &AppSettings::recursionDepthChanged, this, &CaptureModel::refresh);
    connect(m_settings, &AppSettings::libraryFoldersChanged, this, [this] {
      rewatch();
      refresh();
    });
    // A mark change only repaints flags, so never pay for a rescan.
    connect(m_settings, &AppSettings::marksChanged, this, &CaptureModel::applyMarks);
  }

  m_midnightTimer.setSingleShot(true);
  connect(&m_midnightTimer, &QTimer::timeout, this, [this] {
    checkDayRollover();
    scheduleMidnight();
  });
  scheduleMidnight();

  rewatch();
  refresh();
}

CaptureModel::~CaptureModel() {
  // Stop the walk rather than wait for a large tree to finish; the result is
  // discarded either way, and the wait keeps the future from delivering into
  // a destroyed watcher.
  m_cancel->store(true);
  m_scanWatcher.waitForFinished();
}

void CaptureModel::scheduleMidnight() {
  const QDateTime now = QDateTime::currentDateTime();
  const QDateTime midnight = now.date().addDays(1).startOfDay();
  m_midnightTimer.start(static_cast<int>(now.msecsTo(midnight)) + 1000);
}

void CaptureModel::checkDayRollover() {
  const QDate today = QDate::currentDate();
  if (today == m_labelDate) {
    return;
  }
  m_labelDate = today;
  if (!m_records.isEmpty()) {
    emit dataChanged(index(0), index(static_cast<int>(m_records.size()) - 1),
                     {CaptureRoles::DayLabelRole});
  }
  emit dayLabelsChanged();
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

  for (const QString& folder : std::as_const(m_extraRoots)) {
    list.append({folder, depth, CaptureRecord::Picture, CaptureRecord::Video});
  }
  for (const QString& file : std::as_const(m_extraFiles)) {
    list.append({file, 1, CaptureRecord::Picture, CaptureRecord::Video});
  }
  if (m_settings) {
    for (const QString& folder : m_settings->libraryFolders()) {
      list.append({folder, depth, CaptureRecord::Picture, CaptureRecord::Video});
    }
  }
  return list;
}

QVariantList CaptureModel::automaticFolders() const {
  struct Source {
    QString label;
    QString path;
  };
  QList<Source> sources = {
      {QStringLiteral("Screenshots"), CaptureLocations::screenshots()},
      {QStringLiteral("Screen recordings"), CaptureLocations::recordings()},
      {QStringLiteral("Pictures"), CaptureLocations::pictures()},
      {QStringLiteral("Videos"), CaptureLocations::videos()},
  };
  if (!m_settings || m_settings->scanDownloads()) {
    sources.append({QStringLiteral("Downloads"), CaptureLocations::downloads()});
  }

  QVariantList result;
  QHash<QString, int> rowByPath;
  for (const Source& source : std::as_const(sources)) {
    const QFileInfo info(source.path);
    QString path = info.canonicalFilePath();
    if (path.isEmpty()) {
      path = QDir::cleanPath(info.absoluteFilePath());
    }
    const auto found = rowByPath.constFind(path);
    if (found == rowByPath.cend()) {
      QVariantMap row;
      row.insert(QStringLiteral("label"), source.label);
      row.insert(QStringLiteral("path"), path);
      row.insert(QStringLiteral("available"), info.isDir());
      rowByPath.insert(path, static_cast<int>(result.size()));
      result.append(row);
      continue;
    }
    QVariantMap row = result.at(*found).toMap();
    row.insert(QStringLiteral("label"), row.value(QStringLiteral("label")).toString() +
                                            QStringLiteral(" + ") + source.label);
    result[*found] = row;
  }
  return result;
}

bool CaptureModel::folderAvailable(const QString& path) const { return QFileInfo(path).isDir(); }

void CaptureModel::addExtraFiles(const QStringList& paths) {
  const qsizetype before = m_extraFiles.size();
  for (const QString& path : paths) {
    const QFileInfo info(path);
    if (info.isFile() && CaptureScanner::isSupported(info.suffix())) {
      m_extraFiles.insert(info.canonicalFilePath());
    }
  }
  if (m_extraFiles.size() != before) {
    refresh();
  }
}

void CaptureModel::setExtraRoot(const QString& directory) {
  const QString canonical = QFileInfo(directory).canonicalFilePath();
  const QString home = QFileInfo(QDir::homePath()).canonicalFilePath();
  if (canonical.isEmpty() || !QFileInfo(canonical).isDir() || canonical == home ||
      canonical == QDir::rootPath() || m_extraRoots.contains(canonical)) {
    return;
  }
  m_extraRoots.append(canonical);
  rewatch();
  refresh();
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
    return record.isDocument() ? QStringLiteral("PDF document") : kindLabel(record.kind);
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
  case CaptureRoles::StampRole:
    return record.modified;
  case CaptureRoles::IsVideoRole:
    return record.isVideo();
  case CaptureRoles::IsDocumentRole:
    return record.isDocument();
  case CaptureRoles::FavoriteRole:
    return record.favorite;
  case CaptureRoles::HiddenRole:
    return record.hidden;
  case CaptureRoles::CameraRole:
    return record.camera;
  case CaptureRoles::LensRole:
    return record.lens;
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
      {CaptureRoles::StampRole, "stamp"},
      {CaptureRoles::IsVideoRole, "isVideo"},
      {CaptureRoles::IsDocumentRole, "isDocument"},
      {CaptureRoles::FavoriteRole, "favorite"},
      {CaptureRoles::HiddenRole, "hidden"},
      {CaptureRoles::CameraRole, "camera"},
      {CaptureRoles::LensRole, "lens"},
  };
}

QString CaptureModel::pathAt(int row) const {
  if (row < 0 || row >= m_records.size()) {
    return {};
  }
  return m_records.at(row).path;
}

int CaptureModel::rowOf(const QString& path) const {
  if (m_rowIndexValid) {
    return m_rowsByPath.value(path, -1);
  }
  for (int row = 0; row < m_records.size(); ++row) {
    if (m_records.at(row).path == path) {
      return row;
    }
  }
  return -1;
}

void CaptureModel::applyMetadata(const QList<MetadataUpdate>& updates) {
  if (updates.isEmpty() || m_records.isEmpty()) {
    return;
  }

  int firstChanged = m_records.size();
  int lastChanged = -1;
  bool dateChanged = false;
  bool cameraChanged = false;
  for (const MetadataUpdate& update : updates) {
    const int row = rowOf(update.path);
    if (row < 0) {
      continue;
    }
    CaptureRecord& record = m_records[row];
    if (record.modified != update.modified || record.bytes != update.bytes ||
        record.device != update.device || record.inode != update.inode) {
      continue;
    }
    m_metadata.insert(record.path, update);
    bool changed = false;
    if (update.captured.isValid() && !record.hasProducerTimestamp &&
        record.captured != update.captured) {
      record.captured = update.captured;
      dateChanged = true;
      changed = true;
    }
    if (record.camera != update.camera || record.lens != update.lens) {
      record.camera = update.camera;
      record.lens = update.lens;
      cameraChanged = true;
      changed = true;
    }
    if (!changed) {
      continue;
    }
    firstChanged = qMin(firstChanged, row);
    lastChanged = qMax(lastChanged, row);
  }

  if (lastChanged >= 0) {
    QList<int> roles;
    if (dateChanged) {
      roles.append({CaptureRoles::CapturedRole, CaptureRoles::DayKeyRole,
                    CaptureRoles::DayLabelRole, CaptureRoles::TimeLabelRole});
    }
    if (cameraChanged) {
      roles.append({CaptureRoles::CameraRole, CaptureRoles::LensRole});
    }
    emit dataChanged(index(firstChanged), index(lastChanged), roles);
  }
}

QString CaptureModel::dayLabelAt(int row) const {
  if (row < 0 || row >= m_records.size()) {
    return {};
  }
  return dayLabel(m_records.at(row).captured.date());
}

QUrl CaptureModel::fileUrl(const QString& path) const { return QUrl::fromLocalFile(path); }

QString CaptureModel::uriList(const QStringList& paths) const {
  QString list;
  for (const QString& path : paths) {
    list += QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) + QStringLiteral("\r\n");
  }
  return list;
}

QStringList CaptureModel::missingMarks(const QStringList& marks, const QSet<QString>& livePaths) {
  QStringList dead;
  for (const QString& path : marks) {
    if (!livePaths.contains(path) && !QFileInfo::exists(path)) {
      dead.append(path);
    }
  }
  return dead;
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
  const QStringList marks = m_settings ? m_settings->markedPaths() : QStringList();
  const std::shared_ptr<std::atomic_bool> cancel = m_cancel;

  m_scanWatcher.setFuture(QtConcurrent::run([scanRoots, marks, cancel] {
    ScanResult result;
    result.records = CaptureScanner::scan(scanRoots, cancel.get(), &result.directories);
    if (cancel->load()) {
      return result;
    }
    // The existence checks belong here, off the GUI thread: a mark on an
    // unmounted network share can take seconds to answer.
    QSet<QString> live;
    live.reserve(result.records.size());
    for (const CaptureRecord& record : result.records) {
      live.insert(record.path);
    }
    result.deadMarks = missingMarks(marks, live);
    return result;
  }));
}

void CaptureModel::holdPath(const QString& path) {
  m_heldPaths.insert(path);
  scheduleRefresh();
}

void CaptureModel::releasePath(const QString& path) {
  m_heldPaths.remove(path);
  scheduleRefresh();
}

void CaptureModel::adoptResults(ScanResult result) {
  QList<CaptureRecord>& scanned = result.records;
  // Row signals can invoke rowOf while this diff is in progress. Fall back to
  // the live list until every removal and insertion has settled.
  m_rowIndexValid = false;
  if (!m_heldPaths.isEmpty()) {
    scanned.removeIf(
        [this](const CaptureRecord& record) { return m_heldPaths.contains(record.path); });
  }

  if (m_settings) {
    m_settings->forgetMarks(result.deadMarks);
    m_settings->reconcileAlbums(scanned);
    m_settings->reconcileTags(scanned);
    for (CaptureRecord& record : scanned) {
      record.favorite = m_settings->isFavorite(record.path);
      record.hidden = m_settings->isHidden(record.path);
    }
  }

  // Keep already-resolved metadata through ordinary watcher rescans. Without
  // this, every new screenshot would briefly put copied photos back on their
  // mtime dates, and drop their cameras, until the cache was applied again.
  QSet<QString> livePaths;
  livePaths.reserve(scanned.size());
  for (CaptureRecord& record : scanned) {
    const auto known = m_metadata.constFind(record.path);
    if (known == m_metadata.cend()) {
      continue;
    }
    if (known->modified != record.modified || known->bytes != record.bytes ||
        known->device != record.device || known->inode != record.inode) {
      continue;
    }
    if (!record.hasProducerTimestamp && known->captured.isValid()) {
      record.captured = known->captured;
    }
    record.camera = known->camera;
    record.lens = known->lens;
    livePaths.insert(record.path);
  }
  for (auto it = m_metadata.begin(); it != m_metadata.end();) {
    if (livePaths.contains(it.key())) {
      ++it;
    } else {
      it = m_metadata.erase(it);
    }
  }

  QHash<QString, int> incoming;
  incoming.reserve(scanned.size());
  for (int row = 0; row < scanned.size(); ++row) {
    incoming.insert(scanned.at(row).path, row);
  }

  int removalRuns = 0;
  bool insideRemoval = false;
  for (const CaptureRecord& record : std::as_const(m_records)) {
    const bool missing = !incoming.contains(record.path);
    if (missing && !insideRemoval) {
      ++removalRuns;
    }
    insideRemoval = missing;
  }

  if (removalRuns > kMaximumIncrementalRemovalRuns) {
    beginResetModel();
    m_records = std::move(scanned);
    endResetModel();
  } else {
    // Remove what vanished, in runs from the bottom so row numbers stay valid.
    int row = static_cast<int>(m_records.size()) - 1;
    while (row >= 0) {
      if (incoming.contains(m_records.at(row).path)) {
        --row;
        continue;
      }
      int first = row;
      while (first > 0 && !incoming.contains(m_records.at(first - 1).path)) {
        --first;
      }
      beginRemoveRows({}, first, row);
      m_records.remove(first, row - first + 1);
      endRemoveRows();
      row = first - 1;
    }

    // Update what stayed. A rewritten file (a recording finalised in place)
    // changes size and mtime; the mtime is what busts the thumbnail cache.
    QSet<QString> kept;
    kept.reserve(m_records.size());
    for (int existing = 0; existing < m_records.size(); ++existing) {
      CaptureRecord& record = m_records[existing];
      const CaptureRecord& fresh = scanned.at(incoming.value(record.path));
      kept.insert(record.path);
      if (record.modified == fresh.modified && record.bytes == fresh.bytes &&
          record.kind == fresh.kind && record.video == fresh.video &&
          record.document == fresh.document && record.captured == fresh.captured &&
          record.hasProducerTimestamp == fresh.hasProducerTimestamp &&
          record.favorite == fresh.favorite && record.hidden == fresh.hidden) {
        continue;
      }
      record = fresh;
      const QModelIndex changed = index(existing);
      emit dataChanged(changed, changed);
    }

    // Insert what is new at the top. The scan delivers newest first, and the
    // proxy orders the whole library anyway.
    QList<CaptureRecord> added;
    for (const CaptureRecord& record : scanned) {
      if (!kept.contains(record.path)) {
        added.append(record);
      }
    }
    if (!added.isEmpty()) {
      beginInsertRows({}, 0, static_cast<int>(added.size()) - 1);
      added.append(m_records);
      m_records = std::move(added);
      endInsertRows();
    }
  }

  rebuildRowIndex();

  m_scanning = false;
  emit scanningChanged();
  emit countChanged();
  emit automaticFoldersChanged();

  checkDayRollover();
  rewatch(result.directories);

  if (m_rescanQueued) {
    m_rescanQueued = false;
    scheduleRefresh();
  }
}

void CaptureModel::rebuildRowIndex() {
  m_rowsByPath.clear();
  m_rowsByPath.reserve(m_records.size());
  for (int row = 0; row < m_records.size(); ++row) {
    m_rowsByPath.insert(m_records.at(row).path, row);
  }
  m_rowIndexValid = true;
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

void CaptureModel::rewatch(const QStringList& scannedDirectories) {
  // QFileSystemWatcher is not recursive. The worker scan returns every
  // directory it visited, so installing watches here performs no second walk
  // on the GUI thread. Keep a generous cap so a pathological tree cannot
  // consume the user's inotify allowance. A missing root is watched through
  // its nearest existing ancestor so creating or mounting it triggers a scan.
  // Trees beyond the cap get a quiet worker rescan once a minute instead.
  constexpr int kMaximumWatches = 4096;
  QStringList wanted;
  QSet<QString> wantedSet;
  bool incomplete = false;
  const auto addWanted = [&](const QString& path) {
    if (path.isEmpty() || wantedSet.contains(path)) {
      return;
    }
    if (wanted.size() >= kMaximumWatches) {
      incomplete = true;
      return;
    }
    wanted.append(path);
    wantedSet.insert(path);
  };

  for (const CaptureScanner::Root& root : roots()) {
    QDir candidate(root.path);
    while (!candidate.exists() && !candidate.isRoot()) {
      if (!candidate.cdUp()) {
        break;
      }
    }
    if (!candidate.exists()) {
      continue;
    }

    const QString path = candidate.canonicalPath();
    addWanted(path);
  }
  QStringList orderedDirectories = scannedDirectories;
  orderedDirectories.sort(Qt::CaseSensitive);
  for (const QString& directory : std::as_const(orderedDirectories)) {
    addWanted(directory);
  }

  const QStringList watched = m_watcher.directories();
  const QSet<QString> current(watched.cbegin(), watched.cend());
  QStringList removed;
  for (const QString& path : current) {
    if (!wantedSet.contains(path)) {
      removed.append(path);
    }
  }
  if (!removed.isEmpty()) {
    m_watcher.removePaths(removed);
  }

  QStringList added;
  for (const QString& path : std::as_const(wanted)) {
    if (!current.contains(path)) {
      added.append(path);
    }
  }
  if (!added.isEmpty() && !m_watcher.addPaths(added).isEmpty()) {
    incomplete = true;
  }

  if (incomplete) {
    if (!m_fallbackRefreshTimer.isActive()) {
      m_fallbackRefreshTimer.start();
    }
  } else {
    m_fallbackRefreshTimer.stop();
  }
}

void CaptureModel::scheduleRefresh() { m_refreshTimer.start(); }
