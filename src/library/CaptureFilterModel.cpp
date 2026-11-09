#include "library/CaptureFilterModel.h"

#include "app/AppSettings.h"
#include "library/CaptureModel.h"
#include "library/CaptureRoles.h"
#include "library/SectionRows.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

CaptureFilterModel::CaptureFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
  // Qt's C-locale collator ignores numeric mode. Keep natural ordering when
  // launched from a minimal environment as well as a localized desktop.
  if (m_nameCollator.locale().language() == QLocale::C) {
    m_nameCollator.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
  }
  m_nameCollator.setCaseSensitivity(Qt::CaseInsensitive);
  m_nameCollator.setNumericMode(true);
  setDynamicSortFilter(true);
  // Metadata dates can move individual rows without rebuilding the proxy.
  // Scan and mark changes use the full update path for the other sort modes.
  setSortRole(CaptureRoles::CapturedRole);
  sort(0);

  connect(this, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this, &CaptureFilterModel::countChanged);

  m_pairingTimer.setSingleShot(true);
  m_pairingTimer.setInterval(0);
  connect(&m_pairingTimer, &QTimer::timeout, this, [this] {
    beginFilterUpdate();
    endFilterUpdate();
    emit countChanged();
  });

  m_folderIndexTimer.setSingleShot(true);
  m_folderIndexTimer.setInterval(0);
  connect(&m_folderIndexTimer, &QTimer::timeout, this, [this] {
    const bool folderIndexChanged = rebuildFolderIndex();
    const bool datesChanged = rebuildDateIndex();
    const bool camerasChanged = rebuildCameraIndex();
    if (folderIndexChanged) {
      emit foldersChanged();
    }
    if (datesChanged) {
      emit dateBucketsChanged();
    }
    if (camerasChanged) {
      emit this->camerasChanged();
    }
  });
  m_duplicateOrderTimer.setSingleShot(true);
  m_duplicateOrderTimer.setInterval(0);
  connect(&m_duplicateOrderTimer, &QTimer::timeout, this, [this] {
    if (!m_duplicateGroups.isEmpty() || !m_similarGroups.isEmpty()) {
      rebuildDuplicateOrdinals();
      invalidate();
    }
  });

  m_searchTimer.setSingleShot(true);
  m_searchTimer.setInterval(150);
  connect(&m_searchTimer, &QTimer::timeout, this, [this] { setSearchText(m_pendingSearchText); });

  m_releaseKeysTimer.setSingleShot(true);
  m_releaseKeysTimer.setInterval(60'000);
  m_storedScanTimer.setSingleShot(true);
  m_storedScanTimer.setInterval(30);
  connect(&m_storedScanTimer, &QTimer::timeout, this, &CaptureFilterModel::startStoredScan);
  connect(&m_storedWatcher, &QFutureWatcher<StoredScan>::finished, this,
          &CaptureFilterModel::storedScanFinished);

  connect(&m_releaseKeysTimer, &QTimer::timeout, this, [this] {
    std::vector<SearchKeys>().swap(m_searchKeys);
    m_folderKeys.clear();
  });

  m_ocrFilterTimer.setSingleShot(true);
  m_ocrFilterTimer.setInterval(60);
  connect(&m_ocrFilterTimer, &QTimer::timeout, this, [this] {
    beginFilterUpdate();
    endFilterUpdate();
    if (rowCount() > 0) {
      emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {CaptureRoles::OcrSnippetRole});
    }
    emit countChanged();
  });
}

CaptureFilterModel::~CaptureFilterModel() {
  // The scan holds copies of everything it reads; waiting only keeps it from
  // outliving the loader's owner.
  m_storedCancel->store(true);
  m_storedWatcher.waitForFinished();
}

void CaptureFilterModel::setSourceModel(QAbstractItemModel* model) {
  m_metadataIndexesDirty = false;
  m_pairingTimer.stop();
  m_groupedOutPaths.clear();
  m_folderIndexTimer.stop();
  m_duplicateOrderTimer.stop();
  for (const QMetaObject::Connection& connection : std::as_const(m_sourceConnections)) {
    disconnect(connection);
  }
  m_sourceConnections.clear();
  m_searchKeys.clear();
  m_settings = nullptr;

  QSortFilterProxyModel::setSourceModel(model);
  if (!model) {
    m_folders.clear();
    m_folderItemCounts.clear();
    emit foldersChanged();
    m_dateBuckets.clear();
    m_dateDays.clear();
    emit dateBucketsChanged();
    m_cameras.clear();
    m_lenses.clear();
    emit camerasChanged();
    return;
  }

  // Coalesce scan diffs and mark batches. A RAW's filter eligibility changes
  // whether its JPEG is visible, so both rows must be reconsidered together.
  const auto pairingChanged = [this] { m_pairingTimer.start(); };
  m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsInserted, this, pairingChanged));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsRemoved, this, pairingChanged));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this, pairingChanged));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::dataChanged, this,
      [this, pairingChanged](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
        const bool metadataOnly = !roles.isEmpty() && std::all_of(roles.begin(), roles.end(), [](int role) {
          return role == CaptureRoles::CapturedRole || role == CaptureRoles::DayKeyRole ||
                 role == CaptureRoles::DayLabelRole || role == CaptureRoles::TimeLabelRole ||
                 role == CaptureRoles::DateLabelRole ||
                 role == CaptureRoles::CameraRole || role == CaptureRoles::LensRole;
        });
        if (!metadataOnly || !m_cameraFilter.isEmpty() || !m_lensFilter.isEmpty() ||
            m_dateFrom.isValid() || m_dateTo.isValid() || !m_query.isEmpty()) {
          pairingChanged();
        }
      }));
  if (auto* captures = qobject_cast<CaptureModel*>(model)) {
    // Tag and album names are searched, so membership follows the settings.
    m_settings = captures->settings();
    m_sourceConnections.append(connect(captures, &CaptureModel::folderSourcesChanged, this,
                                       &CaptureFilterModel::reloadLibraryRoots));
    if (m_settings) {
      m_sourceConnections.append(connect(m_settings, &AppSettings::tagsChanged, this,
                                         &CaptureFilterModel::reloadOrganization));
      m_sourceConnections.append(connect(m_settings, &AppSettings::albumsChanged, this,
                                         &CaptureFilterModel::reloadOrganization));
    }
    m_sourceConnections.append(connect(captures, &CaptureModel::metadataIndexingChanged, this,
        [this, captures] {
          if (!captures->metadataIndexing() && m_metadataIndexesDirty) {
            m_metadataIndexesDirty = false;
            m_folderIndexTimer.start();
          }
        }));
  }

  // A source insertion can be completely filtered out, in which case the
  // proxy emits no row signal even though sourceCount changed.
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, &CaptureFilterModel::countChanged));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, &CaptureFilterModel::countChanged));
  // A scan diff can emit thousands of row signals. Folder choices and their
  // recursive counts need one rebuild after that batch, not one full source
  // walk per inserted, removed, or changed run.
  const auto rebuildFolders = [this] { m_folderIndexTimer.start(); };
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, rebuildFolders));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, rebuildFolders));
  m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this, rebuildFolders));
  const auto rebuildDuplicateOrder = [this] {
    if (!m_duplicateGroups.isEmpty() || !m_similarGroups.isEmpty()) {
      m_duplicateOrderTimer.start();
    }
  };
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsInserted, this, rebuildDuplicateOrder));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::rowsRemoved, this, rebuildDuplicateOrder));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::modelReset, this, rebuildDuplicateOrder));
  m_sourceConnections.append(
      connect(model, &QAbstractItemModel::dataChanged, this,
              [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                if (roles.isEmpty() || roles.contains(CaptureRoles::PathRole)) {
                  m_folderIndexTimer.start();
                }
                if (roles.isEmpty() || roles.contains(CaptureRoles::CapturedRole) ||
                    roles.contains(CaptureRoles::StampRole) ||
                    roles.contains(CaptureRoles::CameraRole) ||
                    roles.contains(CaptureRoles::LensRole)) {
                  const auto* captures = qobject_cast<CaptureModel*>(sourceModel());
                  if (captures && captures->metadataIndexing()) m_metadataIndexesDirty = true;
                  else m_folderIndexTimer.start();
                }
                if ((!m_duplicateGroups.isEmpty() || !m_similarGroups.isEmpty()) &&
                    (roles.isEmpty() || roles.contains(CaptureRoles::PathRole) ||
                     roles.contains(CaptureRoles::FileNameRole) ||
                     roles.contains(CaptureRoles::CapturedRole) ||
                     roles.contains(CaptureRoles::BytesRole))) {
                  m_duplicateOrderTimer.start();
                }
              }));
  (void)rebuildFolderIndex();
  (void)rebuildDateIndex();
  (void)rebuildCameraIndex();
  reloadOrganization();
  reloadLibraryRoots();
  emit foldersChanged();
  emit dateBucketsChanged();
  emit camerasChanged();
  beginFilterUpdate();
  endFilterUpdate();
}

void CaptureFilterModel::beginFilterUpdate() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  beginFilterChange();
#endif
}

void CaptureFilterModel::endFilterUpdate() {
  rebuildPairing();
  if (manyRowsChange()) {
    // Qt applies a filter change one run of rows at a time and rebuilds its
    // row map for each run. A search widening across a large library flips
    // thousands of scattered runs, which costs far more than one fresh
    // mapping and one layout change. The mapping is built now, while the
    // verdicts above are still current.
    invalidate();
    (void)rowCount();
  } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
  }
  m_acceptedValid = false;
  if (rowCount() > 0) {
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                     {CaptureModel::CompanionPathRole, CaptureRoles::RawFormatRole});
  }
}

bool CaptureFilterModel::manyRowsChange() {
  // Small libraries and small changes keep Qt's row-by-row update, which
  // lets the grid keep its delegates.
  constexpr int kIncrementalRows = 4096;
  constexpr int kIncrementalChanges = 512;
  m_acceptedValid = false;
  if (!sourceModel()) return false;
  const int rows = sourceModel()->rowCount();
  if (rows < kIncrementalRows) return false;
  m_accepted.resize(rows);
  int changed = 0;
  for (int row = 0; row < rows; ++row) {
    const bool accepted = filterAcceptsRow(row, {});
    m_accepted[row] = accepted;
    if (accepted != mapFromSource(sourceModel()->index(row, 0)).isValid()) ++changed;
  }
  m_acceptedValid = true;
  return changed > kIncrementalChanges;
}

void CaptureFilterModel::rebuildPairing() {
  m_groupedOutPaths.clear();
  if (!m_pairRawJpeg || m_duplicatesOnly || m_similarOnly || !sourceModel()) return;
  // Two source passes per filter/scan batch, then constant-time row decisions.
  QSet<QString> eligible;
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    const CaptureRecord& record = sourceRecord(row);
    if (!record.companionPath.isEmpty() && matchesFilters(row, record)) eligible.insert(record.path);
  }
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    const CaptureRecord& record = sourceRecord(row);
    if (!record.isRaw() || record.companionPath.isEmpty() || !eligible.contains(record.path) ||
        !eligible.contains(record.companionPath)) continue;
    const bool explicitRaw = m_explicitPaths.contains(record.path);
    const bool explicitJpeg = m_explicitPaths.contains(record.companionPath);
    if (explicitRaw && explicitJpeg) continue;
    if (explicitJpeg) {
      m_groupedOutPaths.insert(record.path);
    } else {
      m_groupedOutPaths.insert(record.companionPath);
    }
  }
}

void CaptureFilterModel::setExplicitPaths(const QStringList& paths) {
  const QSet<QString> explicitPaths(paths.begin(), paths.end());
  if (explicitPaths == m_explicitPaths) return;
  beginFilterUpdate();
  m_explicitPaths = explicitPaths;
  endFilterUpdate();
  emit countChanged();
}

void CaptureFilterModel::setPairRawJpeg(bool value) {
  if (m_pairRawJpeg == value) return;
  beginFilterUpdate();
  m_pairRawJpeg = value;
  endFilterUpdate();
  emit pairRawJpegChanged();
  emit countChanged();
}

void CaptureFilterModel::setKindFilter(int kind) {
  if (m_kindFilter == kind) {
    return;
  }
  beginFilterUpdate();
  m_kindFilter = kind;
  endFilterUpdate();
  clearSmartCollection();
  emit kindFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setSortMode(int mode) {
  if (m_sortMode == mode) {
    return;
  }
  m_sortMode = mode;
  m_duplicateOrderTimer.stop();
  rebuildDuplicateOrdinals();
  invalidate();
  clearSmartCollection();
  emit sortModeChanged();
}

void CaptureFilterModel::queueSearchText(const QString& text) {
  if (text.isEmpty() || text == m_searchText) {
    setSearchText(text);
    return;
  }
  m_pendingSearchText = text;
  m_searchTimer.start();
}

void CaptureFilterModel::commitPendingSearch() {
  if (m_searchTimer.isActive()) setSearchText(m_pendingSearchText);
}

void CaptureFilterModel::setSearchText(const QString& text) {
  const bool pending = m_searchTimer.isActive();
  m_searchTimer.stop();
  if (m_searchText == text) {
    // An explicit reset also cancels uncommitted typing and restores the field.
    if (pending) emit searchTextChanged();
    return;
  }
  beginFilterUpdate();
  applySearchText(text);
  endFilterUpdate();
  clearSmartCollection();
  if (rowCount() > 0) {
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {CaptureRoles::OcrSnippetRole});
  }
  emit searchTextChanged();
  emit countChanged();
}

void CaptureFilterModel::setFavoritesOnly(bool value) {
  if (m_favoritesOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_favoritesOnly = value;
  endFilterUpdate();
  clearSmartCollection();
  emit favoritesOnlyChanged();
  emit countChanged();
}

void CaptureFilterModel::setFolderFilter(const QString& folder) {
  const QString normalized = folder.isEmpty() ? QString() : QDir::cleanPath(folder);
  if (m_folderFilter == normalized) {
    return;
  }
  beginFilterUpdate();
  m_folderFilter = normalized;
  endFilterUpdate();
  clearSmartCollection();
  emit folderFilterChanged();
  emit countChanged();
}

QStringList CaptureFilterModel::folders() const { return m_folders; }

int CaptureFilterModel::folderItemCount(const QString& folder) const {
  return m_folderItemCounts.value(QDir::cleanPath(folder));
}

bool CaptureFilterModel::rebuildFolderIndex() {
  QSet<QString> unique;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QString path =
          sourceModel()->data(sourceModel()->index(row, 0), CaptureRoles::PathRole).toString();
      if (!path.isEmpty()) {
        unique.insert(QFileInfo(path).absolutePath());
      }
    }
  }
  QStringList folders(unique.begin(), unique.end());
  std::sort(folders.begin(), folders.end(), [](const QString& left, const QString& right) {
    const int folded = QString::compare(left, right, Qt::CaseInsensitive);
    return folded != 0 ? folded < 0 : left < right;
  });

  QHash<QString, int> counts;
  counts.reserve(unique.size());
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QString path =
          sourceModel()->data(sourceModel()->index(row, 0), CaptureRoles::PathRole).toString();
      QString directory = QFileInfo(path).absolutePath();
      while (!directory.isEmpty()) {
        // Source roots can contain media only in descendants and therefore
        // are not necessarily in the visible folder list. Keep ancestor
        // counts too so source chips and empty-state diagnostics stay honest.
        ++counts[directory];
        const QString parent = QFileInfo(directory).path();
        if (parent == directory || parent == QStringLiteral(".")) {
          break;
        }
        directory = parent;
      }
    }
  }

  if (m_folders == folders && m_folderItemCounts == counts) {
    return false;
  }
  m_folders = std::move(folders);
  m_folderItemCounts = std::move(counts);
  return true;
}

void CaptureFilterModel::setAlbumFilter(const QString& name, const QStringList& paths) {
  const QSet<QString> next(paths.begin(), paths.end());
  if (m_albumFilter == name && m_albumPaths == next) {
    return;
  }
  // Files joining or leaving the album keep a saved view open on it; only
  // another album leaves the view.
  const bool changedName = m_albumFilter != name;
  beginFilterUpdate();
  m_albumFilter = name;
  m_albumPaths = next;
  endFilterUpdate();
  if (changedName) {
    clearSmartCollection();
  }
  emit albumFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setTagFilter(const QString& name, const QStringList& paths) {
  const QSet<QString> next(paths.begin(), paths.end());
  if (m_tagFilter == name && m_tagPaths == next) {
    return;
  }
  const bool changedName = m_tagFilter != name;
  beginFilterUpdate();
  m_tagFilter = name;
  m_tagPaths = next;
  endFilterUpdate();
  if (changedName) {
    clearSmartCollection();
  }
  emit tagFilterChanged();
  emit countChanged();
}

namespace {
QDate isoDate(const QString& value) {
  return value.isEmpty() ? QDate() : QDate::fromString(value, Qt::ISODate);
}
} // namespace

void CaptureFilterModel::setDateFrom(const QString& value) {
  const QDate parsed = isoDate(value);
  if (parsed == m_dateFrom && m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateFrom = parsed;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setDateTo(const QString& value) {
  const QDate parsed = isoDate(value);
  if (parsed == m_dateTo && m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateTo = parsed;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setDateField(int value) {
  const int bounded = qBound(0, value, 1);
  if (bounded == m_dateField && m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateField = bounded;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setDateRange(const QString& from, const QString& to, int field) {
  const QDate nextFrom = isoDate(from);
  const QDate nextTo = isoDate(to);
  const int nextField = qBound(0, field, 1);
  if (nextFrom == m_dateFrom && nextTo == m_dateTo && nextField == m_dateField &&
      m_modifiedAfter.isEmpty()) {
    return;
  }
  beginFilterUpdate();
  m_dateFrom = nextFrom;
  m_dateTo = nextTo;
  m_dateField = nextField;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::setModifiedAfter(const QString& value) {
  const QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
  const QString normalized = parsed.isValid() ? parsed.toString(Qt::ISODate) : QString();
  if (normalized == m_modifiedAfter && !m_dateFrom.isValid() && !m_dateTo.isValid()) {
    return;
  }
  beginFilterUpdate();
  m_dateFrom = {};
  m_dateTo = {};
  m_dateField = parsed.isValid() ? 1 : 0;
  m_modifiedAfter = normalized;
  m_modifiedAfterMs = parsed.isValid() ? parsed.toMSecsSinceEpoch() : 0;
  endFilterUpdate();
  clearSmartCollection();
  emit dateRangeChanged();
  emit countChanged();
}

void CaptureFilterModel::clearDateRange() {
  if (!m_modifiedAfter.isEmpty()) {
    setModifiedAfter({});
  } else {
    setDateRange({}, {}, 0);
  }
}

bool CaptureFilterModel::rebuildDateIndex() {
  QMap<QDate, int> dayCounts;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const QDate date = sourceRecord(row).captured.date();
      if (date.isValid()) {
        ++dayCounts[date];
      }
    }
  }

  QMap<QString, int> monthCounts;
  QHash<QString, QVariantList> daysByMonth;
  QList<QDate> dates = dayCounts.keys();
  std::reverse(dates.begin(), dates.end());
  for (const QDate& date : std::as_const(dates)) {
    const QString monthKey = date.toString(QStringLiteral("yyyy-MM"));
    monthCounts[monthKey] += dayCounts.value(date);
    QVariantMap row;
    row.insert(QStringLiteral("date"), date.toString(Qt::ISODate));
    row.insert(QStringLiteral("label"),
               QLocale::system().toString(date, QStringLiteral("dddd, d MMMM")));
    row.insert(QStringLiteral("count"), dayCounts.value(date));
    daysByMonth[monthKey].append(row);
  }

  QVariantList buckets;
  QStringList monthKeys = monthCounts.keys();
  std::reverse(monthKeys.begin(), monthKeys.end());
  for (const QString& key : std::as_const(monthKeys)) {
    const QDate start = QDate::fromString(key + QStringLiteral("-01"), Qt::ISODate);
    QVariantMap row;
    row.insert(QStringLiteral("key"), key);
    row.insert(QStringLiteral("label"),
               QLocale::system().toString(start, QStringLiteral("MMMM yyyy")));
    row.insert(QStringLiteral("count"), monthCounts.value(key));
    row.insert(QStringLiteral("from"), start.toString(Qt::ISODate));
    row.insert(QStringLiteral("to"), start.addMonths(1).addDays(-1).toString(Qt::ISODate));
    buckets.append(row);
  }
  if (buckets == m_dateBuckets && daysByMonth == m_dateDays) {
    return false;
  }
  m_dateBuckets = std::move(buckets);
  m_dateDays = std::move(daysByMonth);
  return true;
}

QVariantList CaptureFilterModel::dateDays(const QString& monthKey) const {
  return m_dateDays.value(monthKey);
}

namespace {

QVariantList countedChoices(const QHash<QString, int>& counts) {
  QStringList names = counts.keys();
  std::sort(names.begin(), names.end(), [&counts](const QString& a, const QString& b) {
    if (counts.value(a) != counts.value(b)) {
      return counts.value(a) > counts.value(b);
    }
    return a.compare(b, Qt::CaseInsensitive) < 0;
  });
  QVariantList rows;
  rows.reserve(names.size());
  for (const QString& name : std::as_const(names)) {
    QVariantMap row;
    row.insert(QStringLiteral("name"), name);
    row.insert(QStringLiteral("count"), counts.value(name));
    rows.append(row);
  }
  return rows;
}

} // namespace

bool CaptureFilterModel::rebuildCameraIndex() {
  QHash<QString, int> cameraCounts;
  QHash<QString, int> lensCounts;
  if (sourceModel()) {
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      const CaptureRecord& record = sourceRecord(row);
      if (!record.camera.isEmpty()) {
        ++cameraCounts[record.camera];
      }
      if (!record.lens.isEmpty()) {
        ++lensCounts[record.lens];
      }
    }
  }
  QVariantList cameras = countedChoices(cameraCounts);
  QVariantList lenses = countedChoices(lensCounts);
  if (cameras == m_cameras && lenses == m_lenses) {
    return false;
  }
  m_cameras = std::move(cameras);
  m_lenses = std::move(lenses);
  return true;
}

void CaptureFilterModel::setCameraFilter(const QString& camera) {
  if (m_cameraFilter == camera) {
    return;
  }
  beginFilterUpdate();
  m_cameraFilter = camera;
  endFilterUpdate();
  clearSmartCollection();
  emit cameraFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setLensFilter(const QString& lens) {
  if (m_lensFilter == lens) {
    return;
  }
  beginFilterUpdate();
  m_lensFilter = lens;
  endFilterUpdate();
  clearSmartCollection();
  emit lensFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::setMinimumRating(int stars) {
  const int bounded = qBound(0, stars, 5);
  if (m_minimumRating == bounded) {
    return;
  }
  beginFilterUpdate();
  m_minimumRating = bounded;
  endFilterUpdate();
  clearSmartCollection();
  emit minimumRatingChanged();
  emit countChanged();
}

QVariantMap CaptureFilterModel::currentView() {
  commitPendingSearch();
  QVariantMap view;
  // "search" stays plain words, which is all 1.18 and older understand;
  // the query with its field tokens has a key of its own.
  view.insert(QStringLiteral("search"), m_query.plainWords());
  view.insert(QStringLiteral("query"), m_searchText);
  view.insert(QStringLiteral("kind"), m_kindFilter);
  view.insert(QStringLiteral("folder"), m_folderFilter);
  view.insert(QStringLiteral("favorites"), m_favoritesOnly);
  view.insert(QStringLiteral("showHidden"), m_showHidden);
  view.insert(QStringLiteral("dateFrom"), dateFrom());
  view.insert(QStringLiteral("dateTo"), dateTo());
  view.insert(QStringLiteral("dateField"), m_dateField);
  view.insert(QStringLiteral("modifiedAfter"), m_modifiedAfter);
  view.insert(QStringLiteral("tag"), m_tagFilter);
  view.insert(QStringLiteral("album"), m_albumFilter);
  view.insert(QStringLiteral("camera"), m_cameraFilter);
  view.insert(QStringLiteral("lens"), m_lensFilter);
  view.insert(QStringLiteral("minRating"), m_minimumRating);
  view.insert(QStringLiteral("sort"), m_sortMode);
  return view;
}

void CaptureFilterModel::applyView(const QString& name, const QVariantMap& view,
                                   const QStringList& tagPaths) {
  beginFilterUpdate();
  // A view saved before field tokens has only plain words, which keep their
  // old meaning: "-draft" there is text, not an exclusion.
  applySearchText(view.contains(QStringLiteral("query"))
                      ? view.value(QStringLiteral("query")).toString()
                      : SearchQuery::fromPlainWords(view.value(QStringLiteral("search")).toString()));
  m_kindFilter = qBound(kAllKinds, view.value(QStringLiteral("kind"), kAllKinds).toInt(),
                        static_cast<int>(CaptureRecord::Document));
  m_folderFilter = view.value(QStringLiteral("folder")).toString();
  m_favoritesOnly = view.value(QStringLiteral("favorites")).toBool();
  m_showHidden = view.value(QStringLiteral("showHidden")).toBool();
  m_dateFrom = isoDate(view.value(QStringLiteral("dateFrom")).toString());
  m_dateTo = isoDate(view.value(QStringLiteral("dateTo")).toString());
  m_dateField = qBound(0, view.value(QStringLiteral("dateField")).toInt(), 1);
  m_modifiedAfter = view.value(QStringLiteral("modifiedAfter")).toString();
  m_modifiedAfterMs = QDateTime::fromString(m_modifiedAfter, Qt::ISODate).toMSecsSinceEpoch();
  m_tagFilter = view.value(QStringLiteral("tag")).toString();
  m_tagPaths = QSet<QString>(tagPaths.begin(), tagPaths.end());
  m_cameraFilter = view.value(QStringLiteral("camera")).toString();
  m_lensFilter = view.value(QStringLiteral("lens")).toString();
  m_minimumRating = qBound(0, view.value(QStringLiteral("minRating")).toInt(), 5);
  m_sortMode = qBound(0, view.value(QStringLiteral("sort"), NewestFirst).toInt(),
                      static_cast<int>(RatingFirst));
  // Facets combine, so a saved view keeps its album as well. Membership is
  // read now rather than stored, so the view follows later additions.
  m_albumFilter = view.value(QStringLiteral("album")).toString();
  m_albumPaths.clear();
  if (!m_albumFilter.isEmpty() && m_settings) {
    const QStringList albumPaths = m_settings->albumPaths(m_albumFilter);
    m_albumPaths = QSet<QString>(albumPaths.begin(), albumPaths.end());
  }
  if (!m_tagFilter.isEmpty() && tagPaths.isEmpty() && m_settings) {
    const QStringList paths = m_settings->tagPaths(m_tagFilter);
    m_tagPaths = QSet<QString>(paths.begin(), paths.end());
  }
  m_duplicatesOnly = false;
  m_similarOnly = false;
  m_smartCollectionFilter = name;
  endFilterUpdate();
  invalidate();
  emit searchTextChanged();
  emit kindFilterChanged();
  emit folderFilterChanged();
  emit favoritesOnlyChanged();
  emit showHiddenChanged();
  emit dateRangeChanged();
  emit tagFilterChanged();
  emit cameraFilterChanged();
  emit lensFilterChanged();
  emit minimumRatingChanged();
  emit sortModeChanged();
  emit albumFilterChanged();
  emit duplicatesOnlyChanged();
  emit similarOnlyChanged();
  emit smartCollectionFilterChanged();
  emit countChanged();
}

void CaptureFilterModel::clearSmartCollection() {
  if (m_smartCollectionFilter.isEmpty()) {
    return;
  }
  m_smartCollectionFilter.clear();
  emit smartCollectionFilterChanged();
}

void CaptureFilterModel::setShowHidden(bool value) {
  if (m_showHidden == value) {
    return;
  }
  beginFilterUpdate();
  m_showHidden = value;
  endFilterUpdate();
  clearSmartCollection();
  emit showHiddenChanged();
  emit countChanged();
}

void CaptureFilterModel::setDuplicatesOnly(bool value) {
  if (m_duplicatesOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_duplicatesOnly = value;
  if (value) {
    m_similarOnly = false;
  }
  endFilterUpdate();
  rebuildDuplicateOrdinals();
  invalidate();
  clearSmartCollection();
  emit duplicatesOnlyChanged();
  if (value) {
    emit similarOnlyChanged();
  }
  emit countChanged();
}

void CaptureFilterModel::setSimilarOnly(bool value) {
  if (m_similarOnly == value) {
    return;
  }
  beginFilterUpdate();
  m_similarOnly = value;
  if (value) {
    m_duplicatesOnly = false;
  }
  endFilterUpdate();
  rebuildDuplicateOrdinals();
  invalidate();
  clearSmartCollection();
  emit similarOnlyChanged();
  if (value) {
    emit duplicatesOnlyChanged();
  }
  emit countChanged();
}

void CaptureFilterModel::setDuplicateGroups(const QHash<QString, QString>& groups) {
  if (m_duplicateGroups == groups) {
    return;
  }

  beginFilterUpdate();
  m_duplicateGroups = groups;
  m_duplicateOrderTimer.stop();
  rebuildDuplicateOrdinals();
  endFilterUpdate();
  invalidate();
  emit countChanged();
}

void CaptureFilterModel::setSimilarGroups(const QHash<QString, QString>& groups) {
  if (m_similarGroups == groups) {
    return;
  }
  beginFilterUpdate();
  m_similarGroups = groups;
  rebuildDuplicateOrdinals();
  endFilterUpdate();
  invalidate();
  emit countChanged();
}

QString CaptureFilterModel::pathAt(int row) const {
  return data(index(row, 0), CaptureRoles::PathRole).toString();
}

QString CaptureFilterModel::deletionPathAt(int row) const {
  const auto source = mapToSource(index(row, 0));
  return source.isValid() ? static_cast<const CaptureModel*>(sourceModel())->deletionPathAt(source.row())
                          : QString();
}

QString CaptureFilterModel::companionPathAt(int row) const {
  return data(index(row, 0), CaptureModel::CompanionPathRole).toString();
}

QString CaptureFilterModel::fileNameAt(int row) const {
  return data(index(row, 0), CaptureRoles::FileNameRole).toString();
}

QString CaptureFilterModel::dayLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::DayLabelRole).toString();
}

QString CaptureFilterModel::gridLabelAt(int row) const {
  if (!m_duplicatesOnly && !m_similarOnly) {
    return dayLabelAt(row);
  }
  const QHash<QString, QString>& groups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
  const QString group = groups.value(pathAt(row));
  const int ordinal = m_duplicateOrdinals.value(group);
  if (ordinal <= 0) {
    return {};
  }
  return QStringLiteral("%1 %2 of %3")
      .arg(m_similarOnly ? QStringLiteral("Similar set") : QStringLiteral("Exact match"))
      .arg(ordinal)
      .arg(m_duplicateOrdinals.size());
}

QVariantMap CaptureFilterModel::tileAt(int row) const {
  QVariantMap tile;
  const QModelIndex at = index(row, 0);
  if (!at.isValid()) {
    return tile;
  }
  const QHash<int, QByteArray> names = roleNames();
  for (auto it = names.cbegin(); it != names.cend(); ++it) {
    tile.insert(QString::fromUtf8(it.value()), data(at, it.key()));
  }
  return tile;
}

QVariantList CaptureFilterModel::reviewSections() const {
  QVariantList sections;
  if (!m_duplicatesOnly && !m_similarOnly) {
    return sections;
  }
  const QHash<QString, QString>& groups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
  const QString noun = m_similarOnly ? QStringLiteral("Similar set") : QStringLiteral("Exact match");
  QString current;
  int first = 0;
  const auto close = [&](int end) {
    if (end > first) {
      sections.append(QVariantMap{
          {QStringLiteral("first"), first},
          {QStringLiteral("count"), end - first},
          {QStringLiteral("label"),
           QStringLiteral("%1 %2").arg(noun).arg(sections.size() + 1)}});
    }
  };
  const int rows = rowCount();
  for (int row = 0; row < rows; ++row) {
    const QString group = groups.value(pathAt(row));
    if (row == 0 || group != current) {
      close(row);
      current = group;
      first = row;
    }
  }
  close(rows);
  return sections;
}

QObject* CaptureFilterModel::sectionRows() {
  if (!m_sectionRows) {
    m_sectionRows = new SectionRows(this);
  }
  return m_sectionRows;
}

QString CaptureFilterModel::timeLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::TimeLabelRole).toString();
}

QString CaptureFilterModel::sizeLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::SizeLabelRole).toString();
}

QString CaptureFilterModel::kindLabelAt(int row) const {
  return data(index(row, 0), CaptureRoles::KindLabelRole).toString();
}

int CaptureFilterModel::kindAt(int row) const {
  return data(index(row, 0), CaptureRoles::KindRole).toInt();
}

bool CaptureFilterModel::isVideoAt(int row) const {
  return data(index(row, 0), CaptureRoles::IsVideoRole).toBool();
}

bool CaptureFilterModel::isDocumentAt(int row) const {
  return data(index(row, 0), CaptureRoles::IsDocumentRole).toBool();
}

bool CaptureFilterModel::isAnimatedAt(int row) const {
  const QModelIndex source = mapToSource(index(row, 0));
  return source.isValid() && sourceRecord(source.row()).animated;
}

QString CaptureFilterModel::thumbnailVersionAt(int row) const {
  return row >= 0 && row < rowCount()
             ? data(index(row, 0), CaptureRoles::ThumbnailVersionRole).toString() : QString();
}

qint64 CaptureFilterModel::stampAt(int row) const {
  return data(index(row, 0), CaptureRoles::StampRole).toLongLong();
}

QString CaptureFilterModel::ocrSnippetAt(int row) const {
  return data(index(row, 0), CaptureRoles::OcrSnippetRole).toString();
}

QVariant CaptureFilterModel::data(const QModelIndex& proxyIndex, int role) const {
  if (role == CaptureRoles::RawFormatRole && proxyIndex.isValid()) {
    const CaptureRecord& record = sourceRecord(mapToSource(proxyIndex).row());
    if (!record.companionPath.isEmpty() && m_groupedOutPaths.contains(record.companionPath)) {
      return QStringLiteral("RAW+JPG");
    }
  }
  if (role == CaptureModel::CompanionPathRole &&
      (!m_pairRawJpeg || m_duplicatesOnly || m_similarOnly)) return QString();
  if (role == CaptureRoles::OcrSnippetRole) {
    if (!proxyIndex.isValid()) {
      return {};
    }
    return ocrSnippet(sourceRecord(mapToSource(proxyIndex).row()));
  }
  return QSortFilterProxyModel::data(proxyIndex, role);
}

QHash<int, QByteArray> CaptureFilterModel::roleNames() const {
  QHash<int, QByteArray> roles = QSortFilterProxyModel::roleNames();
  roles.insert(CaptureRoles::OcrSnippetRole, QByteArrayLiteral("ocrSnippet"));
  return roles;
}

void CaptureFilterModel::setOcrText(const QString& path, const QString& text) {
  if (path.isEmpty()) {
    return;
  }
  if (text.isEmpty()) {
    if (m_text.remove(path) == 0) {
      return;
    }
  } else {
    // Kept as UTF-8, the original for snippets and the folded form for
    // matching: matching scans half the bytes UTF-16 would.
    const auto found = m_text.constFind(path);
    if (found != m_text.cend() && found->original == text.toUtf8()) {
      return;
    }
    m_text.insert(path, SearchText::FoldedText::make(text));
  }
  ++m_textVersion;
  if (m_query.hasText()) {
    m_ocrFilterTimer.start();
  }
}

void CaptureFilterModel::setDocumentText(const QString& path, const QString& text,
                                         bool persisted) {
  if (path.isEmpty()) {
    return;
  }
  if (text.isEmpty()) {
    if (!m_documentText.contains(path) && !m_storedDocuments.contains(path)) {
      return;
    }
    dropDocumentText(path);
  } else {
    const auto found = m_documentText.find(path);
    if (found != m_documentText.end() && found->text.original == text.toUtf8()) {
      found->persisted = found->persisted || persisted;
      return;
    }
    dropDocumentText(path);
    ResidentText entry;
    entry.text = SearchText::FoldedText::make(text);
    entry.bytes = entry.text.bytes() +
                  entry.text.marks.size() * qsizetype(sizeof(std::pair<qsizetype, qsizetype>));
    entry.persisted = persisted;
    entry.use = m_documentUse.insert(m_documentUse.end(), path);
    m_documentTextBytes += entry.bytes;
    m_documentText.insert(path, std::move(entry));
    trimDocumentText();
  }
  ++m_textVersion;
  if (m_query.hasText()) {
    m_ocrFilterTimer.start();
  }
}

void CaptureFilterModel::setDocumentTextLoader(TextLoader loader) {
  m_documentTextLoader = std::move(loader);
  trimDocumentText();
}

void CaptureFilterModel::setDocumentTextBudget(qsizetype bytes) {
  m_documentTextBudget = std::max<qsizetype>(0, bytes);
  trimDocumentText();
}

bool CaptureFilterModel::matchingStoredText() const {
  return m_storedScanning || !m_storedPending.isEmpty() || m_storedScanTimer.isActive();
}

void CaptureFilterModel::dropDocumentText(const QString& path) {
  const auto found = m_documentText.find(path);
  if (found != m_documentText.end()) {
    m_documentTextBytes -= found->bytes;
    m_documentUse.erase(found->use);
    m_documentText.erase(found);
  }
  if (m_storedDocuments.remove(path) > 0) {
    m_storedPending.remove(path);
    m_storedMatches.remove(path);
  }
  releaseKeyText(path);
}

// Least recently shown first. Text that is not in the disk cache stays, so
// nothing becomes unsearchable.
void CaptureFilterModel::trimDocumentText() {
  if (!m_documentTextLoader) {
    return;
  }
  for (auto it = m_documentUse.begin();
       it != m_documentUse.end() && m_documentTextBytes > m_documentTextBudget;) {
    const QString path = *it;
    ++it;
    const auto found = m_documentText.find(path);
    if (found == m_documentText.end() || !found->persisted) {
      continue;
    }
    m_documentTextBytes -= found->bytes;
    m_documentUse.erase(found->use);
    m_documentText.erase(found);
    m_storedDocuments.insert(path, ++m_storedSerial);
    releaseKeyText(path);
    if (!m_storedTerms.isEmpty()) {
      m_storedPending.insert(path);
      m_storedScanTimer.start();
    }
  }
}

// The folded copy a row's search keys share would keep dropped text alive
// until the next filter pass.
void CaptureFilterModel::releaseKeyText(const QString& path) {
  const auto* source = qobject_cast<const CaptureModel*>(sourceModel());
  if (!source) {
    return;
  }
  const int row = source->rowOf(path);
  if (row >= 0 && row < static_cast<int>(m_searchKeys.size()) && m_searchKeys[row].path == path) {
    m_searchKeys[row].ocr = QByteArray();
    m_searchKeys[row].text = 0;
  }
}

// Called whenever the query changes; the stored matches answer the text
// terms, so only a change to those starts the documents on disk over.
void CaptureFilterModel::resetStoredMatches() {
  QList<QByteArray> terms;
  for (const SearchQuery::Term& term : std::as_const(m_query.terms)) {
    if (term.field == SearchQuery::Text) terms.append(term.utf8);
  }
  if (terms == m_storedTerms) {
    return;
  }
  m_storedTerms = std::move(terms);
  m_storedSnippetTerms = m_searchTerms;
  m_storedMatches.clear();
  m_storedPending.clear();
  ++m_storedGeneration;
  // A scan for the old words stops at its next document.
  m_storedCancel->store(true);
  m_storedCancel = std::make_shared<std::atomic_bool>(false);
  if (m_storedTerms.isEmpty()) {
    m_storedScanTimer.stop();
    return;
  }
  for (auto it = m_storedDocuments.cbegin(); it != m_storedDocuments.cend(); ++it) {
    m_storedPending.insert(it.key());
  }
  if (!m_storedPending.isEmpty()) {
    m_storedScanTimer.start();
  }
}

void CaptureFilterModel::startStoredScan() {
  if (m_storedScanning || m_storedPending.isEmpty() || m_storedTerms.isEmpty() ||
      !m_documentTextLoader) {
    return;
  }
  const auto* source = qobject_cast<const CaptureModel*>(sourceModel());
  QList<StoredJob> jobs;
  jobs.reserve(m_storedPending.size());
  for (const QString& path : std::as_const(m_storedPending)) {
    const int row = source ? source->rowOf(path) : -1;
    const QString name = row >= 0 ? source->recordAt(row).fileName : QFileInfo(path).fileName();
    jobs.append({path, name, m_storedDocuments.value(path)});
  }
  m_storedPending.clear();
  m_storedScanning = true;
  m_storedWatcher.setFuture(QtConcurrent::run(
      [jobs = std::move(jobs), terms = m_storedTerms, snippets = m_storedSnippetTerms,
       loader = m_documentTextLoader, generation = m_storedGeneration, cancel = m_storedCancel] {
        return scanStored(jobs, terms, snippets, loader, generation, *cancel);
      }));
}

CaptureFilterModel::StoredScan CaptureFilterModel::scanStored(
    const QList<StoredJob>& jobs, const QList<QByteArray>& terms, const QStringList& snippetTerms,
    const TextLoader& loader, quint64 generation, const std::atomic_bool& cancel) {
  StoredScan scan;
  scan.generation = generation;
  for (const StoredJob& job : jobs) {
    if (cancel.load()) break;
    const QString text = loader(job.path);
    if (text.isNull()) {
      scan.lost.append({job.path, job.serial});
      continue;
    }
    // Folded the same way as text kept in memory, so both match alike. Most
    // documents do not match, so the snippet's offsets are built only for
    // those that do.
    const QByteArray folded = SearchText::fold(text).toUtf8();
    StoredMatch match;
    for (int index = 0; index < terms.size() && index < 64; ++index) {
      if (folded.contains(terms.at(index))) match.terms |= quint64(1) << index;
    }
    if (match.terms == 0) continue;
    const SearchText::FoldedText kept = SearchText::FoldedText::make(text);
    const QString name = SearchText::fold(job.fileName);
    for (const QString& term : snippetTerms) {
      if (name.contains(term)) continue;
      match.snippet = kept.snippet(term.toUtf8(), 44, 56);
      if (!match.snippet.isEmpty()) break;
    }
    scan.matches.insert(job.path, {job.serial, match});
  }
  return scan;
}

void CaptureFilterModel::storedScanFinished() {
  m_storedScanning = false;
  const StoredScan scan = m_storedWatcher.result();
  if (scan.generation == m_storedGeneration) {
    bool matched = false;
    for (auto it = scan.matches.cbegin(); it != scan.matches.cend(); ++it) {
      const auto stored = m_storedDocuments.constFind(it.key());
      if (stored == m_storedDocuments.cend() || stored.value() != it->first) continue;
      m_storedMatches.insert(it.key(), it->second);
      matched = true;
    }
    for (const auto& [path, serial] : scan.lost) {
      const auto stored = m_storedDocuments.find(path);
      if (stored == m_storedDocuments.end() || stored.value() != serial) continue;
      m_storedDocuments.erase(stored);
      m_storedPending.remove(path);
      emit documentTextLost(path);
    }
    if (matched && m_query.hasText()) {
      m_ocrFilterTimer.start();
    }
  }
  startStoredScan();
}

bool CaptureFilterModel::storedTextHas(const QString& path, int ordinal) const {
  if (ordinal >= 64 || m_storedMatches.isEmpty()) {
    return false;
  }
  const auto found = m_storedMatches.constFind(path);
  return found != m_storedMatches.cend() && ((found->terms >> ordinal) & 1) != 0;
}

const SearchText::FoldedText* CaptureFilterModel::textFor(const CaptureRecord& record) const {
  // A path that changes between picture and PDF keeps only its own
  // producer's text.
  if (record.isDocument()) {
    const auto found = m_documentText.constFind(record.path);
    return found == m_documentText.cend() ? nullptr : &found->text;
  }
  const auto found = m_text.constFind(record.path);
  return found == m_text.cend() ? nullptr : &found.value();
}

int CaptureFilterModel::rowOf(const QString& path) const {
  if (path.isEmpty() || !sourceModel()) {
    return -1;
  }
  const auto* source = static_cast<const CaptureModel*>(sourceModel());
  const int sourceRow = source->rowOf(path);
  if (sourceRow < 0) {
    return -1;
  }
  const QModelIndex proxyIndex = mapFromSource(source->index(sourceRow, 0));
  return proxyIndex.isValid() ? proxyIndex.row() : -1;
}

QString CaptureFilterModel::adjacentPathInFolder(const QString& path, int direction) const {
  const int current = rowOf(path);
  const int rows = rowCount();
  if (current < 0 || rows < 2 || direction == 0) {
    return {};
  }

  const QString directory = QFileInfo(path).absolutePath();
  const int step = direction < 0 ? -1 : 1;
  for (int offset = 1; offset < rows; ++offset) {
    const int row = (current + step * offset + rows) % rows;
    const QString candidate = pathAt(row);
    if (QFileInfo(candidate).absolutePath() == directory) {
      return candidate;
    }
  }
  return {};
}

QString CaptureFilterModel::adjacentPath(const QString& path, int direction) const {
  const int current = rowOf(path);
  const int rows = rowCount();
  if (current < 0 || rows < 2 || direction == 0) {
    return {};
  }
  const int next = (current + (direction < 0 ? -1 : 1) + rows) % rows;
  return pathAt(next);
}

int CaptureFilterModel::sourceCount() const {
  return sourceModel() ? sourceModel()->rowCount() : 0;
}

// The source is always a CaptureModel; reading its records directly keeps a
// full re-sort of a large library from building a QVariant per comparison.
const CaptureRecord& CaptureFilterModel::sourceRecord(int sourceRow) const {
  return static_cast<const CaptureModel*>(sourceModel())->recordAt(sourceRow);
}

QString CaptureFilterModel::ocrSnippet(const CaptureRecord& record) const {
  if (m_searchTerms.isEmpty()) {
    return {};
  }
  const SearchText::FoldedText* text = textFor(record);
  if (!text) {
    if (!record.isDocument()) return {};
    const auto stored = m_storedMatches.constFind(record.path);
    return stored == m_storedMatches.cend() ? QString() : stored->snippet;
  }
  if (record.isDocument()) {
    // Shown documents stay in memory longest.
    const auto found = m_documentText.constFind(record.path);
    m_documentUse.splice(m_documentUse.end(), m_documentUse, found->use);
  }
  // Read for every visible tile after each keystroke, so the match is found
  // in the folded text already kept and only the piece around it is folded
  // again to show it from the original.
  const QString name = SearchText::fold(record.fileName);
  for (const QString& term : m_searchTerms) {
    if (name.contains(term)) continue;
    const QString snippet = text->snippet(term.toUtf8(), 44, 56);
    if (!snippet.isEmpty()) return snippet;
  }
  return {};
}

bool CaptureFilterModel::recordLessThan(const CaptureRecord& first,
                                        const CaptureRecord& second) const {
  const auto pathOrder = [&] {
    return QString::compare(first.path, second.path, Qt::CaseSensitive) < 0;
  };
  switch (m_sortMode) {
  case OldestFirst:
    return first.captured != second.captured ? first.captured < second.captured : pathOrder();
  case LargestFirst:
    return first.bytes != second.bytes ? first.bytes > second.bytes : pathOrder();
  case SmallestFirst:
    return first.bytes != second.bytes ? first.bytes < second.bytes : pathOrder();
  case NameAscending:
    if (const int names = m_nameCollator.compare(first.fileName, second.fileName);
        names != 0) {
      return names < 0;
    }
    return pathOrder();
  case RatingFirst:
    if (first.rating != second.rating) {
      return first.rating > second.rating;
    }
    return first.captured != second.captured ? first.captured > second.captured : pathOrder();
  case NewestFirst:
  default:
    return first.captured != second.captured ? first.captured > second.captured : pathOrder();
  }
}

void CaptureFilterModel::rebuildDuplicateOrdinals() {
  m_duplicateOrdinals.clear();
  const QHash<QString, QString>& reviewGroups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
  if (!sourceModel() || reviewGroups.isEmpty()) {
    return;
  }

  // Each set is represented by the first member under the selected sort. The
  // sets therefore stay together without making Newest, Oldest or Name lie
  // about their order.
  QHash<QString, int> representatives;
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    const CaptureRecord& record = sourceRecord(row);
    const QString group = reviewGroups.value(record.path);
    if (group.isEmpty()) {
      continue;
    }
    const auto current = representatives.constFind(group);
    if (current == representatives.cend() ||
        recordLessThan(record, sourceRecord(current.value()))) {
      representatives.insert(group, row);
    }
  }

  QStringList groupOrder = representatives.keys();
  std::sort(groupOrder.begin(), groupOrder.end(),
            [this, &representatives](const QString& first, const QString& second) {
              const CaptureRecord& a = sourceRecord(representatives.value(first));
              const CaptureRecord& b = sourceRecord(representatives.value(second));
              if (recordLessThan(a, b)) {
                return true;
              }
              if (recordLessThan(b, a)) {
                return false;
              }
              return first < second;
            });
  for (int index = 0; index < groupOrder.size(); ++index) {
    m_duplicateOrdinals.insert(groupOrder.at(index), index + 1);
  }
}

bool CaptureFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
  if (sourceParent.isValid()) {
    return false;
  }
  if (m_acceptedValid && sourceRow < static_cast<int>(m_accepted.size())) {
    return m_accepted[sourceRow];
  }
  const CaptureRecord& record = sourceRecord(sourceRow);
  return !m_groupedOutPaths.contains(record.path) && matchesFilters(sourceRow, record);
}

bool CaptureFilterModel::matchesFilters(int sourceRow, const CaptureRecord& record,
                                        int relax) const {
  if (!m_showHidden && record.hidden) {
    return false;
  }
  if (m_duplicatesOnly && !m_duplicateGroups.contains(record.path)) {
    return false;
  }
  if (m_similarOnly && !m_similarGroups.contains(record.path)) {
    return false;
  }
  const bool refine = (relax & RelaxRefinements) == 0;
  if (refine && m_favoritesOnly && !record.favorite) {
    return false;
  }
  if (refine && m_minimumRating > 0 && record.rating < m_minimumRating) {
    return false;
  }
  if (!m_albumFilter.isEmpty() && !m_albumPaths.contains(record.path)) {
    return false;
  }
  if (!m_tagFilter.isEmpty() && !m_tagPaths.contains(record.path)) {
    return false;
  }
  if (refine && !m_cameraFilter.isEmpty() && record.camera != m_cameraFilter) {
    return false;
  }
  if (refine && !m_lensFilter.isEmpty() && record.lens != m_lensFilter) {
    return false;
  }
  if (m_modifiedAfterMs > 0 && record.modified <= m_modifiedAfterMs) {
    return false;
  }
  const QDate date = m_dateField == 1 ? QDateTime::fromMSecsSinceEpoch(record.modified).date()
                                      : record.captured.date();
  if (m_dateFrom.isValid() && date < m_dateFrom) {
    return false;
  }
  if (m_dateTo.isValid() && date > m_dateTo) {
    return false;
  }
  if (!m_folderFilter.isEmpty()) {
    const QString directory = QFileInfo(record.path).absolutePath();
    const QString prefix = m_folderFilter.endsWith(QLatin1Char('/'))
                               ? m_folderFilter
                               : m_folderFilter + QLatin1Char('/');
    if (directory != m_folderFilter && !directory.startsWith(prefix)) {
      return false;
    }
  }
  if (refine && m_kindFilter != kAllKinds) {
    if (m_kindFilter == CaptureRecord::Document) {
      if (!record.isDocument()) {
        return false;
      }
    } else if (static_cast<int>(record.kind) != m_kindFilter) {
      return false;
    }
  }
  return (relax & RelaxSearch) != 0 || m_query.isEmpty() || matchesSearch(sourceRow, record);
}

bool CaptureFilterModel::anyMatches(int relax) const {
  if (!sourceModel()) return false;
  for (int row = 0; row < sourceModel()->rowCount(); ++row) {
    if (matchesFilters(row, sourceRecord(row), relax)) return true;
  }
  return false;
}

QString CaptureFilterModel::emptyReason() const {
  if (rowCount() > 0) return {};
  if (sourceCount() == 0) return QStringLiteral("library");
  if (m_duplicatesOnly) return QStringLiteral("duplicates");
  if (m_similarOnly) return QStringLiteral("similar");
  // Read only while the grid is empty, so walking the library is rare.
  if (!m_query.isEmpty() && anyMatches(RelaxSearch)) return QStringLiteral("search");
  const bool refined = m_kindFilter != kAllKinds || m_favoritesOnly || m_minimumRating > 0 ||
                       !m_cameraFilter.isEmpty() || !m_lensFilter.isEmpty();
  if (refined && anyMatches(RelaxSearch | RelaxRefinements)) return QStringLiteral("filters");
  return QStringLiteral("scope");
}

void CaptureFilterModel::clearRefinements() {
  setSearchText(QString());
  setKindFilter(kAllKinds);
  setFavoritesOnly(false);
  setMinimumRating(0);
  setCameraFilter(QString());
  setLensFilter(QString());
}

void CaptureFilterModel::clearFacets(bool includeSearch) {
  commitPendingSearch();
  const bool review = m_duplicatesOnly || m_similarOnly;
  const bool dates = m_dateFrom.isValid() || m_dateTo.isValid() || !m_modifiedAfter.isEmpty() ||
                     m_dateField != 0;
  const bool search = includeSearch && !m_searchText.isEmpty();
  const bool any = review || dates || search || !m_folderFilter.isEmpty() ||
                   !m_albumFilter.isEmpty() || !m_tagFilter.isEmpty() ||
                   !m_cameraFilter.isEmpty() || !m_lensFilter.isEmpty() || m_minimumRating > 0;
  if (!any) {
    clearSmartCollection();
    return;
  }
  const bool duplicates = m_duplicatesOnly;
  const bool similar = m_similarOnly;
  const bool folder = !m_folderFilter.isEmpty();
  const bool album = !m_albumFilter.isEmpty();
  const bool tag = !m_tagFilter.isEmpty();
  const bool camera = !m_cameraFilter.isEmpty();
  const bool lens = !m_lensFilter.isEmpty();
  const bool rating = m_minimumRating > 0;
  // One filter pass for all of them, rather than one per facet.
  beginFilterUpdate();
  m_duplicatesOnly = false;
  m_similarOnly = false;
  m_folderFilter.clear();
  m_albumFilter.clear();
  m_albumPaths.clear();
  m_tagFilter.clear();
  m_tagPaths.clear();
  m_cameraFilter.clear();
  m_lensFilter.clear();
  m_minimumRating = 0;
  m_dateFrom = {};
  m_dateTo = {};
  m_dateField = 0;
  m_modifiedAfter.clear();
  m_modifiedAfterMs = 0;
  if (search) applySearchText({});
  endFilterUpdate();
  if (review) {
    // Review sets sort by set; the plain grid goes back to the sort order.
    rebuildDuplicateOrdinals();
    invalidate();
  }
  clearSmartCollection();
  if (search) {
    if (rowCount() > 0) {
      emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {CaptureRoles::OcrSnippetRole});
    }
    emit searchTextChanged();
  }
  if (duplicates) emit duplicatesOnlyChanged();
  if (similar) emit similarOnlyChanged();
  if (folder) emit folderFilterChanged();
  if (album) emit albumFilterChanged();
  if (tag) emit tagFilterChanged();
  if (camera) emit cameraFilterChanged();
  if (lens) emit lensFilterChanged();
  if (rating) emit minimumRatingChanged();
  if (dates) emit dateRangeChanged();
  emit countChanged();
}

bool CaptureFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const CaptureRecord& a = sourceRecord(left.row());
  const CaptureRecord& b = sourceRecord(right.row());
  if (m_duplicatesOnly || m_similarOnly) {
    const QHash<QString, QString>& groups = m_similarOnly ? m_similarGroups : m_duplicateGroups;
    const QString firstGroup = groups.value(a.path);
    const QString secondGroup = groups.value(b.path);
    if (firstGroup != secondGroup) {
      return m_duplicateOrdinals.value(firstGroup) < m_duplicateOrdinals.value(secondGroup);
    }
  }
  return recordLessThan(a, b);
}

void CaptureFilterModel::applySearchText(const QString& text) {
  m_searchText = text;
  m_query = SearchQuery::parse(text);
  m_searchTerms = m_query.textValues();
  resetStoredMatches();
  // Folded keys cost memory in a large library, so they go a while after the
  // search is cleared; searching again soon after reuses them.
  if (m_query.isEmpty()) m_releaseKeysTimer.start();
  else m_releaseKeysTimer.stop();
}

QString CaptureFilterModel::textSearch() const {
  return m_query.textValues(true).join(QLatin1Char(' '));
}

void CaptureFilterModel::reloadOrganization() {
  QHash<QString, QString> tags;
  QHash<QString, QString> albums;
  const auto fold = [](const QHash<QString, QStringList>& byPath, QHash<QString, QString>* out) {
    out->reserve(byPath.size());
    for (auto it = byPath.cbegin(); it != byPath.cend(); ++it) {
      QStringList names;
      names.reserve(it.value().size());
      for (const QString& name : it.value()) names.append(SearchText::fold(name));
      out->insert(it.key(), names.join(QLatin1Char('\n')));
    }
  };
  if (m_settings) {
    fold(m_settings->tagsByPath(), &tags);
    fold(m_settings->albumsByPath(), &albums);
  }
  if (tags == m_tagText && albums == m_albumText) return;
  m_tagText = std::move(tags);
  m_albumText = std::move(albums);
  ++m_organizationVersion;
  if (m_query.isEmpty() || !sourceModel()) return;
  beginFilterUpdate();
  endFilterUpdate();
  emit countChanged();
}

// in: matches the folder as written from home, so "home" or a user name does
// not match the whole library. Plain words match only the folders below the
// library root the file was found in; a file outside every root matches its
// own folder's name.
const CaptureFilterModel::FolderKeys& CaptureFilterModel::folderKeys(const QString& path) const {
  const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
  const QString directory = slash > 0 ? path.left(slash) : QString();
  const auto found = m_folderKeys.constFind(directory);
  if (found != m_folderKeys.cend()) return found.value();
  static const QString home = QDir::homePath();
  FolderKeys keys;
  QString written = directory;
  if (directory == home) {
    written = QStringLiteral("~");
  } else if (directory.startsWith(home) && directory.size() > home.size() &&
             directory.at(home.size()) == QLatin1Char('/')) {
    written = QStringLiteral("~") + directory.mid(home.size());
  }
  keys.path = SearchText::fold(written);
  bool underRoot = false;
  for (const QString& root : m_libraryRoots) {
    // Roots are canonical, so only "/" ends in a slash.
    const qsizetype prefix = root.endsWith(QLatin1Char('/')) ? root.size() : root.size() + 1;
    if (directory == root) {
      underRoot = true;
    } else if (directory.size() > prefix && directory.startsWith(root) &&
               directory.at(prefix - 1) == QLatin1Char('/')) {
      keys.words = SearchText::fold(directory.mid(prefix));
      underRoot = true;
    }
    if (underRoot) break;
  }
  if (!underRoot) {
    keys.words = SearchText::fold(directory.mid(directory.lastIndexOf(QLatin1Char('/')) + 1));
  }
  // Bounded: a library has far fewer folders than files, and a rescan that
  // churns through many renamed folders starts the cache over.
  if (m_folderKeys.size() > 65536) m_folderKeys.clear();
  return m_folderKeys.insert(directory, keys).value();
}

void CaptureFilterModel::reloadLibraryRoots() {
  QStringList roots;
  if (const auto* captures = qobject_cast<const CaptureModel*>(sourceModel())) {
    for (const QVariant& row : captures->folderSources()) {
      const QString root = row.toMap().value(QStringLiteral("path")).toString();
      if (!root.isEmpty()) roots.append(root);
    }
  }
  // The deepest root first, so a library folder inside Pictures counts from
  // itself.
  std::sort(roots.begin(), roots.end(),
            [](const QString& first, const QString& second) { return first.size() > second.size(); });
  if (roots == m_libraryRoots) return;
  m_libraryRoots = std::move(roots);
  m_folderKeys.clear();
  std::vector<SearchKeys>().swap(m_searchKeys);
  if (m_query.isEmpty() || !sourceModel()) return;
  beginFilterUpdate();
  endFilterUpdate();
  emit countChanged();
}

const CaptureFilterModel::SearchKeys& CaptureFilterModel::searchKeys(
    int sourceRow, const CaptureRecord& record) const {
  if (sourceRow >= static_cast<int>(m_searchKeys.size())) {
    m_searchKeys.resize(std::max<size_t>(sourceRow + 1, sourceModel()->rowCount()));
  }
  SearchKeys& keys = m_searchKeys[sourceRow];
  const auto same = [](const QString& first, const QString& second) {
    return first.constData() == second.constData() && first.size() == second.size();
  };
  if (!same(keys.path, record.path) || !same(keys.fileName, record.fileName) ||
      !same(keys.camera, record.camera) || !same(keys.lens, record.lens) ||
      !same(keys.caption, record.caption) || keys.document != record.isDocument()) {
    keys.path = record.path;
    keys.document = record.isDocument();
    keys.fileName = record.fileName;
    keys.camera = record.camera;
    keys.lens = record.lens;
    keys.caption = record.caption;
    keys.name = SearchText::fold(record.fileName);
    const FolderKeys& folder = folderKeys(record.path);
    keys.folder = folder.path;
    keys.folderWords = folder.words;
    keys.cameraKey = SearchText::fold(record.camera);
    keys.lensKey = SearchText::fold(record.lens);
    keys.captionKey = SearchText::fold(record.caption);
    keys.organization = 0;
    // Rows move when a scan inserts or removes files, so text looked up for
    // the file that was here before is not this one's.
    keys.text = 0;
  }
  if (keys.organization != m_organizationVersion) {
    keys.tags = m_tagText.value(record.path);
    keys.albums = m_albumText.value(record.path);
    keys.organization = m_organizationVersion;
  }
  // Text arrives a picture at a time; rows look it up again only after some
  // arrived, not on every keystroke.
  if (keys.text != m_textVersion) {
    const SearchText::FoldedText* text = textFor(record);
    keys.ocr = text ? text->folded : QByteArray();
    keys.text = m_textVersion;
  }
  return keys;
}

bool CaptureFilterModel::matchesSearch(int sourceRow, const CaptureRecord& record) const {
  const SearchKeys& keys = searchKeys(sourceRow, record);
  int textOrdinal = 0;
  for (const SearchQuery::Term& term : m_query.terms) {
    bool hit = false;
    switch (term.field) {
    case SearchQuery::Text:
      // Folders count from below the library's roots and at a word start,
      // so "pictures" or "eos" does not match a whole root.
      hit = keys.name.contains(term.value) ||
            SearchText::containsWordStart(keys.folderWords, term.value) ||
            keys.captionKey.contains(term.value) || keys.tags.contains(term.value) ||
            keys.albums.contains(term.value) || keys.cameraKey.contains(term.value) ||
            keys.lensKey.contains(term.value) || keys.ocr.contains(term.utf8) ||
            (keys.document && storedTextHas(record.path, textOrdinal));
      ++textOrdinal;
      break;
    case SearchQuery::Tag:
      hit = SearchText::containsWordStart(keys.tags, term.value);
      break;
    case SearchQuery::Album:
      hit = SearchText::containsWordStart(keys.albums, term.value);
      break;
    case SearchQuery::Folder:
      hit = SearchText::containsWordStart(keys.folder, term.value);
      break;
    case SearchQuery::Camera:
      hit = SearchText::containsWordStart(keys.cameraKey, term.value);
      break;
    case SearchQuery::Lens:
      hit = SearchText::containsWordStart(keys.lensKey, term.value);
      break;
    case SearchQuery::Type:
      switch (term.type) {
      case SearchQuery::Screenshot: hit = record.kind == CaptureRecord::Screenshot; break;
      case SearchQuery::Recording: hit = record.kind == CaptureRecord::Recording; break;
      // Photos and other stills; screenshots have their own type.
      case SearchQuery::Image:
        hit = !record.isVideo() && !record.isDocument() &&
              record.kind != CaptureRecord::Screenshot;
        break;
      case SearchQuery::Video: hit = record.isVideo(); break;
      case SearchQuery::Download: hit = record.kind == CaptureRecord::Download; break;
      case SearchQuery::Pdf: hit = record.isDocument(); break;
      case SearchQuery::Raw: hit = record.isRaw(); break;
      case SearchQuery::Animated: hit = record.animated; break;
      }
      break;
    case SearchQuery::Before:
    case SearchQuery::After: {
      const QDate date = record.captured.date();
      hit = date.isValid() &&
            (term.field == SearchQuery::Before ? date < term.date : date > term.date);
      break;
    }
    }
    if (hit == term.negated) return false;
  }
  return true;
}

namespace {

QString homeRelative(const QString& path) {
  static const QString home = QDir::homePath();
  if (path == home) return QStringLiteral("~");
  if (path.startsWith(home) && path.size() > home.size() && path.at(home.size()) == QLatin1Char('/')) {
    return QStringLiteral("~") + path.mid(home.size());
  }
  return path;
}

} // namespace

QVariantList CaptureFilterModel::searchSuggestions(const QString& text, int cursor) const {
  constexpr int kMaximum = 8;
  cursor = qBound(0, cursor, static_cast<int>(text.size()));
  // The token under the cursor: back to the last space outside quotes, and
  // forward to the next one, so accepting mid-word replaces the whole word.
  // Inside quotes, \" and \\ are escapes and do not end the value.
  bool quoted = false;
  int start = 0;
  for (int index = 0; index < cursor; ++index) {
    if (quoted && text.at(index) == QLatin1Char('\\') && index + 1 < cursor) ++index;
    else if (text.at(index) == QLatin1Char('"')) quoted = !quoted;
    else if (text.at(index).isSpace() && !quoted) start = index + 1;
  }
  int end = cursor;
  while (end < text.size() && !(text.at(end).isSpace() && !quoted)) {
    if (quoted && text.at(end) == QLatin1Char('\\') && end + 1 < text.size()) ++end;
    else if (text.at(end) == QLatin1Char('"')) quoted = !quoted;
    ++end;
  }
  QString token = text.mid(start, cursor - start);
  QString prefix;
  if (token.startsWith(QLatin1Char('-'))) {
    prefix = QStringLiteral("-");
    token.remove(0, 1);
  }
  QVariantList rows;
  const qsizetype colon = token.indexOf(QLatin1Char(':'));
  if (colon < 0) {
    // A word that starts a field name offers the field, so the syntax can be
    // found without the help sheet.
    const QString lowered = token.toLower();
    if (lowered.size() < 2) return rows;
    for (const QString& field : SearchQuery::fieldNames()) {
      if (field.startsWith(lowered) && field != lowered) {
        rows.append(QVariantMap{{QStringLiteral("label"), field + QLatin1Char(':')},
                                {QStringLiteral("detail"), QStringLiteral("Search field")},
                                {QStringLiteral("insert"), prefix + field + QLatin1Char(':')},
                                {QStringLiteral("start"), start},
                                {QStringLiteral("end"), end},
                                {QStringLiteral("complete"), false}});
      }
    }
    return rows;
  }

  const QString fieldName = token.left(colon);
  const int field = SearchQuery::fieldFor(fieldName);
  if (field < 0) return rows;
  QString partial = token.mid(colon + 1);
  // An opening quote may still wait for its closing one, which accepting a
  // value adds.
  const bool opened = partial.startsWith(QLatin1Char('"'));
  bool closed = false;
  if (opened) partial = SearchQuery::readQuoted(partial, 1, nullptr, &closed);
  const QString needle = SearchText::fold(partial.simplified());

  struct Candidate {
    QString value;
    QString label;
    QString detail;
  };
  QList<Candidate> candidates;
  const auto counted = [](int count) {
    return count == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(count);
  };
  switch (field) {
  case SearchQuery::Tag:
    if (m_settings) {
      for (const QString& name : m_settings->tagNames()) {
        candidates.append({name, name, counted(m_settings->tagItemCount(name))});
      }
    }
    break;
  case SearchQuery::Album:
    if (m_settings) {
      for (const QString& name : m_settings->albumNames()) {
        candidates.append({name, name, counted(m_settings->albumItemCount(name))});
      }
    }
    break;
  case SearchQuery::Folder:
    for (const QString& folder : m_folders) {
      const QString relative = homeRelative(folder);
      candidates.append({relative, relative, counted(m_folderItemCounts.value(folder))});
    }
    break;
  case SearchQuery::Camera:
  case SearchQuery::Lens:
    for (const QVariant& row : field == SearchQuery::Camera ? m_cameras : m_lenses) {
      const QVariantMap choice = row.toMap();
      const QString name = choice.value(QStringLiteral("name")).toString();
      candidates.append({name, name, counted(choice.value(QStringLiteral("count")).toInt())});
    }
    break;
  case SearchQuery::Type:
    for (const QString& name : SearchQuery::typeNames()) {
      candidates.append({name, name, QStringLiteral("Kind of file")});
    }
    break;
  case SearchQuery::Before:
  case SearchQuery::After: {
    // Years the library has, newest first; once a year is typed, its months.
    QStringList seen;
    const bool months = partial.size() >= 5 && partial.at(4) == QLatin1Char('-');
    for (const QVariant& row : m_dateBuckets) {
      const QVariantMap bucket = row.toMap();
      const QString key = bucket.value(QStringLiteral("key")).toString();
      const QString value = months ? key : key.left(4);
      if (seen.contains(value)) continue;
      seen.append(value);
      candidates.append({value, months ? bucket.value(QStringLiteral("label")).toString() : value,
                         field == SearchQuery::Before ? QStringLiteral("Earlier than this")
                                                      : QStringLiteral("Later than this")});
    }
    break;
  }
  default:
    break;
  }

  // Values starting with what was typed come first, then word starts inside.
  QList<Candidate> leading;
  QList<Candidate> inner;
  for (const Candidate& candidate : std::as_const(candidates)) {
    const QString folded = SearchText::fold(candidate.value);
    // Nothing to offer for a value that is already typed in full, unless
    // its quote is still open.
    if (folded == needle && (!opened || closed)) continue;
    if (folded.startsWith(needle)) leading.append(candidate);
    else if (SearchText::containsWordStart(folded, needle)) inner.append(candidate);
  }
  leading.append(inner);
  for (const Candidate& candidate : std::as_const(leading)) {
    if (rows.size() >= kMaximum) break;
    rows.append(QVariantMap{
        {QStringLiteral("label"), candidate.label},
        {QStringLiteral("detail"), candidate.detail},
        {QStringLiteral("insert"),
         prefix + fieldName + QLatin1Char(':') + SearchQuery::quote(candidate.value)},
        {QStringLiteral("start"), start},
        {QStringLiteral("end"), end},
        {QStringLiteral("complete"), true}});
  }
  return rows;
}
